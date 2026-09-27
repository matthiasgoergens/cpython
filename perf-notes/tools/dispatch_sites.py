#!/usr/bin/env python3
"""Count indirect jumps in the interpreter's dispatch function(s) of a CPython binary.

With computed gotos each opcode handler should end in its own indirect jump
("replicated dispatch"), which gives the branch predictor a separate slot per
handler.  If a compiler merges those tails (gh-129987 for GCC, the LLVM 19
tail-duplication regression for clang) the count collapses.  For the tail-call
interpreter every handler is its own function, so we count indirect jumps and
indirect tail calls across all handler functions instead.

Usage: dispatch_sites.py PATH/TO/python-or-libpython
"""
import re
import subprocess
import sys

binary = sys.argv[-1]
asm = subprocess.run(['objdump', '-d', '--no-show-raw-insn', binary],
                     capture_output=True, text=True, check=True).stdout

func = None
counts = {}
for line in asm.splitlines():
    m = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
    if m:
        func = m.group(1)
        continue
    if func and re.search(r'\sjmp\s+\*', line):
        counts[func] = counts.get(func, 0) + 1

eval_funcs = {f: n for f, n in counts.items() if f.startswith('_PyEval_EvalFrameDefault')}
handlers = {f: n for f, n in counts.items() if f.startswith('_TAIL_CALL_')}
import os
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Python', 'generated_cases.c.h')
n_targets = len(re.findall(r'^\s*TARGET\(', open(src).read(), re.M)) if os.path.exists(src) else None
print(f'indirect jmps in _PyEval_EvalFrameDefault*: {sum(eval_funcs.values())} {eval_funcs}')
if handlers:
    print(f'tail-call handlers with indirect jmps: {len(handlers)}, total {sum(handlers.values())}')
if n_targets:
    print(f'opcode TARGETs in generated_cases.c.h: {n_targets}')
