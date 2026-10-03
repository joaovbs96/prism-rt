# PrismRT in Hydra and usdview

`hdPrism` consumes Hydra scene-index data directly and shares the existing PrismRT path tracer and MaterialX/MDL backend with the standalone executable. It targets unmodified OpenUSD 26.05. USD's built-in render-delegate adapter owns the render index and usdview control interface; PrismRT uses the exported `HdRenderDelegate::SetTerminalSceneIndex` and `Update` hooks for scene synchronization. The adapter receives only cameras, buffers, and tasks. Geometry and materials are read from the full upstream scene index and do not have legacy `Sync` implementations.

This is a scene-index renderer behind USD's compatibility adapter, not a fully native `HdRenderer` implementation. The adapter is intentional: the 26.05 Windows DLL interface does not export all the symbols needed by an external `HdRenderer` subclass. Using USD's own adapter avoids changing USD headers, exports, or binaries. The build script does not apply USD patches.

## Build and launch

The local configuration targets the single OpenUSD source/build/install tree at `external_deps/OpenUSD`, using its 26.05 headers and ABI. Rebuild that installation with Python, imaging, GPU/GL support, and usdview before building the plugin:

```powershell
python build_script.py --usd-only
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

| Hydra setting | Default | Meaning |
| --- | --- | --- |
| `prism:samplesPerPixel` | 256 | Samples per pixel before convergence |
| `prism:maxDepth` | 32 | Maximum path depth |
| `prism:rrMinDepth` | 4 | Start of Russian roulette |
| `prism:maxSampleRadiance` | 10 | Existing hue-preserving per-sample clamp; zero disables it |
| `prism:seed` | 0 | Per-pixel RNG seed |
| `prism:previewMaxDimension` | 96 | Preview longest edge, clamped to 16-1024 pixels |
| `prism:adaptivePreview` | true | Adapt preview resolution using measured render time |
| `prism:previewTargetMs` | 33 | Preview render-time target, clamped to 1-1000 ms |
| `prism:refinementDelayMs` | 100 | Quiet interval before refinement, clamped to 0-2000 ms |
| `domeLightCameraVisibility` | true | Show dome light in primary camera rays; secondary lighting remains enabled |
| `prism:environmentSampling` | `mis` | Environment next-event sampling with MIS, or `bsdf` |
| `prism:textureFiltering` | `filtered` | Footprint-based anisotropic mip filtering, or highest-resolution `bilinear` reference mode |
| `prism:sphereBatching` | false | Experimental batches of at most 1,024 eligible analytic spheres; ordinary instancing remains the default |
| `prism:resourceRevision` | 0 | Increment to invalidate cached material and external texture fingerprints and reload dome resources without rebuilding geometry |
| `prism:selectionOutline` | true | Draw selection as an outline; false uses filled highlighting |
| `prism:selectionOutlineRadius` | 2 | Outline thickness in pixels |
| `prism:selectionUseHostSettings` | false | Use the host's Hdx outline parameters instead of PrismRT's overrides |

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

The launcher also loads PrismRT's Python usdview extension from `tools/usdview_plugins`. Stock 26.05 usdview drops the selected instance index when forwarding selection to its scene index. Our extension forwards the selection model's original indices through the internal `prism:usdviewSelection` renderer setting, preserving individual-instance outlines without patching USD. It uses usdview's plugin registration and `StageView` renderer-setting methods; it does not replace usdview functions. Other hosts can supply complete Hydra selection data directly. Without the extension, stock usdview still renders and picks correctly, but individual-instance highlighting follows its upstream whole-prim selection limitation.

The render buffers expose linear floating-point `color`, projected `depth` in `[0,1]`, `cameraDepth`, world `normal`, eye-space `Neye`, and integer `primId`, `instanceId`, and `elementId`. IDs and geometric AOVs use a deterministic center ray independent of antialiasing and depth-of-field samples. `elementId` refers to the coarse polygon face. Prim, instance, and face picking use visible primary intersections; edge, vertex, and deep picking are deferred.

Color alpha accumulates primary coverage, including stochastic cutouts and silhouette samples. With no dome, or `domeLightCameraVisibility=false`, primary rays that escape the scene produce transparent black; unresolved early terminations retain coverage. A visible dome keeps opaque alpha. Changing dome camera visibility restarts preview and full accumulation.

## Standalone comparisons

The no-argument executable displays help. Rendering uses scene files and requires an explicit camera. USD camera paths and explicit camera position/target are supported; glTF/GLB and OBJ use an explicit camera position/target:

```powershell
./build/RelWithDebInfo/PrismRT.exe `
  --scene tests/hydra/fixtures/parity.usda --camera /World/Camera `
  --env tests/hydra/fixtures/environment.ppm --env-exposure 0 `
  --width 128 --height 128 --spp 256 --seed 0 `
  --max-depth 32 --rr-min-depth 4 --max-sample-radiance 10 `
  --output build/standalone.exr
```

