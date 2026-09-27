> **Measured so far (2026-09-27, see LOG.md):** M1 per-type cache: +0.75% geomean *instructions*
> (richards +6%), timing pending. M2 gh-132336 noinline: claimed 0.9% slowdown **not reproduced**
> (−0.12% [−0.37, +0.12]). M3 frame pointers: −1.15% [−1.53, −0.81] without them. R1 PEP 848 vs.
> thresholds: gen-0 ×2 −2.24%, ×4 −3.75% geomean, but only −0.28% / −0.64% excluding async_tree*.

# Disputed or inconclusive CPython performance measurements

Compiled 2026-09-27 against `main` (3.16a0; local HEAD e7a995ef; the clone is shallow).
Sources are GitHub PR and issue pages, discuss.python.org, and PEPs. I read each through WebFetch summaries, so re-check the exact numbers on the page before quoting them.

What goes in this file: the design work is done, and the missing piece is a measurement people can trust.
The file has two main sections, in the order the user asked for:

- **Section 1: merged changes with a measured or suspected slowdown in the default (GIL) build.** The code is on main, so a solid A/B leads directly to a revert or a fix.
- **Section 2: changes that were rejected, closed, or stalled** because the claimed win was small, noisy, or never measured. A good measurement could revive them.

Section 3 lists open PRs where a slowdown is being called noise. Section 4 has methodology notes.

Two scores are given for each item:
- **Impact (I):** likely effect on the pyperformance geomean for the default build (H = at least 1%, M = 0.3–1%, L = below 0.3% or niche).
- **Cost (C):** how cheaply a two-build comparison settles it (cachegrind instruction counts plus wall-clock timing with layout randomization). Cheap = one commit or one flag, Medium = needs a rebase or port, Expensive = needs a reference implementation or non-pyperformance workloads.

Standard settle recipe (called **"std A/B"** below):
1. Build the commit and its parent, both `--enable-optimizations --with-lto`, default GIL build, with the same compiler (both GCC and clang if possible).
2. Run the full pyperformance suite with `pyperf compare_to` and significance testing.
3. Run cachegrind `Ir` counts on a representative subset.
4. Re-run with Stabilizer or link-order randomization (at least 3 layouts) so that a ±1% layout effect is not taken for a real result.

---

## Section 1: merged, measured or suspected slowdown (still on main)

### M1. PR #150160 (gh-145685): per-type method cache [I: M–H, C: Cheap]
- Link: https://github.com/python/cpython/pull/150160 (merged 2026-07-21, commit `daf09e17` on main).
- **Change:** replaces the global MRO/type-attribute cache with a cache on each type. The main use is free-threaded scaling of `_PyType_Lookup` and `_PyType_LookupStackRefAndVersion`.
  - Adds `Python/typecache.c` (246 lines).
  - Shrinks `Objects/typeobject.c` (−220 / +x).
  - Touches `pystate.c` and `pystats.c`.
  - Follow-up gh-155243 removed `_PySeqLock`.
- **Numbers:**
  - The GIL build on the vultr x86-64 runner (June 16) came out **about 1% slower** on the pyperformance geomean. The author called it "within noise".
  - colesbury (clang-20, LTO) found that cache misses "drop substantially" and the overall effect is "small (possibly within the noise margin)". His reasoning: the MRO hit rate was already high, and specialization bypasses the cache.
  - **Mark Shannon objected** to calling 1% noise. He asked for hit, miss and collision statistics plus memory impact. Stats were added (resizes and invalidations), but no second GIL-build timing was posted.
- **Settle:** std A/B of `daf09e17` against `daf09e17^`.
  - Watch LOAD_ATTR/LOAD_GLOBAL deopt-heavy and class-creation-heavy benchmarks: `richards`, `deltablue`, `go`, `raytrace`, `chaos`, `sqlglot*`, `dataclasses`, `create_gc_cycles`, `typing_runtime_protocols`.
  - Use pystats to count `_PyType_Lookup` calls for each benchmark. If the count is small, 1% cannot come from the lookup itself, and any difference is layout or icache.
- **Revert or fix scope:** `Objects/typeobject.c` lookup path and `Python/typecache.c`. A partial fix is to keep the per-type cache only under `Py_GIL_DISABLED` and use the old global cache in the default build.

