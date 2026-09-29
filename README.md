# ctypes: packed structures passed or returned by value on x86-64

Evidence for a CPython issue. A `ctypes.Structure` whose `_pack_` leaves a field
under-aligned, and which is 16 bytes or smaller, is passed to and returned from foreign
functions incorrectly on x86-64 System V platforms.

## Reproducer

`repro.py PACK LAYOUT FIELDS` builds a C function that receives the structure by value and
copies its bytes out, and calls it through ctypes, e.g.

    python3 repro.py 2 ms H,I      # uint16_t, uint32_t with _pack_ = 2: segmentation fault
    python3 repro.py 4 ms H,I      # _pack_ = 4, no under-aligned field: works

(needs clang; `_layout_ = "ms"` is required for `_pack_` on non-Windows since 3.14, and the C
side then uses `__attribute__((ms_struct))`, which for these plain integer fields does not change
the layout.)

## Sweep

`sweep/sweep.py PYTHON COMPILER OUTDIR` checks every two- and three-field structure over
`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `float`, `double`, with no `_pack_` and with
`_pack_` 1, 2, 4, 8 (2520 cases), both as an argument (the C function returns an FNV-1a hash of
the bytes of each field it received, so a mismatch cannot turn into a wild write; padding is
excluded because its contents are unspecified) and as a return value; each case in its own
process.

| file | platform | C compiler | CPython | cases | wrong |
|---|---|---|---|---|---|
| `results/linux-x86_64-clang22-cpython3.14.7.json` | x86-64 Linux | clang 22.1.8 | 3.14.7 | 2520 | 528 |
| `results/linux-x86_64-gcc16-cpython3.14.7.json` | x86-64 Linux | GCC 16.2.1 | 3.14.7 | 2520 | 528 |
| `results/macos-arm64-appleclang17-cpython3.14.7.json` | arm64 macOS 27.2 | Apple clang 1700.6.3.2 | 3.14.7 | 2520 | 0 |

`sweep/check_rule.py results/*.json` recomputes the rule: on x86-64 all 528 wrong cases, and
none of the others, are structures of at most 16 bytes with a field under-aligned by `_pack_`.
The same crash reproduces on 3.13 and on main (969af80daf0).
