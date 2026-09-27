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

## Dispatch replication (user question: is 234 jumps for 232 targets good or bad?)
With computed gotos each handler ends in its own indirect `jmp`, which gives the predictor per-opcode
state. Merged tails (gh-129987, LLVM 19 tail-dup) share one slot and mispredict more. So "unmerged" is the
intended state. Counts (`perf-notes/tools/dispatch_sites.py`, reported by every CI build):
non-PGO GCC 13: 257 indirect jmps (tail duplication even exceeds the 231 TARGETs); PGO+LTO: 233 + 1 in .cold.
Open question worth measuring: how much replication still buys on modern ITTAGE-style predictors
(Rohou et al. 2015 found the switch-vs-threaded gap nearly gone). Planned exp3: GCC computed-goto vs
`--without-computed-gotos` (fully merged extreme) vs clang-21 CG vs clang-21 tail-call.

## FINDING: gh-129987 was the LLVM 19 tail-dup regression; clang 19 still merges every dispatch site
Compiling Python/ceval.c at -O3 (same flags as the build), counting indirect `jmp`s in _PyEval_EvalFrameDefault:
| compiler | dispatch jmps |
|---|---|
| gcc-12 / gcc-13 / gcc-14 | 257 |
| clang-18 | 269 |
| **clang-19 (19.1.7)** | **1** (fully merged) |
| clang-21 | 268 |
| clang-19 + empty `asm volatile` barrier per DISPATCH | 1 (barrier does not help: clang forms one shared indirectbr in IR and relies on backend tail duplication) |
| **clang-19 + `-mllvm -tail-dup-pred-size=1000`** | **269** (restored) |
So the upstream-able fix is a configure check: when CC is clang 19, add `-mllvm -tail-dup-pred-size=1000`
(or restrict it to ceval.o). Anyone still on clang 19 is affected (FreeBSD base LLVM 19? Apple clang based on LLVM 19? to verify).
TODO: measure the speed effect in CI (clang-19 PGO+LTO with/without the flag).

## TODO (user): -O2 vs -O3, per-flag ablation, evolutionary flag search (cf. Don Stewart's GA for GHC flags, Acovea)
1. CI arms: OPT=-O2 vs -O3 (PGO+LTO). 2. Ablate -O3-only passes on the Ir+cache/branch-sim proxy.
3. GA over flags using the cheap proxy; finalists re-checked with PGO in CI (flag effects interact with PGO).
Lower priority (user): test where tail-call gains come from (dispatch vs regalloc/layout).

## CI experiment queue (times SGT)
- exp1 (run 36291438990): base/aa/same/dealloc/vecfast. Builds took 43–58 min per job; blocks running since ~12:15.
- exp2 (run 36291441251): base/same/m2_inline/nofp/gc2x/gc4x. Same timing.
- exp3 (perf-ci/exp3-dispatch-compilers, pushed ~12:50): base (gcc13 CG), same, gcc_switch
  (--without-computed-gotos), c19 (clang-19: 1 dispatch site), c19fix (clang-19 + tail-dup-pred-size=1000,
  lld), c21 (clang-21 CG), c21tc (clang-21 tail-call). Each build logs its dispatch-site count.
- exp4 (perf-ci/exp4-optlevel): 2×2 factorial of -O2/-O3 × frame pointers on/off (+ same-binary control).
  It also replicates exp2's nofp arm on independent runners.

## Proxy fix (13:45 SGT): cachegrind runs must disable ASLR
The first full PGO baseline Ir run showed zero or negative deltas for small benchmarks (unpack_sequence,
pickle_list, telco). The cause is run-to-run startup noise: id()-based hashing and set/dict orders change
with ASLR. With `setarch -R` repeated runs agree to within 0.01% (richards: 11 Ir out of 989M; telco delta
291M, which is now sensible). Also dropped the warmup value (the 2K−K difference cancels warmup), which
halves the cost. `compare` skips benchmarks whose baseline delta is below 1e9 Ir. Two benchmarks dominate
the cost: bpe_tokeniser (86e9 Ir per loop) and pprint (48e9), 34% of the total.
All earlier local Ir numbers are discarded. The deterministic queue (tc-before, tc-after, pgo-base,
pgo-dealloc, pgo-vecfast) runs one build at a time with all 4 cores.

