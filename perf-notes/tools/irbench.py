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
import re
import subprocess
import sys
import tempfile
import time
import tomllib

BENCHDIR = glob.glob('/home/user/drv/lib/python3.*/site-packages/pyperformance/data-files/benchmarks')[0]
PYPERF_LIB = '/home/user/pyperf-lib'

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


def env_for():
    env = dict(os.environ)
    env['PYTHONPATH'] = PYPERF_LIB
    env['PYTHONHASHSEED'] = '0'
    env.pop('PYTHONHOME', None)
    return env


def worker_cmd(python, script, extra, loops):
    return [python, '-u', script, '--worker', '--loops', str(loops), '--warmups', '1',
            '--values', '1', '--processes', '1', '--inherit-environ', 'PYTHONPATH,PYTHONHASHSEED'] + extra


def run_native(python, script, extra, loops):
    t = time.perf_counter()
    p = subprocess.run(worker_cmd(python, script, extra, loops), env=env_for(),
                       capture_output=True, text=True, cwd=os.path.dirname(script))
    dt = time.perf_counter() - t
    if p.returncode:
        raise RuntimeError(p.stderr[-2000:])
    return dt


def run_cg(python, script, extra, loops, sim):
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, 'cg.out')
        cmd = ['valgrind', '--tool=cachegrind', f'--cachegrind-out-file={out}']
        if sim:
            cmd += ['--cache-sim=yes', '--branch-sim=yes']
        else:
            cmd += ['--cache-sim=no']
        cmd += worker_cmd(python, script, extra, loops)
        p = subprocess.run(cmd, env=env_for(), capture_output=True, text=True,
                           cwd=os.path.dirname(script))
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
    jobs = []
    for name, script, extra in benches:
        k = loops[name]
        for mult in (1, 2):
            jobs.append((name, mult, script, extra, k * mult))
    res = {}
    t0 = time.time()
    with cf.ThreadPoolExecutor(args.j) as ex:
        futs = {ex.submit(run_cg, args.python, s, e, l, args.sim): (n, m) for n, m, s, e, l in jobs}
        for fut in cf.as_completed(futs):
            n, m = futs[fut]
            try:
                res.setdefault(n, {})[m] = fut.result()
            except Exception as e:
                print(f'{n} x{m} FAILED: {e}', file=sys.stderr)
    out = {}
    for n, d in sorted(res.items()):
        if 1 in d and 2 in d:
            out[n] = {ev: d[2][ev] - d[1][ev] for ev in d[1]}
    print(f'{len(out)} benchmarks in {time.time() - t0:.0f}s', file=sys.stderr)
    with open(args.out, 'w') as f:
        json.dump({'python': args.python, 'sim': args.sim, 'results': out}, f, indent=1)


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
    m = sp.add_parser('compare')
    m.add_argument('files', nargs='+')
    m.add_argument('--metric', default='Ir')
    m.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()
    {'calibrate': calibrate, 'run': run, 'compare': compare}[args.cmd](args)


if __name__ == '__main__':
    main()
