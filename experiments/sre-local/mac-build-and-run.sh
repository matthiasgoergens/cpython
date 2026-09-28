#!/bin/sh
# Usage: build.sh VARIANT SRC   (SRC = src-main | src-pr under ~/clang19-ablation)
set -u
v=$1; src=$HOME/clang19-ablation/$2; b=$HOME/sre-exp/build/$v
mkdir -p "$b" && cd "$b" || exit 1
CC=clang "$src/configure" --with-lto > configure.log 2>&1 || { echo CONFIGURE-FAILED; exit 1; }
grep 'dispatch jumps needs' configure.log
case $v in
  *-sreswitch) sed -i "" 's|-c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|-DUSE_COMPUTED_GOTOS=0 -c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|' Makefile
               grep -c -- '-DUSE_COMPUTED_GOTOS=0 -c $(srcdir)/Modules/_sre/sre.c' Makefile ;;
  *-srenolto)  sed -i "" 's|-c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|-fno-lto -c $(srcdir)/Modules/_sre/sre.c -o Modules/_sre/sre.o|' Makefile
               grep -c -- '-fno-lto -c $(srcdir)/Modules/_sre/sre.c' Makefile ;;
esac
make -j8 > make.log 2>&1 || { echo MAKE-FAILED; tail -20 make.log; exit 1; }
./python.exe -c 'import math, re; print("imports ok")'
printf 'sre br: '; objdump --disassemble --no-show-raw-insn python.exe | awk '/^[0-9a-f]+ <_sre_ucs[124]_match>:/{f=$2} /^$/{f=""} f && /\tbr\tx/{n[f]++} END{for(k in n) printf "%s=%d ", k, n[k]}'; echo
python3 $HOME/clang19-ablation/dispatch_sites.py "$b/python.exe"
#!/bin/sh
# Usage: run-bench.sh OUT.jsonl BLOCKS
B=$HOME/clang19-ablation/build; E=$HOME/sre-exp/build
export PERF_PYPERF_LIB=$HOME/clang19-ablation/pyperf-lib
exec nice $HOME/clang19-ablation/drv/bin/python $HOME/sre-exp/tools/blockbench.py run \
  --arm main=$B/main/python.exe --arm same=$B/main/python.exe --arm pr=$B/pr/python.exe \
  --arm pr_sreswitch=$E/pr-sreswitch/python.exe --arm pr_srenolto=$E/pr-srenolto/python.exe \
  --arm pr_asmgoto=$E/pr-asmgoto/python.exe --arm main_sreswitch=$E/main-sreswitch/python.exe \
  --loops $HOME/sre-exp/tools/loops.json --bench '^regex_' \
  --blocks "$2" --rounds 3 --values 1 --seed 158286 --tag m4max \
  --out "$HOME/sre-exp/timing/$1"
