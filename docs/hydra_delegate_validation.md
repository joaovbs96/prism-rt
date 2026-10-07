# Hydra delegate improvement validation

This ledger records Hydra delegate changes and their validation evidence. The
2026-10-03 integration work is recorded first; the older sections preserve the
historical results associated with `HYDRA_DELEGATE_PLAN.md`.

## Milestone 4 � structural transactions

Instanced prototype/subset additions and removals passed the focused suite in
15.21 seconds (`build/phase4-prototype-c-test.log`), including 16, 128, 1,024 and
16,384 placements, fresh-render parity and retained old-frame picks. Preflight
now accounts for subset additions and parent removals coalesced during bank
replay; earlier candidates conservatively rebuilt these sequences.

Subtree addition/removal/re-addition passed the focused suite in 14.57 seconds
(`build/phase4-subtree-b-test.log`) at all four placement counts. Added descendants
are captured recursively; removed geometry is inventoried from bank records.
Unrelated paths retain IDs and geometry. A detach-count assertion first advances
both banks: an older bank that never contained a transient added subtree can
correctly perform zero physical detachments when that subtree is removed.

Explicit paired renames passed the focused suite in 14.09 seconds
(`build/phase4-rename-test.log`) at all four placement counts. Tests verify
compatible prototype/placement reuse, exact image parity, destination-path IDs,
tombstones, rename-back bank replay and retained old-frame picks. Additional
nested/duplicate and evicted-prototype cases are in the final candidate build.
The first final CTest run passed 32/33 tests and exposed a cold-rename replay
ordering bug: a subsequent visible edit could restore using the old path before
rekeying. The candidate now rechecks evicted-prototype eligibility after rekeying.
The failed run is preserved in `build/phase4-full-ctest-cold-rename-failure.log`.
Capture also deduplicates overlapping parent/descendant notices within a generation.
The corrected focused suite passed in 14.97 seconds
(`build/phase4-restoration-test.log`), and the complete CTest suite passed 33/33
in 71.82 seconds (`build/phase4-full-ctest.log`). Nested instances with duplicate
source indices and cold-prototype rename/restore/removal are included.
The pre-review candidate is preserved as
`build-fresh-validation/hydra-performance-phase4/hdPrism-before-counter-review.dll`,
SHA-256 `ac716ef0634e07116f43b7ee4a6cc06d75ad97d11a5b629a5bd14c3a5d610f6d`.
A final review made placement work counters accumulate across a transaction and
removed a redundant second placement reconciliation during rename. The nested
rename matrix now asserts exactly one visit per placement. The reviewed build
passed 33/33 CTest tests in 69.39 seconds (`build/phase4-reviewed-ctest.log`).
Its DLL is `build-fresh-validation/hydra-performance-phase4/hdPrism.dll`, SHA-256
`bad8c4c743bc758e61f0fde4e1f0c521b3dadce9d83085bb93e1079b862b36ca`.

Filtered and bilinear 256-sample parity tests passed, and all four final images
matched the preserved pre-regression images exactly (zero RMSE and luminance
error), recorded in `build/phase4-parity.log`. Stock usdview presentation, motion,
preview picking, smoke and background/FrameRecorder PNG/EXR tests passed. The
odd-size partial-tile screenshot was inspected and showed distributed updates.
Host artifacts are in `build-fresh-validation/hydra-performance-phase1/ui/*-phase4`.
Real-host structural scaling passed sequentially at 16, 128, 1,024 and 16,384
placements (`build-fresh-validation/hydra-performance-phase4/scaling-final`).

Procedural InOneWeekend and QuadTest export determinism and cross-frontend parity
passed in both filtering modes. CornellBox is deterministic but its standalone
versus Hydra comparison exceeds the existing 1% threshold: normalized RMSE
0.0721589 at 64 samples and 0.04222 at 256 samples (luminance error 0.00160345 and
0.00225168 respectively). The candidate matches the preserved pre-regression
Hydra DLL exactly at 64 samples and at 256 samples in both filtering modes, so
this is a pre-existing cross-frontend discrepancy, not a changed Hydra image.
No tolerance or renderer code was changed to suppress it. Evidence is in
`build/phase4-export-baseline.log`, `build/phase4-exports-and-scaling.log` and
`build-fresh-validation/hydra-performance-phase4/export-confirmation`.
The final-binary performance comparison passed all four gates at both
resolutions. Five alternating runs per build/resolution were performed without
concurrent validation jobs. Raw runs, per-run memory/work counters, and the
hashed summary are in
`build-fresh-validation/hydra-performance-phase1/gate-phase4-final/summary.json`.

| Resolution / median metric | Pre-regression | Previous tiled | Final candidate |
| --- | ---: | ---: | ---: |
| 1080p first sample (s) | 0.5081 | 0.6085 | 0.4411 |
| 1080p samples/s | 3.1654 | 2.3307 | 3.6661 |
| 1080p paint p95 (ms) | 34.710 | 9.760 | 8.869 |
| 1080p update-gap p95 (ms) | 146.456 | 138.209 | 137.208 |
| 1080p setting-change cancellation (ms) | 4.143 | 2.593 | 2.511 |
| 4K first sample (s) | 1.9840 | 2.2336 | 1.6298 |
| 4K samples/s | 0.6599 | 0.5012 | 0.8331 |
| 4K paint p95 (ms) | 110.231 | 14.128 | 13.468 |
| 4K update-gap p95 (ms) | 283.814 | 206.519 | 162.838 |
| 4K setting-change cancellation (ms) | 28.760 | 8.845 | 17.152 |

