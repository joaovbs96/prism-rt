# Round 3 validation

## Integration status (27 September 2026)

Test workstation: Windows, Intel Core i7-14700K, 28 logical processors.

The integrated build now passes all 22 CTest tests, including actual native MDL
2D/UDIM targets, argument-block resource isolation, SDK texture-canvas
comparisons, footprint propagation, and Hydra snapshot/buffer tests. This is
not completion of the large-scene acceptance matrix below.

The added generated-source cache tests cover corrupted/truncated artifacts,
concurrent publication, exact disk-quota boundaries, and reuse in a fresh
process matching independent generation. Its existing memory-cache test now
uses an isolated temporary cache: the first full run found that its scalar
miss assertion incorrectly depended on the disk cache being empty. After the
test-only correction it passed three consecutive runs; the other 20 tests had
already passed. The sphere tests include the 1,024-entry boundary, local batch
updates, mixed batch/ordinary parent transforms, materials and picking.

A later fresh-directory build exposed intermittent concurrent generated-cache
publication failure (also reproduced on repetition 11 of a stress run). Cache
publication and eviction now share a cross-process lock, and Windows publishes
closed artifacts with atomic replacement. The threaded and eight-process tests
each passed 30 consecutive repetitions after the fix. The complete 22-test
suite then passed in `build-round3-clean`; logs are
`build/round3-cache-stress.log` and `build/round3-clean-install-final.log`.

The real native-MDL target test now closes compilation transactions before
executing targets, checks selector fallback resource isolation, and verifies
nonzero derivatives against a checker mip reference. It passed three repeated
runs as well as the complete suite. Instance-selection regressions cover owned
old-frame topology, nested reordered source indices, masked ordinals, and
unrelated simultaneous selections.

Subsequent focused regressions also pass for multi-repeat UV derivatives and
actual filtered TextureSystem samples against a supersampled checker reference.
The native MDL target test covers rejection of incompatible string arguments.
Its previous SDK database teardown warning was traced to an unreleased retained
`IValue_texture::get_type()` interface; owning that interface with an SDK Handle
eliminates the warning on the real 2D/UDIM tests.

Visibility-only updates now enable/disable dependent Embree placements without
rebuilding unrelated prototypes. A hide, prototype-edit, show sequence is
covered separately. If the hidden prototype has lost its placement records,
restoring visibility conservatively rebuilds the snapshot and a synchronized
spare; `visibilityRecoveryRebuilds` and `conservativeRebuilds` report that cost.

The independent InOneWeekend comparison identified a camera unit error:
Hydra's camera adapter had already converted focal length to world units, but
the renderer applied a second factor of 0.1. This made its lens aperture ten
times too small. Both frontends now use the same aperture conversion helper.
With matched bilinear texture filtering, the 32-spp comparison improved from
approximately 7.03% normalized RMSE to **0.215%**, with **0.000239%** relative
mean-luminance difference. Three Hydra reruns were bit-identical at both 1 and
32 spp. Artifacts are in `build/iwo-parity-current`; these comparisons are
correctness measurements, not cold-loading benchmarks.

Three fresh-process stock usdview smoke runs in
`build/round3-usdview-final-r{1,2,3}` passed camera navigation, selection
outlines, picking, time changes, material and geometry edits, visibility, AOV
display, resizing, and renderer recreation. Each process used a separate empty
prepared-texture cache; the operating system file cache was not cleared.

| Run | Camera p95 / p99 (ms) | Updates/s | Geometry / material / visibility / resize convergence (ms) |
|---|---:|---:|---:|
| 1 | 32 / 47 | 43.17 | 375 / 500 / 235 / 469 |
| 2 | 47 / 47 | 40.40 | 328 / 437 / 219 / 406 |
| 3 | 46 / 47 | 39.60 | 359 / 453 / 250 / 485 |

Each navigation test includes 60 camera moves and waits for the requested
camera revision and framebuffer upload. Earlier timings based only on a global
publication counter are superseded. All three met the 50 ms p95 target on the
small parity fixture. This is not a production-scene latency guarantee;
large-scene measurements are reported below. Exit codes, cache paths, and
timing summaries are in `build/round3-usdview-final-summary.json`.

