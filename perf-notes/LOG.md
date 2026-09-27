# Work log

## Environment (2026-09-27)
- 4-core Intel Xeon VM @ 2.8GHz, 15 GB RAM, gcc 13.3, clang/lld 18, valgrind 3.22.
- No hardware perf counters (no `cpu` PMU in the VM), no `perf`. Wall-clock is noisy.
- No GCC 16 or LLVM 19+, so no tail-call interpreter build locally.
- Upstream main 6af40a6 (3.16.0a0). Frame pointers are on by default (PEP 831).

## Measurement plan
- **Proxy A (deterministic, minutes):** `perf-notes/tools/irbench.py`. Runs each pure-Python
  pyperformance benchmark body under cachegrind at K and 2K loops and takes the difference in
  instruction counts (Ir). Optional `--sim` adds cache and branch simulation and gives a rough
  cycle estimate.
- **Proxy B:** pystats (`--enable-pystats`) for specialization hit/miss/deopt diagnoses.
- **Confirmation:** pyperformance wall time on PGO+LTO builds, several processes. Code-layout
  sampling via lld `--shuffle-sections=<seed>` over several seeds (a cheap static cousin of
  Stabilizer), and Stabilizer itself if it can be built here (it needs a newer LLVM than 18?).

## Early diagnoses from cachegrind (non-PGO -O3 build)
- **richards:** 51% of Ir is in `_PyEval_EvalFrameDefault`. The generic attribute path
  (`_PyObject_GenericGetAttrWithDict` 3.7%, `_PyObject_GetMethodStackRef` 2.8%,
  `unicodekeys_lookup_unicode` 4.9%, `_PyObject_TryGetInstanceAttribute` 2.9%) is about 14%
  in total. Nearly all sites specialize statically, so this is likely misses at polymorphic
  sites (`self.fn(...)` over 4 Task subclasses): monomorphic caches plus deopt.
- **raytrace:** `initialize_locals` 2.5%, `_PyEval_FrameClearAndPop` 2.4%,
  `_PyFrame_ClearExceptCode` 5.6% (decref and dealloc of temporaries), `subtype_dealloc` 1.3%.
  Calls with defaults or kwargs don't take the `CALL_PY_EXACT_ARGS` path?

## pystats over the suite (66 benchmarks, non-PGO pystats build)
Tip: `Tools/scripts/summarize_stats.py --json-output FILE` gives the raw counters as JSON (use that
instead of scraping the HTML tables). Two inputs give a comparison, e.g. base vs. patch.
- 31% of Python-level calls enter the interpreter from C (`PyEval_EvalDefault`, 46M) vs 69% inlined.
  By kind: function vectorcall 20%, generator resumption from C 10.9%, dunder slot 5.9%, bound method 5.1%.
  Per 10k bytecodes: async_generators 568, mdp 395 (mostly generators consumed by C builtins), hexiom 176,
  raytrace 69 (all slot dunders), async_tree* 110–240 (asyncio callbacks).
- Tier-1 miss ratios in *our* aggregate are low (LOAD_ATTR_INSTANCE_VALUE 1.0%, METHOD_WITH_VALUES 1.6%,
  BINARY_OP_ADD_FLOAT 4.8%, FOR_ITER_LIST 3.2%), lower than the public July 2025 aggregate.

## Microbenchmarks: machine instructions per iteration (non-PGO build, cachegrind)
| pattern | Ir/iter |
|---|---|
| `for i in range(N): pass` | 168 (!) of which ~73 is int alloc/free |
| `sum(gen)` (generator resumed from C) | 425 |
| `any(genexpr)` | 420 |
| `v.add(v)` plain method call | 499 |
| `v + v` via Python `__add__` (slot → C → eval) | 1019 |
| `sorted(key=lambda)` / `list(map(lambda))` | 835 |

So a Python dunder costs about 2x a plain method call. Inlining Python `__add__` etc. in BINARY_OP (like
BINARY_OP_SUBSCR_GETITEM does for `__getitem__`) would save ~500 Ir per op.

## Micro-level: allocation/deallocation overhead (non-PGO, non-LTO; must recheck on PGO+LTO)
- `_Py_Dealloc` non-GC path: 23 instructions, mostly saving and restoring 5 callee-saved registers that only
  the GC (trashcan/recursion-margin) path needs; no shrink-wrapping. It is 2.9% of raytrace Ir.
