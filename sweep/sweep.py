"""Scope of the ctypes by-value bug for packed structs.

For every struct shape (2 and 3 fields from TYPES) and pack in PACKS, and both
directions (argument, return value), classify ctypes' behaviour as
    ok       C sees / returns exactly the bytes ctypes has
    wrong    no crash, but the bytes differ (silent corruption)
    crash    the process died (signal)
    error    ctypes raised (e.g. refused the type)
The argument direction has C return an FNV-1a hash of the bytes it received,
so a mismatch cannot turn into a wild write. Each case runs in its own process.

Usage: sweep.py PYTHON COMPILER OUTDIR
"""
import itertools
import json
import os
import pathlib
import subprocess
import sys

TYPES = {"B": "uint8_t", "H": "uint16_t", "I": "uint32_t", "Q": "uint64_t", "f": "float", "d": "double"}
CTYPES = {"B": "c_uint8", "H": "c_uint16", "I": "c_uint32", "Q": "c_uint64", "f": "c_float", "d": "c_double"}
PACKS = [None, 1, 2, 4, 8]


def shapes():
    for n in (2, 3):
        yield from itertools.product(TYPES, repeat=n)


def cases():
    for shape in shapes():
        for pack in PACKS:
            yield "".join(shape), pack


def c_source(all_cases):
    out = ["#include <stdint.h>", "#include <string.h>", "#include <stddef.h>",
           "static uint64_t fnv(const unsigned char *p, size_t n) {",
           "  uint64_t h = 1469598103934665603ULL;",
           "  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }",
           "  return h; }",
           "static uint64_t fnv_more(uint64_t h, const unsigned char *p, size_t n) {",
           "  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }",
           "  return h; }"]
    for i, (shape, pack) in enumerate(all_cases):
        if pack:
            out.append(f"#pragma pack(push, {pack})")
        attr = "__attribute__((ms_struct))" if pack else ""
        fields = " ".join(f"{TYPES[c]} f{j};" for j, c in enumerate(shape))
        out.append(f"typedef struct {attr} {{ {fields} }} S{i};")
        if pack:
            out.append("#pragma pack(pop)")
        out.append(f"size_t size{i}(void) {{ return sizeof(S{i}); }}")
        # hash only field bytes: padding contents are unspecified and need not survive a copy
        parts = " ".join(f"h = fnv_more(h, (const unsigned char *)&s.f{j}, sizeof s.f{j});" for j in range(len(shape)))
        out.append(f"uint64_t arg{i}(S{i} s) {{ uint64_t h = 1469598103934665603ULL; {parts} return h; }}")
        out.append(f"S{i} ret{i}(const unsigned char *src) {{ S{i} s; memcpy(&s, src, sizeof s); return s; }}")
    return "\n".join(out) + "\n"


CHILD = r'''
import ctypes, sys, json
lib = ctypes.CDLL(sys.argv[1])
i, shape, pack, direction = int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
CT = {"B": ctypes.c_uint8, "H": ctypes.c_uint16, "I": ctypes.c_uint32, "Q": ctypes.c_uint64,
      "f": ctypes.c_float, "d": ctypes.c_double}
ns = {"_fields_": [(f"f{j}", CT[c]) for j, c in enumerate(shape)]}
if pack != "None":
    ns["_layout_"] = "ms"; ns["_pack_"] = int(pack)
S = type("S", (ctypes.Structure,), ns)
getattr(lib, f"size{i}").restype = ctypes.c_size_t
csize = getattr(lib, f"size{i}")()
if csize != ctypes.sizeof(S):
    print(json.dumps({"result": "layout-mismatch", "c": csize, "py": ctypes.sizeof(S)})); sys.exit(0)
raw = bytes((37 * k + 11) % 251 + 1 for k in range(ctypes.sizeof(S)))  # distinct non-zero bytes
def fnv(b, h=1469598103934665603):
    for x in b:
        h ^= x; h = (h * 1099511628211) & (2**64 - 1)
    return h
def field_bytes(obj):
    b = bytes(obj)
    return [b[getattr(S, n).offset:getattr(S, n).offset + ctypes.sizeof(t)] for n, t in S._fields_]
if direction == "arg":
    f = getattr(lib, f"arg{i}"); f.argtypes = [S]; f.restype = ctypes.c_uint64
    obj = S.from_buffer_copy(raw)
    got = f(obj)
    h = 1469598103934665603
    for fb in field_bytes(obj):
        h = fnv(fb, h)
    print(json.dumps({"result": "ok" if got == h else "wrong"}))
else:
    f = getattr(lib, f"ret{i}"); f.argtypes = [ctypes.c_char_p]; f.restype = S
    got = f(raw)
    print(json.dumps({"result": "ok" if field_bytes(got) == field_bytes(S.from_buffer_copy(raw)) else "wrong"}))
'''


def main():
    python, compiler, outdir = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    all_cases = list(cases())
    (outdir / "cases.c").write_text(c_source(all_cases))
    subprocess.run([compiler, "-O1", "-shared", "-fPIC", "-Wno-attributes", "-Wno-pragma-pack",
                    "-o", str(outdir / "cases.so"), str(outdir / "cases.c")], check=True)
    (outdir / "child.py").write_text(CHILD)
    results = []
    for i, (shape, pack) in enumerate(all_cases):
        for direction in ("arg", "ret"):
            p = subprocess.run([python, str(outdir / "child.py"), str(outdir / "cases.so"), str(i), shape,
                                str(pack), direction], capture_output=True, text=True, timeout=60)
            if p.returncode < 0:
                res = {"result": "crash", "signal": -p.returncode}
            elif p.returncode != 0:
                res = {"result": "error", "err": p.stderr.strip().splitlines()[-1][:200] if p.stderr.strip() else ""}
            else:
                res = json.loads(p.stdout.strip().splitlines()[-1])
            res.update(shape=shape, pack=pack, direction=direction)
            results.append(res)
    (outdir / "results.json").write_text(json.dumps(results, indent=0))


if __name__ == "__main__":
    main()
