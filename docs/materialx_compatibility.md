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

## Document colour space through USD

A `.mtlx` file referenced into a USD stage reaches the renderer as a UsdShade
network (usdMtlx), not as the document. usdMtlx (OpenUSD 26.08) authors an
input's own `colorspace` as USD `colorSpace` attribute metadata, which UsdImaging
(`UsdColorSpaceAPI::ComputeColorSpaceName`) and hdMtlx carry into the generated
document; the document's `<materialx colorspace="...">` it keeps only as the
translated layer's `customLayerData["colorSpace"]`. Layer metadata does not
compose through a reference, so the stage's untagged colours arrived untagged
and were used as working-space (ACEScg) values, while the same document through
`--material`, glTF/OBJ or `MaterialDesc` converted them (the shader ball's marble
differed by 0.006 normalized RMSE, `two_materials.mtlx`'s red by 0.20).

The repair restores the document's space where the composed stage is still
visible (`src/usd/mtlx_color_space.h`):

- `MtlxDocumentColorSpace(UsdPrim)` takes the strongest layer of the Material
  prim's stack that has usdMtlx's file format (`mtlx`) and returns its
  `customLayerData["colorSpace"]`. Each Material takes its own document's, so a
  scope referencing several documents (the shader ball's examples: three
  `lin_rec709`, `bubblegum.mtlx` none) is handled per material.
- Both loaders publish it as the material prim data source
  `prismMtlxDocument.colorSpace`: the CLI in `UsdMaterialDataSource`
  (`src/mesh/usd_mesh.h`); Hydra through an API schema adapter for
  `MaterialXConfigAPI`, which usdMtlx applies to every Material it translates
  (`src/hydra/mtlx_color_space_adapter.cpp`, registered in hdPrism's
  `plugInfo.json`; it only adds that key, so other render delegates ignore it).
- The material service (`Prepare` in `src/materialx/material_service.cpp`) tags
  every network parameter that holds a colour or file value (`GfVec3f`,
  `GfVec4f`, `SdfAssetPath`) and has no colour space with it, before the network
  is captured. An input's own space wins, as MaterialX inherits it. vector3/4
  values share the Gf types; MaterialX applies a colour space to colour-typed
  inputs and colour images only, as with the document attribute.

Because the tags are in the captured network, every cache keyed by it (the
early material cache, conversion artifacts, the captured-material and template
registries) and the serialized document's content key include the space; a
document without `colorspace` (or a UsdShade material) gets no tags, so its
materials and images are unchanged.

Not repaired: a value authored over a referenced material in another layer
(an `over` in the stage) also takes the document's space, as the referenced
values do; a material whose nodes come from several `.mtlx` documents takes its
Material prim's document. Neither occurs in our assets.

Checks: `prism_mtlx_colorspace_test` (the prim and Hydra data sources, the
tags), `prism_parity_mtlx_document_colorspace_*` and
`prism_mtlx_document_colorspace_matches_tagged` (the referenced document,
CLI and Hydra, against the same network in UsdShade with `colorSpace` metadata,
`tests/hydra/fixtures/mtlx_document_colorspace/tagged.usda`: 0.104 normalized
RMSE before, bit-identical after), and `prism_material_preview_matches_stage`
(tolerance 0.02 to 1e-4).

## Tagged colours on uniform inputs

A few library inputs are `uniform`: `gltf_pbr`'s `attenuation_color` (the only
`color3` one in MaterialX 1.39.5) and `UsdUVTexture`'s `color4` `scale` and
`bias`. When such an input holds a constant in a color space other than the
working space (its own `colorspace`, or one inherited from the document), the
generator transforms it with a function call and makes the material parameter
varying (`Surface_attenuation_color_cm_in`), which cannot feed the uniform MDL
parameter: `C219 'NG_prism_gltf_pbr_<hash>' : cannot convert argument
'attenuation_color' from 'color' to 'uniform color'`, and the material renders
magenta. This happens whenever a transform is generated (the tag `lin_rec709`
or `srgb_texture` in the ACEScg working space), not when the tag is the working
space, and with or without folded class parameters.

`ConvertUniformColors` converts such constants into the working space in memory
and tags them with it, so no transform is generated. It does so for the spaces
the generator would transform (`lin_rec709` and those of the library's
`<space>_to_lin_rec709` nodes); `MDLMaterial` supplies the conversion:
`lin_rec709` through the working space's matrix (`ColorManagement::FromRec709`,
exactly as the glTF converter converts `KHR_materials_volume`'s attenuation
colour), other spaces through OpenColorIO. Untagged values, `none`, a tag equal
to the working space, connected inputs and varying inputs are left alone.