- `PyFloat_FromDouble` / `_PyLong_FromMedium` freelist path calls out-of-line `_Py_NewReference`
  (refcnt=1 plus a ref-tracer check). LTO may inline it.

## Candidate: _PyEval_Vector exact-positional-args fast path (branch perf/eval-vector-fast)
C→Python calls (20% of all Python calls in the suite: sorted/map keys, callbacks, slot dunders, asyncio)
built a zeroed 8-slot temp array, increfed args into it, then ran the fully general initialize_locals().
New fast path when kwnames==NULL, argcount==co_argcount, no kwonly and no */** params: push the frame and
write the args straight into localsplus (as CALL_PY_EXACT_ARGS does).
Micro (non-PGO Ir/iter): sorted(key=lambda) 835→707, list(map(lambda)) 834→706, `v+v` via __add__ 1019→886.
Tests: test_call test_extcall test_funcattrs test_sys_settrace test_monitoring test_generators test_descr
test_class test_inspect test_capi.test_misc all pass.

## Candidate: _Py_Dealloc non-GC fast path (branch perf/dealloc-split)
Non-GC objects with no ref tracer now tail-jump to tp_dealloc (7 instructions instead of 23). The GC /
trashcan / tracer logic moved to a noinline helper. Micro: `for i in range(N): pass` 168→153 Ir/iter.
Tests: test_gc test_capi.test_object test_weakref test_finalization test_descr pass.

## Suspect regression: per-type method cache (gh-145685, PR #150160, merged 2026-07-21)
It replaced the global type-attribute cache with a per-type open-addressing table in Python/typecache.c.
The PR itself measured the GIL build about 1% slower, which was waved off as noise (M. Shannon objected).
In non-LTO builds `_PyTypeCache_Lookup` is an out-of-line call returning a 24-byte struct;
the dunder micro spends ~100 Ir per slot lookup (`_PyTypeCache_Lookup` 43 + `_PyType_LookupStackRefAndVersion`
33 + `lookup_method_ex` 26). Next: measure on PGO+LTO, and try an inline fast path for the GIL build.

## Methodology update (after user guidance): randomized block designs, relative numbers only
- `perf-notes/tools/blockbench.py`: a block is one pass over all (benchmark × build) cells in a fresh
  random order on one machine. The response is log(time), and comparisons are paired within blocks.
  Arms can contain several builds (layout seeds), which are treated as a random factor.
  Per benchmark: mean paired log-ratio, block-bootstrap CI, sign-flip permutation p. The geomean CI comes
  from resampling whole blocks.
- An A/A arm (the same commit built separately) is always included as a negative control.
- A local A/A smoke test on this overloaded 4-core VM (load ~12) showed ±15% swings between identical
  binaries at n=2. That is why absolute numbers are never compared across blocks.
- **GitHub CI as a block farm** (`perf-notes/ci/`): `make_branch.sh` pushes a throwaway `perf-ci/<name>` branch.
  Its tip commit has all arm commits as parents (so they are fetchable by SHA), and its tree has every
  built-in workflow removed and only `perf-block.yml` added. Each matrix job builds all arms (PGO+LTO)
  on its own runner and runs complete blocks, so runner-to-runner variation is a block effect.
  Results come back base64-gzipped in the job log and as an artifact.
- exp1 (run 36291034777): arms base / aa (A/A) / dealloc / vecfast, 6 jobs × 4 blocks, all 66 benchmarks.

## PGO+LTO check of the diagnoses (GCC 13, --enable-optimizations --with-lto)
- `_Py_Dealloc`: still pushes rbp/r12/rbx and adjusts rsp before testing the GC flag (no shrink-wrapping).
- `_PyTypeCache_Lookup`: still out of line (71 instructions). `initialize_locals` is still a separate
  function behind `_PyEval_Vector`.
- Dispatch-site merging (gh-129987, where GCC kept 47/306 sites): **not reproduced** here. There are 234
  indirect `jmp`s in `_PyEval_EvalFrameDefault` for 232 targets. It probably depends on the GCC version
  or configuration.

## exp2 on CI (perf-ci/exp2-disputes): four disputes in one randomized-block experiment
Arms: base=6af40a6 (anchor), same (same binary: run-to-run control), m2_inline (gh-132336 noinline
reverted), nofp (--without-frame-pointers, PEP 831), gc2x / gc4x (gen-0 threshold ×2 / ×4 via sitecustomize,
Pitrou's untested question on PEP 848). 8 jobs × 3 blocks.
