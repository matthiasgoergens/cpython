#!/usr/bin/env python3
"""Cheap, deterministic proxy for pyperformance: instruction counts under cachegrind.

For every pure-Python pyperformance benchmark we run the pyperf worker once
with K loops and once with 2K loops (each with one warmup value) under
`valgrind --tool=cachegrind` and take the difference.  That cancels out
interpreter startup and imports and leaves ~2K loops of warmed-up work.

Usage:
  irbench.py calibrate --python BUILD/python --out loops.json
  irbench.py run --python BUILD/python --loops loops.json --out res.json [-j 4] [--sim]
  irbench.py compare base.json new.json [more.json ...]
"""
import argparse
import concurrent.futures as cf
import glob
import json
import math
import os
import random
import statistics
import re
import subprocess
import sys
import tempfile
import time
import tomllib

def _find_benchdir():
    if os.environ.get('PERF_BENCHDIR'):
        return os.environ['PERF_BENCHDIR']
    import pyperformance
    return os.path.join(os.path.dirname(pyperformance.__file__), 'data-files', 'benchmarks')


BENCHDIR = _find_benchdir()
# pyperf installed with `pip install --target` so that the interpreters under test can import it.
PYPERF_LIB = os.environ.get('PERF_PYPERF_LIB', '/home/user/pyperf-lib')

# Benchmarks we skip: need network/threads/subprocess heavy, or measure startup.
SKIP = {
    'python_startup', 'python_startup_no_site', 'concurrent_imap', 'asyncio_tcp',
    'asyncio_tcp_ssl', 'sqlite_synth', 'regex_compile',  # regex_compile imports other bms
    'hg_startup',
}


def discover():
    out = []
    for d in sorted(glob.glob(os.path.join(BENCHDIR, 'bm_*'))):
        if os.path.exists(os.path.join(d, 'requirements.txt')):
            continue
        tomls = [os.path.join(d, 'pyproject.toml')] + sorted(glob.glob(os.path.join(d, 'bm_*.toml')))
        for t in tomls:
            if not os.path.exists(t):
                continue
            with open(t, 'rb') as f:
                cfg = tomllib.load(f).get('tool', {}).get('pyperformance', {})
            name = cfg.get('name')
            if not name or name in SKIP:
                continue
            script = os.path.join(d, cfg.get('runscript', 'run_benchmark.py'))
            out.append((name, script, list(cfg.get('extra_opts', []))))
    return out


def env_for(hashseed=0):
    # The hash seed is a layout-like nuisance factor: callers should sample it
    # (sharing one seed across the arms being compared), not pin it.
    env = dict(os.environ)
    env['PYTHONPATH'] = PYPERF_LIB
    env['PYTHONHASHSEED'] = str(hashseed)
    env.pop('PYTHONHOME', None)
    return env


def worker_cmd(python, script, extra, loops, warmups=1):
    return [python, '-u', script, '--worker', '--loops', str(loops), '--warmups', str(warmups),
            '--values', '1', '--processes', '1', '--inherit-environ', 'PYTHONPATH,PYTHONHASHSEED'] + extra


def run_native(python, script, extra, loops):
    t = time.perf_counter()
    p = subprocess.run(worker_cmd(python, script, extra, loops), env=env_for(),
                       capture_output=True, text=True, cwd=os.path.dirname(script), timeout=120)
    dt = time.perf_counter() - t
    if p.returncode:
        raise RuntimeError(p.stderr[-2000:])
    return dt