The candidate beats the pre-regression first-sample and throughput targets and
meets the paint/update-gap limits relative to the tiled build. The 4K cancellation
tradeoff remains: the measured seed-setting call is slower than the tiled build.
This is a host setting-call measurement, not isolated worker-drain time, and no
claim is made that cancellation has been fixed. Results differ from the earlier
milestone 1 session; comparisons use contemporaneous alternating runs.

### Deferred follow-up: render interruption latency (2026-10-03)

Deferred at the user's request; no cancellation optimization is underway.
The final 4K benchmark measured a median of 17.152 ms for the complete
random-seed setting call, versus 8.845 ms in the previous tiled build (about
8.3 ms extra per interruption). This includes stopping ongoing work and updating
the setting. It is neither isolated cancellation time nor time to the first
replacement image, and it is not overhead paid on every frame. Camera movement
also interrupts rendering, but these numbers were measured with seed changes.

When revisiting, instrument worker draining, presentation-payload construction,
final commit and setting-update work separately before attributing the increase.
Investigate cancellation checks between payload tiles and the time spent draining
active tasks; these are hypotheses, not established causes. Preserve immutable
published frames and safe task draining, then repeat the alternating 1080p/4K
comparison and verify that throughput, redraw cadence and image parity remain
within the existing gates. Use the final benchmark summary linked above as the
recorded starting point.

All four implementation milestones are delivered in the separate validation
build. The normal plugin has not been replaced. Renderer-core code, final color
sampling and stock OpenUSD 26.05 remain unchanged. Remaining limitations are the
named conservative structural fallbacks, whole-array comparisons for Hydra
notices, copying the immutable picking table during topology changes, the 4K
cancellation tradeoff, and the baseline-identical Cornell cross-frontend export
threshold failure described above.

## Milestone 3 � instance metadata deltas validated

The initial milestone 3 validation build passed the scene bridge suite in
11.20 seconds (`build/phase3-initial-test.log`). Single-source edits at 16, 128,
1,024 and 16,384 placements visit one dependent placement and report zero
canonical, picking, count, input-metadata and dependency rebuild records.
This is an initial correctness result, not final milestone acceptance.

The next candidate adds affected-prototype input-cache refresh, affected-prim
picking reconstruction, immutable picking-pointer assertions and separate
bookkeeping timing scopes. Full-array input comparisons remain explicit.
Topology changes currently copy the immutable picking table before replacing
its affected records; transform-only changes share the original table.
`lastPlacementReconcileMilliseconds` is an enclosing scope; its input comparison
and dependency subscopes must not be added to it as independent wall time.

The final focused suite passed in 10.26 seconds (`build/phase3-final-test.log`).
Stock usdview placement probes passed sequentially at 16, 128, 1,024 and 16,384
placements (`build-fresh-validation/hydra-performance-phase3/scaling-pure`).
The host probe distinguishes a position edit that must replay an earlier
prototype change from a pure position edit; the initial combined assertion
failed because it included that required replay. Both operations remain recorded.
The preserved milestone 3 DLL SHA-256 is
`eae59b227c88df72d7643f16ad2029922babc34d86d69cb9764f4bad4447fd61`.
Final full-suite and image/host regression checks remain due after milestone 4.

## Four-phase performance plan, 2026-10-03 — milestone 1 performance gate passed

This follow-up is limited to refinement overhead, geometric-only AOV rendering,
instance bookkeeping, and incremental prototype/subtree/rename transactions, in
that order. Later milestones remain pending until the first performance gate
passes. The renderer core and final color sampling are unchanged.

The refinement candidate caches the interleaved tile schedule and replaces
64-tile execution barriers with one pass-wide TBB job. A coordinator consumes
completed-tile flags at approximately 100 ms intervals; only intermediate
publications construct immutable tile payloads. Completion publishes the exact
accumulated image directly. Cancellation drains the job before its captured
buffers or scene can be released. Separate scopes measure summed ray/AOV worker
time and coordinator wait, payload, publication, and final-commit wall time.

Pilot measurements exposed two presentation costs after removing the rendering
barriers. The candidate now reuses same-size GPU source textures, batches up to
1,024 tiles per atlas, and destroys retired tile packets outside the buffer
publication lock. These preserve partial updates, background composition, the
unchanged-redraw upload cache, and the stock-format fallback.

The preserved pre-regression DLL is
`build/hydra-integration-baseline/bundle/plugin/hdPrism.dll`; the previous tiled
DLL is `build-fresh-validation/hydra-display-candidate/bundle/plugin/hdPrism.dll`.
Candidate versions and run artifacts are under
`build-fresh-validation/hydra-performance-phase1`. The five-run comparison
rotates all three variants, uses identical instanced Kitchen settings and a
12-second observation window, and runs without concurrent validation jobs.
The machine remains interactive, with uncontrolled background load.

The harness now uses `perf_counter()` for Windows sub-millisecond timings and
counts both partial packets and completed samples as display updates. Earlier
partial-only gap calculations are not comparable. The tracked
`tests/hydra/summarize_refinement.py` records binary hashes, per-run timings and
work/memory counters, medians, and each acceptance criterion separately.

