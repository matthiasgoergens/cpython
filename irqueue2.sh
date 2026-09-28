#!/bin/bash
# Second pass: warm .pyc caches (irbench now does it); includes a PGO A/A build.
while pgrep -f irqueue.sh > /dev/null; do sleep 30; done
cd /home/user/cpython
while ! grep -q BUILD_DONE /home/user/build/pgo-aa/make.log 2>/dev/null; do sleep 30; done
for spec in tc-before:/home/user/build/tc-before/python tc-after:/home/user/build/tc-after/python pgo-base:/home/user/build/pgo/python pgo-aa:/home/user/build/pgo-aa/python pgo-vecfast:/home/user/build/pgo-vecfast/python; do
  name=${spec%%:*}; py=${spec#*:}
  /home/user/drv/bin/python perf-notes/tools/irbench.py run --python $py --loops /home/user/res/loops.json --out /home/user/res/v2-$name-ir.json -j4 > /home/user/res/v2-$name-ir.log 2>&1
  echo "$(TZ=Asia/Singapore date +%H:%M) done $name"
done
