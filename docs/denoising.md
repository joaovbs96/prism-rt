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

| Device | Part 1 (now) | Later |
| --- | --- | --- |
| `cpu` | OIDN's CPU device on host images | |
| `vulkan` | the same CPU denoiser on host copies (correct, slower) | part 2: nvpro-samples' vk_oidn (OIDN's network as Vulkan compute) recorded into the device's own command buffer, on buffers it holds (device addresses), with a resolve pass from the accumulation to RGBA colour/albedo/normal |
| `metal` | the same CPU denoiser on host copies | part 3: OIDN's Metal device on shared `MTLBuffer`s |

All backends run OIDN's network weights. OIDN is built from Intel's source
release with only its CPU device, against PrismRT's oneTBB 2022.2
(`tools/ci/build_oidn.ps1`; why not the prebuilt package: DECISIONS.md), and is
optional at build time: `PRISM_ENABLE_OIDN` defaults to ON when CMake finds
`OpenImageDenoise` 2.x (`OpenImageDenoise_DIR`). A build without it renders
as before; asking it to denoise is an error in the CLI and a warning (the
denoised AOV then holds the raw colour) in the delegate.

Where the data lives, and how parts 2 and 3 plug in:
- Whole-image renders (the CLI) accumulate on the device. The device
  denoises at the end of `RenderImage` (`DenoiseRenderedImage`); today every
  device reads its sums back and calls the CPU denoiser. Part 2 overrides
  that in the Vulkan device: resolve and `cmdDenoise` on the GPU, then read
  back only the colour and the denoised colour.
- Progressive passes (the delegate) hand each pass's samples to the frontend,
  which accumulates on the host (its render buffers), so the denoiser takes
  host images there. The Vulkan denoiser of part 2 can take them by upload;
  avoiding that copy would mean keeping the pass accumulation on the GPU,
  which `PassRequest` does not do yet.

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
they are with `cleanAux` off (cheaper, for interactive use).

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
`denoiser` (backend name). The CLI prints the denoise time and records
`denoiseMilliseconds` in its `[PrismProfile]` line.

## Cost

OIDN's CPU device, i7-14700K (28 threads), 1920x1080 with albedo and normal:
see the timings in `local/reports/denoise_part1.md`; high quality takes a few
hundred milliseconds per frame and prefiltering about triples it. It shares
the process's oneTBB threads with a CPU render. Memory: the delegate keeps 10
floats and a count per pixel while denoising (about 44 bytes, 90 MB at 1080p
with the input copy).

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
