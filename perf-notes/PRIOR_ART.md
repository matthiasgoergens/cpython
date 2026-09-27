# Prior-art report: CPython interpreter performance, as of 2026-09-27

**Version check:** the local checkout `/home/user/cpython` is 3.16 alpha (`Include/patchlevel.h`: 3.16, `PY_RELEASE_LEVEL_ALPHA`; HEAD 6af40a6, 2026-09-27). 3.15 is at rc2.

## 0. Context that decides what is worth trying now

- **The Faster CPython team is gone.** Microsoft cancelled it in May 2025, and Shannon, Snow and Katriel were laid off. Work continued as "community stewardship", with a fortnightly performance working group coordinated by Shannon. Links: https://discuss.python.org/t/community-stewardship-of-faster-cpython/92153 and https://www.theregister.com/2025/05/16/microsofts_axe_software_developers/
- **The JIT is frozen.** On about 2026-06-05 the Steering Council halted all new JIT features, optimizations and performance work on main (bug and security fixes are still allowed) until a standards-track PEP is accepted, with roughly a 6-month window. Link: https://discuss.python.org/t/an-announcement-from-the-steering-council-regarding-the-jit-project/107638
  - The response is PEP 836, "JIT Go Brrr" (Draft, July 2026). It reports the 3.15 JIT at +4.7% to +12.6% geomean over the interpreter, and plans to move from tracing to a method JIT, whose proof of concept is +4–5%. Link: https://peps.python.org/pep-0836/
  - **Implication:** tier-1 interpreter work, compiler work and build/GC work are where ≥1% wins can land in 3.16. The ban does not clearly cover tier 1, but its scope is not precisely defined.
