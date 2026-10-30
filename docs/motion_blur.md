# Motion blur plan (Phase 8b)

Status: planned, not started. Decisions accepted by João on 2026-10-30; the work
starts after the performance work (docs/PLAN.md). The stages below are to be
regrouped into fewer, larger merges before starting. File references are as of `7e80fb7`.

## Starting point

- The SDK reserves the shapes (`docs/sdk_design.md` section 5):
  - `Sampled<T>` is defined in `include/prism/types.h:99-115`.
  - It is used for `MeshDesc::points` and `SpheresDesc::centers`, for the
    instance, light and environment transforms, for
    `CameraDesc::cameraToWorld`, and by `updateMeshPoints` and
    `setTransform`.
  - `MeshDesc::velocities` and `accelerations` are stored but unused.
  - Other reserved settings: `SceneOptions::timeCodesPerSecond`,
    `RenderSettings::shutter` (`include/prism/session.h:70,108`) and
    `Capabilities::motionBlur = false` (`include/prism/renderer.h:33`).
  - Python exposes all of them (`python/src/module.cpp`).
- The core collapses samples with `Nearest()` (`src/scene/scene.cpp:155`). It
  picks the sample nearest time 0, not the one nearest the shutter centre.
  The camera has its own copy in `src/camera/camera_projection.cpp:58`.
- Gaps against the design:
  - `CameraDesc` has no `shutter`.
  - The "one warning per scene" exists only in Hydra
    (`scene_bridge.cpp:1407`, once per path).
- Hydra:
  - It reads every data source at time 0.
  - `plugin.cpp:43-44` declares no motion blur support (scene and camera).
  - `CameraFromPass` (`src/hydra/pass_camera.h`) reads no shutter.
  - `disableMotionBlur` is ignored (`render_products.cpp:285`).
- CLI: `src/mesh/usd_mesh.h` reads transforms, points, skinning and
  PointInstancers at the single `--time`.
- Integrator:
  - `Ray` (`shaders/integrator/types.h:145`) has no time.
  - `PathRay` wraps an `RTCRay` whose `time` stays 0.
  - MDL `state.animation_time` is fixed at 0 (`src/mdl_material.h:708`,
    `shaders/gpu/gpu_backend.hlsl:544`).
- Vulkan:
  - The BLAS and TLAS are built once (`vulkan_device.cpp:~2935-3100`, no
    `ALLOW_UPDATE`).
  - Instance matrices come from Embree at time 0 (`gpu_scene.cpp:443`).

## Design

- **Time per sample.**
  - Each sample draws `t = lerp(open, close, u)` once and keeps it for the
    whole path.
  - `u` comes from a separate Owen-scrambled Sobol stream (a salted seed). It
    does not advance the sampler's `dimension`.
  - It is drawn only when the committed scene or camera moves within an open
    shutter, so motion-off images stay bit-identical.
- **Where the time goes.**
  - It lives in backend state, not in `Ray`: `CpuBackend::time` sets
    `RTCRay::time`, and on the GPU the frame carries it.
  - MDL `animation_time` gets the sample time.
- **Camera.**
  - `cameraToWorld` is interpolated on the host: per sample on the CPU, per
    pass on the GPU.
  - One shared helper does this, next to `camera_rays.h`.
  - `cameraSample` itself is unchanged.
- **Embree (CPU).**
  - Moving geometry and instances get `rtcSetGeometryTimeStepCount(N)`. Steps
    are uniform over the shutter, and authored samples are resampled onto
    them by bracketing.
  - Rotating transforms use `RTC_FORMAT_QUATERNION_DECOMPOSITION`, so
    rotations do not shrink.
  - Only moving geometry pays for motion BVHs.
- **Placements.**
  - A moving mesh is never flattened into world space; it becomes an Embree
    instance.
  - A `placement_motion` map gives `prepareHit` the matrix at `ray.time` for
    the normal and tangent frames.
  - Static placements keep today's path.
- **Deformation.**
  - Points, normals, tangents and sphere centres are stored per step, and
    `RayHit` interpolates the attributes.
  - Subdivision and displacement are refined once per step (the topology is
    the same).
- **Light table.** Moving or deforming emissive placements are left out of
    NEE and reached only by BSDF hits (unbiased, noisier).
  - Analytic lights and domes stay at one instant.
- **Vulkan: one instant per pass.**
  - Each pass gets its time from a scrambled radical inverse of the sample
    index.
  - Before each dispatch the host:
    - interpolates the camera;
    - writes the instance matrices and normal columns;
    - refits the TLAS (`ALLOW_UPDATE`, `MODE_UPDATE`, with a full rebuild
      every K passes);
    - for deformation, interpolates vertices on the CPU, uploads them and
      refits the BLAS.
  - Image-mode replicas are off when something moves.
  - Low sample counts show strobed copies, which converge to the CPU result.
