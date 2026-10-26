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
default. The `prism:sssFallback` render setting (CLI `--sss-fallback true`;
its default comes from `PRISM_SSS_FALLBACK=1`), or passing
`fallback_sss_to_diffuse=true`, restores the old substitution, for comparison
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

The loader gives each `open_pbr_surface` whose `geometry_thin_walled` is a
constant `true` a [private copy](#private-graphs) of its genmdl implementation
graph, sets `thin_walled` on that graph's surface constructor, and binds the
material's `backsurfaceshader` to the same shader (a thin-walled MDL material
scatters with its backface BSDF from behind). The backends see that
`backface.scattering` is `surface.scattering` (equal slot hashes) and compile
it once: the CPU and GPU backface use the surface's function (a second copy
made thin-walled GPU programs 1.9x larger).
OpenPBR's thin wall is an infinitesimal sheet whose normal-incidence
transmittance is `transmission_color` whatever the depth, while the graph uses
`transmission_color` as the tint only when `transmission_depth` is zero, so the
depth is cleared on these nodes. A connected `geometry_thin_walled` is left as
it is (MDL's `thin_walled` is uniform).

`standard_surface`'s graph uses `thin_walled` only to pick translucency over
subsurface scattering and has no `thin_walled` on its constructor, so
thin-walled `standard_surface` glass refracted like a solid (and lost energy
leaving it, R + T 0.64 at 41 degrees inside). Autodesk Standard Surface
defines a thin wall as an infinitely thin shell that transmits without
refraction (and scatters subsurface light as translucency), which is MDL's
`thin_walled`: a constant `true` gets the same thin-walled graph when the node
transmits or scatters (`transmission` or `subsurface` not zero; an opaque one
renders the same either way). The graph has no transmission medium.

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
generator turns into the material's `volume`. The loader gives each
`gltf_pbr` with a finite `attenuation_distance` a [private copy](#private-graphs)
of its genmdl graph with that layer (`LayerGltfPbrVolume`), so the integrator
traces the absorbing medium as it does for OpenPBR glass.

## Private graphs

The repairs that change a surface's graph (thin walls, glTF volume, the
dielectric coupling below, transparent UsdPreviewSurface, Lama) rewrite a
private copy of the node's genmdl implementation, not the material: the
loader adds a node definition and graph cloned from the library's to the
document (`ClonePrivateGraph`), rewrites the copy, names both by a hash of the
rewritten graph (`ND_prism_<category>_<hash>`, `PublishPrivateGraph`) and
points the node at it; the node keeps its inputs. A repair that finds an
unexpected shape returns the node to the library's graph.

MaterialX's MDL generator emits a graph implementation as an MDL function
whose internal constants are literals, but publishes every unconnected input
of the material's own nodes as a material parameter. The repairs used to
flatten the surface into the material (`flattenSubgraphs`), which made its
~100 internal constants class-compilation parameters (OpenPBR glass: 150-180
against 40-80), among them the uniform `scatter_mode` of every
`dielectric_bsdf`: `mx_dielectric_bsdf` selects its R, T or RT lobe by it, so
every lobe kept all three variants. The CLI compiles classes unfolded, and
its repaired OpenPBR programs were 3.5x the library's (playground: 14 of 22
GPU programs at 7-15 MB of SPIR-V instead of 2 MB, a 412 s cold Vulkan
start). With the private graphs the parameters are the surface's inputs again
and the programs the library's size (all 22 at 1.9-2.2 MB). Hydra folds
constant parameters (`PRISM_MDL_COMPILE_MODE=auto`) and was never affected.
Materials with the same repair share the graph, so they still share one class
program; a decision a repair takes from the node's values (a constant zero
thin-film weight drops the film's RT lobe) makes another graph.

## Energy loss of layered dielectric transmission

Upstream (MaterialX's MDL library and its surface graphs, 1.39.5 with MDL SDK
2026.0.2). `open_pbr_surface`, `standard_surface` and `gltf_pbr` all build
their dielectric as a reflection-only lobe layered over a mix of a
transmission-only lobe and the opaque base:

    layer(R, mix(T [over its VDF], opaque, transmission))

MaterialX's MDL `mx_dielectric_bsdf` (pbrlib_1_9) turns the layer into
`df::custom_curve_layer` with a Schlick curve (normal reflectivity
`((ior-1)/(ior+1))^2`, grazing `1 - roughness`). libbsdf weights a custom-curve
base by `1 - weight * max(curve(cos k1), curve(cos k2))`
(`curve_layer_sample`/`_evaluate` in `libbsdf.cpp`), and since the curve has
no IOR it takes `k2`'s cosine as is. For light leaving the medium `k2` is the
refracted direction, more grazing than `k1`, so the transmitted light is cut by
`curve(k2) - curve(k1)` that is never reflected either. Smooth IOR 1.5 glass,
sampled from inside (`prism_dielectric_albedo_test -v`):

