# Who ships CPython built by a clang with the LLVM 19 computed-goto tail-dup regression? (as of 2026-09-27)

Context: with clang 19, the tail duplicator stops un-merging the shared `indirectbr`, so every `DISPATCH()` in
`_PyEval_EvalFrameDefault` collapses into a single `jmp *` (see `LOG.md`, "FINDING: gh-129987 was the LLVM 19
tail-dup regression"). We measured the cost at about 8.6% on pyperformance (PGO+LTO, x86-64). Builds that use the
tail-calling interpreter (`--with-tail-call-interp`) are not affected, and neither are GCC or MSVC builds.

Conventions:
- **[verified-binary]**: I downloaded the shipped binary and read the compiler string (`[Clang …]` in `sys.version`,
  from libpython or the `.comment` section) and, where available, `CONFIG_ARGS` / `Py_TAIL_CALL_INTERP` from sysconfigdata.
- **[verified-source]**: I read the build recipe or the compiler source at the relevant tag.
- **[inferred]**: reasoning from versions or policy, not checked against a binary.
- **[unverified]**: memory or secondary reporting.

---

## 0. Correction to our premise: which LLVM versions are affected

I checked `llvm/lib/CodeGen/TailDuplicator.cpp` at each tag.

| LLVM | State | Evidence |
|---|---|---|
| ≤ 18.1.8 | **Not affected.** The pred/succ limit does not exist yet. | [#78582](https://github.com/llvm/llvm-project/pull/78582) was merged on 2024-04-17, so it first ships in 19. `TailDupPredSize` is absent at `llvmorg-18.1.8`. [verified-source] |
| 19.1.0 – 19.1.7 | **Fully affected.** There is a hard limit when pred > 16 and succ > 16. | No 19.x backport of any fix. [verified-source] |
| **20.1.0** | **Partially mitigated.** It has [#116072](https://github.com/llvm/llvm-project/pull/116072) (fhahn, 2025-01-23: the limit applies only when phis are present) but not the computed-goto exemption. | fhahn: #116072 "improves performance of Python on some inputs by 2-3% on Apple Silicon". Rebasing #114990 on top gave "only in the noise/ < 0.5%" **on Apple Silicon**. On x86-64 it is unknown. [verified-source + quote] |
| **20.1.1 – 20.1.8** | **Fixed.** #114990 was backported via [#130585](https://github.com/llvm/llvm-project/pull/130585) (merged 2025-03-11), then refined by [#133082](https://github.com/llvm/llvm-project/pull/133082) (the blockaddress-based detection, #132536). | `terminatorIsComputedGoto` / `HasComputedGoto` bypass the limit at `llvmorg-20.1.1` and later. [verified-source] |
| 21.1.x | **Fixed.** On Darwin, the aggressive duplication is delayed until after register allocation ([#150911](https://github.com/llvm/llvm-project/pull/150911), backported to 21.x in #151680). | `DupComputedGotoLate … isOSDarwin()` appears in `llvmorg-21.1.0`. [verified-source] |
| main (22/23+) | **Fixed.** Computed gotos are unrestricted after register allocation on all targets. | [verified-source] |

So the precise statement is "fixed in LLVM **20.1.1**", not "fixed in LLVM 20". The 20.1.0 release still lacks the
exemption. On trunk, #114990 was merged 2025-03-10, reverted 2025-03-21 over compile-time blowups, and relanded with
#132536 on 2025-03-26.

Workaround on 19.x: the `-mllvm -tail-dup-pred-size=… -tail-dup-succ-size=…` knobs exist in 19.1.7 (`TailDupPredSize`,
default 16), which is what `perf/clang19-taildup` uses.

### Apple clang ↔ LLVM mapping

Apple clang is built from `swiftlang/llvm-project`. I read `TailDuplicator.cpp` on each `swift/release/*` branch
[verified-source]. The Xcode ↔ Apple-clang build numbers come from the [yamaya gist](https://gist.github.com/yamaya/2924292).

| Xcode | Apple clang | Swift branch → LLVM base | Tail-dup state |
|---|---|---|---|
| 16.0–16.2 | 16.0.0 (clang-1600.0.26.x) | 6.0 → LLVM 17-based | Not affected |
| **16.3–16.4** | **17.0.0 (clang-1700.0.13.3 / .13.5)** | 6.1 → `stable/20240723`, LLVM 19 | **Fully affected** (no #116072, no #114990) |
| **26.0–26.3** | **17.0.0 (clang-1700.3.19.1 … 1700.6.4.2)** | 6.2 → LLVM 19 + #116072 | **Partially mitigated** (same state as LLVM 20.1.0) |
| 26.4+ / 27.0 | 21.0.0 (clang-2100.x) | 6.3 → LLVM 21 (has `DupComputedGotoLate`) | Fixed |

Caveat: Xcode's clang may carry changes beyond the open-source swiftlang branch. [inferred]

---

## 1. Exposure table

"Affected" means computed goto **and** a compiler in an affected row above. "Partial" means the Apple clang
1700.3–1700.6 or LLVM 20.1.0 state.

| Shipper | CPython versions | Compiler (version) | Dispatch | Affected? | Evidence |
|---|---|---|---|---|---|
| **FreeBSD pkg (lang/python311–314), current `latest` for FreeBSD:14 and FreeBSD:15 amd64** | 3.11.16, 3.12.14, 3.13.15, 3.14.7 | base clang **19.1.7** (`llvmorg-19.1.7-0-gcd708029e0b2`) | computed goto (ports pass no tail-call flag; `--with-lto` thin, no PGO) | **YES** | [verified-binary]: all 8 packages show `[Clang 19.1.7 (…llvmorg-19.1.7…)]`. Ports `USES=compiler:c11` means base cc ([python314 Makefile](https://github.com/freebsd/freebsd-ports/blob/main/lang/python314/Makefile)). |
| FreeBSD base clang by release | – | 13.5: 19.1.7; 14.2: 18.1.6; **14.3: 19.1.7; 14.4: 19.1.7**; 14.5 (2026-09-08): 21.1.8; **15.0: 19.1.7; 15.1: 19.1.7**; main: 21.1.8 | – | – | [verified-source]: `lib/clang/include/VCSVersion.inc` at each `release/*` tag in freebsd-src. No local tail-dup backport in `contrib/llvm-project`. Packages are built on the oldest supported minor of each branch, so exposure continues until 14.4 and 15.1 are EOL or 15.2 lands with a newer LLVM [inferred, policy]. |
| **OpenBSD 7.8 (Oct 2025) / 7.9 (May 2026) packages** | 7.8: 3.12.11; 7.9: 3.13.13 | base clang **19.1.7** | computed goto | **YES** | [verified-binary]: `python-3.12.11.tgz` and `python-3.13.13.tgz` (amd64) both show `[Clang 19.1.7 ]`. The port uses `COMPILER = base-clang …` ([Makefile.inc](https://github.com/openbsd/ports/blob/master/lang/python/Makefile.inc)). Base LLVM comes from the release pages ([78](https://www.openbsd.org/78.html), [79](https://www.openbsd.org/79.html)). 7.5–7.7 used clang 16.0.6 (not affected). -current imported LLVM 21.1.6 on 2026-05-29, so 8.0 should be fixed [inferred]. |
| **OpenMandriva Lx 6.0 (stable, Apr 2025)** | 3.11.11 | clang **19.1.7** (the distro default compiler) | computed goto (`--with-computed-gotos=yes`, `--enable-optimizations`, `--with-lto=thin`) | **YES** | [verified-binary]: `lib64python3.11_1-3.11.11` contains `clang version 19.1.7`. The [6.0 python.spec](https://github.com/OpenMandrivaAssociation/python/blob/6.0/python.spec) and [6.0 llvm.spec](https://github.com/OpenMandrivaAssociation/llvm/blob/6.0/llvm.spec) (19.1.7) confirm. |
| OpenMandriva ROME / Cooker (rolling) | 3.14.7 | clang 23.x | **tail call** (`--with-tail-call-interp` on x86_64/aarch64) | no | [verified-source]: [master python.spec](https://github.com/OpenMandrivaAssociation/python/blob/master/python.spec). |
| **Homebrew bottles, macOS Sequoia (arm64_sequoia, sequoia x86_64)**, current | python@3.11, 3.12, 3.13, 3.14 (3.14.7 / 3.13.15 / …) | Apple clang **17.0.0 (clang-1700.6.4.2 = Xcode 26.3)** | computed goto (`--enable-optimizations --with-lto`, no tail-call) | **PARTIAL** (#116072-only state). Earlier Sequoia bottles built with Xcode 16.3/16.4 (clang-1700.0.13.x) were fully affected [inferred]. | [verified-binary]: bottles pulled from ghcr.io. The formula is [python@3.14.rb](https://github.com/Homebrew/homebrew-core/blob/main/Formula/p/python@3.14.rb). |
| Homebrew bottles, macOS Tahoe (arm64_tahoe) | 3.11–3.14 | Apple clang 21.0.0 (clang-2100.1.1.101) | computed goto | no | [verified-binary] |
| Homebrew bottles, macOS Sonoma | 3.11–3.14 | Apple clang 16.0.0 (clang-1600.0.26.6) | computed goto | no | [verified-binary] |
| Homebrew Linux | – | GCC | computed goto | no | [inferred] |
| **MacPorts binary archives, darwin_24 (macOS 15)** | python313 3.13.15, python314 3.14.7 (+lto+optimizations) | Apple clang **17.0.0 (clang-1700.0.13.5 = Xcode 16.4)** | computed goto (`--with-computed-gotos`, PGO+LTO by default) | **YES** (full) | [verified-binary]: from packages.macports.org. Portfile: [python314](https://github.com/macports/macports-ports/blob/master/lang/python314/Portfile). darwin_25 (macOS 26) uses clang-2100 (no); darwin_23 uses clang-1500 (no). |
| python.org macOS installer | **3.14.4 only** | Apple clang **17.0.0 (clang-1700.6.4.2)** | computed goto (`Py_TAIL_CALL_INTERP` = 0) | **PARTIAL** | [verified-binary]. The rest: 3.13.0–3.13.3 used clang-1500; 3.13.4–3.13.13 and 3.14.0–3.14.3 used clang-1600 (no); 3.13.14, 3.13.15, 3.14.5–3.14.7 used clang-2100 (no). **3.15.0rc2 uses `--with-tail-call-interp`** with clang-2100 (no). All builds use `--enable-optimizations --with-lto=full --with-computed-gotos`. |
| **conda-forge osx-arm64 / osx-64** | 3.11.14, 3.12.12, 3.13.8–3.13.13 (built ~Oct 2025 – Apr 2026) | conda-forge clang **19.1.7** | computed goto | **YES** for those builds (historical; still installable when pinned) | [verified-binary]: `3.13.8/10/12/13` and `3.11.14`/`3.12.12` show `[Clang 19.1.7 ]`. Older 3.11.11–3.13.5 used 18.1.8 (no). Current 3.11.16/3.12.14/3.13.15 use **21.1.8** (no). |
| conda-forge osx, 3.14.x / 3.15 | 3.14.4–3.14.7, 3.15.0rc2 | clang 20.1.8 / 21.1.8 | **tail call** (`--with-tail-call-interp` on osx) | no | [verified-binary] plus [build_base.sh](https://github.com/conda-forge/python-feedstock/blob/main/recipe/build_base.sh) ("This should only be used with clang 20.1+"). |
| conda-forge linux | all | GCC (3.13.15: `[GCC 15.3.0]`) | computed goto | no | [verified-binary]. The global pin is `c_compiler_version: 15 # [linux]` ([pinning](https://github.com/conda-forge/conda-forge-pinning-feedstock/blob/main/recipe/conda_build_config.yaml)). |
| **python-build-standalone (and therefore uv, rye, mise, hatch)**, releases **20250106, 20250115, 20250205, 20250212** | 3.10.16, 3.11.11, 3.12.8/3.12.9, 3.13.1/3.13.2 (and 3.14.0a3–a5, before tail-call was enabled on 2025-02-12) | PBS toolchain clang **19.1.6** (Linux x86_64/aarch64 and macOS) | computed goto (PGO+LTO(+BOLT on Linux)) | **YES** (historical; uv installs persist until upgraded) | [verified-source]: `pythonbuild/downloads.py` at each tag. `llvm-18.0.8+20240713` = 18.1.8 (no). From 20250311 on, the LLVM 20.1.0 toolchain **carries a `tail-duplicator-computed-gotos.patch`** ([PBS #553](https://github.com/astral-sh/python-build-standalone/pull/553): "fix a regression in LLVM 19"). Later releases use 20.1.4, 21.1.4 and 22.1.3 (no). 3.14+ uses tail-call since [#524](https://github.com/astral-sh/python-build-standalone/pull/524). This is the build Simon Willison compared against ([nelhage](https://blog.nelhage.com/post/cpython-tail-call/)). |
| pyenv (macOS) | any 3.11–3.14 compiled locally | system Apple clang | computed goto (no PGO unless the user opts in) | **YES** on Xcode/CLT 16.3–16.4, **partial** on 26.0–26.3, no on 26.4+ | [verified, 3rd-party report]: [pyenv#3326](https://github.com/pyenv/pyenv/issues/3326) shows `Python 3.13.7 … [Clang 17.0.0 (clang-1700.0.13.5)] on darwin`. Linux pyenv uses the system gcc (no). |
| **Nixpkgs 25.05 (darwin)** | python311–313 | stdenv clang **19.1.7** (`llvmPackages = llvmPackages_19`; darwin stdenv uses the default) | computed goto (`ac_cv_computed_gotos=yes`; `enableOptimizations ? false`) | **YES** [verified-source, not binary] | [all-packages.nix @ nixos-25.05](https://github.com/NixOS/nixpkgs/blob/nixos-25.05/pkgs/top-level/all-packages.nix); no tail-dup patch in `common/llvm`. 25.11 and 26.05 use LLVM 21 (no). Linux Nix uses gcc (no). 25.05 is EOL. |
| Chimera Linux (clang/musl, LLVM is the system compiler) | 3.12.x packages built ~late 2024 – 2025-04-16 | clang 19.1.x (19.1.4→19.1.7) | computed goto (`--with-computed-gotos`) | **historical yes** [inferred] | [cports llvm history](https://github.com/chimera-linux/cports/commits/master/main/llvm/template.py): 20.1.3 arrived on 2025-04-16. Now LLVM 22.1.8 and python 3.14.6 (no). |
| **BeeWare Python-Apple-support (iOS; used by Briefcase/Toga)** | 3.13-b12, 3.14-b9 (and probably neighbouring builds) | Apple clang **17.0.0 (clang-1700.0.13.5)** | computed goto | **YES** (arm64 iOS) | [verified-binary]. 3.13-b15 and 3.14-b11 use clang-2100 (no); b2–b9 of 3.13 and b2–b6 of 3.14 use clang-1500 (no). |
| CPython official Android (python.org tarballs, 3.13+) | 3.13, 3.14.7, 3.15 | NDK **r27d** (`ndk_version=27.3.13750724`) → Android clang 18.0.4 (clang-r522817, upstream base `3c92011b`, LLVM 18 dev) | computed goto | no | [verified-binary] `[Clang 18.0.4 (…llvm-project d8003a45…)]`. [verified-source]: base `3c92011b` has no `TailDupPredSize`. |
| Termux | 3.14.6 | NDK r30 (clang-r574158c, ~LLVM 22 dev) | computed goto | no [inferred] | [properties.sh](https://github.com/termux/termux-packages/blob/master/scripts/properties.sh) (`TERMUX_NDK_VERSION_NUM=30`). Past NDK r27/r28 bases (r522817, r530567 = `3b5e7c83`) both predate #78582 [verified-source], so there is no historical exposure either. |
| Pyodide / Emscripten (wasm32) | – | emscripten clang | N/A | no [inferred] | WebAssembly has no indirect branch: `indirectbr` is lowered to a `br_table` switch in IR, before MachineFunction tail-dup runs. |
| Windows python.org | – | MSVC | switch / computed goto N/A | no | – |
| Windows clang-cl | 3.14+ | clang-cl (opt-in, `--tail-call-interp` in PCbuild) | tail call when used | no known shipper | [unverified] |
| NetBSD (pkgsrc), DragonFly | – | GCC (base) | computed goto | no | [unverified] |
| Alpine, ALT, Debian, Ubuntu, Fedora, Arch, Wolfi | – | GCC | computed goto | no | [unverified for ALT, Wolfi] |
| Gentoo with LLVM profile | user-built | user's clang | computed goto | possibly, depending on what the user chose | [inferred] |

---

## 2. Public discussion linking CPython slowness to clang 19

- **Nelson Elhage, "Performance of the Python 3.14 tail-call interpreter"** (2025-03-09):
  [blog.nelhage.com/post/cpython-tail-call](https://blog.nelhage.com/post/cpython-tail-call/). Computed goto under
  clang 19.1.7 was **1.09x slower** than clang 18 on a Raptor Lake i5-13500 and **1.12x slower on an M1**. `clang19.taildup`
  (the `-mllvm` knobs) recovered it to 1.01x faster / 1.02x slower. The headline 3.14 tail-call speedup "turned out to be
  primarily due to inadvertently working around a regression in LLVM 19." The post names python-build-standalone as the
  in-the-wild baseline (Simon Willison's 10% number compared tail-call 3.14 against PBS 3.13 built with clang 19.1.6).
  It does not mention Homebrew, python.org, FreeBSD or Apple clang.
- The LLVM issue is [llvm#106846](https://github.com/llvm/llvm-project/issues/106846), opened 2024-08-31 by the Ajla author. It
  reports the worst regression on Sandy Bridge and the least on Zen 4.
- [cpython#129987](https://github.com/python/cpython/issues/129987) (dispatch merging) is closed. The discussion is
  GCC-centred; the linked PRs are about SLP vectorization. It says nothing explicit about clang 19.
- In [llvm#150911](https://github.com/llvm/llvm-project/pull/150911), fhahn (Apple) cites a ~3% regression "in some
  workloads using Python 3.12" from the early #114990 duplication, and #116072 cites +2–3% for Python on Apple Silicon.
  Apple was evidently benchmarking CPython against this code.
- I found **no CPython issue or Homebrew/FreeBSD/OpenBSD bug report** about the slowdown on those platforms. The
  exposure there appears to be unnoticed.

---

## 3. Takeaways

1. **The largest current exposure** is on the BSDs: **FreeBSD** (every current `pkg` Python on 14.x and 15.x is
   clang 19.1.7 with computed goto, until package builders move to 14.5 or 15.2) and **OpenBSD 7.8/7.9**. Both are
   verified from binaries, and both use unpatched upstream 19.1.7.
2. **macOS 15 Sequoia package managers:** MacPorts darwin_24 builds use Xcode 16.4, so they are fully affected.
   Homebrew Sequoia bottles use Xcode 26.3, the partial #116072-only state. Its effect on x86-64 is unknown; on Apple
   Silicon, fhahn's data suggests most of the loss is recovered. Homebrew Sequoia bottles built between roughly Apr and
   Sep 2025 (Xcode 16.3/16.4) were fully affected.
3. **Historical, now fixed:** PBS/uv Jan–Feb 2025, conda-forge macOS 3.11–3.13 from roughly Oct 2025 to Apr 2026,
   Nixpkgs 25.05 darwin, Chimera until Apr 2025, BeeWare iOS mid-2025 builds, and the python.org 3.14.4 macOS installer
   (partial).
4. **An upstream CPython configure workaround** (add `-mllvm -tail-dup-pred-size=… -tail-dup-succ-size=…` for clang 19.x, and
   possibly Apple clang 1700.x) would mainly benefit the FreeBSD and OpenBSD ports and people building with Xcode 16.3–26.3.
   The BSD ports could apply the flag themselves today.
5. **Open measurement question:** does the #116072-only state (LLVM 20.1.0, Apple clang 1700.3–1700.6) still merge
   CPython's dispatch on x86-64 PGO+LTO? Count `jmp *` in `_PyEval_EvalFrameDefault` with a clang 20.1.0 build.

## Measured: Apple clang on GitHub macOS runners (2026-09-27, 20:25 SGT)
`Python/ceval.o` at -O3 (main 6af40a6), indirect dispatch branches in `_PyEval_EvalFrameDefault`
(arm64 `br xN` / x86-64 `jmp *`), default vs with `-mllvm -tail-dup-pred-size=1000`:

| Xcode | Apple clang | arm64 | x86_64 | with flag (arm64/x86_64) |
|---|---|---|---|---|
| 15.0.1–15.4 | clang-1500.x | 275 | — | (n/a) |
| 16.0–16.2 | clang-1600.0.26.x | 290 | 269 | (n/a) |
| **16.3, 16.4** | **clang-1700.0.13.3/.5** | **1** | **1** | 291 / 269 |
| **26.0.1–26.3** | **clang-1700.3.19.1 – 1700.6.4.2** | **123** | **112** | 287 / 269 |
| 26.4.1–26.6 | clang-2100.x (Apple clang 21) | 200 | (not installed) | 328 |

- Xcode 16.3–16.4 = fully merged (LLVM 19 state) on both architectures.
- Xcode 26.0–26.3 ("partial fix") still merges ~60% of dispatch sites on CPython — affects current Homebrew Sequoia bottles.
- Apple clang 21 (Darwin-specific late duplication) replicates fewer sites than the flag achieves (200 vs 328); perf impact unknown.
- Performance impact on macOS not measured yet (only dispatch counts).

### Patch verification on macOS (21:00 SGT)
The configure check (`__apple_build_version__` in [17000000, 18000000)) selects exactly Xcode 16.3–26.3 on the
runners and restores 291/287 (arm64) and 269 (x86_64) dispatch jumps per object. Unaffected Xcodes are left alone,
which is required: clang ≤ 18 rejects `-tail-dup-pred-size` as an unknown option. Final-binary (LTO, ld64
`-Wl,-mllvm`) verification and the speed impact (mac1b) are pending.
