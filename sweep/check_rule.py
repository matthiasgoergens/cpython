"""Recompute the rule from a sweep result file: a case is wrong exactly when the
structure is at most 16 bytes and _pack_ leaves some field under-aligned.
Run with CPython >= 3.14 (for _layout_):  python check_rule.py results/*.json"""
import collections
import ctypes
import json
import sys

CT = {"B": ctypes.c_uint8, "H": ctypes.c_uint16, "I": ctypes.c_uint32, "Q": ctypes.c_uint64,
      "f": ctypes.c_float, "d": ctypes.c_double}


def predicted(shape, pack):
    ns = {"_fields_": [(f"f{j}", CT[c]) for j, c in enumerate(shape)]}
    if pack:
        ns["_layout_"] = "ms"
        ns["_pack_"] = pack
    S = type("S", (ctypes.Structure,), ns)
    under = any(getattr(S, f"f{j}").offset % ctypes.alignment(CT[c]) for j, c in enumerate(shape))
    return "wrong" if under and ctypes.sizeof(S) <= 16 else "ok"


for path in sys.argv[1:]:
    rows = json.load(open(path))
    table = collections.Counter((predicted(r["shape"], r["pack"]), r["result"]) for r in rows)
    print(path, dict(table))
