# Denoising

PrismRT denoises its path-traced colour with Intel Open Image Denoise (OIDN)
2.5.1, its `RT` filter guided by an albedo and a normal image that the path
tracer gathers with the colour. Denoising is off by default; the raw colour
stays the default output.

## Backends

A render device hands out its denoiser (`RenderDevice::CreateDenoiser`,
`src/device/device.h`) and denoises its own whole-image renders
(`ImageRequest::denoise`, `denoised`), so the frontends never pick a backend.
The interface is `prism::device::Denoiser` (`src/device/denoiser.h`): one call
with the colour, albedo and normal images and the settings.

| Device | Denoiser |
| --- | --- |
| `cpu` | OIDN's CPU device on host images |
| `vulkan` | vk_oidn 0.2.0: OIDN 2.5.1's network as Vulkan compute, on the GPU (below); OIDN's CPU device on host copies when the GPU cannot run it |
| `metal` | OIDN's Metal device on the render queue and shared `MTLBuffer`s (below); OIDN's CPU device on host copies when OIDN has no Metal device |

All backends run OIDN 2.5.1's network weights. On Windows OIDN is built from
Intel's source release with only its CPU device, against PrismRT's oneTBB 2022.2
(`tools/ci/build_oidn.ps1`; why not the prebuilt package: DECISIONS.md); on
macOS with its CPU and Metal devices (`tools/ci/build_oidn_macos.sh`). It is
optional at build time: `PRISM_ENABLE_OIDN` defaults to ON when CMake finds
`OpenImageDenoise` 2.x (`OpenImageDenoise_DIR`). A build without it renders
as before; asking it to denoise is an error in the CLI and a warning (the
denoised AOV then holds the raw colour) in the delegate.

Where the data lives:
- Whole-image renders (the CLI) accumulate on the device, which denoises at
  the end of `RenderImage`: the Vulkan and Metal devices on the GPU, on
  their accumulation buffer (`GpuRenderDevice::DenoiseAccumulation`); the
  CPU device (and a GPU device without its GPU denoiser) reads the sums back
  and calls its denoiser (`DenoiseRenderedImage`).
- Progressive passes (the delegate) hand each pass's samples to the frontend,
  which accumulates on the host (its render buffers), so the denoiser takes
  host images there; the Vulkan and Metal denoisers copy them into their
  buffers. Avoiding that copy
  would mean keeping the pass accumulation on the GPU, which `PassRequest`
  does not do.

## The Vulkan denoiser (vk_oidn)

nvpro-samples' [vk_oidn](https://github.com/nvpro-samples/vk_oidn)
(Apache-2.0) is OIDN's `RT` filter as Vulkan compute: the same U-Net and
weights (OIDN 2.5.1's), FP16 tensors, tensor cores through
`VK_KHR_cooperative_matrix` on NVIDIA GPUs and generic convolutions on
others. It records its dispatches into a command buffer and never submits or
waits by itself. PrismRT links NVIDIA's prebuilt SDK package (a static
library with the kernels and weights; `tools/ci/fetch_vk_oidn.ps1`, why not a
source build: DECISIONS.md); `PRISM_ENABLE_VK_OIDN` defaults to ON when CMake
finds it (`vk_oidn_DIR`) and the build has OIDN.

`src/device/vulkan/vulkan_denoiser.{h,cpp}`:
- Device features: when the GPU has all that vk_oidn needs (`shaderInt64`,
  `shaderInt16`, `storageBuffer16BitAccess`, `shaderFloat16`, the Vulkan
  memory model with device scope, `maintenance4`, `subgroupSizeControl`,
  `computeFullSubgroups`; `VK_KHR_cooperative_matrix` when present), the
  Vulkan device enables them at creation; otherwise it enables none and
  denoises with OIDN's CPU device on host copies.
- At its first use the device creates the denoiser (vk_oidn's context and
  pipelines, a small pipeline of PrismRT's own passes,
  `shaders/gpu/vulkan/denoise.hlsl`) and prints once which denoiser it uses:
  `PrismRT Vulkan: denoising on the GPU with vk_oidn 0.2.0 Vulkan
  (cooperative matrices)`, `(generic convolutions)`, or `denoising with
  OIDN's CPU device on host copies (<reason>)`. The CLI prints the denoiser
  with each denoise and records it in `[PrismProfile]` (`denoiser`); the
  delegate reports it in its `denoiser` statistic.