| inside angle | 0 | 20 | 40 | 60 (TIR) |
|---|---|---|---|---|
| R + T before | 1.000 | 1.000 | 0.795 | 1.000 |
| R + T after | 1.000 | 1.000 | 1.000 | 1.000 |

From outside R + T is 1, with Schlick's R instead of Fresnel's (0.067 instead
of 0.089 at 60 degrees). Rough glass loses more (`specular_roughness` 0.3:
R + T 0.74 at 85 degrees outside, 0.67 at 85 degrees inside). The three shading models measured
the same. In the white furnace (`tests/hydra/fixtures/glass_furnace.usda`) the
closed glass sphere rendered 3% dark (image mean 0.987, worst pixel 0.79),
with MIS and with BSDF-only environment sampling alike.

MaterialX's own `dielectric_bsdf` with `scatter_mode` `RT` (a GGX
`scatter_reflect_transmit` lobe) splits by its exact Fresnel term, including
total internal reflection, and conserves energy. Layering distributes over the
mix, so the loader rewrites a [private copy](#private-graphs) of each
transmissive surface's genmdl graph as

    mix(RT [over its VDF], layer(R, opaque), transmission)

(`CoupleSurfaceTransmissionFresnel`; `LayerGltfPbrVolume` couples the glTF
graphs it copies). R and T share IOR and roughness in all three graphs
(`standard_surface` only without `transmission_extra_roughness`, and with
`specular` 1), so every closed transmissive surface is rewritten; how depends on
what else is layered on the dielectric:

- **Thin films** (`open_pbr_surface` `thin_film_weight`, `standard_surface`
  `thin_film_thickness`, `gltf_pbr` `iridescence`): the RT lobe gets the
  reflection lobe's film (a mix of filmed and plain RT lobes by the film
  weight). libbsdf applies a film around an RT lobe to its Fresnel term with
  R + T = 1, so this is exact; the film's reflectance becomes the film over
  Fresnel's term instead of over the Schlick curve.