EXR output preserves linear HDR values after the configured sample clamp. Other output formats use the existing display clamp and gamma 2.2 conversion. The standalone environment retains its original factor of two; an otherwise identical Hydra dome therefore needs intensity `2` for comparison with `--env-exposure 0`.

`tests/hydra/validate.py` independently ingests the same fixture through standalone USD ingestion and the real Hydra plugin, renders twice, checks exact deterministic reruns, and compares linear EXRs. Its acceptance thresholds are normalized RGB RMSE and relative mean luminance error both at most 1%. It also compares an equivalent PreviewSurface fixture. Logs, EXRs, and timings are written to `build/hydra-validation`.

The usdview interaction script is `tests/hydra/usdview_smoke.py`, for the rebuilt USD's `testusdview --testScript` runner. It exercises picking, selection outlines and thickness, filled highlighting, visibility edits, AOV switching, resizing, and renderer teardown/recreation, saving screenshots for inspection. Scripts being present does not itself establish acceptance: consult the generated logs and images from an actual run.

## Scope and costs

### Interactive display and noisy HDR lighting

Camera changes immediately trace and display a coarse one-sample preview for the current camera. By default, the preview is capped at 96 pixels on its longest edge and enlarged to the viewport, keeping ray work bounded as the viewport grows. It updates during continuous dragging, even when mouse events arrive faster than a full-resolution pass can finish, and is intentionally noisy and pixelated. By default, full-resolution accumulation starts after a 100 ms quiet interval, avoiding repeated setup and cancellation between mouse events. Preview samples use separate random-number state and never enter final accumulation; once navigation stops, complete full-resolution passes replace the preview and refine normally. Resizing requires allocating new buffers.

`tests/hydra/usdview_motion.py` tests held-button trucking and orbiting through usdview's mouse-event handlers, checks that the rendered image changes on every input event without rebuilding geometry, and verifies refinement after release. It records screenshots and frame timings in `build/hydra-motion` (or `PRISM_VALIDATION_DIR`). The controlled parity fixture also checks that the panel moves in the expected screen direction, so changing noise or GL overlays cannot substitute for actual camera feedback. Run it with the installed USD runtime and `testusdview --testScript`, using either `tests/hydra/fixtures/parity.usda` or the OpenChessSet stage.

usdview's **default dome** is OpenUSD's Stinson Beach environment, not PrismRT's bundled indoor EXR. It uses intensity 1 and exposure 0. Hydra respects these authored values without the standalone environment's historical extra factor of two. To reproduce standalone lighting, use the same HDR and account for that factor (for example, standalone exposure -0.5 corresponds to a USD dome exposure +0.5 at intensity 1).

The beach HDR contains a small, extremely bright sun. Environment MIS is now the default in both frontends: one importance-sampled environment direction competes with BSDF sampling at supported scattering vertices. The distribution uses exact texel solid angles and a small uniform mixture to cover filtered texture values. Diffuse and MDL materials supply matching BSDF evaluations/PDFs; legacy rough-metal and glTF samplers retain their BSDF-only behavior. The default sample clamp of 10 can still reduce brightness: use zero when measuring energy or variance. "Converged" means the requested sample count was reached, not that a noise threshold was met.

Scene inputs are captured on the owning thread in incremental slices, then prepared in background jobs. The previous fully shaded snapshot remains navigable while the next generation prepares. Publication briefly stops rendering and swaps complete snapshots; superseded jobs cannot publish. Statistics expose pending generations and whether the displayed scene predates edits.

Local-space mesh prototypes are shared by Embree instances with composed placement transforms; nested USD instances do not require deep Embree instance stacks. Two top-level scene versions replay outstanding changes only while inactive. Transform and visibility edits update dependent placements; material and dome updates retain geometry. Known geometry edits replace affected prototypes; unknown locators conservatively rebuild with a recorded reason. Restoring visibility when prototype records are missing also takes the conservative path. Camera-only changes reuse geometry. Subdivision meshes use their polygon control hull. Spheres remain analytic; other supported implicit shapes use USD's mesh-conversion filter.

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

`GetRenderStats()` exposes material conversion/generation/compilation counters and timings, worker CPU time separately from batch elapsed time, prototype/placement rebuild counters, Embree timings, and buffer publication/mapping timing. Set `PRISM_PROFILE_BUFFERS=1` to measure worker buffer-write time; workers accumulate timings locally and update each buffer once per tile, while ordinary rendering leaves this instrumentation disabled. Material compilation concurrency is configured with `PRISM_MATERIAL_THREADS`; the default is half the hardware threads capped at four, with a minimum of one. `PRISM_MDL_VERBOSE=1` enables compiler diagnostics that would otherwise obscure loading measurements.

`tests/hydra/benchmark_loading.py` runs stock `testusdview` in fresh processes at one, two, and four compilation workers, three runs each, and recreates the renderer in each process. It records cache labels, process-to-image time, renderer timings, peak working set, and screenshots. A fresh process clears process-local material caches; it does **not** flush the operating system's file cache. Compare cold and warm results separately.
