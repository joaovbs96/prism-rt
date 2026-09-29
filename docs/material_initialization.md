# Material initialization

PrismRT separates native MDL programs from per-material argument blocks and
resource registries. Published bindings retain their programs. Shader calls
receive the argument block and resource handler from the same binding.

The native program cache is partitioned by SDK context, compiled class and
argument interface, derivative/callable ABI, and runtime requirements. Custom
runtime targets preserve the SDK resource-table prefix and append argument
textures to private binding registries. Builtin-handler targets additionally
include their resource signature in the key. Identical program requests share
one preparation job; unrelated resource variants do not hold a class-wide mutex.

The custom runtime copies and interns the SDK's internal BSDF tables. These are
numerical 3D tables, not image files or mipmapped textures. Unsupported resource
layouts retain whole-target SDK fallback. `PRISM_MDL_RESOURCE_MODE=custom` and
`PRISM_MDL_RESOURCE_MODE=builtin` select reference validation modes; the default
is `auto`. Forced custom mode reports unsupported targets as failures.

The SDK 2025.0.3 OpenPBR microflake sheen table reports `DFK_INVALID` while
providing a valid 32×32×1 `Float32<3>` BSDF-data texture. The runtime therefore
validates the supplied shape, dimensions, pixel layout, and payload instead of
requiring a named table-kind enum. It copies the supplied numerical values
without approximation. Custom/builtin tests exercise both Standard Surface and
OpenPBR evaluation, sampling, and PDFs. Full-JIT comparisons allow 0.05% relative
rounding tolerance between scalar callbacks and SDK-inlined lookup arithmetic;
deterministic reruns within a backend still require exact results.

Native MDL texture-result storage is explicitly aligned to 16 bytes, including
storage embedded in prepared shading contexts. A textured opacity graph exposed
the missing alignment: the native init function could fault when writing cached
`float4` results. The material test covers direct and prepared evaluation of this
graph. Class bindings are not evaluated against an incomplete zero shading
state during preparation; state-dependent properties are evaluated at the hit.

Material graphs are captured independently by USD ingestion and Hydra. Their
identity includes authored typed values, connections, terminals, node definition
information, material configuration, resource provenance, and resolver context.
Removing the enclosing material prim path allows otherwise equivalent graphs to
reuse conversion. Path associations are scoped to a scene; dirty paths invalidate
those shortcuts without flushing unrelated immutable graph or program records.

Template bindings use generator-published uniform names and validate the exact
MDL argument layout. Connected input defaults remain structural and are not
treated as runtime arguments. Numeric vectors and colors use distinct MDL
types. Unsupported mappings fall back to full preparation; string selectors,
primvar names, color interpretation, and graph topology remain structural.

Texture source fingerprints are memoized within the process. Prepared image
artifacts remain content-addressed on disk. Explicit reload advances the source
version, even if size and modification time are unchanged; old snapshots retain
their existing prepared resources. Metadata is not treated as proof of content
across fresh processes.

## Measurements

The `materialSdkTranslationCalls`, `materialConversionCalls`,
`materialGenerationCalls`, `materialBindingCreations`,
`materialFallbackTranslations`, and `materialResourceReadBytesCumulative`
Hydra statistics are renderer-lifetime work counters. Completed superseded jobs
are included. `materialProgramWaitSeconds` is cumulative waiting time across
workers, not elapsed loading time. Existing per-generation phase timings remain
separate. `processTexture*` statistics are shared-process counters, so other
renderer instances may contribute to them.

`processMaterialResourceReadCalls`, `processMaterialResourceHashCalls`, and
`processMaterialResourceBytesRead` account separately for resolver asset
materialization. Its trusted digest avoids another full-source hash in
TextureService. `processMaterialResourceWaitSeconds` measures cumulative
single-flight waiting, not elapsed loading time. These counters are also
process-wide.

`processPreparationAdmissionWaitSeconds` reports cumulative admission waiting
across jobs. `conversionCalls` counts logical material conversions, including
the existing simple OpenPBR conversion path; the shared captured-graph registry's
`stockConversions` counter counts only actual calls to USD's HdMtlx converter.
The resource registry retains bounded source/publication metadata (4,096
records); this round does not replace the existing eviction policy with a new
resource LRU or impose a hard process-memory limit.

