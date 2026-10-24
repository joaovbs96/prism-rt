# ImageBuf, resident pixels, and TextureSystem benchmark

> **Historical.** `prism_render_cost_benchmark` (and
> `tests/benchmark_texture_access.py`, which drove it) compared the hard-coded
> C++ InOneWeekend scene with its USD export on the original recursive
> integrator. Both the C++ scenes and that integrator were removed, and the
> benchmark with them; the measurements below are kept as a record.

This compares environment radiance access only. Production code and MDL's
material-texture runtime are unchanged. All three variants retain the same
environment importance distribution, solid-angle PDFs, MIS, transforms,
bilinear filtering, longitude wrapping, latitude clamping, and RNG sequences.

## Configuration

- OpenImageIO 3.1.12, RelWithDebInfo, i7-14700K, 28 TBB logical processors.
- InOneWeekend procedural geometry and handwritten materials: 256 x 144,
  32 spp, maximum depth 32, roulette depth 4, seed 0, sample clamp disabled.
- Each strategy runs in a separate process, sequentially without other renders
  or builds. One warmup precedes three timed renders; tables show the median.
  Image writing, preparation, and instrumented passes are excluded.
- TextureSystem uses one private system shared by the render workers, a cached
  texture handle, per-thread lookup state, and a 128 MB tile-cache budget.
- `TextureSystem::texture()` samples our existing latitude-longitude coordinates
  with NoMIP, bilinear interpolation, periodic S, clamped T, and zero derivatives.
  This isolates accessor overhead; it does not benchmark anisotropic filtering
  or a production ray-footprint implementation.
- TextureSystem inputs are tiled, mipmapped, float OpenEXR `.tx` files, prepared
  without resizing or color conversion. Highest-resolution pixels are used.
- Pole/seam and 1,024 deterministic random-direction probes precede rendering.
  Their RMS and maximum differences from ImageBuf were zero for both maps.
  These probes warm some tiles, so the first-render timings are not cold-cache
  measurements. OS file caches were not cleared.

## Render-only results

Seconds, using the same handwritten shaders for all entries:

| Environment | Sampling | ImageBuf | Resident RGB array | TextureSystem |
|---|---|---:|---:|---:|
| Exported gradient, 32 x 2048 | MIS | 4.3994 | 0.09965 | 0.15667 |
| Exported gradient, 32 x 2048 | BSDF-only | 1.3501 | 0.05200 | 0.07883 |
| Indoor HDR, 4096 x 2048 | MIS | 4.6469 | 0.11336 | 0.18990 |
| Indoor HDR, 4096 x 2048 | BSDF-only | 1.4464 | 0.05907 | 0.08211 |

TextureSystem is approximately 28x faster than ImageBuf for the exported sky
and 24.5x faster for the indoor HDR with MIS. Resident arrays remain faster in
these fully resident, single-environment tests. All corresponding images and
checksums are identical; the EXR comparisons report zero error. Every timed
repeat is deterministic, and profiling-on/off images also match exactly.

The actual exported OpenPBR/MDL scene was also measured with TextureSystem and
the original gradient sky:

| Sampling | ImageBuf baseline | Resident baseline | TextureSystem |
|---|---:|---:|---:|
| MIS | 5.66296 s | 0.320356 s | 0.389310 s |
| BSDF-only | 1.25473 s | 0.183863 s | 0.216422 s |

The first two columns are the earlier matched measurements documented in
[render_cost.md](render_cost.md); the new TextureSystem run used the same
settings and scene data. Its MIS repetitions were 0.380590, 0.401053, and
0.389310 seconds. Both output EXRs compare exactly with the original ImageBuf
MDL images. TextureSystem is about 14.5x faster than the current access path,
and about 22% slower than resident pixels for this particular MDL workload.
The separate material import took 119.199 seconds; texture access does not
solve material preparation cost.

## Setup and memory

For the 4K environment, stage/camera/environment preparation took 0.594 s for
ImageBuf, 0.665 s for resident arrays, and 0.920 s for TextureSystem. The last
includes 0.202 s to create the `.tx`. Production could reuse a content-validated
prepared texture; this benchmark regenerated it. The source was about 24.6 MB
on disk and the prepared float mipmapped file about 57.3 MB.

TextureSystem's reported peak tile memory was 1.5 MB for the narrow gradient
and 96 MB for the 4K HDR. Both fit the configured 128 MB budget, so these tests
do not establish performance under cache eviction or texture streaming pressure.

For the 4K case, source RGB floats occupy 96 MiB. The resident prototype adds
another 96 MiB array. All implementations retain the original ImageBuf and
the existing full-resolution importance tables. Process peak working sets were
approximately 488, 489, and 490 MiB respectively, dominated by common setup
allocations. These are whole-process peaks, not steady-state texture memory;
the 128 MB cache budget is not a whole-renderer memory limit. A production
TextureSystem integration should avoid retaining an unnecessary source pixel
copy after constructing the sampling distribution.

## Interpretation

The earlier speedup demonstrates an unsuitable hot-path accessor, not a reason
to abandon OIIO. TextureSystem removes most of that overhead while providing a
foundation for tiled texture caching and footprint-based filtering. Resident
arrays are a useful speed reference and possible budgeted optimization for
small, frequently sampled environments.

Before choosing a general production policy, measure many textures exceeding
the cache budget, actual mip/anisotropic footprints, resource reloads, and the
native MDL backend's texture integration. Do not infer those results from this
single-environment benchmark. TextureSystem lookup failures abort the benchmark
rather than silently rendering black.

## Reproduction and artifacts

Build `prism_render_cost_benchmark` in RelWithDebInfo and initialize
`prism_env.ps1`, then run:

```powershell
python tests/benchmark_texture_access.py --output build/texture-sky
python tests/benchmark_texture_access.py --output build/texture-indoor --env-file assets/environment/IndoorEnvironmentHDRI003_4K-HDR.exr
```

The runner records commands, all three repetitions, Windows process peak
working sets, and image-comparison results in `results.json`. Use `--include-usd`
to compile and measure the exported OpenPBR/MDL scene as well; each strategy
then pays its own material preparation cost.

Measured artifacts are `build/round2-validation/texture-sky` and
`build/round2-validation/texture-indoor`. TextureSystem cache statistics and
preprocessing/probe details are in their `texturesystem.log` files.
The MDL run is in `build/round2-validation/texture-mdl.log`, with its EXRs in
`build/round2-validation/texture-mdl`. The benchmark target builds successfully;
the three-strategy runners, deterministic-repeat checks, profiling invariance,
finite-pixel checks, directional probes, and EXR comparisons all passed.
