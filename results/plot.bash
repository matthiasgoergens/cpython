#!/bin/bash
set -xeuo pipefail
#  IFS=$'\n\t'

bench="mixed_append_and_rotate"

for impl in meque deque; do
    cat "${bench}.data" | grep "${impl}" > "${bench}.${impl}.data"
done

gnuplot plot_${bench}.gnuplot
