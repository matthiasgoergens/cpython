Clang 19 and Apple clang from Xcode 16.3–26.3 merge the computed-goto dispatch jumps into one shared indirect jump (gh-158283). This adds a configure check for those compilers that passes `-mllvm -tail-dup-pred-size=1000` when compiling `ceval.c`, which restores one dispatch jump per opcode. The limit comes from llvm/llvm-project#78582.

With `--with-lto` the option also has to reach the linker's LTO backend, because the clang driver ignores `-mllvm` on the link line. It is passed as `-Wl,-plugin-opt=` for lld and GNU ld/gold with LLVMgold, and as `-Wl,-mllvm,` for ld64.

The check sits next to the Clang 22 `-finline-max-stacksize` workaround (gh-148284) and follows the same pattern. It is limited to the affected versions because Clang 18 and older reject the option.

Dispatch jumps in `_PyEval_EvalFrameDefault` in the final binary, before → after:

| compiler | build | x86-64 | arm64 |
|---|---|---|---|
| Clang 19.1.7 | `--with-lto=thin` | 1 → 276 | |
| Clang 19.1.7 | `--enable-optimizations --with-lto` | 1–7 → 359–365 | |
| Xcode 16.4 (Apple clang 1700.0.13) | `--with-lto` | 1 → 276 | 1 → 300 |
| Xcode 26.3 (Apple clang 1700.6) | `--with-lto` | 62 → 276 | 94 → 300 |

On GitHub's macOS runners the check matches Xcode 16.3, 16.4, 26.0.1, 26.1.1, 26.2 and 26.3, and not Xcode 15.0.1–15.4, 16.0–16.2 or 26.4.1–26.6. On Linux it matches Clang 19 but not Clang 18, Clang 21 or GCC.

pyperformance, this PR vs main (negative is faster; 95% CI from a bootstrap over independent builds; details in the issue):

| configuration | builds | geomean | 95% CI |
|---|---|---|---|
| Clang 19, `--with-lto=thin`, no PGO (FreeBSD's configuration) | 8 | −8.7% | [−9.2, −8.3] |
| Clang 19, `--enable-optimizations --with-lto` | 6 | −8.4% | [−9.5, −6.9] |
| Xcode 16.4, `--enable-optimizations --with-lto`, macOS arm64 | 5 | −11.4% | [−13.2, −9.8] |
| Xcode 26.3, `--enable-optimizations --with-lto`, macOS arm64 | 5 | −1.4% | [−2.2, −0.7] |

Builds with GCC, MSVC, Clang 18 and older, or Clang 20 and newer are unchanged.

This PR was prepared with the help of an AI assistant (Claude Code) and reviewed by me. Raw data and scripts are on the [`clang19-dispatch-data`](https://github.com/matthiasgoergens/cpython/tree/clang19-dispatch-data) branch of my fork.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
