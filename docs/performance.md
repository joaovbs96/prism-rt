# Performance

## Measurement method

The loading benchmark is in progress; formal acceptance is incomplete. The runner in [benchmark_loading.py](../tests/hydra/benchmark_loading.py) launches stock `testusdview` in fresh processes, initially using Storm so PrismRT does not inherit Prism-specific warmup. It tests Chess, Kitchen and InOneWeekend with 1, 2 and 4 material workers, three process runs per setting. Each process records one fresh-renderer sample and one renderer recreation with process-local caches warm. The [summarizer](../tests/hydra/summarize_loading.py) reports these cache classes separately and summarizes medians and ranges.

A fresh process clears PrismRT's process-local material cache; it does not clear the operating system file cache. Process and host first-image timings include Storm startup. Prism first-frame and snapshot timings measure Prism phases. Buffer-write profiling is enabled for this run, and its worker times aggregate parallel worker durations rather than wall time. The host is an Intel Core i7-14700K (28 logical processors). Results are pending in `build/loading-validation`.

## Verified checks

- The controlled [procedural/USD render-cost comparison](render_cost.md) identified HDR pixel access as the dominant runtime cost in exported InOneWeekend. A benchmark-only resident-pixel prototype reduced the MDL scene from 5.663 s to 0.3204 s at 256 x 144, 32 spp, with exact before/after images. This is a proposed production optimization, not an already deployed environment change.

- The path-cache CTest run passed all 10 tests; see [path-cache-ctest.log](../build/round2-validation/path-cache-ctest.log).
- The usdview interaction smoke run completed successfully. Recorded time to convergence was 2.234 s after a geometry edit, 3.515 s after a material edit, 1.344 s after a visibility edit, and 3.172 s after resizing. Measurements are in [interaction-timings.json](../build/round2-usdview-smoke-final/interaction-timings.json).
- The material-cache check verified texture-free class reuse, 100 distinct colors, and one native JIT with separate argument blocks.
- The analytic-sphere and shared-camera build passed all 13 CTests. Actual usdview sessions on InOneWeekend and OpenChessSet passed normal-AOV display and exact renderer-recreation checks; InOneWeekend also rendered identically at Low and Very High complexity with zero triangles. Logs are in `build/round2-validation`, with the initial sphere viewport captures in `build-fresh-validation/usdview-weekend-analytic`.
- Controlled image parity was rechecked at 64 spp: 2.14e-8 normalized RGB RMSE and 4.47e-9 relative mean-luminance error, with exact deterministic reruns and equivalent PreviewSurface/MaterialX images. These pass the 1% limits documented in [Hydra validation](hydra.md). The current analytic InOneWeekend comparison at 256 x 144, 32 spp, and sample clamp disabled does **not** pass: NRMSE is 7.02864%, with 0.0252074% mean-luminance error. Both frontends used the same build and shared camera-ray arithmetic. Artifacts are `build/round2-validation/analytic-weekend-camera-*`; the cause of the remaining pixel differences is unresolved, so full-scene parity acceptance is incomplete. The older tessellated 64-spp result (3.4% NRMSE) is not a like-for-like comparison.

## Historical scene timings

These are single historical renders, not repeated benchmark samples. Cache state and run-to-run variation were not controlled, so they do not establish a speedup or worker-count comparison.

| Scene | Snapshot build | Render | SPP | Meshes | Triangles | Estimated geometry |
|---|---:|---:|---:|---:|---:|---:|
| OpenChessSet | 45.840 s | 0.228 s | 8 | 49 | 1,429,872 | 223,060,032 bytes |
| Kitchen_instanced | 0.567 s | 0.056 s | 2 | 1,788 | 536,913 | 83,758,428 bytes |

The loading table will be filled from the completed three-scene benchmark. Until then, comparative performance acceptance remains open. Fresh relocated-install coverage and motion tests on larger scenes are also pending.

## Interpretation limits

MDL variants remain cached for the renderer process lifetime, affecting warm-recreation results. Historical InOneWeekend measurements used coarse tessellated spheres; the current implementation uses analytic spheres. Report this representation change when comparing geometry memory and rendering results. See [Hydra scope and costs](hydra.md) for rendering limitations and supported geometry behavior.
