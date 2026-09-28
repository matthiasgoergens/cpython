# _sre dispatch under gh-158286: local measurements (2026-09-28)

Regex benchmarks only (regex_dna, regex_effbot, regex_v8), blockbench randomised blocks, CPU per row.
Arms: main (a57d165), pr (65772c6, the PR), pr_asmgoto (PR + `sre-asmgoto.patch`: shared dispatch block is
an `asm goto` target, keeping _sre's dispatch merged), pr_srenolto (sre.c compiled -fno-lto), pr_sreswitch /
main_sreswitch (sre.c with -DUSE_COMPUTED_GOTOS=0), main_asmgoto, same (same binary as main).

| machine | toolchain | files |
|---|---|---|
| i9-13900K | clang 19.1.1, thin LTO, podman `build.sh`/`run-bench*.sh` | i9-regex*, i9-asmgoto* |
| M4 Max | Apple clang 1700.6.3.2, full LTO, `mac-build-and-run.sh` | m4max-regex* |
| M3 (Air) | same binaries as M4 Max | m3air-regex*, m3air-variance.txt |

Indirect-jump counts: jumps.txt, sreswitch.txt, host-compilers.txt (GCC 16.2.1, Clang 22.1.8), mac-*.txt.
regex-by-cpu.txt: exp3/exp9b/exp11b regex rows split by runner CPU (from all-cpu.jsonl).
