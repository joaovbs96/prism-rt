# OBJ materials

Wavefront OBJ scenes (`.obj` with `.mtl` material libraries) load in the
standalone CLI (`PrismRT --scene file.obj --camera-position ... --camera-target
...`). Their MTL materials become MaterialX `open_pbr_surface` documents and go
through the same MaterialX → MDL pipeline as USD's and glTF's materials, so
they render on every device (CPU, Vulkan, Metal) with MDL's BSDFs, next-event
estimation and emission.

The code: `src/materialx/obj_material.{h,cpp}` (the converter: MaterialXCore
and tinyobjloader only) and `src/mesh/obj_mesh.h` (geometry, MTL statement
scan, maps, compilation, error material).

## Where OBJ can be loaded

| Front end | OBJ |
| --- | --- |
| CLI (`PrismRT --scene x.obj`) | yes (`ObjMesh`) |
| `ObjMesh` without an MDL context | every material renders with the error material |
| Hydra / usdview, or a USD stage referencing an `.obj` | **no**: the OpenUSD build has no OBJ file-format plugin. Adobe's `usd-fileformat-plugins` (below) would add one. |

## Reference: Adobe's OBJ file-format plugin

The mapping follows Adobe's open-source USD file-format plugin for OBJ,
[adobe/USD-Fileformat-plugins](https://github.com/adobe/USD-Fileformat-plugins)
(Apache 2.0), read at commit `6441b97807a59472337f9c3d02df089d45ed1c48`:
`obj/src/obj.cpp` (`readMtl`: the statements and map options it parses),
`obj/src/objImport.cpp` (`importMaterialProperty`, `shininessToRoughness`,
`importEmissive`, `importObj`: the material mapping), `obj/src/obj.h`
(`ObjMaterial`, `ObjMap`) and `utils/src/materials.cpp` /
`utils/src/layerWriteOpenPBR.cpp` (normal and colour-space handling). No code
was copied; the mapping was reimplemented over tinyobjloader's parse.

What was taken from it:

- **OpenPBR as the target.** Adobe's plugin writes an OpenPBR network by
  default (`writeOpenPBR`, with UsdPreviewSurface and ASM as alternatives).
  `open_pbr_surface` has every lobe MTL needs (coat for `Pc`, fuzz for `Ps`,
  anisotropy, thin transmission colour, emission, opacity), so it fits better
  than `gltf_pbr` (no fuzz/sheen roughness pairing, no `geometry_opacity`
  separate from alpha modes).
- **Two models.** A material authoring `Pr` or `Pm` (or their maps) is PBR:
  `Kd` is the base colour, `Pm` the metalness, `Pr` the roughness, and `Ks`/`Ns`
  are not read. Otherwise it is Phong: `Kd` is the base colour, the material
  stays dielectric (metalness 0: a bright `Ks` must not turn paint into metal),
  and the roughness comes from `Ks` and `Ns`.
- **Phong roughness** (`shininessToRoughness`, the roughness half of the
  [Azure Remote Rendering mapping](https://learn.microsoft.com/azure/remote-rendering/reference/material-mapping)):
  `roughness = sqrt(2 / (Ns · luminance(Ks) + 2))`, `luminance` with weights
  (0.2125, 0.7154, 0.0721); below 0.08 it becomes 0 (a mirror). Unauthored `Ks`
  and `Ns` are 0.5 each. With `map_Ks` or `map_Ns`, PrismRT evaluates the same
  expression per texel in the graph (Adobe bakes a texture).
- **Value and map.** A map and its value multiply, except that a value equal
  to the default (0 for colours and `Pr`/`Pm`, 1 for `d`) beside a map is a
  placeholder (Maya writes `Kd 0 0 0` next to `map_Kd`) and the map is used
  alone; an explicit 0 where the default is not 0 (for example `d 0`) wins
  over the map.
- **Channels.** Scalar maps read red by default (`-imfchan l`, luminance, is
  read as red: grey maps), `-imfchan r|g|b` the named channel, `m` and `z` the
  alpha.
- **Emission** is `Ke` × `map_Ke` (Adobe also multiplies an `adobe_glow`
  extension, not supported here). Adobe sets `emission_luminance` to an
  ASM-to-OpenPBR factor; PrismRT sets 1, so `Ke` is the emitted radiance (as
  the former C++ materials and glTF's `emissiveFactor` behave).
- **Opacity** is `d` × `map_d` (tinyobjloader turns `Tr` into `1 − Tr` when
  `d` is absent; `d` wins when both are given).
- **Normals**: `norm` (a tangent-space normal map) wins over `bump` (a height
  map); `Ni` is the specular IOR (default 1.5).
- **Map placement**: `-o` and `-s` translate and scale the texture
  coordinates (`u' = u · s + o`); maps repeat.

Where PrismRT departs from Adobe's plugin:

| Statement | Adobe | PrismRT | Why |
| --- | --- | --- | --- |
| `illum` | ignored | 0 and 1 drop the specular lobe (`specular_weight` 0); 4 and 9 are dissolve glass, 6 and 7 refracting glass (below); 10 warns | the MTL specification defines these models; glass assets rely on them |
| `Tf` | ignored | the transmission colour of the glass illum modes; otherwise warns | the specification's transmission filter |
| `Ni` 1 or below | IOR 1 (no reflection) | 1.5 (warns below 1) | tinyobjloader cannot tell `Ni 1` from no `Ni`, and old exporters write `Ni 1` for every material |
| `map_d` without `-imfchan` | red | alpha when the image has one, otherwise red | cut-outs commonly put the mask in the colour map's alpha |
| `bump` | baked to a normal map | a height map evaluated in the graph (below) | |
| `Ps`, `Pc`, `Pcr`, `aniso` | not read | `fuzz_weight`, `coat_weight`, `coat_roughness`, `specular_roughness_anisotropy` (with `map_Ps`, `map_Pc`, `map_Pcr`, `map_aniso`) | the PBR extension's remaining lobes have OpenPBR inputs |
| Colour maps | colour space left to the reader (8-bit maps read as sRGB) | tagged `srgb_texture` (overridable with tinyobjloader's `-colorspace sRGB|linear`) | explicit tags, as for glTF |

## Mapping

| MTL | open_pbr_surface | Notes |
| --- | --- | --- |
| `Kd` × `map_Kd` | `base_color` | `Kd` `lin_rec709`, map `srgb_texture`; no `Kd` and no map: OpenPBR's 0.8 |
| `Ks`, `Ns` (× `map_Ks`, `map_Ns`) | `specular_roughness` | Phong model only (see above) |
| `Pr` × `map_Pr`, `Pm` × `map_Pm` | `specular_roughness`, `base_metalness` | PBR model; without `Pr` OpenPBR's 0.3 |
| `Ps` × `map_Ps` | `fuzz_weight` | |
| `Pc` × `map_Pc`, `Pcr` × `map_Pcr` | `coat_weight`, `coat_roughness` | |
| `aniso` × `map_aniso` | `specular_roughness_anisotropy` | `anisor` warns: OpenPBR has no anisotropy rotation input |
| `Ni` | `specular_ior` | above 1 only |
| `d` × `map_d` (`Tr`) | `geometry_opacity` | not for the dissolve glass modes |
| `illum` 0, 1 | `specular_weight` 0 | |
| `illum` 4, 9 (glass on) | `transmission_weight` = 1 − `d` × `map_d` | the dissolve is the glass's transparency; no cut-out |
| `illum` 6, 7 (refraction on) | `transmission_weight` 1 | `d` still cuts out |
| `Tf` | `transmission_color` | glass modes only, `lin_rec709` |
| `Ke` × `map_Ke` | `emission_color`, `emission_luminance` 1 | radiance; map `srgb_texture` |
| `norm` (`-bm`) | `geometry_normal` ← `normalmap` (`scale` = `-bm`) | raw vector3 map, OpenGL (+Y) convention |
| `bump`, `map_bump` (`-bm`) | `geometry_normal` ← `normalmap` | see below |
| `disp` (`-mm base gain`, `-imfchan`, `-o`, `-s`) | the surfacematerial's `displacementshader` ← `displacement` (`displacement` = base + gain × texel) | see below |
| `Ka` | — | ambient light means nothing to a path tracer; read silently |
| `map_Ka`, `refl`, `decal`, `sharpness`, others | — | one warning per load each: `PrismRT OBJ: MTL refl is not supported; ignored (first in material 'white')` |

Texture options: `-o`, `-s` (u and v), `-clamp on` (clamp addressing),
`-imfchan`, `-bm` (bump and normal maps) and tinyobjloader's `-colorspace`
are honoured; `-mm` on `disp` too; `-mm` elsewhere, `-t`, `-boost`, `-blendu/-blendv`, `-texres` and a third
(w) `-o`/`-s` component warn once per load.

**Bump maps.** MaterialX's `heighttonormal` cannot be used: in generated MDL
it evaluates its upstream graph once instead of around the point (all nine
Sobel samples are the same value), so it is flat. The converter builds the
normal itself: four lookups of the height map one texel apart (the map's size
comes from its header), central differences in height units per texel, and
`n = normalize(−bm · dh/du, −bm · dh/dv, 1)` encoded for `normalmap`. A height
rising by 1 over 4 texels with `-bm 1` tilts the normal by 14°.

**Displacement maps.** `disp` becomes the material's displacement: a
`displacement` shader (float, along the normal) bound to the surfacematerial's
`displacementshader` input — the shape USD MaterialX displacement has once
the Hydra delegate merges a displacement terminal into the surface document
(`AddDisplacementShader`, `src/hydra/materials.cpp`), so the material's MDL
`geometry.displacement` carries it. The height is the map's red channel (or
`-imfchan`), raw, placed by `-o`/`-s`; `-mm base gain` gives
`base + gain × texel` (a `multiply` and an `add`, as `displacement` has a
scale but no offset; without a base the gain is its `scale`). The standalone
loader does not displace meshes yet (the Hydra delegate displaces at load
time; the CLI's displacement is planned with its subdivision), so a load with
`disp` warns once:
`PrismRT OBJ: disp is converted (material 'x'), but the standalone loader does not displace meshes yet (planned)`.

## Maps and the cache

Maps resolve relative to their MTL file's directory, then the OBJ's (forward
or back slashes). Each is published content-addressed into the material cache
(`PRISM_CACHE_DIR` or `%LOCALAPPDATA%\PrismRT\cache`, `materials\<sha1>.<ext>`)
by `PrepareMaterialResource` (`src/materialx/resolved_resource.h`, as for USD
textures), beside the generated `obj_<sha1>.mtlx` documents, which name maps
by file name. Formats MDL's image plugin cannot read (TGA, BMP, ...; the
`Samba_Easy` asset's maps are `.TGA`) are transcoded to PNG first. Nothing is
written next to the source asset. A missing map warns
(`PrismRT OBJ: material 'pbr': norm: cannot find 'normalmap.tex'; ignored`)
and the material renders without it.

Texture coordinates are stored as the OBJ gives them: MTL's origin (bottom
left) is MaterialX's. The former C++ materials read maps flipped vertically;
textured OBJ assets now show their maps the right way up.

## Materials that fail, and faces without one

A material whose MDL compilation fails renders with the magenta error
material (`ErrorMaterial`), after one line naming it and the reason, as for
USD and glTF:
`PrismRT OBJ: material 'x' failed to load (...); rendering it with the magenta error material`.
Faces without a material (no `usemtl`, or an MTL file that is missing) are
grey (`Diffuse` 0.5), as before. The CLI reports the split:
`OBJ: 263 MaterialX material(s) (188 compiled, 75 reused), 0 failed, in 58.5 s`.

Materials with identical documents within a load compile once (`reused`);
the generated-MDL, template and GPU program caches apply across loads.

History: OBJ materials used to load as the C++ `Diffuse` material of
`src/material.h` (`Kd` × `map_Kd` only; everything else warned).

## Colour management

MTL colours (`Kd`, `Ks`, `Ke`, `Tf`) are read as linear Rec.709 and tagged
`lin_rec709`; colour maps (`map_Kd`, `map_Ks`, `map_Ke`) are `srgb_texture`;
data maps (`map_Ns`, `map_d`, `map_Pr`, `map_Pm`, `map_Ps`, `map_Pc`, `map_Pcr`,
`map_aniso`, `bump`, `norm`) are raw. The generator converts each tagged value
into the working space once ([color_management.md](color_management.md)).

## Tests

`ctest -R prism_obj` (fixtures in `tests/obj`, written by
`tests/obj/make_fixtures.py`):

| Test | Checks |
| --- | --- |
| `prism_obj_material_test` | the statement scan, the Phong roughness, the converted document for every fixture material and for inline MTL text (defaults, placeholders, channels, unsupported statements, missing maps, `disp` as a wired displacement shader with `-mm`), validation against the standard library and MDL generation |
| `prism_obj_{textured,pbr,glass,emissive}_render` | every fixture renders on the CPU with every material compiled and no unsupported statement |
| `prism_obj_textured_test` | `map_d` cuts the quad's corner out to the background, the opaque part is the leaf, bump and normal maps add shading detail (more than 3× the noise of a flat region) |
| `prism_obj_glass_test` | through the refracting sphere the white background is tinted by `Tf` |
| `prism_obj_emissive_test` | the `Ke 2 2 2` emitter shows radiance 2 (1%), the `map_Ke` one its texture |
| `prism_obj_textured_vulkan_parity` | Vulkan vs CPU: NRMSE < 0.1, mean < 2% (measured 0.0032 and 0.005%) |

## Known gaps

- No OBJ file-format plugin for Hydra / usdview.
- `disp` is converted but only applied once the standalone loader displaces
  meshes (Hydra cannot load OBJ).
- `anisor`, `-mm` (except on `disp`), `-t`, `-boost`, `map_Ka`, `refl`, `decal`, Adobe's
  `adobe_*` statements and the MDL-material variant of Adobe's MTL files are
  not mapped (each warns once).
- Vertex colours (`v x y z r g b`) are not used.
- `illum` 10 (shadow matte) is not supported.
- `bump`'s slope is per texel of the height map, so the same `-bm` tilts the
  normals of a higher-resolution map less.