## Large-scene baseline

The current integrated DLL was tested with three fresh `testusdview` processes
per scene, four material workers, a fresh prepared-texture cache per process,
and 60 camera moves. Each process first created Prism from a Storm host, then
switched away and recreated Prism in the same process to measure a warm
renderer pass. The OS file cache was not cleared. These first measurements use
the automatic scene framing; ChessSet leaves most of the viewport on its
environment, so its navigation numbers are not a close-view target. A
separate ChessSet geometry stress-view pass is recorded below; its camera is
below the board and should not be treated as an above-board composition.

| Scene | First-image cold median (s) | Same-process recreation median (s) | Navigation p95 / p99 median (ms) | Peak process working set median (GiB) | 50 ms p95 runs |
|---|---:|---:|---:|---:|---:|
| OpenChessSet | 22.24 | 2.96 | 35.80 / 36.45 | 1.65 | 3/3 |
| Kitchen_set_instanced | 3.64 | 3.39 | 80.16 / 89.00 | 0.77 | 0/3 |
| InOneWeekend | 6.39 | 1.57 | 33.68 / 34.12 | 1.67 | 3/3 |

Kitchen navigation exceeds the 50 ms p95 target in all three repeats. The
reported p95/p99 values cover 60 camera moves, including preview publication
and framebuffer upload. Working-set measurements come from Windows process
counters. Embree's memory counters are invalid zero values in this frozen DLL
and are intentionally excluded until their allocation callback accounting is
fixed. Process logs, frame images, per-pass renderer stats, and JSON are under
`build/round3-large-scenes/empty`.

The usdview smoke was rerun after explicitly authoring the UsdShade
`info:implementationSource = "id"` fallback on its dome fixture. It passed
with exit code 0 and no invalid implementation-source warning. The measured
DLL was `build/hydra/plugin/hdPrism.dll`, SHA-256
`C64104F684AE46CC3BA5BCCDB5273F74BE85B667DD3AF1AD936450615564B842`, written
2026-09-27 10:15:44 UTC. Machine metadata: Intel Core i7-14700K, 28 logical
processors, Windows NT 10.0.26200. Physical memory was unavailable because
WMI access is denied in this environment.

### ChessSet geometry stress view

After fixing the free-camera near clip for small focus distances, three fresh
processes focused `/ChessSet/Black/RookL` with an empty prepared cache, four
workers, and 60 navigation moves in both the auto-framed and focused views.
All three runs exited successfully. The focused 96 by 96 preview now contains
dense scene geometry instead of only the environment, but its `rotPhi = -25°`
places the camera below the target. Treat these results as geometry-heavy
stress views, not an above-board rook inspection or final visual validation.

| Run | Cold first frame (s) | Same-process recreation (s) | Auto-view p95 / p99 (ms) | Focused-view p95 / p99 (ms) | Process wall (s) |
|---|---:|---:|---:|---:|---:|
| 1 | 23.73 | 2.77 | 35.68 / 36.38 | 35.05 / 35.40 | 62.30 |
| 2 | 22.22 | 2.59 | 28.77 / 34.32 | 17.21 / 17.53 | 63.34 |
| 3 | 22.55 | 2.66 | 33.72 / 34.28 | 33.74 / 34.34 | 60.78 |

The focused camera used distance `0.2427`, near clip `9.7087e-5`, and far
clip `2,000,000`. The measured plugin was SHA-256
`82fe1bdfdc1bef2c721f42151c60424d5b6da2ea47f056b5d38c0c659ee44cd9`, written
2026-09-27 10:53:50 UTC. Per-run logs, screenshots, process timings, renderer
stats, and camera details are under `build/round3-chess-focus-selection-cache`.

The standalone simple-OpenPBR conversion now shares immutable MaterialX library
definitions instead of copying them per material, and writes recovered graphs
to immutable content-named cache files. An exploratory fresh-process
InOneWeekend load took 9.57 seconds with an empty prepared cache. Its 32-spp
bilinear image agrees with the pre-change image to normalized RMSE
`3.53e-8` and relative mean-luminance error `2.17e-9`. This single run is a
regression check, not the required repeated cold-loading comparison.

