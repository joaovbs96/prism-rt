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
| `prism:hiddenGeometryBudgetMiB` | 0 | Soft budget for retained hidden converted geometry; zero keeps unlimited retention |
| `prism:resourceRevision` | 0 | Increment to invalidate cached material and external texture fingerprints and reload dome resources without rebuilding geometry |
| `prism:selectionOutline` | true | Draw selection as an outline; false uses filled highlighting |
| `prism:selectionOutlineRadius` | 2 | Outline thickness in pixels |
| `prism:selectionUseHostSettings` | false | Use the host's Hdx outline parameters instead of PrismRT's overrides |

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

The launcher also loads PrismRT's Python usdview extension from `tools/usdview_plugins`. Stock 26.05 usdview drops the selected instance index when forwarding selection to its scene index. Our extension forwards the selection model's original indices through the internal `prism:usdviewSelection` renderer setting, preserving individual-instance outlines without patching USD. It uses usdview's plugin registration and `StageView` renderer-setting methods; it does not replace usdview functions. Other hosts can supply complete Hydra selection data directly. Without the extension, stock usdview still renders and picks correctly, but individual-instance highlighting follows its upstream whole-prim selection limitation.

The render buffers expose linear floating-point `color`, projected `depth` in `[0,1]`, `cameraDepth`, world `normal`, eye-space `Neye` and `Peye`, `primvars:st`, and integer `primId`, `instanceId`, and `elementId`. IDs and geometric AOVs use a deterministic center ray independent of antialiasing and depth-of-field samples. `Peye` is the hit position transformed into eye space; `primvars:st` is the interpolated first UV set, with hit UVs used when that primvar is absent. `Pworld` is not currently exposed. `elementId` refers to the coarse polygon face. Prim, instance, and face picking use visible primary intersections; edge, vertex, and deep picking are deferred. Signed and unsigned 16-bit integer AOV buffers and signed 32-bit integer buffers are supported; Int32 IDs retain their exact values. Packed float32/uint8 buffers are rejected with a Hydra warning. Extra Hydra camera clip planes are not applied to ray intersections.

Color alpha accumulates primary coverage, including stochastic cutouts and silhouette samples. With no dome, or `domeLightCameraVisibility=false`, primary rays that escape the scene produce transparent black; unresolved early terminations retain coverage. A visible dome keeps opaque alpha. Changing dome camera visibility restarts preview and full accumulation.

Picking traces deterministic full-resolution rays against the camera and immutable scene owned by the displayed frame, including during low-resolution preview. Retained frames and mapped buffers keep their original geometry across later edits and renderer destruction. An externally retained inactive scene uses a conservative rebuild recorded by `pinnedSnapshotFallbacks`; ordinary released-frame bank reuse remains available.

Hydra caches local-space environment importance distributions by prepared texture identity and exact RGB multiplier. Dome rotation and bank reuse share the distribution while each wrapper keeps its authored direction transform and PDF. Multiplier or texture-resource changes rebuild it. Statistics expose distribution builds, cache hits and uniquely shared importance-table bytes. The exact RGB multiplier is part of the cache key, so intensity/multiplier-only edits rebuild the table; reusing it across those edits remains deferred.

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

Multisampled Hydra-buffer tests validate allocation, rendering, `Resolve()` and mapped pixels, but do not validate presentation through usdview or another host GL compositor.

The usdview interaction script is `tests/hydra/usdview_smoke.py`, for the rebuilt USD's `testusdview --testScript` runner. It exercises picking, selection outlines and thickness, filled highlighting, visibility edits, AOV switching, resizing, and renderer teardown/recreation, saving screenshots for inspection. Scripts being present does not itself establish acceptance: consult the generated logs and images from an actual run.

## Scope and costs

### Interactive display and noisy HDR lighting

Camera changes immediately trace and display a coarse one-sample preview for the current camera. By default, the preview is capped at 96 pixels on its longest edge and enlarged to the viewport, keeping ray work bounded as the viewport grows. It updates during continuous dragging, even when mouse events arrive faster than a full-resolution pass can finish, and is intentionally noisy and pixelated. By default, full-resolution accumulation starts after a 100 ms quiet interval, avoiding repeated setup and cancellation between mouse events. Preview samples use separate random-number state and never enter final accumulation; once navigation stops, complete full-resolution passes replace the preview and refine normally. Resizing requires allocating new buffers.

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

usdview's **default dome** is OpenUSD's Stinson Beach environment, not PrismRT's bundled indoor EXR. It uses intensity 1 and exposure 0. Hydra respects these authored values without the standalone environment's historical extra factor of two. To reproduce standalone lighting, use the same HDR and account for that factor (for example, standalone exposure -0.5 corresponds to a USD dome exposure +0.5 at intensity 1).

The beach HDR contains a small, extremely bright sun. Environment MIS is now the default in both frontends: one importance-sampled environment direction competes with BSDF sampling at supported scattering vertices. The distribution uses exact texel solid angles and a small uniform mixture to cover filtered texture values. Diffuse and MDL materials supply matching BSDF evaluations/PDFs; legacy rough-metal and glTF samplers retain their BSDF-only behavior. The default sample clamp of 10 can still reduce brightness: use zero when measuring energy or variance. "Converged" means the requested sample count was reached, not that a noise threshold was met.

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

`GetRenderStats()` exposes material conversion/generation/compilation counters and timings, worker CPU time separately from batch elapsed time, prototype/placement rebuild counters, Embree timings, and buffer publication/mapping timing. Set `PRISM_PROFILE_BUFFERS=1` to measure worker buffer-write time; workers accumulate timings locally and update each buffer once per tile, while ordinary rendering leaves this instrumentation disabled. Material compilation concurrency is configured with `PRISM_MATERIAL_THREADS`; the default is half the hardware threads capped at four, with a minimum of one. `PRISM_MDL_VERBOSE=1` enables compiler diagnostics that would otherwise obscure loading measurements.

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
