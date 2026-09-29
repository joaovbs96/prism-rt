# InOneWeekend render-cost comparison

The original procedural gradient and the exported HDR sky take very different
rendering paths. The first controlled comparison identifies environment
evaluation, rather than geometry or MDL execution, as the dominant cost of the
current exported scene.

## Method

`prism_render_cost_benchmark` constructs InOneWeekend with seed 0 and exports
those instantiated objects into its output directory. The procedural and USD
variants use the same USD camera, baked sky, per-pixel seeds, resolution,
maximum depth 32, roulette depth 4, and disabled sample clamp. The original
analytic gradient is measured separately. The procedural shaders and exported
OpenPBR shaders are different BSDFs; their images are not expected to match.

Measurements below use RelWithDebInfo, 256 x 144 pixels, 32 spp, and TBB's 28
logical processors on the local i7-14700K. Render times exclude setup and image
writing: one warmup followed by three timed repeats in one process. They are
steady rendering measurements, not cold-loading benchmarks or viewport frame
rates. No other PrismRT renders or builds ran alongside these measurements.

Separate 8-spp profiling passes time intersection, hit preparation, material
sampling, material evaluation, shadow intersections, cutout evaluation, and
environment work. These are sums of worker **elapsed** times, not CPU time or
end-to-end wall time. Instrumentation and scheduling affect the sums; they
locate bottlenecks rather than predict exact speedups. Timed passes do not read
the per-intersection profiling clocks.

All three timed images must match exactly. Each profiled image must also match
an unprofiled 8-spp image exactly, and all pixels must be finite.

## Current implementation

Median render times in seconds:

| Geometry and materials | Environment | MIS | BSDF-only |
|---|---|---:|---:|
| Direct procedural spheres, handwritten shaders | Baked HDR | 4.337 | 1.344 |
| Imported USD spheres, OpenPBR/MDL | Same baked HDR | 5.663 | 1.255 |
| Direct procedural spheres, handwritten shaders | Original gradient | 0.074 | 0.050 |
| Instanced procedural spheres, handwritten shaders | Same baked HDR | 4.228 | 1.234 |
| Batched procedural spheres, handwritten shaders | Same baked HDR | 4.352 | 1.308 |

Both geometry experiments render exactly the same pixels as the direct sphere
baseline in both sampling modes. Instancing versus batching produces no useful
wall-time improvement while environment evaluation dominates.

In the MIS profile, environment work accounts for 26.635 of 27.329 summed worker
seconds for handwritten shaders, and 33.648 of 35.648 seconds for the MDL scene.
The MDL path performs more environment queries, as well as more expensive
shading. Primary/path and shadow Embree intersections together account for only
0.413 summed seconds in the MDL profile.

The hot radiance path calls `ImageBuf::getpixel` four times for each bilinear
lookup. The texture constructor already forces a float image read; these
results do not establish repeated disk reads or a lazy-loading fault. A
raw-pixel-access experiment isolates accessor overhead below.

The one observed setup run took 0.00035 seconds to construct the procedural
scene and 114.986 seconds in the USD material/geometry import phase. Export and
stage/camera/environment loading were timed separately. This is one setup
observation, not a three-run cold-load performance claim.

## Resident-pixel experiment

The benchmark-only `--resident-hdr` implementation bulk-copies float RGB once
and replaces only the four per-lookup `getpixel` calls with immutable vector
reads. It retains the same filtering arithmetic, transformed-environment
sampling/PDF methods, double normalization, wrap/clamp behavior, and RNG use.
This diagnostic explicitly requires the exported fixture's identity dome
transform; it is not yet a general production environment implementation.

An initial `--skip-usd` run measured 0.0884 seconds for direct procedural spheres
with MIS, versus 4.337 seconds before: approximately 49 times faster. All six
direct/instanced/batched MIS and BSDF-only EXRs compared exactly with the
corresponding original-accessor images. Removing the pixel-access overhead
leaves the baked HDR only modestly more expensive than the analytic gradient.

The first geometry-only resident run measured 0.1069 seconds for instanced
spheres and 0.0897 seconds for batched spheres with MIS. These sub-second
measurements have visible run-to-run variation; they support further batching
tests, not a promised end-to-end speedup for arbitrary MDL scenes.

