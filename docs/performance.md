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

## BSDF table lookups

MDL's layered BSDFs read the SDK's BSDF data tables (energy compensation)
through `tex_lookup_float3_3d` at every layer, and each call inlines
`prism_mdl_lookup_3d` (`shaders/gpu/mdl_runtime.hlsl`). The generic version
handled every wrap and crop mode per axis, with a loop, short-circuit
conditions and a bounds check per texel: in Lion's callable, 77 copies made
70% of the inlined code, and DXC's SSA rewrite of their locals took most of
its 20 s. The rewrite keeps it inlined (out-of-line calls were tried and
reverted, see [gpu_devices.md](gpu_devices.md)) and branch-light: vector
axes, selects for wrap modes and crop checks, no texel bounds checks, and
one branch reading the second and third components of RGB tables only.

Offline DXC 1.8 (`-T lib_6_6 -HV 2021 -spirv -O0`) on the largest callable
of each scene, dumped with `PRISM_GPU_DUMP_PROGRAMS` (best of two runs;
instructions, blocks and locals after `-fcgl` and exhaustive inlining):

| Callable | DXC time | DXC peak | SPIR-V | Inlined instructions / blocks / locals |
| --- | ---: | ---: | ---: | --- |
| Lion | 18.3 → 2.0 s | 1600 → 137 MiB | 2366 → 1290 KB | 325k / 26.5k / 41.9k → 161k / 4.7k / 22.4k |
| Teapot | 1.32 → 0.62 s | 151 → 54 MiB | 527 → 360 KB | 74.7k / 5.8k / 9.4k → 49.2k / 2.4k / 6.4k |
| Chess | 1.19 → 0.51 s | 135 → 46 MiB | 494 → 327 KB | 70.3k / 5.3k / 9.0k → 44.7k / 1.9k / 5.9k |
| Damascus egg | 2.94 → 1.0 s | 326 → 86 MiB | 935 → 600 KB | 145k / 11.2k / 17.8k → 94.2k / 4.4k / 11.7k |

Cold loads with `prism_hydra_render` (RTX 4070 Ti, driver 610.88), 480x270,
4 spp, empty `PRISM_CACHE_DIR`, unique `PRISM_VULKAN_SHADER_SALT`,
`PRISM_DISK_CACHE_GB=60`; medians of 4 runs before and 2 after, alternating.
DXC and driver times are summed over the scene's callables (slowest in
parentheses); the kernel rows are the compute kernel's SPIR-V and pipeline.

| Scene | DXC | DXC peak | SPIR-V | Driver libraries | GPU ready |
| --- | ---: | ---: | ---: | ---: | ---: |
| Lion (1) | 24.2 → 3.5 s | 1600 → 137 MiB | 2366 → 1289 KB | 4.6 → 3.5 s | 31.3 → 19.6 s |
| Teapot (2) | 4.7 (2.4) → 2.7 (1.4) s | 151 → 55 MiB | 1042 → 706 KB | 2.1 (1.0) → 1.6 (0.8) s | 25.3 → 28.5 s |
| Chess (5) | 10.0 (2.5) → 5.0 (1.1) s | 135 → 47 MiB | 2028 → 1357 KB | 3.7 (0.9) → 3.0 (0.7) s | 9.4 → 9.0 s |
| OpenPBR playground (36) | 75.8 (7.4) → 29.5 (1.8) s | 418 → 72 MiB | 13.6 → 8.9 MB | 33.8 (2.5) → 25.2 (1.7) s | 55.1 → 55.1 s |
| Damascus egg (6) | 7.2 (3.8) → 3.5 (1.4) s | 326 → 87 MiB | 2194 → 1440 KB | 3.7 (1.6) → 2.4 (1.1) s | 8.1 → 7.8 s |
| CornellBox (kernel) | SPIR-V 2.8 → 1.6 s | | | pipeline 3.6 → 2.6 s | 7.9 → 5.6 s |
| Parity fixture (kernel) | SPIR-V 1.3 → 1.3 s | | | pipeline 1.9 → 2.0 s | 4.3 → 4.4 s |

