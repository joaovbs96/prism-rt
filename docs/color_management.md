# Colour management

PrismRT renders in one scene-linear **working space**, ACEScg by default,
taken from an OpenColorIO config. Every colour that enters the renderer is
converted into it once; linear outputs stay in it, and 8-bit outputs go
through a display and view (ACES 2.0 SDR on an sRGB display by default).

The code is `src/color/color_management.{h,cpp}` (`prism::color`). The
colour management is process-wide (`prism::color::Active()`): both loaders,
the texture service, the MaterialX generator and the render devices read it.

## Config, working space and view

| Setting | CLI | Hydra render setting | Default |
| --- | --- | --- | --- |
| Config | `--ocio-config FILE\|URI` | `prism:colorManagement:ocioConfig` | `$OCIO`, else `ocio://studio-config-latest` |
| Working space | `--working-space NAME` | `prism:colorManagement:workingSpace` | `ACEScg` (the config's `scene_linear` role if it has no ACEScg) |
| Display | `--display NAME` | `prism:colorManagement:display` | the config's default display |
| View | `--view NAME` | `prism:colorManagement:view` | the display's default view |

- With OpenColorIO 2.6.0, `ocio://studio-config-latest` is the built-in
  `studio-config-v5.0.0_aces-v2.1_ocio-v2.6`: displays `sRGB - Display`
  (default), `Gamma 2.2 Rec.709 - Display`, Display P3, Rec.1886, Rec.2100 and
  others; views `ACES 2.0 - SDR 100 nits (Rec.709)` (default),
  `Un-tone-mapped`, `Video (colorimetric)` and `Raw`. The built-in configs are
  compiled into the OpenColorIO DLL, so nothing else ships.
- Names are OCIO colour space names or aliases, case-insensitive:
  `ACEScg`/`acescg`/`lin_ap1`, `lin_rec709` (`Linear Rec.709 (sRGB)`),
  `lin_displayp3`, `lin_adobergb`, `lin_rec2020`... The working space must be
  scene-linear; `Create` refuses others, and an unknown display or view.
- The CLI reads the stage's `prism:colorManagement:*` attributes on the
  RenderSettings prim named by `renderSettingsPrimPath`, as the Hydra delegate
  does; command-line options take precedence.
- In Hydra, changing the config or working space rebuilds the scene
  (materials, lights, domes and their textures convert again). Display and
  view are only recorded there: the colour AOV is linear.
- The colour management is per process. Two Hydra delegates in one process
  with different working spaces are not supported; a new delegate starts from
  the defaults (or its render settings).

### Linear Rec.709 (the renderer before colour management)

```
PrismRT ... --working-space lin_rec709 --display "Gamma 2.2 Rec.709 - Display" --view Un-tone-mapped
```

reproduces the earlier images exactly: no colour converts, the luminance
weights are the old Rec.709 constants, and the view is `pow(clamp(x), 1/2.2)`
(OCIO's lossless processor matches OpenImageIO's to the bit). Tests store
images from the old renderer and compare (see Tests).

## What converts

| Input | Authored in | Conversion |
| --- | --- | --- |
| MaterialX values and images with a colour space (`colorspace` on the input, node, graph or document; USD `colorSpace` metadata) | that space | MaterialX's generated MDL code, into the working space |
| MaterialX values and images without a colour space | - | none: used as working-space values |
| MaterialX data spaces (`raw`, `Raw`, `data`, `non-color`) | - | none (the loader maps them to `none`) |
| UsdLux `inputs:color`, `colorTemperature` (blackbody) | linear Rec.709 | matrix, in `MakeSceneLight` (both loaders) and the Hydra dome |
| Dome and rect-light `inputs:texture:file` | linear Rec.709 | texels, when the texture is prepared |
| CLI `--env FILE`, `--env-color` | linear Rec.709 | texels; matrix |
| glTF base colour and emission, OBJ diffuse (legacy materials) | linear Rec.709 (after the existing glTF decode) | texels and factors separately (exact when either is grey) |
| `displayColor` and other untagged USD colours | - | none |

### MaterialX: the spike, and the generator's colour transforms

MaterialX 1.39.5's `DefaultColorManagementSystem` finds a transform by name,
`ND_<source>_to_<target>_<type>`, and its library (`cmlib`) only has
transforms **into `lin_rec709`**. With `targetColorSpaceOverride = "acescg"`
it finds none, `supportsTransform` is false, and the generator silently emits
no transform: sRGB textures and tagged constants would reach the renderer
undecoded. So the target cannot simply be set to the working space.

`MDLGenerator` (`src/materialx/mdl_generator.cpp`, `ColorTransformLibrary`)
instead gives the colour management system a small library, with the
standard library as its data library, that adds for every cmlib source space
`ND_<source>_to_<working>_color3/color4`: cmlib's decode to `lin_rec709`
followed by the working space's matrix (a `transformmatrix` node, the same
form cmlib uses for `acescg_to_lin_rec709`), and `lin_rec709_to_<working>`.
The target is the working space's MaterialX name (`acescg`; an unnamed space
gets `prism_working_<hash>`). As before, a value already in the target space
gets no transform, so:

