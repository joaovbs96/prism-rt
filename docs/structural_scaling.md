# Structural scaling investigation

This is the baseline investigation before the material-initialization changes.
Its source findings and plugin hash describe that measured revision. See
[Material initialization](material_initialization.md) for the new architecture
and its validation status.

This investigation separates scene-size-dependent work from unavoidable work on
the edited data. It uses generated fixtures rather than optimizing particular
production scenes. Renderer behavior is unchanged; the additions are diagnostic
USD fixtures, a stock-usdview probe, and this report.

## Method

Test machine: Intel Core i7-14700K (28 logical processors), Windows, stock
OpenUSD 26.05, four Prism material workers. The unchanged plugin SHA-256 is
`5D2DC459F6F3C36C784B17C848C276C1129F2C92FA44703DE953BA75A1B48081`.

Vary material assignments, equivalent material prims, parameter variants,
authored shader structures, prototype placements, mesh partitioning at fixed
triangle count, and texture references/content independently where possible.
Each recorded run starts a new process and an empty renderer disk cache. The OS
file cache is not cleared. The viewport is 64 by 64 at one sample per pixel to
reduce rendering cost relative to preparation.

The final matrix keeps only the pseudo-root expanded in usdview's Prim Browser.
A timing wrapper around `_resetPrimView` measures its cost without suppressing
the callback. The original expanded-browser reproduction is reported separately.

Each process measures first Prism creation and same-process renderer recreation,
followed by transform, material, visibility and point edits. USD stage opening
and initial Storm creation precede the measured first-Prism interval. Structural
additions before and after a local material edit measure conservative rebuilding.
They cannot isolate the global material epoch: structural notices themselves
call `InvalidateAll()`, independently of the preceding material edit.

Elapsed times include host processing and final framebuffer presentation.
Process CPU time is reported separately. Statistics-query time is also recorded:
`GetStats()` scans geometry/picking metadata while holding a mutex, so frequent
polling is itself a scene-size-dependent cost. Counters that describe the last
snapshot or operation are gauges, not cumulative totals; their differences must
not be interpreted as work counts. Full before/after dictionaries are retained.
Conversion time includes resource resolution; worker intervals can overlap.
Phase fields are not an additive decomposition of total elapsed time.

## Findings established by source inspection

- Material preparation deduplicates requests by material path before conversion.
  Cross-path content identity is established after resource resolution,
  fingerprinting, MaterialX conversion and XML serialization. Native-code reuse
  therefore does not eliminate all per-material preparation.
- The generic Hydra conversion path calls `doc->importLibrary` before
  serialization (`materials.cpp:696-697`). Sharing the generator's immutable
  library does not remove this earlier per-document copying. The simple OpenPBR
  fast path avoids it; arbitrary supported networks still take the generic path.
- Material preparation is serial at the front of the **background** snapshot
  job, before its compiler pool runs. This increases snapshot-ready latency;
  it is not evidence that compilation runs on the viewport thread.
- A local material invalidation advances a process-wide cache generation.
  Unrelated **path-cache** entries become stale on later lookup. Their unchanged
  early-cache entries can still recover hits after network capture, dependency
  validation and a linear scan; early lookup does not compare that generation.
  Targeted refresh does not eagerly reconvert every unrelated material, and
  reconversion after a local edit is not inevitable.
- Geometry additions/removals and unknown notices explicitly invalidate all
  path/early material caches (`scene_bridge.cpp:2676-2679`), including unchanged
  networks. The compiled content cache remains available, but is reached after
  conversion; native-code reuse is not lost merely because these caches clear.
- Initial/structural snapshot construction builds both an active snapshot and
  a spare bank before publication. Prototype storage may be shared, but scene
  traversal, placement construction, top-level acceleration work and metadata
  are repeated. Several public timing fields describe only the active bank.
- A local prototype edit converts the affected prototype, then rebuilds the
  complete top-level Embree scene and placement metadata. A low prototype-build
  count does not establish local total work.
- Instancer updates regenerate and sort all placements for affected prototypes.
  Root-transform edits exercise that path. The measured point-position edit
  instead emits `primvars` dirtiness, classified as Prototype: it reaches
  `_RefreshPrototypes` and full `_UpdatePlacements`, with no new prototype
  storage required. `lastPlacementRecordsVisited == 0` on that path does not
  mean no placements were processed. Updating one position in a large point
  instancer is not an O(1) placement update in the current implementation.
- Every successful generation rebuilds picking metadata, including material-only
  changes. These immutable tables provide correct ownership but currently lack
  independent reuse when identity/topology is unchanged.
