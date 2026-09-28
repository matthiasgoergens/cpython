#!/bin/sh
# Usage (inside container): build-cc.sh VARIANT SRC CC   -- plain --with-lto=thin build, python only
set -u
v=$1; src=/src/$2; cc=$3; b=/build/$v
mkdir -p "$b" && cd "$b" || exit 1
CC=$cc "$src/configure" --with-lto > configure.log 2>&1 || { echo CONFIGURE-FAILED; tail -5 configure.log; exit 1; }
grep 'dispatch jumps needs' configure.log
make -j16 python > make.log 2>&1 || { echo MAKE-FAILED; tail -20 make.log; exit 1; }
./python -c 'import sys; print(sys.version)'
