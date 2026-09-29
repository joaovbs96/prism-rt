# Proposed next round: bounded interaction cost and large scenes

Historical proposal written before the shared-texture/asynchronous-preparation
implementation. Descriptions of "current" code below refer to that baseline.
See [round 3 validation](round3_validation.md) and
[texture service](texture_service.md) for implementation status, measured
results, and remaining acceptance gaps. Production file-backed textures now use
TextureSystem; resident arrays remain benchmark-only.

This is a proposal, not a claim that these capabilities are implemented. Keep
stock USD, the shared standalone/Hydra CPU renderer, and fully shaded published
snapshots. The objective is responsive interaction and work proportional to
what changed; no finite renderer can promise constant render time for arbitrary
numbers of unique meshes, shader graphs, textures, or lights.

## 1. Land the measured runtime improvement

Replace per-lookup ImageBuf access through a shared texture service. The
[resident-pixel comparison](render_cost.md) measured 5.663 s versus 0.3204 s
for the actual exported MDL scene, with exact images. The subsequent
[TextureSystem comparison](texture_access.md) also removes most accessor
overhead and is the preferred candidate for general texture caching/filtering;
retain resident arrays as a measured optional fast path, not the architecture
for all scene textures. Preserve filtering, PDFs, transforms, and RNG sequences.
Add ownership and reload tests, including non-identity dome transforms and image
data windows. Account for texture memory and avoid unnecessary duplicate pixel
buffers. Native MDL texture integration needs a separate backend audit.

Then reduce repeated MDL state initialization and compiler-proven constant
opacity/emission/thin-walled work per immutable argument block. Keep generic
fallbacks for textured or state-dependent expressions. Consider an opaque
occlusion path that avoids constructing full shaded hits. Cache camera ray
parameters once per pass; Hydra currently inverts camera/projection matrices
inside the per-ray helper.

## 2. Make scene preparation asynchronous and cancellable

`SceneBridge::Prepare` currently stops rendering, holds the bridge mutex, builds
or updates the snapshot, and renders the preview before returning. Parallel MDL
compilation still joins inside this synchronous path. It does not make the host
responsive during loading.

Capture immutable dirty inputs on the scene-index owner thread in bounded
chunks. Perform conversion, resource reads, compilation, and acceleration
construction on background workers. Use generation numbers to reject obsolete
results, coalesce edits, bound the job queue, and prioritize the latest camera.
Publish complete snapshots with a short swap. Keep the previous fully shaded
snapshot available during preparation; first load gets progress and cancellation
until the complete snapshot is ready. Do not introduce gray placeholder
materials. Preview work must run outside the scene mutex and host UI thread.

Give navigation a measured time budget and adapt preview resolution to it.
Keep preview accumulation separate, cancel at short work boundaries, and
increase quality after interaction stops. Expose preparation progress, stale
snapshot state, cancellation latency, and preview/refinement timing in stats.

## 3. Make edits local, including metadata and bindings

The current placement update creates a replacement top-level scene and loops
over every prototype/placement. The material update revisits all prototypes,
subsets, and placements. Preserving mesh buffers alone is not sufficient.

Maintain stable placement IDs and reverse dependency maps for transform
ancestors, instancers, material bindings, resources, and prototype geometry.
Update affected placements and binding references only. Batch compatible dirty
kinds instead of treating their combination as an automatic full rebuild.
Record every conservative fallback and its cause.

Never mutate Embree data referenced by active rendering. Evaluate inactive
scene versions with replayed dirty updates, or short quiescent commit windows,
depending on measured commit cost. Embree's acceleration commit may still do
global work; distinguish its cost from avoidable global traversal in our code.
Choose update/rebuild policy from measured traversal degradation and edit cost.

## 4. Make reuse and memory scale with actual uniqueness

- Reuse material graph structure before expensive MaterialX conversion and
  generation. Existing native class-code reuse already works; distinct color
  values should become cheap parameter-block instances of a shared template.
  Structural and resource edits need explicit fallback/invalidation rules.
- Remove per-material standard-library duplication. Budget concurrent jobs by
  memory as well as worker count; the standalone material collection currently
  shows a transient working set around 10 GB for this small scene.
- Add persistent caches for supported converted/generated artifacts, keyed by
  network structure, dependencies, generator/compiler options, and SDK versions.
  Use native-code persistence only where the SDK supports safe serialization.
- Bound in-memory caches by bytes, retain entries referenced by live snapshots,
  and evict unused records safely. Bound and clean generated/resource caches.
- Replace linear prototype searches with fingerprint buckets and collision
  verification. Prefer authored prototype identity when available. Current mesh
  conversion expands triangles and searches previous prototypes linearly.
- Share coarse-face ID arrays and material-subset binding tables on prototypes;
  placements should hold references, not copies. Deduplicate compatible vertex
  attribute tuples while preserving face-varying UV/normal discontinuities.
- Test packed sphere geometry for spherical placements; retain affine instances
  for ellipsoids and complex prototypes. Preserve source IDs, instance chains,
  local UV frames, picking, and update behavior. The benchmark's sphere batches
  are a prototype, not production-ready scene synchronization.

Add texture residency limits, shared resource storage, appropriate mip levels,
and later streaming policies. Avoid camera-frustum-only geometry removal:
offscreen objects can still cast shadows and appear in reflections/refraction.
USD payload loading and proxy/render purposes should be explicit quality and
working-set controls.

## 5. Improve usefulness and time to a clean image

Real production scenes need more than fast sphere/mesh traversal. Prioritize
USD rect/disk/sphere/distant lights and emissive-mesh next-event sampling with
correct MIS. Dark interiors cannot rely on chance BSDF hits to converge well.
Use a scalable light-selection distribution when light counts grow.

Then add prototype-cached subdivision refinement, curves/hair, points, more
material primvars/UV sets, and the texture workflows required by the chosen
asset corpus (including UDIMs). Motion blur and volumes should have explicit
later milestones. Report unsupported features and omitted prim counts; fast
but incomplete renders are not successful scalability results.

Evaluate adaptive sampling and an optional denoised display using reliable
albedo/normal guides. Keep raw linear output and correctness tests independent
of denoising; preserve deterministic reference mode and robust cancellation.

## Acceptance and execution order

Implement separate reviewable stages: shared HDR fast path; asynchronous host
integration; truly local edits; material/memory scaling; remaining shading and
geometry hot paths; production lighting and scene features.

Set provisional navigation targets on named hardware and a fixed asset corpus:
20-30 preview updates/s, p95 input-to-image latency below 50 ms, and short bounded
host-thread work slices. These are targets to validate, not guarantees for all
assets. Measure p99 and cancellation tails as well as medians.

Generate scaling fixtures varying one factor at a time: shared versus unique
mesh prototypes, placement counts, shared versus unique shader graphs,
parameter-only material variants, texture working set, and light counts. Test
single-object edits at increasing scene sizes. Counters must show unrelated
geometry/materials are not reconverted; memory measurements must include
metadata, Embree structures, texture pixels, SDK state, and retained snapshots.

Continue actual usdview navigation, editing, selection, AOV, resizing, renderer
switching, and repeated scene replacement tests. Keep image parity, deterministic
reruns, transformed instances, cutouts, and safe teardown as release gates.
Finish with clean-install and relocated-plugin tests against the one external
stock USD installation. Report cold preparation, warm reuse, render-only speed,
interaction latency, and time to a specified image error separately.
