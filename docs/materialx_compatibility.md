# MaterialX compatibility preprocessing

`DocumentLoader::load` applies renderer fallbacks to the in-memory authored
document after XML loading and before importing the shared standard library,
validation, and MDL generation. It does not rewrite source `.mtlx` files.
The SSS fallback and the conductor clamp live side by side with independent
switches.

## SSS-to-diffuse fallback (opt-in)

Subsurface scattering is traced as a random walk through the MDL volume
coefficients (`volume.absorption_coefficient`, `volume.scattering_coefficient`;
see `shaders/integrator/path_tracer.h`), so the loader no longer removes it by
default. Setting `PRISM_SSS_FALLBACK=1` (or passing
`fallback_sss_to_diffuse=true`) restores the old substitution, for comparison
or for scenes where the walk is too expensive:

| MaterialX node category | Input replaced with constant zero |
| --- | --- |
| `standard_surface` | `subsurface` |
| `open_pbr_surface` | `subsurface_weight` |

This selects the surface model's ordinary diffuse branch instead of its SSS
branch; everything else is preserved. It handles top-level nodes and nodes in
authored nodegraphs, removes `nodename`, `nodegraph`, `output` and
`interfacename` bindings before setting the value (a value does not override a
connection), and sets an absent weight explicitly. Imported standard-library
nodes are added afterwards, so the shared library is not changed.

## Color space names

Assets name color spaces after their OCIO configs (`srgb_tx`, `Raw`, `ACEScg`,
`Utility - sRGB - Texture`, `lin_srgb`, ...), which MaterialX does not know:
their textures would stay unconverted. The loader maps these aliases to
MaterialX's names (`srgb_texture`, `lin_rec709`, `acescg`, ...) on the document
and on every element (`CanonicalColorSpace` in `src/materialx/loader.cpp`).

## Input types

Documents written by exporters, and those hdMtlx regenerates from USD shading
networks, do not always type inputs as their node definitions declare:

- An input connected to a node output of another type (a `color3` image driving
  a `float` opacity) gets an `extract` (vector to float) or `convert` node.
- An input typed differently from its node definition is retyped to the
  declaration: vector values written as strings, flags written as integers,
  and shader inputs hdMtlx typed after the graph output feeding them (the graph
  output then gets the `extract`/`convert` node).

Without these repairs validation fails ("doesn't match declaration") and the
material renders as the magenta error material.

## Layers over mixes

MaterialX's MDL generator implements `layer` by passing the base BSDF into the
top BSDF's `base` parameter (`ClosureLayerNodeMdl.cpp`). A `mix` as the top
only works when one of its sides is empty; otherwise the base is dropped
(`// TODO handle mix, add, and multiply`, still the case in MaterialX 1.39.5),
which renders glazed or coated materials black. The loader distributes such
layers over the mix, which is equivalent:

    layer(mix(fg, bg, m), base) = mix(layer(fg, base), layer(bg, base), m)

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

`materialx_loader_sss` covers the SSS fallback, `materialx_loader_clamp` the
conductor clamp, and `materialx_loader_types` the color space names, input type
repairs and layer distribution. The small standalone test project avoids loading the renderer
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

## MDL module naming

The generator names a module after its material alone (`::materialx::Gold`).
The MDL database is global, and `load_module_from_string` reports a duplicate
name as success while keeping the *first* definition, so two documents defining
the same material name would silently share one shader. `MaterialCompiler`
therefore suffixes the module name with a 64-bit FNV-1a hash of the weakly
canonical document path plus the material name, e.g.
`::materialx::M_Pawn_Top_B_6343e2a651b69193`. FNV-1a rather than `std::hash`
keeps names, and the MDL dump filenames derived from them, stable across runs.
A remaining duplicate is logged as `[WARN] Module ... was already loaded`.

Recovered UsdShade documents are written to a temp file named after the full
prim path rather than the leaf name, so same-named materials in different
scopes get distinct files and hence distinct modules.

The loading summary reports compiled, reused, and bound counts separately.
This improves material-loading work, not path-tracing convergence. Camera rays
already use independent uniform subpixel jitter in `Camera::sample_square`, called
by `get_ray` for every sample; no additional jitter is needed or added here.
