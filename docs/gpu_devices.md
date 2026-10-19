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
  countertop, Stirling) exceeds it; the Vulkan device then renders on the CPU.
- `PRISM_GPU_TEXTURE_BUDGET_MIB=<n>`: GPU memory for textures (default 45%
  of the device's). Images beyond it are uploaded at reduced resolution,
  reported as `PrismRT Vulkan: N of M images uploaded at reduced resolution
  to fit GPU memory (PRISM_GPU_TEXTURE_BUDGET_MIB)` (`PrismRT Metal:` on
  Metal).
- `PRISM_DISABLE_METAL=1`, `PRISM_DISABLE_VULKAN=1`: hide a device.
