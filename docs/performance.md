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

## Render profile

`PRISM_RENDER_PROFILE=1` times the stages of the CPU render loops
(`src/render_profile.h`) and prints a table when the render ends: the CLI
after its `[PrismProfile]` line, `prism_hydra_render` at exit (every render
of the process). Each row is a stage's calls, summed worker elapsed time,
time per call and share of the `sample (total)` row:

| Stage | What it times |
| --- | --- |
| sample (total) | one path sample of a pixel; everything below except film, adaptive check and denoise |
| intersection, hit preparation | the closest-hit Embree query of a path segment and the `RayHit` built from it |
| NEE light choice | the light pick and light sample of next-event estimation (includes `emitter radiance`) |
| NEE shadow ray | the whole shadow test (`visible`), including the shadow rows below |
| shadow intersection, shadow hit preparation, shadow cutout | the Embree queries, hit preparation and cutout opacity of shadow-ray crossings |
| MDL prepare (init) | `perturbNormal` and the material's `init` (all texture results) at a vertex |
| MDL sample, MDL evaluate, MDL medium | the BSDF sample (with cutout, thin-walled and emission), evaluate, volume coefficients |
| emitter radiance | a textured emitter's emission at a light sample |
| texture lookup, BSDF table lookup | the MDL texture runtime's 2D (OIIO) and BSDF data-table lookups |
| environment | dome radiance, sampling, pdf and lobes |
| film | accumulating the samples into the image (CLI: the final resolve; Hydra: each pass's commit) |
| adaptive check | the convergence checks of adaptive sampling |
| denoise | the denoiser (CLI: the final denoise; Hydra: the denoise worker's jobs) |

The sums are worker elapsed time, not CPU or wall time: with 28 threads a
1 s render sums to about 28 s. Stages nest (a texture lookup inside an MDL
prepare counts in both), so shares do not add up to 100%. The clocks cost
about 20-40 ns per timed call, which inflates the short stages (BSDF table
and texture lookups are called tens of millions of times); compare the
shares of one build's profile, and measure gains with wall time and
alternating runs. Profiling off, each timed scope is one thread-local null
check: images are bit-identical with it on or off, and render times are
within run-to-run noise of a build without it. Only the CPU device is timed
(the GPU devices' kernels run on the GPU); the shared integrator's two hooks
(`PRISM_PROFILE` in `shaders/integrator/path_tracer.h`) expand to nothing
in HLSL.

## CPU shadow rays through cutouts (October 2026)

Two changes to shadow rays in scenes with cutouts (glTF MASK/BLEND, alpha
cards, `displayOpacity`), measured on the CPU at 256 spp against the previous
build, alternating, median of 3 (`--adaptive-threshold 0`):

- **Opaque primitives first** (`src/device/opaque_occluders.h`). Each
  placement is classified when the CPU device prepares a scene: opaque,
  not opaque (a cutout, or outside a linked light's shadow-link categories),
  or mixed with a flag per primitive (the merged mesh of a glTF or OBJ file).
  A shadow ray's closest hit decides most rays (a miss is visible, an opaque
  primitive blocks); after a cutout hit, an Embree any-hit query with an
  argument filter looks for an opaque primitive behind it before any cutout
  is evaluated. Culled back faces (`prism:honorDoubleSided`) are skipped by
  the filter. Scenes with no cutouts keep the plain `rtcOccluded1`
  (bit-identical). Images: same expectation, other noise (cutouts in front of
  an opaque blocker no longer draw random numbers), as on the GPU.
- **A cutout's own init** (`mdl::MaterialFunctions::cutout_init_function_index`).
  Programs whose cutout varies over the surface get a second single-init
  group of the material, `init` and `geometry.cutout_opacity` only: a shadow
  crossing runs the lookups the cutout reads (and the shading normal, as the
  shared init computes it) instead of every texture of the material.
  Bit-identical images; native JIT time unchanged within noise (foliage
  fixture, 0.23-0.27 s either way).

| Scene | Render time, new / old | NEE shadow ray share (old -> new) | Cutout per crossing |
| --- | --- | --- | --- |
| `tests/hydra/fixtures/foliage_cards.usda` (40 textured leaf cards, wall) | 0.87 | 23.3% -> 12.5% | 5.4 -> 1.6 us |
| the same with an opaque awning over half of it (not in the repo) | 0.82 | 21.0% -> 4.7% | 0.6 M of 4.9 M crossings left |
| `tests/gltf/features.gltf` (dome only) | 1.04 (noise) | 1.8% -> 2.1% | - |
| `tests/obj/textured.obj` (dome only) | 0.99 | 2.1% -> 2.4% | - |
| CornellBox (no cutouts, unchanged code path) | 0.95 (noise) | - | - |

Run-to-run noise was about 5% (CornellBox runs the same code in both).
Where shadow rays are a few percent of the frame (the glTF and OBJ
fixtures), nothing changes measurably; the gains are in scenes whose shadow
rays cross cutouts. The opaque-first query alone gave the awning variant
0.76 and the foliage fixture nothing (its shadow rays have no opaque blocker
behind the leaves); the cutout's own init gave the foliage fixture 0.85.
Convergence (1024 spp, two seeds of each build): the cross NRMSE between the
builds equals the noise floor of either (ratio 0.999-1.000 on the foliage,
awning, glTF and cutout fixtures), the mean luminance agrees to 1e-5, and the
mean over the cutout edges (the 10% largest image gradients) differs by less
than the two seeds of the old build do. CPU-vs-Vulkan NRMSE is unchanged
(foliage 0.0196, awning 0.0019, glTF 0.0100 for old and new).

## Scene loading, batch 3 (October 2026)

CPU only (`PRISM_DISABLE_VULKAN=1`), 320x180, 4 spp, `--seed 0
--adaptive-threshold 0`, each run under a 24 GB job cap. The previous build
and the new one ran in alternating rounds in one session; medians, 2 cold
rounds (a fresh `PRISM_CACHE_DIR` per run) and 5 warm. Hydra times are
`prism_hydra_render` with `PRISM_LOAD_PROFILE=1`; CLI times are the
`[PrismProfile]` `loadSeconds` and `renderSeconds` (the render includes the
CPU device's scene preparation). Every image of the new build is
bit-identical to the old build's (104 EXR comparisons, the countertop 7 times
in each loader).

- **Dense per-triangle material tables** (item A). A per-triangle hash map,
  copied per placement, became a table shared by a prototype's placements.
- **Parallel MaterialX compiles in the SDK** (item C): glTF, OBJ and
  API/Python scenes.
- Memoised ancestor walks in the Hydra snapshot build (item B) were not
  done: the whole walk was under 2% of the load on 4,096 and 16,384 mesh
  prims (0.15 s of 22 s, 0.74 s of 44 s); the scene capture took 7.7 s and
  30 s there (about 1.8 ms per prim).

| Scene | Measure | Cold, new / old | Warm, new / old |
| --- | --- | ---: | ---: |
| 16 instances of a 980k-triangle mesh with a GeomSubset (synthetic) | Hydra first preview | 0.36 (3.13 -> 1.11 s) | 0.34 (3.60 -> 1.24 s) |
| | Hydra walk / device prepare / spare bank | 0.02 / 0.02 / - | 0.03 / 0.02 / 0.04 |
| | CLI load / render | 0.50 / 0.03 | 0.55 / 0.02 |
| MaterialEggs countertop (14 GeomSubsets) | Hydra walk / spare bank | 0.77 / - | 0.68 / 0.34 |
| | Hydra first preview, CLI load | 0.98, 0.95 | 1.00, 0.97 |
| | CLI render | 0.81 | 0.90 |
| `tests/gltf/features.gltf` (11 materials) | CLI load | 0.43 (11.5 -> 5.0 s) | 0.43 (12.6 -> 5.4 s) |
| 48-material OBJ, 6 structures (synthetic) | CLI load | 0.73 | 0.83 |
| `tests/gltf/metal_rough_spheres.gltf` (8 materials, one class) | CLI load | 0.99 | 0.95 |
| Chess (no subsets, USD materials) | Hydra first preview, CLI load | 0.97, 1.02 | 1.10, 0.97 (noise) |

On the countertop the material phase dominates the load (about 25 s), so the
walk's 0.7-0.9 s saving and the occluder/light-table scans of the device
preparation (0.07 s -> 0.002 s) stay within the run-to-run noise of the first
preview. Glossy materials of one class (metal_rough_spheres) share one
program, so they compiled quickly already; the gain is with several
structures (features.gltf). The first MaterialX material of a commit still
compiles alone, since it loads the MDL modules every generated material
imports.
## Hydra scene capture (2026-10-31)

CPU only (`PRISM_DISABLE_VULKAN=1`), `prism_hydra_render` at 160x90, 4 spp,
`--seed 0 --adaptive-threshold 0`, each run under a 24 GB job cap,
`PRISM_LOAD_PROFILE=1`. The previous build (main) and the new one ran in
alternating rounds in one session; medians of 3. Synthetic stages: N cube
meshes (one Xform each, under two group levels, 8 UsdPreviewSurface
materials), and N instanceable Xforms of a two-mesh class. Capture is the
`capture_ms` of the `capture.done` mark (time inside Prepare); wall is
`capture.start` to `capture.done`, which also counts the host's render calls
and 10 ms polls between slices.

| Scene | Hydra prims | Capture ms, old -> new | Ratio | Capture wall, s | First preview, s |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1,024 meshes | 2,132 | 264 -> 23 | 0.09 | 0.32 -> 0.02 | 1.27 -> 1.41 (noise) |
| 4,096 meshes | 8,480 | 1,030 -> 89 | 0.09 | 1.24 -> 0.10 | 2.43 -> 1.45 |
| 16,384 meshes | 33,872 | 8,974 -> 350 | 0.04 | 10.87 -> 0.43 | 14.19 -> 2.79 |
| 1,024 instances | 1,115 | 51 -> 15 | 0.29 | 0.06 -> 0.02 | 1.15 -> 1.10 |
| 4,096 instances | 4,391 | 206 -> 53 | 0.26 | 0.25 -> 0.06 | 1.27 -> 1.19 |
| 16,384 instances | 17,495 | 888 -> 201 | 0.23 | 1.06 -> 0.22 | 2.84 -> 1.97 |
| MaterialEggs countertop | 274 | 597 -> 184 | 0.31 | 0.70 -> 0.21 | material-bound (~23-28 s, noise) |
| Chess | 151 | 272 -> 84 | 0.31 | 0.33 -> 0.10 | 3.53 -> 4.08 (noise) |

The old capture grew faster than the prim count (0.12 ms per prim at 8k,
0.27 ms at 34k); the new one stays about 0.01 ms per prim. A per-name timer
of the old capture (16,384 meshes) put the time in UsdImaging's data sources
during the copy: `primvars` 1.3 s, `usdMaterialBindings` 1.35 s,
`materialBindings` 1.2 s, `mesh` 0.9 s, `__usdPrimInfo` 0.34 s, the rest under
0.2 s each; the queue, record map and retained-index inserts were under 0.1 s.
One chess round of the new build took 591 ms while the MDL prewarm held the
workers. Every image (36 synthetic, 12 countertop/chess with a cold first
round, and 3-step transform edit sequences on 4,096 meshes, 4,096 instances
and a countertop egg) is bit-identical to the old build's.

## GPU pass pipeline, guides and edits (2026-10-31)

Performance batch 3, GPU part (Vulkan, RTX 4070 Ti, 1920x1080 unless noted,
`--adaptive-threshold 0 --seed 0`). Each pair alternates the previous build
(baseline) and the new one in one session; the per-pass numbers are
`PRISM_GPU_VERBOSE`'s "GPU pass" lines averaged over a render's passes
(`prism_hydra_render`). All final images are bit-identical to the previous
build's (`prism_compare_images --exact`, colour and `denoised` layers, cold
and warm caches), except one cold InOneWeekend pair whose baseline handed
over from the CPU preview after one sample and the new build after none
(timing; the new image equals the warm baseline's bit for bit).

**Pipelined pass chunks** (`GpuRenderDevice::RenderPass`, Vulkan
`submitChunk`/`waitChunk`). A pass traces 256k-pixel chunks; each used to be
submitted, waited for and read back before the next was built, so the GPU
idled during the readback, the list build and the submission. Profile of the
previous build: on CornellBox a pass's wall time was about twice its GPU time
(27 ms against 14 ms in one session, 11 ms against 5 ms in another). Now two
chunks are in flight: each chunk's command buffer updates the frame
(`vkCmdUpdateBuffer`) and copies its part of the pass's pixel list into
`kSlotPixels` behind a barrier on the kernels before it, then runs the
kernel; the host reads chunk k back while chunk k+1 runs. The pass list is
also built by rows (`std::iota`) when no adaptive mask applies.
`PRISM_GPU_PASS_CHUNKS_IN_FLIGHT=1` restores one at a time (diagnostics).

| Scene | Pass wall, ms (old -> new) | Ratio | Render time, s (old -> new) |
| --- | --- | --- | --- |
| CornellBox, 64 spp | 11.24 -> 11.11, 11.84 -> 10.46 | 0.99, 0.88 | 1.50 -> 1.51, 1.61 -> 1.48 |
| InOneWeekend, 64 spp | 31.13 -> 25.22, 30.59 -> 25.33 | 0.81, 0.83 | 3.21 -> 3.05, 3.08 -> 2.72 |
| OpenChessSet, 32 spp (texture streaming on) | 45.01 -> 36.31, 43.44 -> 35.31 | 0.81, 0.81 | 3.18 -> 2.71, 3.12 -> 2.80 |
| QuadTest, 64 spp (with the row-built list) | 11.01 -> 7.66 | 0.70 | 1.45 -> 1.24 |
| QuadTest, adaptive 0.02, 128 spp | 5.89 -> 3.96 | 0.67 | 1.95 -> 1.71 |

The first three rows were measured before the row-built list; on QuadTest,
one against two chunks in flight in the new build gave 10.12 -> 7.70 ms,
so the overlap is about 2.4 ms of the 3.4 ms gained. CornellBox's GPU work
is about 5 ms a pass, less than the host's readback and list build, so it
gains little: those passes are host-bound. The "GPU" time the log sums
overlaps between chunks (the next chunk's timestamp can start before the
barrier releases it), so it reads 10-25% higher than before.

**Guides-only light AOVs** (`kFrameGuidesOnly`). Denoising needs the albedo
and normal, but every GPU sample also wrote the diffuse, specular and
emission AOVs to host memory, four interleaved `float4` per pixel (CLI: read
and added in place every sample). Profile of the previous build: denoising
raised the CLI's GPU time from 6.1 to 11.2 s (CornellBox, 128 spp) and 6.1 to
13.1 s (InOneWeekend), and the Hydra pass wall from 27 to 58 ms (CornellBox).
When no light AOV output is bound, the light AOV kernel variant now stores
only the guides, in two planes of one `float4` per pixel, at a frame flag
(no new pipeline). The first version kept the entries interleaved (two per
pixel) and was slower than four (CornellBox CLI GPU 11.4 -> 13.9 s, and
QuadTest 11.45 -> 13.6 s with the same binary, on against off): stores to
host memory that a SIMD group does not write contiguously cost more than
their size. The planes made it 0.56x.

| Case | Old -> new | Ratio |
| --- | --- | --- |
| QuadTest CLI, 128 spp, GPU s (same binary, guides off -> on) | 11.53 -> 6.47 | 0.56 |
| InOneWeekend CLI, 128 spp, GPU s | 12.66 -> 8.49 | 0.67 |
| OpenChessSet CLI, 64 spp, GPU s | 6.38 -> 3.98, 6.51 -> 4.01 | 0.62, 0.62 |
| OpenChessSet Hydra, 32 spp, pass wall ms | 72.6 -> 55.2, 66.6 -> 56.1 | 0.76, 0.84 |
| vk_oidn accumulation resolve and filter, GPU ms (CLI) | 90 -> 74 | 0.82 |

The denoised images are unchanged (the guides are the same sums). Light AOV
outputs that are bound keep the four-entry layout. The same planar layout
for the full light AOVs, and for the CLI's colour sums (two interleaved
`float4` per pixel, read and written in host memory every sample), is
likely a gain too (not done: the Metal denoiser and the accumulation layout
would change; to measure).

**Geometry kept on Vulkan edits** (`gpu::SameGeometry`, as on Metal). Edits
that keep the geometry (transforms, materials, lights) keep the geometry buffers and bottom-level structures; only the
other buffers and the top level are rebuilt. OpenChessSet, 960x540 4 spp,
three transform edits of `/ChessSet/White/King` per run, `vulkan.scene`
load-profile marks, 4 pairs (one cold): scene update 0.267 -> 0.231 s mean
(pair ratios 0.75, 0.84, 1.03, 0.88), its geometry buffers and structures
0.115 -> 0.061 s. CornellBox: 0.009 s either way. What remains is
`BuildGpuScene` (0.14 s on the chess set: it converts the whole scene again)
and the other uploads (about 0.06 s, mostly the dome's 4k importance tables,
rebuilt and uploaded on every edit): the next candidates for edit latency.

## Interpretation limits

MDL variants remain cached for the renderer process lifetime, affecting warm-recreation results. Historical InOneWeekend measurements used coarse tessellated spheres; the current implementation uses analytic spheres. Report this representation change when comparing geometry memory and rendering results. See [Hydra scope and costs](hydra.md) for rendering limitations and supported geometry behavior.
