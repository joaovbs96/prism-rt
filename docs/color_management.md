# Colour management

PrismRT renders in one scene-linear **working space**, ACEScg by default,
taken from an OpenColorIO config. Every colour that enters the renderer is
converted into it once; linear outputs stay in it, and 8-bit outputs go
through a display and view (ACES 2.0 SDR on an sRGB display by default).

The code is `src/color/color_management.{h,cpp}` (`prism::color`). The
colour management belongs to each renderer (`prism::RendererContext::setColor`,
`src/renderer/renderer_context.h`): both loaders, the texture service, the
MaterialX generator and the render devices read their renderer's.

## Config, working space and view

| Setting | CLI | Hydra render setting | Default |
| --- | --- | --- | --- |
| Config | `--ocio-config FILE\|URI` | `prism:colorManagement:ocioConfig` | `$OCIO`, else `ocio://studio-config-latest` |
| Working space | `--working-space NAME` | `prism:colorManagement:workingSpace`, else the settings prim's `renderingColorSpace`, else the `renderingColorSpace` render setting | `$PRISM_WORKING_SPACE`, else `ACEScg` (the config's `scene_linear` role if it has no ACEScg) |
| Output space (Hydra only) | none | `prism:colorManagement:outputSpace` | the `renderingColorSpace`, else linear Rec.709 ([Output space](#output-space)) |
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
- **`renderingColorSpace`**, the standard `UsdRenderSettings` attribute
  ("the rendering color space the renderer should use for internal color
  calculations"), is the working space when neither `--working-space` nor
  `prism:colorManagement:workingSpace` (nor `PRISM_WORKING_SPACE`) names one.
  Precedence: flag > `prism:colorManagement:workingSpace` >
  `PRISM_WORKING_SPACE` (the setting's environment default) >
  `renderingColorSpace` > ACEScg. Both loaders read it from the active
  settings prim (the CLI through `ResolveRenderJob`, the delegate from the
  Hydra render settings schema). The delegate also takes it as the
  unnamespaced render setting `renderingColorSpace`
  (`HdRenderSettingsPrimTokens->renderingColorSpace`, as Typhoon does), for
  hosts that do not author into the stage; the prim's attribute comes first
  ([hydra.md](hydra.md)).
  `prism::color::RenderingColorSpaceWorkingSpace` maps its token:
  - USD's names (`GfColorSpaceNames`: `lin_rec709_scene`, `lin_ap1_scene`,
    `lin_p3d65_scene`, `lin_rec2020_scene`, `lin_adobergb_scene`, ...)
    resolve through the config's aliases (the studio config has the ASWF
    interop names), e.g. `lin_ap1_scene` is ACEScg.
  - `data`, `raw` and `identity` render in **linear Rec.709**. Typhoon
    (NVIDIA's proposed reference renderer) documents `data` as "bypasses
    source color transforms"; PrismRT has no mode that skips them, and linear
    Rec.709 is the space USD authors untagged values and light colours in, so
    those reach the image unchanged exactly as with no transforms. Only
    inputs tagged with another space (a `colorSpace`, a MaterialX
    `colorspace`, an sRGB image) still convert. See docs/DECISIONS.md.
  - `unknown` and an unauthored attribute take the default (ACEScg). A name
    the config lacks or a space that is not scene-linear (`srgb_rec709_scene`,
    `g22_ap1_scene`) also takes the default, with a message (`PrismRT:
    renderingColorSpace '...' is not a scene-linear colour space ...`)
    instead of failing the render: the attribute is authored for every
    renderer.
  - Typhoon's default is `lin_rec709_scene`; PrismRT's stays ACEScg. A stage
    that wants the same image in both authors the attribute.
  Tests: `ctest -R color_rendering_space` (both loaders: the patches in linear
  Rec.709 and bit-identical to `--working-space lin_rec709`; the flag and
  `prism:colorManagement:workingSpace` override it) and
  `prism_color_management_test` (the token mapping).
- In Hydra, changing the config or working space rebuilds the scene
  (materials, lights, domes and their textures convert again). Display and
  view are only recorded there: the colour AOV is linear.
- Each Hydra delegate has its own colour management (its renderer's). Two
  delegates in one process with different working spaces each render their
  own exactly (`prism_hydra_host_working_spaces`); a new delegate starts from
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
| MaterialX values and images with a colour space (`colorspace` on the input, node, graph or document, also of a `.mtlx` referenced into USD: [materialx_compatibility.md](materialx_compatibility.md#document-colour-space-through-usd); USD `colorSpace` metadata) | that space | MaterialX's generated MDL code, into the working space |
| MaterialX values and images without a colour space | - | none: used as working-space values |
| MaterialX data spaces (`raw`, `Raw`, `data`, `non-color`) | - | none (the loader maps them to `none`) |
| UsdLux `inputs:color`, `colorTemperature` (blackbody) | linear Rec.709 | matrix, in `MakeSceneLight` (both loaders) and the Hydra dome |
| Dome and rect-light `inputs:texture:file` | linear Rec.709 | texels, when the texture is prepared |
| CLI `--env FILE`, `--env-color` | linear Rec.709 | texels; matrix |
| glTF materials (MaterialX `gltf_pbr`, [gltf.md](gltf.md)) | colour textures `srgb_texture`, factors `lin_rec709`, data textures raw | MaterialX's generated MDL code; `attenuation_color` (uniform) and `COLOR_0` on the CPU at load |
| OBJ materials (MaterialX `open_pbr_surface`, [obj.md](obj.md)) | colour maps `srgb_texture`, MTL colours `lin_rec709`, data maps raw | MaterialX's generated MDL code |
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
- **Hydra colour AOV**: linear in the delegate's **output space** (below),
  linear Rec.709 by default. PrismRT's presentation applies no view; the
  host does.

### Output space

A Hydra host cannot tell the delegate which colour space it expects:
`HdAovDescriptor` carries a format but no colour space, and the delegate
never sees the host's colour correction task. Stock `HdxColorCorrectionTask`
in `sRGB` mode applies only the sRGB transfer curve, so it reads the colour
AOV as linear Rec.709, as Storm and Typhoon write it. The delegate therefore
renders in the working space but presents its AOVs in a separate output
space, `prism:colorManagement:outputSpace` ([hydra.md](hydra.md)):

- empty (default): the `renderingColorSpace` when one is set (the active
  settings prim's, else the `renderingColorSpace` render setting; `data`,
  `raw` and `identity` are linear Rec.709), else **linear Rec.709**. A host
  that knows nothing of PrismRT shows the right colours; a stage that asks
  for a rendering colour space gets its AOVs in it, as Typhoon does;
- `working`: the working space, as before the output space existed (the
  AOVs are then bit-identical to earlier builds);
- any scene-linear OCIO name or alias of the config. A name the config lacks
  or a space that is not scene-linear warns and takes the default.

The conversion is a 3x3 matrix (working space to output space, from OCIO) in
the render buffers (`PrismRenderBuffer::SetOutputTransform`), applied when a
pixel is encoded: after the accumulation (the buffers accumulate
working-space samples and convert the mean), to previews, refinement tiles
and the denoised colour (the denoiser runs on working-space data). When the
output space is the working space nothing converts.

- **Which AOVs:** `color`, `denoised` and the light AOVs `albedo`, `diffuse`,
  `specular`, `emission` all convert, so `diffuse + specular + emission =
  color` still holds and the denoiser's guides stay consistent with what a
  host composites. Data AOVs (depth, normals, positions, ids, `primvars:st`)
  do not.
- **Out of gamut:** colours outside the output gamut (ACEScg colours outside
  Rec.709) become negative components. Float AOVs keep them (as Storm's do);
  8-bit (`UNorm8`) AOVs clamp to [0, 1], 16-bit floats keep them.
- **Clear values** are the host's and are not converted.
- **Changing** the output space restarts the accumulation; it does not
  rebuild the scene.
- **Read back:** `GetRenderSetting("prism:colorManagement:aovColorSpace")`
  returns the OCIO name of the resolved output space (the `prismUsdview`
  extension sets usdview's OCIO input colour space from it), and the render
  stats `prismOutputColorSpace` and `prismWorkingColorSpace` report both.
- **Not affected:** the CLI's outputs, the SDK and Python (their films are in
  the working space), and `prism_hydra_render`, which sets `working` (its
  images are tagged in the working space, like the CLI's) unless given
  `--output-space` or `--host-srgb`.
- Rejected: defaulting the delegate's working space to linear Rec.709. It is
  simpler but changes the rendered image, not only its presentation
  (docs/DECISIONS.md, 2026-10-31).

Test: `prism_color_host_srgb_*` renders `tests/hydra/fixtures/color_macbeth.usda`
(the 24 ColorChecker patches) through `prism_hydra_render --host-srgb`, which
behaves like a stock Hdx host in `sRGB` mode (no Prism settings; the colour
AOV through the sRGB curve into an 8-bit PNG), and compares it with the CLI's
`--working-space lin_rec709` EXR through the same curve; and
`prism_color_output_space_matrix` checks that the default output is the
`working` output times the ACEScg-to-Rec.709 matrix.

### Failures never reach the host

The delegate creates its colour management with
`ColorManagement::CreateOrFallback`: an unreadable config, a working space
the config lacks or that is not scene-linear, or a config without linear
Rec.709 warns (`TF_WARN`) and falls back to the built-in config with the same
working space, then to the built-in defaults, and finally to a pass-through
colour management (linear Rec.709, nothing converts, no config), so an
exception never reaches Hydra. Linear Rec.709 is found by name or alias
(`lin_rec709`, `lin_rec709_scene`, `Linear Rec.709 (sRGB)`, `lin_srgb`,
`srgb_linear`, `Utility - Linear - sRGB`, ...) before a config counts as
lacking it; this applies to the CLI too. The delegate builds no view
processor (`Create(options, false)`), so unknown display and view names are
only recorded. A render setting that names an unusable config or working
space later keeps the previous colour management, with a warning. Tests:
`prism_hydra_color_fallback_*` (`$OCIO` naming a missing file or
`tests/hydra/fixtures/ocio_no_rec709.ocio`: a warning and the same image as
the built-in config; unknown display and view names) and
`prism_color_management_test` (aliases, fallbacks, pass-through).

### usdview

usdview colour-corrects the colour AOV itself (Hdx's colour correction task).
In its `openColorIO` mode it converts from its OCIO input colour space
(without one, the config's `scene_linear` role, ACEScg) through the config's
default display and view: with the studio config `sRGB - Display` /
`ACES 2.0 - SDR 100 nits (Rec.709)`, like the CLI's 8-bit outputs. That needs
PrismRT's OpenUSD patch (below). The `prismUsdview` extension sets the input
colour space to the delegate's output space
(`prism:colorManagement:aovColorSpace`, `Linear Rec.709 (sRGB)` by default)
when it first reports one and whenever it changes; a space picked in
usdview's *OCIO Config* menu stays until then. In `sRGB` mode usdview applies
the sRGB curve to the AOV, which is right for the default linear Rec.709
output (not tone mapped). So both modes show the right colours; the forced
`openColorIO` start-up mode is kept for the ACES view (the look of the CLI's
PNGs). Storm and Typhoon also write linear Rec.709, so the input colour space
the extension leaves set is right for them too (unless
`prism:colorManagement:outputSpace` is `working` with an ACEScg working
space).

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
- The extension also turns usdview's "Enable Default Dome Light" on at
  start-up (`PRISM_USDVIEW_DEFAULT_DOME_LIGHT=off|saved` to change; in the
  package `PRISMRT_USDVIEW_DEFAULT_DOME_LIGHT`); see [hydra.md](hydra.md).
- The extension leaves the display and view to the config's defaults.
  `PRISM_USDVIEW_OCIO_VIEW` chooses another view (`default`: the config's), on
  `PRISM_USDVIEW_OCIO_DISPLAY` (default `sRGB - Display`; usdview ignores a
  display without a view). In the package: `PRISMRT_USDVIEW_OCIO_VIEW` and
  `PRISMRT_USDVIEW_OCIO_DISPLAY`.
- **OpenUSD patch for the ACES 2.0 views.** Unpatched, OpenUSD 26.08's
  `HdxColorCorrectionTask` (OCIO v2 path; unchanged on OpenUSD's `dev` branch
  at `bc32a655a417`, 2026-10-13) renders every view that uses OpenColorIO's
  1D/2D LUT textures black on OpenGL and Vulkan, among them the studio
  config's ACES 2.0 views (a single-channel and an RGB 363-entry table): it
  names the sampler of 3D LUTs only ("Failed verification:
  !texInfo.texName.empty() && !texInfo.samplerName.empty()"), creates
  single-channel LUTs without data and counts RGB LUTs as 4 floats per texel.
  The analytic views (`Un-tone-mapped`, `Video (colorimetric)`) were not
  affected. `tools/ci/patches/openusd-26.08-hdx-ocio-lut-sampler.patch` fixes
  all three; `build_usd_stack.ps1` applies every
  `tools/ci/patches/openusd-<version>-*.patch` for deps.json's OpenUSD
  version after cloning (`git apply --check`, then `git apply`; the build
  stops if one does not apply), copies them to `<prefix>\patches` (the package
  ships them with OpenUSD's licence) and the patch hash goes into the CI cache
  key and the local prefix name (`openusd-26.08-ocio2.6.0-p<hash>`, see
  [deployment.md](deployment.md#local-dependency-stack)). Upstream: OpenUSD
  PR #3419 (open since 2024-11; fixes the first two) and issue #3274.
  **Dropping the patch:** when an OpenUSD release has the fix, the upgrade
  (deps.json's `openusd` version) no longer picks up the `openusd-26.08-`
  patch; delete it then. If a new release still has the bug, rebase the patch
  as `openusd-<new version>-hdx-ocio-lut-sampler.patch` (the build fails if it
  no longer applies). With an unpatched OpenUSD (an older prefix), set
  `PRISM_USDVIEW_OCIO_VIEW=Un-tone-mapped`.
- On the CPU device the usdview viewport (camera gate) matches the CLI's PNG
  of the same view, the default ACES 2.0 SDR one as well as
  `Un-tone-mapped`: `tests/hydra/fixtures/color_management.usda` to within
  one 8-bit level (98% of the pixels; the rest are usdview's gate and axis
  lines), the Cornell box to 0.3 levels on average over 16x16-pixel blocks
  (per-pixel differences are sampling noise). usdrecord's image of the
  fixture equals the CLI's where it is opaque (it writes the dome with
  alpha 0).
- usdview fills its *OCIO Config* menu (displays, views, input colour spaces)
  with `PyOpenColorIO`. vcpkg's port has no Python bindings, so the pip wheel
  `opencolorio==2.6.0` (pinned in `tools/deps.json`, the same version as
  vcpkg's) provides it. The wheel brings its own
  `PyOpenColorIO\bin\OpenColorIO_2_6.dll`, but the module imports it by name:
  usdview loads Hdx (and so vcpkg's `OpenColorIO_2_6.dll`) first, and Windows
  reuses the loaded module, so one copy serves Hdx, PrismRT and the Python
  module (checked in a running usdview: a single `OpenColorIO_2_6.dll`, from
  vcpkg's `bin`). If a script imports `PyOpenColorIO` before `pxr`, the wheel's
  DLL is the one loaded and Hdx then uses it. Both are 2.6.0, and the usdview
  launchers import `pxr` first. The menu lists the config's displays, views
  and colour spaces; it shows no checked view while the extension leaves the
  config's default view in use (an empty display/view setting). The view
  comes from the extension (above) or from OpenColorIO's
  `OCIO_ACTIVE_DISPLAYS`/`OCIO_ACTIVE_VIEWS` (these also change the CLI's
  defaults, as they act on the config). The package ships the wheel without
  its `bin` directory, so it holds exactly one OpenColorIO DLL.
- `sRGB` only applies the sRGB curve: with the default output space (linear
  Rec.709) it shows the right colours, not tone mapped, like the CLI's
  `Un-tone-mapped` sRGB view. With `prism:colorManagement:outputSpace =
  working` and an ACEScg working space it would show ACEScg values as
  Rec.709 (less saturated), as every build did before the output space.
- How to check by hand: `.\run_usdview.ps1 tests\hydra\fixtures\color_macbeth.usda`,
  then View > Color Correction > `sRGB` and `openColorIO`: the patches keep
  their hues in both (the ACES view is a little darker and desaturates the
  brightest ones); the *OCIO Config* menu's input colour space reads
  `Linear Rec.709 (sRGB)`.
- `usdrecord --colorCorrectionMode openColorIO --renderer PrismRT` uses the
  config's default view (with `$OCIO` set): ACES 2.0 SDR with the studio
  config, like the CLI. `OCIO_ACTIVE_VIEWS` chooses another.

## Tests

- Output space and fallbacks: see [Output space](#output-space) and
  [Failures never reach the host](#failures-never-reach-the-host).
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
