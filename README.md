# Raw results of the CPython performance investigation (Sept 2026)

Everything measured in the investigation whose notes live on branch
`claude/cpython-performance-optimization-vv6mlp` (directory `perf-notes/`; start with `perf-notes/HANDOFF.md`
and `perf-notes/LOG.md`). This branch is data only: nothing here is meant to be merged.

Files larger than 500 kB are gzipped. Timing rows (`all.jsonl`) have one row per measured process:
`arm, bench, block, round, pos, hashseed, t` (plus `tag`, from which the job is derived). Analyse with

    PERF_BENCHDIR=/nonexistent python3 perf-notes/tools/blockbench.py analyze ci/exp5/all.jsonl --base base

(`zcat` the file first if it is gzipped.) `builds.txt` holds each arm's `HASH` (binary/.text) and `DISPATCH`
(indirect jumps in `_PyEval_EvalFrameDefault`) lines. `jobN.log(.gz)` are the full GitHub Actions job logs.

## GitHub Actions experiments (`ci/`), x86-64 ubuntu runners unless noted

The arm definitions are in `expN.json` at the top level. The workflow run IDs are for
https://github.com/matthiasgoergens/cpython/actions/runs/<id>. Headline: geomean vs base, cluster-bootstrap 95% CI
over independent builds; negative = faster.

| dir | question | arms | headline | run |
|---|---|---|---|---|
| ci/exp1 | first CI run: split `_Py_Dealloc`, fast `_PyEval_Vector` | base, aa, same, dealloc, vecfast (PGO+LTO, 6 jobs) | dealloc +0.38% [+0.18, +0.57] (rejected); vecfast −0.37% n.s.; PGO A/A up to ~1% per build | 36291438990 |
| ci/exp2 | disputes: gh-132336 noinline revert, frame pointers, GC gen0 threshold | base, same, m2_inline, nofp, gc2x, gc4x | noinline revert −0.12% n.s.; no frame pointers −1.15%; GC ×2 −2.24% (−0.28% w/o async_tree), ×4 −3.75% | 36291441251 |
| ci/exp3 | compilers and dispatch merging | GCC 13 (base), GCC switch dispatch, Clang 19, Clang 19 + tail-dup flag, Clang 21, Clang 21 tail-call | Clang 19 +7.66% vs GCC; flag −8.57% vs Clang 19; Clang 21 −1.96% vs GCC; switch +3.54% | 36295318668 |
| ci/exp4 | -O2 vs -O3, frame pointers | base (-O3), o2, o3_nofp, o2_nofp | -O2 +5.44%; o3_nofp −1.32%; o2_nofp +3.56% | 36295330112 |
| ci/exp5 | per-type method cache (#150160) | daf09e17^ vs daf09e17, 10 jobs | +0.50% [+0.25, +0.75], 9/10 builds slower | 36302863178 |
| ci/exp6 | vecfast with 20 builds | base, same, vecfast | +0.08% [−0.19, +0.36] (null) | 36303903612 |
| ci/exp7 | Stabilizer on CI (non-PGO clang 21) | plain/stab × {base, pad, vec} | Stabilizer overhead +17.3%; see LOG | 36308497711 |
| ci/exp8 | inline fix for #150160 | base, same, tcinline, 20 jobs | +0.17% [−0.06, +0.41]: no recovery | 36309399239 |
| ci/exp9 | clang-19 configure patch, first version | c19 vs c19patched | patch ineffective under LTO (flag not forwarded); superseded by exp9b | 36310129296 |
| ci/exp9b | clang-19 configure patch, fixed, PGO+LTO | c19, c19same, c19patched | −8.35% [−9.51, −6.90] | 36317831268 |
| ci/exp10a–d | GCC -O3 leave-one-out ablation | 14 passes off one at a time, plus -O2 inlining params | only inlining matters (+5.21%); no_unswitch_loops −0.47% n.s. but binascii −12…−21% | 36312125843, 36312127825, 36312130261, 36312132351 |
| ci/exp11b | clang-19 patch, thin LTO, no PGO (FreeBSD config) | c19, c19same, c19patched, 8 jobs | −8.69% [−9.23, −8.29] | 36317834044 |
| ci/mac1b | Apple clang impact, macos-15 arm64, PGO+LTO | x164, x164p, x263, x263p, same | Xcode 16.4 patch −11.45%; Xcode 26.3 patch −1.36% | 36320447808 |
| ci/macprobe2 | dispatch count per Xcode, with the patch's configure decision | per-object | see LOG | 36318797549 |
| ci/ltoprobe | dispatch count in final LTO binaries on macOS | Xcode 16.4/26.3 × base/patched × arm64/x86_64 | 1/94/62 → 300/276 | 36320445459 |

(exp11, the unfixed patch on the FreeBSD config, and mac1, which failed on a build-script bug, were superseded and not collected.)

## Local experiments (top level)

* `stab/`: Stabilizer experiments on the dev box (exp1, exp2cpu: layout-randomisation modes; exp3ab/exp4ab:
  neutral A/B sanity test). Written up in `perf-notes/STABILIZER.md` §5–6.
* `*-ir.json`, `v2-*-ir.json`: cachegrind instruction-count proxy (`perf-notes/tools/irbench.py`), v2 = randomised
  replicates. Compare with `irbench.py compare A.json B.json`.
* `cg2-*.tsv`: per-function callgrind counts for richards at 20 and 40 iterations (the #150160 diagnosis in LOG).
* `pystats*`: pystats runs; `cc/`: ceval.o objects from different compilers (dispatch-jump counts).
* `aa*.jsonl`, `det*.json`, `calib-pgo.log`, `loops.json`: early calibration and noise studies.