- Whole-image renders: a resolve pass reads the accumulation buffer (colour
  sums, then the light AOV sums) times each pixel's 1 / samples (the same
  product the host computes for the returned colour) into RGBA float colour,
  albedo and normal buffers; the filters run; only the denoised colour is
  copied back. Host images (the delegate, or any `Denoiser::Denoise` call):
  packed into an upload buffer, copied, denoised, read back. One submission
  per denoise on the device's queue (shared with the render under its queue
  lock), waited with a fence.
- Prefiltering (`prism:denoisePrefilter`): vk_oidn has no albedo or normal
  filter of its own, so OIDN's (`rt_alb`, `rt_nrm`; `_large` for high
  quality, as OIDN picks them) are packed for it by the fetch script and
  embedded in the build; they run as vk_oidn LDR filters (the albedo with
  OIDN's sRGB transfer, the normal with the linear one on `n * 0.5 + 0.5`,
  mapped there and back by two small passes, as OIDN's own input and output
  processing do), then the colour filter takes the guides as clean.
- Filters are kept for the last size and settings (a change re-creates them
  and uploads their weights).

| Variable | Effect |
| --- | --- |
| `PRISM_GPU_DENOISER=cpu` | The Vulkan device uses OIDN's CPU device on host copies (and enables none of vk_oidn's features) |
| `VK_OIDN_COOPMAT=0` / `1` | vk_oidn's generic convolutions / tensor cores also on non-NVIDIA GPUs |
| `VK_OIDN_PROFILE=1` | vk_oidn prints per-layer GPU times when a filter is destroyed |
| `PRISM_GPU_VERBOSE=1` | The filters' memory and tile count when created |

## The Metal denoiser (OIDN's Metal device)

OIDN ships a Metal device of its own (`OIDN_DEVICE_METAL`, Apple silicon,
macOS 13+): the same `RT` filter, weights and prefilter networks as its CPU
device, run with Metal Performance Shaders Graph. It is not in OIDN's
default build, so `tools/ci/build_oidn_macos.sh` builds OIDN 2.5.1 with the
CPU and Metal devices (it needs Xcode's Metal compiler; see the script).

`src/device/metal/metal_denoiser.{h,mm}` (built with the Metal device when the
build has OIDN, `PRISM_HAS_METAL_OIDN`):
- At its first use the Metal device creates the denoiser: `oidnNewMetalDevice`
  on the render device's own `MTLCommandQueue` (so OIDN's command buffers are
  ordered with the renders), and a small resolve kernel (Metal Shading
  Language, compiled at run time). It prints once which denoiser it uses:
  `PrismRT Metal: denoising on the GPU with OIDN 2.5.1 Metal`, or `denoising
  with OIDN's CPU device on host copies (<reason>)` when
  `oidnIsMetalDeviceSupported` says no (an OIDN without its Metal device, or a
  GPU OIDN does not support) or the device cannot be created.
- Images: four packed RGB float `MTLBuffer`s (colour, albedo, normal,
  output), shared storage with hazard tracking, handed to OIDN with
  `oidnNewSharedBufferFromMetal`. Whole-image renders: the resolve kernel reads
  the accumulation buffer (colour sums, then the light AOV sums) times each
  pixel's 1 / samples (the host's float product) into the colour, albedo and
  normal buffers on the GPU; OIDN filters them there; the host reads only the
  denoised colour (shared storage: no copy). Host images (the delegate, any
  `Denoiser::Denoise` call) are written into the same buffers first.
- Prefiltering is OIDN's own (the albedo and normal through `RT` filters in
  place, then `cleanAux`), as on the CPU device. Filters are kept and
  recommitted for each call.
- `PRISM_GPU_DENOISER=cpu` forces the host-copy CPU fallback;
  `PRISM_GPU_VERBOSE=1` prints the set-up and per-denoise times.
- Not measured yet (no Mac run): timings and the agreement with the CPU
  device. The tests below expect it within 1e-2 NRMSE of OIDN's CPU device
  on the same raw render, like vk_oidn.

## Inputs