## CI sizing fix (14:20 SGT)
exp1/exp2 blocks are far slower than planned: `scale 4` multiplied every benchmark's loops, including
those where one loop already takes seconds (bpe_tokeniser ~14 s/loop by Ir estimate, pprint ~8 s, mdp ~4 s,
barnes_hut ~3 s). The jobs may run into the 350-minute job limit; the report step still prints the
partial results. From now on `perf-notes/ci/loops.json` is time-targeted (about 0.3 s per process,
estimated from per-loop Ir at ~3e9 Ir/s) and configs use scale 1. The four >3 s/loop benchmarks are
excluded from CI timing (they stay in the deterministic Ir proxy). Estimated ~30 min per block with 6 arms × 3 rounds.

## RESULTS (15:30 SGT): first CI timing experiments (GitHub runners, PGO+LTO, randomized interleaved blocks)
Analysis: paired log-ratios within blocks. CIs come from a cluster bootstrap over jobs, since each job
has its own builds and blocks within a job share binaries. Negative = faster.

### Controls: where the noise is
- Same binary run as two arms: exp1 −0.06% [−0.19, +0.04]; exp2 +0.07% [−0.05, +0.22]. Run-to-run noise is ≈ ±0.1%.
- **PGO builds are not reproducible**: `.text` hashes differ between two builds of the same commit, and
  also between jobs. The A/A arm (same commit, separate PGO build) varies per job from −1.18% to +0.44%.
  **Build-to-build variation dominates**, so what's needed is more independent builds (jobs), not more
  blocks per job, or deterministic PGO training.

### exp1: prototypes (6 builds × 2 blocks)
- dealloc fast path: **+0.38% slower** [+0.18, +0.57]; all 6 builds slower (+0.07…+0.76), even though the
  Ir proxy shows fewer instructions. **Rejected.** Instruction counts alone misled here.
- `_PyEval_Vector` exact-args fast path: −0.37% [−0.78, +0.04], 4/6 builds faster. Promising, not
  significant. Needs more independent builds.

### exp2: disputes (8 builds × 2 blocks)
| arm | all 88 | excl. async_tree* (72) | 95% CI (all) |
|---|---|---|---|
| GC gen-0 threshold ×2 | −2.24% | −0.28% | [−2.40, −2.12] |
| GC gen-0 threshold ×4 | −3.75% | −0.64% | [−4.07, −3.51] |
| --without-frame-pointers | −1.15% | −1.19% | [−1.53, −0.81] |
| revert gh-132336 noinline | −0.12% | −0.10% | [−0.37, +0.12] |
- GC: async_tree* −15…−34%, xml_etree_parse −14%; small losses on deltablue +1.9% and create_gc_cycles +1.6%.
  **This answers Pitrou's question on PEP 848: raising the existing thresholds captures most of the claimed
  3–5%, and the headline number is dominated by the 14 async_tree variants.** Memory impact not yet measured.
- Frame pointers: a broad −1.2% cost, consistent with PEP 831's own estimate (known trade-off).
- gh-132336 (noinline): the claimed 0.9% default-build slowdown is **not reproduced** (any effect ≤ ~0.4%).

### Deterministic Ir proxy (local, PGO+LTO, ASLR off): per-type method cache #150160
after vs before: **+0.75% geomean instructions** (66 benchmarks); richards +6.0%, richards_super +5.7%,
regex_v8 +2.3%, typing_runtime_protocols +2.2%, xml_etree +2.2%, argparse +2.0%, async_tree +0.3…2%.
Consistent with the PR's own "1% slower" measurement. Being re-run with warm .pyc caches (a K/2K
compile race was found) and with a PGO A/A for the Ir noise floor. Timing confirmation is still to do (exp5).