- **Tints**: one RT lobe has one tint, so differently tinted lobes
  (`transmission_color` at depth 0, `standard_surface`'s `transmission_color`
  — its graph has no medium —, glTF's `base_color`, a tinted `specular_color`)
  go through an in-document helper BSDF, `prism_dielectric_split`, with
  reflection tint s and transmission tint t:

      min(s, t) * RT + (s - min) * fresnel_layer(R) + (t - min) * fresnel_layer(base: T)

  RT is untinted; `df::fresnel_layer` weights a bare reflection lobe by its
  Fresnel term and a transmission base by 1 - F with the refracted half vector,
  so smooth lobes are exact (R = s F, T = t (1 - F), total internal reflection
  included). Rough tinted glass is approximate: R + T is 0.92 of the expected
  at 85 degrees outside with a transmission tint, 0.87 at 40 degrees inside
  with a tinted `specular_color` (before: 0.70 and 0.84), and is logged: the
  Fresnel layer weights a rough lobe's base by the heuristic above, and
  libbsdf compensates multiple scattering only for reflecting GGX lobes, not
  for a transmit-only one (`df::fresnel_layer` over rough transmission alone
  loses more than MaterialX's curve layer), so only the RT lobe is exact when
  rough. With a transmission tint (s = 1 >= t) the rough transmit-only term
  vanishes.
- **Coats**: a smooth coat over smooth glass of the same IOR
  (`coat_roughness` and `specular_roughness` 0, `coat_ior` equal to
  `specular_ior`; `standard_surface` `coat_IOR` and `specular_IOR`, both 1.5 by
  default; `gltf_pbr` `clearcoat_roughness` and `roughness` 0, `ior` 1.5)
  becomes `prism_fresnel_layer`, `df::fresnel_layer` with the coat's IOR and
  weight over the coat's base: light refracted with the same IOR keeps exactly
  1 - F. MDL refracts with one material IOR, so a coat of another IOR
  (OpenPBR's default `coat_ior` is 1.6) can't be a Fresnel layer: from inside,
  its Fresnel term reflects totally at angles the glass transmits, which
  trapped light in closed glass (a furnace sphere rendered 0.64 at its
  center). Rough coats or glass keep the custom-curve coat too: a Fresnel layer
  gains energy there (R + T 1.19 at 40 degrees inside, rough coat over smooth
  glass) or loses more than the curve (0.66, smooth coat over rough glass). The
  coat over the opaque part also becomes Fresnel's rather than Schlick's.
  Coupled coated OpenPBR glass reflects at both interfaces with the material
  IOR (R 0.078 at normal incidence with `coat_ior` equal to `specular_ior`,
  where OpenPBR's index-matched glass under the coat reflects nothing, R 0.04):
  the RT lobe can't take OpenPBR's IOR relative to the coat. The coupling did
  this before the coat repair as well.
- **Not repaired, logged**: coats of another IOR (R + T 0.946 at 60 degrees
  inside; 0.87 of the light entering a closed sphere at 85 degrees leaves it),
  thin films over tinted glass (0.55 of the expected
  at 41 degrees inside), rough coats (0.74 at 85 degrees, mostly the rough
  layering loss of opaque surfaces below), coat darkening, OpenPBR fuzz (0.75 at
  41 degrees inside) and glTF sheen (0.63): libbsdf weights a sheen layer's
  base by `1 - max(rho(k1), rho(k2))`, the same heuristic, and MDL has no other
  sheen layer.

Measured with `prism_dielectric_albedo_test -s` (IOR 1.5, 20000 samples,
lowest R + T over 0 to 85 degrees, with its angle; tinted cases relative to
R = s F + t (1 - F)):

| configuration | outside before | after | inside before | after |
|---|---|---|---|---|
| smooth glass, `specular_color` 0.9999 (split) | 0.9999 | 0.9998 | 0.639 (41) | 0.9998 |
| rough glass (0.3), `specular_color` 0.9999 | 0.737 (85) | 0.999 | 0.672 (85) | 0.999 |
| tinted transmission, depth 0 (all three) | 0.992 | 0.995 | 0.574 (41) | 0.995 |
| tinted `specular_color` (1, 0.5, 0.5) | 0.986 | 0.998 | 0.717 (41) | 0.996 |
| tinted transmission, rough 0.3 | 0.700 (85) | 0.922 (85) | 0.580 (85) | 0.978 |
| thin film / iridescence (all three) | 0.992 | 1.000 | 0.615 (41) | 1.000 |
| thin film, rough 0.3 | 0.734 (85) | 0.999 | 0.671 (85) | 1.000 |
| smooth coat of the glass's IOR, `coat_darkening` 0 | 1.000 | 1.000 | 0.946 (60) | 1.000 |
| `standard_surface` coat / glTF clearcoat | 1.000 | 1.000 | 0.778 (41) | 1.000 |
| `standard_surface` `thin_walled` | 1.000 | 1.000 | 0.639 (41) | 1.000 |
| `UsdPreviewSurface` opacity 0, roughness 0 | 1.000 | 1.000 | 0.639 (41) | 1.000 |
| `LamaDielectric`, smooth / rough 0.3 | 1.000 / 0.737 | 1.000 / 0.999 | 0.639 / 0.672 | 1.000 / 1.000 |
| `LamaGeneralizedSchlick` (defaults) | 0.040 | 1.000 | 0.040 | 1.000 |

Of the light entering a closed smooth sphere at 75 to 85 degrees, 0.74 to
0.88 left it before (paths through a sphere keep their incidence angle) and
1.000 after, for every repaired smooth case. After the repair the furnace
renders the dome (mean 1.0000, worst pixel 0.004 at 16 spp). Thin-walled surfaces don't refract and don't lose energy.
Material templates keep the weight and the inputs these decisions read
structural, so a variant never binds onto a prototype rewritten for its seed's
values. A draft upstream report proposes the RT form for the graphs.

The two helper BSDFs are a node definition and an inline genmdl
implementation (`sourcecode`, an MDL material expression) added to the
document that uses them, `ND_prism_fresnel_layer` and
`ND_prism_dielectric_split`; MaterialX has no node for an MDL Fresnel layer.

### UsdPreviewSurface

With `opacity` below one in transparent mode (`opacityMode` 0, no
`opacityThreshold`, which makes opacity a cutout) MaterialX's
`UsdPreviewSurface` graph (the one USD ships) refracts through a
`dielectric_bsdf` T with `ior`:

    layer(generalized_schlick R (F0 from ior), mix(diffuse, T, opacity))

which MDL turns into a `color_custom_curve_layer` with the same exit loss. The
graph's transmission is always smooth while the specular lobe has
`roughness`, so only a smooth specular agrees with it: such surfaces
(`roughness` 0, `metallic` 0, metalness workflow, no clearcoat) are rewritten
as `mix(layer(R, diffuse), RT, opacity)`; the opaque part keeps its Schlick
reflection. A rough specular has no exact form: an RT lobe of that roughness
would conserve energy but blur the refraction, a large visual change (the
input's default is 0.5), so rough transparent surfaces keep the graph and its
loss (R + T 0.71 at 85 degrees outside, 0.64 at 41 inside) and are logged, as
are the metallic and specular workflows' tinted reflections and the clearcoat.

### Lama

`LamaDielectric` layers its reflection over its transmission over the interior
VDF, both `dielectric_bsdf` with the same IOR, roughness and frame; with equal
`reflectionTint` and `transmissionTint` (MaterialX applies each to its lobe on
both sides) it becomes `layer(RT, VDF)`, exact for rough lobes too. Differing
tints are logged.

`LamaGeneralizedSchlick`'s reflection is `multiply(R, reflectionTint)` on top
of a layer, and MaterialX's MDL generator drops a layer's base under a
`multiply` top, so MDL rendered the reflection only (R + T 0.04). When the
curve is Fresnel's Schlick approximation (`reflectivity90` white,
`reflectivityProfile` 0.2, a constant gray reflectivity or IOR) and the tints
agree, the node becomes the RT dielectric of that IOR over the interior
(exact; its reflection becomes Fresnel's). Otherwise the tint moves into the
reflection lobe's `color0` and `color90` (a Schlick curve is linear in them)
and the lobe becomes the layer's top, which restores the transmission; that
keeps the exit loss and the graph's second Schlick weight on its transmission
lobe (T 0.92 at normal incidence) and is logged.

## Energy loss of opaque layered dielectrics (not repaired)

The same libbsdf layering darkens opaque surfaces. A white Lambertian base
under the specular layer (`open_pbr_surface` `base_color` 1,
`standard_surface` `base_color` 1) returns R = 0.954 at normal incidence; in a
white furnace a smooth white sphere renders 0.956 at its center (0.960 with
`specular_roughness` 0.3) instead of the dome's 1. The OpenPBR specification
requires 1 here (its glossy-diffuse slab is `E_spec + (1 - E_spec) C`, "which
guarantees that a white C passes the furnace test"), and so does MaterialX's
albedo-scaling `layer` in its GLSL and OSL code. Two mechanisms, both in the
curve layer's base weight `1 - max(c(k1), c(k2))`:

- **Smooth, all angles**: the `c(k2)` term attenuates the diffuse light leaving
  through the interface but nothing returns the part reflected back into the
  base (no interreflection, the price of a reciprocal weight). The loss is the
  cosine-weighted mean of the curve over k2: R = 0.9543 at 0 degrees with
  MaterialX's Schlick curve, computed and measured alike (0.948 with MDL's
  `fresnel_layer` and the true IOR, whose mean Fresnel term is larger).
- **Rough, at grazing angles**: the base is cut by the curve at the macro
  normal, `c(n.k1)` (0.59 at 85 degrees, the grazing reflectivity being
  1 - roughness), while the rough lobe reflects only
  the curve averaged over its microfacets (0.33; the GGX lobe itself returns
  1.00 with its multiscatter term): R = 0.738 at 85 degrees with
  `specular_roughness` 0.3.

MDL has no layer that weights a base by the top's directional albedo
(`fresnel_layer`, `custom_curve_layer`, `measured_curve_layer` and sheen all use
the same heuristic), so an exact repair needs libbsdf; see the upstream draft.
Brightening the base (about 1.05 on the diffuse weight) would fix normal
incidence only and gain about 1.7% at grazing angles, on nearly every opaque
material, so it is not done.

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
  included. Coated glass is therefore darkened and tinted by `base_color`:
  smooth coated glass with the default `base_color` 0.8 keeps R + T 0.76 at
  normal incidence. The
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
thin-walled expansion and the refraction IOR, `materialx_loader_fresnel` the
coupled transmission graph, `materialx_loader_dielectric` (against MaterialX's
own library graphs, from the MaterialX install) the split, thin-film, coat,
thin-walled `standard_surface`, `UsdPreviewSurface` and Lama rewrites.
`prism_openpbr_transmission_test`
(main build) samples compiled OpenPBR glass: thin walls transmit undeflected and
tinted, closed glass refracts with `specular_ior` and carries its medium.
`prism_dielectric_albedo_test` (main build) samples compiled smooth glass (a
plain RT `dielectric_bsdf`, the three surfaces plain, tinted, filmed and
coated, thin-walled `standard_surface`, `UsdPreviewSurface`, `LamaDielectric`,
`LamaGeneralizedSchlick`) from both sides at six angles: R + T must be 1 (or
s F + t (1 - F) when tinted) and, where Fresnel's applies, R Fresnel's, and
light entering a closed sphere must leave it (paths trapped by a total
internal reflection that disagrees with the refraction show only there);
`-v [case]` prints the table, `-s [case]` surveys rough, partly repaired and
opaque models as well, and `-f file.mtlx` surveys a document. The
`prism_scene_features_glass_furnace_*` checks require the furnace to render the
dome (mean within 0.001, worst pixel within 0.02). The small standalone test project avoids loading the renderer
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