| Input | What | Where |
| --- | --- | --- |
| colour | the accumulated linear HDR colour (ACEScg working space by default), after the sample clamp; the CLI applies the camera exposure after denoising (OIDN's HDR mode scales its input to its training range, so a scale before or after gives the same image) | the CLI's `RenderImage` result; the delegate's own sums |
| albedo | the `albedo` light AOV (docs/hydra.md, "Light AOVs"): the first non-sharp vertex's BSDF weight, through mirrors and smooth glass | `LightAovPixel::albedo` |
| normal | the shading normal at the albedo's vertex (the same vertex, so also through sharp bounces), world space, facing the ray; 0 where the path escapes first. Averaged over the pixel's samples like the albedo (anti-aliased, not normalised, in [-1, 1]), as OIDN asks | `LightAovPixel::normal` (`LightAovSample::normal` in the integrator) |

OIDN accepts world or view space normals; world space needs no camera and is
what the integrator has. The normal is the interpolated shading normal of the
hit (`SurfaceHit::n`), not the material's bump-mapped normal (MDL does not
return that to the integrator).

The guides come with the light AOVs, so asking for denoising turns them on:
the CPU path tracer gathers them at run time; the GPU kernels use their light
AOV variant (`PRISM_LIGHT_AOVS`; the normal travels in the `w` of the first
three AOV entries, so the buffers did not grow). Without denoising (and
without light AOVs) nothing runs and the images are bit-identical to before.

`prism:denoisePrefilter` (default true) denoises the albedo and normal first
and then passes them as clean (`cleanAux`), OIDN's recommendation for final
frames with noisy guides (depth of field, rough glass); false passes them as
they are with `cleanAux` off. Prefiltering costs about six times the plain
denoise on the CPU (1080p, high: 3.1 s against 0.5 s) for about 10% less
error on the test room; at high resolutions in usdview, turn it off (or use
`fast`) for quicker interval denoises.

## Settings and outputs

| Setting | Default | CLI | Meaning |
| --- | --- | --- | --- |
| `prism:denoise` | false | `--denoise none\|aov\|replace` (none) | Delegate: show the colour AOV denoised (its accumulation, which `Map` reads, stays raw). CLI: `prism:denoise = true` means `--denoise replace` |
| `prism:denoiseQuality` | `high` | `--denoise-quality fast\|balanced\|high` | OIDN's quality mode |
| `prism:denoisePrefilter` | true | `--denoise-prefilter true\|false` | Prefilter the guides (above) |
| `prism:denoiseInterval` | 16 | Hydra only (interactive) | The delegate denoises the display every this many passes |

The CLI's `--denoise`:
- `none`: no denoising.
- `aov`: the single image also gets a `denoised` layer (EXR; other formats hold
  the colour only and get the raw colour, with a message).
- `replace`: the image's colour is the denoised colour (EXR and 8-bit
  formats); with render products, their colour vars are.

The `denoised` AOV (`AovKind::Denoised`, RGBA float like `color`) is a
RenderVar `sourceName` and a Hydra AOV: it is always denoised, whatever
`prism:denoise` says, and has channels `<var>.R/G/B(/A)` in EXRs (multi-part
too). `prism_hydra_render` has the same `--denoise`, `--denoise-quality`,
`--denoise-prefilter` options (and `--denoise-interval`) for an explicit
camera; render products take their `denoised` vars.

In the delegate (usdview), with `prism:denoise` or a bound `denoised` AOV:
- the render worker accumulates the colour and the guides; every
  `prism:denoiseInterval` passes it hands a copy of the means to a denoise
  worker thread and goes on rendering. A finished denoise is published at
  the next pass boundary (or while waiting for a pass): the `denoised` AOV's
  buffers, and with `prism:denoise` the colour buffers' display, which then
  shows the denoised image (no progressive tiles) until the next one;
- on pause, the accumulation so far is denoised; when the render converges
  (samples, adaptive sampling, time limit), it denoises the final samples and
  only then reports convergence (the worker answers a stop at once);
- the CPU preview while a GPU prepares, and the coarse camera-motion
  preview, are not denoised (the `denoised` AOV shows the raw preview);
- turning `prism:denoise` on or off restarts the accumulation (the guides
  are needed from its first sample); the other denoise settings resume it, so
  a converged render denoises again with them.

Statistics: `denoises` (published), `denoiseMilliseconds` (the last one),
`denoiser` (backend name). The CLI prints the denoise time and the denoiser
and records `denoiseMilliseconds` and `denoiser` in its `[PrismProfile]` line.

## Cost