The subsequent full run, including all imported MDL materials, measured:

| Variant | Original access, MIS | Resident access, MIS | Resident access, BSDF-only |
|---|---:|---:|---:|
| Direct procedural spheres, baked HDR | 4.337 s | 0.0989 s | 0.0550 s |
| Imported USD, OpenPBR/MDL | 5.663 s | 0.3204 s | 0.1839 s |
| Original procedural gradient | 0.0737 s | 0.0724 s | 0.0526 s |
| Instanced procedural spheres, baked HDR | 4.228 s | 0.1209 s | 0.0620 s |
| Batched procedural spheres, baked HDR | 4.352 s | 0.0997 s | 0.0512 s |

The actual MDL scene improves by approximately **17.7x**. All ten before/after
EXRs compare exactly, including both MDL images. In the new MDL MIS profile,
material sample/evaluate/cutout work accounts for about 52% of summed worker
elapsed time, environment work 14%, and Embree intersections 15%. Those become
reasonable next optimization targets after eliminating the accessor bottleneck.
The roughly 18% reduction from instanced to batched handwritten-shader spheres
is a separate controlled geometry experiment, not a measured 18% improvement
to the MDL scene or a production Hydra batching implementation.

The full experiment's log and images are
`build/round2-validation/render-cost-resident.log` and
`build/round2-validation/render-cost-resident`. Material preparation still took
113.670 seconds in that run: the runtime fix does not solve cold loading.
The final all-target build and all 13 CTests passed; see
`build/round2-validation/render-cost-regression-build.log` and
`build/round2-validation/render-cost-ctest.log`.

## Proposed changes, in order

The follow-up [TextureSystem benchmark](texture_access.md) compares a scalable
OIIO access path with both implementations below; direct arrays are a useful
reference, not a recommendation to bypass TextureSystem for all textures.

1. Give the shared environment implementation immutable, direct CPU pixel
   access established at load time. Preserve existing distribution, filtering,
   transforms, and MIS. Prefer a lifetime-safe view of existing float storage
   where practical; an additional RGB float copy costs 12 bytes per texel.
   Validate rotated/scaled domes, poles, black maps, resource reloads, and image
   equality before deploying the general implementation.
2. Reduce MDL per-hit work without changing the BSDF: reuse initialized state
   between sampling and evaluating one hit, and cache compiler-proven constant
   opacity, emission, and thin-walled properties per material argument block.
   Shared target code does not imply identical parameter values. Proven opaque
   blockers can avoid full shading-state construction on shadow rays. Do not
   skip NEE merely because a sampled event was delta when a material also has
   continuous lobes.
3. Consider packed sphere geometry for placements that remain spherical under
   their transforms. Keep affine instances for ellipsoids and repeated complex
   meshes. Preserve per-primitive materials, source/instance picking IDs, UV
   frames, and targeted editing. Measure rebuild cost as well as render time.
4. Address material conversion for cold loading separately. Runtime environment
   improvements do not remove the substantial preparation cost measured here.

Avoid specializing every colored sphere into separate native MDL code: that
would sacrifice the existing single-code-program reuse and could worsen
loading. Any specialization should be limited and justified by a new profile.

## Reproduction

From PowerShell at the repository root:

```powershell
. ./prism_env.ps1
cmake --build build --config RelWithDebInfo --target prism_render_cost_benchmark
./build/RelWithDebInfo/prism_render_cost_benchmark.exe build/render-cost 256 144 32 --geometry-experiment
./build/RelWithDebInfo/prism_render_cost_benchmark.exe build/render-cost-resident 256 144 32 --geometry-experiment --resident-hdr
# Fast geometry/environment-only diagnostic, without compiling USD materials:
./build/RelWithDebInfo/prism_render_cost_benchmark.exe build/render-cost-quick 256 144 32 --geometry-experiment --resident-hdr --skip-usd
```

The benchmark writes its generated USDA, companion sky, and linear EXRs into
the chosen output directory. Preserve stdout for timings, profiles, checksums,
and comparisons. The measured baseline log is
`build/round2-validation/render-cost.log`; images are in
`build/round2-validation/render-cost`.
