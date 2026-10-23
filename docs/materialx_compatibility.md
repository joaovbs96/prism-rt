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

A layer whose top is itself a layer hands the base on to that layer's base,
which loses it the same way when that base is a mix (the Lion's dust over
sheen over glaze). Layering is associative, so the loader first rewrites

    layer(layer(top, middle), base) = layer(top, layer(middle, base))

and then distributes the exposed mix.

## Thin-walled open_pbr_surface

MaterialX 1.39.4's `open_pbr_surface` graphs (the genmdl variant included) use
`geometry_thin_walled` only to pick the subsurface lobe and never pass it to
their `<surface>` constructor, so the generated MDL material is always closed
(`thin_walled: false`). 1.39.5 (OpenPBR 1.1.1, one graph for every target)
passes it on, but still leaves the material without a backface shader and
keeps the `transmission_depth` medium, so the repair below still applies. Thin-walled glass then refracts like a
solid and fills with the `transmission_depth` medium; on a single-sided or open
mesh (a bottle, a jar) that medium has no inside, and a `transmission_color`
meant as the sheet's tint becomes a strong absorption through the whole object.

The loader expands each `open_pbr_surface` whose `geometry_thin_walled` is a
constant `true` into its genmdl implementation graph (`flattenSubgraphs`,
restricted to that node), sets `thin_walled` on the graph's surface
constructor, and binds the material's `backsurfaceshader` to the same shader
(a thin-walled MDL material scatters with its backface BSDF from behind).
OpenPBR's thin wall is an infinitesimal sheet whose normal-incidence
transmittance is `transmission_color` whatever the depth, while the graph uses
`transmission_color` as the tint only when `transmission_depth` is zero, so the
depth is cleared on these nodes. A connected `geometry_thin_walled` is left as
it is (MDL's `thin_walled` is uniform).

## Refraction IOR of uncoated open_pbr_surface transmission

MaterialX's MDL `dielectric_bsdf` gives the material its own IOR, and the coat
is OpenPBR's outermost dielectric: MDL refracts the transmission with
`coat_ior` (1.6 by default) rather than `specular_ior`, whether or not there is
a coat. Without a coat (`coat_weight` 0) `coat_ior` has no other effect, so the
loader sets it to the constant `specular_ior` on transmissive surfaces. A
coated surface keeps the mismatch (MDL has a single, uniform IOR), and so does
a varying `specular_ior`. Material templates keep `specular_ior`, `coat_ior`,
`coat_weight` and `transmission_weight` structural on transmissive OpenPBR
surfaces, so a bound variant never refracts with its seed's IOR.

## gltf_pbr volume attenuation

MaterialX 1.39.5's `gltf_pbr` graph builds the `KHR_materials_volume` medium
(an `anisotropic_vdf` absorbing `ln(attenuation_color) / attenuation_distance`)
but never connects it, so attenuation is dropped and thick glTF glass renders
clear. `open_pbr_surface` layers its transmission over its VDF, which the MDL
generator turns into the material's `volume`. The loader expands each
`gltf_pbr` with a finite `attenuation_distance` into its genmdl graph and adds
that layer (`LayerGltfPbrVolume`), so the integrator traces the absorbing
medium as it does for OpenPBR glass.

## Deliberate non-repairs

Two differences from the OpenPBR Shader Playground's Arnold reference render
are intentional; don't "fix" them in preprocessing.

- **Shader overrides vs interface connections.** The asset's
  `material_assignment.usda` overrides `open_pbr_surface` inputs (for example
  the bottle's `coat_weight` 0 and `coat_ior` 1), but usdMtlx connects those
  inputs to the material interface, which carries the `.mtlx` values. Under USD
  composition a connection wins over an authored value, so every USD renderer
  sees the `.mtlx` values. The Arnold render came from the original Maya scene.
  PrismRT renders what USD composes; this is an asset issue (compare its
  "value and connection conflict" issue #5).
- **Coat darkening on transmission.** MaterialX's `open_pbr_surface` estimates
  the base albedo from `base_color` and `subsurface_color`, ignoring
  `transmission_weight`, and then darkens the whole base, transmitted light
  included. Coated glass is therefore darkened and tinted by `base_color`. The
  OpenPBR specification doesn't settle whether darkening applies to transmitted
  light. PrismRT follows MaterialX here rather than patching its internal graph;
  reporting it upstream is the right fix.

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
conductor clamp, `materialx_loader_types` the color space names, input type
repairs and layer distribution, and `materialx_loader_transmission` the
thin-walled expansion and the refraction IOR. `prism_openpbr_transmission_test`
(main build) samples compiled OpenPBR glass: thin walls transmit undeflected and
tinted, closed glass refracts with `specular_ior` and carries its medium. The small standalone test project avoids loading the renderer
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
