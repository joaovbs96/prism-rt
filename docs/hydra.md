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
| `prism:selectionOutline` | true | Draw selection as an outline; false uses filled highlighting |
| `prism:selectionOutlineRadius` | 2 | Outline thickness in pixels |
| `prism:selectionUseHostSettings` | false | Use the host's Hdx outline parameters instead of PrismRT's overrides |

Selection reuses `HdxColorizeSelectionTask`, with native prim/instance/face IDs supplying its selection mask. usdview retains control of highlight color and highlight visibility. Outline changes affect presentation and do not restart accumulation. Host applications can supply `enableOutline` and `outlineRadius` through the standard Hdx task parameters when `prism:selectionUseHostSettings` is enabled.

The launcher also loads PrismRT's Python usdview extension from `tools/usdview_plugins`. Stock 26.05 usdview drops the selected instance index when forwarding selection to its scene index. Our extension forwards the selection model's original indices through the internal `prism:usdviewSelection` renderer setting, preserving individual-instance outlines without patching USD. It uses usdview's plugin registration and `StageView` renderer-setting methods; it does not replace usdview functions. Other hosts can supply complete Hydra selection data directly. Without the extension, stock usdview still renders and picks correctly, but individual-instance highlighting follows its upstream whole-prim selection limitation.

The render buffers expose linear floating-point `color`, projected `depth` in `[0,1]`, `cameraDepth`, world `normal`, eye-space `Neye`, and integer `primId`, `instanceId`, and `elementId`. IDs and geometric AOVs use a deterministic center ray independent of antialiasing and depth-of-field samples. `elementId` refers to the coarse polygon face. Prim, instance, and face picking use visible primary intersections; edge, vertex, and deep picking are deferred.

## Standalone comparisons

The no-argument executable keeps its original behavior. Scene mode requires an explicit camera:

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

Camera changes immediately trace and display a coarse one-sample preview for the current camera. The preview is capped at 96 pixels on its longest edge and enlarged to the viewport, keeping ray work bounded as the viewport grows. It updates during continuous dragging, even when mouse events arrive faster than a full-resolution pass can finish, and is intentionally noisy and pixelated. Full-resolution accumulation starts after a 100 ms quiet interval, avoiding repeated setup and cancellation between mouse events. Preview samples use separate random-number state and never enter final accumulation; once navigation stops, complete full-resolution passes replace the preview and refine normally. Resizing requires allocating new buffers.

`tests/hydra/usdview_motion.py` tests held-button trucking and orbiting through usdview's mouse-event handlers, checks that the rendered image changes on every input event without rebuilding geometry, and verifies refinement after release. It records screenshots and frame timings in `build/hydra-motion` (or `PRISM_VALIDATION_DIR`). The controlled parity fixture also checks that the panel moves in the expected screen direction, so changing noise or GL overlays cannot substitute for actual camera feedback. Run it with the installed USD runtime and `testusdview --testScript`, using either `tests/hydra/fixtures/parity.usda` or the OpenChessSet stage.

usdview's **default dome** is OpenUSD's Stinson Beach environment, not PrismRT's bundled indoor EXR. It uses intensity 1 and exposure 0. Hydra respects these authored values without the standalone environment's historical extra factor of two. To reproduce standalone lighting, use the same HDR and account for that factor (for example, standalone exposure -0.5 corresponds to a USD dome exposure +0.5 at intensity 1).

The beach HDR contains a small, extremely bright sun. With the current BSDF-only path tracing, rare paths to the sun can leave bright or gray speckles on dark surfaces. The default sample clamp of 10 reduces those contributions and can also darken the result; increasing the clamp restores energy at the cost of greater variance. Lowering it can hide speckles but increases that bias. "Converged" currently means the requested sample count was reached, not that a noise threshold was met. Increase **Samples** for a cleaner image; efficient HDR sun sampling would require the separately deferred environment importance sampling/NEE/MIS work.

Scene changes are batched into owned Embree snapshots after canceling the previous render. Camera-only changes reuse geometry. Instances are flattened into world-space meshes, so memory and rebuild costs grow with expanded geometry rather than prototype geometry. Subdivision meshes use their polygon control hull. Implicit shapes use USD's mesh-conversion filter.

Curves, volumes/SSS transport, displacement, subdivision refinement, motion blur, analytic USD lights, arbitrary shader primvars, and multiple simultaneous UV sets are outside this implementation's initial scope. Dome lighting and emissive materials use the existing path tracer; no new light-sampling algorithm is introduced. See [material support](hydra_materials.md) for network conversion, asset resolution, opacity, caching, and diagnostics.
