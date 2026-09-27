# Work log

## Environment (2026-09-27)
- 4-core Intel Xeon VM @ 2.8GHz, 15 GB RAM, gcc 13.3, clang/lld 18, valgrind 3.22.
- No hardware perf counters (no `cpu` PMU in the VM), no `perf`. Wall-clock is noisy.
- No GCC 16 or LLVM 19+, so no tail-call interpreter build locally.
- Upstream main 6af40a6 (3.16.0a0). Frame pointers are on by default (PEP 831).

## Measurement plan
- **Proxy A (deterministic, minutes):** `perf-notes/tools/irbench.py`. Runs each pure-Python
  pyperformance benchmark body under cachegrind at K and 2K loops and takes the difference in
  instruction counts (Ir). Optional `--sim` adds cache and branch simulation and gives a rough
  cycle estimate.
- **Proxy B:** pystats (`--enable-pystats`) for specialization hit/miss/deopt diagnoses.
- **Confirmation:** pyperformance wall time on PGO+LTO builds, several processes. Code-layout
  sampling via lld `--shuffle-sections=<seed>` over several seeds (a cheap static cousin of
  Stabilizer), and Stabilizer itself if it can be built here (it needs a newer LLVM than 18?).

## Early diagnoses from cachegrind (non-PGO -O3 build)
- **richards:** 51% of Ir is in `_PyEval_EvalFrameDefault`. The generic attribute path
  (`_PyObject_GenericGetAttrWithDict` 3.7%, `_PyObject_GetMethodStackRef` 2.8%,
  `unicodekeys_lookup_unicode` 4.9%, `_PyObject_TryGetInstanceAttribute` 2.9%) is about 14%
  in total. Nearly all sites specialize statically, so this is likely misses at polymorphic
  sites (`self.fn(...)` over 4 Task subclasses): monomorphic caches plus deopt.
- **raytrace:** `initialize_locals` 2.5%, `_PyEval_FrameClearAndPop` 2.4%,
  `_PyFrame_ClearExceptCode` 5.6% (decref and dealloc of temporaries), `subtype_dealloc` 1.3%.
  Calls with defaults or kwargs don't take the `CALL_PY_EXACT_ARGS` path?
