#!/usr/bin/env python3
"""Per-process distribution statistics for blockbench.py results.

For every (benchmark, arm): the response of one process is the median of its
timed values.  Reports the mean over processes, the between-process standard
deviation (CV), the max/min spread, the mean within-process CV (timer noise
inside one layout), and a Shapiro-Francia normality test (Royston's
approximation) per cell and pooled over benchmarks (z-scores within cells).

Usage: stabstats.py results.jsonl [...] [--base plain]
"""
import argparse
import json
import math
import statistics
from statistics import NormalDist

N01 = NormalDist()


def shapiro_francia(xs):
    """Return (W', p) for n >= 5; small p = evidence against normality."""
    n = len(xs)
    if n < 5 or statistics.pstdev(xs) == 0:
        return float('nan'), float('nan')
    ys = sorted(xs)
    m = [N01.inv_cdf((i - 0.375) / (n + 0.25)) for i in range(1, n + 1)]
    ybar, mbar = statistics.fmean(ys), statistics.fmean(m)
    num = sum((a - ybar) * (b - mbar) for a, b in zip(ys, m)) ** 2
    den = sum((a - ybar) ** 2 for a in ys) * sum((b - mbar) ** 2 for b in m)
    w = num / den
    u = math.log(n)
    v = math.log(u)
    mu = -1.2725 + 1.0521 * (v - u)
    sigma = 1.0308 - 0.26758 * (v + 2 / u)
    z = (math.log(1 - w) - mu) / sigma if w < 1 else -float('inf')
    return w, 1 - N01.cdf(z)


def moments(xs):
    m = statistics.fmean(xs)
    s = statistics.pstdev(xs)
    if s == 0:
        return 0.0, 0.0
    n = len(xs)
    skew = sum(((x - m) / s) ** 3 for x in xs) / n
    kurt = sum(((x - m) / s) ** 4 for x in xs) / n - 3
    return skew, kurt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--base', default=None, help='arm used as 1.00 for the relative-time column')
    args = ap.parse_args()
    rows = []
    for path in args.files:
        with open(path) as f:
            rows += [json.loads(l) for l in f if l.strip()]
    cells = {}
    for r in rows:
        if r['values']:
            cells.setdefault((r['bench'], r['arm']), []).append(r['values'])
    benches = sorted({b for b, _ in cells})
    arms = sorted({a for _, a in cells})
    print(f'{"bench":10s} {"arm":14s} {"n":>3s} {"mean ms":>9s} {"rel":>6s} {"CV%":>6s} '
          f'{"spread%":>8s} {"inCV%":>6s} {"skew":>6s} {"kurt":>6s} {"SF-W":>6s} {"p":>6s}')
    pooled = {a: [] for a in arms}
    for b in benches:
        base_mean = None
        if args.base and (b, args.base) in cells:
            base_mean = statistics.fmean(statistics.median(v) for v in cells[(b, args.base)])
        for a in arms:
            procs = cells.get((b, a))
            if not procs:
                continue
            meds = [statistics.median(v) for v in procs]
            mean = statistics.fmean(meds)
            sd = statistics.stdev(meds) if len(meds) > 1 else 0.0
            incv = statistics.fmean(statistics.stdev(v) / statistics.fmean(v) for v in procs if len(v) > 1) \
                if any(len(v) > 1 for v in procs) else float('nan')
            skew, kurt = moments(meds)
            w, p = shapiro_francia(meds)
            if sd > 0:
                pooled[a] += [(x - mean) / sd for x in meds]
            rel = mean / base_mean if base_mean else float('nan')
            print(f'{b:10s} {a:14s} {len(meds):3d} {1000 * mean:9.2f} {rel:6.3f} {100 * sd / mean:6.2f} '
                  f'{100 * (max(meds) - min(meds)) / mean:8.2f} {100 * incv:6.2f} {skew:6.2f} {kurt:6.2f} '
                  f'{w:6.3f} {p:6.3f}')
    print('\nPooled normality (z-scores within each benchmark, per arm):')
    for a in arms:
        xs = pooled[a]
        if len(xs) >= 5:
            w, p = shapiro_francia(xs)
            skew, kurt = moments(xs)
            print(f'  {a:14s} n={len(xs):3d}  W\'={w:.3f}  p={p:.3f}  skew={skew:+.2f}  excess kurtosis={kurt:+.2f}')


if __name__ == '__main__':
    main()