Known acceptance gaps include the complete OpenChessSet/Kitchen performance
matrix, filtered-image reference comparisons, and broader filtered-scene
accuracy tests. Resource accounting is partial: admission uses
estimated compilation costs, native SDK allocations are not fully attributed,
and the disk budget covers prepared textures and generated MDL source but not
resolver-materialized source files. Optional sphere batching remains disabled
by default; its measured image/performance comparison is reported below.
Broader structural material parameterization beyond exposed color defaults is
not complete.

## Clean-directory build and relocated installation

The `build-round3-clean` configuration reuses the selected external USD,
MaterialX, OIIO and other SDK installations. After fixing the test compile error
and intermittent cache race found by its initial build/checks, the resumed
validation passed all 22 CTest tests and installed into a new prefix containing
spaces. Build-tree product DLL paths were removed before executing installed
binaries; external dependency directories remained available.

`build/round3-clean-install-accepted.log` records the successful complete run.
It includes CLI/importer smoke checks, texture cache process comparisons,
standalone/Hydra comparisons, exported procedural scenes, native-instance
bindings, and actual usdview lifecycle, sustained-drag, instance-selection and
extension checks. The installation excludes Python bytecode cache directories.

The filtered standalone/Hydra comparisons used independently ingested scenes:

| Fixture | Normalized RMSE | Relative mean-luminance difference |
| --- | ---: | ---: |
| Controlled MaterialX panel, 128 spp | 2.13667e-8 | 4.57068e-9 |
| Exported InOneWeekend, 1,024 spp | 0 | 0 |
| Exported CornellBox, 128 spp | 0.00827189 | 0.000441206 |
| Exported QuadTest, 128 spp | 9.0768e-9 | 8.73422e-11 |

All entries pass the 0.01 limit for both ratios. Each frontend's rerun was
bit-exact, and the PreviewSurface/equivalent MaterialX comparison was also
bit-exact. These finite fixtures do not establish accuracy for all filtered
production textures.

The installed lifecycle test passed camera navigation, live edits, resizing,
picking, outline/fill/highlight toggles, AOV changes and renderer recreation.
The sustained mouse-drag test measured 41.72 ms median and 48.08 ms p95 on this
workstation. It now waits for the exact requested camera revision and includes
waiting, presentation and framebuffer reads in latency; its old assumption of
synchronous preview publication failed against the asynchronous implementation.
Panel silhouette movement and geometry-rebuild assertions remain enabled.
Artifacts are under `build-round3-clean/usdview-validation`.

The normal `build` directory was rebuilt successfully as well. Its current
plugin SHA-256 is
`5d2dc459f6f3c36c784b17c848c276c1129f2c92fa44703de953ba75a1b48081`.

Three subsequent fresh-process Chess runs used that normal-build plugin, four
material workers, an empty renderer cache for each run, and the corrected
above-board camera (`--focus-prim /ChessSet/Black/RookL --focus-elevation 25`).
The operating-system file cache was not cleared. The first captured preview was
visually checked: the board and chess pieces are visible from above. This is a
96-by-96 interactive preview, not a final image-quality reference.

| Run | Cold first image (s) | Same-process recreation (s) | Focused navigation p95 / p99 (ms) |
| --- | ---: | ---: | ---: |
| 1 | 23.85 | 2.91 | 24.89 / 32.78 |
| 2 | 22.80 | 3.00 | 33.62 / 33.65 |
| 3 | 22.51 | 2.90 | 29.96 / 32.19 |

All runs passed; artifacts and commands are in
`build/round3-chess-above-board`. Median cold first-image time remains 22.80 s;
the 2.90 s recreation median must not be presented as cold-loading performance.

## Texture working-set benchmark

This validation measures a set of tiled textures larger than the configured
OIIO tile-cache budget. The fixture has 20 deterministic 512 by 512 RGBA float
images (80 MiB of source pixels) and a 32 MiB default cache budget. It prepares
lossless `.tx` files, checks same-process resource deduplication, then compares
anisotropic mip-filtered samples with bilinear highest-resolution samples.

Run the benchmark after building `prism_texture_cache_benchmark`:

