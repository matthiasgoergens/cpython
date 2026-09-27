#!/usr/bin/env python3
"""Run every calibrated benchmark once under a --enable-pystats build.

Each benchmark writes its stats into OUT/<bench>/ so that they can be
summarized one by one or all together with Tools/scripts/summarize_stats.py.
"""
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from irbench import discover, env_for, worker_cmd  # noqa: E402

python, loops_file, out = sys.argv[1:4]
with open(loops_file) as f:
    loops = json.load(f)
for name, script, extra in discover():
    if name not in loops:
        continue
    shutil.rmtree('/tmp/py_stats', ignore_errors=True)
    os.makedirs('/tmp/py_stats')
    env = env_for()
    env['PYTHONSTATS'] = '1'
    p = subprocess.run(worker_cmd(python, script, extra, loops[name]), env=env,
                       capture_output=True, text=True, cwd=os.path.dirname(script))
    if p.returncode:
        print(name, 'FAILED', p.stderr[-500:])
        continue
    dest = os.path.join(out, name)
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree('/tmp/py_stats', dest)
    print(name, 'ok', flush=True)
