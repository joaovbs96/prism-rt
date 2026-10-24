# glTF materials

glTF 2.0 scenes (`.gltf`, `.glb`) load in the standalone CLI
(`PrismRT --scene file.gltf --camera-position ... --camera-target ...`).
Their materials become MaterialX `gltf_pbr` documents and go through the same
MaterialX → MDL pipeline as USD's MaterialX materials, so they render on every
device (CPU, Vulkan, Metal) with MDL's BSDFs, next-event estimation and
emission.

The code: `src/materialx/gltf_material.{h,cpp}` (the converter: MaterialXCore
and tinygltf only) and `src/mesh/gltf_mesh.h` (geometry, images, compilation,
error material).

## Where glTF can be loaded

| Front end | glTF |
| --- | --- |
| CLI (`PrismRT --scene x.gltf`) | yes (`glTFMesh`) |
| Hydra / usdview, or a USD stage referencing a `.gltf` | **no**: the OpenUSD build has no glTF file-format plugin (`usd`, `usda`, `usdc`, `usdz` and `mtlx` only). A follow-up would add one (for example Adobe's `usd-fileformat-plugins`, or a small Sdf file format over the same converter). |

## Materials that fail

A material whose conversion or MDL compilation fails renders with the magenta
error material (`ErrorMaterial` in `src/material.h`: magenta emission, no
scattering, not a light), as a USD material that fails to compile does, after
one line naming it and the reason:
`PrismRT glTF: material 3 'Glass' failed to load (MDL compilation failed (...)); rendering it with the magenta error material`.
The CLI reports the split:
`glTF: 15 MaterialX material(s) (15 compiled, 0 reused), 0 failed, in 8.2 s`.

History: until this converter replaced them, glTF materials could also load
as hand-written C++ materials (`gltfPBR` and its lobes, `--gltf-materials
legacy` / `PRISM_GLTF_MATERIALS`), which were also the fallback for a failed
material. They sampled the BSDF only, lost energy on rough metals, never
emitted, ignored alpha and most extensions, and normal-mapped incorrectly (on
`normal_map.gltf` 2–3× too dark); on the metal/rough spheres they were within
NRMSE 0.18 of this mapping. They were removed with the other legacy materials.

## Mapping

Core metallic-roughness:

| glTF | gltf_pbr input | Notes |
| --- | --- | --- |
| `baseColorFactor` × `baseColorTexture` × `COLOR_0` | `base_color` (rgb), `alpha` (a) | texture `srgb_texture`, factor `lin_rec709`; `gltf_image` color4 → `multiply` → `convert` / `extract 3` |
| `metallicFactor`, `roughnessFactor` × `metallicRoughnessTexture` | `metallic` (B), `roughness` (G) | one raw `gltf_image` vector3, two `extract`s |
| `normalTexture` (+ `scale`) | `normal` | raw `gltf_image` vector3 → `normalmap` with `scale` (`gltf_normalmap` has no scale) |
| `occlusionTexture` | — | not mapped: `gltf_pbr` ignores `occlusion`, and a path tracer computes occlusion |
| `emissiveFactor` × `emissiveTexture` | `emissive` | texture sRGB, factor `lin_rec709`. Emits light |
| `alphaMode` OPAQUE / MASK / BLEND, `alphaCutoff` | `alpha_mode` 0/1/2, `alpha_cutoff` | MASK cuts out, BLEND is stochastic coverage |
| `doubleSided` | — | per triangle in `Mesh::single_sided`; honoured only with `--honor-double-sided true`, as for USD |
| no material on a primitive | the glTF default material | white, metallic 1, roughness 1 (previously this crashed) |

Extensions:

| Extension | gltf_pbr inputs | Notes |
| --- | --- | --- |
| `KHR_materials_emissive_strength` | `emissive_strength` | |
| `KHR_materials_transmission` | `transmission` (texture R) | MDL refracts (not thin-walled, see below) |
| `KHR_materials_volume` | `thickness` (texture G), `attenuation_distance`, `attenuation_color` | Beer-Lambert absorption in the closed mesh: MaterialX 1.39.5's `gltf_pbr` builds the absorbing VDF but never connects it, so `DocumentLoader` layers the transmission over it (`LayerGltfPbrVolume`, [materialx_compatibility.md](materialx_compatibility.md#gltf_pbr-volume-attenuation)). `attenuation_color` is a uniform input: converted to the working space on the CPU, untagged. No `attenuationDistance` (infinite) or `thicknessFactor` 0 (thin-walled in glTF) gives no medium (`attenuation_distance` 1e30) |
| `KHR_materials_ior` | `ior` | |
| `KHR_materials_clearcoat` | `clearcoat` (R), `clearcoat_roughness` (G), `clearcoat_normal` | |
| `KHR_materials_sheen` | `sheen_color` (sRGB texture), `sheen_roughness` (A) | |
| `KHR_materials_specular` | `specular` (A), `specular_color` (sRGB texture) | |
| `KHR_materials_iridescence` | `iridescence` (R), `iridescence_ior`, `iridescence_thickness` | thickness = min + (max − min) × G, or the maximum without a texture |
| `KHR_materials_anisotropy` | `anisotropy_strength` (× texture B), `anisotropy_rotation` (+ atan2 of the texture's RG direction) | as MaterialX's `gltf_anisotropy_image`, with the texture's own sampler and texture coordinate |
| `KHR_materials_dispersion` | `dispersion` | |
| `KHR_materials_unlit` | `emissive` = base colour, black non-reflective surface | an approximation (warns) |
| `KHR_texture_transform` | `gltf_image` `offset`, `rotate` (degrees), `scale`, `texCoord` override | exact: `gltf_image`'s `place2d` with pivot (0, 1) reproduces glTF's T·R·S in the V-flipped space |
| `KHR_mesh_quantization` | — | the accessor reader takes normalized and integer components |
| `KHR_materials_variants` | — | the primitive's default `material` is used; the extension warns |
| anything else ( `KHR_draco_mesh_compression`, `KHR_texture_basisu`, `KHR_lights_punctual`, `KHR_animation_pointer`, ...) | — | one line each: `PrismRT glTF: extension X is not supported; ignored` |

Textures and texture coordinates:

- **Samplers**: `wrapS`/`wrapT` REPEAT / CLAMP_TO_EDGE / MIRRORED_REPEAT →
  `uaddressmode`/`vaddressmode` `periodic` / `clamp` / `mirror`; `magFilter`
  NEAREST → `filtertype closest`, otherwise linear (mip filtering is the
  renderer's own).
- **V flip**: glTF's texture origin is top-left, MaterialX's bottom-left. The
  loader stores `v' = 1 − v` for every primitive (this
  is what `gltf_image`'s pivot (0, 1) and MaterialX's own glTF loader
  expect); tangents are then derived in that space, so glTF's +Y-up normal
  maps come out right. `prism_gltf_normal_map_test` checks normal-mapped
  quads against quads rotated to the mapped normals, along U and V.
- **TEXCOORD_1** is the mesh primvar `TEXCOORD_1` (V flipped too), read with
  `geompropvalue` into the image's `texcoord`, i.e. an MDL scene data lookup
  on both CPU and GPU, as USD primvars are. Sets above 1 warn and use 0.
- **COLOR_0** (vec3/vec4, float or normalized integers) is the color4 primvar
  `COLOR_0`, converted from linear Rec.709 into the working space at load
  time, multiplying base colour and alpha.

## Colour management

Per the glTF spec, colour textures (base colour, emissive, sheen and specular
colour) are sRGB and tagged `srgb_texture`; data textures (metallic-roughness,
normal, transmission, ...) are raw. Colour factors are linear Rec.709 and are
tagged `lin_rec709`, so the generator converts each into the working space
(ACEScg by default) exactly once ([color_management.md](color_management.md)).
Texture and factor convert separately before they multiply, which is exact
when either is grey.

## Images

The loader keeps images encoded (`tinygltf` "as is": nothing is decoded). Each referenced image — an external file, a data URI or
a GLB buffer view — is published content-addressed into the material cache
(`PRISM_CACHE_DIR` or `%LOCALAPPDATA%\PrismRT\cache`, `materials\<sha1>.png`)
by `PublishMaterialResourceBytes` (`src/materialx/resolved_resource.h`, the
in-memory counterpart of `PrepareMaterialResource`), and registered with the
texture service as a trusted source. The generated `.mtlx` sits in the same
directory (`gltf_<sha1>.mtlx`), which is an MDL resource path, so the
documents name images by file name. Nothing is written next to the source
asset. Embedded images therefore become ordinary files of the MDL texture
pipeline and the texture service (mip levels, streaming, GPU budgets).

## Load-time cost and caching

Each distinct material document costs one MDL compilation, compiled one
after another at load: about 0.2 s for a factor-only material and 0.5 s for a
textured one (8 sphere materials: 1.4 s; ABeautifulGame's 15: 8.2 s, cold
caches). Materials with identical
documents within a load compile once (`reused` in the report), and the
existing caches apply across loads and processes: generated MDL (persistent),
compiled templates, the GPU program cache (8 factor-only glTF materials share
one GPU program). Images are never decoded at load: ABeautifulGame loaded in
8.8 s, against 15.4 s for the former C++ materials, which decoded every image
(one run each, not a benchmark).

## Tests

`ctest -R prism_gltf` (fixtures in `tests/gltf`, written by
`tests/gltf/make_fixtures.py`):

| Test | Checks |
| --- | --- |
| `prism_gltf_material_test` | the converted document for each feature, validation against the standard library, MDL generation (sRGB and `lin_rec709` transforms into ACEScg), the volume layer (`LayerGltfPbrVolume`) |
| `prism_gltf_{spheres,textured,features,embedded}_render` | every fixture renders with every material compiled (none uses the error material) |
| `prism_gltf_error_material_{render,test}` | without the MaterialX libraries each material warns and renders magenta |
| `prism_gltf_normal_map_test` | normal-mapped halves shade like quads rotated to the mapped normals (U and V) |
| `prism_gltf_volume_{cpu,vulkan}_test` | through a closed IOR-1 sphere the background is `attenuationColor^(2 / attenuationDistance)` (Beer-Lambert) within 5% |
| `prism_gltf_{spheres,textured}_vulkan_parity` | Vulkan vs CPU: NRMSE < 0.1, mean < 2% (measured 0.0002 and 0.0095) |

## Known gaps

- Thin-walled transmission: `gltf_pbr` has no thin-walled switch, so
  transmission without a volume (or with thickness 0) still refracts as a
  solid; only its absorption is dropped. Volume attenuation spans the whole
  closed mesh (`thickness` and its texture are not used: the path tracer
  measures real path lengths).
- A `gltf_pbr` with attenuation is expanded into its graph before MDL
  generation, so it compiles longer (about 4–5 s instead of 0.5 s).
- Material variants (`KHR_materials_variants`), `KHR_materials_diffuse_transmission`, KTX2/Basis and
  WebP-only textures, Draco/meshopt compression, `KHR_lights_punctual`,
  cameras, skinning, morph targets and animation.
- Vertex colour alpha multiplies `alpha` but, as glTF says, only BLEND/MASK
  materials use it.
- Hydra cannot load glTF (no file-format plugin).
