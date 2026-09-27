#!/usr/bin/env python3
"""Build every arm listed in perf-notes/ci/config.json (run on the CI runner).

Arm spec forms in config["arms"]:
  "name": "<sha>"                                   build <sha> with config["configure"]
  "name": {"ref": "<sha>", "configure_extra": "..."} same, with extra configure flags
  "name": {"same_as": "<arm>"}                      same binary as <arm> (run-to-run control)
  "name": {"same_as": "<arm>", "sitecustomize": "..."}
                                                    wrapper around <arm>'s binary that runs the
                                                    given Python code at startup (e.g. GC tuning)
Writes $HOME/b-<name>/python for each arm and prints binary/.text hashes.
"""
import hashlib
import json
import os
import subprocess
import sys

HOME = os.environ['HOME']
cfg = json.load(open(sys.argv[1]))
repo = sys.argv[2]
base_configure = cfg.get('configure', '--enable-optimizations --with-lto')


def sh(cmd, **kw):
    print('+', cmd, flush=True)
    subprocess.run(cmd, shell=True, check=True, **kw)


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()[:16]


arms = cfg['arms']
built = {}

# Per-arm compilers: "cc": "clang-19" installs that LLVM from apt.llvm.org and
# builds with its clang/llvm-ar/llvm-profdata first on PATH.
llvm_versions = sorted({int(v['cc'].split('-')[1]) for v in arms.values()
                        if isinstance(v, dict) and v.get('cc', '').startswith('clang-')})
if llvm_versions:
    sh('curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null')
    for n in llvm_versions:
        sh(f'echo "deb https://apt.llvm.org/noble/ llvm-toolchain-noble-{n} main" | sudo tee /etc/apt/sources.list.d/llvm{n}.list')
    sh('sudo apt-get update -q')
    sh('sudo apt-get install -yq ' + ' '.join(f'clang-{n} lld-{n} llvm-{n}' for n in llvm_versions))

for name, spec in arms.items():
    if isinstance(spec, str):
        spec = {'ref': spec}
    if 'ref' not in spec:
        continue
    key = (spec['ref'], spec.get('configure_extra', ''))
    bdir = f'{HOME}/b-{name}'
    src = f'{HOME}/src-{spec["ref"][:12]}'
    if not os.path.exists(src):
        sh(f'git -C {repo} fetch -q --depth=1 origin {spec["ref"]}')
        sh(f'git -C {repo} worktree add -q --detach {src} FETCH_HEAD')
    os.makedirs(bdir, exist_ok=True)
    env = dict(os.environ)
    if spec.get('cc', '').startswith('clang-'):
        n = spec['cc'].split('-')[1]
        env['PATH'] = f'/usr/lib/llvm-{n}/bin:' + env['PATH']
        env['CC'] = 'clang'
    elif spec.get('cc'):
        env['CC'] = spec['cc']
    try:
        sh(f'{src}/configure {base_configure} {spec.get("configure_extra", "")} > configure.log 2>&1', cwd=bdir, env=env)
        sh(f'make -j{os.cpu_count()} > make.log 2>&1', cwd=bdir, env=env)
        sh(f'grep -m1 "^CC=" Makefile; {bdir}/python -c "import sys; print(sys.version)"', cwd=bdir)
    except subprocess.CalledProcessError:
        sh(f'tail -60 {bdir}/configure.log {bdir}/make.log || true')
        raise
    built[key] = bdir
    # Fail early if an optional-but-benchmarked extension module did not build.
    sh(f'{bdir}/python -c "import _decimal, _pickle, _json, _elementtree, _sqlite3"')

for name, spec in arms.items():
    if isinstance(spec, dict) and 'same_as' in spec:
        bdir = f'{HOME}/b-{name}'
        target = f'{HOME}/b-{spec["same_as"]}/python'
        os.makedirs(bdir, exist_ok=True)
        if 'sitecustomize' in spec:
            sdir = f'{bdir}/site'
            os.makedirs(sdir, exist_ok=True)
            with open(f'{sdir}/sitecustomize.py', 'w') as f:
                f.write(spec['sitecustomize'] + '\n')
            with open(f'{bdir}/python', 'w') as f:
                f.write(f'#!/bin/sh\nPYTHONPATH="{sdir}${{PYTHONPATH:+:$PYTHONPATH}}" exec {target} "$@"\n')
            os.chmod(f'{bdir}/python', 0o755)
        else:
            os.symlink(target, f'{bdir}/python')

for name in arms:
    py = f'{HOME}/b-{name}/python'
    real = os.path.realpath(py)
    text = f'{HOME}/text-{name}.bin'
    if open(real, 'rb').read(2) == b'#!':
        real = open(real).read().split('exec ')[1].split()[0]
    subprocess.run(['objcopy', '-O', 'binary', '--only-section=.text', real, text], check=True)
    out = subprocess.run([py, '-c', 'import sys, gc; print(sys.version.split()[0], gc.get_threshold())'],
                         capture_output=True, text=True, env=dict(os.environ, PYTHONPATH=''))
    print(f'HASH {name} binary={sha(real)} text={sha(text)} {out.stdout.strip()}', flush=True)
    ds = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools',
                         'dispatch_sites.py'), real], capture_output=True, text=True)
    print(f'DISPATCH {name}: {ds.stdout.strip().splitlines()[0] if ds.stdout.strip() else ds.stderr[-200:]}', flush=True)
