#!/bin/sh
# Usage: run-bench.sh OUT.jsonl BLOCKS
A=$HOME/prog/python/clang19-dispatch-notes/ablation
S=$HOME/prog/python/pr158286-ci-notes/sre
T=$HOME/prog/python/clang19-dispatch-notes/wt-sre-cpu
out=$1; blocks=$2
exec taskset --cpu-list 4-5 nice podman run --rm \
  -v $A/src-main:/src/src-main:ro -v $A/src-pr:/src/src-pr:ro \
  -v $A/builds/main:/build/main:ro -v $A/builds/pr:/build/pr:ro \
  -v $S/builds/main-sreswitch:/build/main-sreswitch:ro -v $S/builds/pr-sreswitch:/build/pr-sreswitch:ro \
  -v $S/builds/pr-srenolto:/build/pr-srenolto:ro -v $S/builds/pr-asmgoto:/build/pr-asmgoto:ro -v $S/builds/main-asmgoto:/build/main-asmgoto:ro \
  -v $HOME/prog/python/cpythons/sre-asmgoto:/src/src-asmgoto:ro -v $HOME/prog/python/cpythons/sre-asmgoto-main:/src/src-asmgoto-main:ro \
  -v $T:/perf-notes:ro -v $S/timing:/out \
  -e PERF_PYPERF_LIB=/pyperf-lib \
  localhost/cpy-clang19-bench \
  /drv/bin/python /perf-notes/tools/blockbench.py run \
    --arm main=/build/main/python --arm same=/build/main/python \
    --arm pr=/build/pr/python --arm pr_asmgoto=/build/pr-asmgoto/python \
    --arm main_asmgoto=/build/main-asmgoto/python \
    --loops /perf-notes/ci/loops.json --bench '^regex_' \
    --blocks "$blocks" --rounds 3 --values 1 --seed 158286 --tag i9-local \
    --out "/out/$out"
