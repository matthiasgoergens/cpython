#!/bin/sh
# Usage (inside container): build.sh VARIANT SRC
# VARIANT *-srenolto: compile Modules/_sre/sre.c with -fno-lto appended last,
# so its code is generated at compile time without the link-time option.
set -u
v=$1; src=/src/$2; b=/build/$v
export PATH=/usr/lib/llvm-19/bin:$PATH
mkdir -p "$b" && cd "$b" || exit 1
CC=clang-19 "$src/configure" --with-lto=thin > configure.log 2>&1 || { echo CONFIGURE-FAILED; exit 1; }
grep 'dispatch jumps needs' configure.log
case $v in
  *-sreswitch) sed -i 's|-c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|-DUSE_COMPUTED_GOTOS=0 -c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|' Makefile
              grep -c -- '-DUSE_COMPUTED_GOTOS=0 -c $(srcdir)/Modules/_sre/sre.c' Makefile ;;
  *-srenolto) sed -i 's|-c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|-fno-lto -c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|' Makefile
              grep -c -- '-fno-lto -c $(srcdir)/Modules/_sre/sre.c' Makefile ;;
esac
make -j16 python > make.log 2>&1 || { echo MAKE-FAILED; tail -20 make.log; exit 1; }
grep -m1 -E 'Modules/_sre/sre.c' make.log
