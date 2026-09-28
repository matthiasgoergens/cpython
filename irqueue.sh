#!/bin/bash
# Deterministic Ir proxy queue: one build at a time, all cores.
cd /home/user/cpython
for spec in tc-before:/home/user/build/tc-before/python tc-after:/home/user/build/tc-after/python pgo-base:/home/user/build/pgo/python pgo-dealloc:/home/user/build/pgo-dealloc/python pgo-vecfast:/home/user/build/pgo-vecfast/python; do
  name=${spec%%:*}; py=${spec#*:}
  /home/user/drv/bin/python perf-notes/tools/irbench.py run --python $py --loops /home/user/res/loops.json --out /home/user/res/$name-ir.json -j4 > /home/user/res/$name-ir.log 2>&1
  echo "$(TZ=Asia/Singapore date +%H:%M) done $name"
done
