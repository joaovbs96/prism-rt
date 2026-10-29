# Material preview

PrismRT renders a MaterialX document on a built-in shaderball, with no scene
of your own:

```
PrismRT --material look.mtlx [--material-name NAME] --output look.png [--width 512 --height 512 --spp N --device cpu ...]
```

```python
import prismrt as prism
result = prism.preview_material("look.mtlx", width=512, height=512, samples=256, device="auto")
prism.io.write("look.png", result.aovs["color"], prism.Renderer())
```

## The scene

The preview is the USD Working Group's
[Standard Shader Ball](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)
(CC BY 4.0; `resources/shaderball/ATTRIBUTION.md`), shipped unchanged in
`resources/shaderball`, opened through `resources/material_preview.usda`:

- **Geometry: the `subdiv` variant** (the asset's default): the material
  surface is a Catmull-Clark subdivision surface, refined by OpenSubdiv like
  any USD subdivision mesh (`--subdivision-level`, default 2), and displaced
  when the material has a displacement shader. The `triangulated` variant is
  not shipped.
- **Lighting: the asset's own**, its `environment.usda` layer: a closed box
  (100 cm wide) with textured walls and floor (ACEScg EXR maps) and two
  emitters of rect lights (`scene_lights` variant `on`). The asset has no
  dome or HDRI. The CLI's default dome does not apply (the scene has lights);
  `--env` / `--env-color` still add an environment.
- **Camera: the asset's** `/standard_shader_ball_scene/camera` (50 mm, a
  square aperture), so the default resolution is 512x512 (CLI and Python).
- **Material binding**: the previewed material replaces the binding of
  `/standard_shader_ball_scene/shader_ball/material_surface` at load time
  (`prism::usd::LoadOptions::materialOverrides`); the files are not edited.
  The stand (`neutral_objects/base`, `core`) and the subsurface bars keep the
  asset's materials (`neutral`, `sss_bars`), in the asset's `standard_surface`
  `material_model` variant, selected by `resources/material_preview.usda` (a
  PrismRT layer over the unchanged asset). The asset's file default, OpenPBR,
  leaves `neutral` without its `base_color` texture (the stand renders near
  white instead of mid-grey with the "Material Preview" text).

## Material documents

- `--material` takes a `.mtlx` file; Python also takes MaterialX XML text or a
  `MaterialX.Document` (as `Scene.add_material`; a document read from a file
  resolves its textures beside that file, otherwise pass `search_path`).
- **Several materials**: without `--material-name` the document's first
  material is previewed, the rule of `MaterialDesc::materialName` everywhere
  in the API. The CLI says which one and lists the others
  (`Material: red of two.mtlx (the first of 2: red blue; --material-name picks another)`);
  Python returns them (`MaterialPreview.material_name`, `.document_materials`).
- **Errors**, checked before the scene loads: a missing file (CLI:
  `MaterialX document '...' does not exist`, exit 1; Python `NotFoundError`),
  a name the document does not have (the message lists its materials;
  `NotFoundError`), a document that cannot be read or has no material
  (`InvalidArgumentError`). A material that fails to compile renders magenta,
  as everywhere else.
- `--material` replaces `--scene`; `--camera*`, `--render-settings` and
  `--output-dir` do not apply (an error says so). Other options work as
  usual.

## Interfaces

- C++ (`prism_usd`, `include/prism/usd.h`): `prism::usd::LoadMaterialPreview(scene,
  MaterialDesc, options)` adds the stage, the material and its binding to a
  `prism::Scene` and returns the asset's camera; `MaterialPreviewScene()` is
  the stage's path. The CLI and the Python package call it, so both render
  the same scene (`prism_python_preview` checks them bit for bit).
- Python: `prismrt.preview_material(material, *, material_name=None,
  width=512, height=512, samples=None, device="auto", renderer=None,
  search_path=None, aovs=("color",), progress=None, **session_kwargs)`, with
  the CLI's sample clamp (10) by default; `prismrt.usd.load_material_preview(scene,
  material, ...)` adds the preview to a scene of your own (material named
  `preview_material`, camera `preview_camera`), e.g. to swap materials
  without reloading (`examples/python/12_material_preview.py`).

## Where the files are found

`MaterialPreviewScene()` looks for `resources/material_preview.usda`
beside the `prism_usd` library (then one directory up), so no environment
variable is needed: the build copies it and `resources/shaderball` next to
`prism_usd.dll` (`build/<config>`, target `prism_preview_resources`), `install`
puts it in `bin/resources` (Windows; `lib/resources` elsewhere) and
`tools/package_windows.ps1` copies it into the package. A wheel must put it
beside its `prism_usd` library (`prismrt/resources/shaderball`).
`PRISM_MATERIAL_PREVIEW_SCENE` names another stage with the same prims.

## Tests

All on the CPU, 64x64, 64 samples, seed 7, no adaptive sampling:

- `prism_parity_material_preview`: the stage with the marble document bound in
  USD (`tests/hydra/fixtures/material_preview.usda`), CLI against Hydra, within
  the noise (0.037 normalized RMSE, mean 0.0007; two seeds differ by 0.089):
  the five rect lights are ordered differently, as in the `lights` fixture.
- `prism_material_preview_matches_stage`: the CLI's `--material` render against
  that stage's CLI render (3.5e-6, mean 7.5e-8; tolerance 1e-4; see below).
- `prism_material_preview_reference`: against
  `tests/hydra/fixtures/material_preview/reference_marble.exr`, at a noise-floor
  tolerance (normalized RMSE 0.15, mean 0.01; another material differs by 0.34
  and 0.24).
- The error cases (`prism_material_preview_missing_file`, `_unknown_name`,
  `_scene_conflict`, `_first_material`) and `prism_python_preview` (Python
  against the CLI bit for bit at 128x128; names, documents and errors).

**A material bound in USD renders the same**: a `.mtlx` referenced into a
stage (usdMtlx, then the material service) keeps its *document-level*
`colorspace` (the marble and `two_materials.mtlx` say `lin_rec709`) in both
loaders (`src/usd/mtlx_color_space.h`; `docs/materialx_compatibility.md`,
"Document colour space through USD"). Until 2026-10-29 it was lost and the
colours were used as working-space (ACEScg) values: the marble's stage differed
from `--material` by 0.006 normalized RMSE, `two_materials.mtlx`'s red by 0.20
(2.8% mean); now 3.5e-6 and 6.0e-8.
