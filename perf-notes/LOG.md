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