The buffer and scene-bridge CTests passed 2/2 in 9.41 seconds
(`build/phase1-focused-v5.log`). The cancellation test pauses after observing
the first intermediate packet and retains that immutable packet directly;
materializing an entire CPU display image before cancellation allowed the
faster renderer to legitimately finish its pass during the test itself.

Five 1080p runs per variant passed every performance threshold:

| Median | Pre-regression | Previous tiled | Candidate |
| --- | ---: | ---: | ---: |
| First completed sample | 0.470 s | 0.580 s | 0.437 s |
| Samples/second over observation | 3.330 | 2.417 | 3.834 |
| p95 paint time | 27.68 ms | 8.70 ms | 8.81 ms |
| p95 display-update gap | 138.26 ms | 131.69 ms | 133.99 ms |
| Setting-change cancellation | 3.91 ms | 2.86 ms | 2.85 ms |

The same candidate's five-run 4K comparison did not pass the paint-time gate:

| Median | Pre-regression | Previous tiled | Candidate |
| --- | ---: | ---: | ---: |
| First completed sample | 1.775 s | 2.110 s | 1.395 s |
| Samples/second over observation | 0.742 | 0.583 | 0.916 |
| p95 paint time | 89.68 ms | 12.48 ms | 15.90 ms |
| p95 display-update gap | 206.43 ms | 160.79 ms | 170.54 ms |
| Setting-change cancellation | 14.61 ms | 8.49 ms | 19.82 ms |

The other three 4K criteria passed. Raw measurements, hashes, and decisions are
in `build-fresh-validation/hydra-performance-phase1/gate-v5/summary.json`; its
candidate is preserved as `candidate-v5.dll`. Publication cadence refinement
and broad correctness validation are still pending. These results must not be
treated as completion of milestone 1 or the four-phase plan.

The next candidate anchors publication deadlines rather than adding payload
construction time to every 100 ms interval. Its focused 4K pilot recorded
11.32 ms p95 paint time versus 12.46 ms for the tiled build; this single run is
diagnostic, not acceptance. A subsequent exception-drain guard also releases
paused workers if coordinator allocation fails. The resulting candidate is
undergoing the full repeat in `gate-v7`.

The cadence-corrected build passed all 33 CTests in 61.54 seconds
(`build/phase1-ctest-v6.log`). After the exception-drain change, focused bridge
and buffer tests passed 2/2 in 9.71 seconds (`build/phase1-focused-v7.log`).
Filtered and bilinear 256-spp determinism, standalone/Hydra parity and
PreviewSurface/MaterialX checks passed. All four final images match the
preserved baselines exactly, with zero RMSE and luminance error. Evidence is
under `hydra-performance-phase1/hydra-validation-{filtered,bilinear}` and
`build/phase1-parity-v7b.log`. The first launcher attempt used the unwritable
default user cache; the successful rerun explicitly uses `build/test-cache`.

The real usdview presentation probe also passed on this candidate, covering
zero uploads on unchanged redraws, selection/background changes, normal-AOV
fallback, resize/cancellation, and a 2049-by-2049 partial display. Evidence is
under `hydra-performance-phase1/ui/presentation` and
`build/phase1-presentation-v7.log`.

The final five alternating runs per variant/resolution passed all four gates
at both resolutions (`hydra-performance-phase1/gate-v7/summary.json`). The
accepted candidate DLL is preserved as `candidate-v7.dll`, SHA-256
`b37a6aa4b7b9f7209ec27ecb6dfe822d9e80f17b8009e8be07b231644ad00cc9`.

| Resolution / median | Pre-regression | Previous tiled | Accepted candidate |
| --- | ---: | ---: | ---: |
| 1080p first completed sample | 0.467 s | 0.572 s | 0.429 s |
| 1080p samples/second | 3.408 | 2.499 | 3.917 |
| 1080p p95 paint | 25.85 ms | 9.10 ms | 8.47 ms |
| 1080p p95 display gap | 132.37 ms | 131.90 ms | 127.12 ms |
| 1080p cancellation | 4.38 ms | 2.85 ms | 2.95 ms |
| 4K first completed sample | 1.762 s | 2.062 s | 1.395 s |
| 4K samples/second | 0.669 | 0.583 | 0.917 |
| 4K p95 paint | 94.08 ms | 12.26 ms | 10.87 ms |
| 4K p95 display gap | 207.27 ms | 159.57 ms | 160.81 ms |
| 4K cancellation | 16.08 ms | 6.81 ms | 20.73 ms |

The accepted candidate improves the measured first-sample and throughput
regression while preserving redraw cadence. Cancellation remains bounded by
draining worker tasks and any complete-frame commit already in progress; the
4K cancellation median is worse than the tiled reference and is not claimed
as an improvement. Raw memory/work counters and summed worker versus wall
timings remain in each run's `cadence.json` and the summary. The candidate is
accepted against the specified first-sample/throughput/paint/gap thresholds;
milestones 2–4 remain pending.

### Milestone 2 — geometric-only rendering

