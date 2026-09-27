# Does Apple clang merge CPython's computed-goto dispatch? Published evidence (as of 2026-09-27)

Scope: the LLVM tail-duplication regression for `indirectbr` blocks ([llvm#78582](https://github.com/llvm/llvm-project/pull/78582),
LLVM 19), the partial fix [#116072](https://github.com/llvm/llvm-project/pull/116072) (LLVM 20.1.0), the full fix
[#114990](https://github.com/llvm/llvm-project/pull/114990) (LLVM 20.1.1 via backport), and the Darwin-only follow-up
[#150911](https://github.com/llvm/llvm-project/pull/150911) (LLVM 21.1.0). The question is which Apple clang / Xcode
releases carry which state. This complements `CLANG19_EXPOSURE.md`, which covers who ships affected binaries.

Labels: **[V]** = verified here (quote, source file, or commit read directly). **[I]** = inference. **[N]** = searched for
and not found.

---

## TL;DR

| Xcode | Apple clang | Open-source proxy branch (LLVM major) | TailDuplicator state | Verdict | Evidence strength |
|---|---|---|---|---|---|
| 16.0–16.2 | 16.0.0 (clang-1600.0.26.3/.4/.6) | `swift/release/6.0` (LLVM 17) | No pred/succ limit | **Not affected** | Strong for source [V]. No binary disassembly published. |
| 16.3–16.4 (and 16.3 betas from 2025-02-21) | 17.0.0 (clang-1700.0.9.2 beta, 1700.0.13.3, 1700.0.13.5) | `swift/release/6.1`, `6.1.1` (LLVM 19) | #78582 limit, **no** #116072, **no** #114990 | **Affected (full LLVM 19 state)** | Medium. Source is verified [V]. There are no public disassembly or benchmarks of the Apple binary [N]. Apple's own statements imply this state costs about 2–3% on Apple Silicon for Python [V quote]. |
| 26.0–26.3 | 17.0.0 (clang-1700.3.19.1, 1700.4.4.1, 1700.6.3.2, 1700.6.4.2) | `swift/release/6.2`, `6.2.0`, `6.2.2` (LLVM 19 + cherry-picked #116072) | Limit applies only when phis are present; no computed-goto exemption | **Partial (same as LLVM 20.1.0)** | Medium. Source is verified [V]. Apple's engineer says this state recovers the Python loss on Apple Silicon, and that adding #114990 on top gives "< 0.5%" more [V quote]. x86-64 is unknown. No public disassembly [N]. |
| 26.4+ (26.4, 26.4.1, 26.5, 26.6, 27.0) | 21.0.0 (clang-2100.0.123.102, 2100.1.1.101, 2100.3.34.2) | `swift/release/6.3`, `6.4.x` (LLVM 21) | #114990 + #150911 (`DupComputedGotoLate` on Darwin) | **Fixed** | Strong for source [V]. No published measurement [N]. |

Apple's released clang is built from Apple-internal sources. The swiftlang branches are the best public proxy, but
they are not proof of what shipped [I]. The Xcode→clang build mapping comes from the
[yamaya gist](https://gist.github.com/yamaya/2924292) [V].

---

## 1. The LLVM changes: who, what, and which benchmarks

### #78582: introduces the regression (LLVM 19)
- Author: **Quentin Dian (DianQK)**. Merged 2024-04-17. Upstream commit `86a7828`.
  [PR](https://github.com/llvm/llvm-project/pull/78582) [V]
- Motivation: compile-time and OOM blowups ([#78578](https://github.com/llvm/llvm-project/issues/78578)). The PR says:
  "Duplicating a BB which has both multiple predecessors and successors will result in a complex CFG and also may cause
  huge amount of PHI nodes." [V]
- The PR adds `-tail-dup-pred-size` and `-tail-dup-succ-size`, both defaulting to 16 in the 19.1.7 source [V].
- An early warning sign on AArch64 came from alexfh (2024-04-22): "We started seeing a ~36% regression in the
  [evalloop.c] benchmark on AArch64" (llvm-test-suite `SingleSource/Benchmarks/Misc/evalloop.c`). The regression went
  away with `-tail-dup-succ-size=32`; 24 was not enough. The author called the benchmark unrepresentative. [V, via
  WebFetch summary of the PR thread]
- The regression itself is [llvm#106846](https://github.com/llvm/llvm-project/issues/106846), filed by Mikulas Patocka
  (Ajla) on 2024-08-31: "clang-19 joins all the 'goto *next_label' statements in a function into just one 'jmp *'
  instruction". The slowdown was worst on Sandy Bridge and absent on Zen 4. The issue is labelled `regression:19` with
  milestone "LLVM 20.X Release". [V]

### #116072: partial fix, driven by Apple and CPython (LLVM 20.1.0)
- Author: **Florian Hahn (fhahn, Apple)**. Opened 2024-11-13, merged 2025-01-23.
  [PR](https://github.com/llvm/llvm-project/pull/116072) [V]
- Commit message: "This adjusts the threshold logic added in #78582 to only trigger for cases where there are actually
  phis to duplicate in either TailBB or in one of the successors. … **This improves performance of Python on some inputs
  by 2-3% on Apple Silicon CPUs.**" [V]
- fhahn, 2024-11-18: "For this particular case, it is a computed GOTO, but completely removing the cutoff increases the #
  of instructions by 10%, without any gain." [V]
- fhahn, 2025-01-22: "It would be great if we could get this resolved one way or another for the Clang 20 release, as
  this at the moment causes a **2-3% performance loss for Python workloads on ARM64** :) I rebased and tested #114990 and
  unfortunately it doesn't yield the same perf gain ( only in the noise/ < 0.5%)". On #114990 he also wrote: "I tried
  the patch when building Python on macOS and it improves performance by ~0.5% while with #116072 increases performance
  by 2-3%". [V]
- dianqk, 2025-01-23: "LGTM. I believe these two PRs address different issues. **I can verify that the PR doesn't resolve
  #106846 due to phis.**" [V] In other words, #116072 alone does **not** un-merge the Ajla test case, which is x86. It
  does help CPython on Apple Silicon, as measured by Apple.
- The Python version is not stated in #116072. The later #150911 says "Python 3.12". Neither PR names the workload or the
  Apple CPU model [V].

### #114990: full fix, computed-goto exemption (trunk 2025-03-10; LLVM 20.1.1)
- Author: **DianQK**. Merged 2025-03-10. [PR](https://github.com/llvm/llvm-project/pull/114990) [V]
- The fix exempts blocks ending in a computed goto (not jump tables) from the pred/succ limit. DianQK measured Ajla
  going from 98.4 billion to 77.3 billion `instructions:u` [V].
- It was reverted on trunk (#132431, by alexfh) over protobuf compile-time and memory blowups, then relanded with better
  detection (#132536). It was backported to 20.x as #130585 and refined in #133082. The source at `llvmorg-20.1.1`+ has
  `HasComputedGoto` bypassing the limit. [V, see `CLANG19_EXPOSURE.md` §0 and this session's `raw.githubusercontent` reads]

### #150911: Darwin-specific follow-up, delay to after RA (LLVM 21.1.0)
- Author: **Florian Hahn (Apple)**. Merged 2025-07-31. Backported to `release/21.x` as
  [#151680](https://github.com/llvm/llvm-project/pull/151680), merged 2025-08-21. [V]
- Description: "#114990 allowed more aggressive tail duplication for computed-gotos in both pre- and post-regalloc tail
  duplication. In some cases, performing tail-duplication too early can lead to worse results, especially if we
  duplicate blocks with a number of phi nodes. **This is causing a ~3% performance regression in some workloads using
  Python 3.12.** … For the case in #106846, I get the same performance with and without this patch on Skylake." [V]
- fhahn on #114990, 2025-07-28: "We found that perfoming aggressive tail-dup for blocks with computed gotos before
  register allocation is causing a 3% regression on some Python 3.12 workloads **on macOS**." [V]
- In the #151680 backport, fhahn says the change "fixes a regression in the Python interpreter **on AArch64 compared to
  20.1.0**". mikulas-patocka reported a 9% regression on an X86 synthetic benchmark (pointless moves). As a result the
  change was **restricted to Apple platforms** (`DupComputedGotoLate = HasComputedGoto && …isOSDarwin()`). [V, via
  WebFetch summary. The `isOSDarwin()` gate is verified in the `llvmorg-21.1.0` and `swift/release/6.3` source.]
- On LLVM main / `llvmorg-22.1.0`, the Darwin gate is gone: computed gotos are exempt pre-RA and duplicated post-RA on all
  targets (`if (HasComputedGoto && !PreRegAlloc) MaxDuplicateCount = max(…,10)`;
  `if (PreRegAlloc && pred>… && succ>…)`) [V, source].

**Reading of Apple's own numbers [I]:** on Apple Silicon with Python 3.12, fhahn's data orders the states as follows.
LLVM 19 (#78582 only) is about 2–3% slower than LLVM 20.1.0 (#116072). The 20.1.1–20.1.8 state (#114990, early
duplication) is about 3% slower than 20.1.0. LLVM 21 (#150911) is about the same as 20.1.0. So on arm64 macOS, the
"partial" state is the *good* state, and Apple's clang 1700.3–1700.6 (Xcode 26.0–26.3) is not meaningfully penalised
there. Whether every CPython dispatch site is un-merged in that state has not been shown. No one has published a
`br xN` count.

---

## 2. Apple clang ↔ swiftlang branch ↔ TailDuplicator state

### Xcode → Apple clang (yamaya gist) [V]
16.0 `clang-1600.0.26.3` · 16.1 `.26.4` · 16.2 `.26.6` · **16.3 `clang-1700.0.13.3`** · **16.4 `1700.0.13.5`** ·
**26.0/26.0.1 `1700.3.19.1`** · **26.1 `1700.4.4.1`** · **26.2 `1700.6.3.2`** · **26.3 `1700.6.4.2`** · 26.4/26.4.1
`clang-2100.0.123.102` (Apple clang **21.0.0**) · 26.5/26.6 `2100.1.1.101` · 27.0 `2100.3.34.2`.
Source: [gist.github.com/yamaya/2924292](https://gist.github.com/yamaya/2924292).
Xcode 16.3 beta 1 (2025-02-21) shipped Apple clang 17.0.0 (1700.0.9.2) with Swift 6.1, per xcodereleases.com
(via search summary) [V-weak].
Xcode 26 ships Swift 6.2, and Xcode 26.4 ships Swift 6.3, per the Apple release notes (search summary) [V-weak].
Xcode 16.3 ships Swift 6.1 [widely known; not re-fetched].

### swiftlang/llvm-project, `llvm/lib/CodeGen/TailDuplicator.cpp` (read via raw.githubusercontent.com today) [V]

| Branch | LLVM major | `TailDupPredSize` limit | #116072 phi check | #114990 `HasComputedGoto` bypass | #150911 `DupComputedGotoLate` (Darwin) |
|---|---|---|---|---|---|
| `swift/release/6.0` | 17 | absent | – | – | – |
| `swift/release/6.1`, `6.1.1` | 19 | **yes** (hard limit, line 581) | **no** | no | no |
| `stable/20240723` (head `6db1f39`) | 19 | yes | **yes** | no | no |
| `swift/release/6.2`, `6.2.0`, `6.2.2` | 19 | yes | **yes** | no | no |
| `stable/20250402`, `stable/20250601` | 21 (dev) | yes | yes | **yes** (`terminatorIsComputedGoto`, pre-#150911) | no |
| `swift/release/6.3`, `6.4.x`, `stable/21.x` | 21 | yes | yes | yes | **yes** (`…WithSuccessors`, `isOSDarwin()`) |

- File history on `swift/release/6.1`: the newest TailDuplicator commits are `0f0cfcf` (#99652, 2024-07-19) and
  `86a7828` (#78582, 2024-04-17). There is no #116072.
  [history](https://github.com/swiftlang/llvm-project/commits/swift/release/6.1/llvm/lib/CodeGen/TailDuplicator.cpp) [V]
- On `swift/release/6.2`, the top commit is **`c811c97f3587274aaf2078b168d5331248ae399e`** "[TailDup] Allow large
  number of predecessors/successors without phis. (#116072)". Author Florian Hahn 2025-01-23; **committer Florian Hahn
  2025-01-24**, i.e. fhahn cherry-picked it himself onto the LLVM 19-based Apple branch the day after the upstream merge.
  [history](https://github.com/swiftlang/llvm-project/commits/swift/release/6.2/llvm/lib/CodeGen/TailDuplicator.cpp),
  commit read via `git fetch` [V]
- #114990 was **never** cherry-picked to the LLVM 19-based Apple branches (6.1, 6.2) [V]. This is consistent with
  Apple's measurement that #114990 did not help, and later hurt, Python on Apple Silicon [I].
- The Apple clang 21 branches carry the upstream 21.x state, including the #150911 backport [V].

---

## 3. Published benchmarks and disassembly of CPython on macOS

- **Nelson Elhage, "Performance of the Python 3.14 tail-call interpreter"** (2025-03-09,
  [blog](https://blog.nelhage.com/post/cpython-tail-call/)); the same data is in his
  [cpython#128718 comment](https://github.com/python/cpython/pull/128718) of 2025-03-06. [V]
  - Machines: Intel Raptor Lake i5-13500 and an "Apple M1 Macbook Air". "All builds use LTO and PGO." The compilers are
    **upstream LLVM via Nix** ("You can reproduce these builds using my `nix` configuration"): `clang18` = 18.1.8,
    `clang19` = 19.1.7, and `clang19.taildup` = 19.1.7 with `-mllvm -tail-dup-pred-size=5000` (and the LDFLAGS
    equivalent). **Not Apple clang/Xcode**; the post never mentions Apple clang.
  - M1 results vs clang18: clang19 **1.12x slower**, clang19.taildup 1.02x slower, clang19.tc 1.00x.
  - Disassembly was done only on x86-64: `objdump -S --disassemble=_PyEval_EvalFrameDefault … | egrep -c 'jmp\s+\*'`
    gives **332** for clang18 and **3** for clang19. There is no arm64 `br xN` count.
  - Quote: "these impressive performance gains turned out to be primarily due to inadvertently working around a
    regression in LLVM 19."
- **Ken Jin (Fidget-Spinner)**, cpython#128718, 2025-03-06: "I will advocate to the team to updating the benchmarking
  results with the numbers of GCC and **Xcode clang 17** as baseline, which means a 3-5% speedup, not 10% speedup." [V]
  Apple clang **17** existed only as the Xcode 16.3 beta then (beta 1 on 2025-02-21). That compiler is Swift 6.1-based
  and in the fully affected state per the source. [I] So a baseline described as "Xcode clang 17" may itself have had
  merged dispatch.
- **LWN, "Python, tail calls, and performance"** (Jake Edge, 2025-08-20, on Ken Jin's EuroPython 2025 talk,
  [LWN](https://lwn.net/Articles/1033373/)): "It also showed **5-7% improvement for Arm64 macOS, but there is a belief
  that its compiler has the same bug**; unfortunately, that cannot be confirmed because _another_ bug, in profile-guided
  optimization (PGO), does not allow measuring the performance correctly." [V] This is the only public statement
  suspecting Apple's compiler, and it is explicitly unconfirmed.
- **Ken Jin, "Python 3.15's interpreter for Windows x86-64 should hopefully be 15% faster"** (2025-12-24,
  [blog](https://fidget-spinner.github.io/posts/no-longer-sorry.html)): "the tail calling interpreter for CPython was
  found to beat the computed goto interpreter by 5% on pyperformance on AArch64 macOS using XCode Clang". He is
  "partially retracting that apology, but only for two platforms—macOS AArch64 (XCode Clang) and Windows x86-64 (MSVC)."
  There is no Xcode version and no link for the macOS number. [V]
  [cpython#139922](https://github.com/python/cpython/issues/139922) (2025-10-10) likewise lists "4-5% pyperformance
  faster on macOS AArch64" with no compiler version [V]. The Xcode version behind these numbers is unknown. If it was
  16.3/16.4, part of the 5% would be the tail-dup bug [I].
- **faster-cpython/benchmarking-public**: the documented darwin-arm64 runner is an "M1 arm64 Mac® Mini, running macOS
  13.2.1, clang 1400.0.29.202" (Apple clang 14, not affected). The last darwin results are dated 2025-08-30
  ([repo](https://github.com/faster-cpython/benchmarking-public)) [V]. I found no per-Xcode comparison there [N].
- **CPython 3.14 What's New** (in-tree `Doc/whatsnew/3.14.rst`) states the baseline as "Python 3.14 built with Clang 19"
  and does not mention macOS or Xcode [V].
- **Not found [N]:** no benchmark comparing python.org vs Homebrew vs pyenv on macOS attributed to Xcode 16.3+; no
  Homebrew issue or discussion about a Python slowdown tied to Xcode 16.3/26.x; no "Python slower on macOS 15" report
  tied to the compiler; no blog post or issue with an arm64 `br x…` count in `_PyEval_EvalFrameDefault` for any Apple
  clang. [cpython#129987](https://github.com/python/cpython/issues/129987) (dispatch merging) has no macOS/Apple clang
  data in its fetched content. Searches covered combinations of Xcode 16.3, clang-1700, Homebrew, pyperformance,
  `_PyEval_EvalFrameDefault`, `br x`, and `tail-dup-pred-size`.

---

## 4. Other interpreters

- **Ajla** (llvm#106846) is the original report, on x86 (Sandy Bridge worst, Zen 4 none). It has no Apple data [V].
- **llvm-test-suite `evalloop.c`**: about 36% regression on AArch64 (Google, alexfh) right after #78582 landed [V].
- **tinyactor** ([PR #162](https://github.com/tiancaiamao/tinyactor/pull/162)): with Apple clang 16 (LLVM 17-based, not
  affected), computed goto measured +16–19% on a dispatch microbenchmark after code-shape changes. The PR notes the tail
  duplicator recovered only 3 indirect-branch points. That shows code-shape sensitivity, not the LLVM 19 regression [V].
- **Ruby YARV, Lua/LuaJIT, wasm3, WAMR, QuickJS, Luau:** I found no public report tying their performance to LLVM 19 or
  Apple clang 17 tail duplication [N]. Search summaries that claimed Ruby was affected cited no source, so I discount
  them.

---

## 5. Conclusions

Facts [V]:
1. Apple (fhahn) benchmarked CPython (3.12) on Apple Silicon against this exact code path throughout 2024–2025. #116072
   and #150911 exist because of those measurements.
2. The open-source branch behind Xcode 16.3/16.4 (`swift/release/6.1`) has the unmitigated LLVM 19 limit. The branch
   behind Xcode 26.0–26.3 (`swift/release/6.2`) has #116072 only, cherry-picked by fhahn on 2025-01-24. Apple clang 21
   (Xcode 26.4+) has the full fix, including the Darwin-specific #150911.
3. No public disassembly or benchmark isolates any Apple clang build. The only public suspicion (LWN, Aug 2025) is
   explicitly unconfirmed.

Inferences [I]:
- Xcode 16.3–16.4 (clang-1700.0.13.x) most likely merges most CPython dispatch jumps on arm64 and x86-64, as upstream
  19.1.x does. Apple's own number for the arm64 cost relative to the #116072 state is about 2–3%. Elhage's upstream-19
  M1 number relative to clang 18 is 12%, but that includes other 18→19 differences and PGO effects.
- Xcode 26.0–26.3 is probably fine on arm64 for CPython, per Apple's measurements. It is unknown on x86-64 macOS, where
  dianqk showed #116072 does not fix the phi-bearing Ajla case.
- Direct check (still open): build 3.12–3.14 with each Xcode and count `br x` in `_PyEval_EvalFrameDefault`, e.g.
  `otool -tv … | awk '/_PyEval_EvalFrameDefault:/,/^_[A-Za-z]/' | grep -c 'br\s*x'`.