SDK teardown requires preparation jobs to finish first. Material prefetch joins
its worker pools before returning; renderer destruction cancels and joins its
snapshot future before releasing scene/material ownership. The shared Hydra SDK
context lives for the process lifetime. Clearing a program cache alone is not a
mechanism for shutting down concurrent SDK work.

`tests/hydra/structural_scaling.py` uses stock testusdview, independent empty
renderer-cache directories, and a controlled Prim Browser state. It records OS
cache conditions explicitly. `tests/hydra/check_material_initialization.py`
checks work-count gates and reports three-run timing targets separately. These
loading fixtures are not lit image-parity fixtures. Use `tests/hydra/validate.py`
for independently ingested standalone/Hydra linear-image comparisons.

## Validation command checklist

These commands are a reproducible checklist. Completed results are summarized
in [material_initialization_results.md](material_initialization_results.md);
checks marked pending have not been run for the current source revision. They assume
the configured Windows build is in `build`, the runtime wrapper is
`build/with_runtime.ps1`, and OpenUSD's stock script is at
`../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py`.

Build the material, texture, and Hydra validation targets, then run their
registered CTests:

```powershell
cmake --build build --config RelWithDebInfo --target `
  prism_hydra_materials_test prism_hydra_selection_filter_test `
  prism_hydra_scene_bridge_test prism_material_generation_cache_test `
  prism_captured_material_graph_test prism_generated_mdl_cache_test `
  prism_standalone_material_reuse_test `
  prism_mdl_texture_runtime_test prism_mdl_texture_target_test `
  prism_texture_service_test prism_resolved_material_resource_test `
  prism_hydra_material_loading_test prism_hydra_render `
  prism_compare_images
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 ctest `
  --test-dir build -C RelWithDebInfo --output-on-failure -R `
  "prism_(hydra_render_exercise|hydra_materials_test|hydra_selection_filter_test|hydra_scene_bridge_test|material_generation_cache_test|captured_material_graph_test|generated_mdl_cache_test|generated_mdl_fresh_process_test|generated_mdl_concurrent_process_test|standalone_material_reuse_test|mdl_texture_runtime_test|mdl_texture_target_test|texture_service_test|resolved_material_resource_test|hydra_material_loading_test)"
```

The target names come from `CMakeLists.txt`; some are conditional on Hydra/MDL
dependencies enabled by the selected build. The integrated validation log
records 25/25 passing CTest cases (`build/material-init-final-ctest3.log`).
`prism_standalone_material_reuse_test` may report CTest skip code 77 when its runtime prerequisites are unavailable. `with_runtime.ps1`
loads the configured plugin resource directory into `PXR_PLUGINPATH_NAME`; the
commands below leave that variable to the wrapper instead of overriding it.

Run the unique-texture stress case (32 distinct 512×512 files, three fresh
testusdview processes) and inspect correctness gates and phase medians:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/structural_scaling.py --axis textures_unique --value 32 `
  --repeats 3 --output build/material-init-textures32 `
  --testusdview ../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/summarize_structural.py build/material-init-textures32/results.json `
  --output build/material-init-textures32/summary.json
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/check_material_initialization.py `
  build/material-init-textures32/results.json `
  --output build/material-init-textures32/check.json `
  --markdown build/material-init-textures32/check.md
```

For `textures_unique/32`, the checker requires one SDK translation, zero
fallback translations, and no material failures. Process resource reads,
hashes, and texture hashes must not increase during structural-only edits.
Completed three-run results are recorded in `build/material-init-final-textures32/`.

Run the complete structural matrix (eight axes and all configured levels,
three fresh processes per case), then summarize it:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/structural_scaling.py --repeats 3 `
  --output build/material-init-full-matrix `
  --testusdview ../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/summarize_structural.py build/material-init-full-matrix/results.json
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/check_material_initialization.py `
  build/material-init-full-matrix/results.json `
  --markdown build/material-init-full-matrix/check.md
```

For the equivalent-material control, run this second three-process case; the
fixture has 65 authored materials (the 64-material palette plus the local edit
control):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/structural_scaling.py --axis identical_materials --value 64 `
  --repeats 3 --output build/material-init-identical64 `
  --testusdview ../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py
