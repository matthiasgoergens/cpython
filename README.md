# Measurements for python/cpython#158283 (Clang 19 / Apple clang dispatch merging)

This branch holds the raw data and the scripts behind the numbers in
[python/cpython#158283](https://github.com/python/cpython/issues/158283) and its PR. Nothing here is meant to be merged.

## Method

Each experiment builds several **arms** (CPython builds) on the same GitHub Actions runner and runs pyperformance
benchmarks through pyperf worker processes. The design is randomized blocks:

* Within each block, the benchmark order is shuffled.
* For each benchmark there are several rounds. In each round every arm runs once, in a random order. The arms share one
  random `PYTHONHASHSEED` for that round.
* Every job builds all arms from scratch, so separate jobs give separate builds. PGO builds are not
  deterministic, and the build-to-build variation is often the biggest source of noise.

The analysis (`tools/blockbench.py analyze`) works on per-benchmark mean paired log-ratios against a base arm.
It reports a geometric mean over benchmarks with a 95% bootstrap CI over blocks. It also runs a **cluster bootstrap
over jobs**, and that interval accounts for build-to-build variation; the numbers quoted in the issue use it.

Every experiment includes a same-binary A/A arm (`same`, `c19same`) as a control.

`ci/make_branch.sh NAME CONFIG.json WORKFLOW.yml` creates a throwaway `perf-ci/NAME` branch. Its tip commit has all arm
commits as parents, and its tree contains only the chosen workflow (the fork's normal CI is removed) plus the tools.
`ci/build_arms.py` builds the arms that `config.json` describes. It prints `HASH` and `DISPATCH` lines per arm: the
latter is the number of indirect jumps in `_PyEval_EvalFrameDefault` (`tools/dispatch_sites.py`).

## Experiments

| dir | what | workflow |
|---|---|---|
| `experiments/exp3` | Compiler comparison, x86-64 Linux, PGO+LTO, 6 jobs × 2 blocks. Arms: GCC 13 (`base`), GCC `--without-computed-gotos`, Clang 19, Clang 19 + flag (`c19fix`, flags passed by hand), Clang 21, Clang 21 tail-call. | `ci/perf-block.yml` |
| `experiments/exp9b` | The proposed configure patch vs main, Clang 19, `--enable-optimizations --with-lto`, 6 jobs | `ci/perf-block.yml` |
| `experiments/exp11b` | Same, Clang 19, `--with-lto=thin` without PGO (FreeBSD ports' configuration), 8 jobs | `ci/perf-block.yml` |
| `experiments/mac1b` | macOS 15 arm64, PGO+LTO, Xcode 16.4 and 26.3, each main vs patch, 5 jobs | `ci/perf-block-macos.yml` |
| `experiments/macos-probes` | Dispatch-jump counts for every Xcode on the macos-14/15/15-intel/26 runners (`ceval.o`, with the patch's configure decision), and in final `--with-lto` binaries | `ci/macos-dispatch-probe.yml`, `ci/macos-lto-probe.yml` |

Each experiment directory contains:
* `config.json`: the arms, with the commit SHAs;
* `all.jsonl`: one row per measured process, with fields `arm`, `bench`, `block`, `round`, `pos`, `hashseed`, `t`;
* `builds.txt`: the `HASH` and `DISPATCH` lines;
* `analysis_base_<arm>.txt`: the `blockbench.py analyze` output;
* `workflow_run.txt`: the Actions run, whose logs expire after 90 days.

To re-run an analysis:

    PERF_BENCHDIR=/nonexistent python3 tools/blockbench.py analyze experiments/exp11b/all.jsonl --base c19

`PERF_BENCHDIR` just stops `irbench.py` from looking for a pyperformance install, which `analyze` does not need.

The `-O3` object-file counts for GCC 12/13/14, Clang 18/19/21 in the issue were measured locally with
`tools/dispatch_sites.py` on `Python/ceval.o`.