## Correction (user): pinning layout contradicts the Stabilizer lesson
`setarch -R` and `PYTHONHASHSEED=0` removed variance by conditioning on ONE layout sample, which can bias
comparisons (a patch that shifts allocations changes address- and hash-dependent work). Changed:
- irbench: ASLR on; R replicates (default 3) with random hash seeds, identical across builds
  (common random numbers). `compare` reports a paired geomean with a bootstrap-over-replicates CI and the
  noisiest benchmarks. nbody's Ir varies by ~1.3% (sd) across replicates, which was hidden before.
- blockbench (CI timing): a random hash seed per round, shared by all arms in the round (recorded per row).
  exp1–exp5 ran with seed 0 pinned: their results are conditional on one hash layout.
- The deterministic Ir result for #150160 (+0.75%) must be re-checked with the randomized replicates.

## exp6 (queued ~15:50 SGT): power run for the `_PyEval_Vector` fast path
User guidance: 1% was an arbitrary bar; a simple change with a smaller but robust win is worth having.
exp1 gave −0.37% [−0.78, +0.04] from 6 builds. Build-to-build noise is ~0.5% per job, so exp6 uses
20 independent jobs × 1 block (base, same-binary control, vecfast) with random per-round hash seeds.

## Stabilizer (16:10 SGT): CPython builds and runs with code randomisation (see STABILIZER.md)
- Non-PGO -O3 via szc/LLVM 21; all 7,821 core functions randomised. Mode: STABILIZER_CODE_MODE=retained
  STABILIZER_MAX_EPOCHS=1 (one fresh layout per process; legacy trap mode breaks vfork children and regrtest).
- Stabilizer patches: matthiasgoergens/stabilizer branch `claude/cpython-support` (8 commits), incl. a real
  sampling fix (copies previously always started at 16 mod 32, so the eval loop saw only ~12 page offsets).
- Overhead ~+19% vs plain clang-21 -O3 (transformed program: tabled calls/globals, no jump tables), so it
  tells whether an effect is real, not how big. Entry stubs still fixed; data/extension modules not randomised.
- Local variance test inconclusive: the VM is too loaded (2–5% process noise). Next: run it on CI runners.

## exp7 (launched ~16:25 SGT): Stabilizer validation on CI runners
Non-PGO builds of 8e4bbcab in two families: plain clang-21 -O3 {base, pad, vecfast} and Stabilizer
{base, pad, vecfast}. `pad` (perf/layout-pad) only adds an unused function to ceval.c, i.e. a pure layout
change. Prediction: plain pad-vs-base shows a *consistent* nonzero "effect" across jobs (deterministic
builds, so layout bias); under Stabilizer pad-vs-base ≈ 0; vecfast keeps its effect in both if it is real.
10 jobs × 1 block × 3 rounds.

## RESULTS (17:30 SGT): exp3–exp6 (GitHub runners, PGO+LTO unless noted; cluster-bootstrap CIs over jobs; negative = faster)
Controls (same binary): exp3 +0.18% [−0.05,+0.41], exp4 +0.11%, exp5 −0.04% [−0.14,+0.07], exp6 −0.04% [−0.12,+0.04].

### exp5 — per-type method cache (#150160, dispute M1), 10 build pairs: **CONFIRMED REGRESSION**
typecache vs parent: **+0.50% [+0.25, +0.75]**, 9/10 builds slower. Worst: scimark_lu +5.6%, xml_etree_process +3.4%,
xml_etree_generate +3.3%, async_generators +3.0%, async_tree_eager +2.6%, deepcopy_reduce +2.3% (richards +2.8%, n.s.).
Consistent with the Ir proxy (+0.75% instructions, richards +6%) and with the PR's own "~1% slower" that was
waved off as noise. Next: inline GIL-build fast path for `_PyTypeCache_Lookup` (keeps the per-type cache that
free-threading needs), then measure.