- a tagged value or image converts exactly once (MDL textures are read with
  `tex::gamma_linear`; the decode is in the generated code, never in the
  texture service, so there is no double sRGB decode);
- an `acescg`-tagged value in ACEScg, or an untagged one, does not convert;
- in linear Rec.709 the stock library is used unchanged and the generated MDL
  is the same as before.

Untagged values stay as they are because that is what MaterialX's generator
does (no source space, no transform) and what the Eggs' Karma references
assume: their untagged colours are ACEScg values. MaterialX's specification
reads them as `lin_rec709`; set the document or input `colorspace` for that.

The generated-MDL caches, the compiled-template registry and the GPU program
caches are keyed by the working space (the generator's options identity
includes it; the template registry stores it).

### Textures

`TexturePrepareOptions::workingSpace` (`src/texture_service.h`) converts the
colour channels from `colorSpace` into the working space after gamma and
exposure, with an OCIO CPU processor. It holds the colour management's
`identity()` (config, working space, matrix), which is part of the texture
cache key and the in-memory stamp, so prepared textures of different working
spaces never mix; textures that need no conversion keep their earlier keys.
`ImageTexture(path, colorSpace)` and `Texture::convertToWorkingSpace` set it.
MDL textures leave it empty.

Light textures are read as linear Rec.709, whatever `colorSpace` metadata
their attribute has: the Hydra scene index does not carry attribute
metadata, so the CLI does not read it either. 8-bit dome textures are not
sRGB-decoded (as before).

## Light selection

Light power (`LightPower`), emissive triangles in the light table, the
environment's importance tables and spatial light selection
(`lightImportance` in `shaders/integrator/lights.h`) weigh radiance by the
working space's luminance: Y of the colour through Rec.709
(`ColorManagement::luminanceWeights`; ACEScg about 0.2676, 0.6744, 0.0580;
exactly 0.2126, 0.7152, 0.0722 in linear Rec.709). The GPU kernels get them in
`GpuFrame::color` and `SampleSettings::luminance` (no new binding). These are
sampling weights only; any weights give the same expected image.

## Outputs

- **CLI EXR**: linear in the working space after the sample clamp, with
  `oiio:ColorSpace` and OpenEXR `chromaticities` (ACEScg, linear Rec.709,
  Display P3 and Adobe RGB primaries).
- **CLI 8-bit formats** (PNG, JPEG, ...): the linear image through the view,
  clamped to [0, 1], exactly once. This replaces the earlier gamma 2.2.
- **Hydra colour AOV**: linear in the working space. PrismRT's presentation
  (`PresentationCompositor`) applies no view; the host does.

### usdview

usdview colour-corrects the colour AOV itself (Hdx's colour correction task)
with OpenColorIO, from the config's `scene_linear` role (ACEScg, PrismRT's
default working space). Because of an OpenUSD bug (below) its default view
is the studio config's `Un-tone-mapped` sRGB view, not ACES 2.0 SDR like the
CLI's 8-bit outputs: correct ACEScg-to-sRGB colours, without tone mapping.

