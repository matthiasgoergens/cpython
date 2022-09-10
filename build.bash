#!/bin/bash
set -e
set -u
set -o pipefail
set -x

dir="fast-builds/less-alloc"
here="$(realpath .)"

mkdir --parent "${dir}"
pushd "${dir}"
# Don't care about leaks for now.
# export ASAN_OPTIONS=detect_leaks=0
export CC="clang"
# export CC="gcc"
# nice "${here}/configure" \
#     --with-assertions \
#     --with-address-sanitizer \
#     --with-trace-refs \
#     --with-pydebug \
#     --with-undefined-behavior-sanitizer

nice "${here}/configure" \
    --enable-optimizations
nice make -j8

# ./_bootstrap_python ../empty.py
