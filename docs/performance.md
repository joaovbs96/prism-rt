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

## Loading profile (October 2026)

Measured with `PRISM_LOAD_PROFILE=1` (see [Hydra loading measurements](hydra.md#loading-measurements))
through `prism_hydra_render` at 480x270, 4 spp, on the i7-14700K / RTX 4070 Ti,
each process under a 24 GB job-memory cap. Warm means a second run on the same
private `PRISM_CACHE_DIR`. The baseline and the changed plugin ran in
alternating rounds; the table gives the time from process start to the first
published image, each a two-round mean (Vulkan: one round).

| Scene, warm | CPU before | CPU after | Ratio | Vulkan before | Vulkan after | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| MaterialEggs countertop (35 materials) | 25.2 s | 13.5 s | 0.54 | 30.4 s | 18.9 s | 0.62 |
| OpenPBR shader playground | 13.4 s | 7.6 s | 0.57 | | | |
| Stirling | 7.5 s | 5.0 s | 0.67 | 9.5 s | 6.8 s | 0.72 |
| Egg jade | 3.8 s | 1.9 s | 0.51 | 4.8 s | 3.0 s | 0.62 |
| Teapot (UDIM textures) | 4.7 s | 2.5 s | 0.53 | 15.8 s | 9.6 s | 0.61 |
| Chess | 3.5 s | 2.6 s | 0.75 | 5.0 s | 4.1 s | 0.82 |

In usdview (the real launcher, warm caches, Vulkan) the countertop's first
image went from 26.9 s to 14.9 s. The countertop, playground and Stirling
kernels exceed DXC's memory limit, so their Vulkan runs render on the CPU.

What the timeline showed before the change, and what changed:

- Material conversion through hdMtlx ran serially on the build thread: about
  0.6 s per material (the countertop's 34 conversions took 19-21 s of its 25 s
  material phase; hdMtlx imports and validates the whole standard library for
  every document, which PrismRT cannot avoid without patching USD). It now runs
  on the material workers, interleaved with compilation, largest graphs first.
  Conversions slow down about 1.8x each when eight run at once, so the gain is
  about 2.5x on the material phase.
- UDIM tiles were read, hashed and published one at a time (2.2-2.9 s on the
  teapot and the playground, warm); they are now handled in parallel (0.45 s
  on the teapot).
- Mesh conversion and subdivision started only after every material had
  compiled; now meshes whose materials cannot displace convert meanwhile
  (0.2-1 s; the countertop's displaced meshes still wait).
- Native MDL code generation (LLVM) remains the largest material cost (12-15 s
  of worker time on the countertop and the playground). The MDL SDK 2026.0.2
  native back end does not support `ITarget_code::serialize`
  (`supports_serialization()` is false), so it cannot be cached on disk; more
  than eight workers did not help (14 workers: same wall time, twice the
  per-material JIT time).
- Cold runs are dominated by texture preparation (teapot: 30 s, mostly the
  UDIM `.tx.exr` creation inside one material's compile) and, on Vulkan, by
  DXC (9-23 s, cached afterwards) and driver pipeline compilation (up to 17 s
  for ALab's kernel, now cached in `.vkpc` files).
- Teapot on Vulkan spent 6.5-8.9 s per warm run decoding its 94 prepared
  float textures (5.6 GB) into GPU formats; GPU-ready copies now remove most
  of it (see below).
- The usdview "discarded first build": usdview edits its default light about
  1.2 s after population, which cancels the first generation. The second
  generation reuses the conversions and programs the first finished, so this
  costs about 1 s now.

## Texture pipeline v2 (October 2026)

Same setup as above (480x270, 4 spp, 24 GB cap), process wall time; cold is
an empty `PRISM_CACHE_DIR` (Vulkan cold includes DXC and driver compiles),
warm the next run on that cache. Single rounds on a machine in use; repeated
identical cold runs varied up to 1.5x. See
[the texture service](texture_service.md#preparation-pipeline).

| Scene | CPU cold | CPU warm | Vulkan cold | Vulkan warm |
| --- | ---: | ---: | ---: | ---: |
| Teapot (94 textures, UDIM) | 30.3 s → 11.8 s | 3.4 s → 2.8 s | 40.5 s → 15.0 s | 12.2 s → 4.9 s |
| Lion (38, UDIM) | 23.1 s → 10.0–12.9 s | 4.4 s → 3.3 s | 53.5 s → 28.3 s | 9.5 s → 4.9 s |
| Chess (43) | 5.4 s → 4.9 s | 3.9 s → 3.3 s | 10.0 s → 7.9 s | 5.4 s → 4.3 s |
| OpenPBR playground (152) | 49.1 s → 25.2 s | 10.6 s → 8.4 s | 85.2 s → 48.1 s | 34.5 s → 17.0 s |

- UDIM tiles are prepared in parallel (Teapot CPU cold 30 s → 12 s, Lion
  23 s → 11 s); they were prepared one after another inside one material's
  compile.
- Vulkan uploads read the GPU-ready copies instead of decoding the float
  `.tx.exr` files: image preparation 7.4 s → 1.7 s warm on Teapot, 18.2 s →
  3.5 s on OpenPBR. Image memory went from 5611 to 5334 MiB (Teapot) and 6694
  to 5392 MiB (OpenPBR): images reduced for the budget stay 8-bit, and the
  budget counts 16-bit textures at two bytes per component, so more images
  are reduced (Teapot 4 → 7 of 94, OpenPBR 38 → 42 of 152) and the budget is
  now actually respected. 8-bit sources already uploaded as 8-bit (every
  MaterialX texture is prepared with gamma 1), and RGB still needs RGBA
  storage (the RTX 4070 Ti has no sampled RGB8 format).
- Background `.tx.exr` writes: Teapot Vulkan cold 0.91x in three alternating
  rounds (CPU cold unchanged within noise).
- Warm loads no longer read and hash every source (OpenPBR 1.7 GB, 3.2 s →
  0.15 s; Teapot 0.6 s → 0.03 s).
- Vulkan cold loads are now dominated by DXC (Lion 18 s) and, on OpenPBR, by
  MDL-to-HLSL translation and pipeline creation.

## GPU preparation overlapped with the material phase (October 2026)

The Vulkan device used to start its own work only once the scene was
published: MDL-to-HLSL translation of one link unit, then DXC and pipeline
libraries, while images uploaded. Now (see [gpu_devices.md](gpu_devices.md#how-it-fits-together)):

- every MDL class is translated alone with the HLSL backend on the material
  worker right after class compilation (0.15-0.4 s each, in parallel across
  workers instead of serially at the end; kept in the disk cache, so warm
  loads translate nothing);
- once the programs seen make the compute kernel certainly too large, each
  program's callable compiles (DXC, then its pipeline library) on background
  compile threads while other materials and textures are still prepared;
  the scene's pipeline links what is ready;
- textures upload from their GPU copies as they are prepared, up to the
  texture budget; the scene's upload takes them and uploads the rest.

Same setup as above (`prism_hydra_render`, 480x270, 4 spp, 24 GB cap,
`PRISM_DISK_CACHE_GB=60`); cold is an empty `PRISM_CACHE_DIR` for either
device and a unique `PRISM_VULKAN_SHADER_SALT`, warm the next run on that
cache (valid only with zero textures prepared and zero DXC runs). Medians of
three alternating before/after rounds. The machine was busier than for the
tables above (Teapot CPU cold 30 s against 12 s then), so compare ratios.
"GPU ready" is the `vulkan.scene` mark; "tail" is GPU ready minus the
snapshot's publication.

| Scene | Vulkan cold: GPU ready | tail | Vulkan warm: GPU ready | tail | CPU cold | CPU warm |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Teapot | 33.0 → 29.9 s (0.90) | 4.0 → 0.7 s | 4.6 → 3.4 s (0.73) | 2.3 → 0.8 s | 29.7 → 30.5 s | 2.4 → 2.6 s |
| Lion | 44.6 → 34.7 s (0.78) | 25.5 → 13.6 s | 4.4 → 3.1 s (0.72) | 1.7 → 0.3 s | 17.5 → 18.0 s | 2.8 → 2.8 s |
| Chess | 11.6 → 9.5 s (0.82) | 4.4 → 1.3 s | 3.8 → 3.1 s (0.81) | 0.9 → 0.2 s | 6.9 → 6.9 s | 3.1 → 3.1 s |
| OpenPBR playground | 78.3 → 58.9 s (0.75) | 26.8 → 5.6 s | 18.5 → 13.8 s (0.75) | 11.3 → 5.7 s | 50.8 → 53.9 s | 7.9 → 7.8 s |
| Jade egg | 8.3 → 8.1 s (0.97) | 5.5 → 4.8 s | 3.1 → 2.4 s (0.79) | 0.8 → 0.2 s | 2.9 → 3.0 s | 2.3 → 2.4 s |
| Parity fixture (compute kernel) | 4.5 → 4.6 s | 3.6 → 3.5 s | 1.0 → 1.0 s | 0.1 → 0.1 s | 1.0 → 1.0 s | 1.0 → 1.0 s |

CPU times are the first full sample (`frame.full_sample`); the CPU device
runs none of the new code, and its differences are within the run-to-run
spread (up to 1.5x on cold runs here).

- **What still serializes.** The lion's one callable takes DXC 25-27 s
  next to the texture preparation (20 s alone): it starts 3-5 s into the
  material phase and still ends 13 s after publication. On the jade egg and the parity fixture the tail is the
  raygen (or compute kernel) pipeline that a cold driver compiles in 5-9 s
  (unique salt). OpenPBR's remaining 5.6 s is the scene's own image upload
  (3.8 s: 53 of 152 images left to the scene, 33 of them reduced by the
  budget after an early full-resolution upload) and the GPU scene
  conversion. Megakernel scenes still compile only after the scene is known.
- **Warm.** The tail is now the image upload of whatever the early uploads
  did not take and the GPU scene conversion; warm material phases are
  0.0-0.6 s longer than before (the early translations read from the disk
  cache).
- **Images.** Bit-identical to before on every scene with the CPU preview off
  (GPU samples only), except OpenPBR: its programs translated alone round
  slightly differently from its link unit, and 4% of pixels diverge
  (NRMSE 0.019 at 4 spp, 0.0073 at 64 spp, mean luminance 0.0009% at 64 spp).
  CPU images are identical. With the preview on, the before and after images
  differ by how many samples the CPU rendered before the handover.

## Interpretation limits

MDL variants remain cached for the renderer process lifetime, affecting warm-recreation results. Historical InOneWeekend measurements used coarse tessellated spheres; the current implementation uses analytic spheres. Report this representation change when comparing geometry memory and rendering results. See [Hydra scope and costs](hydra.md) for rendering limitations and supported geometry behavior.
