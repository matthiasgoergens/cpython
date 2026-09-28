# Handoff: CPython performance investigation (state as of 2026-09-28, SGT)

Read this first, then `GOALS.md` (the user's goals and standing rules) and `LOG.md` (chronological results).
Everything below is on GitHub in `matthiasgoergens/cpython` unless marked "not pushed".

## Standing rules (from the user)

- Times in SGT (UTC+8).
- No AI attribution anywhere: no "Generated with/by Claude Code" footers and no Co-Authored-By/Claude-Session
  commit trailers. Anything filed upstream is filed in the user's name (author Matthias Goergens
  <matthias.goergens@gmail.com>).
- Measurement: randomized block designs with arms interleaved per benchmark, same-binary A/A controls, a
  bootstrap over independent builds (PGO build-to-build variation dominates), and layout randomization over
  pinning (the Stabilizer lesson). Micro-benchmarks only as proxies. About 1% on general benchmarks is a win;
  smaller is fine if the statistics are robust.
- Plain writing for anything public: no LLM-isms, no "Summary" heading.

## Where things are

| what | where |
|---|---|
| Notes, tools, CI scripts | branch `claude/cpython-performance-optimization-vv6mlp`, dir `perf-notes/` |
| Raw results of all experiments (earlier ones included) | branch `perf-results` (orphan; README indexes every experiment with arms, headline and run id) |
| Data and scripts for the filed Clang 19 issue | branch `clang19-dispatch-data` (orphan) |
| The upstream PR's branch | `pr/clang19-dispatch` (one commit on upstream main) |
| Patch prototypes | `perf/clang19-taildup`, `perf/dealloc-split`, `perf/eval-vector-fast`, `perf/layout-pad`, `perf/m2-no-noinline`, `perf/typecache-inline` |
| CI experiment branches | `perf-ci/*` (throwaway; each tip commit has the arm commits as parents; the tips rewritten on 28 Sep carry "[skip ci]" so the rewrite did not re-run them) |
| Stabilizer with CPython support | `matthiasgoergens/stabilizer`, branch `claude/cpython-support` |

Key documents in `perf-notes/`: `PRIOR_ART.md`, `DISPUTES.md` (with measured results at the top),
`OTHER_VMS_AND_TOOLCHAIN.md`, `STABILIZER.md`, `CLANG19_EXPOSURE.md`, `APPLE_CLANG_EVIDENCE.md`,
and `drafts/` (copies of the filed issue and PR text; GitHub is the source of truth).

## Filed upstream

- Issue python/cpython#158283: Clang 19 and Xcode 16.3–26.3 merge the computed-goto dispatch jumps (8–9% slower on
  Linux, 11% on macOS arm64 with Xcode 16.4, 1.4% with Xcode 26.3). Labels: build, performance, type-bug.
- PR python/cpython#158286: a configure check adds `-mllvm -tail-dup-pred-size=1000` for Clang 19 and Apple clang
  1700.x, and passes it to the LTO backend (`-Wl,-plugin-opt=` or `-Wl,-mllvm,`). As of 28 Sep: open, no reviews
  yet. The user's local session is removing the "Generated with Claude Code" footer from the PR text. The user asked
  to keep the disclosure sentence as it is.
- Not filed yet: downstream reports. FreeBSD ports (python311–314 use base clang 19.1.7) and MacPorts (Xcode 16.4
  on macOS 15) have strong cases; for Homebrew (Xcode 26.3, 1.4%) the case is weak. Exposure evidence is in
  `CLANG19_EXPOSURE.md`. The fix for them is the same flag, or wait for the CPython release that carries the PR.

The cloud session could not attach python/cpython (name clash with the fork), so it used a separate cloud session
per filing or edit.

## Results so far (details in LOG.md and the `perf-results` README)

- Clang 19 dispatch merging: the big finding, filed (above).
- -O2 vs -O3 (GCC 13, PGO+LTO): -O2 is 5.4% slower. The leave-one-out ablation (exp10) shows this is all -O3's
  inlining limits (+5.2% with -O2's). No individual loop or vectorizer pass matters on the geomean. Turning off
  loop unswitching speeds up binascii by 12–21% (base16, ascii85); this is unconfirmed.
- Frame pointers: turning them off saves 1.15–1.3%. This is PEP 831 territory: a dispute, not a fix.
- GC gen0 threshold ×2/×4: −2.2%/−3.8%, but mostly async_tree (−0.3%/−0.6% without it); memory not measured.
- gh-132336 noinline revert: −0.12% n.s.; the claimed 0.9% did not reproduce.
- #150160 (per-type method cache, gh-145685) regressed pyperformance by +0.50% [+0.25, +0.75] (Ir +0.74%, richards
  +6%). Cause: type attribute lookups went from an inlined global-cache probe (~11 instructions) to an out-of-line
  per-type lookup (~45 instructions). The inline-fix prototype (`perf/typecache-inline`) recovers about a third in
  Ir but nothing measurable with PGO (exp8).