Implemented classification before preview/color-pass allocation. Supported
geometric-only requests generate the same deterministic center rays directly,
resolve shared native AOV planes, and publish one complete pass. Color tracing,
preview, refinement delay, color scratch and persistent RNG storage are skipped.
Completed deterministic output is unaffected by sample-budget-only changes;
adding color resets accumulation with the existing seed rules. Unsupported-only
requests publish their clear values without ray work. Reused unsupported
buffers explicitly stage their requested clear, preserving immutable old maps.

Focused bridge/buffer tests passed 2/2 in 9.63 seconds
(`build/phase2-focused-final.log`). They compare all nine supported geometric
outputs byte-for-byte against mixed renders, with duplicate destinations,
repeated bindings, null color, unsupported outputs and depth-of-field enabled.
They verify zero extra color rays/previews, zero color scratch/RNG storage,
shared requested-only staging, retained maps, sample-budget preservation,
fresh color accumulation after rebinding, changed unsupported clears and exact
integer clears of 16,777,217. A fresh 37-by-29 geometric-only fixture verifies
one pass, camera/scene invalidation and zero color work. A 4097-by-2049 pass
is canceled after center-ray work begins, preserving its complete clear and
withholding convergence; resize/restart then succeeds. The refined quadrant
assertion also passes for distributed intermediate color packets.

The real usdview presentation/AOV-switching probe passed
(`build/phase2-presentation.log`, artifacts under
`hydra-performance-phase1/ui/presentation-phase2`). The milestone binary is
preserved at `build-fresh-validation/hydra-performance-phase2/hdPrism.dll`.
Milestones 3–4 and the final broad validation remain pending.

## Hydra integration improvements, 2026-10-03

The integration scope covers requested-only geometric AOV staging, cached GPU
presentation, interleaved refinement, sparse instance edits, incremental
instance-count/subset changes, and an opt-in hidden-geometry budget. Render
regions and additional USD geometry features are outside this change.

The `prism-hydra-fresh` build completed successfully in
`build-fresh-validation`. The usual `build` plugin was still loaded by an
existing usdview process, so it was not replaced. The fresh plugin SHA-256 is
`a8aff0383ad5da31568f65362c770e392660e4e50cdeff4b2cfa4a0f85cbc814`.

All 33 CTest tests passed in 68.74 seconds; the log is
`build/hydra-integration-final-ctest.log`. The expanded bridge tests cover AOV
subsets and repeated destinations, exact typed IDs, odd-size tile coverage,
instance edits and count changes, and hidden-geometry eviction/restoration with
retained immutable frames. Budget tests include shared storage, LRU ordering,
edits while evicted, purpose changes, zero/default budgets and pinned overage.

Both filtered and bilinear 256-spp validation runs passed determinism,
standalone/Hydra parity, and PreviewSurface/MaterialX checks. All four final
standalone/Hydra images also matched their preserved per-renderer baselines
exactly, with zero RMSE and zero luminance error. Baselines are under
`build/hydra-integration-baseline/hydra-validation-{filtered,bilinear}`; final
results are under `build-fresh-validation/hydra-validation-{filtered,bilinear}`.

The real usdview presentation probe passed with the fresh plugin. Twelve
unchanged paints left CPU-to-GPU upload bytes unchanged at 21,199,920, while
the skipped-upload counter rose from 89 to 115. Selection, deselection and
background changes added no image uploads. The probe also exercised a normal
AOV fallback and return to color, camera/resize cancellation, and partial
presentation at an odd 2049-by-2049 resolution. That partial update used 17
atlas draws and 17,825,792 uploaded atlas bytes. Artifacts are under
`build-fresh-validation/hydra-display-candidate/presentation`.

The default 256-spp background probe also passed, including visible-dome,
white/black hidden-dome viewport checks and actual FrameRecorder PNG/EXR exports
with transparent background alpha. Artifacts are under
`build-fresh-validation/hydra-display-candidate/background-default-lazy`.
The harness now forces the initial framebuffer/engine for hidden Qt windows and
imports `UsdAppUtils` immediately before recorder creation. Importing it during
testusdview's initial script loading caused a plugin-load failure in this
environment; the same plugin bundle passed after the import was deferred.

The existing usdview smoke and motion bodies passed against the same fresh
binary, using a wrapper to initialize the hidden viewport before their normal
steps. Smoke covered picking/selection, camera preview, time/geometry/material/
visibility changes, superseded texture preparation, AOV switching, resize and
renderer recreation. All 48 held-button truck/orbit motion events updated the
image and camera without rebuilding geometry, then converged to 16 spp after
release. Evidence is under `build-fresh-validation/hydra-integration-ui`,
including `smoke-motion-results.json`, `smoke/interaction-timings.json` and
`motion/motion.json`. Those interaction timings had concurrent validation load
and are diagnostic only.

The fresh-process structural probe passed all 76 operations across 16, 128,
1,024 and 16,384 placements. At every size a single-source position edit visited
and updated exactly one placement. Count-add attached one placement without
allocating a new TLAS, rebuilding prototype geometry or incrementing the
placement-topology fallback counter. Count-remove can reuse a bank that already
has the requested count and therefore report zero detaches. Picking and
structural correctness checks passed. Use
`build-fresh-validation/hydra-integration-structural/fresh-run/results.json`
and `results_summary.json`; the parent directory contains an unsuccessful
launcher attempt.

