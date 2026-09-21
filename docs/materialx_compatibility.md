# MaterialX compatibility preprocessing

`DocumentLoader::load` applies renderer fallbacks to the in-memory authored
document after XML loading and before importing the shared standard library,
validation, and MDL generation. It does not rewrite source `.mtlx` files.
The SSS fallback and the conductor clamp live side by side with independent
switches.

## SSS-to-diffuse fallback

PrismRT currently samples MDL surface BSDFs without integrating volume scattering
or absorption. MaterialX's MDL implementation of subsurface scattering uses both
a transmitting surface and interior volume coefficients. Ignoring the volume
while retaining that surface makes SSS objects appear transparent; it does not
provide a diffuse approximation automatically.

The loader therefore defaults to the following compatibility substitutions:

| MaterialX node category | Input replaced with constant zero |
| --- | --- |
| `standard_surface` | `subsurface` |
| `open_pbr_surface` | `subsurface_weight` |

This selects the surface model's ordinary diffuse branch instead of its SSS
branch. Specular, metalness, base color, roughness, normal mapping, and explicitly
authored transmission are preserved. Glass pawn tops therefore remain glass.
The approximation uses the model's existing diffuse color, not a reconstruction
of its subsurface color or scattering radius. It provides opaque diffuse shading
where appropriate, not physically correct subsurface transport.

The operation handles top-level nodes and nodes inside authored nodegraphs.
It removes `nodename`, `nodegraph`, `output`, and `interfacename` bindings before
setting the value: merely adding `value="0"` does not override a connected input.
An absent weight is also explicitly set to zero. Imported standard-library nodes
are added afterwards so their implementation graphs and the shared library are
not changed. Other node categories, custom SSS implementations, and direct MDL
materials are not covered by this fallback.

Every affected surface node is reported as `[MaterialX] SSS-to-diffuse fallback`.
`MDLMaterial` uses the loader's default, so both direct `.mtlx` loading and the
USD-to-MaterialX loading path receive the fallback. Calling `create_empty()` and
then building a document manually does not run preprocessing.

When volume transport is implemented, opt out explicitly:

```cpp
DocumentLoader loader(stdlib, /*clamp=*/false,
                      /*fallback_sss_to_diffuse=*/false);
```

Do not remove the fallback merely because the MDL backend compiles volume fields:
the path integrator must actually consume those fields. Keep it enabled until
SSS surface/volume transport has been validated end to end.

## standard_surface conductor clamp

Upstream MaterialX bug: `standard_surface` feeds `base_color` and
`specular_color` into `artistic_ior` as reflectivity and edge color. The MDL
implementation clamps reflectivity internally but not the edge color, so a
`specular_color` component above one (physicallybased.info gold uses 1.001)
extrapolates the conductor IOR below zero. The symptom is a green disk at
normal incidence on an otherwise gold sphere.

With `clamp=true` (what `MDLMaterial` passes), the loader limits exactly those
two inputs on `standard_surface` nodes to `[0, 0.9999]`, including nodes inside
authored nodegraphs:

- A constant already in the working space (`lin_rec709`, `none`, or untagged)
  is clamped in place.
- Anything only known later — texture/graph connections, interface bindings, or
  constants in another color space such as `acescg` — has its binding moved onto
  an inserted `clamp` node. Color management converts to linear Rec.709 *after*
  document preprocessing and can push an in-range ACEScg value above one, so a
  value clamp alone would not be sufficient there.

Other inputs (e.g. `emission_color`) and other shading models are untouched.
`open_pbr_surface` uses a Schlick conductor and renders the same gold values
correctly without a clamp. The `base` and `specular` float weights are not
clamped; authoring them above one can still reach the same failure.

## Regression checks

`materialx_loader_sss` covers the SSS fallback and `materialx_loader_clamp` the
conductor clamp. The small standalone test project avoids loading the renderer
and its USD/MDL runtime dependencies:

```text
cmake -S tests -B build/sss_tests -DMaterialX_DIR=<install>/lib/cmake/MaterialX
cmake --build build/sss_tests --config Release
ctest --test-dir build/sss_tests -C Release --output-on-failure
```

It checks constant and connected weights, nodegraph outputs, nested OpenPBR
interface bindings, absent weights, opt-out behavior, unchanged transmission and
base color, source-file preservation, and untouched imported/shared library data.

## Reusing referenced materials during USD loading

Within one USDMesh load, successful referenced `.mtlx` compilations are reused
by `(weakly canonical file path, MaterialX material name)`. Each USD material prim
still receives its own lookup entry, pointing to the shared renderer material ID.
The existing owning vector holds the compiled material once; no extra ownership
or render-time cache synchronization is required.

The cache is local to the load, where SDK context, libraries, and preprocessing
options are fixed. It does not persist across reloads, deduplicate different
files with matching names, cache failures, or merge recovered UsdShade documents
whose composed parameters might differ. Failed path canonicalization simply
skips caching. This optimization preserves the current referenced-file import
semantics; it does not address the separate USD material-override limitation.

Independently of that cache, all materials share one `MDLGenerator` per
libraries path (`MDLGenerator::shared`), and USD material collection reuses its
standard-library document. The MaterialX libraries are therefore loaded once per
process rather than once per material. Generation through the shared instance is
serialized with a mutex because its `GenContext` is shared state.

The loading summary reports compiled, reused, and bound counts separately.
This improves material-loading work, not path-tracing convergence. Camera rays
already use independent uniform subpixel jitter in `Camera::sample_square`, called
by `get_ray` for every sample; no additional jitter is needed or added here.