Teapot's and OpenPBR's GPU-ready times are dominated by texture preparation
and vary by several seconds between runs; their shader work is lower. The
parity fixture uses no tables.

Time per sample, warm, 960x540: the median GPU pass time of a run, median
of three alternating runs at 64 spp (five at 16 spp):

| Scene | 64 spp | 16 spp |
| --- | ---: | ---: |
| Lion | 7.67 → 7.01 ms | 7.89 → 7.61 ms |
| Teapot | 3.52 → 3.53 ms | 4.12 → 3.49 ms |
| Chess | 21.48 → 21.21 ms | 21.63 → 21.81 ms |

- **Images.** With the GPU rendering every sample, Lion and chess are
  bit-identical to before, Teapot differs in one pixel by 1.5e-6, OpenPBR in
  7 pixels by 3e-8. (Cold runs differ where the CPU preview rendered a
  different number of samples before the handover.) A diagnostic build
  running both versions per lookup found no bit difference on Lion. No
  run had non-finite pixels: 4, 16 and 64 spp, under GPU-AV, and the
  fixture sweep under core and synchronization validation. Vulkan against
  the CPU at 64 spp is unchanged (Lion mean ratio 0.99966, NRMSE 0.0198;
  Teapot 0.99953, 0.0154; chess 0.99817, 0.0560).
- **Variant without the component branch** (always three reads per
  texel): DXC and driver slightly faster (Lion 1.8 s, driver 2.9 s), but 24
  reads instead of 8 for scalar tables; per sample it was within noise
  (Lion 7.50 ms at 64 spp).

## GPU texture streaming (October 2026)