These checks establish correctness and reduced work, not a general timing
speedup. Whole-array instance comparisons remain linear: the one-source edit
reported 33, 257, 2,049 and 32,769 compared elements respectively. Hidden geometry
has a soft budget over unique converted geometry storage; retained frames may
pin storage above the limit. The default `hiddenGeometryBudgetMiB = 0` is
unlimited. GPU upload counters describe the optimized Float32 RGBA/depth path;
other presentation formats use the stock fallback.

### Repeated viewport comparison

Two alternating baseline/candidate pairs at each resolution completed without
probe errors on the instanced Kitchen scene. Each fresh process used the same
12-second observation interval, with hidden-window initialization outside that
interval. Other agent build/test jobs were idle; this remained an interactive
machine with uncontrolled background load. Per-run data and phase counters are
in `build-fresh-validation/hydra-display-candidate/alternating-benchmarks/summary.json`.
Each pair of values below represents the two runs, not an average.

| Resolution / measurement | Baseline runs | Candidate runs |
| --- | --- | --- |
| 1080p p95 paint time | 140 / 78 ms | 16 / 31 ms |
| 1080p first completed sample | 0.469 / 0.641 s | 0.796 / 0.937 s |
| 1080p completed samples in 12 s | 12 / 18 | 19 / 13 |
| 4K p95 paint time | 134.6 / 131.4 ms | 31 / 18.25 ms |
| 4K first completed sample | 3.031 / 2.953 s | 3.797 / 3.500 s |
| 4K completed samples in 12 s | 5 / 5 | 3 / 4 |

The candidate reduced geometric staging from 44 to 16 bytes per pixel for
usdview's bound AOV set: 91,238,400 to 33,177,600 bytes at 1080p and 364,953,600
to 132,710,400 bytes at 4K, a 63.6% reduction. Candidate full-frame CPU
presentation materialization remained zero; tile generation and atlas packing
still copy pixel data. Reported zero median paint times mean below the harness
timer's approximately 15-16 ms resolution, not literally zero work.

These runs show shorter slow redraws and lower staging memory, with a
first-sample regression at both resolutions and lower 4K sample throughput.
At 4K, the first trace scope rose from 2,294 / 2,311 ms to 3,193 / 2,989 ms,
and commit rose from 147 / 135 ms to 209 / 170 ms. The trace scope includes tile
wave scheduling, tracing, payload construction and partial publication, so it
does not isolate the cause. Profiling those phases is the next performance
priority; these results do not support a general rendering speedup claim.

## Baseline

Before the current fixes, the configured `prism-hydra` build passed the existing
25-test CTest suite. The four 256-spp standalone/Hydra parity baseline renders
were preserved under `build/hydra-delegate-validation` before the plugin was
rebuilt. Those baseline images are the comparison point for subsequent image
changes; they are not new acceptance results for individual fixes.

## Completed