### exp6 — `_PyEval_Vector` exact-args fast path, 20 build pairs: **NO EFFECT**
vecfast vs base: +0.08% [−0.19, +0.36]. exp1's −0.37% (6 builds) was build-to-build noise. Dropped.
(Lesson recorded: 6 PGO builds are not enough; ~0.5% per-build noise.)

### exp3 — dispatch & compilers (6 jobs; clang arms use lld/ThinLTO, so compiler comparisons are whole-toolchain)
| arm | vs gcc13 base | dispatch jmps |
|---|---|---|
| gcc --without-computed-gotos | **+3.54%** [+3.13, +3.93] | 1 |
| clang-19 (merged dispatch) | **+7.66%** [+6.41, +8.69] | 1–9 |
| clang-19 + `-mllvm -tail-dup-pred-size=1000` | −1.56% [−1.82, −1.29] | ~357 |
| clang-21 computed goto | −1.96% [−2.33, −1.52] | ~360 |
| clang-21 tail-call interp | −1.60% [−1.96, −1.25] | (per-handler) |
- **The clang-19 fix is worth 8.6% [7.4, 9.6]** (c19fix vs c19, all 6 builds 6–10%): anyone building CPython with
  clang 19 (computed goto) loses ~9%. Candidate upstream fix: configure adds the flag for clang 19.
- **Replicated dispatch still matters on these CPUs**: fully merged (switch) costs 3.5% with GCC 13.
- **Tail-call vs computed goto, clang 21, x86-64 Linux: +0.36% [−0.38, +1.01]** — no measurable gain.
- clang-21 PGO+ThinLTO beats gcc-13 PGO+LTO by ~2%.

