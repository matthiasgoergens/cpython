#!/usr/bin/env python3
"""Compare the spread of per-block paired differences between blockbench runs.

    variance_compare.py BASE_ARM RUN1.jsonl [RUN2.jsonl ...]

For each run, benchmark and arm: the standard deviation (in %) of the per-block
log-difference arm - BASE_ARM, the block count, and the resulting 95% CI half-width
of the mean (1.96 * sd / sqrt(n)).  Same response as blockbench analyze: median of
a cell's values, mean of log over the rounds of a block.  Background noise that
does not bias the paired comparison should still show up here as a larger sd.
"""
import json
import math
import statistics
import sys

base = sys.argv[1]
print(f'{"run":28} {"bench":14} {"arm":16} {"n":>3} {"sd %":>6} {"ci95 ±%":>8}')
for path in sys.argv[2:]:
    cell = {}
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r['values']:
                cell.setdefault((r['block'], r['bench'], r['arm']), []).append(
                    math.log(statistics.median(r['values'])))
    mean = {k: statistics.fmean(v) for k, v in cell.items()}
    blocks = sorted({k[0] for k in mean})
    for bench in sorted({k[1] for k in mean}):
        for arm in sorted({k[2] for k in mean} - {base}):
            d = [mean[(b, bench, arm)] - mean[(b, bench, base)] for b in blocks
                 if (b, bench, arm) in mean and (b, bench, base) in mean]
            if len(d) < 3:
                continue
            sd = 100 * statistics.stdev(d)
            print(f'{path.rsplit("/", 1)[-1][:28]:28} {bench:14} {arm:16} {len(d):3} '
                  f'{sd:6.2f} {1.96 * sd / math.sqrt(len(d)):8.2f}')