| Item | Change | Evidence |
| --- | --- | --- |
| A1 | Reused the existing scene snapshot for frame changes, including xform-only animation, while preserving exact output against fresh renders. | Commit `309d505`. The `--animation-exercise` regression compares frames 1 and 2 across xform, points, visibility, material, and dome animation. The mixed and xform-only fixtures matched fresh renders exactly. In the xform-only case, `snapshotBuilds` and `conservativeRebuilds` stayed at 1 and 3 (baseline: 1 to 2 and 4 to 5), while placement invalidations increased by one. |
| A2 | Made explicit `Restart` reset progressive sample accumulation without rebuilding the scene snapshot. | Commit `43956d5`. `SceneBridge::RestartAccumulation` resets sample/convergence state only, and `Restart` uses it. `prism_hydra_scene_bridge_test` passed, verifying samples reset and then, after prepare/rerender, `snapshotBuilds`, `preparationGeneration`, and `displayedGeneration` stayed unchanged. |
| A3 | Continued committed progressive samples when the sample budget rises, while rejecting interrupted work after a budget reduction and restarting cleanly after buffer reallocation. | Implementation commit `bdc467f`; independent integration coverage commit `c892ed3`. `prism_hydra_scene_bridge_test` verifies 64-to-128 samples exactly match an uninterrupted 128-sample render, interrupted work resumed at its committed target matches a fresh bridge, multiple color bindings share coherent frames, and resize and bound-buffer replacement restart at sample zero. The expanded focused test passed 1/1 in 3.98 seconds with the accumulation helper success marker. |
| A6 | Batched all dirty locators from one Hydra observer callback into one generation and grouped invalidations by kind. | Commit `92bf364`. `prism_hydra_scene_bridge_test` exercises a real retained-scene observer callback, confirms one generation for a mixed callback, no generation for an empty callback, and global placement dirtiness updates transforms and restores geometry. |
| B1 | Added `HdSiExtComputationPrimvarPruningSceneIndex` between implicit-surface conversion and the Prism transport filter. CPU computed primvars are evaluated when the mesh points value is read. | Commit `f80a3bc`. `prism_hydra_ext_computation_test` passed for a trivial CPU callback and a stock USD Skel scene-index fixture. The skinned fixture at `tests/hydra/fixtures/skinned.usda` rendered at 64x64, 8 spp; its independently authored deformed-mesh reference matched with RGB RMSE 0 and luminance delta 0. The render reported one prototype and one triangle. |
| B2 | Added double-precision Hydra points, normals and UV ingestion, preserving supported VtVec3dArray/VtVec2dArray inputs through conversion. | Commit `d1ae671`. The registered `RunB2DoublePrimvarIntegrationTests` helper in `prism_hydra_scene_bridge_test` compares double-primvar and float-reference renders exactly, including a non-axis normal. UV conversion is implemented and separate UV pixel assertions are covered by C2. |
| B3 | Honored constant `displayOpacity` on mesh and sphere fallback materials, including when displayColor is absent. Authored, successfully resolved materials keep precedence. | Commit `6859bfb`. The fallback helper unit test passed 1/1 (0.07 s); focused B2/B3/C2 bridge integration passed 1/1 (2.58 s), covering opaque/transparent/fractional mesh fallback, no displayColor, spheres, edits to existing mesh/sphere prototypes, and authored-material precedence. Defaults preserve the opaque fallback path. |
| C2 | Added eye-space position (`Peye`) and interpolated `primvars:st` AOVs using the deterministic center-ray hit; retained `hit.u/v` behavior when `st` is absent. | Commit `ccfeb53`. The focused B2/B3/C2 `prism_hydra_scene_bridge_test` passed 1/1 (2.58 s), covering preview and full-resolution layouts, analytical eye-space coordinates, and double-precision UV interpolation. `Pworld` remains deferred because it is not a stock token in this OpenUSD build. |
| B4 | Routed material conversion, resource, pre-warm, and MDL compile errors through Tf warning diagnostics so Hydra hosts can receive them. Routine configuration and material-template debug output stays on stderr. | Commit `2ceab8f`. `prism_hydra_materials_test` passed. The test exercises material conversion and compilation but does not independently capture each warning text. |
| C5 | Used supplied projection and data-window dimensions to construct cropped camera rays without applying frame offsets twice; applied linear exposure compensation to RGB while leaving alpha unchanged. | Commit `ed02683`. `prism_hydra_pass_camera_test` passed; a cropped fixture matches its full-frame region exactly, invalid AOV dimensions remain unconverged and recover, and +2 stops scales RGB by exactly four while preserving alpha. Extra clip planes remain deferred. |
| C3 | Added signed/unsigned 16-bit and signed 32-bit scalar/vector render-buffer formats, bounded conversion and correctly sized typed clears. Unsupported packed float32/uint8 allocations now warn and fail. `Resolve()` remains a no-op because pixels are stored as a directly readable resolved image. | Commit `12c812e`. `prism_hydra_render_buffer_test` passed integer formats, exact Int32 IDs including 16,777,217 and extrema, clears, preview publication, NaN/Inf handling, unsupported packed format rejection, and multiSampled allocation/Resolve/Map. The separate Hydra presentation test is below; no usdview/GL compositor multisample test was run. |
| A4/A5 locator mapping | Normalized root materialBindings edits on geomSubset prims to Material invalidations, and geomSubset topology edits (indices/elementType) to Prototype invalidations, for both subset and parent. extent/displayStyle-only edits are ignored; an empty locator stays conservative. Added a face-subset rebinding render regression. | Commit `80c3367` with corrected locator mapping. `prism_hydra_scene_bridge_test` passed 1/1. The image regression rebinds a red face to blue and verifies the mesh has blue faces and no remaining red. Purpose/render-tag visibility behavior is covered by D3 below. |
| A7 | The render worker sleeps for 2 ms while paused instead of continuously yielding, while still checking promptly for stop requests. | Commit `11296e2`. `prism_hydra_scene_bridge_test` passed 1/1. |
| A8 | Cached authored render-settings paths and their original traversal precedence; ordinary settings synchronization no longer walks all scene-index children. | Commit `a678a03`. The test preserves nonlexical reverse-child precedence and checks 3 `GetPrim` calls with zero `GetChildPrimPaths` calls while syncing settings among 120 prims. `prism_hydra_scene_bridge_test` passed 1/1. |
| C1 | Added `domeLightCameraVisibility`; hidden-dome primary misses are transparent while dome illumination of surfaces remains unchanged. | Commit `84e6089`. `prism_hydra_(scene_bridge|primary_coverage)_test` passed 2/2 (2.68 s). Coverage includes default visibility, hidden dome preview/full misses, unchanged surface RGB, and cutout coverage. Separate 256-spp runs were deterministic and exact against each renderer's preserved baseline; direct standalone/Hydra comparison had normalized RGB RMSE 2.16302e-8 and relative mean luminance error 4.56409e-9, not bit-exact. |
| C4 | Published an immutable picking scene with each frame and used that frame's camera/scene for full-resolution picks. | Commit `dc54def`. Bounds and scene bridge tests passed 2/2 (2.50 s); bridge integration checks AOV/pick agreement, retained frame/map stability across edits and destruction, plus 3 released-frame edit cycles with zero pinned-bank fallbacks. Final usdview preview-pick validation passed all four tiny-target pixels at 16x16 preview with stable scene/camera epochs and no mismatches; artifact: `build/hydra-delegate-validation/final/usdview-preview-pick/preview-pick.txt`. |
| D4 | Reused semantically equivalent picking tables and invalidated them when placement identity/order/topology changes. | Commit `f8dcf3f`. Focused bridge test passed 1/1 (2.31 s), covering equivalent refreshes, transforms, changed same-count mappings, ordinary uninstanced add/remove occupancy, and published table identity. |
| D6 | Cached snapshot geometry and placement diagnostics instead of recomputing them on statistics reads. | Commit `f53947c`. Focused bridge test passed 1/1. `RunCachedCountTests` covers unique-storage mesh/triangle totals, topology replacement, placement-count-only changes, and structural remove/re-add; live prim count is separate from prim-path slots. |
| D3 | Retained disabled placements and their geometry/material dependencies for hidden and purpose-filtered prototypes. Visibility, purpose and render-tag changes now toggle placement eligibility without converting geometry; hidden batch-eligible spheres use ordinary disabled placements. | Commit `750961b`. Focused scene-bridge validation passed with retained visibility/purpose/instance coverage. Tests exercise initially hidden and toggled spheres, hidden geometry/material edits, ancestor visibility, purpose filtering, and render-tag edits. On Kitchen, mesh/triangle counts and active geometry bytes matched baseline (1,440 meshes, 439,069 triangles, 43,303,116 bytes); D3 reported 1,788 enabled placements and 2 top-level scene allocations. The memory samples do not quantify hidden-placement overhead; see the limitation below. |
| D2 | Retained the top-level scene for dependency-local prototype and placement updates. Stable placement keys use instancer paths, source indices and duplicate occurrence; changed instance counts and keys take named conservative fallbacks. | Core commit `0fec96c`; structural harness commit `27d22f0`. Focused bridge CTest passed 1/1 (2.96 s). Final `testusdview` probes at 16/128/1,024 placements each visited N records for a one-position edit but updated one transform, without new prototype geometry or TLAS allocation; changed banks committed once. Prototype point edits reported 2 updated paths and N+1 repointed placements, with no full snapshot or top-level allocation. Count add/remove reported `placement count changed`, preserved mesh/triangle totals, and advanced the topology fallback counter. |
| C3 presentation | Verified host-requested multisampled buffer presentation through allocation, rendering, `Resolve()` and mapped pixels. | Commit `f6abdc7`. The Hydra integration test renders a dome-lit mesh with both multiSampled flags, checks a hit ID and nonzero RGB, and compares resolved pixels exactly. It does not exercise a host compositor or usdview display. |
| D5 | Reused local environment importance distributions across scene banks and dome rotations when source image and multiplier are unchanged. | Commit `a6c19e4`. Cache and bridge tests passed 2/2 (2.97 s), covering exact wrapper math/RNG, cache bytes, rotation reuse, multiplier/resource invalidation; build counts were 1 initially/for spare and rotation, then 2 for multiplier and 3 for changed resource. Final Hydra 256-spp filtered-bilinear parity was exact (RMSE 0). |
| B3/C2 test registration | Registered fallback-opacity and geometric-AOV integration helpers in the bridge test target, plus the fallback helper unit target. | Commits `17b8639` and `d2f34bb`; registration verified in `CMakeLists.txt`. |
| D1 | Added persistent canonical prim slots and incremental reconciliation for eligible typed leaf mesh and ordinary sphere additions/removals. Removed paths remain tombstones; re-adding the same path revives its slot. | Commit `f6669f8`; final harness commit `37d5259`. Focused `prism_hydra_scene_bridge_test` passed 1/1 in 3.98 s. The full structural matrix below checks nine fresh-process scenes. |