```powershell
python tests/benchmark_scalability.py `
  --executable build/RelWithDebInfo/prism_texture_cache_benchmark.exe `
  --output build/round3-validation/scalability
```

The runner writes `results.json` and per-process logs. It runs three fresh
processes against empty prepared-texture directories and three fresh processes
against a directory populated by a separate warm-up process. Every helper
process repeats preparation in-process and records whether it received the
same immutable handles. Two extra helper processes concurrently prepare the
same 20 sources into one cache; the runner checks that both processes succeed,
all 20 published `.tx` files are readable and nonempty, and no temporary files
remain. The cold and warm labels describe the prepared `.tx`
cache state; the runner does not clear the operating system's file cache.

The validation ran with OpenImageIO 3.1.12.0, a 32 MiB tile-cache budget, and
30,000 deterministic sample points per process. Three fresh processes ran in
each prepared-cache state. The operating system file cache was left intact.

| Prepared cache | Preparation median (s) | Sampling median (s) | Disk cache hits per process | Filtered checksum |
|---|---:|---:|---:|---|
| Empty | 0.652 | 2.004 | 0 per process | `0xd9e71e021624a858` |
| Prepopulated | 0.060 | 2.016 | 20 per process | `0xd9e71e021624a858` |

Both states reported 20 same-process memory-cache hits, a 31.9 MiB peak OIIO
tile cache, 512 peak resident tiles, and 1.3 GiB of pixel data read after the
filtered and bilinear passes. Their filtered checksum was identical, as was
the bilinear checksum (`0xd094007ecf7df123`). The filtered-versus-bilinear RMS
was 0.185 and the maximum absolute channel difference was 0.558. These are
differences between anisotropic mip-filtered and bilinear highest-resolution
sampling modes, not errors against ground truth. The warm
prepared cache shortened median preparation time. The 0.012 second difference
in median lookup time is small for this run.

The concurrent-publication check passed with two fresh processes targeting one
empty cache. Both completed with stable finite checksums; one observed 20 disk
cache hits after the other published those entries. The cache contained 20
nonempty readable `.tx` files and no temporary files after both exited.

The JSON retains per-repeat process times, OIIO cache statistics, memory-cache
hit counts, checksums, finite-value results, and RMS / maximum differences.
This fixture measures cache pressure and sampling behavior for generated
images. It does not represent a broad production scene or establish a general
performance threshold. The helper verifies that its runtime OIIO reports the
same version as its compiled headers; the validated binary used OpenImageIO
3.1.12.0 from the project dependency installation.

## Instrumented Kitchen navigation diagnostic

One follow-up fresh-process Kitchen run against the later instrumented plugin
completed successfully (exit code 0, workers 4, empty prepared cache). The
60-move default-view pass measured navigation p95/p99 of 82.37/87.38 ms. Each
preview traced 9,216 pixels at 96 by 96, bound five AOVs, and published 8 MiB.
Median ray work was 2.34 ms, publication 2.55 ms, and StopRender 1.51 ms
(p95 values 2.88, 2.95, and 2.53 ms). Thus those measured renderer stages
account for only a small part of the 82 ms interaction latency; most time is
outside the instrumented stages, including host/event/framebuffer work.

The added close-view pass in this harness was still not a true object-targeted
Kitchen view: its measured preview remained 96 by 96. It had 91.37/100.37 ms
navigation p95/p99, with median ray work 4.36 ms and publication 2.42 ms.
These are diagnostic values from one process, not a repeated acceptance result.
The exact run artifacts are under `build/round3-kitchen-instrumented`. The
plugin was SHA-256 `e3145b8a7a9a1e9e8b01a55532de55f89e67f7a7dbe4725e3304f443e05a39d7`,
written 2026-09-27 10:35:42 UTC.

