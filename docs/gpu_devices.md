# Render devices: CPU, Metal and Vulkan

PrismRT renders through a *device*. Every device runs the same path tracer,
`shaders/integrator/path_tracer.h`, against its own backend:

| Device | Select with | Backend | Status |
| --- | --- | --- | --- |
| `metal` | `--device metal`, `PRISM_DEVICE=metal`, `prism:device` | HLSL kernel: DXC to DXIL, Metal shader converter; inline ray queries | macOS on Apple GPUs; ray-tracing hardware from M3 (M1/M2 run the same API in software) |
| `vulkan` | `--device vulkan`, ... | The same HLSL kernel, compiled to SPIR-V by DXC; `VK_KHR_ray_query` | Windows/Linux with NVIDIA RTX, AMD RX 6000+ or Intel Arc. Builds and compiles its kernel on macOS; not yet run-tested |
| `cpu` | `--device cpu`, ... | The same integrator compiled as C++, on Embree and the CPU materials (native MDL) | All platforms |
| `cpu-reference` | `--device cpu-reference`, ... | The original `SceneManager::trace` | Validation reference, bit-identical to the renderer before devices existed |

`auto` (the default) picks Metal on macOS and Vulkan elsewhere, and falls
back to the CPU device with a message when no GPU device is usable.
`PrismRT --list-devices` lists what this machine offers.

## How it fits together

- **One integrator.** [`shaders/integrator/path_tracer.h`](../shaders/integrator/path_tracer.h)
  is `PathTracer<Backend, Hit, Shading>`, written in the common subset of
  C++ and HLSL 2021 (structs without constructors, aggregate
  initialization, `(float3)x` for scalar-to-vector conversion, no swizzles,
  references or pointers; see `types.h`). The CPU device instantiates it with
  `CpuBackend` (`src/device/cpu_backend.h`; HLSL types and intrinsics for
  C++ come from `src/device/hlsl_math.h`), the GPU kernel
  (`shaders/gpu/kernel.hlsl`) with `GpuBackend`
  (`shaders/gpu/gpu_backend.hlsl`). Random numbers are drawn in the original
  order, so devices agree sample for sample up to floating-point and
  intersection differences. The CPU device matches `cpu-reference` far
  below the noise floor; the remaining difference comes from its
  single-precision camera rays.