- **Frame pointers are on by default in 3.15 (PEP 831).** Measured cost is 1.5–1.9% on x86-64 and up to 2.3% on Graviton; `--without-frame-pointers` recovers it (https://peps.python.org/pep-0831/). Any A/B comparison against 3.14 must control for this.
- **The incremental GC was reverted** in 3.14.5 and 3.15 (gh-142516, gh-148726). Reintroducing it now requires a PEP. Link: https://discuss.python.org/t/reverting-the-incremental-gc-in-python-3-14-and-3-15/107014

---

## 1. Top candidates to revive or try now (prioritized)

**1. Tail-calling interpreter with GCC 16 (x86-64 `preserve_none`)**
- **What:** GCC 16 added `preserve_none` on x86-64, the missing piece for GCC. The GCC-15 issue https://github.com/python/cpython/issues/132138 said "no perf win at all" without it.
- **Local check:** `Python/ceval_macros.h:83-96` only checks `__has_attribute(preserve_none)` and `musttail`, so GCC 16 should already compile with `--with-tail-call-interp`.
- **Numbers so far:**
  - With clang against a good baseline: about 2% on x86-64 Linux and 5–7% on macOS arm64 (https://lwn.net/Articles/1033373/, https://blog.nelhage.com/post/cpython-tail-call/).
  - With MSVC (VS2026) it was 15–20%, and it is now the default in the Windows 3.15 binaries (https://fidget-spinner.github.io/posts/no-longer-sorry.html, PR https://github.com/python/cpython/pull/143068).
- **Why stalled:** reports say GCC 16 regresses tail-call performance under PGO, and GCC 15 had a PGO+musttail compile error (GCC bug 119618). Nobody has published a GCC 16 pyperformance run. Arch Linux has a packaging MR, https://gitlab.archlinux.org/archlinux/packaging/packages/python/-/merge_requests/9 (I could not open it: access denied).
- **Why it matters:** most Linux distros build with GCC, so this is the widest-reach ≥1% candidate.
- **To do:** benchmark GCC 16 with PGO+LTO, computed-goto vs tail-call, and file GCC bugs if needed. AArch64 `preserve_none` in GCC (bug 118328) is unverified.

**2. Top-of-stack caching in the tier-1 interpreter (https://github.com/python/cpython/issues/131498)**
- **What:** Shannon's plan to keep the top of stack (TOS) in a local variable, cache size 1. It was the first item in the 3.14 plan (`faster-cpython/ideas/3.14/README.md`).
- **Status:** 7 linked preparatory PRs, all open. The JIT got top-of-stack caching (https://github.com/python/cpython/issues/135379; `tos_cache` has 3862 hits in `executor_cases.c.h` and 0 in `generated_cases.c.h`), but tier 1 never did.
- **Numbers:** none measured for tier 1. In the JIT, the register-allocation work was reported at about +0.5% geomean and +16% on nbody. "Modest" is the author's own expectation.
- **Why stalled:** layoffs, and priority went to the JIT.
- **Still applies?** Yes, and it is not JIT-frozen. It interacts with the tail-call calling convention (one more register argument).

**3. Mark Shannon's "Performance TODOs" (https://github.com/python/cpython/issues/144388, Feb 2026, 0 of ~10 done)**
Tier-1 and compiler items:
- extended basic blocks in the bytecode compiler;
- better conversion of `LOAD_FAST` to `LOAD_FAST_BORROW`;
- tracking NULL, immortal and borrowed locals to cheapen `RETURN_VALUE`;
- cheaper insertion-order updates in `STORE_ATTR_INSTANCE_VALUE`;
- function and code watchers;
- `_LOAD_SPECIAL`.

For scale, `LOAD_FAST_BORROW` itself (https://github.com/python/cpython/issues/130704, 3.14) removed about 90% of `LOAD_FAST` refcounting for **2–3%**. Widening its coverage via extended basic blocks is a credible 0.5–1%+ (my estimate, unmeasured). These are the most "ready-to-pick-up" items with an authoritative author.

**4. Range / virtual-iterator specialization (https://github.com/python/cpython/pull/135063, Shannon, open, stale since 2025-10)**
- **What:** `GET_ITER`/`FOR_ITER` for `range` using tagged ints, with no iterator object.
- **Numbers:** microbenchmarks 1.08–1.23x. Full suite: Linux ±0, Windows +0.3%, macOS +8% (the Mac numbers were flagged as inconsistent).
- **Status:** the parent virtual-iterator work (https://github.com/python/cpython/issues/132554) is merged; `FOR_ITER_VIRTUAL` and `GET_ITER_VIRTUAL` are present locally. The PR is waiting on core review and needs a rebase and re-measurement.

**5. Refcount-aware in-place arithmetic in tier 1**
- **Brandt Bucher, "mutate LHS during float ops"** (https://github.com/python/cpython/pull/30594, 2022): **1.01x geomean** (scimark_sparse 1.07x, spectral_norm 1.04x). Closed after review pushback about macro style, not on performance.
- **Shannon, refcount/table-driven `BINARY_OP` specialization** (https://github.com/faster-cpython/ideas/issues/662, PR https://github.com/python/cpython/pull/117627): abandoned at about 1% slower, with spectral_norm +7% and nbody −12%.
- **Current main:** `_BINARY_OP_{ADD,SUBTRACT,MULTIPLY}_{INT,FLOAT}_INPLACE` exist only as `tier2 op` (`Python/bytecodes.c:720-855`).
- **Idea:** a cheap tier-1 variant (a runtime refcount==1 check, or specialization on it). It plausibly still gives about 1% on numeric benchmarks.
- **Related, closed:** https://github.com/python/cpython/pull/150425 (widen int fast paths to int64, closed 2026-07-10). It claimed "pyperformance 1.25x", which is **not credible as a full-suite geomean and is unverified**. Its microbenchmarks were 1.37x. A follow-up PR, https://github.com/python/cpython/pull/151290, is open.

**6. GC tuning within the generational collector (now back in main; young threshold 2000/10/10, `Include/internal/pycore_interp_structs.h:280`)**
- **Adaptive gen-0 threshold** `sqrt(long_lived_pending)+C` (https://github.com/faster-cpython/ideas/issues/523, Dec 2022): **1.02x geomean**, async_tree_io 1.84x, async_tree_memoization 1.62x. No follow-up; the author's branch was never turned into a PR.
- **Skip the resurrection/reachability pass when no finalizers are present** (https://github.com/python/cpython/pull/132488): 20 benchmarks faster and 7 slower (coverage 1.09x, spectral_norm 1.07x, unpack_sequence 1.07x slower). Approved by Jelle Zijlstra, stale since 2025-04.
- **Shannon's "reduce overhead of cycle GC"** (https://github.com/python/cpython/pull/124717) was closed because it was slower.
- **Caveats:** pyperformance has few realistic cyclic-garbage workloads, and the incremental-GC saga makes reviewers wary. Any change here needs memory numbers too.

**7. PGO training workload (https://github.com/python/cpython/pull/130702, Neil Schemenauer, closed 2026-07-03 as stale)**
- **What:** replace the unit-test `PROFILE_TASK` with a curated set of dependency-free benchmark-like tasks.
- **Numbers:** an earlier (2023) experiment gave about 4% on pyperformance.
- **Why stalled:** Brandt Bucher's objection that it is circular or "gaming" the benchmarks.
- **Revival angle:** use a training set that is disjoint from pyperformance (for example the Pyston macrobenchmarks, or stdlib-heavy scripts) and validate on pyperformance. Old issues: https://github.com/python/cpython/issues/80225 and https://github.com/python/cpython/issues/69103.

**8. Shrinking inline caches (https://github.com/faster-cpython/ideas/issues/533, open since 2023)**
- **What:** 16-bit versions and moving cached objects out of line. `LOAD_ATTR` would go from 18 to 7–8 bytes and `LOAD_GLOBAL` from 10 to 6.
- **Status:** not done. `LOAD_ATTR` is still 9 code units (`_PyLoadMethodCache` in `Include/internal/pycore_code.h:108-120`).
- **Numbers:** none measured. The benefit is i-cache/d-cache density on large benchmarks, so the gain is uncertain.

**9. Python-to-Python call path and frame slimming (https://github.com/faster-cpython/ideas/issues/661, Shannon 2024; https://github.com/faster-cpython/ideas/issues/657, Ken Jin's interleaved frame layout)**
- **Ideas:** unrolled or fixed-length `_INIT_CALL_PY_EXACT_ARGS`, drop `f_globals`/`f_builtins` from the frame, cheaper stack-space checks.
- **Status:** `_PyInterpreterFrame` still carries `f_globals`, `f_builtins`, `f_locals`, `frame_obj` and more (`Include/internal/pycore_interpframe_structs.h`).
- **Numbers:** none. Removing globals and builtins slows tier-1 global loads, and Mark estimated it pays off only when tier 2 runs at 3:1 over tier 1. So the frame-init unrolling is the tier-1-safe part.

**10. Tagged integers on the evaluation stack (https://github.com/faster-cpython/ideas/issues/676, plus deferred refcounts https://github.com/faster-cpython/ideas/issues/677 and https://github.com/python/cpython/issues/120024)**
- **Status:** the infrastructure exists (`Py_INT_TAG`, `PyStackRef_TagInt`/`IsTaggedInt` in `pycore_stackref.h`). It is used only internally (lasti, virtual iterators). Full tagged small ints was listed as "possible future work for 3.15" and was never started.
- **Assessment:** high potential and a large effort, with C-API boundary costs. It is the biggest structural lever still unpulled.

**Honorable mentions**
- Small-list freelist, https://github.com/python/cpython/pull/129921: neutral on pyperformance (1.01x), open and stale.
- `--enable-bolt`: 1–5% claimed at 3.12, still experimental, and breaks with new LLVM versions. See https://github.com/python/cpython/issues/101525 and https://github.com/faster-cpython/ideas/issues/224.

---

## 2. Area notes

**Dispatch: tail calls, computed goto, register allocation**
- The headline 10–15% for tail calls in 3.14 was mostly a workaround for an LLVM 19 regression (tail duplication, LLVM PR #78582, fixed by #114990). Against clang-18 or GCC, the real gain is 1–5% (Nelhage blog).
- GCC 14 already dispatches computed goto well.
- The "switched-goto" idea for compilers without computed goto (https://github.com/faster-cpython/ideas/issues/537) is superseded by MSVC tail calls.
- Clang 22 miscompiled or blew the stack with LTO+PGO, and the fix was to block inlining of huge functions in `ceval.c` (https://github.com/python/cpython/issues/148284). This is a reminder that eval-loop codegen is compiler-fragile.
- `_Py_HOT_FUNCTION` for clang (https://github.com/python/cpython/pull/130891) was closed with no numbers.

**Specialization coverage (main)**
- **Present:** BINARY_OP (16 variants including `BINARY_OP_EXTEND`), LOAD_ATTR (13), CALL (20), and TO_BOOL, COMPARE_OP, CONTAINS_OP, GET_ITER, FOR_ITER, STORE_ATTR, STORE_SUBSCR, CALL_KW and CALL_FUNCTION_EX families.
- **Gaps to check with pystats:**
  - COMPARE_OP has only float, int and str;
  - STORE_SUBSCR has only dict and list;
  - `LOAD_ATTR_CLASS` does not unbind classmethods (https://github.com/python/cpython/pull/148610, open, but it may count as JIT work);
  - the constant-builtin guard elimination PR, https://github.com/python/cpython/pull/132708, is open.
- **Superinstructions:** tier-1 superinstructions have been mostly consolidated. JIT superinstructions (https://github.com/faster-cpython/ideas/issues/647) were never benchmarked.

**Object layout and allocation**
- **Already landed:** inline values and managed dicts (3.13), small-int cache raised to 1025, and freelists for ints, floats, ranges, methods and so on. Umbrella PR https://github.com/python/cpython/pull/128368 measured 1.08x geomean on its microbenchmarks; the worthwhile parts were split into individual PRs.
- **Header shrinking:** reducing plain objects from 8 to 6 words (https://github.com/python/cpython/issues/95245) and compact headers (https://github.com/faster-cpython/ideas/discussions/125) have no recent activity.
- **mimalloc:** the default only for free-threaded builds. For the GIL build it is available via `PYTHONMALLOC=mimalloc`. I found **no published pyperformance comparison** for the GIL build (unverified gap), and there are memory-overhead concerns (https://github.com/python/cpython/issues/135153).
- **Huge pages for pymalloc** (https://github.com/python/cpython/issues/144319): merged as opt-in (`PYMALLOC_USE_HUGEPAGES`, 2 MiB arenas). Allocation microbenchmarks improved 7–30%; no pyperformance number.

**GC:** covered in candidate 6. The 3.16+ incremental GC needs a PEP. Shannon's "better cycle collector" idea (marking possible cycle roots on decref) is in the 3.14 plan and was never started.

**Build level**
- `-fno-semantic-interposition` is already used under `--enable-optimizations` (`configure.ac:1915`).
- BOLT is experimental.
- Frame pointers are default-on at a cost of about 1.5–2%.
- I found no current CPython discussions on `-fno-plt`, static libpython or text hugepages (not verified either way).
- The official Windows build is now tail-call, and macOS is 5% faster with tail calls.
- python-build-standalone/uv ship clang tail-call builds.

**Startup**
- Deep-freeze was removed in 3.13 (https://github.com/python/cpython/issues/108716).
- PEP 690 was rejected. PEP 810 explicit lazy imports is in 3.15.

**Tier 2 / JIT (all frozen until PEP 836 is resolved)**
- **Merged:** trace recording ("dual dispatch"), JIT top-of-stack caching, refcount elimination (https://github.com/python/cpython/issues/134584), and `_MAKE_HEAP_SAFE` elimination (https://github.com/python/cpython/pull/144414).
- **Open or stalled:**
  - constant pool, https://github.com/python/cpython/pull/140968;
  - free-threaded JIT, https://github.com/python/cpython/pull/141595;
  - executor growth limit, https://github.com/python/cpython/pull/143814.
- **Abandoned:** partial evaluator, https://github.com/python/cpython/pull/124910 (the author wanted a different approach).
- **Pyston's Marius Wachtler, DynASM hybrid JIT PoC** (https://github.com/python/cpython/pull/146235, March 2026, stale): **+4–5% geomean** over main, including PGO+LTO+tailcall; nbody 1.8x. Not intended for merge.
- **Plan and history:** https://fidget-spinner.github.io/posts/faster-jit-plan.html; the 3.15 JIT write-up is https://blog.python.org/2026/03/jit-on-track/.

**Other runtimes**
- Pyston's roughly 30% broke down as about 10% landed independently in CPython, 10% in pyston-lite, and 10% that was never upstreamed (https://blog.pyston.org/2022/06/08/announcing-pyston-lite-our-python-jit-as-an-extension-module/). I did not verify the list of specific patches.
- Cinder's contributions landed as immortal objects (PEP 683), comprehension inlining (PEP 709), eager tasks, and lazy imports (now PEP 810).
- RegCPython (register VM, academic, https://dl.acm.org/doi/10.1145/3568973) was never pursued; see https://github.com/faster-cpython/ideas/issues/22.

## 3. Measurement practice

- **pyperformance plus the Pyston macrobenchmarks via bench_runner.** Public results: https://github.com/faster-cpython/benchmarking-public. After the layoffs, infrastructure was being moved to the PSF, and ARM donated a machine.
- **Community dashboards:** Thomas Wouters' curated runs (https://github.com/Yhg1s/python-benchmarking-public) and Savannah Ostrowski's daily JIT-vs-interpreter tracker https://www.doesjitgobrrr.com (https://github.com/savannahostrowski/doesjitgobrrr).
- **pystats** for counts and miss rates (`ideas/pystats_docs.md`; https://github.com/faster-cpython/ideas/issues/652 on pystats drifting unexpectedly).
- **Noise:** https://github.com/faster-cpython/ideas/issues/551 ("despairing at the noisiness") and https://github.com/faster-cpython/ideas/issues/109.
- **Layout sensitivity:** the 2016 "performance depends on dead code" thread (https://mail.python.org/pipermail/speed/2016-April/000341.html); the LLVM 19 tail-duplication effect; the Clang 22 inlining crash. macOS results were explicitly called inconsistent in PR 135063.
- **Cachegrind:** instruction-count benchmarking is not part of CPython's official pipeline (I found no evidence it is).
- **Practical advice:** report geomean on at least two machines and compilers. Pin the compiler version, the frame-pointer setting, and whether tail calls are on.

**Unverified or could not access:** the Arch MR contents; the GCC 16 changes page (403); the GCC AArch64 `preserve_none` status; the pyperformance claim in PR 150425; comment threads on some python/cpython issues (the GitHub API is not enabled for python/cpython in this session, so I used WebFetch summaries). A shallow clone of faster-cpython/ideas was made in the session scratchpad.

---

# Diagnoses & complaints (added 2026-09-27)

This section collects places where someone measured, profiled or complained that "X is slow" or "Y regressed", whether or not a fix followed. For each entry: the complaint or diagnosis, the evidence, a link, whether current `main` (6af40a6) addresses it (checked in source where feasible), and how it could become a fix.

**Main data source.** The newest public tier-1 pystats aggregate from the Faster CPython benchmark runner is `faster-cpython/benchmarking-public`, `results/bm-20250719-3.15.0a0-800d37f/...-pystats.md`. It covers all of pyperformance, about 231 billion tier-1 instructions. **The public pystats runs stop in July 2025**, after the Azure runner was lost in the layoffs; nobody has published pystats for current main. Where I cite pystats below, I checked current `Python/specialize.c` and `Python/bytecodes.c` to see whether the gap still exists.

## A. Specific and still unaddressed (highest value)

**A1. FOR_ITER over zip, dict views and enumerate is never specialized.**
- **Evidence:** pystats show 28.9% of FOR_ITER executions deferred (1.48B). The failure kinds are zip 33.8%, dict items 18.4%, dict keys 17.2%, enumerate 9.0%, set 4.5%, seq iter 4.3%.
- **Main:** in `_Py_Specialize_ForIter` (`Python/specialize.c:2681`), the "null index" path only handles `PyRangeIter_Type` and `PyGen_Type`. Everything else falls through to generic `FOR_ITER`. An attempt for dict items, https://github.com/python/cpython/pull/143666, was JIT-oriented and closed unmerged in 2026-01.
- **Fix:** add a tier-1 `FOR_ITER_ITERNEXT`, guarded on the type version or exact type, that calls `tp->tp_iternext` directly and skips the generic path. Or add direct variants for zip, enumerate and dict item iterators.

**A2. COMPARE_OP and CONTAINS_OP miss common types.**
- **COMPARE_OP evidence:** 9.3% deferred (530M). Of the failures, tuple is 35%, different types (for example int vs float) 23.5%, big int 10%, baseobject 7.5%.
- **CONTAINS_OP evidence:** 7.8% deferred. Failures are str 34%, list 25%, tuple 25%.
- **Main:** COMPARE_OP has only `FLOAT`, `INT` and `STR`; CONTAINS_OP has only `SET` and `DICT` (`bytecodes.c`). Both gaps are unaddressed.
- **Fix:** mixed int/float compare, `x in str`, and `x in tuple/list` (small linear scan with an identity fast path). Tuple compare could route through `BINARY_OP_EXTEND`-style descriptors.

**A3. Array, dict-subclass and Python-`__setitem__` subscripts fall back to the generic path.**
- **BINARY_OP evidence:** 12.3% deferred (2.38B, the largest deferred family). Failures include subscr array 11.6%, subscr Counter 10.5%, tuple slice 10.1%, defaultdict 8.0%, remainder 5.8%, floor divide 3.9%, shifts about 5%, and subscr bytes 2.4%.
- **STORE_SUBSCR evidence:** 42.7% deferred. Failures are other 38%, array int 24%, Python `__setitem__` 20%, dict subclass without override 10.6%.
- **Related complaint:** array loops are 1.57x slower in 3.12 than 3.11 on macOS; https://github.com/python/cpython/issues/123540 is **still open with no diagnosis**. My inference is that the array subscript gap is a likely contributor.
- **Main:** the xor/and/or int cases are now handled by `BINARY_OP_EXTEND` (`specialize.c:2255`). Array, Counter/defaultdict (dict subclasses with `__missing__`), tuple slices, `%`, `//` and shifts on compact ints are still unhandled. `BINARY_OP_SUBSCR_DICT` requires an exact dict.
- **Fixes:**
  - `BINARY_OP_SUBSCR_SQ_ITEM`: any type whose `mp_subscript` or `sq_item` is known, with a compact int index. This covers array, deque, bytes, bytearray, range and memoryview.
  - `BINARY_OP_SUBSCR_DICT_SUBCLASS`, when the type's `mp_subscript == dict_subscript`.
  - `STORE_SUBSCR_GETITEM`-style frame pushing for Python `__setitem__`.
  - Int `%`, `//`, `<<` and `>>` added to `binaryop_extend_descrs`.

**A4. About 25% of Python-function frames are entered from C, through a full `_PyEval_EvalFrameDefault` entry.**
- **Evidence (pystats "Call stats"):** 24.8% of Python calls are not inlined. The breakdown is function vectorcall from C 17.0%, generator resumption from C 7.7%, `api` 6.1%, slot wrappers (Python dunders via `slot_tp_*`) 4.1%, method 1.7%.
- **Supporting counts:** `INTERPRETER_EXIT` runs 1.87B times (0.8% of instructions); 72% of those follow `RETURN_VALUE` and 26% follow `YIELD_VALUE`. Each C→Python entry pays C-stack frame setup, the entry frame, and loss of specialization context.
- **Main:** structurally unchanged. `CALL_ALLOC_AND_ENTER_INIT`, `BINARY_OP_SUBSCR_GETITEM`, `LOAD_ATTR_PROPERTY` and `FOR_ITER_GEN` inline some cases, but consumers such as `sorted(key=)`, `map`, `sum(gen)`, `"".join(gen)` and `list(gen)` still re-enter.
- **Fix:** inline more Python-dunder calls: `STORE_SUBSCR` with Python `__setitem__`, COMPARE_OP with Python `__eq__`/`__lt__`, CONTAINS_OP with `__contains__`, FOR_ITER with Python `__next__`. Also specialize `CALL` of `list`/`tuple`/`sum`/`join` on a generator into an in-interpreter loop. Shannon's 3.14 plan item "move `any`/`all`/`enumerate` to Python" (`faster-cpython/ideas/3.14/README.md`) targets the same cost.

**A5. Class instantiation with keywords, or with a non-simple `__init__`, is not specialized.**
- **Evidence:** pystats CALL failure kinds are dominated by "init not simple" and "init not python". CALL_KW is 91.8% deferred with an 85.8% miss rate (small in pyperformance, but ubiquitous in real code: `Point(x=1, y=2)`, dataclasses with `kw_only`, attrs).
- **Main:** `specialize_class_call` (`specialize.c:1641-1649`) needs a Python `__init__` with `function_kind == SIMPLE_FUNCTION`, meaning no `*args`, `**kwargs` or keyword-only arguments (`specialize.c:1474`). The CALL_KW family is only `BOUND_METHOD`, `PY` and `NON_PY`, with no `CALL_KW_ALLOC_AND_ENTER_INIT`. Unaddressed.
- **Fix:** add `CALL_KW_ALLOC_AND_ENTER_INIT`, and allow keyword-only arguments in the init check (reusing the `CALL_KW_PY` arg binding).

**A6. `STORE_ATTR` specializations do wasted work on fresh objects** (https://github.com/python/cpython/issues/144141, Shannon, 2026-01, open, no PR).
- **Diagnosis:** a store into a newly created object reads the old value, tests it for NULL, updates insertion order and XDECREFs it. That is 48 instructions instead of 26 for `_STORE_ATTR_INSTANCE_VALUE`, and 32 instead of 14 for `_STORE_ATTR_SLOT`, on AArch64.
- **Related:** pystats show `STORE_ATTR_INSTANCE_VALUE` with a **10.6% miss ratio**. Shannon's TODO list (https://github.com/python/cpython/issues/144388) also flags the insertion-order update cost.
- **Main:** unchanged (`bytecodes.c:3130-3147`).
- **Fix:** a `_STORE_ATTR_INSTANCE_VALUE_NULL` variant, selected when the optimizer knows or guards that the slot is NULL (inside `__init__`), and a cheaper insertion-order update.

**A7. High deopt ratios on the hottest attribute specializations.**
- **Evidence:** `LOAD_ATTR_INSTANCE_VALUE` (5.3B executions, 2.3% of all instructions) has a 6.9% miss ratio. Other miss ratios: `LOAD_ATTR_METHOD_WITH_VALUES` 10.2%, `LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES` 38.6%, `CALL_BOUND_METHOD_EXACT_ARGS` 14.4%, `FOR_ITER_TUPLE` 15.1%, `TO_BOOL_ALWAYS_TRUE` 25.5%, `TO_BOOL_NONE` 11%. Misses total 1.54B (0.7% of instructions), and each costs a deopt plus re-dispatch of the generic op.
- **LOAD_ATTR failure kinds:** mutable class 17%, method 14%, overriding descriptor 11%, metaclass attribute 5%.
- **Diagnosis:** tier 1 has monomorphic caches only, so polymorphic sites keep deopting and re-specializing under backoff.
- **Fix ideas:** 2-way polymorphic `LOAD_ATTR_INSTANCE_VALUE` (two type versions), or a less oscillation-prone backoff. Cheap to prototype and measure with pystats miss counters first.

**A8. `NOP`s are executed about 2.6B times (1.1% of all tier-1 instructions).**
- **Evidence (pystats):** 28% are NOP→NOP chains; the predecessors are `JUMP_BACKWARD` 30%, `RESUME_CHECK` 19.5% and `POP_JUMP_IF_FALSE` 9%.
- **Verified on main** with a local build (`dis`): the compiler leaves a `NOP` for the line of `while True:` (the loop back-edge jumps to it, so it runs every iteration), for `try:` (runs on every call of a function whose body starts with `try`), and for `pass`.
- **Fix:** let line-number events be derived without a real instruction. For example, point jump targets past line-only NOPs and let instrumentation (sys.monitoring) re-insert line markers when tracing is on. Expected gain is small (≤0.5%) but it is nearly free at runtime.

**A9. Stack-shuffle overhead: `COPY` + `SWAP` are 2.0% of executed instructions.**
- **Evidence (pair counts):** `COPY COPY → BINARY_OP_SUBSCR_LIST_INT/BINARY_OP` and `... → SWAP SWAP → STORE_SUBSCR` is the `a[i] op= x` pattern, with 4 extra dispatches each time. `COPY → TO_BOOL_BOOL` comes from `and`/`or` and comparison chains.
- **Other stack traffic:** `PUSH_NULL` is 0.7%, mostly after `LOAD_ATTR_MODULE` and `LOAD_FAST_BORROW` for calls.
- **Fix:** macro-instructions for augmented subscript and attribute assignment, or compiler changes that avoid DUP/ROT. `LOAD_ATTR_MODULE`+`PUSH_NULL` fusion (it could push NULL itself, as `LOAD_GLOBAL` does).

**A10. Shared-libpython builds pay `__tls_get_addr` on every `_PyThreadState_GET()` / `_PyInterpreterState_GET()` (my own diagnosis, not measured).**
- **Evidence:**
  - `_Py_tss_tstate` and `_Py_tss_interp` are plain `extern thread_local` (`Include/internal/pycore_pystate.h:93-94`, `Include/pyport.h:487-499`) with no `tls_model` attribute.
  - With `-fPIC` (used by `--enable-shared`, the configuration Fedora and others ship), GCC emits `call __tls_get_addr@PLT` for such an access. I verified the codegen with a minimal repro (gcc 13, `-O2 -fPIC -fno-semantic-interposition`); with `-mtls-dialect=gnu2` it becomes a TLSDESC indirect call.
  - Hot paths that hit it:
    - every freelist push and pop (`_Py_freelists_GET` → `_PyInterpreterState_GET`, `pycore_freelist.h:28`);
    - every GC-object dealloc (`_Py_Dealloc`, `Objects/object.c:3318`);
    - many allocation paths.
  - Static builds (python.org, bench_runner) use `%fs:` directly, so **pyperformance as normally run cannot see this cost.**
- **Main:** unaddressed. The only prior discussion I found (https://discuss.python.org/t/tls-related-code-in-python-pystate-c/56822) contains no measurements.
- **Fix:** `__attribute__((tls_model("initial-exec")))` on these two variables when building libpython (glibc reserves surplus static TLS for dlopen'd libraries; musl needs checking), or `-mtls-dialect=gnu2`. **To measure:** pyperformance with `--enable-shared`, before and after.

**A11. The trashcan now runs inside `_Py_Dealloc` for every GC object** (https://github.com/python/cpython/pull/132280, gh-124715, merged 2025-04 for 3.14).
- **Diagnosis (mine, not measured):** every dealloc of a GC type now reads the thread state and computes the recursion-margin check before calling `tp_dealloc` (`Objects/object.c:3317-3324`). In shared builds that read is also the TLS call from A10.
- **Separately:** https://github.com/python/cpython/issues/130706 (open) shows the reftracer check in `_Py_Dealloc` forcing 3 push/pop register spills on x86-64. The report is about the free-threaded build but the check exists in both builds (`_PyReftracerTrack` at `object.c:3345`).
- **Fix:** move the rare paths (trash deposit, reftracer) out of line and pass tstate in from callers that already have it.

**A12. Remaining refcount traffic.**
- **Evidence (pystats object stats):** interpreter mortal increfs are 43.2B and decrefs 55.0B. Outside the interpreter there are 23.5B *immortal* increfs and 23.1B immortal decrefs: C code still branches on immortality for, for example, None, True, False and small ints. Only 70.6% of allocations come from freelists.
- **Remaining non-borrowed local loads:** `LOAD_FAST` (non-borrow) still runs 3.47B times, 8% of local loads.
- **Related issues:**
  - https://github.com/python/cpython/issues/117425 (remove incref/decref of specific immortal objects; open since 2024);
  - https://github.com/python/cpython/issues/145860 (`BUILD_INTERPOLATION`/`BUILD_TEMPLATE` do incref-then-decref; open, PR https://github.com/python/cpython/pull/148201);
  - the unchecked items on https://github.com/python/cpython/issues/144388.
- **Fix:** convert more `LOAD_FAST` to `LOAD_FAST_BORROW` via extended basic blocks (Shannon's TODO), and use `Py_DECREF_MORTAL` / immortal-aware no-op variants in hot C paths.

**A13. `BINARY_SLICE` and `STORE_SLICE` are never specialized** (100% deferred; 556M and 113M executions).
- **Main:** `_SPECIALIZE_BINARY_SLICE` and `_SPECIALIZE_STORE_SLICE` are literal "Placeholder until we implement ... specialization" stubs (`bytecodes.c` around lines 1078 and 1116). `_BINARY_SLICE` has inline fast paths for list, tuple and str, but **bytes/bytearray slicing allocates a slice object** and goes through `PyObject_GetItem`.
- **Related complaint:** 188-byte `bytes` packet slicing in a loop was reported as 3–6x slower on 3.14 than 3.11 (https://discuss.python.org/t/python-3-11-and-function-call-frequency/108013, July 2026). It was never reproduced or diagnosed.
- **Fix:** a bytes fast path in `_BINARY_SLICE`; real specializations; a list `STORE_SLICE` (JIT-only PR https://github.com/python/cpython/pull/149446 is open).

## B. Reported regressions between versions

| Complaint | Evidence | Link | Status on main | Turn into fix |
|---|---|---|---|---|
| Loops over `array.array` are 1.57x slower (macOS) and about 5% slower (Linux) in 3.12 than 3.11 | reproducer in the issue | https://github.com/python/cpython/issues/123540 (open) | likely partly A3 (array subscripts unspecialized) | A3 |
| List comprehension about 3–7% slower in 3.12 than 3.11, with higher variance | `[a*2 for a in range(10**6)]` | https://github.com/python/cpython/issues/113041 (open, no diagnosis) | unknown; re-measure on main | bisect with pystats |
| Comprehensions and generators 1.34x slower in 3.15a3 than 3.11.14 (microbenchmarks) | Zenodo report; quality and methodology unverified | https://zenodo.org/records/18355482 | unknown | re-measure; generator-from-C is A4 |
| Cyclic GC 6.3x slower on main vs 3.13 (incremental GC) | `bm_gc_collect.py` | https://github.com/python/cpython/issues/129210 (open) | incremental GC reverted in 3.14.5/3.15; the linked PR https://github.com/python/cpython/pull/132488 (skip resurrection check without finalizers) is still unmerged | revive #132488 |
| Sphinx 48% slower (incremental GC) | | https://github.com/python/cpython/issues/124567 (closed) | reverted | — |
| pyperformance `coverage` benchmark 1.36x slower in 3.13 than 3.12; `sys.settrace` "dramatic slowdown" in 3.12; tracing severely degraded in 3.11; cProfile 10x overhead in 3.11–3.14 (previously 1.5–2.5x), which also defeats specialization | | https://github.com/python/cpython/issues/107674 (open), https://github.com/python/cpython/issues/93516 (open), https://en.lewoniewski.info/2024/python-3-12-vs-python-3-13-performance-testing/, https://discuss.python.org/t/cprofile-performance-3-8-vs-3-11-9/59034 | open. The `coverage` benchmark sits inside the pyperformance geomean, so the legacy tracing path matters for the headline number | cheaper legacy `settrace` over sys.monitoring |
| `create_gc_cycles` +22%, `gc_traversal` +9%, `many_optionals` +65–73% (argparse) in 3.14 vs 3.13 (Windows) | | https://en.lewoniewski.info/2025/python-314-vs-313-312-311-310-performance-testing-video/ | argparse fixed (https://github.com/python/cpython/issues/142267, formatter recreated twice per `add_argument`, 3.8x); GC items reverted | — |
| `pickle` +19% vs 3.11 and `json_dumps` +7–13% vs 3.12 (Windows, 3.14) | same source | same | not analysed; unverified | profile |
| `bench_mp_pool` 27–315x slower in 3.14 on Linux | | https://github.com/python/cpython/issues/139881 (closed "not planned") | my inference: the default start method on Linux changed from `fork` to `forkserver` in 3.14. This is a benchmark artifact, but it drags the geomean when comparing across versions | set the start method in the benchmark |
| 3.13 about 7.5–25% slower on the python.org macOS ARM installer | | https://github.com/python/cpython/issues/122580 | fixed (build config) | — |
| `pathlib.Path` hashing 3–4x slower since 3.12 | | https://github.com/python/cpython/issues/138407 (open; PR https://github.com/python/cpython/pull/138645) | open | stdlib only |
| `asyncio.gather()` slower and more memory since gh-157213 | | https://github.com/python/cpython/issues/158239 (open, PR https://github.com/python/cpython/pull/158240) | **new regression on main (Sept 2026)**; relevant to the async_tree benchmarks | merge the fix |
| Bound-method creation regression (3.9) | Raymond Hettinger | https://github.com/python/cpython/issues/83298 (open since 2019) | largely mitigated: `LOAD_ATTR_METHOD_*` avoids creation for calls, and there is a `pymethodobjects` freelist (`pycore_freelist_state.h:33`) | close or re-measure |
| perf trampoline / frame pointers cost 8% (2023 estimate) | | https://discuss.python.org/t/the-performance-of-python-with-perf-support-is-not-great-and-is-going-to-get-a-lot-worse/25280 | PEP 831 later measured 0.1–2.3% and made frame pointers the default in 3.15 | `--without-frame-pointers` recovers it |

## C. Regressions from the free-threading refactors in the default (GIL) build

**No published measurement isolates these.** Checked in source:

- `LOCK_OBJECT`/`UNLOCK_OBJECT` are `(1)`/no-op in the default build (`Python/ceval_macros.h:322-323`).
- `FT_ATOMIC_*` wrappers are plain loads and stores (`Include/internal/pycore_pyatomic_ft_wrappers.h:150-164`).
- Stackref close and dup test a tag bit in place of the immortality check (`pycore_stackref.h:527-724`). This should be roughly cost-neutral, but no one has published an A/B.

Real candidates for leftover cost:
- TLS in shared builds (A10);
- the trashcan and reftracer in `_Py_Dealloc` (A11);
- the extra `index_or_null` stack slot for every iterator (virtual iterators, 3.15), which adds `POP_ITER` at 0.5% of instructions.

The free-threaded build itself has many open scaling and overhead issues, for example https://github.com/python/cpython/issues/157914 (PyMutex spin loop), https://github.com/python/cpython/issues/156132 (refcount memory ordering) and https://github.com/python/cpython/issues/140795 (ssl). They are out of scope for the GIL-build geomean.

## D. Methodology notes relevant to diagnoses

- Pystats have not been published for main since 2025-07. **Regenerating pystats for main** (`--enable-pystats` plus `Tools/scripts/summarize_stats.py`) is the cheapest first step to re-rank A1–A9.
- Pystats "Failure kind" percentages count specialization *attempts*, not executions. Rank candidates by the deferred execution counts. I reported both.
- The July 2025 pystats run used the incremental GC (gen-0 collections were 0). GC visit numbers (gen-1: 9.8B visits for 95M objects collected) do not describe the restored generational GC.