The material service binds a template's colours as authored, which would bind
such a colour unconverted, so a tagged `gltf_pbr` `attenuation_color` stays
part of the template structure (`IsCompatibleTemplateValue` in
`src/materialx/prepared_template.h`): materials that differ in it compile
separately. Not repaired: a uniform input bound through a nodegraph interface
input that carries the color space.

## Input types

Documents written by exporters, and those hdMtlx regenerates from USD shading
networks, do not always type inputs as their node definitions declare:

- An input connected to a node output of another type (a `color3` image driving
  a `float` opacity) gets an `extract` (vector to float) or `convert` node.
- An input typed differently from its node definition is retyped to the
  declaration: vector values written as strings, flags written as integers,
  and shader inputs hdMtlx typed after the graph output feeding them (the graph
  output then gets the `extract`/`convert` node).

- An integer input written as one of its declared `enum` names gets the
  name's `enumvalues` entry (or its index): USD authors UsdPreviewSurface's
  `opacityMode` as a token (`"transparent"`, `"presence"`), MaterialX 1.39.5
  declares an integer with those names.

Without these repairs validation fails ("doesn't match declaration") and the
material renders as the magenta error material.

A node that names no definition gets the one `Node::getNodeDef` would pick:
its category and output type, and its version (the default version when it
names none). The repairs above used to skip categories with several
definitions of the same type, which left `standard_surface` (versions 1.0.0
and 1.0.1) and `UsdPreviewSurface` documents unrepaired: an
`emission_color` fed by a vector3 graph output (the Typhoon suite's
tangent/bitangent visualizations) or a token `opacityMode` made MaterialX
find no definition ("Could not find a nodedef for node 'Surface'").

Such a node with an input its definition does not declare also matches no
definition (`Node::getNodeDef` compares the inputs; a node naming its
definition gets it regardless, and is left alone). `open_pbr_surface`
geometry inputs written under `standard_surface`'s names (`normal`,
`tangent`, `coat_normal`, `thin_walled`) take OpenPBR's (`geometry_normal`,
...), unless that input is authored too: the Typhoon suite's
`openPbr_bump_fractal` connects a bump to `normal`, and Typhoon renders the
bump. Other undeclared inputs are dropped with a warning.

USD's `UsdPrimvarReader_matrix` has no MaterialX counterpart (1.39.5 leaves
`ND_UsdPrimvarReader_matrix44` commented out: "Getting primvar of matrix type
is not supported in MaterialX standard library"). hdMtlx finds no definition
for `ND_UsdPrimvarReader_matrix` and writes a `surface` node of type
`surfaceshader` in its place, which fails generation. The loader replaces it
with a `constant` matrix44 of its `fallback` (the identity unless authored),
as when the primvar is missing, and warns: matrix primvars are not read.

## What the log shows

Assets written for older MaterialX versions or by Houdini (the MaterialEggs
countertop: `unifiednoise3d.clampoutput` authored as an integer, `freq` and
`hsvadjust.amount` as vector3, color3 `fractal3d` outputs into vector3 `add`
inputs) need hundreds of the repairs above. The log stays short; the details
are the renderer's "mdl" debug output (`PRISM_MDL_VERBOSE=1`, or the "mdl"
debug category of the renderer's options):

- hdMtlx validates the document it builds from a USD network and posts what
  MaterialX finds as one warning ("Validation warnings for generated MaterialX
  file", a line per input). The material service converts under a
  `TfDiagnosticTrap` (`src/materialx/hdmtlx_document.h`, for the surface and
  the displacement terminal) and drops that warning; with "mdl" debug output
  it prints it as `[Hydra Material] <material>: hdMtlx reported N MaterialX
  validation issue(s), repaired when the document loads:` and the lines. The
  document is unchanged. Everything else hdMtlx posts (definitions not found,
  unsupported parameter types, errors) is posted as before, and what the
  loader cannot repair is still reported when it validates the final
  document (`[WARN] Document validation issues: ...`).
- The loader prints one line per material it repaired, `[MaterialX]
  <material>: 7 type repairs, 2 clamps`: the `extract`/`convert` nodes it
  inserted between outputs and inputs of other types, and the conductor clamps
  below. The per-repair lines (`... feeds vector3 input ...; inserted
  convert`, `Clamped '...'`, `Inserted clamp node on ...`) are debug output.
  Warnings that mean a visible difference stay warnings (`layered
  transmission only partly repaired`, dropped inputs, unconvertible colour
  spaces).
- The MDL compiler's warnings about generated code that is never used (C181
  unused variable, C183 unused parameter, C350 unused let temporary) are debug
  output (`src/mdl/messages.h`). Its other messages print once: the SDK sends
  core compiler errors and warnings to Prism's receiving logger (`warn : ...`)
  and to the execution context, and only the logger's copy is printed.