- **Explicit path state.** What a path carries between bounces is one
  struct, `PathState` (ray, sampler, radiance, weights, the previous
  vertex's MIS data, medium, depth and flags), advanced by step functions:
  `extend` (trace the segment, media, crossed lights, escape), then at a
  surface `chooseLight` (the next-event light sample), `backend.shade` (the
  material runs once: BSDF sample, BSDF towards the light, medium),
  `arrive` (emission, cutout crossings), `connectLight` (shadow ray) and
  `continuePath` (roulette, next segment). The light sample needs nothing
  from the material, so it is drawn first and one material invocation
  answers both the BSDF sample and the light evaluation; a cutout crossing
  rewinds the sampler to before the unused light sample. Random numbers are
  drawn in the original order: images are bit-identical to the single-loop
  integrator on the CPU and Vulkan devices.
- **One GPU kernel.** Metal and Vulkan compile the same HLSL; only
  `shaders/gpu/metal/resources.hlsl` (descriptor heaps) and
  `shaders/gpu/vulkan/resources.hlsl` (descriptor set bindings) differ.
- **GPU scene.** `src/device/gpu/gpu_scene.*` flattens a `SceneManager` into
  shared records (`shaders/shared/prism_gpu_types.h`): one bottom-level
  acceleration structure per prototype (triangles; analytic spheres as
  procedural boxes), one instance per placement, per-primitive materials, the
  legacy materials and textures, and the environment with its importance
  tables.
- **MDL on the GPU.** `src/device/gpu/gpu_materials.*` compiles the scene's
  MDL classes again with the HLSL backend into one link unit, with a
  generated dispatch function per entry point. Materials keep their native
  argument blocks (the shading-language backends use the native layout);
  only texture indices are remapped. The MDL code comes from the SDK's HLSL
  backend for both devices. Classes that differ only in names
  (MaterialX node names become parameter names) share one program: the 15
  OpenChessSet materials compile as 4.
- **No SDK fork.** Apple GPUs lack FP64. `hlsl_fp32_rewrite.*` rewrites the
  generated HLSL to single precision; on FP64 test code its output is
  byte-identical to the `hlsl_disable_double` option of our MDL SDK fork.
- **Geometric outputs** (primary AOVs, picking) are traced on the CPU scene,
  which is exact and already present.

## Shader compilation and caches

GPU shading languages inline every call, so the kernel contains each
material's code once per call site; the integrator therefore calls every
material entry point from exactly one place, and the kernel entry calls
`renderSample` once. Even so, SPIR-V legalization of a kernel with many
materials needs more than the default 4M ids, so the Vulkan device passes
`-fspv-max-id` to DXC (the final module's ids are compacted).

If the Vulkan device cannot prepare a scene (its kernel fails to compile,
its resources do not fit), it prints the reason once and renders that scene
on the CPU device until the scene changes.

| OpenChessSet (4 MDL programs) | Time |
| --- | --- |
| First run: MDL codegen + DXC (`-Od`) + Metal shader converter + Metal pipeline | about 30 s |
| Later runs (metallib cached in `~/Library/Caches/PrismRT/metal`, pipeline in Metal's system cache) | about 1.2 s shader setup, 2.3 s scene setup in total |

The Vulkan device caches its SPIR-V the same way (`.../PrismRT/vulkan`, or
`PRISM_CACHE_DIR/vulkan`). Next to each kernel it keeps the driver's
pipeline-cache data (`<SPIR-V digest>-<pipeline cache UUID>-<vendor>-<device>-<driver version>.vkpc`),
so creating the pipeline does not depend on the driver's own shader cache,
which other applications share and may evict: an evicted 2 MB kernel took 8 s to
create on an RTX 4070 Ti, against 0.01-0.05 s from either cache. A driver
update or another GPU reads its own file; the driver checks the data header
again. Both files are written to a temporary name and renamed, so processes
sharing the cache directory never read a partial file. Uploaded textures
are reused across Hydra scene rebuilds.

## Material programs as callable shaders (Vulkan)

One inlined kernel stops compiling at about 1 MB of HLSL: DXC's SPIR-V
legalization of the single function needs more than 8 GB, and the NVIDIA
driver's compile of unlegalized SPIR-V more than 12 GB and 400 s. Below that
size it compiles, but slowly (OpenChessSet: 41 s cold) and, for all but the
smallest material sets, runs slower than separate programs. The Vulkan
device therefore does not inline the materials of most scenes; it builds a
ray tracing pipeline (`VK_KHR_ray_tracing_pipeline`,
`src/device/vulkan/vulkan_material_calls.*`):

- **Raygen shader.** `kernel.hlsl` compiled with `PRISM_CALL_MATERIALS`:
  the same integrator and step functions, legacy materials, lights and
  environment, but no MDL code. `GpuBackend::shade` packs a `MaterialCall`
  (`shaders/gpu/material_call.hlsl`) and calls the hit's MDL program with
  `CallShader(program, call)`; `cutoutOpacity` of a varying cutout does the
  same in its own mode. It depends on no scene (with reordering, on the
  hint bit count only), so the device compiles it in the background from
  its creation, while the first scene loads.
- **One callable per MDL program.** `shaders/gpu/vulkan/material_callable.hlsl`
  with that program's dispatch (`MdlShaderCode::programDispatch`) and only
  the functions of the scene's link unit it reaches (a top-level split of the
  generated HLSL, `LinkUnitTrimmer`; DXC would drop the others anyway, the
  SPIR-V is identical). The program's index is left out of its source (fixed
  function names and dispatch case: only that program reaches the callable),
  so a program compiles to the same callable in any material set whose
  link unit gives it the same code. It runs the megakernel's own code: the
  program's init on the prepared shading point, then `GpuBackend::shade`
  (BSDF sample, BSDF towards the light sample, medium). With the integrator's
  step functions that is one call per surface bounce.
- **Intersection stays in inline ray queries** inside the raygen shader, not
  `TraceRay`: the hit shading is the material, which is already a callable;
  the query loop (including the analytic sphere test) is the megakernel's,
  so both paths trace identical rays; and no hit groups, intersection
  shaders or payload for hits are needed. Shader execution reordering works
  without hit objects (a hint is enough).
- **Shader execution reordering** (opt-in, `PRISM_VULKAN_REORDER=1`). With
  `VK_NV_ray_tracing_invocation_reorder`, the raygen shader reorders threads
  by program (`OpReorderThreadWithHintNV`, hint bits for the program count)
  just before the call.
- **Compilation.** Each callable and the raygen shader is its own DXC run
  (`lib_6_6`, cached by source in the SPIR-V cache) and, with
  `VK_KHR_pipeline_library`, its own pipeline library, compiled by the
  driver right after its DXC run on the same thread: DXC of one shader
  overlaps the driver compile of others. Up to `PRISM_VULKAN_COMPILE_THREADS`
  shaders (default min(16, hardware threads)) compile at once, with up to
  `PRISM_DXC_JOBS` DXC processes (default half the hardware threads, at most
  12 and one per 2 GiB of memory; each capped by `PRISM_DXC_MEMORY_MIB`).
  The background raygen compile and its DXC run above normal priority.
  Libraries stay in memory for the device's life, each with its own
  `VkPipelineCache` file (`<SPIR-V digest>-lib-<pipeline cache UUID, device,
  driver>.vkpc`); a scene's pipeline links the raygen library and its
  programs' libraries (10–60 ms), so a new scene or an edited material set
  compiles only the callables the device has not seen. The pipeline compiles
  while the scene's buffers, images and acceleration structures upload.
  Without pipeline libraries (or `PRISM_VULKAN_PIPELINE_LIBRARIES=0`) the
  pipeline is created whole, as a deferred operation joined by worker
  threads, with one cache file per material set
  (`<digest>-<pipeline cache UUID, device, driver>.vkpc`).
- **Payload.** 47 words in (the shading point fields the MDL state and BSDF
  read, the BSDF sample point, the light direction, the RNG state) and 21
  out: 272 bytes through the callable's stack per call.
- **Fallback.** If the pipeline cannot be built, the scene renders on the
  CPU device as before.

When it is used (`PRISM_VULKAN_CALLABLES`): unset (`auto`), when the
assembled compute kernel is above `PRISM_VULKAN_CALLABLE_THRESHOLD_KIB`
(default 320), when DXC already failed to compile it within its memory limit
(the remembered `.too-large` marker), or when it fails that way now; `1`
always; `0` never. Kernels up to 320 KB carry little material code next to
the integrator (about 180 KB): the compute kernel then compiles about as fast
as the raygen shader alone and runs faster (CornellBox, 256 KB: 1.7x;
the test fixtures, 183–230 KB: up to 10%), while from the lion (392 KB, one
program) up every scene measured is faster with callables or within noise.
Devices without ray tracing pipelines use the compute megakernel, and the
CPU fallback for kernels too large for it.

Measured before pipeline libraries, trimmed callables and the size rule
(one monolithic pipeline per material set, callables forced), on an RTX
4070 Ti (driver 610.88, DXC 1.8), 960x540, 64 spp,
Hydra (`prism_hydra_render`). Chess and the eggs are forced through callables
(`PRISM_VULKAN_CALLABLES=1`) for comparison with the megakernel. Cold: every
shader compiled anew (`PRISM_VULKAN_SHADER_SALT`); warm: SPIR-V and pipeline
caches present. GPU times are the median pass time per run, averaged over 4
alternating runs (another process used the GPU meanwhile; runs it slowed by
over 1.5x are left out). Parity is against the CPU device with the same seed;
the noise floor is the CPU with seed 1 against seed 0.

| Scene (programs) | Cold setup: total (DXC wall, slowest, largest DXC; driver) | Warm setup | Callables ms/sample | Megakernel ms/sample (cold compile) | CPU ms/sample (approx.) | Mean ratio, NRMSE vs CPU (noise floor) | Callables vs megakernel |
| --- | --- | --- | --- | --- | --- | --- | --- |
| OpenPBR playground (35) | 26.4 s (15.2 s, 5.2 s, 440 MiB; 10.2 s, +820 MiB) | 1.0 s | 85.8 | does not compile | 660 | 0.9989, 0.075 (0.228) | — |
| Stirling (24) | 13.6 s (6.5 s, 2.7 s, 245 MiB; 6.9 s) | 0.4 s | 13.2 | does not compile | 56 | 0.9986, 0.034 (0.126) | — |
| MaterialEggs countertop (34) | 24.4 s (14.4 s, 4.5 s, 350 MiB; 9.1 s) | 1.1 s | 107.7 | does not compile | 560 | 1.0000, 0.027 (0.159) | — |
| OpenChessSet (5) | 8.7 s (2.2 s, 1.5 s, 136 MiB; 6.5 s) | 0.1 s | 23.4 | 45.5 (41 s) | 180 | 0.9982, 0.056 (0.157) | 1.00000, 0.0022 |
| Jade egg (7) | 8.4 s (1.9 s, 0.9 s, 79 MiB; 6.4 s) | 0.1 s | 22.3 | 26.7 (34 s) | 185 | 0.9998, 0.0015 (0.072) | 1.00000, 0.0007 |
| Brown egg (6) | 8.1 s (1.7 s, 1.1 s, 93 MiB; 6.4 s) | 0.1 s | 18.0 | 19.7 (26 s) | 180 | 0.9997, 0.0036 (0.066) | 1.00000, 0.0011 |
| Gold egg (6) | 7.7 s (1.3 s, 0.6 s, 55 MiB; 6.4 s) | 0.1 s | 18.0 | 18.1 (24 s) | 155 | 0.9998, 0.0024 (0.070) | 1.00000, 0.0006 |

- **Parity.** Callables reproduce the megakernel image (mean ratio
  1.00000, NRMSE 0.0006–0.002, float-level differences in the inlined
  code); against the CPU they stand exactly where the megakernel does. The
  0.1–0.2% darker mean on textured scenes (chess, Stirling, OpenPBR) is the
  GPU's mipmapped texture filtering, also seen with the megakernel. OpenPBR
  ran with the default texture budget (38 of 152 images at reduced
  resolution); its 8.7 GB of full-resolution textures do not fit next to the
  desktop on a 12 GB card and paging made it 2.7x slower. The fixtures
  (lights, emissive, linking, SSS furnace, parity, CornellBox, cutouts) match
  the megakernel the same way and are clean under `VK_LAYER_KHRONOS_validation`.
- **Speed.** The callable path is not slower than the megakernel on any
  scene that has both, and up to 2x faster (chess): the compute megakernel
  uses 168–255 registers per thread and spills 1.5–3.4 KB (driver
  statistics), while the raygen shader keeps only the integrator's state and
  each material runs with its own register allocation.
- **Compile cost.** DXC per callable takes 0.2–5 s and at most 440 MiB
  (the megakernel needed more than 8 GB); the driver compiles the pipeline on
  worker threads (Stirling: 16.5 s on one thread, 7 s on 25) and about 6 s of
  it is the raygen shader alone. Warm starts take 0.1–1.1 s.
- **Shader execution reordering** (hint by program, `PRISM_VULKAN_REORDER=1`)
  is off by default: 2–8% slower on OpenPBR, chess and the eggs, neutral on
  Stirling, 16% faster on the countertop. Reordering by program alone does
  not pay for itself; the countertop gains because its many programs
  interleave within warps.
- **Not measured:** occupancy and spills of the raygen and callables. The
  NVIDIA driver lists no executables for ray tracing pipelines
  (`VK_KHR_pipeline_executable_properties`), and Nsight Compute was not
  available.

## Mac GPU vs CPU

Apple M5 (10 CPU threads, 8 GPU cores), macOS 26. Times are the renderer's
`renderSeconds` with warm shader caches; for the GPU they include the scene
upload. "Noise floor" is the CPU against itself with another seed: a GPU
difference below it is sampling noise, not bias.

| Scene | Size, spp | CPU | Metal | Speed-up | Metal vs CPU NRMSE | Noise floor NRMSE | Mean luminance difference |
| --- | --- | --- | --- | --- | --- | --- | --- |
| OpenChessSet, standalone | 1280x720, 256 | 221.3 s | 21.4 s | 10.4x | 0.068 | 0.109 | 0.02% |
| OpenChessSet, Hydra | 1280x720, 256 | 221.9 s | 16.9 s | 13.2x | 0.068 | — | 0.003% |
| CornellBox | 640x360, 128 | 28.6 s | 1.5 s | 18.8x | 0.036 | 0.118 | 0.05% |
| QuadTest | 640x360, 128 | 11.0 s | 1.1 s | 10.4x | 0.0001 | 0.017 | 0.00002% |
| InOneWeekend (485 analytic spheres) | 640x360, 128 | 31.1 s | 11.3 s | 2.7x | 0.015 | 0.042 | 0.01% |
| Parity fixture (environment map) | 256x256, 256 | 9.2 s | 3.1 s | 3.0x | 0.00008 | 0.010 | 0.00005% |
| Hydra fixtures (instances, preview, bindings) | 256x256, 128 | 0.6–5.3 s | 0.5–2.4 s | 0.7–10x | ≤ 0.019 | — | ≤ 0.03% |

The OpenChessSet scene is lit only by the Poly Haven `hamburg_hbf` 4K
environment map (`local/chess/chess_scene.usda`).

| CPU | Metal |
| --- | --- |
| ![CPU](images/chess_cpu.jpg) | ![Metal](images/chess_metal.jpg) |
| CPU seed 0 vs seed 1, x4 | Metal vs CPU, x4 |
| ![CPU noise](images/chess_diff_cpu_cpu.jpg) | ![Metal difference](images/chess_diff_metal_cpu.jpg) |

The Metal difference is darker than the CPU's own seed-to-seed noise and
shows no structure. Remaining differences come from single-precision camera
rays, GPU mipmapped texture filtering against OIIO's anisotropic filtering,
and hardware intersection.

Notes:

- Small scenes are dominated by the fixed GPU scene setup (about 1–2 s); the
  speed-up grows with image size and sample count.
- Analytic spheres are procedural primitives tested in the ray-query loop,
  which Apple's hardware traverses slower than triangles (InOneWeekend).

Reproduce with `local/baseline/compare_gpu.sh <output dir>` (local, untracked
setup; see `prism_env.sh`).

## Sampling

Everything below lives in the shared integrator (`shaders/integrator/`), in
the C++/HLSL subset, so the CPU, Vulkan and Metal devices draw the same
numbers in the same order and agree sample for sample.

- **Sample points.** `Sampler` (`types.h`) hands out 4-D points:
  Owen-scrambled Sobol (Burley 2020) indexed by the sample number, with a
  per-pixel seed and a separate index shuffle and scramble for each 4-D
  group, so any sample index works in progressive rendering. The first four
  groups are Sobol (camera pixel and lens, the first BSDF sample, the light
  selection, the light sample); later ones are hashed random points.
  Materials and cutouts keep the pixel's LCG. Numbers stay on a 24-bit grid
  in (0, 1): zero becomes 2^-25, because MDL's layered BSDFs pick a
  zero-weight lobe for xi exactly 0 and divide by its probability.
- **Next-event estimation** at every scattering vertex, with power-heuristic
  MIS against BSDF sampling for every light type. Probabilistic NEE
  (`prism:neeProbabilityMin`) is off by default.
- **Light selection.** An alias table over the light table picks by power
  (analytic lights, constant-emission triangles, the dome as one entry).
  With `prism:lightSelection spatial` (default) and 2 to 64 analytic lights,
  an analytic pick is redone among the analytic lights in proportion to
  `lightImportance` at the shading point: radiance times the solid angle
  the light subtends, zero behind one-sided planar lights, inside spheres
  and cylinders, and for lights not linked to the receiver. Triangles and
  the dome keep their probabilities. BSDF hits on a light recompute its
  selection probability from the vertex that scattered the ray (not the ray
  origin, which moves at cutout crossings).
- **Shapes.** Spheres: uniform cone. Rects: uniform solid angle (spherical
  rectangles, Urena et al. 2013) above 1e-3 sr, area below. Cylinders: the
  arc visible from the shading point, equiangular along the axis. Disks and
  triangles: area. Distant lights: cone. Dome: texel alias table.
- **Russian roulette** from `prism:rrMinDepth` (4) with survival
  clamp(path weight, 0.05, 0.95).

Measured on Vulkan at 256x256 against 32768-spp references, sample clamp off,
RMS over 4 seeds (before: the integrator before these changes, with
probabilistic NEE and power selection):

| Scene | NRMSE 64 spp before → after | NRMSE 256 spp before → after | Time per sample |
| --- | --- | --- | --- |
| CornellBox (small area light) | 0.0101 → 0.0088 | 0.0045 → 0.0038 | +2% |
| `tests/hydra/fixtures/lights.usda` (5 UsdLux lights) | 0.224 → 0.053 | 0.113 → 0.027 | +2% |
| Large rect light near a ball and floor | 0.106 → 0.080 | 0.053 → 0.040 | +3% |
| MaterialEggs jade egg (dome, subsurface) | 0.118 → 0.116 | 0.062 → 0.061 | +9% |
| OpenChessSet (dome) | 0.147 → 0.151 | 0.080 → 0.081 | +1% |

Relative mean luminance stays within 0.05% of the references. Where the
noise came from (64 spp): on CornellBox, 77% from light that bounced at least
twice, 23% from directly visible edges and the light, under 1% from direct
light; on the lights fixture, 89% from direct light, nearly all of it on the
wall next to the tube light; on the egg and the chess set, roughly 45% direct
and 55% indirect. Always sampling lights costs the egg and the chess set a few
percent of efficiency (the egg's random walks leave through many vertices);
`prism:neeProbabilityMin 0.05` restores the previous behavior.

Tried without a measurable gain: Sobol points for all dimensions or for 8 or
16 groups (within 2%), packing the light selection into the light sample's
group, and Russian roulette from depth 2 (worse) or 8 (CornellBox -14% MSE,
OpenChessSet +18% time). Pixel filters other than the box were not tried:
they change the image, not just its noise.

## Diagnostics

- `PRISM_GPU_VERBOSE=1`: compile and upload timings.
- `PRISM_GPU_DEBUG=1|2|3|4`: GPU primary-hit views (normal, texture
  coordinates, material, distance).
- `PRISM_SHADER_DIR=<repo>/shaders`: load shader sources from disk instead
  of the embedded copies.
- `PRISM_DXC=<path>`: the DXC executable to use.
- `PRISM_DXC_MEMORY_MIB=<n>` (Windows): memory DXC may use, by default a
  quarter of physical memory and at most 8 GiB. SPIR-V legalization of
  kernels with very many materials (OpenPBR playground, MaterialEggs
  countertop, Stirling) exceeds it; the Vulkan device then uses callable
  shaders (above), or renders on the CPU without ray tracing pipelines.
- `PRISM_VULKAN_CALLABLES=0|1|auto`, `PRISM_VULKAN_CALLABLE_THRESHOLD_KIB=<n>`,
  `PRISM_VULKAN_REORDER=1`, `PRISM_DXC_JOBS=<n>`,
  `PRISM_VULKAN_COMPILE_THREADS=<n>`, `PRISM_VULKAN_PIPELINE_LIBRARIES=0`
  (one monolithic pipeline), `PRISM_VULKAN_TRIM_CALLABLES=0` (callables with
  the whole link unit): the callable path (above).
- `PRISM_VULKAN_PIPELINE_STATS=1`: the driver's statistics of each compiled
  shader (registers, spills, stack) through
  `VK_KHR_pipeline_executable_properties`.
- `PRISM_VULKAN_PARALLEL_PIPELINE=0` (driver compile on one thread),
  `PRISM_VULKAN_PIPELINE_CACHE=0` (ignore stored pipeline caches) and
  `PRISM_VULKAN_SHADER_SALT=<n>` (unique shaders on both paths, so DXC and
  the driver's own shader cache compile anew): cold-compile measurements.
- `PRISM_GPU_TEXTURE_BUDGET_MIB=<n>`: GPU memory for textures (default 45%
  of the device's). Images beyond it are uploaded at reduced resolution,
  reported as `PrismRT Vulkan: N of M images uploaded at reduced resolution
  to fit GPU memory (PRISM_GPU_TEXTURE_BUDGET_MIB)` (`PrismRT Metal:` on
  Metal). Vulkan uploads from each texture's GPU-ready copy
  ([texture service](texture_service.md#preparation-pipeline)) and counts its
  real storage: 8-bit sources as UNORM8 (also when reduced), 16-bit integer
  sources as UNORM16, half and float as the float `.tx.exr` rule chose
  (8-bit when every value is k/255, half below 60000, otherwise float),
  environments as float. Metal still estimates one byte per component and
  reads the `.tx.exr`.
- `PRISM_GPU_TEXTURE_COPIES=0`: Vulkan uploads from the float `.tx.exr`
  files, as before GPU-ready copies.
- `PRISM_DISABLE_METAL=1`, `PRISM_DISABLE_VULKAN=1`: hide a device.