Vulkan streams whole mip levels from feedback instead of uploading every
texture (see [the texture service](texture_service.md#gpu-texture-streaming)).
Measured with `prism_hydra_render` (its settling pre-pass, then 64 spp with
residency held), 960x540, RTX 4070 Ti (driver 610.88), default texture
budget (45% of 12 GB), each process under a 24 GB job cap. **Another
application (a game) was running on the GPU during every run, so all times
are indicative only**; before and after ran alternately. Warm: the mean of
two alternating runs on the same cache; cold: an empty `PRISM_CACHE_DIR`
and a unique `PRISM_VULKAN_SHADER_SALT`, one run each. "Ready" is the
`vulkan.scene` mark, "first sample" the first full sample (with streaming it
includes settling), times since process start. Texture memory is the
images' estimated levels (resident) and allocated device memory;
"device-local" is the process's own use from `VK_EXT_memory_budget`.

| Scene | Texture memory | Ready, warm | First sample, warm | Converged, warm | Ready, cold | GPU ms per pass |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| OpenPBR playground | 5392 MiB, 42 of 152 reduced → 2644 MiB resident, 2687 allocated (device-local 3512) | 14.1 → 8.7 s | 14.3 → 10.0 s | 19.4 → 15.2 s | 59.1 → 61.2 s | 76.1 → 77.9 |
| Teapot | 5334 MiB, 7 of 94 reduced → 615 / 786 MiB (1050) | 3.25 → 2.32 s | 3.40 → 2.63 s | 3.86 → 3.06 s | 34.3 → 39.5 s | 3.56 → 3.38 |
| Lion | 4437 MiB → 769 / 939 MiB (1267) | 3.07 → 2.71 s | 3.23 → 3.11 s | 3.96 → 3.82 s | 20.8 → 23.1 s | 7.42 → 7.17 |
| Chess | 763 MiB → 520 / 691 MiB (977) | 3.01 → 2.99 s | 3.21 → 3.29 s | 4.86 → 4.90 s | 9.1 → 10.7 s | 20.97 → 21.01 |
| Jade egg | 171 MiB (no streamed textures) | 2.31 → 2.27 s | 2.48 → 2.44 s | 4.05 → 3.98 s | 9.6 → 9.2 s | 20.2 → 20.0 |

- **Memory and warm start.** Scenes upload only 128-texel tails (OpenPBR:
  9.7 MiB for 151 images) before the first pass, then what the view asks
  for: OpenPBR needs 2.6 GB at this view, so nothing is reduced any more,
  and GPU ready and the first sample come 4-6 s earlier. Settling takes 2-3
  feedback rounds (OpenPBR 0.8-1.1 s, others 0.1-0.4 s). `nvidia-smi` used
  memory (whole GPU, including the game) during the OpenPBR render: 7814 →
  5114 MiB.
- **Cold.** Slower by 1.5-5 s: preparation now also filters and writes each
  texture's mip chain into its GPU copy (a third more bytes), and the first
  run on a cache rewrites version 1 copies. Single runs; cold times varied
  by up to 1.5x before.
- **Feedback cost.** The same settled residency rendered with feedback
  recorded on every pass (`PRISM_GPU_STREAMING_FEEDBACK=1`, one pixel of
  each 4 x 4 block) against none: Teapot 3.35 / 3.38 ms, Lion 7.47 / 7.26,
  chess 20.89 / 21.23, OpenPBR 77.0 / 76.9 per pass: within the run-to-run
  noise (at most 3%). Offline renders stop recording once residency is held;
  interactive rendering records on every pass.
- **Parity with the CPU device** (64 spp, same seed; 8 x 8 block NRMSE with
  samples clamped at 4, and mean ratio): OpenPBR 0.0162 / 0.9983 →
  0.0164 / 1.0034, Teapot 0.0028 / 0.9995 → 0.0030 / 1.0001, Lion
  0.0029 / 0.9996 → 0.0030 / 1.0004, chess 0.0083 / 0.9982 → 0.0081 / 1.0015,
  jade egg unchanged. Per-pixel NRMSE (OpenPBR 0.077 → 0.078) is dominated by
  paths that diverge between devices, not by texture resolution: an upload
  of every full-resolution level gives the same 0.078. The mean moves
  because all Vulkan images now take the stored box-filtered mips; the blit
  mips were darker (OpenPBR 0.65%); the CPU's OIIO mips lie between.
- **Identity.** With every level resident the streamed image is
  bit-identical to whole images; with the default request rule, OpenPBR
  differs from it in 328 of 518,400 pixels (block NRMSE 0.0003), and with
  exact requests in 39 (see the texture service).
- **Determinism.** Repeated runs are bit-identical on every scene, also with
  a budget the requests do not fit.
- **Interactive policy** (`--progressive-textures`: restart at most once,
  before 16 samples): converged images bit-identical to the settled ones on
  every scene here.

Reduced budget (`PRISM_GPU_TEXTURE_BUDGET_MIB`), against streaming with the
default budget (8 x 8 block NRMSE):

| Scene, budget | Streaming | Whole images reduced to the budget (before) |
| --- | --- | --- |
| OpenPBR, 1500 MiB | 1457 MiB resident, 34 of 148 requested textures below their request; 0.0004 | 123 of 152 images reduced; 0.0174 |
| Teapot, 1500 MiB | everything requested fits; bit-identical | 7 of 94 reduced at the default budget already |
| Teapot, 300 MiB | 60 of 86 below their request; 0.00009 | 93 of 94 reduced; 0.00005 |

Streaming keeps the most-used levels within the budget, so OpenPBR at
1500 MiB stays within noise of the unrestricted image where uniform
reduction does not; Teapot's textures barely show at this view either way.
Runs at a constrained budget are deterministic: residency is planned from
the requests alone while settling.

## Interpretation limits

MDL variants remain cached for the renderer process lifetime, affecting warm-recreation results. Historical InOneWeekend measurements used coarse tessellated spheres; the current implementation uses analytic spheres. Report this representation change when comparing geometry memory and rendering results. See [Hydra scope and costs](hydra.md) for rendering limitations and supported geometry behavior.