def run_cg(python, script, extra, loops, sim, hashseed=0, no_aslr=False):
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, 'cg.out')
        # ASLR stays on by default: address-dependent behaviour (id() hashing,
        # set/dict order, GC timing) is a layout factor to sample, not to pin
        # (the Stabilizer lesson).  --no-aslr exists for debugging only.
        cmd = (['setarch', os.uname().machine, '-R'] if no_aslr else []) + [
               'valgrind', '--tool=cachegrind', f'--cachegrind-out-file={out}']
        if sim:
            cmd += ['--cache-sim=yes', '--branch-sim=yes']
        else:
            cmd += ['--cache-sim=no']
        # No warmup value: the 2K-minus-K difference already cancels startup and warmup.
        cmd += worker_cmd(python, script, extra, loops, warmups=0)
        p = subprocess.run(cmd, env=env_for(hashseed), capture_output=True, text=True,
                           cwd=os.path.dirname(script), timeout=3600)
        if p.returncode:
            raise RuntimeError(p.stderr[-3000:])
        events = summary = None
        with open(out) as f:
            for line in f:
                if line.startswith('events:'):
                    events = line.split()[1:]
                elif line.startswith('summary:'):
                    summary = [int(x) for x in line.split()[1:]]
        return dict(zip(events, summary))


def calibrate(args):
    benches = discover()
    loops = {}
    for name, script, extra in benches:
        # Native time for loops=1 includes startup; grow loops until the
        # incremental time of the second run is ~target seconds.
        try:
            base = run_native(args.python, script, extra, 1)
            k = 1
            while True:
                t = run_native(args.python, script, extra, 2 * k)
                if t - base > args.target or k >= 1 << 20:
                    break
                k *= 2
            loops[name] = k
            print(f'{name:35s} loops={k:<8d} startup+1={base:.3f}s  2k={t:.3f}s', flush=True)
        except Exception as e:
            print(f'{name:35s} FAILED: {str(e).strip().splitlines()[-1] if str(e).strip() else e}', flush=True)
    with open(args.out, 'w') as f:
        json.dump(loops, f, indent=1)


def run(args):
    with open(args.loops) as f:
        loops = json.load(f)
    benches = [b for b in discover() if b[0] in loops]
    if args.only:
        pat = re.compile(args.only)
        benches = [b for b in benches if pat.search(b[0])]
    # Warm the bytecode caches first.  Otherwise the K and 2K runs race to
    # compile and write .pyc files and whichever loses pays for compilation.
    for name, script, extra in benches:
        try:
            run_native(args.python, script, extra, 1)
        except Exception as e:
            print(f'{name}: warm-up failed: {e}', file=sys.stderr)
    # Replicates r = 0..R-1 use hash seeds derived from --seed, identical for
    # every build, so comparisons between builds are paired (common random numbers).
    seeds = [random.Random(args.seed * 1000 + r).randrange(1, 2**32) for r in range(args.replicates)]
    jobs = []
    for name, script, extra in benches:
        k = loops[name]
        for r, seed in enumerate(seeds):
            for mult in (1, 2):
                jobs.append((name, r, mult, script, extra, k * mult, seed))
    res = {}
    t0 = time.time()
    with cf.ThreadPoolExecutor(args.j) as ex:
        futs = {ex.submit(run_cg, args.python, s_, e, l, args.sim, seed, args.no_aslr): (n, r, m)
                for n, r, m, s_, e, l, seed in jobs}
        for fut in cf.as_completed(futs):
            n, r, m = futs[fut]
            try:
                ev = fut.result()
                res.setdefault(n, {}).setdefault(r, {})[m] = ev
                with open(args.out + '.partial', 'a') as pf:  # observable progress
                    pf.write(json.dumps({'bench': n, 'rep': r, 'mult': m, 'ev': ev}) + '\n')
            except Exception as e:
                print(f'{n} rep{r} x{m} FAILED: {e}', file=sys.stderr)
    out, reps = {}, {}
    for n, byrep in sorted(res.items()):
        deltas = [{ev: d[2][ev] - d[1][ev] for ev in d[1]} for r, d in sorted(byrep.items()) if 1 in d and 2 in d]
        if deltas:
            reps[n] = deltas
            out[n] = {ev: sum(d[ev] for d in deltas) / len(deltas) for ev in deltas[0]}
    print(f'{len(out)} benchmarks x {len(seeds)} replicates in {time.time() - t0:.0f}s', file=sys.stderr)
    with open(args.out, 'w') as f:
        json.dump({'python': args.python, 'sim': args.sim, 'seeds': seeds, 'results': out,
                   'replicates': reps}, f, indent=1)


