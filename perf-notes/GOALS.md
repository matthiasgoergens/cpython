# Performance work: goals (as given by the user)

Base: latest upstream `python/cpython` `main` (6af40a6, 2026-09-27).
Branch: `claude/cpython-performance-optimization-vv6mlp` on `matthiasgoergens/cpython`.

## Goals

1. **Make CPython faster**, working in the spirit of the old Faster CPython project.
2. **Measure the way the community measures**: pyperformance / pyperf, geometric
   mean across the suite, comparisons like the faster-cpython `bench_runner`
   setup (PGO+LTO builds, `pyperf compare_to`, significance testing).
3. **Consider the user's resurrected Stabilizer** (`matthiasgoergens/stabilizer`)
   for layout-randomized, statistically sound measurement, so that code-layout
   luck isn't mistaken for a real win or loss.
4. **Bar for success**:
   - About a **1% speedup on a general benchmark** (pyperformance geomean) is
     already a win, and such wins stack.
   - Wins that only show up in **niche areas must be correspondingly larger**.
5. **Use cheap proxies to guide the search.** A full, statistically
   meaningful, ablated pyperformance run is expensive. Find fast signals
   (instruction counts, cachegrind/callgrind, `perf stat`, a small benchmark
   subset, micro-benchmarks) that predict the full-suite result, and use the
   full suite only to confirm candidates.
6. **Ablate**: every claimed win is measured on its own as well as stacked
   with the others.
7. **Share ideas and do a prior-art search**, with special attention to ideas
   or prototypes that looked great but **stalled**, since these are candidates
   to revive.

## Working rules I set myself

- The user is in Singapore: report times in SGT (UTC+8).

- Keep a written log of candidates, proxy results and full-run results
  (`perf-notes/LOG.md`).
- Before believing a proxy, check it against real timing on at least a few
  pyperformance benchmarks.
- Don't break the test suite: run the relevant `test_*` modules for each change.

## Standing rules
- No AI attribution anywhere: no "Generated with/by Claude Code" footers in PRs, issues or comments, and no
  Co-Authored-By / Claude-Session trailers in commit messages (user instruction, 2026-09-28).