### exp4 — optimisation level × frame pointers (6 jobs)
-O2: **+5.44% slower** [+4.98, +5.96]; -O2 without FP +3.56%; -O3 without FP −1.32% [−1.75, −0.81] (replicates exp2's −1.15%).
So for CPython -O3 is clearly better than -O2 (unlike many programs). Next step toward flag tuning: ablate -O3-only passes.
Note: exp1–exp4 ran with PYTHONHASHSEED pinned to 0; exp5/exp6 sample the seed per round.

## Upstream-ready patches in progress (18:00 SGT)
- **clang-19 dispatch fix** (branch perf/clang19-taildup, patches/clang19-taildup.patch): configure.ac compile-time
  check (clang major 19, non-Apple) adds `-mllvm -tail-dup-pred-size=1000` to CFLAGS_CEVAL, plus LDFLAGS_NODIST
  under LTO. configure regenerated with autoconf 2.72 (clean diff). Verified: clang-19 ceval.o 1 → 269 dispatch
  jmps, clang-21 unaffected. NEWS entry added. exp9 validates the configure path end to end (PGO+LTO,
  default linker) on 6 jobs. Open question: Apple clang releases based on LLVM 19 (Apple numbering differs) are
  not detected.
- **typecache inline** (branch perf/typecache-inline): inline heap-type cache probe on the GIL build; type tests pass;
  exp8 (20 jobs) measures it against main.

## exp10a–d (queued ~18:10 SGT): leave-one-out ablation of GCC 13's -O3 over -O2
-O3 = -O2 + 14 flags (gcse-after-reload, ipa-cp-clone, loop-interchange, loop-unroll-and-jam, peel-loops,
predictive-commoning, split-loops, split-paths, tree-loop-distribution, tree-partial-pre,
unroll-completely-grow-size, unswitch-loops, version-loops-for-strides, vect-cost-model dynamic vs very-cheap)
+ larger inlining params (early-inlining-insns 6→14, inline-heuristics-hint-percent 200→600,
inline-min-speedup 30→15, max-inline-insns-auto 15→30, max-inline-insns-single 70→200).
Arms: -O3 with one flag disabled (or -O2's inlining params), vs -O3 base; 6 jobs each, PGO+LTO.
Hypothesis (user): the -O3 package is a mixed bag, so some passes may hurt; the 5.4% -O2 gap may be mostly inlining.
Also in flight: research on who ships clang-19 computed-goto builds (CLANG19_EXPOSURE.md).

## Clang-19 exposure (19:00 SGT, CLANG19_EXPOSURE.md): the fix would help real shippers
Affected (binary-verified): FreeBSD 14/15 packages python311–python314 (clang 19.1.7, computed goto, no PGO);
OpenBSD 7.8/7.9; OpenMandriva 6.0; MacPorts macOS 15 (Xcode 16.4); Homebrew Sequoia bottles (Xcode 26.3,
LLVM "partial fix" state, effect unmeasured). Formerly affected: python-build-standalone/uv (Jan–Feb 2025 releases),
conda-forge macOS 3.11–3.13 builds, python.org 3.14.4 macOS (partial). Correction: fully fixed only in LLVM 20.1.1
(20.1.0 = partial, #116072), Apple clang: 1700.0.13.x (Xcode 16.3/16.4) fully affected, 1700.3–1700.6
(Xcode 26.0–26.3) partial, clang 21 fixed. No public bug report found. The macOS probe (running) measures Apple clang directly.
TODO: extend the configure check to affected Apple clang build ranges (and 20.1.0 if the probe shows merging).

## exp11 (19:12 SGT): FreeBSD-like configuration — clang 19, --with-lto=thin, no PGO
c19 (main) vs c19patched (perf/clang19-taildup, configure adds the flag automatically), 8 jobs. Numbers for the
FreeBSD ports report. User decision: all issues/PRs are filed under the user's name; drafts go to
perf-notes/drafts/ for review first.

## RESULTS (19:40 SGT): exp7, exp8, exp9 + Apple clang evidence
- **exp8 typecache-inline vs main (20 builds): +0.17% [−0.06, +0.41]** → my inline fix does NOT recover #150160's
  0.5%; the out-of-line call is not the cause. Next: per-function Ir diff tc-before vs tc-after to locate the cost.
- **exp7 Stabilizer validation (10 jobs, non-PGO clang-21):** stab overhead +17.3% [16.7, 17.9].
  plain_pad −0.13% [−0.36,+0.09] (the unused function did not produce a consistent layout bias → weak test);
  stab_pad +0.20% [−0.30,+0.60]. vecfast: plain +0.24% [+0.06,+0.41] ("significant") vs Stabilizer −0.27%
  [−0.63,−0.00] vs PGO (exp6) +0.08% → vecfast is ~neutral; the plain-build "significance" is plausibly layout luck.
  Better validation needed: padding placed right before _PyEval_EvalFrameDefault / several padding sizes.
- **exp9 configure path (PGO+LTO, default linker): c19patched still merged (1–6 dispatch jmps)** — the configure check
  sets the flags (verified locally: CFLAGS_CEVAL and CONFIGURE_LDFLAGS_NODIST contain -mllvm -tail-dup-pred-size=1000)
  but with ThinLTO via the default linker the option apparently does not reach the LTO backend. exp3 (lld,
  -Wl,-mllvm,...) worked. Investigating locally (default ld vs lld, thin LTO). The patch is NOT ready.
- **Apple clang evidence (APPLE_CLANG_EVIDENCE.md):** from swiftlang branches: Xcode 16.0–16.2 not affected (LLVM 17);
  16.3–16.4 affected (LLVM 19 limit, no fix); 26.0–26.3 partial (#116072, per Apple ≈ fine on arm64; x86-64 unknown);
  26.4+ fixed. No published measurements of Apple-clang CPython. macOS probe still queued.