- Rejected or null: `_Py_Dealloc` split (+0.38%, slower), `_PyEval_Vector` fast path (null over 20 builds).
- Stabilizer: works with CPython (about 17–19% overhead). Useful for layout-robust A/B, but the neutral A/B
  validation was weak; see STABILIZER.md §6.

## Open tasks, in rough priority

1. Watch PR #158286 and the issue for review comments and CI. This session could not subscribe (repo out of scope).
2. #150160 follow-ups the user approved: (a) move the `MCACHE_CACHEABLE_NAME` check to the miss/insert path
   (non-cacheable names are never inserted, so they cannot hit); (b) a front cache keyed by (tp_version_tag, name):
   per interpreter with the GIL, per thread or off in the free-threaded build (a shared one would bring back the
   seqlock and cache-line contention that #150160 removed). Measure with the Ir proxy on richards first
   (`cg2-*` method in LOG), then a CI run with 10–20 builds.
3. Confirm the binascii loop-unswitching effect (targeted run, more builds), find the loop, and decide between a
   source change and a per-file flag.
4. Inlining-limit tuning beyond -O3 defaults (the only flag area with any effect); maybe an evolutionary search
   (Don Stewart's prior art) restricted to inlining params.
5. Downstream reports (FreeBSD ports, MacPorts; Homebrew optional). Draft them only after the user confirms.
6. Lower: GC threshold memory cost; better Stabilizer validation (padding right before `_PyEval_EvalFrameDefault`,
   several sizes); the tail-call-gains hypothesis; the lru_cache/OrderedDict revival (the user's earlier work,
   faster-cpython/ideas#450; frame it as "amortized is already the status quo"; Raymond Hettinger was the blocker).

## How to run things

- CI experiments: `perf-notes/ci/make_branch.sh NAME CONFIG.json [WORKFLOW.yml]` pushes `perf-ci/NAME` with only
  our workflow (`perf-block.yml` for Linux, `perf-block-macos.yml` for macOS). The arm spec syntax is in
  `ci/build_arms.py`. Config examples are the `exp*.json` files on the `perf-results` branch. There is an
  account-wide limit of about 20 concurrent jobs, and 5 on macOS.
- Collecting results: job logs download without auth via
  `curl -sL https://api.github.com/repos/matthiasgoergens/cpython/actions/jobs/<job id>/logs`. Take the line after
  the last `=====BEGIN-RESULTS-B64=====` marker, strip the timestamp, base64-decode and gunzip. Grep the
  `HASH`/`DISPATCH` lines for builds.txt.
- Analysis: `PERF_BENCHDIR=/nonexistent python3 perf-notes/tools/blockbench.py analyze all.jsonl --base <arm>`.
- Ir proxy (local, cachegrind): `perf-notes/tools/irbench.py run --python P --loops loops.json --out X.json -j4`,
  then `irbench.py compare A.json B.json`. It needs a pyperformance venv (set `PERF_PYPERF_LIB`/`BENCHDIR` if not in
  the default places).
- Dispatch-jump count: `perf-notes/tools/dispatch_sites.py <binary or .o>`.

## Gotchas learned the hard way

- On macOS, `python` in the build dir is the `Python/` directory (case-insensitive FS); the binary is `python.exe`.
- Under LTO, clang ignores driver-level `-mllvm` at link time; pass it to the linker (`-Wl,-plugin-opt=` or
  `-Wl,-mllvm,`).
- Clang ≤ 18 rejects `-tail-dup-pred-size`.
- PGO builds are not reproducible: always use several independent builds per arm.
- `pkill -f`/`pgrep -f` patterns match the shell running them; kill by PID.
- pyperformance benchmark scripts `import pyperf`; strip that line when running one under callgrind directly.

## Not pushed (lost when the cloud container goes)

Local builds under `/home/user/build` (all can be rebuilt from the commits above), the pyperformance venv, and
LLVM/autoconf toolchains. `configure` was regenerated with autoconf 2.72 (upstream's version); `aclocal.m4` was left
unchanged.