## Follow-up viewport background regression

Commit `1baa527` keeps hidden-dome primary misses transparent in the raw color
AOV and composites usdview's clear color into a separate presentation image.
The real-host regression passed against the rebuilt plugin: dome visibility on
showed the environment; visibility off showed usdview's selected white and
black clear colors. OpenUSD FrameRecorder also wrote PNG and EXR exports with
dome visibility off. The alpha checker read both saved files and verified all
four image corners remained transparent while center geometry remained opaque.
Artifacts and logs are under `build/hydra-background-final`.

The pre-fix host probe confirmed the reported issue: the visible-dome check
passed, while hidden-dome corners stayed `(0, 0, 0)` with a white host clear
color. The final test checks the actual usdview framebuffer and saved export
files; the earlier observation is diagnostic only and is not a timing
comparison.

## Progressive viewport refinement

Float32 RGBA viewport presentation can publish immutable partial-row updates
during full-resolution refinement. The updates are display-only; raw AOVs,
accumulation and RNG state remain tied to complete passes, and canceled or
replaced generations discard their partial image. Other AOV formats retain
whole-pass presentation.

The final observer-corrected 4K Kitchen run is
`build/hydra-refinement-optimized-kitchen-4k-r4/cadence.json`. It measured a
1.578 s first full pass and a 0.516 s first sample-counter advance. The first
pass trace was 1,211 ms and its commit was 78.4 ms. Across 12 s, the framebuffer
CRC changed 65 times; 56 changes occurred while the completed-sample count was
unchanged. The median CRC-change gap was 172 ms and maximum 250 ms. There were
153 actual paints (median 47 ms, maximum 109 ms); the maximum paint gap was
187 ms. Compact primary-hit scratch storage was 364,953,600 bytes. Focused
integration tests passed 2/2 in 5.41 s.