- **Metal.** Renders one instant and reports `motionBlur = false` until its
  own stage.

## Stages

Each stage is one worktree and one merge, after the full serial ctest.

1. **Time plumbing (S, 2–3 days).**
   - Work:
     - `Sampled<>` resampling and bracketing helpers;
     - `CameraDesc::shutter` (an optional, also in Python);
     - the core's one warning per scene;
     - `Nearest` uses the shutter centre;
     - `sampleTime()`, `CpuBackend::time` and MDL `animation_time`.
   - Loaders: both read the shutter (UsdGeomCamera `shutter:open/close`,
     `HdCamera::GetShutterOpen/Close`) and time samples, still rendering one
     instant. Hydra honours `disableMotionBlur`.
   - Tests:
     - unit tests for resampling, stratification and velocity integration
       with `timeCodesPerSecond`;
     - `--exact` against the previous build on existing scenes.
2. **CPU camera and transform motion (M, 4–6 days).**
   - Work:
     - Embree time steps in `src/scene/revision_parts.h`;
     - `placement_motion` and interpolation in `prepareHit`;
     - moving meshes are not flattened;
     - moving emitters are excluded from the light table;
     - `capabilities().motionBlur` is true on the CPU;
     - Hydra's support flags are turned on;
     - the CLI reads through `UsdGeomXformable` time samples.
   - Tests:
     - an analytic moving quad (blur width D + w px, energy conserved);
     - a moving sphere (pixel value = covered fraction of the shutter);
     - camera motion equals the inverse world motion;
     - parity fixtures `motion_transform` and `motion_camera`;
     - `--exact` with motion off.
3. **CPU deformation and velocities (M–L, 5–8 days).**
   - Work:
     - per-step vertex, normal, tangent and sphere buffers;
     - `UsdGeomPointBased::ComputePointsAtTimes` (velocities, accelerations,
       `motion:blurScale`);
     - `UsdGeomPointInstancer::ComputeInstanceTransformsAtTimes` (angular
       velocities);
     - UsdSkel at each time;
     - refinement per step;
     - a memory report.
   - Tests:
     - velocities against the equivalent two-sample points;
     - parity fixtures `motion_deform`, `motion_velocity` and
       `motion_instancer`.
4. **Vulkan per-pass instant (L, 7–10 days).**
   - Work:
     - a time in `PassRequest` (`src/device/device.h:151`) and `GpuFrame`;
     - TLAS refit;
     - CPU-interpolated vertices with a BLAS refit;
     - replicas off when something moves.
   - First runs:
     - under `VK_LAYER_KHRONOS_validation` + GPU-AV on a single-quad scene;
     - one GPU process at a time.
   - Tests:
     - a CPU debug mode `PRISM_MOTION_TIME=pass` (the same estimator) against
       Vulkan at the noise floor;
     - CPU per-sample against Vulkan at high spp;
     - `*_vulkan` fixtures.
   - The Metal checklist entry (`docs/gpu_devices.md`) covers:
     - the `GpuFrame` layout;
     - `refitAccelerationStructure`;
     - replicas;
     - `animation_time`.
5. **Metal native motion (later).**

## Risks

- Memory and BVH build time grow with the step count. Only moving geometry
  pays.
- Deforming subdivision meshes are refined once per step, which is slow.
- Vulkan strobes at low sample counts.
- Hydra's behaviour once the motion support flags are on needs checking on
  USD 26.08 (a velocity-motion scene index may do part of the work).

## Decisions (accepted 2026-10-30)

1. Default shutter when none is authored: [0,0], USD's camera default (no
   blur unless asked for). The alternative is a Karma-style [-0.25, 0.25].
2. Motion is on whenever samples exist and the shutter is open.
   `disableMotionBlur` turns it off.
3. Interactive usdview blurs too (the same path; strobing settles within a few
   passes).
4. Velocities against time-sampled points: USD's rule (velocities win when the
   point counts match).
5. At most 8 Embree time steps; authored samples are resampled onto them.
6. Moving emissive meshes are excluded from NEE for now. Evaluating them at
   the sample time comes later.
7. Lights, the dome and camera attributes other than the transform stay at
   one instant for now.
8. GPU: one instant per pass, not NVIDIA's motion extension (NVIDIA-only).
9. The GPU time goes in a field appended to `GpuFrame` rather than the free
   `work.w` (clearer, and Metal shares the layout).
