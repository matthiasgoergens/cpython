#!/usr/bin/env python3
"""Randomized complete block design for comparing CPython builds by wall-clock time.

Design
------
* A *block* is one pass over all (benchmark, build) cells, in a fresh random
  order, on one machine within a short time window.  Nuisance factors that
  drift slowly (thermal state, noisy neighbours, CPU frequency, the runner
  a CI job landed on) are shared by all cells in a block, so comparisons
  within a block cancel them.
* An *arm* is a treatment (e.g. ``base`` vs ``patch``).  An arm may have
  several *builds* (e.g. different code-layout seeds, or Stabilizer builds).
  The layout seed is a random factor: we average over it, not condition on it.
* Each cell is one fresh process running the pyperf worker with a fixed loop
  count (no calibration, so every arm does the same work): 1 warmup value
  plus V timed values.  The cell's response is the median of its values.

Analysis (``analyze``)
----------------------
For each block and benchmark we take log(time_arm / time_base) (averaging
over the builds of an arm within the block), so each block contributes one
paired difference per benchmark.  Per benchmark we report the mean over
blocks with a block-bootstrap CI and a sign-flip permutation p-value.  The
geometric mean over benchmarks is bootstrapped by resampling whole blocks,
which keeps the within-block correlation between benchmarks.

Usage
-----
  blockbench.py run --arm base=/path/python[,/path2/python] --arm patch=... \\
      --loops loops.json [--bench REGEX] --blocks 10 --out results.jsonl
  blockbench.py analyze results.jsonl --base base
"""
import argparse
import json
import math
import os
import random
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from irbench import discover, env_for, worker_cmd  # noqa: E402


def run_cell_json(python, script, extra, loops, values, tmpdir):
    out = os.path.join(tmpdir, 'cell.json')
    if os.path.exists(out):
        os.unlink(out)
    cmd = worker_cmd(python, script, extra, loops)
    i = cmd.index('--values')
    cmd[i + 1] = str(values)
    cmd += ['--output', out]
    p = subprocess.run(cmd, env=env_for(), capture_output=True, text=True,
                       cwd=os.path.dirname(script), timeout=900)
    if p.returncode:
        raise RuntimeError(p.stderr[-1500:])
    with open(out) as f:
        data = json.load(f)
    res = {}
    for bench in data['benchmarks']:
        name = bench.get('metadata', {}).get('name') or data['metadata'].get('name')
        vals = []
        for r in bench['runs']:
            vals += r.get('values', [])
        res[name] = vals
    return res


def cmd_run(args):
    arms = {}
    for spec in args.arm:
        name, paths = spec.split('=', 1)
        arms[name] = paths.split(',')
    with open(args.loops) as f:
        loops = json.load(f)
    benches = [b for b in discover() if b[0] in loops]
    if args.bench:
        pat = re.compile(args.bench)
        benches = [b for b in benches if pat.search(b[0])]
    rng = random.Random(args.seed)
    tmpdir = os.path.join(os.path.dirname(os.path.abspath(args.out)), '.blockbench_tmp')
    os.makedirs(tmpdir, exist_ok=True)
    cells = [(b, arm, build) for b in benches for arm, builds in arms.items() for build in builds]
    with open(args.out, 'a') as out:
        for block in range(args.blocks):
            order = cells[:]
            rng.shuffle(order)
            t_block = time.time()
            for pos, ((name, script, extra), arm, build) in enumerate(order):
                loops_n = max(1, int(round(loops[name] * args.scale)))
                try:
                    vals = run_cell_json(build, script, extra, loops_n, args.values, tmpdir)
                except Exception as e:
                    print(f'block {block} {name} {arm}: FAILED {str(e)[-300:]}', file=sys.stderr)
                    continue
                for sub, v in vals.items():
                    out.write(json.dumps({
                        'tag': args.tag, 'block': f'{args.tag}:{block}', 'bench': sub or name,
                        'arm': arm, 'build': build, 'pos': pos, 'loops': loops_n,
                        'values': v, 't': time.time()}) + '\n')
                out.flush()
            print(f'block {block} done in {time.time() - t_block:.0f}s', file=sys.stderr, flush=True)


