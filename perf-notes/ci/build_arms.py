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
    try:
        sh(f'{src}/configure {base_configure} {spec.get("configure_extra", "")} > configure.log 2>&1', cwd=bdir)
        sh(f'make -j{os.cpu_count()} > make.log 2>&1', cwd=bdir)
    except subprocess.CalledProcessError:
        sh(f'tail -60 {bdir}/configure.log {bdir}/make.log || true')
        raise
    built[key] = bdir

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
