# Volumes: current state and options

PrismRT has participating media, but no volume shaders or volume geometry. This
note records what exists, what is missing and what each step would take, so the
work can be picked up later.

## What MaterialX provides

MaterialX defines the volume nodes and its MDL generator implements them all
(`libraries/pbrlib/genmdl/pbrlib_genmdl_impl.mtlx`, lines 43-82 in 1.39.4):

| Node | MDL implementation |
| --- | --- |
| `absorption_vdf` | `mx_absorption_vdf(absorption)` |
| `anisotropic_vdf` | `mx_anisotropic_vdf(absorption, scattering, anisotropy)`, Henyey-Greenstein |
| `volume` | `mx_volume(vdf, edf)` |
| `mix_vdf`, `add_vdf`, `multiply_vdf*`, `layer_vdf` | combinators |
| `volumematerial` | the material node that takes a `volumeshader` |

MaterialX does not block volumes: the generated MDL fills the material's
`volume` field. MaterialX 1.39.5 also adds further volume distribution
functions.

## What PrismRT does today

- **Only the `surface` and `displacement` terminals are read.**
  `SurfaceTerminalName` and `AddDisplacementShader` in
  `src/materialx/preview_surface.h`. A USD material's `volume`
  output is never looked at.
- **Media come only from the surface shader's own MDL volume coefficients.**
  OpenPBR/standard_surface transmission depth and subsurface turn into an
  absorbing or scattering medium (`src/mdl/backend_compiler.cpp`, the volume
  coefficients). A separate `volumeshader` would have to be generated and merged
  into the same MDL material as its `volume` field.
- **The integrator's media are basic** (`shaders/integrator/path_tracer.h`,
  random walk):
  - homogeneous only: constant density and colour inside a closed mesh;
  - a single medium at a time, so no nested media (glass containing liquid);
  - no light sampling (NEE) while inside a medium, so dense scattering converges
    slowly;
  - paths leave a medium through cutout holes correctly (fixed in 53ec45d).
- **No volume geometry.** `UsdVol` Volume prims with OpenVDB grids are skipped
  with a warning (the Hydra converter skips `volume` prims; the standalone CLI
  doesn't read them). MDL can't express spatially varying density, so
  heterogeneous volumes need their own data and sampling, independent of
  MaterialX.

## Options, in order

| Step | Scope | Effort | Notes |
| --- | --- | --- | --- |
| 1. Volume terminal | Read the `volume` terminal (or `volumematerial`), generate its VDF and merge it into the surface's MDL material as `volume`. Reuse the existing random-walk media. | Moderate | Homogeneous only: tinted glass interiors, milky liquids, fog inside a closed mesh. Both loaders (Hydra and CLI) and both devices. |
| 2. NEE inside media | Sample lights from scattering events inside a medium, with transmittance to the light. | Moderate | Needed for anything foggy to converge; also helps SSS. |
| 3. Nested media | A small medium stack with priorities, so glass and liquid interfaces work. | Moderate | Required for liquid in glass, ice in drinks and similar. |
| 4. Heterogeneous media | Delta or ratio tracking against a density field with a majorant. | Large | Prerequisite for step 5; needs a majorant grid on both devices. |
| 5. OpenVDB / NanoVDB Volume prims | Read `UsdVol` OpenVDB assets, convert them to NanoVDB for the GPU, and bound them with acceleration-structure proxies. | Large | Smoke, clouds, explosions; the MaterialEggs jade egg's interior. New dependency (OpenVDB/NanoVDB). |

Steps 1-3 build on what exists and keep CPU/GPU parity simple. Steps 4-5 are a
separate feature: new geometry and data structures on every device.

## Test scenes

- MaterialEggs: the jade egg's interior (OpenVDB), plus SSS eggs for step 2.
- ALab: optional volumetric procedurals.
- Small fixtures to add with step 1: a closed mesh with a homogeneous
  `anisotropic_vdf`, compared against an analytic transmittance and albedo.
