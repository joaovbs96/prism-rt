# Render devices: CPU, Metal and Vulkan

PrismRT renders through a *device*. Every device runs the same path tracer,
`shaders/integrator/path_tracer.inc`, against its own backend:

| Device | Select with | Backend | Status |
| --- | --- | --- | --- |
| `metal` | `--device metal`, `PRISM_DEVICE=metal`, `prism:device` | HLSL kernel, DXC + Metal shader converter, hardware ray tracing (inline ray queries) | macOS, Apple GPUs with ray tracing |
| `vulkan` | `--device vulkan`, ... | GLSL kernel, shaderc, `VK_KHR_ray_query` | Builds and compiles its kernel on macOS; needs a ray-query driver (Windows/Linux) to run, untested so far |
| `cpu` | `--device cpu`, ... | The same integrator compiled as C++, on Embree and the CPU materials (native MDL) | All platforms |
| `cpu-reference` | `--device cpu-reference`, ... | The original `SceneManager::trace` | Validation reference, bit-identical to the renderer before devices existed |

`auto` (the default) picks Metal on macOS and Vulkan elsewhere, and falls
back to the CPU device with a message when no GPU device is usable.
`PrismRT --list-devices` lists what this machine offers.

## How it fits together

- **One integrator.** `path_tracer.inc` is written in a C++/GLSL/HLSL subset
  (`shaders/integrator/prism_sl.h`) against a small backend interface
  (intersect, prepare/sample/evaluate a material, cutout, environment
  radiance/sample/pdf). The CPU device includes it into C++
  (`src/device/cpu_integrator.h`); the GPU devices include it into their
  kernels with `shaders/gpu/gpu_backend.inc`. Random numbers are drawn in
  the original order, so devices agree sample for sample up to floating-point
  and intersection differences.
- **GPU scene.** `src/device/gpu/gpu_scene.*` flattens a `SceneManager` into
  shared records (`shaders/shared/prism_gpu_types.h`): one bottom-level
  acceleration structure per prototype (triangles; analytic spheres as
  procedural boxes), one instance per placement, per-primitive materials, the
  legacy materials and textures, and the environment with its importance
  tables.
- **MDL on the GPU.** `src/device/gpu/gpu_materials.*` compiles the scene's
  MDL classes again with the GLSL or HLSL backend into one link unit, with a
  generated dispatch function per entry point. Materials keep their native
  argument blocks (the shading-language backends use the native layout);
  only texture indices are remapped. Classes that differ only in names
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
material entry point from exactly one place.

| OpenChessSet (4 MDL programs) | Time |
| --- | --- |
| First run: MDL codegen + DXC (`-Od`) + Metal shader converter + Metal pipeline | about 30 s |
| Later runs (metallib cached in `~/Library/Caches/PrismRT/metal`, pipeline in Metal's system cache) | about 1.2 s shader setup, 2.3 s scene setup in total |

The SPIR-V of the Vulkan device is cached the same way. Uploaded textures
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

## Diagnostics

- `PRISM_GPU_VERBOSE=1`: compile and upload timings.
- `PRISM_GPU_DEBUG=1|2|3|4`: GPU primary-hit views (normal, texture
  coordinates, material, distance).
- `PRISM_SHADER_DIR=<repo>/shaders`: load shader sources from disk instead
  of the embedded copies.
- `PRISM_DISABLE_METAL=1`, `PRISM_DISABLE_VULKAN=1`: hide a device.