- Dome construction creates a fresh `EnvironmentMap`, whose constructor scans
  all HDR texels and builds probability/alias arrays (`scene_bridge.cpp:1650-1691`,
  `environment.h:85`, `185-226`). Full builds do this for both banks; dome edits
  reconstruct environment state. Prepared texture sharing does not currently
  share those importance tables. This cost was source-audited, not included in
  the no-authored-dome matrix below.
- Native target-code caching is capped by entry count with arbitrary eviction;
  it is not a byte-bounded LRU. SDK allocations and shared resource ownership
  are not fully represented by the material-cache byte estimates.
- Publishing a new generated artifact enumerates/sorts its disk-cache contents
  (`generated_cache.cpp:257-288`). New texture resource publication likewise
  scans resource ownership and disk-cache entries under locks
  (`texture_service.cpp:560-568`, `122-171`). Cache maintenance can therefore
  grow with the cache, independently of the current scene's unique work.

These are mechanisms in the current code, not measured claims about the size
of their performance impact. The measurements below determine which are
visible at the tested scales.

## Reproduction and artifacts

From the configured repository runtime, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python tests/hydra/structural_scaling.py --repeats 3 --output build/structural-final --testusdview ../external_deps/OpenUSD/pxr/usdImaging/bin/testusdview/testusdview.py
powershell -NoProfile -ExecutionPolicy Bypass -File build/with_runtime.ps1 python tests/hydra/summarize_structural.py build/structural-final/results.json
```

Use a new output directory for a new run; cache directories deliberately refuse
reuse. `--axis` and `--value` select smaller reproductions. Full operation stats,
logs and presented frames are under `build/structural-final`; the report records
commands, cache paths and the plugin hash. No renderer source or binary was
changed during this investigation.

## Measured matrix

All 72 runs completed successfully: eight axes, three levels, three fresh
processes per level, each followed by same-process recreation and edit probes.
Validation checked all 801 operation records for material failures and every
run's initial prototype/triangle counts, resource bytes and native-target counts.
Regenerating all 24 USDA fixtures produced byte-identical files. Python syntax
and `git diff --check` passed.
Numbers below are medians in seconds. Within each cell, values correspond to
the three levels in order. Initial Storm/stage loading is excluded from the
first-Prism column; OS file cache was not flushed.

| Variable | Levels | First Prism image | Same-process recreation | New native targets |
|---|---|---|---|---|
| Mesh prims sharing one geometry and shader | 16 / 128 / 1024 | 2.828 / 2.916 / 3.882 | .063 / .163 / 1.204 | 1 / 1 / 1 |
| Equivalent material prims, fixed 128 meshes | 2 / 17 / 65 | 2.926 / 4.529 / 9.642 | .179 / .241 / .277 | 1 / 1 / 1 |
| Unique color values, same shader structure | 1 / 16 / 64 | 4.033 / 5.585 / 9.970 | .195 / .234 / .262 | 1 / 1 / 1 |
| Unique shader structures, fixed 128 meshes | 1 / 4 / 16 | 2.888 / 4.477 / 13.407 | .163 / .163 / .212 | 1 / 4 / 16 |
| Point-instancer placements, one prototype | 16 / 128 / 1024 | 2.627 / 2.726 / 2.873 | .050 / .059 / .048 | 1 / 1 / 1 |
| Mesh partitions, fixed 4096 triangles | 1 / 16 / 128 | 2.676 / 2.780 / 3.382 | .047 / .063 / .180 | 1 / 1 / 1 |
| Identical texture content, different file paths | 2 / 9 / 33 | 2.881 / 4.075 / 6.697 | .080 / .099 / .163 | 1 / 1 / 1 |
| Unique texture contents, same shader structure | 1 / 8 / 32 | 2.886 / 19.463 / 72.882 | .082 / .097 / .146 | 1 / 8 / 32 |

Material/file counts in the equivalent/alias rows include the extra edit-control
alias. Other rows count unique values/structures/contents or geometric elements.
The cold 65-equivalent-material runs ranged from 9.287 to 10.274 seconds; the
32-unique-texture runs ranged from 72.173 to 77.352 seconds.

The strongest measured mechanisms are:

- **Late material reuse.** Equivalent material counts 2 / 17 / 65 incurred
  .188 / 1.629 / 6.279 seconds of material conversion, despite exactly one
  compiled material and one native target throughout. The generic front end
  repeats expensive work per authored path.
- **Broad structural invalidation.** Adding one tiny control mesh to the
  65-equivalent-material fixture took 7.615 seconds and added 6.997 seconds of
  conversion, with zero new native targets. A local material parameter edit
  remained about .181 seconds. Structural notices, rather than all local edits,
  are the demonstrated broad invalidation problem.
- **Resource variants become programs under fallback.** The 32-unique-texture
  fixture resolved/read 24.75 MiB in a median .110 seconds; material conversion
  was 3.944 seconds, while summed native translation time was 61.371 seconds.
  First image took 72.882 seconds. Worker compilation wall intervals summed to
  260.899 seconds versus 67.766 seconds of worker CPU time. Those overlapping
  intervals must not be added to elapsed time; they expose substantial waiting.
- **Genuinely different graphs can compile concurrently.** The 16-structure
  case reached first image in 13.407 seconds with 16 native targets and 39.188
  seconds of worker CPU time. This contrasts with same-structure resource
  variants: four configured workers do not imply equal overlap for every path.
- **Existing reuse is real.** Sixty-four color values produced 64 material
  instances but one native target. Point-instancer levels retained one geometry
  prototype and 216 accounted bytes of its vertex/index/attribute storage.
  That number excludes placements, Embree, picking and container overhead.
  Ordinary single-object transforms remained roughly .05 seconds under this
  probe; small fixture latency does not prove asymptotically local work.

The fixed-triangle partition test showed modest costs at these scales, not a
large geometry bottleneck comparable to the material/resource findings.
Statistics polling added about .419 seconds over the 72.882-second texture
load; it should still be replaced with cached aggregate statistics before much
larger scene-scale profiling. The measurements do not identify render-buffer
locking as a leading loading cost.

## Direct texture-reference check

The existing texture-service fixture prepares 20 resources, retains them, then
requests each again. On the current binary it reported 40 preparation calls,
20 in-memory hits, and 30,966,430 source bytes hashed. The source files total
15,483,215 bytes: the cache-hit requests therefore still hash the complete
source set a second time. Two subsequent fresh-process checks preserved the
same deterministic output. Artifacts: `build/structural-texture-references`.

This establishes work per `TextureService::prepare` call. It does not imply
every material binding reaches that call; upstream material deduplication and
whole-target SDK fallback can alter the number of service requests.

Texture preparation also holds a cache-root-wide cross-process lock across
source inspection, decoding and mip generation. That serializes preparation of
different textures. Lock-wait time has not been directly instrumented here.

## Host-side structural-edit cost

The stock expanded Prim Browser took 55.20 seconds for the first structural
addition in the 1,024-object fixture. A 45-second Python stack trace located
the main thread in `appController._expandPrims`, called by
`_refreshPrimViewSelection -> _resetPrimView -> _resetGUI` while processing Qt
events. The operation eventually completed; this was not evidence of a renderer
deadlock. Artifacts: `build/structural-stall`.

Keeping the pseudo-root expanded but `/World` collapsed reduced the corresponding
structural operations to approximately 1.25 seconds in the confirmation run
(`build/structural-collapsed-check`). No USD source patch or callback bypass was
used. This single reproduction demonstrates strong host-state dependence; it
is separate from the three-repeat controlled renderer matrix below.

## Texture-runtime scope

The Standard Surface fixtures report `texture shape 5 at resource 1` and select
the SDK whole-target fallback even without an authored image. The custom runtime
accepts 2D resources (`texture_runtime.cpp:192-200`); backend fallback is at
`backend_compiler.cpp:453-483`. Consequently, the texture-alias and unique-texture
matrix measures asset resolution and SDK resource handling, **not custom OIIO
lookup performance**. The direct texture-service fixture above exercises that
service separately.

In the installed SDK, shape 5 is `Texture_shape_bsdf_data`: an internal 3D BSDF
table (`imdl_backend.h:821-828`), not an authored volume texture. This makes the
fallback relevant to ordinary surface materials. Detection happens **after**
the first `translate_link_unit`; fallback translates a second native target.
The `materialNativeJits` counter counts the resulting new target, not both
translation calls. `materialNativeJitSeconds` includes both intervals. Early
capability classification or supporting these internal tables would address
a pipeline cost rather than a particular asset.

Identical asset bytes are materialized under content-derived filenames before
the material content-cache lookup (`materials.cpp:441-481`), allowing alias
materials to share compiled content after paying resolution/read costs. With
SDK fallback, target-cache reuse must reject newly introduced resources it
cannot register (`backend_compiler.cpp:75-85`, `291-303`). Distinct texture values
can therefore demand new native targets even when their authored graph shape
is unchanged.

The target compiler holds a per-class striped mutex from cache lookup through
new translation (`backend_compiler.cpp:269-303`, `435-483`). If materials share
the class key but builtin resource remapping rejects reuse, their replacement
compilations stay serialized under that lock. This is appropriate coalescing
for identical work, but fallback makes resource variants require different work
inside the same serialized region. Separate lock-wait instrumentation is absent;
do not attribute all observed wall time to this mutex.

A separate verbose diagnostic confirmed the mechanism for eight unique textures:
eight identical class-cache keys, seven rejected argument-block reuse attempts,
and eight SDK fallbacks before the first Prism image. Artifacts:
`build/structural-resource-keytrace`. Its timing is excluded from the matrix.

## Structural repair directions (not implemented)

1. **Move identity and reuse ahead of conversion.** Build a canonical, compact
   network record from captured inputs. Separate shader structure, parameter
   values and resource identities; cache conversion/generation at those levels.
   Equivalent graphs at different USD paths should not each copy the library,
   serialize XML and resolve the same resources. Preserve authored overrides
   and resolver/version semantics rather than assuming all similar graphs match.
2. **Make runtime capability a preparation decision.** Investigate support for
   MDL's internal BSDF tables and classify known fallback requirements before
   expensive native translation where the SDK permits. Keep coherent fallback
   for genuinely unsupported resources. Do not silently approximate these tables.
3. **Give resources and materials independent revisions.** Ordinary material
   edits need per-material/dependency invalidation; geometry additions must not
   invalidate unrelated materials. Texture identity should be established once
   per source version, with explicit reload and resolver changes advancing that
   version. Content hashing remains necessary, but not on every retained hit.
4. **Publish independently reusable scene components.** Geometry storage,
   placements, material bindings and picking topology should have distinct
   ownership/version boundaries. A material change should retain acceleration
   and identity tables. A prototype edit should not reconstruct unrelated
   placements merely to publish a safe snapshot. Measure Embree commit work
   separately from application bookkeeping.
   Environment radiance transforms and immutable importance tables also deserve
   separate identity: changing dome rotation should not require rereading every
   HDR texel when the same local-space distribution can be reused.
5. **Use stable placement identity.** Point-instancer edits currently regenerate
   and sort all affected prototype placements. Stable source-index/instance-chain
   records permit changed-element updates when the USD notice and retained data
   provide sufficient information. Topology changes still require broader work.
6. **Make cache maintenance amortized and observable.** Avoid full directory
   enumeration on each publication, use byte-aware recency for native code, and
   measure real admission peaks. The current `Reservation::observe()` has no
   callers; its adaptive estimate is not currently exercised. Report SDK memory
   separately instead of treating accounted material bytes as total memory.

These changes should be accepted using work counts, not only scene timings:
equivalent networks convert once, unchanged resources are not rehashed, unrelated
material edits preserve cache hits, and local placement edits do not rewrite
unrelated records. Cold-process and same-process tests must remain separate.
More threads are useful only after duplicated work and serial preparation are
understood; they cannot remove those costs.

## Limits

- The suite is diagnostic, not a claim about million-object performance. It does
  not measure cache-capacity cliffs, filesystem cold-cache behavior, allocator
  stacks, or every SDK allocation.
- Two reusable scene banks and picking ownership are intentional safety
  mechanisms. The audit identifies repeated work around them, not permission
  to mutate published data or remove synchronization.
- The mesh-partition fixture holds 4,096 triangles constant. Its point edit
  touches progressively smaller partitions, so point-edit latency mixes changed
  geometry size and scene bookkeeping. The loading comparison is the cleaner
  fixed-geometry experiment.
- A structural control adds a small mesh bound to the edited material. Material
  edit fanout therefore includes that one extra user. The `expectedFanout`
  fixture field describes the original scene before control additions.
- Texture level N authors N palette resources plus an edit-control alias;
  unique-content count is N, while authored file count is N+1.
- Authored shader structures use live UV input and four ordered add/multiply
  nodes with non-neutral constants. Report actual compilation counts: authored
  structure count alone does not establish distinct optimized machine code.
- Fixtures have no authored dome; default host analytic lights are unsupported.
  They test preparation and frame publication, not lighting quality or rendering
  throughput. No image-parity or convergence claim follows from this suite.
- Stock Storm logs unsupported PPM texture-loading fallbacks in the texture
  fixtures. Prism's SDK material compilation succeeds. Consequently these are
  not Storm texture or end-to-end application startup comparisons.
- The probe yields for 10 ms between frames. Owner-thread capture is sliced at
  roughly 3 ms boundaries, so elapsed capture completion includes this scheduling
  cadence. Compare capture CPU/wall work counters separately from end-to-end
  first-image latency; neither is a navigation p95 measurement.
