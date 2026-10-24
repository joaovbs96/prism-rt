# PrismRT in Hydra and usdview

`hdPrism` consumes Hydra scene-index data directly and shares the existing PrismRT path tracer and MaterialX/MDL backend with the standalone executable. It targets unmodified OpenUSD 26.08 (the version in `tools/deps.json`). USD's built-in render-delegate adapter owns the render index and usdview control interface; PrismRT uses the exported `HdRenderDelegate::SetTerminalSceneIndex` and `Update` hooks for scene synchronization. The adapter receives only cameras, buffers, and tasks. Geometry and materials are read from the full upstream scene index and do not have legacy `Sync` implementations.

This is a scene-index renderer behind USD's compatibility adapter, not a fully native `HdRenderer` implementation. The adapter is intentional: OpenUSD's Windows DLL interface (checked with 26.05) does not export all the symbols needed by an external `HdRenderer` subclass. Using USD's own adapter avoids changing USD headers, exports, or binaries. The build script does not apply USD patches.

## Build and launch

Build OpenUSD with Python, imaging, GPU/GL support and usdview before building the plugin. `tools/build_deps_local.ps1` builds OpenSubdiv, MaterialX and OpenUSD the way CI does, into versioned prefixes; point `cmake/PrismLocalDependencies.cmake` and `prism_local_env.ps1` at them ([deployment.md](deployment.md#local-dependency-stack)):

```powershell
.\tools\build_deps_local.ps1 -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src
cmake -S . -B build -DPRISM_BUILD_HYDRA=ON -DPRISM_COPY_ASSETS=OFF
cmake --build build --config RelWithDebInfo
powershell -NoProfile -ExecutionPolicy Bypass -File run_usdview.ps1 tests/hydra/fixtures/parity.usda
```

The equivalent configure/build preset is `prism-hydra`; use `cmake --preset prism-hydra`, `cmake --build --preset prism-hydra`, and `ctest --preset prism-hydra`. The presets reuse PrismRT's existing build directory and USD installation.

Select **PrismRT** in usdview's renderer menu. The launcher sets plugin discovery, the installed USD Python modules, DLL search paths, and MaterialX library paths. It does not install a separate Python USD package. `PRISM_BUILD_HYDRA` defaults to `OFF`; the standalone executable remains available when it is enabled. Disabling asset copying avoids copying large datasets for validation runs.

For a shell containing the same runtime configuration, start PowerShell with `-ExecutionPolicy Bypass` when local script execution is disabled, then run:

```powershell
. ./prism_env.ps1
./build/RelWithDebInfo/PrismRT.exe --help
ctest --test-dir build -C RelWithDebInfo --output-on-failure
python tests/hydra/test_usdview_selection_plugin.py
python tests/hydra/validate.py
```

## Rendering and selection settings

Every setting is a Hydra render setting (usdview's renderer settings, or
`prism:*` attributes of the active RenderSettings prim, see [Render settings
and products](#render-settings-and-products)). The standalone CLI reads the
same `prism:*` attributes from the RenderSettings prim and has a flag for each
setting that affects a one-shot render. **Precedence:** command-line flag
(CLI, `prism_hydra_render`) > RenderSettings `prism:*` > environment variable
(where one is listed) > built-in default. The defaults live in
`src/render_defaults.h`, shared by the delegate, the CLI and
`prism_hydra_render`. *Hydra change* says what changing the setting does in a
running delegate.

| Setting | Default | CLI flag | Hydra change | Meaning |
| --- | --- | --- | --- | --- |
| `prism:samplesPerPixel` | 2048 | `--spp` | more samples resume the accumulation; fewer stop it | Samples per pixel; with adaptive sampling the per-pixel maximum |
| `prism:adaptiveThreshold` | 0.01 | `--adaptive-threshold` | restarts the accumulation | Adaptive sampling: a pixel stops once its error estimate is below this (0: every pixel gets `samplesPerPixel`). See [Adaptive sampling and time limit](#adaptive-sampling-and-time-limit) |
| `prism:adaptiveMinSamples` | 64 | `--adaptive-min-samples` | restarts the accumulation | Samples before a pixel may stop |
| `prism:timeLimitSeconds` | 0 | `--time-limit` | like `samplesPerPixel` | Render time limit in seconds (0: none); rendering stops at whichever comes first of the threshold, the samples and the limit |
| `prism:maxDepth` | 32 | `--max-depth` | restarts the accumulation | Maximum path depth |
| `prism:rrMinDepth` | 4 | `--rr-min-depth` | restarts the accumulation | Start of Russian roulette |
| `prism:maxSampleRadiance` | 10 | `--max-sample-radiance` | restarts the accumulation | Existing hue-preserving per-sample clamp; zero disables it |
| `prism:neeProbabilityMin` | 0 | `--nee-probability-min` | restarts the accumulation | Probabilistic NEE: from `prism:neeFromDepth` on, lights are sampled with probability clamp(path weight, this, 1); 0 samples them at every vertex (lower noise, slightly more work per sample) |
| `prism:neeFromDepth` | 2 | `--nee-from-depth` | restarts the accumulation | First depth of probabilistic NEE |
| `prism:seed` | 0 | `--seed` | restarts the accumulation | Per-pixel RNG seed |
| `prism:lightSelection` | `spatial` | `--light-selection` | restarts the accumulation | NEE picks among the analytic (UsdLux) lights by their estimated irradiance at the shading point (`spatial`, for 2 to 64 analytic lights), or by power only (`power`); see [Sampling](gpu_devices.md#sampling) |
| `prism:environmentSampling` | `mis` | `--environment-sampling` | restarts the accumulation | Environment next-event sampling with MIS, or `bsdf` |
| `prism:textureFiltering` | `filtered` | `--texture-filtering` | restarts the accumulation | Footprint-based anisotropic mip filtering, or highest-resolution `bilinear` reference mode |
| `prism:textureStreaming` | true | `--texture-streaming` | the device prepares the scene again (textures re-upload) | GPU devices that stream textures (Vulkan) keep only the mip levels shading asks for, within the texture budget; false uploads whole textures reduced to the budget (env `PRISM_GPU_TEXTURE_STREAMING=0` turns streaming off for the process); see [texture streaming](texture_service.md#gpu-texture-streaming) |
| `prism:textureStreamingPrepass` | false | none: the CLI always settles streamed textures before its first sample (as `true`) | restarts the accumulation | Settle streamed textures with feedback-only passes before an accumulation and hold them for all of its samples (deterministic final images; `prism_hydra_render` sets it). Otherwise an accumulation restarts at most once, early, and later loads sharpen it in place |
| `prism:gpuTextureBudgetMiB` | 0 (env `PRISM_GPU_TEXTURE_BUDGET_MIB`) | `--gpu-texture-budget-mib` | the device prepares the scene again (textures re-upload at the new sizes) | GPU memory for textures in MiB (at least 64); 0 is 45% of the device's memory. Textures beyond it upload, or stream, at reduced resolution |
| `prism:subdivisionLevel` | 2 (env `PRISM_SUBDIVISION_LEVEL`) | none (*) | every mesh converts again | Uniform subdivision level of subdivision surfaces, 0-6 (0: control hulls) |
| `prism:subdivisionMaxFaces` | 65536 (env `PRISM_SUBDIVISION_MAX_FACES`) | none (*) | every mesh converts again | Faces one mesh may refine to; denser meshes use lower levels |
| `prism:subdivisionBudgetFaces` | 25165824 (env `PRISM_SUBDIVISION_BUDGET_FACES`) | none (*) | every mesh converts again | Faces all refined meshes of the scene may add up to; beyond it the meshes refining to the most faces drop levels first |
| `prism:displacementLevel` | 4 (env `PRISM_DISPLACEMENT_LEVEL`) | none (*) | every mesh converts again | Subdivision level of meshes their material displaces, 0-6 |
| `prism:displacementMaxFaces` | 262144 (env `PRISM_DISPLACEMENT_MAX_FACES`) | none (*) | every mesh converts again | Faces one displaced mesh may refine to |
| `prism:sssFallback` | false (env `PRISM_SSS_FALLBACK`) | `--sss-fallback` | every material generates again | Render subsurface scattering as its diffuse lobe instead of tracing it ([MaterialX compatibility](materialx_compatibility.md)). Process-wide, like the colour management |
| `prism:honorDoubleSided` | false | `--honor-double-sided` | rebuilds the scene | Honour USD `doubleSided`: back faces of single-sided meshes (`doubleSided` false, USD's default) are not hit, unless their material transmits. Off, every mesh is two-sided as before (most path tracers ignore `doubleSided`). See [Scene features](#scene-features) |
| `prism:device` | `auto` | `--device` | switches the render device | `auto`, `cpu`, `cpu-reference`, `vulkan` or `metal`; env `PRISM_DEVICE` sets the initial value |
| `prism:colorManagement:ocioConfig` | empty | `--ocio-config` | rebuilds the scene | OpenColorIO config; empty uses `$OCIO`, else `ocio://studio-config-latest`. See [colour management](color_management.md) |
| `prism:colorManagement:workingSpace` | empty | `--working-space` | rebuilds the scene | Working space; empty is ACEScg. `lin_rec709` renders as before colour management |
| `prism:colorManagement:display`, `prism:colorManagement:view` | empty | `--display`, `--view` | none | Recorded only in the delegate: the colour AOV stays linear in the working space and usdview applies its own colour correction (`OCIO=ocio://studio-config-latest` for its OCIO mode). The CLI encodes 8-bit outputs with them |
| `domeLightCameraVisibility` | true | none: the CLI shows domes to the camera | restarts the accumulation | Host setting (usdview's dome visibility): show dome light in primary camera rays; secondary lighting remains enabled |
| none (CLI only) | true | `--default-dome` | none | The CLI lights a scene without lights, constant-emission geometry or `--env`/`--env-color` with USD's default dome (StinsonBeach, intensity 1). In the delegate the host provides that dome (usdview's Lights > Enable Default Dome Light), so a `prism:` setting would only reach the CLI: an authored `prism:defaultDome` could not make usdview's toggle agree, and a stage that wants light should author a dome |
| `prism:sphereBatching` | false | Hydra only (**) | rebuilds the scene | Experimental batches of at most 1,024 eligible analytic spheres; ordinary instancing remains the default |
| `prism:hiddenGeometryBudgetMiB` | 0 | Hydra only (**) | none | Soft budget for retained hidden converted geometry; zero keeps unlimited retention |
| `prism:previewMaxDimension` | 0 | Hydra only (interactive) | next preview | Preview longest edge, clamped to 16-1024 pixels; 0 is automatic (512 on GPU devices, 96 on the CPU) |
| `prism:adaptivePreview` | true | Hydra only (interactive) | next preview | Adapt preview resolution using measured render time |
| `prism:previewTargetMs` | 33 | Hydra only (interactive) | next preview | Preview render-time target, clamped to 1-1000 ms |
| `prism:refinementDelayMs` | 100 | Hydra only (interactive) | next preview | Quiet interval before refinement, clamped to 0-2000 ms |
| `prism:gpuPreparationPreview` | true | Hydra only (interactive) | next preparation | While a GPU device (Vulkan, Metal) is still preparing the scene after one second, render progressively on the CPU and continue on the GPU from the same sample once it is ready; env `PRISM_GPU_PREPARATION_PREVIEW=0` sets the initial value, see [CPU preview while the GPU prepares](#cpu-preview-while-the-gpu-prepares) |
| `prism:gpuPreparationPreviewMaxMiB` | 8192 | Hydra only (interactive) | next preparation | Skip that preview when the scene's Embree memory exceeds this many MiB (0 never skips); env `PRISM_GPU_PREPARATION_PREVIEW_MAX_MIB` |
| `prism:resourceRevision` | 0 | Hydra only (interactive) | materials and domes reload | Increment to invalidate cached material and external texture fingerprints and reload dome resources without rebuilding geometry (a CLI run always reads its files afresh) |
| `prism:selectionOutline` | true | Hydra only (interactive) | presentation only | Draw selection as an outline; false uses filled highlighting |
| `prism:selectionOutlineRadius` | 2 | Hydra only (interactive) | presentation only | Outline thickness in pixels |
| `prism:selectionUseHostSettings` | false | Hydra only (interactive) | presentation only | Use the host's Hdx outline parameters instead of PrismRT's overrides |

(*) The standalone loader has no subdivision surfaces or displacement yet
([feature roadmap](feature_roadmap.md#standalone-cli)): it renders control
hulls, so these settings cannot apply, and the CLI warns when the
RenderSettings prim authors them. `prism_hydra_render` has
`--subdivision-level`, `--subdivision-max-faces`, `--subdivision-budget-faces`,
`--displacement-level` and `--displacement-max-faces`.
The subdivision, displacement and texture budget settings replace the
environment-variable-only configuration; the variables now give the defaults.

(**) Not image settings: sphere batching changes how the delegate's retained
scene stores spheres (the CLI loads its spheres its own way, and the image is
the same), and the hidden geometry budget bounds the memory of hidden
prototypes kept for later edits, which a one-shot render does not have.

### Adaptive sampling and time limit

Adaptive sampling is on by default: up to 2048 samples per pixel, threshold
0.01, at least 64 samples. The error estimate (`src/device/adaptive_sampling.h`)
is the Cycles one: |mean of all samples - mean of the even samples| summed over
RGB, over the square root of the mean, relative to the image's mean brightness,
so the threshold means the same at any exposure. Every 8 samples after the
minimum, pixels below the threshold stop, unless a neighbour in their 3x3
window has not converged.

Threshold study (CPU device, 320x180, against a 16384-spp non-adaptive
reference of another seed; NRMSE as `prism_compare_images` computes it):

| Scene | Threshold | Mean spp | Pixels at 2048 | Time (s) | NRMSE | Fixed, same mean spp: NRMSE | Fixed, same time: spp / NRMSE |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Cornell box | 0.005 | 1104 | 52% | 24 | 0.0015 | 0.0021 | 1999 / 0.0015 |
| Cornell box | 0.01 | 1102 | 52% | 24 | 0.0015 | 0.0021 | 1984 / 0.0015 |
| Cornell box | 0.02 | 1079 | 49% | 26 | 0.0015 | 0.0021 | 2021 / 0.0015 |
| Cornell box | 0.05 | 758 | 22% | 21 | 0.0019 | 0.0025 | 1376 / 0.0018 |
| Cornell box | 0.1 | 372 | 4% | 10 | 0.0033 | 0.0038 | 639 / 0.0028 |
| Chess | 0.005 | 1603 | 71% | 53 | 0.0187 | 0.0213 | 2139 / 0.0184 |
| Chess | 0.01 | 1444 | 65% | 51 | 0.0187 | 0.0224 | 2010 / 0.0189 |
| Chess | 0.02 | 1342 | 62% | 51 | 0.0187 | 0.0233 | 2014 / 0.0189 |
| Chess | 0.05 | 1247 | 55% | 62 | 0.0188 | 0.0241 | 2077 / 0.0186 |
| Chess | 0.1 | 980 | 32% | 51 | 0.0215 | 0.0272 | 1535 / 0.0217 |
| Egg (jade) | 0.005 | 2048 | 100% | 58 | 0.0105 | 0.0105 | 2172 / 0.0103 |
| Egg (jade) | 0.01 | 2048 | 100% | 58 | 0.0105 | 0.0105 | 2091 / 0.0105 |
| Egg (jade) | 0.02 | 2037 | 98% | 57 | 0.0106 | 0.0106 | 2139 / 0.0104 |
| Egg (jade) | 0.05 | 1478 | 36% | 60 | 0.0125 | 0.0120 | 1750 / 0.0113 |
| Egg (jade) | 0.1 | 526 | 1% | 23 | 0.0214 | 0.0192 | 629 / 0.0176 |
| OpenPBR | 0.005 | 2048 | 100% | 68 | 0.0351 | 0.0351 | 2089 / 0.0348 |
| OpenPBR | 0.01 | 2048 | 100% | 68 | 0.0351 | 0.0351 | 2120 / 0.0345 |
| OpenPBR | 0.02 | 2048 | 100% | 71 | 0.0351 | 0.0351 | 1618 / 0.0390 |
| OpenPBR | 0.05 | 2018 | 94% | 82 | 0.0353 | 0.0353 | 1978 / 0.0357 |
| OpenPBR | 0.1 | 1515 | 40% | 77 | 0.0403 | 0.0401 | 1907 / 0.0362 |

| Scene | 0.01, min 64 | 0.01, min 16 | 0.05, min 64 | 0.05, min 16 |
| --- | --- | --- | --- | --- |
| Cornell box | 1102 spp, 0.0015 | 1079 spp, 0.0015 | 758 spp, 0.0019 | 735 spp, 0.0019 |
| Chess | 1444 spp, 0.0187 | 1439 spp, 0.0187 | 1247 spp, 0.0188 | 1232 spp, 0.0188 |
| Egg (jade) | 2048 spp, 0.0105 | 2048 spp, 0.0105 | 1478 spp, 0.0125 | 1478 spp, 0.0125 |
| OpenPBR | 2048 spp, 0.0351 | 2048 spp, 0.0351 | 2018 spp, 0.0353 | 2018 spp, 0.0353 |

Every render is 320x180 on the CPU device, at most 2048 samples and at least 64
unless noted; *fixed* renders use no adaptive sampling. *Same time* is the
fixed sample count the adaptive render's time would have bought (from the two
measured times; the machine was in use, so single timings vary by up to 30%,
for example OpenPBR's 0.02 row). Scenes: ssets/scenes/CornellBox.usda, the
chess set, MaterialEggs' egg_jade.usda and the OpenPBR shader playground
(/renderCams/renderCam_mainCU).

- **Thresholds 0.005 to 0.02 lose nothing.** Their NRMSE equals that of 2048
  fixed samples: the pixels they stop early (half of the Cornell box, a third
  of the chess set) contribute no measurable error, and the 8x8 tile with the
  largest error (caustics, small lights, edges) is the same as at 2048. At
  equal mean samples they beat fixed sampling by 25-30% NRMSE (Cornell box) and
  15-20% (chess).
- **0.05 and above converge too early.** The egg's NRMSE rises from 0.0105 to
  0.0125 at 0.05 (worse than fixed sampling with the same mean, 0.0120), and
  0.1 is worse still in every scene but the chess set.
- **On the CPU, adaptive sampling saves samples, not much time.** The pixels it
  keeps sampling are the expensive ones (long indirect paths), so its time is
  close to that of 2048 fixed samples, and fixed sampling for the same time
  reaches the same NRMSE. Dome-lit, textured scenes (the egg, OpenPBR) do not
  reach 0.01 within 2048 samples anywhere: they render like 2048 fixed samples.
- **0.01 is the default**: as safe as 0.005, nearly as economical as 0.02
  (2-7% more samples than 0.02 with the same error), and a factor of five from
  the threshold where pixels stop too early.
- **Minimum 64 samples.** At 0.01 and 0.05, 16 minimum samples change neither
  the NRMSE nor the worst tile and save at most 2% of the samples; 64 keeps a
  margin against a pixel whose first samples all miss a small light or a
  caustic, for that small cost.

**Time limit.** `prism:timeLimitSeconds` (`--time-limit`) bounds the sampling
time; 0 (the default) means none. It counts wall-clock time from the first
full-resolution sample: scene loading, material compilation and device
preparation are not counted (in the delegate neither are the preview, the
refinement delay, the texture-streaming prepass and the time between render
calls; pauses are). It is
checked between samples, so every pixel has at least one sample and the last
sample in progress completes. In the delegate the limit applies per
accumulation: a camera move or scene edit starts a new accumulation and a new
limit, and the progressive passes then stop and report converged; raising the
limit or the samples resumes the accumulation for the remaining time. In
`prism_hydra_render` and usdview the render then reports converged, like a
render that reached its samples; `GetRenderStats()` reports
`timeLimitReached`. In the CLI the limit covers all render products together
and the summary says when it stopped a render. The `cpu-reference` device
ignores the limit.

**Progress.** `percentDone` counts pixels that adaptive sampling stopped as
done and the others by their samples out of `samplesPerPixel`, so it keeps
moving while the last pixels refine; with a time limit it is at least the
elapsed fraction of the limit. It is 100 only once the render converged.
`activePixelFraction` is the fraction of pixels still sampled.

The CLI prints the same measure as a progress line on stderr, for example
`[###########---------] 57%  584/2048 spp  31% px active  1m12s elapsed, ETA 0m54s`:
the samples of the pixels still sampled, the share of pixels adaptive sampling
has not stopped, and an ETA from the elapsed time and the fraction done. On a
terminal the line updates in place (at most four times a second); otherwise
(logs, CTest) it is printed once per 10%. Before the render it names the
loading stage (and, while a GPU device compiles and uploads, `Preparing the
render device`). The device publishes its progress through relaxed atomics
at row and sample boundaries (`prism::device::ImageProgress`) and a monitor
thread reads them, so the render never waits on it. `--no-progress` turns it
off.

PrismRT follows usdview's dome-visibility and background controls. With dome
camera visibility enabled, the environment remains visible to primary rays.
When it is disabled, escaped primary rays are transparent in the raw color
AOV; usdview composites those pixels over its selected clear color for the
viewport. That presentation copy does not alter the render buffer, so image
exports retain transparent RGBA background pixels. This keeps viewport
screenshots opaque while PNG and EXR renders preserve alpha for later
compositing. `tests/hydra/usdview_background.py` exercises both dome states and
host clear colors in a real usdview session, then records PNG and EXR files
through OpenUSD's FrameRecorder and checks their saved alpha channels. The
post-fix run passed; screenshots, exports, and logs are in
`build/hydra-background-final`.

Selection reuses `HdxColorizeSelectionTask`, with native prim/instance/face IDs supplying its selection mask. usdview retains control of highlight color and highlight visibility. Outline changes affect presentation and do not restart accumulation. Host applications can supply `enableOutline` and `outlineRadius` through the standard Hdx task parameters when `prism:selectionUseHostSettings` is enabled.

Selection-offset encoding is cached independently of the camera. Its bounded
four-entry cache keys select/locate modes by the immutable published picking
table, the source selection tracker and version, usdview selection-setting
revision, host highlight-enable flags, and scene selection/instancer-topology
revision. Camera-only frames therefore reuse the encoded selection table.
Selection changes, prim add/remove/rename, universal invalidation, and changes
to selection, `instancedBy`, or instancer topology invalidate it. Cache misses
traverse live selection data only on Hydra's task thread; snapshot workers do
not access those data sources.
Nested instance mappings belong to the published picking table, so a pending
topology edit cannot reinterpret IDs in the previous image. The usdview
extension's authored source indices continue to map against that table. Native
Hydra selections expressed as compact ordinals are conservatively suppressed
after a topology notice until the replacement picking table publishes.

The launcher also loads PrismRT's Python usdview extension from `tools/usdview_plugins`. Stock usdview drops the selected instance index when forwarding selection to its scene index (`UsdImagingGLEngine::AddSelected` ignores it on the scene-index path in 26.05 and still in 26.08). Our extension forwards the selection model's original indices through the internal `prism:usdviewSelection` renderer setting, preserving individual-instance outlines without patching USD. It uses usdview's plugin registration and `StageView` renderer-setting methods; it does not replace usdview functions. Other hosts can supply complete Hydra selection data directly. Without the extension, stock usdview still renders and picks correctly, but individual-instance highlighting follows its upstream whole-prim selection limitation.

The render buffers expose linear floating-point `color` (in the working space, ACEScg by default), projected `depth` in `[0,1]`, `cameraDepth`, world `normal`, eye-space `Neye` and `Peye`, `primvars:st`, and integer `primId`, `instanceId`, and `elementId`. IDs and geometric AOVs use a deterministic center ray independent of antialiasing and depth-of-field samples. `Peye` is the hit position transformed into eye space; `primvars:st` is the interpolated first UV set, with hit UVs used when that primvar is absent. `Pworld` is not currently exposed. `elementId` refers to the coarse polygon face. Prim, instance, and face picking use visible primary intersections; edge, vertex, and deep picking are deferred. Signed and unsigned 16-bit integer AOV buffers and signed 32-bit integer buffers are supported; Int32 IDs retain their exact values. Packed float32/uint8 buffers are rejected with a Hydra warning. Extra Hydra camera clip planes are not applied to ray intersections.

Color alpha accumulates primary coverage, including stochastic cutouts and silhouette samples. With no dome, or `domeLightCameraVisibility=false`, primary rays that escape the scene produce transparent black; unresolved early terminations retain coverage. A visible dome keeps opaque alpha. Changing dome camera visibility restarts preview and full accumulation.

Picking traces deterministic full-resolution rays against the camera and immutable scene owned by the displayed frame, including during low-resolution preview. Retained frames and mapped buffers keep their original geometry across later edits and renderer destruction. An externally retained inactive scene uses a conservative rebuild recorded by `pinnedSnapshotFallbacks`; ordinary released-frame bank reuse remains available.

Hydra caches local-space environment importance distributions by prepared texture identity and exact RGB multiplier. Dome rotation and bank reuse share the distribution while each wrapper keeps its authored direction transform and PDF. Multiplier or texture-resource changes rebuild it. Statistics expose distribution builds, cache hits and uniquely shared importance-table bytes. The exact RGB multiplier is part of the cache key, so intensity/multiplier-only edits rebuild the table; reusing it across those edits remains deferred.

## Render settings and products

PrismRT reads UsdRender prims in both frontends. The **active** settings prim
is the stage's `renderSettingsPrimPath`, or the prim the host picks: usdview's
render settings menu, `prism_hydra_render --render-settings` and
`PrismRT --render-settings`.

| Attribute | usdview (host owns the view) | `prism_hydra_render` (batch) | `PrismRT` CLI |
| --- | --- | --- | --- |
| `prism:*` (the settings above) | yes | yes; its options override authored values | yes; its options take precedence |
| `materialBindingPurposes` | yes | yes | yes |
| `camera` | host | yes | yes, unless `--camera` or a camera position |
| `resolution` | host | yes | yes, unless `--width`/`--height` |
| `pixelAspectRatio`, `aspectRatioConformPolicy` | host | yes | yes |
| `dataWindowNDC` | host | yes | yes |
| `includedPurposes` | host | `render`, `proxy`, `guide` | spheres only skip `guide` and `proxy` (meshes: phase 7) |
| `products` (`RenderProduct`, `RenderVar`) | no | yes | yes, without `--output` and camera options |
| `disableMotionBlur` | no motion blur yet | no motion blur yet | no motion blur yet |
| `disableDepthOfField` | no (warning) | no (warning) | no (warning) |
| `instantaneousShutter`, `renderingColorSpace` | no | no | no |

- **In the delegate.** `HdsiRenderSettingsFilteringSceneIndex`
  (`src/hydra/plugin.cpp`) marks the active prim and keeps only `prism:`
  namespaced settings. `PrismTaskDelegate::_SyncAuthoredSettings` reads the
  prim named by `HdSceneGlobalsSchema`'s active render settings path: its
  `prism:*` settings and its `materialBindingPurposes`. A prim's material is
  its binding for the first of those purposes that it has; the empty token is
  `allPurpose`, and the default is `full`, then `allPurpose`. Changing the
  purposes rebuilds the scene. Like the colour management, the purposes are
  process-wide. Render stats report the prim as `activeRenderSettings`.
- **Batch.** `prism_hydra_render` and the CLI resolve the settings with
  `UsdRenderComputeSpec` (`src/output/render_products.{h,cpp}`). A view is
  framed with `CameraUtilFraming`: display window = resolution, data window =
  the pixels whose centres lie inside `dataWindowNDC` (NDC y up, pixel rows
  down), pixel aspect ratio, and the conform policy as CameraUtil's window
  policy. A cropped image holds only its data window. The EXR keeps that
  window inside the display window.
- **Products.** Each `RenderProduct` of type `raster` is written to its
  `productName`, relative to `--output-dir` (CLI) or the output directory
  (`prism_hydra_render`). Its `orderedVars` become layers:

  | RenderVar `sourceName` (`sourceType`) | AOV | Channels |
  | --- | --- | --- |
  | `color`, `Ci`, `C` (`raw`); `C.*`, `C.*[<L.>O]` (`lpe`) | colour | the first colour var: `R G B` (`A` for `color4f`), unprefixed; later ones `<var>.R` ... |
  | `depth`, `cameraDepth` (`raw`) | NDC depth, camera distance | `<var>.Z` |
  | `normal`, `Neye`, `Peye` (`raw`) | world/eye normal, eye position | `<var>.X/Y/Z` |
  | `primvars:st` (`raw`), `st` (`primvar`) | hit texture coordinates | `<var>.U/V` |
  | `primId`, `instanceId`, `elementId` (`raw`) | Hydra ids (not in the CLI) | `<var>.id` |

  `<var>` is the RenderVar prim's name. Geometric AOVs come from the
  pixel-centre hit in both frontends, so the two give the same values. The
  CLI renders RGB only, so its alpha is 1, as the delegate's is with the dome
  visible to the camera. A var with another source is skipped with a warning,
  as is a product with no supported var. Products that differ only in their
  vars share one render.
- **Outputs** (`src/output/image_writer.{h,cpp}`, shared by both frontends).
  An EXR holds every layer as float channels. It is linear in the working
  space and tagged (`oiio:ColorSpace`, chromaticities). An 8-bit file (PNG,
  JPEG, ...) holds the colour layer only, through the display and view once.
  The layers it drops are reported.
- **The AOV table** (`src/output/aov.h`) lists the AOVs, their channels and
  how geometric ones resolve from a hit, for the delegate's render buffers and
  both writers. Phase 6 adds AOVs there.

```powershell
# Every product of the stage's render settings, into build/products:
./build/RelWithDebInfo/PrismRT.exe --scene tests/hydra/fixtures/render_products.usda --output-dir build/products
./build/hydra/plugin/prism_hydra_render.exe tests/hydra/fixtures/render_products.usda `
  --render-settings default build/products-hydra [--spp N] [options]
```

Without RenderSettings, both frontends behave as before. With a settings prim
but `--output` or a camera option, the CLI writes one colour image as before,
taking the prim's resolution, camera, data window, pixel aspect ratio and
`prism:*` settings unless overridden. `prism_hydra_render scene /Camera out
...` does the same with `--render-settings PATH`.

## Standalone comparisons

The no-argument executable displays help. Rendering uses scene files and a camera: a USD camera path, an explicit camera position/target, or the USD stage's render settings ([Render settings and products](#render-settings-and-products)); glTF/GLB and OBJ use an explicit camera position/target (glTF materials: [gltf.md](gltf.md)):

```powershell
./build/RelWithDebInfo/PrismRT.exe `
  --scene tests/hydra/fixtures/parity.usda --camera /World/Camera `
  --env tests/hydra/fixtures/environment.ppm --env-exposure 0 `
  --width 128 --height 128 --spp 256 --seed 0 `
  --max-depth 32 --rr-min-depth 4 --max-sample-radiance 10 `
  --output build/standalone.exr
```

EXR output preserves linear HDR values in the working space after the configured sample clamp. Other output formats go through the display and view ([colour management](color_management.md)); compare Hydra's linear AOV with the CLI's EXR. The standalone environment retains its original factor of two; an otherwise identical Hydra dome therefore needs intensity `2` for comparison with `--env-exposure 0`.

`tests/hydra/validate.py` independently ingests the same fixture through standalone USD ingestion and the real Hydra plugin, renders twice, checks exact deterministic reruns, and compares linear EXRs. Its acceptance thresholds are normalized RGB RMSE and relative mean luminance error both at most 1%. It also compares an equivalent PreviewSurface fixture. Logs, EXRs, and timings are written to `build/hydra-validation-<filtering>`. `--device cpu` (as CTest runs it) renders both frontends on the CPU device. The [parity harness](#cli-and-hydra-parity) runs the same comparison for many fixtures.

Multisampled Hydra-buffer tests validate allocation, rendering, `Resolve()` and mapped pixels, but do not validate presentation through usdview or another host GL compositor.

The usdview interaction script is `tests/hydra/usdview_smoke.py`, for the rebuilt USD's `testusdview --testScript` runner. It exercises picking, selection outlines and thickness, filled highlighting, visibility edits, AOV switching, resizing, and renderer teardown/recreation, saving screenshots for inspection. Scripts being present does not itself establish acceptance: consult the generated logs and images from an actual run.

## CLI and Hydra parity

PrismRT has two USD loaders: the CLI's (`src/mesh/usd_mesh.h`,
`src/standalone_cli.cpp`) and the delegate's scene index bridge. CTest keeps
them in agreement. `prism_add_parity_fixture` (`cmake/PrismParity.cmake`)
renders a fixture with both on the CPU device, then compares the results
with `prism_compare_images`: normalized RMSE and relative mean luminance,
each within the fixture's tolerance.

To add a fixture, add one call to the list in `CMakeLists.txt` (after the
colour management tests):

```cmake
prism_add_parity_fixture(NAME my_feature SCENE tests/hydra/fixtures/my_feature.usda
    [CAMERA /World/Camera] [WIDTH 64] [HEIGHT 64] [SPP 16]
    [TOLERANCE 0.01] [MEAN_TOLERANCE 0.01]
    [ARGS <options of both>] [CLI_ARGS ...] [HYDRA_ARGS ...]
    [PRODUCTS <product files>...] [EXACT_LAYERS <file>:<layer>...])
```

This creates `prism_parity_my_feature_{cli,hydra}` and the comparison
`prism_parity_my_feature_match`. With `PRODUCTS`, both write the stage's
RenderProducts and each file gets a `..._<file>_<ext>_match`;
`EXACT_LAYERS` also compares geometric layers exactly. Outputs go to
`build/parity/<name>/{cli,hydra}`. Both frontends seed pixels the same way,
so most fixtures agree to about 1e-8. A tolerance above 1% needs a reason
next to the call: `lights` samples its lights in another order, which adds
noise but no bias.

| Fixture | NRMSE | Tolerance |
| --- | --- | --- |
| parity, cutout_clear, cutout_medium, sss_furnace, preview_mtlx, quad_test, color (both spaces), binding (both), render_products, render_crop | below 1e-7 | 1% |
| cornell_box | 1.4e-3 | 1% |
| in_one_weekend | 6.2e-5 | 1% |
| lights | 0.068 (mean 0.2%) | 10% (mean 1%) |
| dome_color_temperature, pointinstancer_invisible_ids, display_color_interpolation | 0 | 1% |
| light_camera_visibility | 2.0e-3 | 1% |
| light_lobes (full, diffuse, specular) | 0.012, 0.005, 0.014 (mean below 1e-4) | 5% (mean 1%): lights in another order |
| double_sided | 0.016 (mean 0.25%) | 3% (mean 1%): the mirrored card's far side (see the call) |

The CLI does not load these fixtures like Hydra yet (phase 7):

- `emissive.usda`: a MaterialX graph the CLI cannot generate;
- `instances.usda`: native instancing;
- `linking.usda`: light linking;
- `native_instance_bindings.usda`;
- `instance_primvars.usda`: per-instance primvars and MDL `geompropvalue`.

`tests/hydra/validate.py` and `validate_exports.py` also run under CTest, on
the CPU device (`--device cpu`), as `prism_hydra_validate_script` and
`prism_hydra_validate_exports_script`. They need no display or GPU.

## Scene features

UsdLux and geometry details both loaders read (the CLI as noted), with a
parity fixture each and a check in `prism_scene_features_test`:

- **Light camera visibility.** A light is seen by camera rays when it has
  Hydra's `light:cameraVisibility`, which UsdImaging reads from a bool
  `cameraVisibility` attribute on the light prim (not `inputs:`; the CLI reads
  the same attribute). Default false, as before; a visible light covers its
  pixels (alpha 1). Domes follow `domeLightCameraVisibility`. Fixture
  `light_camera_visibility.usda`.
- **Dome colour temperature.** `inputs:enableColorTemperature` and
  `inputs:colorTemperature` tint domes like the other lights (blackbody in linear
  Rec.709, converted to the working space). Fixture `dome_color_temperature.usda`.
- **PointInstancer `invisibleIds`** (and `inactiveIds`): hidden. The delegate takes
  the instancer topology mask through `HdInstancerTopologySchema::ComputeInstanceIndicesForProto`,
  which compacts the instance ordinals; `instanceId` and picking keep Hydra's
  compacted numbering, which `HdxPickTask` and the selection observer resolve
  against (numbering by authored slot would select the wrong instances). The
  CLI applies `ComputeMaskAtTime`. Fixture `pointinstancer_invisible_ids.usda`.
- **`displayColor` / `displayOpacity`** of every interpolation (constant, uniform,
  vertex, varying, faceVarying, and indexed) drive the fallback material:
  constant values are material constants, the others mesh primvars read at
  the hit (CPU `FallbackMaterial`; GPU `gpu_fallback_*` through the primvar
  directory's `kPrimvarDisplay*` roles). A primvar whose size does not fit its
  interpolation uses its first value. The CLI expands meshes with per-face
  values and now also honours `displayOpacity`. Fixture `display_color_interpolation.usda`.
- **Light `inputs:diffuse` / `inputs:specular`** (analytic lights and domes): the
  light as diffuse and as glossy BSDF lobes see it. Next-event estimation
  weighs the BSDF's diffuse part (MDL's `bsdf_diffuse`; legacy diffuse
  materials are all diffuse) by `diffuse` and the rest by `specular`; a
  BSDF-sampled hit on a light or dome scales by the diffuse fraction of the
  BSDF in the sampled direction (one more BSDF evaluation per bounce), so both
  strategies stay unbiased and the MIS densities are unchanged. Camera rays
  see lights unscaled; media scattering counts as diffuse. MDL reports its
  multiscatter compensation of glossy lobes in `bsdf_diffuse` (about 1% of a
  metal). The integrator does this only when some light or dome sets a
  multiplier other than 1 (`kFrameLightLobes`); otherwise images are
  bit-identical to before. Fixtures `light_lobes.usda` and its `_diffuse` /
  `_specular` variants: they add up to the full image, and MIS and BSDF-only
  sampling agree per lobe.
- **`doubleSided`** behind `prism:honorDoubleSided` (off by default). With it, a
  back-face hit (the integrator's `front_face`: the transformed winding, so a
  mirroring transform turns a mesh around) on a single-sided triangle is
  skipped, unless the triangle's material transmits (legacy glass; MDL
  materials whose BSDF shows a transmission event when probed at load).
  Camera, BSDF and shadow rays and the geometric AOVs see the same surfaces.
  CPU: the device's ray casts continue past culled back faces
  (`SceneManager::castRayCulled`; `cpu-reference` does not cull); GPU: a cull bit
  in each primitive's material-table entry and non-opaque instances, tested
  in the ray query (`gpu_culled_triangle`). Fixture `double_sided.usda`.
- **Per-instance primvars** (the delegate only): instance-rate primvars of
  PointInstancers and the inherited constant primvars of native instances
  answer MDL `geompropvalue` lookups, before the mesh's own primvars; nested
  instancers contribute outer to inner. CPU: `InstancePrimvars` per placement;
  GPU: `GpuInstance.links.z/w` index entries of the primvar directory.
  Fixture `instance_primvars.usda` (not a parity fixture: the CLI has no
  instancer primvars or general MDL primvars yet).

## Scope and costs

### Interactive display and noisy HDR lighting

Camera changes immediately trace and display a coarse one-sample preview for the current camera. By default, the preview is capped at 96 pixels on its longest edge and enlarged to the viewport, keeping ray work bounded as the viewport grows. It updates during continuous dragging, even when mouse events arrive faster than a full-resolution pass can finish, and is intentionally noisy and pixelated. By default, full-resolution accumulation starts after a 100 ms quiet interval, avoiding repeated setup and cancellation between mouse events. Preview samples use separate random-number state and never enter final accumulation; once navigation stops, complete full-resolution passes replace the preview and refine normally. Resizing requires allocating new buffers. Edits of lights alone wait while the camera moves (for up to half the refinement delay, at most 50 ms, after the last camera change): a host light that follows the camera, such as usdview's default camera light, changes with every camera move, and converting and uploading each of those scenes restarted the render before any preview could finish. Previews keep the displayed lighting until the camera rests; the latest lighting is then captured once, and its refinement counts the quiet interval from the last camera change. `deferredEnvironmentCaptures` counts the Prepare calls that held such edits.

`tests/hydra/usdview_motion.py` tests held-button trucking and orbiting through usdview's mouse-event handlers, checks that the rendered image changes on every input event without rebuilding geometry, and verifies refinement after release. It records screenshots and frame timings in `build/hydra-motion` (or `PRISM_VALIDATION_DIR`). The controlled parity fixture also checks that the panel moves in the expected screen direction, so changing noise or GL overlays cannot substitute for actual camera feedback. Run it with the installed USD runtime and `testusdview --testScript`, using either `tests/hydra/fixtures/parity.usda` or the OpenChessSet stage.

During full-resolution refinement, the Float32 RGBA viewport can publish
immutable 32-by-32 tile updates while a pass is still running. Interleaved
tile order spreads refinement across the image. The schedule is cached by
resolution, and one pass-wide TBB job reports completed tiles to a publication
coordinator at approximately 100 ms intervals. Presentation payloads are built
only for completed tiles needed by an intermediate publication. A pass that
finishes before publication is due publishes its exact image directly. These
updates affect only presentation: raw AOV mapping, saved render pixels,
accumulation state, and random-number sequences remain tied to complete
passes. A canceled or replaced frame discards its partial display image, and
deterministic geometric AOVs continue to come from the committed full render.
Deterministic geometric staging retains one typed output plane per distinct
supported bound AOV. Color-only bindings allocate no geometric scratch;
depth alone or an individual ID AOV uses four bytes per pixel. Duplicate
bindings share staging, and null/unsupported bindings add no scratch.

When deduplicated bindings contain supported geometric outputs and no color
destination, the delegate renders one deterministic center-ray pass. It skips
color preview/path tracing, color-pass scratch, persistent color RNG state and
the refinement delay. Geometric outputs use the same resolution, coordinate
systems, miss values and typed integer storage as mixed color/geometric
renders. They converge after publication regardless of the color sample
budget; changing only that budget does not repeat a completed deterministic
pass. Camera, scene, allocation and binding changes invalidate the outputs,
and adding color starts fresh color accumulation with the usual seed rules.
Unsupported-only requests warn and publish their clear values without rays.
Cancellation leaves the previous complete AOV (or initial clear) visible and
does not publish partially resolved geometric data.

`colorTracedPixels` counts color-ray calls, including preview and canceled
work; `geometricOnlyPixels` counts resolved center rays in non-color requests.
Both aggregate once per task. `colorPassScratchBytes` and
`colorRngStorageBytes` describe storage at the last render invocation; both
are zero for geometric-only requests.

The native presentation task caches a pristine GPU source and skips unchanged
CPU uploads. Completed display tiles are packed into atlases of at most
1024-by-1024 pixels and drawn in batches. Complete images reuse the GPU source
allocation when dimensions match; selection, background compositing, and color correction use separate
working targets. CPU callers can still request a full immutable presentation,
which is materialized lazily. Complete passes replace partial estimates with
the exact accumulated image. Integer and other non-RGBA display formats retain
the stock OpenUSD upload path.

Pass profiling separates ray work and geometric-AOV resolution (summed worker
times, `lastPassRayWorkerMilliseconds` and `lastPassAovWorkerMilliseconds`)
from coordinator waiting, payload construction, and intermediate publication
(`lastPassWaitMilliseconds`, `lastPassPayloadMilliseconds`, and
`lastPassPublicationMilliseconds`). Worker timings are aggregated per task,
without per-pixel atomics. `lastPassTraceMilliseconds` is the enclosing wall
time, and `lastPassCommitMilliseconds` measures final buffer commit. Worker
times overlap each other and coordinator waiting; they must not be added to
wall timings as though they were sequential phases. Cancellation drains the
pass job before releasing its staging or scene references.

Render-device creation and per-scene device preparation (GPU kernel
compilation, which can take tens of seconds with a cold DXC cache, scene and
texture upload, and CPU light tables) run as background jobs that the render
worker waits on while polling for a stop. Hydra's Execute stops the worker on
camera, binding and scene changes from the host's UI thread, so it never waits
for this work: an interrupted job keeps running, and the next render resumes
waiting on it. Until the device is ready the viewport keeps showing its last
image (or the initial clear). Device creation also runs outside the bridge
mutex that Execute and `GetRenderStats` take on the UI thread. Statistics
report `devicePreparing`, `devicePreparations`, `devicePreparationsInterrupted`
and `devicePreparationSeconds`. With `PRISM_GPU_VERBOSE=1`, GPU devices log
each pass's wall, dispatch, GPU and readback times; per-pass results are read
back from host-cached memory where the Vulkan driver offers it.

### Progress and the usdview HUD

While a scene cannot render yet (and while an edit is converted), the renderer reports what it is doing through `GetRenderStats()`. The model is `prism::Progress` (`src/progress.h`): one slot per phase with atomic active/done/total counters and start time, plus a few strings (phase detail, device, error) behind the model's own small mutex. It never takes the bridge mutex, and `SceneBridge::GetProgressStats()` returns the keys below without taking any renderer lock (`GetStats()` merges them before it takes the bridge mutex). Instrumented code reports through one-line hooks (`prism::progress::Add(Phase::Materials)` and so on) that ignore the call when no model exists, such as in the standalone renderer; the bridge installs its model as the process's current one. Updates are atomic increments with no per-item allocation. Phases overlap (textures prepare while materials compile; on Vulkan, material callables compile and images upload while the materials are still compiling; the kernel compile runs inside the GPU upload), so `prismPhase` names the primary one, in the order materials, textures, kernel, upload, loading, device creation, with `rendering` when nothing else runs and `error` over all of them: while materials compile the scene waits on them, whatever GPU work overlaps. Overlapping holders of one phase (`progress::Scope`, `Progress::Hold`) keep it active until the last ends.

| Statistic | Meaning |
| --- | --- |
| `prismPhase` (string) | `idle`, `device`, `loading`, `materials`, `textures`, `kernel`, `upload`, `rendering` or `error` |
| `prismPhaseDetail` (string) | Detail of the primary phase, e.g. `DXC to SPIR-V, 5 MDL programs, 526 KB HLSL`, or `images` while uploading |
| `prismPhaseSeconds` (double) | Seconds the primary phase has been running |
| `prismActivePhases` (string) | Comma-separated names of every active phase |
| `prismMeshesDone` / `prismMeshesTotal` (int) | Mesh prototypes converted in this load (cache hits are not counted) |
| `prismMaterialsDone` / `prismMaterialsTotal` (int) | Materials compiled; the total grows when template seeds or fallback compiles are added |
| `prismTexturesDone` / `prismTexturesTotal` (int) | Textures prepared ahead of material compilation |
| `prismUploadDone` / `prismUploadTotal` (int) | Images prepared for the GPU (zero total outside the image stage; the stage is in `prismPhaseDetail`) |
| `prismDevice` (string) | `vulkan: <GPU name>`, `cpu`, `cpu-reference`, or `cpu (Vulkan fallback: <reason>)` when the GPU device was unavailable at creation or later could not prepare the scene (`kernel too large` only without ray tracing pipelines: otherwise large material sets compile as callable shaders, see [gpu_devices.md](gpu_devices.md#material-programs-as-callable-shaders-vulkan); `kernel compile failed`, ...); empty until the device exists |
| `prismGpuPreview` (bool) | True while the CPU renders a preview for a GPU device that is preparing; `prismDevice` is then `cpu (preview while the GPU prepares)` and `prismPhase` keeps naming the GPU phase |
| `prismError` (string) | Last scene-conversion failure; cleared by the next published snapshot |

Counters stay readable after their phase ends and restart when it begins again. Metal reports the device, loading, material and texture phases but not yet the kernel and upload stages.

The usdview extension turns these into HUD lines under usdview's Hydra/Hgi lines (colors: tan like usdview's own text, blue for work in progress, orange for a GPU fallback, green for converged):

| Phase | HUD |
| --- | --- |
| any | ` Device: vulkan (RTX 4070 Ti)`, or ` Device: cpu (Vulkan fallback: kernel too large)` in the warning color |
| `idle` | ` Status: Preparing...` |
| `loading` | ` Status: Loading scene (meshes 3 / 10)` |
| `materials` | ` Status: Compiling materials 12 / 35`, plus an indented ` textures 4 / 20` line while those still run |
| `textures` | ` Status: Preparing textures 4 / 20` |
| `kernel` | ` Status: Compiling GPU kernel... 42 s` and an indented detail line; the seconds appear from the first second (3 s for other phases) |
| `upload` | ` Status: Uploading to GPU (images) 5 / 20`, `(buffers)`, `(acceleration structures)` |
| `rendering` | ` Samples: N / target` and ` State: refining` or `converged`; with GPU texture streaming first `Texture: resident / budget MiB` (`, N pending` while levels load, `, N below requested` in the warning color when the budget is short) |
| any, with `prismGpuPreview` | ` Device: cpu (preview while the GPU prepares)`, the GPU phase line (` Status: Compiling GPU kernel... 12 s`) and ` Samples: N / target` counting up from the CPU |
| `error` | ` Status: Error: ...` in the warning color |

The formatting is in the pure functions `device_line`, `phase_lines` and `hud_lines` (unit tests in `tests/hydra/test_usdview_selection_plugin.py`). `tests/hydra/usdview_sample_hud.py` also checks that the device line and, for a slow start, a phase line appear before the first sample. `tests/hydra/usdview_phase_hud.py` (run it with `testusdview --testScript` and a fresh `PRISM_CACHE_DIR` to force cold compiles) saves a screenshot of the viewport the first time each phase shows, plus a timeline of the statistics and overlay text. Forcing a fallback needs only `PRISM_DEVICE=vulkan PRISM_DXC=<nonexistent path>`.

### CPU preview while the GPU prepares

A cold GPU device spends tens of seconds compiling its kernel and uploading the scene. If that preparation is still running one second after it started (`PRISM_GPU_PREPARATION_PREVIEW_DELAY_MS` changes the delay), the render worker renders on the CPU device instead, with the usual coarse preview, the refinement delay and full-resolution passes. No second scene exists: the CPU device renders the snapshot's own Embree scene and native MDL materials, which every device already needs (picking and geometric AOVs use them too), plus a per-scene light table. The preview is skipped for scenes whose Embree memory exceeds `prism:gpuPreparationPreviewMaxMiB`.

The CPU passes use an eighth of the hardware threads (at least two; `PRISM_GPU_PREPARATION_PREVIEW_THREADS` overrides) so the shader and driver compilers keep the machine: on 28 threads a cold 35 s Vulkan preparation of the chess set took 38 s without the preview, 31-41 s with 2 to 4 preview threads, 45-58 s with 7 and 56-69 s with 14. The first image appears after 6 s instead of 38-62 s. At each pass boundary the worker checks whether the preparation finished; if so, the next pass runs on the GPU **with the same sample index, per-pixel RNG state and accumulated average**, so accumulation continues instead of restarting and the displayed quality never goes backwards (`gpuPreviewHandovers`, `gpuPreviewHandoverSample` and `gpuPreviewPasses` count handovers, the sample where the GPU took over and the passes the CPU rendered). Camera moves and edits during the preparation keep rendering on the CPU; a preparation that a scene edit supersedes finishes first (it cannot be cancelled) and the next one starts then. A GPU device that cannot prepare the scene (`cpu (Vulkan fallback: ...)`) still hands over, to its own CPU device. `PRISM_TEST_GPU_PREPARATION_DELAY_MS` delays the preparation job for tests, and `PRISM_TEST_GPU_PREVIEW_MAX_SAMPLES=<n>` holds the CPU preview at the pass boundary after n samples until the GPU is ready, so a test sees the handover however long the GPU takes to prepare.

usdview's **default dome** is OpenUSD's Stinson Beach environment, not PrismRT's bundled indoor EXR. It uses intensity 1 and exposure 0. Hydra respects these authored values without the standalone environment's historical extra factor of two. To reproduce standalone lighting, use the same HDR and account for that factor (for example, standalone exposure -0.5 corresponds to a USD dome exposure +0.5 at intensity 1).

The `prismUsdview` extension turns usdview's Lights > "Enable Default Dome Light" on at start-up: a stage without lights is otherwise lit only by usdview's camera light and looks nearly black. usdview saves the toggle in its state file, so the extension sets it at each start-up only and the menu entry still turns it off for the session. `PRISM_USDVIEW_DEFAULT_DOME_LIGHT=off` starts with it off and `saved` keeps usdview's saved state (the package: `PRISMRT_USDVIEW_DEFAULT_DOME_LIGHT`). usdview passes the toggle as a dome-light prim of its engine's task controller (`/<delegate>/_UsdImaging_<renderer>_<engine>/lights/light_<n>`), not a render setting; the camera light is a distant light of the same controller. Storm adds the default dome to the stage's own lights. PrismRT uses it only when the stage has no lights of its own (no dome, sphere, rect, disk, cylinder or distant light, and no geometry with constant emission; usdview's camera light does not count), so a lit stage does not get brighter or show a second dome when the toggle is on (`tests/hydra/coverage_integration_test.cpp`; `tests/hydra/fixtures/unlit.usda` is a stage without lights). The dome texture is a Rec.709 HDR like any dome texture and is converted to the working space.

The standalone CLI follows the same rule: a scene (USD, glTF or OBJ) with no lights, no constant-emission geometry and no `--env`/`--env-color` is lit by the same Stinson Beach dome, as a USD dome at intensity 1 (without `--env`'s factor of two), and the CLI says so on stderr. `--default-dome false` keeps such a scene black. Textured emitters are not detected yet, so a scene lit only by them also gets the dome.

The beach HDR contains a small, extremely bright sun. Environment MIS is now the default in both frontends: one importance-sampled environment direction competes with BSDF sampling at supported scattering vertices. The distribution uses exact texel solid angles and a small uniform mixture to cover filtered texture values. Diffuse and MDL materials supply matching BSDF evaluations/PDFs; legacy rough-metal and glTF samplers retain their BSDF-only behavior. The default sample clamp of 10 can still reduce brightness: use zero when measuring energy or variance. "Converged" means adaptive sampling stopped every pixel, the samples per pixel were reached, or the time limit passed, whichever came first.

Scene inputs are captured on the owning thread in incremental slices, then prepared in background jobs. The previous fully shaded snapshot remains navigable while the next generation prepares. Publication briefly stops rendering and swaps complete snapshots; superseded jobs cannot publish. Statistics expose pending generations and whether the displayed scene predates edits. The scene-index chain resolves CPU Hydra ExtComputation primvars before the transport filter; this includes computed point arrays used by stock USD Skel skinning.

Local-space mesh prototypes are shared by Embree instances with composed placement transforms; nested USD instances do not require deep Embree instance stacks. Two top-level scene versions replay outstanding changes only while inactive. Transform and visibility edits update dependent placements; material and dome updates retain geometry. Known geometry edits replace affected prototypes; unknown locators conservatively rebuild with a recorded reason. Hidden and purpose-filtered prototypes, including zero-instance prototypes, retain their geometry and material dependencies. Visibility, purpose and render-tag edits change placement eligibility without geometry conversion; empty render tags continue to include geometry and render purposes. Initial hidden batch-eligible spheres use ordinary disabled placements; changing visible batch membership records a named conservative fallback. Camera-only changes reuse geometry. `enabledPlacementCount` counts enabled ordinary instances (sphere batch entries have separate counts); `topLevelSceneBuilds` counts actual top-level scene allocations, including spare and canceled work. Subdivision meshes use their polygon control hull. Spheres remain analytic; other supported implicit shapes use USD's mesh-conversion filter.

Disabled placements retain geometry and instance metadata by default. Setting
`prism:hiddenGeometryBudgetMiB` to a positive integer enables least-recently-used
eviction of hidden, purpose-filtered, and zero-instance prototype geometry.
The budget counts unique converted mesh arrays and analytic sphere payloads,
deduplicated across snapshot banks; it excludes captured source data, materials,
textures, and Embree acceleration structures. It is neither a process-memory
cap nor a peak-loading limit. Negative or non-integer values warn and preserve
the previous setting.

Eviction occurs only during safe bank preparation or inactive-bank maintenance.
Published frames remain immutable, and pinned geometry can keep usage above
the target. A budget-only change does not restart accumulation; idle displayed
snapshots may continue retaining geometry. Showing an evicted prototype restores
it from the latest captured geometry and material data before publication.
Increasing or disabling the budget does not eagerly restore cold geometry.

Experimental sphere batching accepts uninstanced spheres with translation and positive uniform scale. Rotated, mirrored, nonuniformly scaled, and instanced spheres retain ordinary instances to preserve local UV frames and picking identity. Affected batches are bounded at 1,024 entries; eligibility changes can trigger a conservative rebuild. Keep batching disabled unless measurements on the intended scene justify it.

Curves, volumes/SSS transport, displacement, subdivision refinement, motion blur, analytic USD lights, arbitrary shader primvars, and multiple simultaneous UV sets are outside this implementation's initial scope. Environment sampling is shared with standalone rendering. Emissive surfaces retain BSDF-path emission; emissive-mesh and analytic-light sampling are deferred. See [material support](hydra_materials.md) for network conversion, asset resolution, opacity, caching, and diagnostics.


## Procedural scene export

The procedural constructors remain available for export and regression tests. Export operates on their instantiated data, preserving stored float RGB values and authored roughness without extra random draws:

```powershell
./build/RelWithDebInfo/PrismRT.exe --export-scene InOneWeekend --seed 0 --output scene.usda
./build/RelWithDebInfo/PrismRT.exe --scene scene.usda --camera /World/Camera --spp 256 --output scene.exr
```

`CornellBox` and `QuadTest` are also available. Exports contain inline OpenPBR MaterialX networks, USD spheres, polygon quads/boxes, camera/lens settings and render resolution. Gradient skies use a relative linear EXR companion. Keep that companion beside its USDA. Both ingestion paths render USD spheres analytically using Embree sphere-point geometry. The old rough-metal sampler and OpenPBR metal differ even when their numeric roughness is identical; exported legacy scenes are not an assertion of identical BSDF appearance.

Sphere prototypes retain local-space geometry and use affine instances for placement, including nonuniform and mirrored transforms. Their silhouettes and normals are independent of usdview's complexity setting. They support prim/instance picking; face IDs are `-1` because implicit spheres have no polygon faces. Export tests compare authored radii and bounds, stored colors, camera rays, and baked sky against the procedural source. Meshes without authored normals use smooth area-weighted per-point normals, matching the standalone USD importer; authored normals retain their interpolation.

USD dome lighting is read when `--env` is absent. Explicit `--env` overrides it and retains the historical factor-of-two convention. For formats without cameras, supply `--camera-position x,y,z --camera-target x,y,z`, with optional `--camera-up`, `--vfov`, `--focus-distance`, and `--defocus-angle`. Asset-specific local launchers belong in ignored `local/`; tracked configuration templates contain no personal paths. See [deployment](deployment.md).

## Loading measurements

Set `PRISM_LOAD_PROFILE=1` to print a loading timeline to stderr: one
`[load] t=<seconds since process start> span <phase> start=... wall=... cpu=... par=...`
line per phase (USD stage open and engine creation in `prism_hydra_render`,
capture, the snapshot build and its material, mesh, walk and Embree phases,
each material conversion and compilation with its sub-phase milliseconds,
texture preparation, render-device creation and preparation, Vulkan shader
and scene setup) and `mark` lines for events (scene notices with their dirty
kinds, publication or discard of a generation, first preview and first full
sample). `cpu` is process CPU time during the phase, so `par` is its average
parallelism including whatever ran concurrently. Disabled, it costs a flag
test per phase.

Material conversion (hdMtlx) runs on the material workers, each converting an
item and then compiling it, largest graphs first; conversions share USD's
hdMtlx standard library under a reader lock, and a network whose node
definitions hdMtlx would add to that library converts alone. Meshes convert in
a TBB task while the materials compile; a mesh whose bound network has a
connected displacement terminal waits for its material, and the refinement
budget planned before the materials resolved must hold afterwards, or the
batch's meshes convert again (counted by `speculativeMeshRetries`). UDIM tiles
are read, hashed and published in parallel.

`GetRenderStats()` exposes material conversion/generation/compilation counters and timings, worker CPU time separately from batch elapsed time, prototype/placement rebuild counters, Embree timings, and buffer publication/mapping timing. Set `PRISM_PROFILE_BUFFERS=1` to measure worker buffer-write time; workers accumulate timings locally and update each buffer once per tile, while ordinary rendering leaves this instrumentation disabled. Material compilation concurrency is configured with `PRISM_MATERIAL_THREADS`; the default is half the hardware threads capped at eight, with a minimum of one (14 workers measured no faster than 8 on a 28-thread machine: the native JITs slow down in proportion). `PRISM_MDL_VERBOSE=1` enables compiler diagnostics that would otherwise obscure loading measurements.

Supported prototype edits repoint dependent instances while retaining the top-level scene. Placement identity includes every instancer path, authored source index and duplicate occurrence; source-order changes refresh picking ordinals without replacing unchanged instances. Unsupported geometry changes and sphere batch membership use named conservative fallbacks. `lastPlacementRecordsVisited`, `lastPlacementTransformsUpdated`, `lastVisibilityPlacementsUpdated`, `lastPrototypePathsUpdated` and `lastPrototypePlacementsRepointed` distinguish visited records from changed work across each bank's outstanding journal. `topLevelSceneCommits` counts commits separately from scene allocations. Dependency lookup limits geometry updates; picking metadata, geometry ownership cleanup and cached-count refresh may still scan retained scene data.

Typed structural transactions support meshes, ordinary spheres, subsets, containers and instancers, including geometry inside existing prototypes. Added descendants are captured recursively; removed descendants come from retained bank records. Transactions reconcile inherited transforms, eligibility and material bindings while preserving unrelated geometry and the top-level scene. Focused matrices cover 16, 128, 1,024 and 16,384 placements and compare images with fresh rebuilds.

Explicit Hydra rename notices retain their old/new path pair. Compatible prototype storage and placement handles are rekeyed; canonical IDs follow the destination path. Unaffected paths retain their IDs, removed source slots remain tombstones, and renaming back revives the original destination-path slot. Published frames retain their original paths and picking tables. Hosts that report a namespace edit as separate removal/addition notices use those transactions; the delegate does not infer a rename pair.

Unknown operations, unsupported primitive transitions, invalid rename pairs or conflicting destinations, sphere-batch membership changes, and unavailable exclusive banks retain conservative fallbacks. Preflight runs before bank mutation; a canceled or failed transaction is discarded without publishing a partially reconciled scene. `primCount` counts live identities; `primPathSlotCount` includes tombstones. `structuralLeafUpdates` (the retained diagnostic name), `structuralFallbacks` and the `lastStructuralPathsUpdated`, `lastStructuralPlacementsAttached` and `lastStructuralPlacementsDetached` gauges expose transaction work. `structuralPlacementDetachCalls` includes discarded work. Structural bookkeeping can still scan retained identity and ownership data; these transactions are not claimed to be constant-time.

Transform-only bank updates reuse the immutable canonical-ID registry and picking
table. They skip geometry-count and input-topology refresh. Attachment/detachment
updates placement dependency memberships individually; ordinal/radix changes
refresh picking records without rewriting unchanged memberships. Input metadata
refresh is limited to affected prototypes and picking reconstruction to affected
prim records. Topology changes still copy the published picking table before
replacing affected records; this copy is not constant-time.

`lastCanonicalRecordsVisited`, `lastPickingRecordsVisited`,
`lastCountRecordsVisited`, `lastInputMetadataPrototypesVisited` and
`lastDependencyPlacementsVisited` expose this work. Corresponding timing scopes
are `lastCanonicalMilliseconds`, `lastPickingMilliseconds`,
`lastCountsMilliseconds`, `lastInputMetadataMilliseconds` and
`lastDependencyMilliseconds`. `lastInputComparisonMilliseconds` measures captured
array comparisons; `lastPlacementReconcileMilliseconds` encloses the entire
placement update, including its subscopes. Counters describe work needed to bring
the selected bank current, including replay of earlier pending changes.

Instancer input caches compare changed arrays and map changed source indices to
their dependent placements, including duplicate indices and nested instancers.
`lastInstanceArrayElementsScanned` measures input comparison separately from
`lastPlacementRecordsVisited` and `lastPlacementTransformsUpdated`. Whole-array
Hydra notices can still require linear scanning. Count and source-key changes
attach/detach placements in the existing inactive top-level scene; the
`lastPlacementTopologyAttached` and `lastPlacementTopologyDetached` counters
record the deltas. Subset additions/removals refresh the existing parent mesh's
material binding metadata without converting unrelated geometry. Ancestor and
prototype-root restructuring and sphere-batch membership retain named fallbacks.

Presentation statistics report `presentationUploadBytes`,
`presentationUploadCalls`, `presentationSkippedUploads`,
`presentationAtlasBytes`, `presentationAtlasDraws`, and
`presentationCopyBytes`. Upload counters describe the optimized Float32 RGBA
path and its Float32 depth input; stock fallback uploads are not counted.

Completed Hydra improvements and their test evidence are tracked in [the delegate validation ledger](hydra_delegate_validation.md), including the latest integration changes and historical plan results.

`tests/hydra/benchmark_loading.py` runs stock `testusdview` in fresh processes at one, two, and four compilation workers, three runs each, and recreates the renderer in each process. It records cache labels, process-to-image time, renderer timings, peak working set, and screenshots. A fresh process clears process-local material caches; it does **not** flush the operating system's file cache. Compare cold and warm results separately.