def bootstrap_ci(xs, stat, n=2000, rng=None):
    rng = rng or random.Random(0)
    reps = sorted(stat([rng.choice(xs) for _ in xs]) for _ in range(n))
    return reps[int(0.025 * n)], reps[int(0.975 * n)]


def signflip_p(diffs, n=4000, rng=None):
    rng = rng or random.Random(1)
    obs = abs(statistics.fmean(diffs))
    hits = 0
    for _ in range(n):
        s = statistics.fmean(d if rng.random() < 0.5 else -d for d in diffs)
        hits += abs(s) >= obs - 1e-15
    return (hits + 1) / (n + 1)


def cmd_analyze(args):
    rows = []
    for path in args.files:
        with open(path) as f:
            rows += [json.loads(l) for l in f if l.strip()]
    # response per cell: median of values; average log over builds of an arm in a block
    cell = {}
    for r in rows:
        if not r['values']:
            continue
        key = (r['block'], r['bench'], r['arm'])
        cell.setdefault(key, []).append(math.log(statistics.median(r['values'])))
    mean_log = {k: statistics.fmean(v) for k, v in cell.items()}
    arms = sorted({k[2] for k in mean_log} - {args.base})
    benches = sorted({k[1] for k in mean_log})
    blocks = sorted({k[0] for k in mean_log})
    for arm in arms:
        print(f'\n=== {arm} vs {args.base}  (negative = faster)  blocks={len(blocks)}')
        per_block = {}  # block -> {bench: diff}
        summary = []
        for b in benches:
            diffs = []
            for blk in blocks:
                a, z = mean_log.get((blk, b, arm)), mean_log.get((blk, b, args.base))
                if a is not None and z is not None:
                    diffs.append(a - z)
                    per_block.setdefault(blk, {})[b] = a - z
            if len(diffs) < 2:
                continue
            m = statistics.fmean(diffs)
            lo, hi = bootstrap_ci(diffs, statistics.fmean)
            p = signflip_p(diffs)
            summary.append((b, m, lo, hi, p, len(diffs)))
        for b, m, lo, hi, p, n in sorted(summary, key=lambda s: s[1]):
            if not args.quiet:
                star = '*' if p < 0.05 and n >= 5 else ' '
                print(f'{b:34s} {100 * (math.exp(m) - 1):+7.2f}%  CI [{100 * (math.exp(lo) - 1):+6.2f}, '
                      f'{100 * (math.exp(hi) - 1):+6.2f}]  p={p:.3f} n={n} {star}')
        # geomean over benchmarks, bootstrapping whole blocks
        good = [b for b, *_ in summary]
        blk_list = [blk for blk in blocks if all(b in per_block.get(blk, {}) for b in good)]

        def geo(sample):
            return statistics.fmean(statistics.fmean(per_block[blk][b] for blk in sample) for b in good)
        if blk_list and good:
            g = geo(blk_list)
            lo, hi = bootstrap_ci(blk_list, geo, n=1000)
            print(f'{"GEOMEAN (" + str(len(good)) + " benches)":34s} {100 * (math.exp(g) - 1):+7.2f}%  '
                  f'CI [{100 * (math.exp(lo) - 1):+6.2f}, {100 * (math.exp(hi) - 1):+6.2f}]  '
                  f'(block bootstrap over {len(blk_list)} complete blocks)')


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    r = sp.add_parser('run')
    r.add_argument('--arm', action='append', required=True, help='name=python[,python...]')
    r.add_argument('--loops', required=True)
    r.add_argument('--bench')
    r.add_argument('--blocks', type=int, default=5)
    r.add_argument('--values', type=int, default=3)
    r.add_argument('--scale', type=float, default=1.0, help='multiply calibrated loop counts')
    r.add_argument('--seed', type=int, default=None)
    r.add_argument('--tag', default=os.uname().nodename)
    r.add_argument('--out', required=True)
    a = sp.add_parser('analyze')
    a.add_argument('files', nargs='+')
    a.add_argument('--base', default='base')
    a.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()
    {'run': cmd_run, 'analyze': cmd_analyze}[args.cmd](args)


if __name__ == '__main__':
    main()