### M2. PR #132337 (gh-132336): mark interpreter "slow path" functions `noinline` [I: M, C: Cheap]
- Link: https://github.com/python/cpython/pull/132337 (merged 2025-04-10, 3.14).
- **Change:** adds `Py_NO_INLINE` to the `_Py_Specialize_*` functions and similar helpers, because PGO+LTO was inlining them into the eval loop. On main the attribute appears 15 times in `Python/specialize.c`, plus `Python/instrumentation.c` and `Python/optimizer.c`.
- **Numbers:**
  - Free-threaded build: about **3.6% faster** (Meta's FT benchmark runner).
  - Default build: about **0.9% slower**. mpage called this "mostly noise".
  - **Mark Shannon:** "Why so casually dismiss a 0.9% slowdown as noise? You seem willing enough to treat a 0.9% speedup as real in other cases." mpage offered to restrict the change to the FT build. It was merged unrestricted.
- **Settle:** std A/B on main with the attributes removed (or `#ifdef Py_GIL_DISABLED`).
  - This needs PGO, because the effect only exists under PGO+LTO inlining. Cachegrind still helps: compare `Ir` and I1 misses in `_PyEval_EvalFrameDefault`.
  - Try GCC and clang, and computed-goto and tail-call builds, since inlining decisions differ.
- **Fix scope:** one macro per function. The cheapest candidate on this list for a clean, actionable result.

### M3. PEP 831 frame pointers on by default (PR #149201) and the tail-call interaction (issue #154124) [I: H, C: Cheap]
- Links:
  - https://github.com/python/cpython/pull/149201 (merged 2026-05-01)
  - https://peps.python.org/pep-0831/
  - https://github.com/python/cpython/issues/154124 (open)
- **Change:** `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer` is the default from 3.15 (`configure.ac:2622`, with `--without-frame-pointers` to opt out).
- **Numbers (PEP):**
  - Geomean overhead: 0.5% (M2), 0.1% (M3 Pro), 0.2% (RPi), 1.5% (Altra Max), 2.3% (Graviton c7g), 1.9% (i7-12700H), 1.8% (EPYC 9654), 1.5% (Xeon 8480).
  - `xml_etree_*` benchmarks are up to **1.31x** slower.
  - On x86-64: +5.5% instructions and +3.3% wall time on C-call-heavy loads, with IPC up 2.1%.
  - The PEP itself says the numbers are "difficult to characterize consistently".
- **Tail-call builds (#154124, Fidget-Spinner, clang 22, i7-12700H, fastmark subset):**
  - Frame pointers cost **2.7%** in the tail-call interpreter, against about 1.5–2% for computed goto, because each opcode handler is its own function with its own prologue.
  - With `-fomit-frame-pointer -momit-leaf-frame-pointer -mreserve-frame-pointer-reg` on the handlers only (clang 21+, which needs handlers in a separate translation unit), the cost drops to **0.2%**.
  - No PR has landed.
- **Settle:** std A/B of default against `--without-frame-pointers`, on GCC and clang, for both interpreter kinds.
  - Then prototype the #154124 flag split and check whether it recovers most of the cost while keeping unwinding intact (test with `perf record --call-graph fp`).
  - Report the `xml_etree_*` and C-call-heavy benchmarks separately.
- **Why it ranks high:** at about 1.5–2% on x86-64, this is the largest known accepted overhead in the default build. The fix is build-system only.

### M4. PR #154430 (gh-154401): skip the thread-state fetch for non-GC types in `_Py_Dealloc` [I: L–M, C: Cheap]
- Link: https://github.com/python/cpython/pull/154430 (merged 2026-07-22, `6fb51f23`). It changes about 15 lines in `Objects/object.c:3313ff`.
- **Numbers:** measured on the **free-threaded build only**.
  - ARM64 macOS: 1.01x faster geomean (fannkuch 1.07x).
  - x86-64 Linux, 2 vCPU: 1.02x faster (nbody 1.18x, which looks like noise).
  - The reviewer noted "The 7% slowdown on x86-64 richards is a little concerning, but I suspect that's just noise". The author agreed the x86 data had "a bit too much noise".
- **The default build was never measured**, although the change applies to it. It adds a branch on `Py_TPFLAGS_HAVE_GC` to every dealloc.
- **Settle:** std A/B on the GIL build (static and `--enable-shared`, since the TLS cost differs; see PRIOR_ART A10/A11). Look at `richards` specifically, and at cachegrind `Ir` in `_Py_Dealloc`.

### M5. PR #145381 (gh-87613): Argument Clinic `@vectorcall` decorator [I: L–M, C: Cheap]
- Link: https://github.com/python/cpython/pull/145381 (merged 2026-09-10, `86b55a64`).
- **Change:** Argument Clinic generates vectorcall parsers for `__new__`/`__init__`, replacing some hand-written ones and adding vectorcall to types that had none.
- **Numbers:**
  - skirpichev's micro runs: geomean 1.00x slower (optimized build) and 1.01x faster (default configuration). Within those: `bytes()` **1.09x slower**, `int(str)` 1.02x slower, `int()` 1.03x faster.
  - cmaloney's pyperformance run: most results not significant. Significant results were `gc_traversal` 1.06x faster, `pickle_list` 1.03x faster, **`unpickle_list` 1.03x slower** and **`xml_etree_parse` 1.03x slower**.
  - Merged with follow-ups promised for bytes and bytearray.
- **Settle:**
  - std A/B of `86b55a64` against its parent.
  - pyperf `bench_func` micro-benchmarks for each converted constructor (`bytes`, `bytearray`, `int`, `tuple`, `list`).
  - Check whether `unpickle_list` and `xml_etree_parse` reproduce under layout randomization.
- **Fix scope:** restore the hand-written fast paths (for example the `switch(nargs)` in `bytes_new`) wherever generated code is slower.

### M6. PR #137828 (GH-137759): drop the `_PyObject_HashFast` str fast path from sets [I: L, C: Cheap]
- Link: https://github.com/python/cpython/pull/137828 (merged 2026-06-15, `ecbd31ee`).
- **Numbers (gcc -O3, isolated CPU, micro):**
  - `set(range(256))` 1.08x faster.
  - **`set(map(chr, range(256)))` 1.06x slower.**
  - Geomean 1.01x faster.
  - No pyperformance run. The reviewers disagreed about whether one pointer comparison matters.
- **Settle:** micro A/B for string-set build and lookup, plus pyperformance subsets that use string-keyed sets (`deltablue`, `richards`, `sqlglot*`, `tomli_loads`, `mdp`). Use cachegrind on `set_contains_key` / `set_add_key` (`Objects/setobject.c:603-625`).

### M7. PR #145751 (GH-145692): `DEOPT_IF` to `EXIT_IF`, plus the related int guards (#150425) [I: L–M, C: Cheap]
- Links:
  - https://github.com/python/cpython/pull/145751 (merged 2026-03-12, `453562a4`)
  - https://github.com/python/cpython/pull/150425 (closed 2026-07-10)
  - Open follow-up: https://github.com/python/cpython/pull/153487
- **Change:**
  - Mostly relabels guards, but it also changes tier-1 handlers: `BINARY_OP_SUBSCR_LIST_INT` and `STORE_SUBSCR_LIST_INT` now handle negative indices, and list locking moved into its own uop.
  - About 160 lines of `bytecodes.c` change, and `generated_cases.c.h` changes.
- **Numbers:**
  - #145751: "generally neutral, less than 1% faster or slower across 5 different platforms". No table was posted.
  - #150425 found that `pyflate` is **1.03x slower**, which it attributed to the `DEOPT_IF(!_PyLong_IsNonNegativeCompact(...))` guards on `_BINARY_OP_SUBSCR_LIST_INT`. #153487 ("Factor non-negative compact int subscript guards") is open.
- **Settle:** std A/B of `453562a4` against its parent, looking at the list-subscript-heavy benchmarks (`pyflate`, `fannkuch`, `nqueens`, `spectral_norm`, `hexiom`), with the JIT off. Then test #153487 on top.

### M8. PR #146064 (gh-135871): reload the lock state while spinning in `PyMutex_LockTimed` [I: none for GIL geomean; FT only, C: Cheap]
- Links:
  - Issue https://github.com/python/cpython/issues/157914 (open)
  - Revert PR https://github.com/python/cpython/pull/158082 (open)
  - Commit `daa159f9` on main
- **Numbers:**
  - M4 Max, 4 threads contending on dict writes: 0.507 s on main against 0.260 s with the change reverted.
  - `lockbench.py` sweep: from "3x more throughput" to "6x less throughput" depending on the workload.
- Included for completeness. It does not affect the default-build geomean. Settle it with `Tools/lockbench` sweeps on x86 and ARM.

### M9. PR #144353 (gh-144319): `madvise(MADV_HUGEPAGE)` in pymalloc [I: L, C: Medium]
- Link: https://github.com/python/cpython/pull/144353 (merged 2026-04-30, `51107387`). Follow-up #147963 fixed a huge-page leak in the datastack allocator.
- **Numbers:**
  - Geomean 1.00x on a "rigorous" run, with mixed results: `asyncio_tcp` 1.12x and `regex_effbot` 1.06x faster, which are I/O-noisy benchmarks.
  - Micro: dTLB misses −15 to −19%, wall time −1 to −5%.
- **Why it's inconclusive:** the effect depends on host THP settings (`madvise` or `always`) and on fragmentation, so bench machines disagree. RSS was not reported.
- **Settle:** std A/B with THP=`madvise` and THP=`never`, recording RSS (`--track-memory`).

### M10. PR #145045 (gh-145044): skip `Py_DECREF` on immortal bools in `unsafe_object_compare` [I: L, C: Cheap]
- Link: https://github.com/python/cpython/pull/145045 (merged 2026-03-10).
- **Numbers:** a custom timer showed 1.0166x. picnixz said "in the realm of noise" and asked for pyperf. The PR was merged for consistency with #136018. Settle with pyperf plus cachegrind on `list.sort` of objects.

---

## Section 2: rejected, closed or stalled because the win was small, noisy or unmeasured

### R1. PEP 848, generational incremental GC (target 3.16) [I: H (claimed 3–5%), C: control run Cheap; full A/B Expensive]
- Links:
  - https://peps.python.org/pep-0848/ (Draft, Mark Shannon)
  - https://discuss.python.org/t/pep-848-generational-incremental-garbage-collection/109214
- **Context:** the 3.14 incremental GC was reverted in 3.14.5 and 3.15 (gh-148726, commit `1575a81b` on main), after memory blow-ups and 6x slower `bm_gc_collect` (issue #129210).
- **Claim:** "3-5% speedup on pyperformance", and up to 100x shorter pauses.
- **Challenge (Antoine Pitrou):** "if you were to double the size of gens 0 and 1 in the legacy GC, such as to halve the number of non-full collections, would that erase the performance uplift?" Shannon replied that the gain comes from not collecting very young objects. No control measurement was posted.
- Tim Peters added that pyperformance barely exercises the cyclic GC realistically.
- Neil Schemenauer's `cyclotron` sweeps on 3.14 against 3.13:
  - time −12% to +67.5%
  - RSS +13% to +469%
  - pause time −93% to +479%
- **Settle, first step (no rebuild):** run pyperformance on main with `gc.set_threshold()` scaled 2x and 4x for gen 0 and gen 1. Inject this with a `sitecustomize` or a `PYTHONSTARTUP` equivalent in the benchmark venv. If the scaled thresholds give 3–5%, the PEP's speedup is a threshold effect.
- **Settle, second step:** std A/B of the PEP 848 reference branch (I did not find its PR number; ask Shannon) against main. Record RSS and max pause as well, and include `gc_collect`, `gc_traversal` and `create_gc_cycles`.

### R2. PR #135063 (GH-132554): specialize `GET_ITER`/`FOR_ITER` for `range` with tagged ints [I: M, C: Medium]
- Link: https://github.com/python/cpython/pull/135063 (open, stale since 2026-04-25, awaiting review from Fidget-Spinner and ericsnowcurrently).
- **Numbers:**
  - Micro geomean 1.08x (range loops of length 1 are 1.23x).
  - Mark Shannon's full-suite run: Linux unchanged, Windows +0.3%, **macOS "suspiciously high 8%"**, and "not slower is good enough".
  - The macOS number was never explained.
- **Caveat:** #147967 (virtual-iterator `FOR_ITER` plus `GET_ITER` specialization, merged 2026-04-16) probably overlaps. The PR needs a rebase before anyone measures it.
- **Settle:** rebase, then std A/B on Linux x86-64 and on macOS ARM (clang). If macOS still shows 8%, run cachegrind or `perf stat` to look for a layout artifact.

### R3. PR #129921 (gh-126703): freelists for small lists [I: M, C: Cheap]
- Link: https://github.com/python/cpython/pull/129921 (open, stale).
- **Numbers:**
  - Micro geomean 1.25x (list create, copy and repeat at 1.30–1.35x).
  - The author's pyperformance run: about +1%, with fannkuch 1.07x.
  - **The Faster CPython bench runner** (Fidget-Spinner): "No speedup nor slowdown (it's within noise)".
  - A new run on the runner was requested from Mike Droettboom and never happened.
- **Why it's a good candidate:** a classic case of micro-benchmarks and pyperformance disagreeing, and the missing run is exactly what we can now do.
- **Settle:** std A/B after a rebase. Watch `fannkuch`, `nqueens`, `deltablue`, `sqlglot*`, `json_loads` and `unpack_sequence`. Use cachegrind on `list_dealloc` and `PyList_New`, and record RSS.

### R4. PR #149317 and PR #157499 (gh-149180): `tp_as_number`/`tp_as_sequence`/`tp_as_mapping` never NULL after `PyType_Ready` [I: L–M, C: Cheap]
- Links:
  - https://github.com/python/cpython/pull/149317 (closed 2026-05-06: "incorrectly merged … need to be more careful")
  - https://github.com/python/cpython/pull/157499 (closed 2026-09-15 by vstinner)
- **Numbers:**
  - #149317: 10 pyperformance benchmarks on macOS without PGO/LTO, all "not significant" (chaos 1.01x slower, float 1.01x faster). picnixz asked for `--with-lto` and PGO ("we are changing branches"), and that run was never done.
  - #157499: vstinner measured `PyNumber_Add(1,2)` at 88.1 → 87.4 ns (1.01x) and closed it: "no significant performance difference".
  - ZeroIntensity: the impact "might be better than you think".
- **Settle:** std A/B with PGO+LTO, plus cachegrind on `Objects/abstract.c` hot paths (`binary_op1`, `PyObject_GetItem`, `PySequence_Check`). The ABI concern (a public `PyTypeObject`) is separate and was the real reason for closing.

### R5. PR #143024 (gh-139716): unify `PyStackRef_FromPyObjectSteal` across GIL and FT builds [I: M (hot path), C: Cheap]
- Link: https://github.com/python/cpython/pull/143024 (open, stale).
- **Change:** removes the immortality check and relies on `Py_DECREF` being a no-op for immortals.
- **Status:** colesbury said "Please benchmark this", and it was never done.
- **Mark Shannon:** "why choose the slower scheme, not the faster one?" His point is that the FT scheme needs an extra memory load in `CLOSE`/`DUP`.
- **Settle:** build three variants: main, the PR, and the reverse (the default-build tag-bit scheme used in the FT build). Compare cachegrind `Ir` and D1 misses in `_PyEval_EvalFrameDefault` on about 10 benchmarks, then run std A/B. This directly tests PRIOR_ART section C, which notes that no one has published an A/B of the stackref close/dup schemes.

### R6. PR #148681 (gh-142183): data stack as one resizable array [I: L–M, C: Medium]
- Link: https://github.com/python/cpython/pull/148681 (open, blocked).
- **Numbers:** the author reports "±1% (within the noise range)" on pyperformance, with no table.
- **Blocker:** pablogsal found that the remote unwinder needs 966 reads instead of 4 on a 1000-frame stack.
- The companion fix #145789 (cache one datachunk per tstate) was merged. It showed 1.31x on the chunk-boundary reproducer.
- **Settle:** std A/B on recursion-heavy benchmarks (`recursive_fib`-style, `deepcopy`, `pickle_pure_python`, `sympy`, `mako`), plus RSS and the Tachyon unwinder cost.

### R7. PR #132618 (gh-132042): pre-build the MRO dict for `find_name_in_mro` during class creation [I: L, C: Cheap]
- Link: https://github.com/python/cpython/pull/132618 (open). It was deferred from 3.15 to 3.16 and is now actionable on main.
- **Numbers:**
  - Class-creation micro geomean 1.15–1.18x (Windows, i5-11600K), with the author noting "results are not very stable due throttling".
  - Meta FT runner: "<1% geomean … within noise limits".
- **Settle:** std A/B plus import-time benchmarks (`python_startup`, `-X importtime` on large stdlib imports, `dataclasses`, `typing` creation). The behavioural change (custom `__eq__`/`__hash__` on dict keys) needs review regardless.

### R8. PR #140688 (gh-140328): reuse interned versions of string constants [I: L, C: Cheap]
- Link: https://github.com/python/cpython/pull/140688 (open, stale).
- **Status:** "I haven't run pyperformance yet … once I have access to an appropriate runner". That is the benchmark-infrastructure gap in its plainest form.
- colesbury prefers to "intern all strings in code objects".
- **Settle:** A/B of both variants (the PR, and intern-all) for startup time, RSS and pyperformance geomean.

### R9. Issue #129976: load the tail-call dispatch target earlier [I: M for tail-call builds only, C: Cheap]
- Link: https://github.com/python/cpython/issues/129976 (open).
- **Evidence so far:** 185 more `jmp reg` instructions (916 against 731). No timing was ever posted.
- **Settle:** apply the macro change to `--with-tail-call-interp` (clang 20+) and compare. This matters only if tail-call becomes the default; it is currently opt-in (`configure.ac:7655`).

### R10. Issue #130961: `-fzero-call-used-regs` (NixOS and hardened distros) and the tail-call interpreter [I: none on the default toolchain; 2–3% for affected distros, C: Cheap]
- Link: https://github.com/python/cpython/issues/130961 (open).
- **Numbers:** the flag costs about 2% (computed goto) and about 3% (tail-call). With `__attribute__((zero_call_used_regs("skip")))` on the handlers, the cost is about 1%. Never benchmarked by core developers.

### Superseded, listed so nobody re-investigates
- **PR #139390** (don't GC-track immutable tuples in `PyTuple_Pack`): 1.01x geomean on Windows (richards 1.10x), closed as stale. Main now does this in `PyTuple_Pack` and `_PyTuple_FromArray` (`Objects/tupleobject.c:166-200`, via #140262).
- **PR #128718** (tail-call interpreter): the first headline of 7–15% was later corrected to 3–5%. Nelhage showed that most of the gain came from avoiding an LLVM 19 computed-goto regression: clang-19 is 1.09x slower than clang-18, and tail-calls with clang-19 are 1.03x faster than clang-18. This is the canonical lesson about pinning the compiler used as the baseline.

---

## Section 3: open PRs where a default-build slowdown is being called noise (check before they merge)

- **PR #148440** (gh-148259): make `list.remove` atomic under a custom `__eq__`.
  - Link: https://github.com/python/cpython/pull/148440 (draft, stale).
  - It affects the GIL build.
  - Size 100–1000: −4.2% to −1.2%; size 10000: −1.1%; the author says "well within the noise".
  - Settle with a pyperf micro-benchmark plus cachegrind on `list_remove`.
- **Issue #158239 / PR #158240:** a new `asyncio.gather()` regression since gh-157213, no numbers posted yet. It is relevant to the `async_tree*` benchmarks. Already tracked in PRIOR_ART B.

---

## Section 4: methodology notes gathered along the way

- **Inada Naoki's benchmark de-noising guide** (https://discuss.python.org/t/a-benchmark-de-noising-skill-for-ai-agent-to-optimize-cpython/109001, guide at https://github.com/methane/cpython-skills):
  - Whether `.pyc` files were cleaned before the PGO training run changed results by more than the size of real optimizations.
  - Unrelated changes that push a function across a boundary cause swings in single benchmarks.
  - Takeaway: pin the PGO training state in every A/B.
- **Thomas Wouters' public bench_runner results** (https://github.com/Yhg1s/python-benchmarking-public):
  - Machines are tc1/tc2 (i3-6100T) and a Pi5, built with PGO and `--with-lto=full`, covering 3.13, 3.14 and main.
  - This is the closest surviving public time series since the Faster CPython runner went away. Check it for steps around the merge dates of M1 (2026-07-21), M4 (2026-07-22), M5 (2026-09-10) and M3 (2026-05-01) before building anything.
  - Caveat: a 3.15 dip from July 2025 was an artifact of libmpdec missing on those hosts.
- **Meta's free-threading benchmark runner** is what most 2025–2026 PRs quote. It measures the FT build, so default-build effects are often never measured (see M4).
- **Asymmetric thresholds** (Shannon on M2): sub-1% speedups get claimed while sub-1% slowdowns get dismissed. Stabilizer-style layout randomization with a fixed decision rule (for example, a 95% CI on the geomean that excludes 0) removes that bias.