- The OpenUSD stack (`tools/ci/build_usd_stack.ps1`, local and CI) is built
  with `PXR_BUILD_OPENCOLORIO_PLUGIN=ON` against vcpkg's OpenColorIO 2.6.0,
  the one PrismRT uses: a single `OpenColorIO_2_6.dll` (vcpkg's) is loaded in
  usdview, the package and the tests. See
  [deployment.md](deployment.md#local-dependency-stack).
- `prism_env.ps1`, `prism_env.sh` and the package's `env_setup.bat` set
  `OCIO=ocio://studio-config-latest` when `OCIO` is unset (PrismRT's own
  default config, made explicit). usdview disables its `openColorIO` mode
  without `$OCIO`.
- usdview starts in its saved colour correction mode (`sRGB` until changed)
  and has no option to choose one. The `prismUsdview` extension sets
  `openColorIO` at start-up when `$OCIO` is set; usdview loads extensions
  before the stage, so the first image already uses it.
  `PRISM_USDVIEW_COLOR_CORRECTION=sRGB|disabled|openColorIO|saved` chooses
  another start-up mode (`saved` keeps usdview's; in the package,
  `PRISMRT_USDVIEW_COLOR_CORRECTION`). Without `$OCIO` a saved `openColorIO`
  becomes `sRGB`.
- **ACES 2.0 views render black in usdview.** OpenUSD 26.08's
  `HdxColorCorrectionTask` (OCIO v2 path, also unchanged on OpenUSD's `dev`
  branch) names the sampler of OpenColorIO's 3D LUT textures but not of its
  1D/2D ones (`sampDesc.debugName` is only set in the 3D loop). On OpenGL and
  Vulkan, every transform that uses 1D/2D LUTs then fails with "Failed
  verification: !texInfo.texName.empty() && !texInfo.samplerName.empty()" and
  the viewport is black. The studio config's ACES 2.0 views use two such
  tables; its analytic views (`Un-tone-mapped` and `Video (colorimetric)`
  tested) work. So with the studio config (`OCIO=ocio://studio-config-*`) the
  extension sets display `sRGB - Display` and view `Un-tone-mapped`.
  `PRISM_USDVIEW_OCIO_DISPLAY` and `PRISM_USDVIEW_OCIO_VIEW` choose others;
  `PRISM_USDVIEW_OCIO_VIEW=default` uses the config's default view (ACES 2.0
  SDR, once OpenUSD is fixed). With another config the extension leaves its
  defaults alone. The fix is the missing `sampDesc.debugName = samplerName;`
  in Hdx's 1D/2D LUT loop; PrismRT does not patch OpenUSD.
- On the CPU device the usdview viewport matches the CLI's
  `--view Un-tone-mapped` PNG to within one 8-bit level outside the HUD
  (`tests/hydra/fixtures/color_management.usda` and the Cornell box).
- usdview fills its *OCIO Config* menu (displays, views, input colour spaces)
  with `PyOpenColorIO`, which is not installed: vcpkg's port has no Python
  bindings, and the pip wheel brings its own OpenColorIO library. usdview then
  prints "Could not import PyOpenColorIO..." and the menu stays empty; the
  view comes from the extension (above) or from OpenColorIO's
  `OCIO_ACTIVE_DISPLAYS`/`OCIO_ACTIVE_VIEWS` (these also change the CLI's
  defaults, as they act on the config).
- `sRGB` only applies the sRGB curve: an ACEScg render then shows ACEScg
  values as Rec.709 (less saturated, not tone mapped). It matches the CLI's
  `Un-tone-mapped` sRGB view when rendering with
  `prism:colorManagement:workingSpace = lin_rec709`.
- `usdrecord --colorCorrectionMode openColorIO --renderer PrismRT` uses the
  config's default view (with `$OCIO` set), so it hits the same ACES 2.0 bug;
  set `OCIO_ACTIVE_VIEWS=Un-tone-mapped` for it.

## Tests

- `prism_color_management_test`: matrices (against the ACES Rec.709-to-AP1
  matrix), luminance weights, the views (gamma 2.2 to 2e-6 of `pow`), texel
  conversion and its cache key, light colours, and the generated MaterialX
  transforms in both working spaces (the spike, as a regression test).
- `prism_color_{cli,hydra}_{acescg,lin_rec709}_patches`: the fixture
  `tests/hydra/fixtures/color_management.usda` (untagged, `lin_rec709`- and
  `srgb_texture`-tagged constants, an sRGB-tagged image, a coloured dome)
  rendered on the CPU by both loaders, each value checked against constants;
  `prism_color_loaders_*_match` compares the loaders.
- `prism_color_view_once_test`: the CLI's PNG is its EXR through the view.
- `prism_color_legacy_*`: linear Rec.709 with the gamma 2.2 view against
  images stored from the renderer before colour management
  (`tests/hydra/fixtures/legacy_rec709`; bit-identical on the machine that
  made them, compared at the usual 1% so other CPUs pass).

## Metal

The shared integrator change is one `float3` in `SampleSettings` and one
`float4` at the end of `GpuFrame`, read from the existing frame buffer; see
the Metal checklist in [gpu_devices.md](gpu_devices.md).
