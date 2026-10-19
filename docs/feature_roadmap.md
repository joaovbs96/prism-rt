# Feature roadmap

What the test scenes still need after the lighting, SSS, subdivision,
displacement, primvar and light-linking round, in rough priority order.
Scenes: **Eggs** (MaterialEggs), **OpenPBR** (OpenPBRShaderPlayground),
**Teapot/Lion** (MaterialXTeapotLion), **Stirling**, **ALab**, **Chess**
(OpenChessSet). They live next to each other in `assets/`.

## Color management

- **ACEScg rendering space.** The Eggs references are rendered in ACEScg and
  several scenes author ACEScg textures and documents (Eggs, Stirling RenderMan
  stage, ALab). Prism renders in linear Rec.709: MaterialX converts texture
  color spaces into it, but the light and dome colors, the comparison with the
  references and the output need a selectable working space.

## Materials

- **MaterialX 1.39.5.** Stirling's car paint uses `flake3d`, which the local
  MaterialX 1.39.4 lacks (usdMtlx drops the node, so the paint renders without
  flakes). Updating MaterialX does not fix the MDL layer-over-mix limitation;
  the loader works around that (`docs/materialx_compatibility.md`).
- **usdMtlx `dielectric_bsdf` lookup.** USD's usdMtlx reader fails to find the
  node definition of some Stirling `dielectric_bsdf` nodes (`Unable to find the
  nodedef`), so their layers lose the coat. The failure happens before Prism
  sees the network; it needs investigating on the USD side.
- **Procedural noise parity.** The Eggs' noise-driven patterns (ore swirl
  stripes, displacement relief) differ from the Karma references: Karma
  evaluates MaterialX noise in its own implementation, Prism in MaterialX's MDL
  one.
- **NEE inside media and nested media.** The random walk samples no lights
  while inside a medium and tracks a single medium (no glass in liquid).
- **Textured emission as lights.** Only constant-emission triangles are
  sampled by NEE; textured emitters rely on BSDF sampling.
- **Material edits of displacing materials** rebuild bindings but not the
  displaced geometry until the next full snapshot.

## Lights

- **Shaping and IES profiles** (`ShapingAPI`; ALab's are empty files).
- **Light filters** and portal lights.
- **Dome texture formats** other than lat-long (angular, mirrored ball,
  cube maps) are read as lat-long with a warning.
- **Many-light sampling.** NEE picks lights by power from an alias table,
  then re-picks analytic lights by their estimated irradiance at the shading
  point (up to 64 of them; see [Sampling](gpu_devices.md#sampling)). Emissive
  triangles are still picked by power alone, and BSDF rays test every
  analytic light; scenes with hundreds of lights want a light BVH.
- **Solid-angle sampling of emissive triangles and disk lights** (spherical
  triangles, Arvo 1995); both are area-sampled. On CornellBox direct light is
  under 1% of the noise, so it was not pursued.

## Geometry

- **Curves** (`BasisCurves`): ALab's fur and cloth (baked procedurals package).
  Embree has curve primitives; the GPU devices need a procedural intersector.
- **Volumes** (`Volume` + OpenVDB): the Eggs' jade egg interior.
- **Points** and particle fields (ALab's optional Gaussian splats).
- **Motion blur**: camera shutters and time-sampled transforms (Eggs, ALab).
- **Adaptive subdivision and dicing.** Subdivision is uniform per mesh under a
  face budget; dense subdivision meshes render as their control hulls, and
  displacement dicing is limited the same way. A scene-wide budget
  (`PRISM_SUBDIVISION_BUDGET_FACES`, default 24 Mi faces) lowers the levels of
  the meshes refining to the most faces first: ALab asks for 48 M faces at
  level 2 and renders with 25 M (40 M triangles, 25 GB peak).
- **Multiple UV sets.** Texture coordinates come from `st` (or `uv`);
  `geompropvalue` reads of other float2 primvars work, but MDL
  `texture_coordinate(i)` beyond the first set does not.

## Scale and performance

- **Out-of-core textures for the GPU.** Images that do not fit the GPU budget
  are uploaded at reduced resolution; ALab's 78 GB 4K texture pack needs
  streaming or virtual texturing.
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
    2. Done on Vulkan (see [GPU devices](gpu_devices.md#large-material-sets-callable-shaders-vulkan)):
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
    5. Open: make callables the default for every Vulkan scene (faster on the
       scenes measured, and the compute kernel's 20–40 s cold compile goes
       away), and port the same split to Metal with visible function tables.
  - Large scenes like ALab also need texture streaming on the GPU: feedback
    of the needed texture, mip and tile from shading, loaded from the already
    tiled `.tx.exr` files into a resident tile pool. Whole mip levels first,
    tiles later. They may also need acceleration-structure compaction.
- **Denoising** (e.g. Intel Open Image Denoise).

## Standalone CLI

The `PrismRT` CLI loads USD through its own path (`src/mesh/usd_mesh.h`): it
reads analytic lights and combines domes, but it has no subdivision,
displacement, named primvars, light linking or UsdPreviewSurface translation.
Hydra (`prism_hydra_render`, usdview) is the reference path.

## Not worth porting from the OptiX path tracer

The older OptiX tracer (`joaovbs96/OptiX-Path-Tracer`) has nothing Prism lacks
apart from what this round added (area-light NEE): its BSDFs are superseded by
MDL, it picks lights uniformly, has no environment importance sampling, no
denoiser and no notable performance techniques.
