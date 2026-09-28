#!/usr/bin/env python3
"""Add the runner CPU to blockbench rows from runs made before rows recorded it.

    backfill_cpu.py OWNER/REPO RUN_ID IN.jsonl OUT.jsonl

Rows are tagged "<run_id>-<matrix job>"; the CPU of each matrix job is read from
the "Machine info" step of that job's log (lscpu "Model name").  Needs `gh`, and
only works while GitHub still has the logs (90 days by default).
"""
import json
import re
import subprocess
import sys

repo, run_id, src, dst = sys.argv[1:5]
jobs = json.loads(subprocess.check_output(
    ['gh', 'api', '--paginate', f'repos/{repo}/actions/runs/{run_id}/jobs?per_page=100'], text=True))['jobs']
cpu_by_tag = {}
for job in jobs:
    m = re.fullmatch(r'block \((\d+)\)', job['name'])
    if not m:
        continue
    p = subprocess.run(['gh', 'api', '--allow-escape-sequences',
                        f'repos/{repo}/actions/jobs/{job["id"]}/logs'],
                       capture_output=True, text=True)
    cpu = re.search(r'Model name:\s*(.+)', p.stdout)
    if p.returncode or not cpu:
        sys.exit(f'job {job["id"]} ({job["name"]}): no CPU in log '
                 f'(gh exit {p.returncode}: {p.stderr.strip()[:200]})')
    cpu_by_tag[f'{run_id}-{m.group(1)}'] = cpu.group(1).strip()
for tag, cpu in sorted(cpu_by_tag.items()):
    print(f'{tag}: {cpu}', file=sys.stderr)

missing = set()
with open(src) as f, open(dst, 'w') as out:
    for line in f:
        if not line.strip():
            continue
        r = json.loads(line)
        r.setdefault('cpu', cpu_by_tag.get(r['tag'], 'unknown'))
        if r['tag'] not in cpu_by_tag:
            missing.add(r['tag'])
        out.write(json.dumps(r) + '\n')
if missing:
    sys.exit(f'no CPU found for tags: {sorted(missing)}')