OIDN's CPU device, i7-14700K (28 threads), 1920x1080 with albedo and normal,
high quality: about 0.4 s steady state (OIDN's benchmark on these DLLs: 398 ms;
Intel's prebuilt, compiled with Intel's clang: 376 ms) and 0.5 s for the CLI's
one denoise (with set-up); prefiltered guides 3.1 s. It shares
the process's oneTBB threads with a CPU render. Memory: the delegate keeps 10
floats and a count per pixel while denoising (about 44 bytes, 90 MB at 1080p
with the input copy).

The Vulkan denoiser, RTX 4070 Ti (driver 610.88), the CLI's one denoise of a
1920x1080 render of denoise.usda at 16 spp, high quality (single runs; the
first denoise of a process includes creating the filters and uploading
their weights; "GPU" is the submission's resolve, filters and read-back copy):

| Denoiser | Prefiltered guides | CLI denoise | of which filter set-up | GPU |
| --- | --- | --- | --- | --- |
| vk_oidn, cooperative matrices | no | 177 ms | 126 ms | 36 ms |
| vk_oidn, cooperative matrices | yes | 481 ms | 379 ms | 88 ms |
| vk_oidn, generic (`VK_OIDN_COOPMAT=0`) | no | 92 ms | 11 ms | 66 ms |
| vk_oidn, generic | yes | 360 ms | 36 ms | 309 ms |
| OIDN CPU on host copies (`PRISM_GPU_DENOISER=cpu`) | no | 448 ms | | |
| OIDN CPU on host copies | yes | 2866 ms | | |

Device memory of the filters at 1080p: 385 MiB, 1.5 GiB with the prefilters
(three large networks' intermediate tensors). On the same raw render, the
denoised images agree with the CPU device's within NRMSE 1.3e-3 to 1.4e-3
(both vk_oidn paths, with and without prefiltering; at 96x64 1.4e-3); on the
synthetic test images, high quality within 1.3e-3 and fast/balanced (FP16
accumulation in vk_oidn) within 7.4e-3.

## Tests

`prism_denoise_test` (the interface on synthetic images, strides, errors);
`prism_denoise_error_{cpu,vulkan,hydra}`: 16 spp denoised against a 4096 spp
reference of `tests/hydra/fixtures/denoise.usda` (textured floor, glass,
mirror, a small light in a closed room) must have at most half the colour's
NRMSE; `prism_denoise_replace_matches_cpu` (`prism:denoise` in the CLI);
`prism_parity_denoise_*` (both loaders' `denoised` products, one part and a
part per var); `prism_denoise_interval_hydra` (interval denoises before
convergence); `prism_hydra_render_buffer_test` (the denoised display) and
`prism_hydra_task_delegate_settings_test` (settings, AOV descriptor).

The Vulkan denoisers (`gpu` label): `prism_denoise_{render,error}_vulkan`
(vk_oidn on the accumulation; fails if the log says it fell back),
`_vulkan_generic` (`VK_OIDN_COOPMAT=0`) and `_vulkan_cpu`
(`PRISM_GPU_DENOISER=cpu`, the host-copy fallback), each at most half the
colour's NRMSE; `prism_denoise_vulkan_same_raw` (those renders' raw colours
are identical) and `prism_denoise_vulkan{,_generic}_matches_cpu` (so their
denoised layers compare the GPU and CPU denoisers on the same input, within
1e-2 NRMSE); `prism_denoise_{render,error}_hydra_vulkan` (the delegate on
Vulkan: vk_oidn on uploaded host images); `prism_denoise_vulkan_test` and
`_vulkan_generic_test` (`prism_denoise_test --vulkan`: the host-image path
against OIDN's CPU device on the synthetic images, every quality with and
without prefiltering, within 1.5e-2, plus strided RGBA in place, the albedo
alone, no guides) and `prism_denoise_vulkan_fallback_test`.

The Metal denoisers (`gpu` label, registered only when the build has the
Metal device): `prism_denoise_{render,error}_metal` (OIDN's Metal device on
the accumulation; fails if the log says it fell back), `_metal_cpu`
(`PRISM_GPU_DENOISER=cpu`), `prism_denoise_metal_same_raw`,
`prism_denoise_metal_matches_cpu` (1e-2), `prism_denoise_{render,error}_hydra_metal`
(the delegate on Metal; fails if its `denoiser` stat is a CPU one),
`prism_denoise_metal_test` (`prism_denoise_test --metal`, 1.5e-2) and
`prism_denoise_metal_fallback_test`. The Mac checklist is in
[gpu_devices.md](gpu_devices.md#metal-checklist).
