# CPython under Stabilizer (code-layout randomisation)

Status (2026-09-27): **working.** A non-PGO `-O3` CPython 3.16a0 (upstream `main` 6af40a6)
builds with Stabilizer's `szc` driver and `-Rcode`, passes the requested tests, runs
pyperformance benchmark bodies, and demonstrably places every interpreter function
(including `_PyEval_EvalFrameDefault` with its computed gotos) at a fresh random address
and alignment in every process. Overhead is about **+19% geomean** against plain clang-21 -O3
(nbody +4% up to raytrace +32%) on richards/nbody/raytrace/float/go. This machine was far too loaded
(4–8 cachegrind jobs on 4 cores) to measure ~1% layout variance. The variance and neutral-A/B
results below (geomean CIs cover 0; per-benchmark "hits" don't replicate) mostly show that
nothing breaks. Redo them on a quiet machine or runner before relying on them. The two most important
limitations are that Stabilizer measures a transformed program (§7) and that the original
entry stubs are not randomised (§7, with a proposed fix).

Stabilizer: <https://github.com/matthiasgoergens/stabilizer>, local branch
`cpython-support` in `/home/user/stabilizer` (not pushed), 8 commits on top of `main` (7d6e642).

## 1. Build recipe

### Toolchain
* LLVM/Clang **21** (apt.llvm.org: `llvm-21 clang-21 llvm-21-dev libclang-21-dev`, plus
  `lld-21` optional). Here `/home/user/llvm21bin` holds `clang clang++ opt llc llvm-link
  llvm-ar llvm-as llvm-config ...` symlinks into `/usr/lib/llvm-21/bin`. `szc` calls the
  bare names `clang opt llc llvm-link`, so this directory must come first on `PATH`.
* A libstdc++ that clang can find. On this box clang picks GCC 14, which has no
  libstdc++ headers installed. Stabilizer therefore needs
  `CPLUS_INCLUDE_PATH=/usr/include/c++/13:/usr/include/x86_64-linux-gnu/c++/13
  LIBRARY_PATH=/usr/lib/gcc/x86_64-linux-gnu/13` (or install `libstdc++-14-dev`).

### Stabilizer
```sh
cd /home/user/stabilizer && git checkout cpython-support
export PATH=/home/user/llvm21bin:$PATH CPATH=/usr/lib/llvm-21/include \
       CPLUS_INCLUDE_PATH=/usr/include/c++/13:/usr/include/x86_64-linux-gnu/c++/13 \
       LIBRARY_PATH=/usr/lib/gcc/x86_64-linux-gnu/13
make release          # LLVMStabilizer.so (pass), libstabilizer.so (runtime), szc
make test             # all regression tests pass on cpython-support (incl. new ones)
```

### CPython (out of tree, source untouched)
CC wrapper `/home/user/build/bin/szc-cc`:
```sh
#!/bin/sh
PATH=/home/user/llvm21bin:$PATH; export PATH
exec /home/user/stabilizer/szc -Rcode "$@"
```
```sh
mkdir -p /home/user/build/stab && cd /home/user/build/stab
/home/user/cpython/configure CC=/home/user/build/bin/szc-cc \
    AR=/home/user/llvm21bin/llvm-ar ax_cv_c_float_words_bigendian=no
STABILIZER_CODE_MODE=retained STABILIZER_MAX_EPOCHS=1 STABILIZER_QUIET=1 nice make -j2
```
* `-c` compiles each `.c` file to LLVM bitcode at CPython's `-O3`. At each executable link, szc
  `llvm-link`s all bitcode (static archives are expanded whole), runs `opt default<O3>`
  plus the Stabilizer passes, `llc -relocation-model=pic --frame-pointer=all`, and
  links with `--emit-relocs` against `libstabilizer.so` (rpath set). `main` is renamed
  `stabilizer_main`, and the runtime's `main` calls it.
* `ax_cv_c_float_words_bigendian=no`: configure greps an object file for a marker string,
  which does not work on bitcode objects.
* Shared objects (`-shared`, i.e. the 77 extension modules) are **not instrumented**. szc
  builds them as ordinary whole-module native code. `math`, `_random`, `_struct`, `_json`,
  `_pickle`, `array` ... are therefore not randomised. The core interpreter (everything in
  `libpython3.16.a` + the 37 built-in modules) is. To randomise a hot extension module, list
  it under `*static*` in the **build directory's** `Modules/Setup.local` (not tried here).
* Build time: about 11 min at `-j2` on a loaded 4-core box (the final `python` link, a whole-program
  `opt`/`llc` of a 42 MB bitcode module, takes about 2 min and runs once per executable:
  `_freeze_module`, `_bootstrap_python`, `python`, `_testembed`).
* `ldd python` shows `libstabilizer.so`. `.text` grows from 5.0 MB (plain clang) to 8.2 MB.
  7,821 functions are randomised.

Control builds used below:
* `/home/user/build/clang21` (= `clang21-plain`, a symlink): the same source, `CC=clang` (21),
  default `-O3`, no Stabilizer. It is the "plain" arm.
* `/home/user/build/szc-norand`: szc **without** `-R` (the same whole-program
  llvm-link/opt/llc pipeline, no instrumentation). It separates the effect of
  "whole-program optimisation" from Stabilizer overhead.
* `/home/user/build/{stab-b,plain-b}`: A/B builds from `/home/user/build/src-ab-b`, a `git archive`
  copy of the same commit with one unused function (`_Py_ab_layout_padding`,
  `__attribute__((used, noinline))`) inserted in `Python/ceval.c` before `PyEval_EvalFrameEx`.

### Runtime modes (environment)
| Setting | Meaning / when to use |
|---|---|
| `STABILIZER_CODE_MODE=retained STABILIZER_MAX_EPOCHS=1` | **Recommended for CPython.** Every function is copied to a random place (and random 16-byte-granular alignment) before `main`. No background thread, no re-randomisation, `fork()` allowed. One layout sample per process. |
| `STABILIZER_CODE_MODE=retained` (default 8 epochs, 500 ms) | Re-randomise every `STABILIZER_INTERVAL_MS` from a maintenance thread, up to `STABILIZER_MAX_EPOCHS` / `STABILIZER_MAX_CODE_BYTES` (one CPython generation copies about 3.6 MB, so the 64 MB default gives about 17 epochs, and `STABILIZER_MAX_CODE_BYTES=1073741824` gives about 290). **`fork()` exits with 78** (so `os.fork`, fork-based multiprocessing and `test_os`/`test_subprocess` fail). `subprocess` is fine because it uses vfork. |
| legacy (unset, or `STABILIZER_CODE_MODE=legacy`) | Original Stabilizer: lazy relocation via `int3` traps + SIGALRM every 500 ms + frame-pointer stack walk. **Not usable for CPython** (see §3). |
| `STABILIZER_QUIET=1` | Suppress the retained-mode banner on stderr. The banner breaks tests that compare stderr. |
| `STABILIZER_CODE_OFFSET=0` | Disable the random start offset (new, see §2). |

Wrappers used for the measurements live in `/home/user/build/bin/` (`py-stab-e1`,
`py-stab-rr`, `py-stab-e1-nooff`, `py-stabb-e1`, `py-plain`, `py-plainb`, `py-norand`).
With `STAB_CPUTIME=1` they put `/home/user/build/bin/cputime/sitecustomize.py` on the path,
which makes pyperf time the main thread's CPU time (`time.perf_counter = time.thread_time`).

## 2. Stabilizer patches (branch `cpython-support`, `git log main..cpython-support`)

| Commit | What / why |
|---|---|
| `2ef7a13` szc: act as a drop-in CC for autoconf/make builds | Passes unknown GCC/Clang flags through (paired-value options, `-Wl,`, `-M*`, `-x`, `-pthread`, `-m*`). Preprocess/`-S`/queries/assembly go straight to clang. Compile-only emits bitcode `.o`. Archives are expanded at link. `-shared` outputs are not instrumented. The default output name matches the compiler default. (From the previous attempt. Reviewed and kept.) |
| `ecb2118` pass: keep computed gotos inside the relocated copy | `opcode_targets` holds absolute label addresses of the *original* body, so without this a relocated `_PyEval_EvalFrameDefault` jumps back into the original, never-randomised code at its first `DISPATCH()`. Each `indirectbr` target is rebased by `(PC-relative address of an anchor label in the executing copy) − (the anchor's original address loaded from data)`. Cost: one load + sub + add per dispatch. Adds `tests/ComputedGoto`. (Previous attempt. Reviewed, and I removed build outputs it had committed.) |
| `e527feb` pass: drop assignment-tracking markers when lowering memory intrinsics | `-g -O3` attaches `!DIAssignID` to `llvm.mem*`. After lowering them to calls the verifier rejects the module. (Previous attempt. Kept.) |
| `7c93fe3` pass: expand arithmetic constant expressions on globals before tabling | Relocation-table slots cannot hold `&a - &b` (perf trampoline size) or `0 - ptrtoint @g`, and llc failed with "Cannot represent a difference across sections". (Previous attempt. Kept.) |
| `7bef57b` pass: give the renamed main default visibility | CPython builds with `-fvisibility=hidden`, so `stabilizer_main` was not visible to `libstabilizer.so`. (Previous attempt. Kept.) |
| `18af5d8` szc: link libstdc++ only when the frontend can resolve it | The last blocker of the previous attempt (`ld: cannot find -lstdc++` while linking `_bootstrap_python`): clang selected GCC 14, which has no `libstdc++.so` dev link. `libstabilizer.so` already depends on libstdc++. |
| `e3844e2` runtime: start each code copy at a random 16-byte offset | **Randomisation-quality fix.** The code heap is a bump allocator with a 16-byte header, so every copy started at 16 mod 32 and big functions at one of about 12 offsets mod 4096 (observed for `_PyEval_EvalFrameDefault`: 0x10–0x170). Cache-line and page alignment of the interpreter loop were therefore hardly sampled. Each copy now requests 64 bytes of padding and starts at a uniformly random multiple of 16 within its size-class slack (≤ 4 KiB). Observed offsets mod 4096 for the eval loop now span 0x130–0xdd0. `STABILIZER_CODE_OFFSET=0` restores the old placement. Adds `tests/StartOffset`. |
| `9eebda5` retained: allow fork without a maintenance thread; `STABILIZER_QUIET` | With `MAX_EPOCHS=1` no owner thread exists, so fork is safe and CPython's fork users work. Updates `tests/RetainedAdmission`. |

`make test` passes on the branch (ComputedGoto, StartOffset, ThreadLocal, RetainedAdmission,
RetainedCode, RelocationTypes, EntryPublication, HelloWorld, CodeLayout, libquantum, bzip2, ...).

Points reviewed and kept as they are:
* TLS: `_Py_tss_tstate` and friends are `_Thread_local`. The pass leaves TLS globals out of the
  relocation tables, and `llvm.threadlocal.address` goes through small fixed helpers, so
  CPython's TLS works in relocated copies (local-exec `TPOFF32` offsets are preserved).
* Threads: CPython startup and the pyperf worker are single-threaded (checked: one task in
  `/proc/self/task`). The regrtest main process with `-j` runs one thread per worker.

## 3. Why the legacy (original) mode does not work for CPython

1. **vfork + traps.** Legacy mode arms `int3` traps on function entries, lazily at start and
   again after every 500 ms timer. `_posixsubprocess` runs `child_exec` in a vfork child with
   all signals blocked. The first trapped function it calls gets `SIGTRAP` delivered while
   blocked, and the kernel kills the child (regrtest `-j2` workers die with exit −5). Even
   unblocked, the handler would mutate the parent's heap from the vfork child.
2. regrtest registers `faulthandler` for `SIGALRM` (with `chain=True`), so every
   re-randomisation tick dumps a traceback. Any test using `signal.alarm`/`setitimer`
   conflicts with the runtime's timer.
3. Threads: the legacy sweep walks only the interrupted thread's stack. It is unsafe once
   CPython has threads (regrtest `-j`, threading tests, faulthandler watchdog).

Retained mode has none of these problems: it installs no signals, prepares copies before `main`,
and never frees old copies. It is the mode to use.

## 4. Validation

1. `./python -c 'print(1)'`: OK in every mode.
   `STABILIZER_CODE_MODE=retained STABILIZER_QUIET=1 ./python -m test test_int test_dict test_grammar test_generators -j2`:
   **all pass** (328 tests). Also tried: with `MAX_EPOCHS=1`, `test_subprocess test_float test_os` (+ the four) → 8
   OK, 1 skipped (1,311 tests, including `test_os`'s fork tests). With re-randomisation (`MAX_EPOCHS=8`)
   `test_os`, `test_posix` and `test_subprocess` exit 78 (fork rejected, as designed). The other tests pass.
   Legacy mode: the sequential run passes, `-j2` fails (§3).
2. `bm_richards run_benchmark.py --worker --loops 20 --warmups 1 --values 5 -p 1` works in
   retained and legacy modes (62.8 / 64.3 ms vs 55.5 ms plain, wall-clock time, loaded machine).
3. Randomisation happens (`perf-notes/stabilizer/layout_probe.py`, using
   `stabilizer_code_location()` / `stabilizer_completed_epochs()` via ctypes). Every process
   and every epoch puts `PyLong_FromLong`, `_PyEval_EvalFrameDefault`,
   `PyObject_GetAttr` and `PyDict_GetItem` at different addresses, at distances of hundreds of MB
   from the original text. After the offset patch, 6 processes × 2 epochs gave eval-loop
   offsets mod 4096 of `0x280 0x130 0x940 0x5a0 0x8f0 0x820 0xdd0 0x1d0 0x300 0xc50 0x840 0xc70`.
   The eval loop keeps running in its copy across computed gotos (`tests/ComputedGoto`
   checks this with return addresses). In CPython, a gdb attach to a busy Python loop showed
   the PC in an anonymous `rwxp` mapping, not in the executable's text. (It also shows that gdb
   and perf cannot symbolise or unwind randomised code.)
4. A broader run (retained, `MAX_EPOCHS=1`, `-j2`): test_sys test_threading test_signal
   test_faulthandler test_exceptions test_descr test_re test_json test_pickle test_math
   test_itertools test_collections test_str test_list test_set test_gc test_weakref
   test_traceback test_ctypes test_capi gave 19/20 files OK (6,268 tests). The only failure is
   `test_sys.TestRemoteExec.test_remote_exec_deleted_static_executable`
   ("Can't send commands ... different Python version"). PEP 768 remote debugging locates
   `_PyRuntime` through the process's mappings, and the extra `libstabilizer.so`/anonymous code
   mappings mislead it once the executable is deleted. The plain build passes.

## 5. Overhead and variance

Setup: the five benchmarks, fixed loop counts (`perf-notes/stabilizer/loops.json`, about
0.3 s/value), 1 warmup + 5 values per process, one process per (benchmark, arm) per block,
arms in random order within each block (`perf-notes/tools/blockbench.py`), 10 blocks (exp1/exp2) or 20 blocks (exp3).
Response per process = median of its 5 values. Analysis with `blockbench.py analyze`
(paired over blocks) and `perf-notes/stabilizer/stabstats.py` (per-process spread,
Shapiro-Francia normality). Raw data: `/home/user/res/stab/exp{1,2cpu,3ab,4ab}.jsonl`.

**Noise caveat.** The whole run shared 4 cores with 4–8 valgrind jobs plus my own builds.
Wall-clock time (exp1) had a within-process CV of about 9%. Timing thread CPU time (exp2,
`STAB_CPUTIME=1`) removes scheduler waits but not cache interference: within-process CV
was still 3–5% and between-process CV 2.4–5.4% even for the plain build. That is several times
larger than the layout effects we want to see (~1%). These numbers bound overhead reasonably
well but say little about layout variance.

### Overhead (exp2, CPU time; relative to plain clang-21 -O3, 95% block-bootstrap CI)
| bench | stabE1 (one layout/process) | stabE1, no offset | stabRR (re-randomise 500 ms) |
|---|---|---|---|
| richards | +18.5% [+16.5, +20.8] | +15.5% | +18.7% |
| nbody | +3.8% [−1.0, +8.9] | +1.3% | +1.9% |
| float | +24.2% [+20.2, +29.1] | +20.3% | +23.9% |
| go | +23.2% [+18.8, +27.4] | +23.2% | +25.5% |
| raytrace | +27.3% [+22.6, +32.2] | +28.4% | +34.2% |
| **geomean** | **+19.1% [+17.0, +21.4]** | +17.4% | +20.3% |

Wall-clock time (exp1) gave the same picture: +20.4% geomean (E1) and +28.5% (RR). With wall
time the RR maintenance thread competes for a CPU and every new generation starts
with cold i-cache/iTLB.

**Decomposition (exp3, 20 blocks, CPU time):** szc *without* randomisation (`szc-norand`,
whole-program `opt -O3`) is **3.7% faster** than plain clang (geomean, CI [−4.4, −2.8]; nbody
−10%, go +3.5%). Stabilizer instrumentation on top of that costs **+23.6%** (CI [+22.4,
+24.8]) against `szc-norand`, and **+19.1%** (CI [+18.0, +20.1]) against plain clang.

Where the overhead comes from (by construction of the pass, not measured one by one):
every direct call becomes a load from a per-function relocation table + indirect call +
the original entry's `jmp *slot` to the current copy. Every global access gains a table
load. `switch` is lowered to compare chains (no jump tables, which would point into the
original). `llvm.mem*` intrinsics become libc calls (no inline expansion of small
memcpy/memset). int↔float conversions become calls to un-randomised helpers. Each
computed goto gets a rebase. Frame pointers are forced (CPython already has them).
nbody is float-arithmetic heavy with few calls, so it is barely affected. raytrace/go/float are
call-heavy.

### Run-to-run variance across processes (exp2, CPU time, CV of per-process medians, n=10)
| bench | plain | stabE1 | stabE1 no offset | stabRR |
|---|---|---|---|---|
| richards | 3.6% | 4.3% | 3.8% | 5.8% |
| nbody | 4.6% | 6.1% | 2.5% | 2.4% |
| raytrace | 4.0% | 3.3% | 3.5% | 4.4% |
| float | 2.4% | 6.3% | 4.3% | 2.3% |
| go | 5.4% | 3.1% | 3.1% | 3.5% |

On this machine, between-process variance is dominated by interference for every arm. Layout
variance does not stand out (expected: we measured a ~1% spread between PGO builds of the
same commit). **Gaussianity:** pooling z-scores over benchmarks (n=50 per arm), the
Shapiro-Francia test gives p = 0.03 (plain), 0.01 (stabE1), 0.01 (E1 no offset) and 0.11
(stabRR). All are right-skewed (+0.4 to +0.8), as you expect from interference, which only
ever adds time. The per-cell tests (n=10) have little power. Conclusion: no evidence either
way about layout-induced normality here. Re-randomisation (RR) looked the most Gaussian,
as the Stabilizer paper predicts, but the evidence is weak.

## 6. A/B sanity test (neutral change)

B = A + one unused function (`_Py_ab_layout_padding`, about 100 bytes) in `Python/ceval.c`.
In the plain build it moves `_PyEval_EvalFrameDefault` by 0x90 bytes (from 64-byte aligned to
16 mod 64) and shifts everything after it. Under Stabilizer the functions themselves are
re-placed at run time, but the original entry stubs and the data/relocation tables shift.
Arms interleaved in blocks, CPU time, one process per cell (exp3: 5 benchmarks × 20 blocks;
exp4: nbody+go × 30 more blocks). Per-benchmark effect of B vs A, 95% CI, sign-flip p:

| bench | plain B/A exp3 | plain B/A exp4 | Stabilizer B/A exp3 | Stabilizer B/A exp4 |
|---|---|---|---|---|
| richards | −1.9% [−4.1, +0.4] | | −1.8% [−4.2, +0.6] | |
| float | −0.1% [−2.2, +1.8] | | +1.4% [−1.4, +4.2] | |
| raytrace | +0.3% [−1.0, +1.7] | | −0.1% [−2.2, +1.9] | |
| nbody | **+2.2% [+0.6, +3.8] p=.014** | +0.4% [−0.9, +1.6] | **+2.2% [+0.2, +4.2] p=.041** | +0.1% [−1.2, +1.3] |
| go | **+2.8% [+0.8, +5.1] p=.020** | +0.3% [−1.4, +1.8] | +1.4% [−0.1, +2.7] | **−1.9% [−3.3, −0.6] p=.010** |
| geomean | +0.66% [−0.08, +1.42] | +0.36% [−0.9, +1.4] | +0.61% [−0.32, +1.63] | −0.93% [−1.9, +0.1] |

Reading: **inconclusive on this machine.** Every geomean CI covers 0, as it should for a neutral
change. But "significant" single-benchmark effects appear and then fail to replicate, or even flip
sign (Stabilizer go: +1.4% then −1.9%), for both the plain and the Stabilizer builds. The
interference here is bursty and not exchangeable between blocks, so the per-benchmark
sign-flip p-values are anti-conservative. The noise floor here is about ±2% per benchmark and
±1% on the geomean, which is larger than the effect Stabilizer is meant to remove. Neither
build shows a replicable layout effect from the unused function. Repeat on a quiet
machine: 30+ blocks, several A/A pairs, and check the false-positive rate of the
decision rule before using it on real patches.

## 7. Caveats

* **Stabilizer measures a transformed program.** Beyond layout it changes code generation:
  no jump tables, out-of-line `memcpy`/`memset`, calls for int↔float conversions,
  indirect calls/table loads for every call and global. A patch whose benefit comes from exactly
  these things (e.g. making a `switch` denser, removing a small `memcpy`, avoiding a call) can
  show a different effect under Stabilizer than in a release build. Also, szc's link step is
  whole-program `opt -O3` (LTO-like), unlike a normal non-LTO build. Use Stabilizer to decide
  *whether* a change is real beyond layout noise. Use several independent normal PGO+LTO builds
  to estimate *how big* it is in production.
* **Only function placement is randomised**, now including alignment mod 16…4096. The basic-block
  layout inside a function is the compiler's and is identical across processes. For CPython,
  where `_PyEval_EvalFrameDefault` dominates, a change that perturbs code *inside* the eval
  loop still changes its internal layout deterministically. Stabilizer cannot turn that into
  noise. It only removes the placement/alignment component and the accidental shifts of all
  *other* functions. Heap and stack randomisation (`-Rheap/-Rstack`) are not supported in retained
  mode, and CPython's obmalloc/mimalloc arenas would bypass `-Rheap` anyway.
* **Entry stubs stay put.** Callers reach a function through its *original* entry (table load,
  indirect call, `jmp *slot` in the original text), and only then jump to the random copy. The
  8-byte stubs of all 7,821 functions keep their link-time addresses. Their cache-line
  sharing and indirect-branch-predictor aliasing is fixed per build, and a patch that shifts
  the original text shifts them all. This is a residual, non-randomised layout factor. Fix
  (follow-up, pass + runtime): give *call-only* uses their own relocation-table slots, and
  have the runtime write the callee's current copy into those slots at each publication.
  Address-taken uses (e.g. CPython's `tp_dealloc == subtype_dealloc` comparisons) must keep
  the original address. Retained copies are never freed, so a store racing with another
  thread is safe. Besides removing the fixed stubs, this also removes one indirect jump
  per call, likely a good part of the overhead.
* Extension modules built as shared objects are not randomised (see §1). The data
  layout (globals, `.rodata`, relocation tables) is not randomised. Adding a function adds
  a relocation table and shifts `.data`.
* Re-randomisation mode rejects `fork()` (exit 78), is bounded by the byte budget (about 290
  generations at the 1 GiB maximum, so about 2.4 min at 500 ms), and each generation takes about 0.2 s
  on the maintenance thread (wall-clock on this loaded box), plus cold caches afterwards. Once exhausted, sampling
  silently stops. Check `stabilizer_retained_exhausted()` if you rely on it.
* PGO under szc was not attempted (`-fprofile-instr-generate/use` would have to flow through
  szc's bitcode pipeline, and Stabilizer's post-O3 transformations change the profile-shaped code).
* Legacy mode is unusable (§3). The `int3`/SIGALRM design conflicts with vfork and CPython's
  signal handling.

## 8. Recommended A/B protocol

1. Build **A** and **B** with the szc recipe (§1), same flags, same toolchain. Optionally
   build 2 or more independent Stabilizer builds per side. Not needed for layout (that is sampled
   at run time), but they catch build nondeterminism.
2. Run with `STABILIZER_CODE_MODE=retained STABILIZER_MAX_EPOCHS=1 STABILIZER_QUIET=1`: one
   fresh random layout per process, no maintenance thread, fork allowed. The unit of
   replication is the **process**. Don't pool many values from one process as if they were
   independent layout samples.
3. Use a randomised complete block design (`perf-notes/tools/blockbench.py run --arm
   A=... --arm B=... --rounds 1 --values 5`): each block runs every (benchmark, arm) once, in
   random order, with a fresh hash seed shared within the round. Collect **≥ 20–30 blocks**
   (processes per arm per benchmark). With layout randomised, the between-process spread includes the
   layout spread, so a paired bootstrap / t-test on per-block log-ratios accounts for layout
   instead of conditioning on one layout. (Whether that spread is Gaussian enough for a t-test
   could not be established here. The block bootstrap does not need it.)
4. Decide with a pre-registered rule: e.g. claim a win only if the 95% CI of the geomean
   log-ratio (block bootstrap) excludes 0. Then confirm magnitude on normal PGO+LTO builds.
5. Report the overhead-free effect only as "under Stabilizer". It is a test of whether the
   effect exists, not a production speedup number.
6. Quick self-check before trusting a campaign: an A/A run (two Stabilizer builds of the same
   source, or A vs A+unused-function) must give a CI that covers 0.

### GitHub Actions
Feasible, with these costs:
* Install LLVM 21 from apt.llvm.org on `ubuntu-24.04`
  (`wget https://apt.llvm.org/llvm.sh && sudo ./llvm.sh 21 && sudo apt-get install -y
  llvm-21-dev libclang-21-dev`), clone the stabilizer branch, `make release`. Put
  `/usr/lib/llvm-21/bin` first on `PATH` (szc calls `clang`, `opt`, `llc`, `llvm-link`). Also install
  `libstdc++-<gcc>-dev` so clang++ finds headers (the runtime is C++).
* CPython szc build: about 10–15 min per side on a 4-vCPU runner (the whole-program `opt`/`llc` of
  `python` is single-threaded and runs 4 times, once per executable). Cache `libpython3.16.a` /
  bitcode objects keyed by the commit to save time.
* Runs: shared runners are noisy (like this box). Stabilizer does not fix that noise, it only
  keeps layout from biasing the comparison. Use many blocks and paired analysis, and
  prefer a dedicated/bare-metal runner for sub-1% claims. `blockbench.py` accepts wrapper scripts as
  builds (this is how the Stabilizer arms ran here), so `perf-notes/ci/perf-block.yml` could add
  Stabilizer arms the same way (not tried).
* Firecracker/VMs without a PMU (like this one) cannot use `perf stat`. Only time is available.

## 9. Files
* `/home/user/stabilizer` branch `cpython-support`: pass/runtime/szc patches and tests.
* `/home/user/build/stab`: the Stabilizer CPython build (`./python`). `/home/user/build/clang21`:
  the plain control. `/home/user/build/szc-norand`: the szc control without randomisation.
  `/home/user/build/{stab-b,plain-b}` + `/home/user/build/src-ab-b`: the A/B builds.
* `/home/user/build/bin/`: `szc-cc`, `szc-cc-norand`, run wrappers, `cputime/sitecustomize.py`,
  `build-ab.sh`.
* `perf-notes/stabilizer/`: `loops.json`, `stabstats.py`, `layout_probe.py`.
* `/home/user/res/stab/`: raw JSONL results and logs.