def cost(ev):
    """Rough cycle estimate if cache/branch sim data is present."""
    if 'Bcm' not in ev:
        return ev['Ir']
    return (ev['Ir'] + 10 * (ev['Bcm'] + ev['Bim'])
            + 10 * (ev['I1mr'] + ev['D1mr'] + ev['D1mw'])
            + 100 * (ev['ILmr'] + ev['DLmr'] + ev['DLmw']))


def compare(args):
    runs = []
    for p in args.files:
        with open(p) as f:
            runs.append(json.load(f)['results'])
    base = runs[0]
    names = sorted(set(base).intersection(*runs[1:]))
    small = [n for n in names if base[n]['Ir'] < args.min_ir]
    if small:
        print(f'skipping {len(small)} benchmarks with baseline delta < {args.min_ir:.0e} Ir: {", ".join(small)}')
    names = [n for n in names if n not in small]
    metric = cost if args.metric == 'cost' else (lambda ev: ev[args.metric])
    hdr = f'{"benchmark":32s}' + ''.join(f'{os.path.basename(p)[:14]:>16s}' for p in args.files[1:])
    print(hdr)
    logs = [[] for _ in runs[1:]]
    for n in names:
        b = metric(base[n])
        row = f'{n:32s}'
        for i, r in enumerate(runs[1:]):
            ratio = metric(r[n]) / b
            logs[i].append(math.log(ratio))
            row += f'{(ratio - 1) * 100:+15.2f}%'
        if not args.quiet:
            print(row)
    print(f'{"GEOMEAN (" + str(len(names)) + ")":32s}' + ''.join(
        f'{(math.exp(sum(l) / len(l)) - 1) * 100:+15.2f}%' for l in logs))
    # Paired analysis over replicates (replicate r used the same hash seed in every file).
    reps = []
    for p in args.files:
        with open(p) as f:
            reps.append(json.load(f).get('replicates'))
    if all(reps) and len(args.files) == 2:
        a, b = reps
        R = min(min(len(a[n]), len(b[n])) for n in names)
        if R >= 2:
            per = {n: [math.log(metric(b[n][r]) / metric(a[n][r])) for r in range(R)] for n in names}
            print(f'paired over {R} replicates (random hash seeds, ASLR on):')
            noisy = sorted(names, key=lambda n: -statistics.pstdev(per[n]))[:5]
            print('  noisiest benchmarks (sd of log-ratio across replicates): ' +
                  ', '.join(f'{n} {100 * statistics.pstdev(per[n]):.2f}%' for n in noisy))
            rng = random.Random(3)
            def geo(idx):
                return statistics.fmean(statistics.fmean(per[n][r] for r in idx) for n in names)
            bs = sorted(geo([rng.randrange(R) for _ in range(R)]) for _ in range(2000))
            g = geo(range(R))
            print(f'  GEOMEAN {100 * (math.exp(g) - 1):+.2f}%  bootstrap-over-replicates CI '
                  f'[{100 * (math.exp(bs[50]) - 1):+.2f}, {100 * (math.exp(bs[1949]) - 1):+.2f}]')


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    c = sp.add_parser('calibrate')
    c.add_argument('--python', required=True)
    c.add_argument('--out', required=True)
    c.add_argument('--target', type=float, default=0.05)
    r = sp.add_parser('run')
    r.add_argument('--python', required=True)
    r.add_argument('--loops', required=True)
    r.add_argument('--out', required=True)
    r.add_argument('-j', type=int, default=4)
    r.add_argument('--sim', action='store_true')
    r.add_argument('--only')
    r.add_argument('--replicates', type=int, default=3)
    r.add_argument('--seed', type=int, default=12345, help='same seed => same hash seeds for every build')
    r.add_argument('--no-aslr', action='store_true', help='debug only: pins one layout sample')
    m = sp.add_parser('compare')
    m.add_argument('files', nargs='+')
    m.add_argument('--metric', default='Ir')
    m.add_argument('-q', '--quiet', action='store_true')
    m.add_argument('--min-ir', type=float, default=1e7)
    args = ap.parse_args()
    {'calibrate': calibrate, 'run': run, 'compare': compare}[args.cmd](args)


if __name__ == '__main__':
    main()
