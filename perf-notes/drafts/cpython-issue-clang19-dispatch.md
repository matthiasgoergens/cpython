# DRAFT — CPython issue (to be filed by @matthiasgoergens after review)

**Title:** Computed-goto interpreter ~9% slower when built with Clang 19: all dispatch jumps are merged

## Bug report

### Summary

When CPython is built with **Clang/LLVM 19** (or an Apple clang based on it), the computed-goto
interpreter loop ends up with a **single shared indirect jump** instead of one dispatch jump per
instruction. This defeats per-opcode branch prediction, which is the reason the computed-goto
interpreter exists. On pyperformance this costs about **9%**.

The cause is an LLVM 19 change that limits tail duplication of blocks ending in an indirect branch
(llvm/llvm-project#78582). Clang always lowers computed gotos to one shared `indirectbr` block and
relies on tail duplication to copy it back into every predecessor. LLVM 20.1.0 partially fixed this
(llvm/llvm-project#116072), and LLVM 20.1.1 fixed it fully (llvm/llvm-project#114990). **No 19.x release
has the fix.**

The tail-calling interpreter (`--with-tail-call-interp`) and GCC builds are not affected.

This is the same effect Nelson Elhage identified in March 2025 as the main source of the reported 3.14
tail-call speedup. As far as I can tell nobody fixed it on the computed-goto side, and gh-129987 was
closed after an unrelated GCC change.

### Evidence

Indirect `jmp`s in `_PyEval_EvalFrameDefault`, measured on `Python/ceval.o` at `-O3` (x86-64) and in the
final PGO+LTO binary:

| compiler | `-O3` object | PGO+LTO binary |
|---|---|---|
| GCC 12 / 13 / 14 | 257 | 234 |
| Clang 18 | 269 | — |
| **Clang 19.1.7** | **1** | **1–9** |
| Clang 19.1.7 + `-mllvm -tail-dup-pred-size=1000` | 269 | ~357 |
| Clang 21 | 268 | ~360 |

Performance on pyperformance (main at 6af40a6, `--enable-optimizations --with-lto`, x86-64 GitHub
runners). The design was randomized blocks with the arms interleaved per benchmark, 6 independent
builds per arm, and a 95% cluster-bootstrap CI over builds. A same-binary control measured +0.18%
[−0.05, +0.41].

| comparison | geomean | 95% CI |
|---|---|---|
| Clang 19 + `-mllvm -tail-dup-pred-size=1000` vs Clang 19 | **−8.6%** (faster) | [−9.6, −7.4] |
| Clang 19 vs GCC 13 | +7.7% (slower) | [+6.4, +8.7] |
| Clang 19 + flag vs GCC 13 | −1.6% | [−1.8, −1.3] |

All 6 builds agreed, with per-build values between −6.0% and −10.2%.
With the proposed configure change applied (same design, same-binary control in each):

| configuration | geomean, patched vs unpatched | 95% CI | dispatch jumps (unpatched → patched) |
|---|---|---|---|
| Clang 19, `--with-lto=thin`, no PGO (FreeBSD ports' configuration), 8 builds | **−8.7%** | [−9.2, −8.3] | 1 → 276 |
| Clang 19, `--enable-optimizations --with-lto`, 6 builds | **−8.4%** | [−9.5, −6.9] | 1–7 → 359–365 |

The largest gains are 20–24% (unpack_sequence, deepcopy_memo, nbody, scimark_sor). The one consistent regression is
regex_effbot, +7–9%.

### Who is affected (verified from the `[Clang …]` string in shipped binaries where possible)

* **FreeBSD 14.x and 15.x packages** python311–python314 (base clang 19.1.7, thin LTO, computed goto).
* **OpenBSD 7.8 and 7.9** python 3.12 and 3.13 (base clang 19.1.7).
* **OpenMandriva Lx 6.0** python 3.11 (clang 19.1.7).
* **macOS builds made with Xcode 16.3–16.4** (Apple clang 1700.0.13.x, LLVM 19 based): 1 dispatch jump left, on
  arm64 and x86-64. An example is MacPorts python313 and python314 on macOS 15.
* **macOS builds made with Xcode 26.0–26.3** (Apple clang 1700.3–1700.6): partly merged, with 123 of ~290 (arm64) or
  112 of ~269 (x86-64) left. An example is Homebrew's macOS 15 (Sequoia) bottles. Xcode 26.4+ (Apple clang 2100)
  still merges some: 200 of 328.
  <!-- TODO: speed impact from mac1b -->
* Earlier python-build-standalone/uv (Jan–Feb 2025) and conda-forge macOS builds used Clang 19 too. Those
  have since moved on.

### Proposed fix

A small configure check. It follows the existing Clang 22 workaround for gh-148284: when the compiler
is Clang 19 or Apple clang 1700.x and computed gotos are enabled, add `-mllvm -tail-dup-pred-size=1000` to
`CFLAGS_CEVAL`. Under LTO, code generation happens at link time, so the option also goes to the linker's LTO
backend: `-Wl,-plugin-opt=` for GNU ld and lld, `-Wl,-mllvm,` for ld64. A plain `-mllvm` on the link line is
silently ignored ("argument unused").

The version gate is necessary: Clang ≤ 18 rejects the option. I verified that the check selects exactly the
affected compilers: Clang 19.1.7, and Xcode 16.3–26.3 on GitHub's macOS runners. Clang 18, Clang 21, Xcode ≤ 16.2
and Xcode 26.4+ are left alone. PR to follow.

### CPython versions tested on:
CPython main branch

### Operating systems tested on:
Linux, macOS
