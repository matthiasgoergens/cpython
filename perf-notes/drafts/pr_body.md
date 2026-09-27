Clang 19, and Apple clang from Xcode 16.3–26.3, merge the computed-goto interpreter's per-opcode dispatch jumps into one shared indirect jump. That defeats per-opcode branch prediction. See gh-NNNNNN for the analysis and measurements.

This PR adds a configure check for the affected compilers. When the check matches, configure adds `-mllvm -tail-dup-pred-size=1000` to `CFLAGS_CEVAL`. The limit comes from llvm/llvm-project#78582, and raising it restores the tail duplication.

Under `--with-lto`, code generation happens in the linker's LTO backend. The clang driver does not forward `-mllvm` there; it prints "argument unused" and ignores it. So the option is also passed to the linker:
* `-Wl,-plugin-opt=` for ld.lld and GNU ld/gold with LLVMgold;
* `-Wl,-mllvm,` for ld64.

The check sits next to the existing Clang 22 `-finline-max-stacksize` workaround (gh-148284) and follows its pattern.

The check has to be limited to the affected versions. Clang 18 and older reject `-tail-dup-pred-size` as an unknown option, and fixed compilers don't need it.

## Verification

Indirect jumps in `_PyEval_EvalFrameDefault` in the final `python` binary, unpatched → patched:

| compiler | build | x86-64 | arm64 |
|---|---|---|---|
| Clang 19.1.7 | `--with-lto=thin` | 1 → 276 | |
| Clang 19.1.7 | `--enable-optimizations --with-lto` | 1–7 → 359–365 | |
| Xcode 16.4 (Apple clang 1700.0.13) | `--with-lto` | 1 → 276 | 1 → 300 |
| Xcode 26.3 (Apple clang 1700.6) | `--with-lto` | 62 → 276 | 94 → 300 |

On GitHub's macOS runners the check matched exactly Xcode 16.3, 16.4, 26.0.1, 26.1.1, 26.2 and 26.3. It did not match Xcode 15.0.1–15.4, 16.0–16.2 or 26.4.1–26.6. It did not match Clang 18, Clang 21 or GCC.

pyperformance, patched vs unpatched. Randomized blocks with arms interleaved per benchmark; the 95% CI is a bootstrap over independent builds, and a same-binary A/A control ran in each experiment:

| configuration | geomean | 95% CI |
|---|---|---|
| Clang 19, `--with-lto=thin`, no PGO (FreeBSD ports' configuration), 8 builds | −8.7% | [−9.2, −8.3] |
| Clang 19, `--enable-optimizations --with-lto`, 6 builds | −8.4% | [−9.5, −6.9] |
| Xcode 16.4, `--enable-optimizations --with-lto`, macOS arm64, 5 builds | −11.4% | [−13.2, −9.8] |
| Xcode 26.3, `--enable-optimizations --with-lto`, macOS arm64, 5 builds | −1.4% | [−2.2, −0.7] |

No change for GCC, MSVC, Clang ≤ 18 or Clang ≥ 20, or for the tail-calling interpreter: the flag only affects tail duplication of indirect branches.

This PR was prepared with the help of an AI assistant (Claude Code) and reviewed by me. The measurements, and the scripts that produced them, are available on request.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