- Textures published into the material cache are named in the generated
  module as absolute MDL paths (`/<hash>.exr`) relative to the document's
  directory. `MaterialCompiler` registers that directory as an MDL search path
  as well as a resource path, as the MDL SDK expects (resource search paths
  for module resources are deprecated: C295 "only found via the resource
  search paths"). The texture files, the generated MDL and the cache keys are
  unchanged.

## One output colour transform per node

MaterialX 1.39.5's shader generator converts the colour output of a node
whose filename inputs carry a colour space as a whole (not image by image),
and registers that conversion once per such input
(`ShaderGraph::applyInputTransforms`). `triplanarprojection` has three
(`filex`, `filey`, `filez`): `finalize` then inserts three transform nodes
under one name, adding the second frees the first while the output still
lists its input, and the next `ShaderInput::breakConnection` reads freed
memory. Both loaders crashed (0xC0000005) on any document with a
triplanarprojection whose files are tagged (the Typhoon suite's
`triplanarprojection_color3` and `texture_image_compare_gallery`). MaterialX's
main branch now keeps the first request for an output ("An output can carry
only one transform"). `KeepOneOutputColorTransform` does the same in the
document: the first tagged filename input keeps its colour space, the others
are tagged `none`, which the generator skips (a different space on another
file is logged). Untagged files, single-file nodes and nodes without a
colour output are left alone.

The same files failed once the crash was gone: the material service
materialized only inputs named `file` or `filename` into the texture cache,
so `filex` kept its authored relative path, which MDL's string modules
reject (C293 "strict relative file paths ... are not supported"). Every
asset-valued input is now materialized (`IsTextureParameter` in
`src/materialx/material_service.cpp`).

## EON energy compensation

`oren_nayar_diffuse_bsdf` with `energy_compensation` (OpenPBR's base,
`LamaDiffuse`) is the EON model: its multiple-scattering lobe is tinted by

    rho_ms = rho^2 E_avg / (1 - rho (1 - E_avg)),   E_avg = A (1 + C2 r),  A = 1 / (1 + C1 r)

with C1 = 1/2 - 2/(3 pi), C2 = 2/3 - 28/(15 pi) (OpenPBR; MaterialX's GLSL
`mx_oren_nayar_compensated_diffuse` and `_dir_albedo`). MaterialX 1.39.5's
MDL library (`pbrlib_1_10.mdl` `mx_oren_nayar_diffuse_bsdf`) passes
`multiscatter_tint: color(1.0)` to `df::diffuse_reflection_bsdf`, and
libbsdf adds that lobe untinted (`tint * f_ss + multiscatter_tint * f_ms`,
`diffuse_sample`/`_evaluate`): right for white albedo only, too bright for
any other (albedo rho E + (1 - E) instead of rho E + rho_ms (1 - E)). Typhoon's
`eon_diffuse_openpbr_compare`: at roughness 0.5 and 1 the red channel (0.95)
matched within 2%, green and blue were 1.3-4x too bright.

`CompensateOrenNayarEnergy` points compensated `oren_nayar_diffuse_bsdf`
nodes at a helper BSDF, `prism_oren_nayar_diffuse_bsdf` (a node definition
and inline genmdl code added to the document, like the dielectric helpers),
which passes `rho_ms` (with libbsdf's saturated tint and roughness):
the document's own nodes directly, and those in the genmdl graph of a node
the document uses through a [private copy](#private-graphs) of that graph
(OpenPBR's `diffuse_bsdf`, `LamaDiffuse`'s compensated lobe; OpenPBR's
uncompensated thin-walled subsurface lobe stays). libbsdf adds the lobe only
when the roughness is above zero, so an `open_pbr_surface` whose
`base_diffuse_roughness` is a constant zero (the default) keeps the
library's graph and its images; material templates keep a zero roughness
structural for the same reason. A repair that rewrites a copy another repair
made (OpenPBR glass) names the result after the library node and drops the
copy it replaced (`LibraryCategory`).

`prism_eon_albedo_test` samples the compiled lobe (rho 0.95/0.5/0.2,
roughness 0.5 and 1, 0 to 80 degrees): within 0.001 of EON per channel
(before: blue 0.379 instead of 0.163 at roughness 1, normal incidence), and
a white one returns 1 (the white furnace). OpenPBR's base gives the same
numbers.

## Upstream issues (reports held back)

Upstream MaterialX/MDL issues the loader works around, documented here while
the reports are held back (docs/DECISIONS.md, 2026-10-25):

- **EON's compensation lobe untinted in the MDL library** (above):
  `mx_oren_nayar_diffuse_bsdf` should pass the EON multiscatter tint
  instead of `color(1.0)`.
- **Several output colour transforms on one node crash generation**
  (above): fixed on MaterialX's main branch after 1.39.5.
- **`UsdPreviewSurface.opacityMode` is an integer in MaterialX 1.39.5**
  while USD authors a token; and **`UsdPrimvarReader_matrix`** has no
  MaterialX definition (hdMtlx writes an invalid node for it).
- Earlier ones, in their sections: the layered dielectric and opaque
  layering energy loss, layers over mixes, thin-walled OpenPBR, the uncoated
  transmission IOR, glTF volume attenuation and the standard_surface
  conductor clamp.

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
unexpected shape returns the node to the library's graph. The copies drop
the library file's source URI: MaterialX writes elements from another file
as an XInclude of it, so neither the name nor the generation caches' keys
(both read the written document) would see the rewrite.

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
  OpenPBR's glass under the coat reflects with the IOR relative to the coat
  (`specular_ior / coat_ior`, 1 when they match), but an RT lobe reflects
  with the material IOR, so the coupled glass reflected a second time (R 0.078
  at normal incidence instead of 0.04). Under such a coat (no thin film), the
  glass is now a transmission-only copy of the lobe mixed with the RT glass by
  `coat_weight` (the uncoated part reflects at the glass), and both refract
  with `specular_ior`: R 0.038 at normal incidence, 0.252 at 75 degrees
  outside (Fresnel 0.040, 0.253), and R + T 1 on both sides. A partial coat
  (weight 0.5) is approximate at grazing angles (R 0.524 at 85 degrees
  against Fresnel's 0.613; R + T stays 1), and refracts with `specular_ior`
  where OpenPBR's mixed `eta_s` would refract with a mix of the two.
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

## Older MaterialX: 1.39.3 and 1.39.4

The repairs above match 1.39.5's library graphs. A host whose process links
an older MaterialX (a USD built against 1.39.3) gets that version's MDL
generator, and usually its library graphs and MDL modules. Three
differences changed renders there; each is repaired only where the linked
code or the library on disk has it, so 1.39.5 renders bit-identically:

- **The layer over a mix of reflections drops the substrate.**
  `open_pbr_surface` layers its dielectric reflection, a mix of the
  thin-film and plain lobes, over the whole substrate (diffuse, subsurface,
  transmission): `layer(mix(tf, R, thin_film_weight), substrate)`. Neither
  generator's layer node handles a mix ("Layers over mixes" above), yet
  1.39.5 emits this library graph distributed (`dielectric_base_tf`,
  `_notf`, `_mix` in its MDL; where it does so was not traced), while
  1.39.3 drops the substrate: every OpenPBR surface only reflected, and the
  Cornell box rendered almost black, the same mean in every channel, with 0
  failed materials. 1.39.4 ships a genmdl-only graph that distributes it by
  hand (`NG_open_pbr_surface_surfaceshader_limited_layers`). The renderer
  checks the linked generator once per process (`MDLGenerator::
  dropsLayerBaseUnderMix`: it generates `layer(mix(R, R), diffuse)` and
  looks for the diffuse call, about 5 ms); where it drops the base, the
  loader distributes the layers whose top is a mix of two lobes in the genmdl
  graph of every node the document uses, through a private copy
  (`DocumentLoader::setDistributeLibraryLayers`). It runs after the other
  graph repairs, which match the undistributed graph.
- **A reflection of IOR 1 still reflects.** The MDL modules before 1.39.5
  (`pbrlib_1_9.mdl` `mx_dielectric_bsdf`) give the reflection-only lobe a
  Schlick curve with grazing reflectivity `1 - roughness` whatever its IOR;
  1.39.5's reflects nothing at IOR 1. `specular_weight = 0` modulates
  OpenPBR's IOR to 1, so the walls kept a grey grazing highlight (0.23 at 80
  degrees) and lost 4% of their diffuse light. Where the module under
  `PRISM_MATERIALX_MDL` still has the old line (`MDLGenerator::
  reflectsAtUnitIor`, read once per path), reflection-only `dielectric_bsdf`
  nodes get `weight = ifequal(ior, 1, 0, weight)`, which is what 1.39.5's
  curve computes (`DocumentLoader::setZeroUnitIorReflections`).
- **gltf_pbr's dielectric is a mix of two layers over one base** in 1.39.3
  (`mix(layer(R, base), layer(R_tf, base), iridescence)`, "the base layer
  BSDF is duplicated"), as is 1.39.4's genmdl OpenPBR graph; 1.39.5 writes
  `layer(mix(R_tf, R), base)`. The transmission coupling did not match the
  older shape, so glTF and OpenPBR glass kept their exit loss there. The
  coupling's private copy factors `mix(layer(a, X), layer(b, X), m)` into
  `layer(mix(a, b, m), X)` first (1.39.5's graphs have no such mix, checked
  over its whole library).

The rewrites are equivalent, so 1.39.3 and 1.39.4 match 1.39.5:
`prism_material_albedo_test` and `prism_eon_albedo_test` print the same
albedos to four digits on all three (Cornell red wall 0.6500, 0.0500, 0.0500
at every angle), and `prism_dielectric_albedo_test` passes on all three, its
glTF lines within 0.0005 of 1.39.5's.

Not repaired: the older MDL modules give a coated surface the coat's IOR
(`ior: color(mxp_ior)`; 1.39.5 keeps the base's, `preserve_volume_ior`), so
coated glass with `coat_ior` other than `specular_ior` refracts and totally
reflects with the coat's IOR there (the survey case `open_pbr_coat`: inside at
40 degrees R 0.76, T 0 on 1.39.3 and 1.39.4; R 0.05, T 0.71 on 1.39.5). The
material IOR is not a MaterialX value, so a graph repair cannot reach it.

A lost diffuse lobe no longer passes silently: after compiling a material
whose document clearly has a diffuse albedo (a constant base colour times its
weight of at least 0.05 on an opaque dielectric `open_pbr_surface`,
`standard_surface`, `gltf_pbr` or `UsdPreviewSurface`, no textures;
`DocumentLoader::baseAlbedo`), the renderer samples its reflection at normal
incidence (64 samples, 10-35 us) and warns when it is under a fifth of that
albedo: `[MDLMaterial] WARNING: <material> reflects 0.000 at normal incidence
although its base colour is (...)`.

## Regression checks

`prism_material_albedo_test` (main build) samples the Cornell box's walls (as
the converter writes them and with hdMtlx's `nodedef`), a default
`open_pbr_surface`, `standard_surface` and `gltf_pbr`: the walls must reflect
their base colour per channel at every angle, the others must keep their
colour, and the renderer's own probe must not warn (`-f file.mtlx` prints a
document's albedo, `-g file.mtlx` its generated MDL). With the 1.39.3 and
1.39.4 harness builds (docs/deployment.md) it is the check for those versions.
`materialx_loader_sss` covers the SSS fallback, `materialx_loader_clamp` the
conductor clamp, `materialx_loader_types` the color space names, input type
repairs and layer distribution, and `materialx_loader_transmission` the
thin-walled expansion and the refraction IOR, `materialx_loader_fresnel` the
coupled transmission graph, `materialx_loader_dielectric` (against MaterialX's
own library graphs, from the MaterialX install) the split, thin-film, coat,
thin-walled `standard_surface`, `UsdPreviewSurface` and Lama rewrites, and
`materialx_loader_uniform_color` the tagged uniform colours (and that the
library has no other uniform `color3` input), `materialx_loader_output_color`
the single output colour transform, `materialx_loader_eon` the EON helper
in direct nodes and private OpenPBR/Lama graphs, and
`materialx_loader_declarations` (against MaterialX's library) versions, enum
names, undeclared inputs and the matrix primvar reader. The
`prism_parity_triplanar_colorspace_*` and `prism_parity_declaration_repairs_*`
tests render those networks through both frontends without the error
material, and `prism_eon_albedo_test` checks EON's albedo. The
`prism_parity_gltf_uniform_colorspace_*` tests (main build) render
`tests/hydra/fixtures/gltf_uniform_colorspace.usda` through both frontends
without a compile failure and compare it with the same colours authored
untagged in ACEScg.
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
