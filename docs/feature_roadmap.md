# Feature roadmap

What the test scenes still need after the lighting, SSS, subdivision,
displacement, primvar and light-linking round, in rough priority order.
Scenes: **Eggs** (MaterialEggs), **OpenPBR** (OpenPBRShaderPlayground),
**Teapot/Lion** (MaterialXTeapotLion), **Stirling**, **ALab**, **Chess**
(OpenChessSet). They live next to each other in `assets/`.

## Color management

- **ACEScg rendering space (done).** Prism renders in an OpenColorIO working
  space, ACEScg by default, with ACES 2.0 SDR views for 8-bit outputs
  ([color_management.md](color_management.md)). Remaining: light textures'
  `colorSpace` metadata (the Hydra scene index does not carry it), sRGB
  decoding of 8-bit dome textures, UsdUVTexture `sourceColorSpace`, and an
  OpenUSD stack built with OpenColorIO so usdview can apply the same views.

## Materials

- **MaterialX 1.39.5 (done).** Stirling's car paint uses `flake3d`, which
  MaterialX 1.39.4 lacked, and usdMtlx could not match some of its
  `dielectric_bsdf` nodes against the 1.39.4 definitions ("Unable to find the
  nodedef"), so the paint lost its flakes and coat. With OpenUSD 26.08 and
  MaterialX 1.39.5 both resolve and the paint renders as authored. The MDL layer-over-mix limitation
  remains; the loader works around it (`docs/materialx_compatibility.md`).
- **Procedural noise parity.** The Eggs' noise-driven patterns (ore swirl
  stripes, displacement relief) differ from the Karma references: Karma
  evaluates MaterialX noise in its own implementation, Prism in MaterialX's MDL
  one.
- **NEE inside media and nested media.** The random walk samples no lights
  while inside a medium and tracks a single medium (no glass in liquid).
- **Textured emission as lights (done).** Triangles whose emission varies
  (textures, primvars, a varying cutout, a directional EDF such as a coated
  OpenPBR emitter) are next-event lights too, on every device: at load time
  `src/device/light_table.h` estimates each triangle's average radiance by
  evaluating the material at stratified points (the CPU MDL code; up to 16
  per triangle face, about 4M evaluations at most, in parallel) and selects
  it by that times its area, with a floor of 10% of its material's mean so
  dark-on-average triangles stay reachable; triangles whose estimate is zero
  are left to BSDF sampling. The light sample evaluates the true emission at
  the sampled point, with the footprint of the BSDF ray in that direction so
  both strategies see the same filtered textures, and BSDF hits weight these
  triangles by their own density. glTF emissive textures and OBJ `map_Ke`
  take this path. Limits: more than 2^20 such triangles are left to BSDF
  sampling; the estimate (1.2 s, 25 s of CPU time, for OpenPBR's
  157k-triangle emissive MEET MAT: 9 points on each of both faces) reruns at
  each scene change, and in Hydra on a GPU twice (the CPU preview builds its
  own light table). `PRISM_VERBOSE_LIGHTS=1` lists the emitting groups.
  Loaded by the CLI, MEET MAT's emission is zero at every probe point (it
  emits through Hydra); not investigated.
- **Material edits of displacing materials** rebuild bindings but not the
  displaced geometry until the next full snapshot.

## Lights

