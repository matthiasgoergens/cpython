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
