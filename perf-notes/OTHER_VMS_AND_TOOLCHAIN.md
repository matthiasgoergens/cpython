# Lessons from other VMs and from the toolchain (as of 2026-09-27)

Scope: CPython main (3.16a0) default GIL build, tier-1 interpreter only, with the JIT frozen. The target is ≥1% pyperformance geomean, or larger niche wins. This complements `PRIOR_ART.md`, whose items are not repeated here except where the toolchain angle is new.

Conventions:
- **[verified]** means I read the number at the linked source.
- **[unverified]** means secondary reporting, memory, or my own reasoning.
- **[est]** is my own size estimate.

---

## 0. Prioritized top-10 experiments

| # | Experiment | Hypothesis | Expected | Cost | How to measure |
|---|---|---|---|---|---|
| 1 | **GCC 16 `--with-tail-call-interp`, PGO+LTO, against computed goto on the same GCC 16** | GCC 16 now has x86-64 `preserve_none` ([GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html)), so the tail-call interpreter should reach clang-level results on the distro compiler. There are reports of a GCC PGO regression with tail calls ([LWN](https://lwn.net/Articles/1033373/)). | 0–3% on x86-64 [est]. Clang on x86-64 Linux gets about 2% against a good baseline [verified, LWN / [nelhage](https://blog.nelhage.com/post/cpython-tail-call/)]. | Low: configure flag and two builds. | Full pyperformance with `pyperf compare_to`. Also `perf stat` branch-misses and instructions. Check with `objdump` that handlers contain no callee-saved pushes. If PGO regresses, file a GCC bug with a reduced handler. |
| 2 | **Frame-pointer cost inside tail-call handlers** | With `-fno-omit-frame-pointer` (the PEP 831 default), every handler that needs a frame does `push rbp; mov rbp,rsp … pop rbp` on each bytecode. PEP 831 measured computed goto, where the eval loop is one frame. Handlers are only entered by tail calls, so omitting the FP in *handlers only* should lose nearly no unwind information: a helper called from a handler still records a return address into the handler [reasoning, unverified]. | 0.5–2% on the tail-call build [est] | Low: `__attribute__((optimize("omit-frame-pointer")))` or a per-TU flag on `generated_tail_call_*`; the attribute is fragile. | A 2×2 of tail-call/computed-goto × FP on/off. Count `push %rbp` in `_TAIL_CALL_*` symbols. Check `perf record --call-graph=fp` stacks stay intact. |
| 3 | **Dispatch-site audit and anti-merge barrier for GCC computed goto** | Compilers merge `goto *` sites. [gh-129987](https://github.com/python/cpython/issues/129987) found 47 sites instead of 306 and got about 1.03x with an empty `asm volatile` barrier; the issue is still open. LLVM 19's tail-dup regression was the same class of bug, costing 6–10% [verified, [llvm#106846](https://github.com/llvm/llvm-project/issues/106846), fixed by PR #114990 in LLVM 20]. | 0–3% [est]. Depends on how many sites GCC 15/16 PGO merges today. | Trivial | Count indirect `jmp *` in `_PyEval_EvalFrameDefault` for each compiler and build type. Add the barrier in `DISPATCH_GOTO` and rerun the suite. Add this as a CI regression check. |
| 4 | **PGO profile quality**: disjoint training set (reviving PR 130702), plus `-fprofile-partial-training`, plus a check of `-fprofile-update=atomic` | GCC optimizes code not seen in training for size, and the unit-test `PROFILE_TASK` is unrepresentative. An earlier experiment with a benchmark-like training set gave about 4% [PRIOR_ART, unverified]. `-fprofile-partial-training` keeps untrained code at `-O2`/`-O3` quality (MySQL reported +5% [unverified]). | 1–3% [est] | Low to medium: Makefile and configure changes. The training corpus needs to be disjoint from pyperformance to avoid a "gaming" objection. | pyperformance, plus a *held-out* macro set (Pyston macrobenchmarks) to show there is no overfitting. |
| 5 | **Polymorphic (2–4-way) and megamorphic fallbacks for `LOAD_ATTR` / `LOAD_ATTR_METHOD_*` / `CALL`** | Monomorphic sites that flip types deopt and respecialize, or stay in generic `PyObject_GetAttr`. richards (`self.fn` sites), go, raytrace and chaos are candidates. Self's PICs gave an 11% median speedup [verified abstract, [Hölzle et al. 1991](https://bibliography.selflanguage.org/pics.html)]. Brunthaler's purely interpretive ICs reached up to 1.71x [verified abstract, [ECOOP'10](https://publications.sba-research.org/publications/ecoop10.pdf)]. | Niche 3–15% on OO benchmarks. Geomean 0.3–1% [est]. | Medium. **Step 0 costs little:** use pystats to count `LOAD_ATTR` executions that are unspecialized, deopting, or backing off, per benchmark. | pystats first; build only if more than ~5% of attribute executions are non-monomorphic on several benchmarks. |
| 6 | **BOLT (and Propeller for clang) on top of PGO+LTO, current toolchains** | Code layout of a large interpreter and its helpers is still suboptimal after PGO. BOLT was measured at about 1% on pyperformance and 4% on the Pyston macrobenchmarks [verified, [PR 95908](https://github.com/python/cpython/pull/95908)]. It has bit-rotted with newer LLVM versions. | 1–3% [est] | Medium: repair `--enable-bolt` for LLVM 21/22. BOLT also works on GCC-built binaries. | pyperformance. Also report i-TLB and L1i misses (`perf stat -e frontend_retired.*`), since the gain is front-end. |
| 7 | **TLS access model for `_Py_tss_tstate` in shared libpython** (Debian, Ubuntu and Fedora ship `--enable-shared`) | `-fPIC` gives global-dynamic TLS, which means a call to `__tls_get_addr` (or TLSDESC) on every `_PyThreadState_GET()` that isn't passed explicitly. Initial-exec is one `mov %fs:off` ([TLS primer](https://chao-tic.github.io/blog/2018/12/25/tls)). libpython is always loaded at startup, except when `dlopen`ed by embedders, so initial-exec is plausible. | 0–2% on shared builds only [est, **no CPython measurement found**] | Low: `-ftls-model=initial-exec` on the TLS variable (`__attribute__((tls_model("initial-exec")))`) or `-mtls-dialect=gnu2`. | Count `__tls_get_addr` call sites with `objdump -d libpython*.so`. Then pyperformance comparing a shared build, the change, and a static build. |
| 8 | **`preserve_most` / `cold` on slow-path helpers called from handlers (clang)**, and outlining cold paths in generated handlers | Deegen and upb show the win comes from making the fast path free of spills. Slow-path calls (dealloc, error formatting, `_PyEval_*` helpers) force the handler to save the live state in callee-saved registers or on the stack. `preserve_most` on the callee removes that ([Haberman](https://blog.reverberate.org/2025/02/10/tail-call-updates.html)). | 0.5–2% [est, unverified] | Medium. Clang only (GCC has no `preserve_most` as far as I know [unverified]). Only valid for internal, non-ABI functions. | Count spills in the hot handlers (`LOAD_ATTR_*`, `BINARY_OP_*`, `CALL_PY_*`), then run pyperformance. |
| 9 | **Top-of-stack / accumulator caching in tier 1** (issue 131498 in `PRIOR_ART.md`) | This is the "accumulator" lesson from V8's Ignition. Register VMs remove about 46% of VM instructions at the cost of 26% more bytecode [verified, [Shi et al.](https://www.scss.tcd.ie/David.Gregg/papers/vee05-ShiGreggBeattyErtl.pdf)]. RegCPython reports 5–9% on 3.10 [verified, [ideas#485](https://github.com/faster-cpython/ideas/issues/485)]. A full register VM is too large a change. With the tail-call convention, a cached TOS is just one more argument register. | 0.5–2% [est] | High for tier 1 (code generator work). | pyperformance, plus instruction counts. |
| 10 | **ISA baseline and alignment sanity checks** (`-march=x86-64-v2/v3`, `-falign-functions=32/64`, `-falign-jumps`) run under Stabilizer | Mostly noise: the only data point I found is CachyOS v3+BOLT Python being 3% *slower* ([sunnyflunk](https://sunnyflunk.github.io/2023/01/15/x86-64-v3-Mixed-Bag-of-Performance.html)). Worth ruling out cheaply, because layout luck can masquerade as the wins above. | ±1% [est] | Trivial | Stabilizer-randomized runs. Treat any gain smaller than the layout variance as nothing. |

**Dropped or deprioritized:**
- A baseline JIT (frozen).
- Pointer compression (ABI).
- A full register VM (cost).
- Hand-written asm interpreter (cost and maintenance).
- `-fno-semantic-interposition` (already on).
- mimalloc for the GIL build (memory regressions, no speed evidence).

---

## Part A: Lessons from other VMs

### A1. Inline caches: polymorphic and megamorphic

**What other VMs do.**
- **V8:** each IC slot moves through uninitialized, monomorphic, polymorphic (up to 4 maps) and megamorphic states. The megamorphic state falls back to a global, fixed-size, lossy "stub cache" hash keyed by (map, name), with primary and secondary tables that are overwritten on collision ([mrale.ph](https://mrale.ph/blog/2015/01/11/whats-up-with-monomorphism.html)).
- **SpiderMonkey:** the CacheIR stub chains serve the Baseline *Interpreter* too. The Baseline Interpreter took Google Docs load from 901 ms to 676 ms, and Speedometer from 31 to 52 against the C++ interpreter [verified, [Mozilla Hacks](https://hacks.mozilla.org/2019/08/the-baseline-interpreter-a-faster-js-interpreter-in-firefox-70/)]. Much of that is ICs rather than dispatch.
- **SpiderMonkey's portable (C++) Baseline Interpreter:** interpreting CacheIR gives 1.26x on Octane. The same author found ICs *hurt* simple ops like `Add`, which ran 2x slower, and help only for property access and calls, hence "hybrid ICs" [verified, [cfallin](https://cfallin.org/blog/2023/10/11/spidermonkey-pbl/)]. This is the closest analog to CPython. The lesson is to spend IC effort on attribute access and calls, not on arithmetic.
- **CRuby:** since Ruby 3.2, the interpreter's ivar IC is keyed on a shape ID. YJIT's polymorphic caches went from 20 entries to 5 in 3.3 based on production data ([Shopify](https://shopify.engineering/ruby-yjit-is-production-ready)). Most real polymorphism is small.
- **Skybison (Instagram's Python):** it had an interpreter-level `LOAD_ATTR_POLYMORPHIC`, which loops over (layout id, offset) pairs ([bernsteinbear](https://bernsteinbear.com/blog/inline-caches-in-skybison/)). That shows a Python-shaped interpreter can do it. No numbers were published.

**CPython today.**
- CPython has monomorphic caches keyed on `tp_version_tag` plus keys and inline-values versions. These are effectively hidden classes.
- On a miss it deopts, and after `ADAPTIVE_COOLDOWN_VALUE` (52) it respecializes.
- A per-interpreter **type attribute cache** (`_PyType_Lookup`, keyed on version and name) already acts as a partial megamorphic stub cache for the *class-level* part.
- The unspecialized path still runs the whole `PyObject_GenericGetAttr` protocol: descriptor checks, then instance dict or inline values, then the class.

**Transfer.**
- **Polymorphic, 2-way:** the 9-code-unit `LOAD_ATTR` cache could hold two (type_version, index) pairs for the `INSTANCE_VALUE` and `METHOD_WITH_VALUES` kinds. It could be added as a separate `_POLY` specialization, entered on a second distinct type instead of deopting.
- **Megamorphic:** a global, direct-mapped table keyed by (type_version, interned name) that returns a *handler kind + slot index* (inline-value index, method pointer, or "has data descriptor"). The generic `LOAD_ATTR` would consult it before `PyObject_GetAttr`. This is V8's stub cache without codegen.
- **Invalidation** comes for free from version tags. The table must be cleared when the version-tag space is reset.
- **Risk:** the PBL experience shows that a generic, table-driven fast path can be slower than a well-written C slow path. Measure with pystats first (experiment 5).

### A2. Accumulator/register bytecode vs. stack bytecode

- **V8 Ignition** is a register machine with an implicit accumulator. The rationale is smaller bytecode and fewer dispatches ([Ignition](https://v8.dev/blog/ignition-interpreter)).
- **Academic evidence:** in Shi, Casey, Ertl and Gregg, the register VM executes about 46% fewer instructions, with bytecode 26% larger, and runs 32% faster with switch dispatch or 26.5% faster with threaded dispatch [verified]. That was on 2005-era hardware against an unoptimized stack VM.
- **For Python:**
  - RegCPython (3.10) reports a best case of 1.287x (fannkuch) and a worst of 0.977x. Anecdotally that is 5% on a production app and about 9% on ARM [verified, [ideas#485](https://github.com/faster-cpython/ideas/issues/485)].
  - Pyston's Kevin Modzelewski notes that register VMs need explicit "kill" flags or instructions for refcounting ([kevmod](https://blog.kevmod.com/2016/07/28/stack-vs-register-bytecodes-for-python/)).
  - CPython 3.11–3.14 already took much of the win via superinstructions, `LOAD_FAST_BORROW` and specialization, so the remaining gap is smaller than in 2022 [unverified].
- **Transfer:** caching the top of stack in a register gets the "accumulator" benefit without changing the bytecode. It pairs naturally with the tail-call convention (experiment 9).

### A3. Interpreters in portable assembler, and which compiler features that motivates

- **JSC's LLInt** is written in offlineasm, a Ruby-implemented macro assembler. The main motivation is precise frame layout for OSR and tier-up, not raw speed ([WebKit](https://docs.webkit.org/Deep%20Dive/JSC/JavaScriptCore.html)).
  - JSC Baseline is more than 2x faster than LLInt on JetStream 2, at 3.97 ns vs 1.71 ns per bytecode [verified, [Speculation in JSC](https://webkit.org/blog/10308/speculation-in-javascriptcore/)].
  - WebKit says roughly three quarters of LLInt time on hot code goes to dispatch branches [unverified; secondary summary of the same post].
- **LuaJIT's interpreter** is hand-written in DynASM. Its wins come from pinning interpreter state in fixed registers across all handlers, from NaN-boxing, and from careful fast/slow separation.
- **Deegen / LuaJIT Remake** generates the interpreter from C++ through LLVM IR, using the GHC calling convention (no callee-saved registers, guaranteed tail calls). Its interpreter is 28–31% faster than LuaJIT's hand-written one and 171–179% faster than PUC Lua [verified, [blog](https://sillycross.github.io/2022/11/22/2022-11-22/), [paper](https://arxiv.org/abs/2411.11469)]. The paper has no per-technique ablation.
  - Its stated compiler complaints: LLVM spills in fast paths even with `unlikely`, and no user-accessible no-callee-saved convention existed then (`preserve_none` now fills that gap).

**What this says about C compilers** (all of it is now partly addressable):
1. **Guaranteed tail calls:** `musttail` is in clang, GCC 15+ and MSVC (VS2026). GCC 15 had false-positive "cannot tail-call" errors (GCC bugs [119536](https://www.mail-archive.com/gcc-bugs@gcc.gnu.org/msg856675.html), [118430](https://www.mail-archive.com/gcc-bugs@gcc.gnu.org/msg845228.html)) and a PGO+musttail failure ([119618](https://www.mail-archive.com/gcc-bugs@gcc.gnu.org/msg857579.html)).
2. **No-callee-saved convention:** `preserve_none` is in clang 19+ (x86-64 and AArch64; RISC-V is in progress, [llvm#223920](https://github.com/llvm/llvm-project/pull/223920)) and in GCC 16 (x86-64). AArch64 in GCC is [bug 118328](https://www.mail-archive.com/gcc-bugs@gcc.gnu.org/msg849316.html); I could not confirm that it landed [unverified].
3. **`preserve_most` for slow-path callees:** clang only.
4. **Register pinning** across the dispatch loop with computed goto: not expressible in C. Tail calls with `preserve_none` are the practical substitute.
5. **Hot/cold splitting inside a handler:** only PGO-driven today.

**Transfer.** The MSVC result shows the effect of splitting the 12k-line function: 15–20% geomean on Windows [verified, [Ken Jin](https://fidget-spinner.github.io/posts/no-longer-sorry.html)], largely because splitting "resets compiler heuristics" so that small helpers get inlined again. It is the portable-assembler lesson reached via C. On Linux it is worth about 2% (clang), and GCC 16 is the open question (experiment 1).

### A4. Pointer compression and smaller headers

- **V8 pointer compression** shrinks the heap by up to 43% and renderer memory by up to 20%, with *CPU and GC improvements of 5–10%* [verified, [v8.dev](https://v8.dev/blog/pointer-compression)].
- **For CPython, this does not transfer.** `PyObject*` is baked into the C API and ABI, and a 4 GiB cage conflicts with extension allocations.
- **What would transfer:**
  - Shrinking per-object overhead (the 16-byte GC pre-header, and the managed-dict and weakref pre-header) for GC-tracked objects.
  - Shrinking inline caches (`PRIOR_ART.md` #8).
  - Both are memory and cache-density wins with unknown speed effect. They are not in the top 10.

### A5. Baseline / template compilers

These are frozen for CPython, but the numbers are useful for calibration:
- **Sparkplug:** 5–15% on real-world pages. JetStream +45% over Ignition alone, and +8% with TurboFan present. Speedometer +41% and +22% [verified, [v8.dev](https://v8.dev/blog/sparkplug)].
- **JSC Baseline:** more than 2x over LLInt.
- **SpiderMonkey:** Baseline JIT over Baseline Interpreter +33% on Speedometer.
- **YJIT:** about 2.2x on railsbench today [unverified, speed.yjit.org via search].
- **CPython's JIT:** PEP 836 reports +4.7% to +12.6% [PRIOR_ART].

The V8 and JSC gains come mostly from removing dispatch and from ICs that are already specialized. CPython's tier-1 already has specialization, which explains why its template JIT gains less.

### A6. Lazy feedback allocation and bytecode flushing

V8 allocates a function's feedback vector only after it has executed about 1 KB of bytecode. It also ages and flushes bytecode. Together these gave -18% memory on typical sites, with no desktop regression and a *speedup* on low-end devices from less GC [verified, [V8 Lite](https://v8.dev/blog/v8-lite)].

The CPython analog would be to allocate `co_code_adaptive` caches and specialization state lazily for code objects that never run (module bodies, rarely called functions). This is a memory and startup-time win, not a pyperformance one, so it is out of scope for the 1% target.

### A7. Other findings with measured payoffs

- **Embedded builtins and short builtin calls (V8):** calls to targets more than 4 GiB away mispredict on Intel and are "always mispredicted" on Apple M1 [verified, [v8.dev](https://v8.dev/blog/short-builtin-calls)]. This matters for CPython only if JIT code or extension modules are mapped far from libpython (the JIT is frozen; for extensions the effect is unmeasured).
- **Self PICs:** 11% median, and 27% in an experimental version with inlining [verified].
- **Deegen:** "type-based slow-path extraction" and a "tag register" (keeping the type-tag constant in a pinned register) are cheap for NaN-boxed values. The CPython analog, tagged ints (`PRIOR_ART.md` #10), is the big structural lever.

---

## Part B: Compiler and toolchain

### B1. Tail-call interpreter status (2026-09)

- **Clang ≥19:** `musttail` + `preserve_none` work. About 2% on x86-64 Linux and 5–7% on macOS arm64 against a good baseline [verified, LWN]. The original "10–15%" was an artifact of the LLVM 19 regression [verified, [Ken Jin's apology](https://fidget-spinner.github.io/posts/apology-tail-call.html)].
- **MSVC (VS2026, `[[msvc::musttail]]`):** 15–20%, and it is the default for the Windows 3.15 build [verified].
- **GCC 15:** `musttail` works (`__attribute__((musttail))` only after the [PR116545 patch](https://gcc.gnu.org/pipermail/gcc-patches/2025-March/677546.html)). There is no `preserve_none`, and there was reportedly no speedup (gh-132138, per PRIOR_ART).
- **GCC 16:** has x86-64 `preserve_none` [verified, GCC 16 changes]. There is a *reported* PGO regression for tail calls [unverified, LWN hearsay]. I found **no published pyperformance run**. Arch Linux has a packaging MR for it (inaccessible to me). Local `Python/ceval_macros.h:84` only feature-tests the attributes, so GCC 16 should build as-is.
- **Stack and `-fzero-call-used-regs`:** hardened distro flags interact badly with the tail-call interpreter ([gh-130961](https://github.com/python/cpython/issues/130961)). Check that Fedora/Ubuntu hardening flags don't silently undo the gain.
- **Frame pointers:** see experiment 2. PEP 831 says the computed-goto eval loop is only 0.1% larger with frame pointers, and eval-dominated workloads run 1–2% *faster*. The 1.5–2.3% cost comes from about 6,000 small helpers [verified, [PEP 831](https://peps.python.org/pep-0831/)]. **I found no measurement of frame-pointer cost for the tail-call build**, where every handler is a function. This is a real gap.

### B2. Computed goto and tail duplication

- **LLVM 19** added tail-dup size limits, which merged all dispatch jumps into one (332 indirect jumps down to 3). The result was 6–10% slower CPython. The workaround was `-mllvm -tail-dup-pred-size=5000` (also `-tail-dup-succ-size`). The fix is [PR #114990](https://github.com/llvm/llvm-project/issues/106846), in LLVM 20 [verified]. The damage is worst on older cores (Sandy Bridge) and negligible on Zen 4.
- **GCC:** [gh-129987](https://github.com/python/cpython/issues/129987) reports 47 vs 306 dispatch sites and about 1.03x from an `asm volatile("")` barrier. It is still open (PRs 132295/132530 are related). This is experiment 3.
- **Clang 22** ([gh-148284](https://github.com/python/cpython/issues/148284)) inlined large helpers into `_PyEval_EvalFrameDefault`. The frame grew enough to segfault `test_call` deep recursion under ThinLTO+PGO. The fix blocks inlining of large functions into ceval for clang 22. There are no performance numbers, but that frame growth is exactly the register-pressure and spill problem. Worth checking: does the fix cost or gain speed, and are GCC 16 frames similar?
- **AArch64:** the badc compiler project found 8,165 FP-relative spill accesses needing extra instructions in *its own* codegen of `_PyEval_EvalFrameDefault`, and got about 4.7% by fixing it ([badc#1070](https://github.com/kromych/badc/issues/1070)). That is **not** GCC or clang. Still, it is a cheap check: count out-of-range `ldur`/`sub` sequences in GCC/clang aarch64 builds.

### B3. Link, visibility and PIC

- **`-fno-semantic-interposition`:** CPython adds it with `--enable-optimizations` since 3.10. Fedora measured up to 27% (nbody 1.36x, raytrace 1.34x) for shared builds [verified, [Fedora](https://fedoraproject.org/wiki/Changes/PythonStaticSpeedup)]. That gap is already closed.
- **Shared vs. static residuals:** the remaining costs are the TLS model (experiment 7), PLT calls to libc (`-fno-plt` saves one indirect jump per libc call; small, and I found no CPython numbers), and GOT loads for extern data. The configure only forces `-ftls-model=global-dynamic` for one exotic platform (`configure.ac:3795`), so Linux gets the `-fPIC` default of global-dynamic. **I found no public measurement of `__tls_get_addr` cost in CPython.**
- **LTO partitioning:** GCC `-flto=auto` partitions by default. Ceval in one partition is fine, but cross-partition inlining of `Objects/*` helpers can suffer. `-flto-partition=one` is a cheap experiment [est ±0.5%].

### B4. Profile-driven and post-link optimization

- **PGO:** `configure.ac:2159-2174` uses `-fprofile-generate` plus `-fprofile-update=atomic` (added to fix a GCC ICE, with the gh-148535 i686 exception), and `-fprofile-use -fprofile-correction`.
  - Atomic updates slow training but shouldn't change the profile for single-threaded tasks. Multithreaded tests may skew counts, which is minor.
  - Add `-fprofile-partial-training` (GCC 10+) and a better `PROFILE_TASK` (experiment 4).
- **BOLT:** about 1% on pyperformance and 4% on Pyston macrobenchmarks at 3.12 [verified]. It requires `-fno-reorder-blocks-and-partition`, which it adds. It is experimental and bit-rots with LLVM versions ([gh-101525](https://github.com/python/cpython/issues/101525)).
- **Propeller** (clang-only, now upstream in LLVM) and **AutoFDO:** no CPython results found. AutoFDO was proposed in [ideas#108](https://github.com/faster-cpython/ideas/issues/108) with no numbers. For a PGO'd binary AutoFDO is usually a substitute rather than an addition, so skip it.
- **Hot/cold splitting:** GCC PGO enables `-freorder-blocks-and-partition` (`.text.unlikely`). That conflicts with BOLT, so pick one per build and measure both.

### B5. ISA level, alignment and allocator

- **x86-64-v2/v3:** there is no credible CPython win on record. `-march=native` helped only together with PGO, on some benchmarks (JSON, scimark: 5–10%), in a 2019 blog on 3.7 [verified, [atleastfornow](https://atleastfornow.net/posts/py3-enable-optimisations/)]. CachyOS v3 was -3% on pybench [verified]. The interpreter is branchy integer code, so there is little for AVX2 to do. BMI/`popcnt`/`lzcnt` could help the int and dict paths slightly.
- **`-falign-functions` / `-falign-jumps`:** mostly layout noise. Use Stabilizer.
- **mimalloc in the GIL build:** available only via `PYTHONMALLOC=mimalloc`. It uses more memory than pymalloc ([gh-135153](https://github.com/python/cpython/issues/135153)). I found no speed evidence for the GIL build, so it is not a priority.

### B6. Upstream compiler work that would help interpreters

1. **GCC:**
   - `preserve_none` on AArch64 (bug 118328).
   - A `preserve_most` equivalent.
   - Fixing the reported PGO+tail-call regression.
   - Keeping `musttail` free of false positives.
2. **Both GCC and LLVM:** a knob or attribute to *never merge indirect-branch sites* (`goto *`) and to exempt computed-goto functions from tail-dup size limits. Today this needs `asm volatile` hacks or `-mllvm` flags.
3. **LLVM:** frame-pointer handling for `preserve_none` functions that only tail-call. The compiler could omit the FP push/pop in such functions without losing unwind information. [idea, unverified]
4. **Better register allocation in huge switch functions:** Deegen's "spills in fast paths despite `unlikely`", and Clang 22's stack blow-up from inlining. Tail calls are the workaround, so upstream priority is low.

---

## Unverified or missing (please treat with care)

- The GCC 16 PGO tail-call regression: LWN hearsay; no bug number found.
- GCC AArch64 `preserve_none`: status unknown.
- Frame-pointer cost in tail-call handlers: never measured anywhere I found.
- TLS model cost in shared libpython: no data found.
- JSC's "three quarters in dispatch": a secondary summary.
- YJIT at 2.2x: a search snippet.
- All [est] sizes are my guesses and should be read as priors, not predictions.