The pre-change evidence is one run only: first full pass 8.094 s and five CRC
changes. It has no saved pre-change DLL, so it does not support a repeated-run
or speedup comparison. Earlier r2/r3 apparent gaps included PNG encoding in
the timed interval; r4 moved image saving outside that interval, so those gaps
are not reported as renderer stalls. The full 33-test CTest suite passed in
55.80 s (`build/hydra-progressive-validation/ctest-full.log`). Both filtered
and bilinear 256-spp validations passed. Each of the standalone and Hydra
images matched its preserved baseline exactly in both filters; all four
comparisons reported zero RMSE and zero luminance error. Logs are
`validate-filtered.log`, `validate-bilinear.log`, and
`exact-{standalone,hydra}-{filtered,bilinear}.log` in
`build/hydra-progressive-validation`.

## Focused validation checkpoint

The expanded `prism_hydra_scene_bridge_test` passed 1/1 in 3.98 seconds against
a freshly built core, with both the D1 structural-update and A3 accumulation
integration success markers present in the log. Their commits and test coverage
are recorded above. This focused run complements, and is separate from, the final
full CTest and usdview acceptance run below.

The build command for the targeted material and render-buffer tests was:

```powershell
cmake --build --preset prism-hydra --target prism_hydra_render_buffer_test prism_hydra_materials_test
powershell -ExecutionPolicy Bypass -File build/with_runtime.ps1 ctest --test-dir build -C RelWithDebInfo --output-on-failure -R 'prism_hydra_(render_buffer|materials)_test'
```

The targeted CTest run completed 2/2 successfully. B1's focused test was added
after the original baseline and is separate from that two-test run. Later
focused runs are item-specific and do not replace a final full-suite run.

## Final broad and structural validation

The final runner at `build/hydra-delegate-validation/final` used a clean
`prism-hydra` build. All 32 configured CTest tests passed (62.50 s). Both
filtered and bilinear 256-spp validation runs passed their deterministic,
standalone/Hydra parity, and PreviewSurface/MaterialX checks. All four rendered
images also matched their preserved per-renderer baselines exactly. The usdview
smoke, motion, and preview-pick checks passed; the preview-pick artifact records
zero mismatches. The aggregate final runner completed every step successfully,
and renderer binary hashes were unchanged after the build. The loaded Prism
plugin SHA-256 was `595fc222df2148af4089b04b84d56ec73c0ab7f54b796e086f4cba47105fa16e`.

The D1-D3 structural matrix passed all nine one-repeat fresh-process cases:
assignment counts 16/128/1,024, placement counts 16/128/1,024, and fixed-triangle
partition counts 1/16/128. Each process used an empty renderer cache. The OS
file cache was not flushed; this run validates behavior and work counters, not
cold-cache timings or speedup claims. Artifacts are under
`build/hydra-delegate-validation/structural-final-d1-d3-rerun1`; raw results
and summary are `results.json` and `results_summary.json`. The runner confirmed
nine cases, nine successful probes, zero failed runs and zero malformed runs.
Harness commits were `27d22f0` (D2 placement/topology probes) and `37d5259` (D1 leaf add/remove checks).

| Structural leaf probe (all nine cases) | Live prim delta | Leaf updates | Fallback delta | Paths updated | Attached | Detached | Captured prims | TLAS allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Add first control mesh | +1 | +1 | 0 | 1 | 1 | 0 | 1 | 0 |
| Add second control mesh | +1 | +1 | 0 | 1 | 1 | 0 | 1 | 0 |
| Remove first control mesh | -1 | +1 | 0 | 2 | 0 | 1 | 0 | 0 |
| Remove second control mesh | -1 | +1 | 0 | 2 | 0 | 2 | 0 | 0 |

The path/attach/detach gauges may include outstanding changes replayed into a
bank, so remove rows can report more than one affected path or detached placement.
Mesh/triangle totals count unique geometry and can stay unchanged when a new prim
reuses an existing local prototype; `primCount` is the exact live-prim assertion.
Placement probes at 16/128/1,024 instances visited N records for a one-position
edit, updated one transform, allocated no new TLAS and rebuilt no prototype
geometry; each mutated bank committed its changed TLAS. Count add/remove used
the named `placement count changed` fallback and preserved mesh/triangle totals.
The prototype point edit repointed affected paths and placements without a full
snapshot rebuild or new TLAS allocation.

## Remaining scope and measurement limits

Extra camera clip planes remain unimplemented. Fallback displayColor interpolation
for per-vertex and face-varying data remains unsupported; constant displayColor
and constant displayOpacity are supported as documented in `hydra_materials.md`.
Environment importance tables are keyed by prepared texture identity and exact
RGB multiplier; reusing a table across intensity/multiplier-only changes remains
deferred. Multisampled buffer tests cover allocation, rendering, `Resolve()` and
mapped pixels, but do not validate presentation through a usdview or other host
GL compositor.

The Kitchen comparison remains one fresh usdview process per binary with two
renderer-creation samples and an empty renderer cache; the OS cache was not
flushed, and baseline and D3 used different plugin binaries. Baseline did not
report `topLevelSceneBuilds`; D3 reported two allocations. Active geometry bytes
matched at 43,303,116 and D3 had 1,788 enabled placements, equal to its instance
count, so the scene did not exercise disabled-placement retention. D3 working set
was about 6.8 MiB lower while peak working set was about 44-48 MiB higher. These
opposing process-memory measures are a loading smoke and scene-count check, not a
memory-overhead estimate for hidden geometry.
