# Hydra delegate improvement validation

This ledger records completed work from `HYDRA_DELEGATE_PLAN.md` and the evidence
collected for it. A plan item appears here only after its implementation and
validation have finished. The plan remains the source for work that is still
pending.

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