```
The equivalent-material control and full matrix completed three repetitions;
their reports are in `build/material-init-final-identical64/` and
`build/material-init-final-matrix/`. Summary timings and limits are in the
linked results document.

Measure real-asset cold-prepared and warm-prepared behavior separately. Each
uses four workers, three fresh processes per scene, and navigation moves.
Fresh processes do not clear OS file caches. The grouped commands omit a focus
prim because the benchmark applies one focus path to every scene:

```powershell
$testusdview = "../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py"
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/benchmark_loading.py --testusdview $testusdview `
  --scene assets/models/OpenChessSet/chess_set.usda `
  --scene assets/models/Kitchen_set/Kitchen_set_instanced.usd `
  --scene assets/scenes/InOneWeekend.usda --workers 4 --repeats 3 `
  --prepared-cache empty --navigate `
  --output build/material-init-realassets-cold
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/benchmark_loading.py --testusdview $testusdview `
  --scene assets/models/OpenChessSet/chess_set.usda `
  --scene assets/models/Kitchen_set/Kitchen_set_instanced.usd `
  --scene assets/scenes/InOneWeekend.usda --workers 4 --repeats 3 `
  --prepared-cache reuse --navigate `
  --output build/material-init-realassets-warm
```

For an object-heavy Chess close-view pass, run Chess separately because that
prim does not exist in Kitchen or InOneWeekend:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/benchmark_loading.py --testusdview $testusdview `
  --scene assets/models/OpenChessSet/chess_set.usda --workers 4 --repeats 3 `
  --prepared-cache empty --navigate --focus-prim /ChessSet/Black/RookL `
  --output build/material-init-chess-focused-cold
```

**Pending:** all real-asset runs and cold/warm summaries.

Check filtered and bilinear rendering with matched fixture settings, then run
the usdview setting, picking, selection, and live-lifecycle smoke:

```powershell
$env:PRISM_CACHE_DIR = "build/material-init-parity-cache"
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/validate.py --spp 256 --texture-filtering filtered
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/validate.py --spp 256 --texture-filtering bilinear
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/hydra/validate_exports.py --spp 256 --inoneweekend-spp 1024 `
  --texture-filtering filtered
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  tests/benchmark_scalability.py `
  --executable build/RelWithDebInfo/prism_texture_cache_benchmark.exe `
  --output build/material-init-filtering-check --parity-only
$env:PRISM_VALIDATION_DIR = "build/material-init-usdview-smoke"
$env:PRISM_CACHE_DIR = "build/material-init-usdview-smoke/cache"
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python `
  ../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py `
  tests/hydra/fixtures/parity.usda --renderer HdPrismRendererPlugin `
  --testScript tests/hydra/usdview_smoke.py
```

Set `PRISM_CACHE_DIR` to a workspace-local directory before validation when
the environment denies access to the default AppData cache; that sandbox
restriction is not a production cache limitation. The PPM dome in this fixture
is unsupported by Storm's GPU texture path, so start this smoke with Prism. The
callback clears that texture when switching to Storm and restores it after
switching back. Structural loading benchmarks still start with Storm and
measure Prism creation separately from host startup.

Three Prism interaction runs, including observed pending texture supersession
and final-image checks, completed with p95 navigation latency of 31-32 ms. The
successful smoke runs used Prism. An earlier diagnostic attempt using Storm
failed on the fixture's PPM dome and is not counted as a passing run. Both
256-spp filtering modes completed. Custom/builtin reference comparison was
exact. Filtered export parity includes a recorded CornellBox 64-spp miss and
an exact 256-spp rerun that passed; see the results document for metrics.
`tools/validate_install.ps1 -SkipUsdview` is the relocated-install smoke;
without `-SkipUsdview` it also exercises the installed usdview lifecycle. It
cleans/configures/builds/tests/installs, so treat it as a separate integration
job with its own build prefix. **Pending:** relocated-install validation.

The checker reports failed processes separately from work-count failures and
attributes nonzero `materialFailures` to each operation phase. For
`textures_unique/32`, it checks `materialFallbackTranslations == 0` and
`materialSdkTranslationCalls == 1` on cold initialization. Structural edits
must have zero deltas for material conversion/generation/SDK translation,
materialized-resource reads/hashes, and texture hashes. Three successful
repeats are required before a target-bearing timing result is considered met
or missed; fewer are labeled `insufficient-repeats`. The callback emits
`structural-remove-control` and `structural-remove-after-material`; both match
the checker's `structural-*` zero-delta gate.