- **Per-light camera visibility, dome `colorTemperature`, `inputs:diffuse` /
  `inputs:specular` multipliers (done).** In both loaders, on every device; see
  [hydra.md](hydra.md#scene-features).
- **Shaping and IES profiles (done).** `UsdLuxShapingAPI` cones, softness,
  focus, focus tint and LM-63 IES profiles on sphere, rect, disk, cylinder
  and point lights, in both loaders, on every device; see
  [hydra.md](hydra.md#scene-features). Not done: photometric types A and B
  (read as type C with a warning) and the TILT block (ignored); ALab's
  profiles are empty files (one warning each, the lights render unshaped).
- **Light filters** and portal lights.
- **Dome texture formats** other than lat-long (angular, mirrored ball,
  cube maps) are read as lat-long with a warning.
- **`DomeLight_1`'s `poleAxis`** (and a Z-up stage's dome pole): domes are
  read with their pole on their local +Y (`scene` on a Y-up stage).
- **Many-light sampling.** NEE picks lights by power from an alias table,
  then re-picks analytic lights by their estimated irradiance at the shading
  point (up to 64 of them; see [Sampling](gpu_devices.md#sampling)). Emissive
  triangles are still picked by (estimated) power alone, and BSDF rays test every
  analytic light; scenes with hundreds of lights want a light BVH.
- **Solid-angle sampling of emissive triangles and disk lights** (spherical
  triangles, Arvo 1995); both are area-sampled. On CornellBox direct light is
  under 1% of the noise, so it was not pursued.
- **Shadows through glass and caustics.** In the OpenPBR scene the shadow
  under the green bottle is green in the reference and neutral in Prism.
  Shadow rays pass only cutouts (`visible()` in
  `shaders/integrator/path_tracer.h`); any transmissive surface blocks them,
  so NEE leaves the floor under the bottle in full shadow. Light reaches it
  only by a BSDF path (floor, through the glass, onto the light by chance):
  slow, speckled noise for area lights, and never for distant lights or
  zero-radius sphere lights, which a sampled ray cannot hit. Options, in
  order:
  1. **Transmissive shadows** (biased; Typhoon's mode when caustics are off,
     and likely what the reference used). Shadow rays continue straight
     through transmissive surfaces, multiplied by the transmission tint (the
     MDL BSDF's weight for a straight-through transmission sample) and by
     `exp(-sigma_a * d)` over each segment inside a medium. No refraction or
     focusing. Behind a setting such as `prism:transmissiveShadows`. Costs
     material code in the shadow loop, which the Vulkan callable path makes
     practical.
  2. **Manifold NEE** (Hanika et al. 2015; Cycles' shadow caustics).
     Unbiased caustics: from the receiver, pick a light point, seed a path
     through the glass (the straight line) and move it with Newton steps
     until it obeys refraction at each interface. Best for smooth geometry
     with one or two refractive interfaces. Each path must belong to one
     estimator: Cycles marks caster and receiver objects and drops BSDF paths
     that reach the light through a caster.
  3. **Path guiding** (see below) converges area-lit caustics faster over
     long renders, but does not fix them at our sample counts.

  Before building either, render the scene at a high sample count. Green
  speckles forming under the bottle mean light already gets through and
  show what option 1 should converge to.
- **Path guiding.** Learn, per region of space, the directions light arrives
  from, and sample a mix of the BSDF and that learned distribution under MIS:
  Müller et al. 2017 (SD-tree, crust-render) or Intel Open PGL (fitted
  mixtures, used by Cycles; CPU only). Renders in passes: paths that deliver
  light train the structure, later passes sample from it. It targets bright
  directions NEE cannot aim at (light through openings, sunlit walls, bounced
  light), so it suits interiors such as ALab and Stirling. Limits:
  - It learns only from paths that already found the light, so rare paths
    (caustics) need long training, often minutes. At tens to hundreds of
    samples per pixel the distribution is still nearly empty.
  - It chooses directions at diffuse and glossy vertices only. Inside smooth
    glass, refraction fixes the direction, so a caustic needs a floor
    direction inside a tiny cone. Coarse bins smear that cone, and it never
    finds distant or zero-radius lights.
  - Camera moves and edits restart accumulation. Interactive use needs the
    learned structure kept across restarts.

  Combined with manifold NEE: guiding cannot help MNEE's own connection,
  which samples no direction at the receiver. If guiding trains on MNEE's
  contributions, it learns that caustic-lit regions are bright and steers
  later bounces toward them, so indirect caustics converge. This is required
  anyway, since MNEE drops the BSDF paths through casters and guiding would
  otherwise learn nothing near them. Learned seed distributions for MNEE
  ("Manifold Path Guiding", Fan et al. 2023) help only if straight-line seeds
  often fail to converge (bumpy or layered glass). Order: transmissive
  shadows or MNEE first, then guiding trained on all estimators.

## Geometry

- **`displayColor`/`displayOpacity` of every interpolation, PointInstancer
  `invisibleIds`, `doubleSided` (behind `prism:honorDoubleSided`, off by default)
  and per-instance primvars (done).** Both loaders, except per-instance
  primvars (the delegate only; the CLI has no instancer or MDL primvars yet).
  See [hydra.md](hydra.md#scene-features).
- **Curves** (`BasisCurves`): ALab's fur and cloth (baked procedurals package).
  Embree has curve primitives; the GPU devices need a procedural intersector.
- **Volumes** (`Volume` + OpenVDB): the Eggs' jade egg interior. The current
  state and a staged plan (volume terminal, NEE in media, nested media,
  heterogeneous media, OpenVDB) are in [volumes.md](volumes.md).
- **Points** and particle fields (ALab's optional Gaussian splats).
- **Motion blur**: camera shutters and time-sampled transforms (Eggs, ALab).
- **Adaptive subdivision and dicing.** Subdivision is uniform per mesh under a
  face budget; dense subdivision meshes render as their control hulls, and
  displacement dicing is limited the same way. A scene-wide budget
  (`prism:subdivisionBudgetFaces`, default 24 Mi faces) lowers the levels of
  the meshes refining to the most faces first: ALab asks for 48 M faces at
  level 2 and renders with 25 M (40 M triangles, 25 GB peak).
- **Multiple UV sets.** Texture coordinates come from `st` (or `uv`);
  `geompropvalue` reads of other float2 primvars work, but MDL
  `texture_coordinate(i)` beyond the first set does not.

## Scale and performance

- **Out-of-core textures for the GPU.** *Whole-mip-level streaming on
  Vulkan: done* (see [texture service](texture_service.md#gpu-texture-streaming)).
  Still open: tile-granularity streaming, Metal (whole uploads), GPU levels
  taken from the `.tx.exr` mips the CPU samples, and ALab (more images than
  the descriptor array and the driver's allocation count allow).
- **GPU kernel compile time** grows with the material set (OpenPBR and Eggs
  take three to six minutes on first use).
- **Per-material shading for large material sets.** The GPU kernel is one
  megakernel that inlines every MDL program. On Vulkan, kernels over about
  1 MB of HLSL (Stirling 1.4 MB with 24 programs, OpenPBR playground 3.0 MB
  with 35, MaterialEggs countertop 3.1 MB with 34) do not compile.
  - DXC's SPIR-V legalization rewrites the single inlined function into SSA
    form, which needs more than 8 GB, so these scenes render on the CPU.
  - Neither alternative works. Skipping that pass, the NVIDIA driver's pipeline
    compile still needs more than 8 GB and 400 s. Keeping the material
    functions out of line (`[noinline]`) makes the driver's compile larger
    and loses the device.
  - Class compilation (`PRISM_MDL_COMPILE_MODE=class`) does not help: fewer
    programs, but 4–7x the HLSL.
  - The long-term fix is to stop compiling all materials into one function:
    per-material-program pipelines with wavefront (queued) shading, or
    callable shaders.
  - Planned order and status:
    1. Done: the path state is explicit (`PathState`) and each bounce is a
       sequence of shared step functions; the material runs once per vertex
       (`backend.shade`: BSDF sample, light evaluation, medium). Images are
       bit-identical to the single loop on the CPU and Vulkan devices.
    2. Done on Vulkan (see [GPU devices](gpu_devices.md#material-programs-as-callable-shaders-vulkan)):
       above the kernel-size threshold each MDL program is a callable shader
       of a ray tracing pipeline, with optional shader execution reordering
       by program. OpenPBR, Stirling and the countertop now render on the
       GPU; cold compiles take 14–26 s (DXC per program in parallel, the
       driver's compile on worker threads), warm starts about 1 s. On chess and the
       eggs, forced through callables, the callable path is as fast as the
       megakernel or faster (chess 2x): the megakernel's register pressure
       and spills cost more than the call overhead.
    3. Partly done: the driver lists no executables for ray tracing
       pipelines, so registers and spills are known only for the compute
       megakernel (255 registers and 1.5–3.4 KB of local memory per thread
       on the eggs and chess). Nsight Compute would give the raygen and
       callable occupancy.
    4. Wavefront is not needed for the scenes measured: callables compile in
       seconds and run at least as fast as the megakernel, and reordering by
       program helps only the countertop (16%) and costs 2–8% elsewhere, so
       material divergence is not the main limit. Reconsider it if profiling the callable path shows low
       occupancy in the raygen shader (its live state across the call) or
       for scenes with many more programs. It would rearrange the step
       functions into separate kernels, not rewrite the integrator, and needs
       a design decision because it changes how the CPU and GPU devices share
       code.
    5. Done on Vulkan: callables are the default for every scene but the
       smallest material sets (kernels up to 320 KB keep the compute kernel,
       which is faster there and compiles as fast), so the compute kernel's
       20–40 s cold compile goes away. The raygen shader depends on no scene
       and compiles once per machine in the background; each shader is a
       pipeline library, linked per scene (see
       [GPU devices](gpu_devices.md#material-programs-as-callable-shaders-vulkan)).
       Open: port the same split to Metal with visible function tables;
       per-program link units (own read-only data and texture table) so a
       program's callable no longer depends on the other programs of its link
       unit; the generated MDL code differs between runs for some scenes
       (commutative operand order), so they compile again on every start.
  - Large scenes like ALab also need texture streaming on the GPU: feedback
    of the needed texture, mip and tile from shading, loaded from the already
    tiled `.tx.exr` files into a resident tile pool. Whole mip levels first,
    tiles later. They may also need acceleration-structure compaction.
- **Denoising**: Open Image Denoise on the CPU (part 1) and vk_oidn on the
  Vulkan device (part 2; [denoising.md](denoising.md)). Open: OIDN's Metal
  device (part 3), the delegate's accumulation on the GPU (no upload per
  denoise), denoised light AOVs, the material's bump-mapped normal as the guide.

## Closing the gap to other Hydra renderers

Compared on 2026-10-21 from each project's README. Gatling, Aurora and
hdOSPRay document little, so a gap may be missing from their columns rather
than from the renderer.

| | Prism | Gatling | Typhoon | Aurora | hdOSPRay | hdMoonRay |
| --- | --- | --- | --- | --- | --- | --- |
| Device | CPU, Vulkan, Metal | Vulkan, Metal (M3+) | CPU | DXR, Hgi RT | CPU, GPU | CPU, XPU |
| Materials | MaterialX via MDL | MaterialX via MDL | MaterialX (own compiler) | Standard Surface | Principled | MoonRay shaders |
| Light linking | yes | ? | ? | no | ? | yes |
| SSS | yes | ? | yes | ? | ? | yes |
| Volumes | material media | no | material media | ? | ? | yes |
| Curves | no | no | yes | ? | yes | yes |
| Subdivision | uniform | no | adaptive | ? | yes | yes |
| Displacement | yes | no | yes | ? | ? | yes |
| Motion blur | no | ? | no | ? | ? | yes |
| IES / shaping | yes | no | yes | ? | ? | yes |
| Adaptive sampling | yes | ? | yes | ? | ? | yes |
| Denoiser | OIDN (CPU; vk_oidn on Vulkan; Metal via the CPU) | ? | ? | planned | OIDN | yes |
| LPEs / light groups | no | ? | no | ? | ? | yes |
| Status | active | active | branch on hold (refactor) | in progress | archived 2025-08 | production |

Cycles' delegate is the other complete one. Typhoon (NVIDIA's proposed OpenUSD
reference renderer, an hdEmbree extension) is the nearest CPU peer, and Gatling
the nearest GPU peer. No other delegate here runs one integrator on CPU and
GPUs from every vendor. To close the gaps, in order:

1. **Common features.** These are the items the table marks "no" for Prism
   and "yes" for most of the others.
   - **Denoising** with Open Image Denoise, fed by the albedo light AOV and a
     normal at the same vertex (both through delta bounces, as OIDN asks):
     done on the CPU device and on Vulkan with vk_oidn (the same network as
     Vulkan compute); Metal denoises through the CPU for now
     ([denoising.md](denoising.md)). Next: OIDN's Metal device.
   - **Adaptive sampling** is on by default (Cycles' error estimate, threshold
     0.01, up to 2048 samples, with a render time limit; [hydra.md](hydra.md#adaptive-sampling-and-time-limit)).
     Left: pairing it with the denoiser (stop earlier when denoising).
   - **Motion blur** (see [Geometry](#geometry)). Do it on the CPU first,
     because Embree has time-sampled geometry. Vulkan's motion-blur extension
     is NVIDIA-only, so the other GPUs need several time steps or the CPU.
   - **Curves** (see [Geometry](#geometry)). Typhoon's coverage is the target:
     linear, Bézier, B-spline and Catmull-Rom curves, drawn as tubes or as
     ribbons when normals are authored.
   - **Volumes** as staged in [volumes.md](volumes.md). UsdVol and OpenVDB
     would put Prism ahead of Typhoon and Gatling, which read neither.
2. **Large scenes on every GPU.**
   - Port the callable-shader split to Metal with visible function tables.
   - Stream textures at tile granularity, and lift ALab's limits on the
     descriptor array and allocation count (see
     [Scale and performance](#scale-and-performance)).
3. **Lights** (see [Lights](#lights)).
   - IES profiles and `ShapingAPI` (done).
   - Light filters and portals.
   - Transmissive shadows, then manifold NEE, for colored shadows and
     caustics through glass (see [Lights](#lights)).
   - Path guiding, for interiors lit through openings or by bounced light,
     where light sampling cannot aim (see [Lights](#lights)).
   - Textured emissive meshes as NEE lights (done).
   - A light BVH for scenes with hundreds of lights.
4. **Production output.**
   - `RenderSettings`, `RenderProduct` and `RenderVar` (done in
     `prism_hydra_render` and the CLI: resolution, camera, data window, pixel
     aspect ratio, purposes, `prism:*` settings and products with named AOV
     channels; see [hydra.md](hydra.md#render-settings-and-products)).
     Remaining: batch rendering through `usdrender` and Houdini's husk, and
     `disableDepthOfField`.
   - Light path expressions and light groups. Done: the `albedo`, `diffuse`,
     `specular` and `emission` light AOVs, `Pworld` and multi-part EXR output
     ([hydra.md](hydra.md#light-aovs)). Possible next AOVs: direct/indirect
     splits, transmission apart from reflection, a separate background.
   - A selectable working space such as ACEScg (see
     [Color management](#color-management)).
   - Adaptive subdivision by screen-space edge length, as Typhoon does it.
5. **Showing that it works.**
   - Run Typhoon's `typhoon-test-suite` and the MaterialX and OpenPBR
     reference scenes, and publish comparisons against Typhoon and Karma.
   - CI on Linux and macOS, so Vulkan on Linux and Metal get run as well as
     built.
   - Prebuilt binaries for current USD and Houdini versions, and an updated
     README with build steps.

## Standalone CLI

The `PrismRT` CLI loads USD through its own path (`src/mesh/usd_mesh.h`):
it reads analytic lights and combines domes, and follows the stage's render
settings and products as `prism_hydra_render` does. Hydra
(`prism_hydra_render`, usdview) is the reference path; the parity tests
([hydra.md](hydra.md#cli-and-hydra-parity)) compare the two on every
fixture, and every `prism:*` setting that affects a one-shot render has a
CLI flag and is read from the RenderSettings prim.

Since phase 7 the CLI loads what the delegate loads (done): a placement per
gprim (polygon meshes in world space; refined, displaced and instanced ones
as prototypes behind Embree instances); subdivision surfaces and load-time
displacement through the delegate's own conversion
(`src/mesh/mesh_conversion.cpp`, with its refinement budget); implicit
cubes, cylinders, cones, capsules and planes as Hydra meshes them;
PointInstancers (nested, `invisibleIds`) and native instances, with
per-instance primvars for MDL scene data lookups and bindings resolved
through instance proxies; mesh holes, `leftHanded` orientation, negative
scales (as the delegate orients mirrored placements), GeomSubset bindings,
uniform and constant normals, `primvars:normals`, named mesh primvars and
inherited constant primvars; purpose (`includedPurposes`) and visibility;
`--time` for animated attributes, transforms, materials and UsdSkel
skinning (linear blend skinning through `UsdSkelCache`); camera exposure;
light and shadow linking; UsdPreviewSurface materials and displacement
terminals (the delegate's network rewrite, `src/materialx/preview_surface.h`);
materials read from the composed UsdShade network (referenced `.mtlx` files
included); and the `primId`, `elementId` and `instanceId` AOVs.

The CLI also compiles USD materials as the delegate does (done): it hands
each bound material's composed network, as a Hydra material data source, to
the delegate's material service (`src/materialx/material_service.cpp`, moved
out of `src/hydra/`), so both front ends share its worker pool, material
templates (one program for a group of compatible materials), folded class
parameters (`PRISM_MDL_COMPILE_MODE`), process caches and on-disk caches.
glTF and OBJ materials still compile one document after another with
unfolded class parameters (they have no Hydra network to hand over).

Left: UsdSkel blend shapes and skinned normals (authored normals of skinned
meshes are not skinned), motion blur, and native instancing's numbering
(prim and instance ids follow the CLI's traversal; the delegate numbers its
propagated prototypes its own way, so the parity test compares prim ids up
to a renumbering). The CLI keeps polygon meshes in world space, so
coplanar overlaps and vertex-interpolated tangent frames differ from the
delegate's at noise level (see the parity table).

glTF loads only in the CLI ([gltf.md](gltf.md)): Hydra and usdview need a
glTF file-format plugin (none in the OpenUSD build), which could reuse the
MaterialX converter. Material variants, thin-walled transmission and the
other gaps listed in gltf.md are open. OBJ is in the same position
([obj.md](obj.md)): CLI only, MTL through a MaterialX `open_pbr_surface`
converter; `anisor`, `-mm`/`-t` map options, reflection maps and vertex
colours are open; `disp` maps displace their faces at load time.

## Not worth porting from the OptiX path tracer

The older OptiX tracer (`joaovbs96/OptiX-Path-Tracer`) has nothing Prism lacks
apart from what this round added (area-light NEE): its BSDFs are superseded by
MDL, it picks lights uniformly, has no environment importance sampling, no
denoiser and no notable performance techniques.