A later host-phase probe ran once against the selection-instrumented plugin
(exit code 0, fresh process, workers 4, empty prepared cache). Navigation
p95/p99 was 86.49/94.00 ms. Median and p95 camera-update time was
0.045/0.056 ms; preview wait was 30.41/57.36 ms; and final framebuffer
handling was 26.42/34.13 ms. These phases overlap with native render-stat
timings and should not be summed. The per-event selection-offset counter
increased by 45.54 ms median and 70.65 ms p95, while pick time was zero; this
indicates selection encoding is the main measured cost. Ray work was 2.21 ms
median, publication 2.68 ms, and StopRender 1.37 ms. The exact host phases,
selection counters, and render stats are in
`build/round3-kitchen-host-phases/Kitchen_set_instanced-workers4-run1/navigation.json`.
The plugin SHA-256 was
`33553a434a9dc81db5864f087d2fcf16b7849a9ee524169983299ed769cbd3ef`, written
2026-09-27 10:44:40 UTC.

The associated parity usdview smoke passed with exit code 0 against the same
plugin. It verified center picking of `/World/Panel`, outline color without
interior tint, outline-radius changes, filled-selection tint, deselection, and
that selection updates do not rebuild the scene snapshot. Camera navigation
p95/p99 was 32/32 ms; the interaction timing and 15 screenshots are under
`build/round3-selection-smoke/output`.

The measured selection cost was then removed from camera-only frames with a
bounded selection-offset cache. Three fresh-process Kitchen repeats measured
navigation p95 values of 34.37, 35.49, and 33.54 ms, compared with 86.49 ms in
the instrumented pre-cache probe. They sustained 43.45, 43.39, and 43.39
camera updates per second. Selection-offset time did not increase during any
camera event. Each process encoded selection twice while loading and visited
2,924 paths in total, then reused that encoding during navigation. The result
isolates the improvement to selection work: preview ray tracing and publication
were already only a few milliseconds.

The three run records are under `build/round3-kitchen-selection-cache`, with
the aggregate in `runs.json` and per-run measurements in
`Kitchen_set_instanced-workers4-run1` through `run3`. The validated plugin
SHA-256 was
`82fe1bdfdc1bef2c721f42151c60424d5b6da2ea47f056b5d38c0c659ee44cd9`.

The expanded usdview smoke passed with exit code 0. In addition to live
selection, deselection, outline, and filled-highlight pixel checks, it toggled
the host highlight off and on and verified the original and selected images.
It also asserted that a camera-only published frame adds neither selection
paths visited nor cache misses. The cache invalidates on published picking-table
identity, source tracker pointer and version, usdview selection revision, host
select/locate-enable flags, and selection or instancer scene notices; camera
generation is deliberately absent from the key.
Smoke artifacts are under `build/round3-selection-cache-smoke/output`.

## Experimental analytic-sphere batching

The opt-in `prism:sphereBatching` path was compared with the default analytic
sphere representation on InOneWeekend at 256 by 144, 32 spp, seed 0, sample
clamp disabled, and filtered texture sampling. Three alternating paired runs
used fresh renderer processes with a distinct initially empty
`PRISM_CACHE_DIR` for each process; the operating-system file cache was not
cleared. Repeats within each mode were bit-exact. The
ordinary and batched images passed the 1% cross-mode threshold with normalized
RMSE 0.0028412 and relative mean-luminance error 0.00000707793. The small
cross-mode pixel difference is consistent with changing Embree from individual
instances to sphere-point entries; it is not a within-mode determinism issue.

Median process wall time was 6.743 seconds for ordinary spheres and 6.679
seconds for batched spheres. Median reported render time changed from 0.521 to
0.500 seconds. This sample does not establish a repeatable overall speedup, so
batching remains disabled by default. Reported Embree live memory decreased
from 333,072 bytes to 43,680 bytes for this scene.

The experimental path batches at most 1,024 entries and preserves a separate
prim ID and material binding for each entry. It accepts only uninstanced
spheres whose transform is translation plus finite positive uniform scale.
Rotated, mirrored, sheared, nonuniformly scaled, and point-instanced spheres
retain the ordinary representation. Radius and eligible transform edits
rebuild only dependent inactive batches; material changes update entry
bindings without rebuilding geometry. A change that crosses the eligibility
boundary uses a recorded conservative snapshot rebuild. These constraints are
part of the opt-in behavior and are not a basis for changing the default.

Commands, per-process cache counts and render statistics, logs, images,
checksums, and comparison results are retained in
`build/round3-sphere-batches/results.json` and its sibling artifacts.

