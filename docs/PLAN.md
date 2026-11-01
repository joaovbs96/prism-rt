# PrismRT plan

The handover for the main (orchestrating) session. Subagents don't read this file; they get self-contained briefs (CLAUDE.md, "Role: orchestrator").

## Working agreement

- Work phase by phase, in the order below. One phase at a time unless a phase says it may run in parallel with another.
- For each phase:
  1. The main session writes a self-contained brief: goal, files, rules that apply (copied from CLAUDE.md), definition of done, return format.
  2. A subagent implements it in its own git worktree, with small local commits.
  3. The subagent writes the details to `local/reports/<phase>.md` and returns at most ~150 words.
  4. The main session spot-checks anything surprising, merges locally into `main`, rebuilds, and runs the full serial `ctest -j 1` once.
  5. The main session adds a short entry to `docs/PROGRESS.md` and any decision to `docs/DECISIONS.md`.
  6. **Stop for João's review.** Push only with his OK.
- Definition of done for any feature phase: both loaders (Hydra + CLI) with a parity fixture; CPU/Vulkan parity; Vulkan validation clean on new GPU paths; Metal checklist entry for shared code; docs updated; the full test suite green.
- On a failure: stop, diagnose from evidence, fix narrowly with a regression test, rerun the focused suite, resume. No speculative fixes.
- GPU safety rules in CLAUDE.md apply to every phase.

## Current state (2026-10-26)

- `main` is the development branch; GitHub's `main` = 686dd68 (everything through the CLI material path and the raygen prewarm is pushed).
- Steps 4–8 below are done (Phases 5, 6, 7 and their follow-ups); steps 2–3 are João's; step 1 waits for the repository going public. Next: Phase 8a, then the SDK.
- CI is off: GitHub Actions stopped on billing and João disabled the workflow. Revisit when the repository goes public (step 1). Until then, the local full serial `ctest -j 1` is the only gate.
- Completed so far: see `docs/PROGRESS.md`. Decisions: `docs/DECISIONS.md`. Longer-term feature list: `docs/feature_roadmap.md`.

## Steps

### 1. CI (deferred until the repository goes public)
- João disabled the Actions workflow (2026-10-25); re-enable it first.
- Public repositories get free Actions minutes; the billing stop (spending limit / payment) goes away or needs fixing then.
- Then: confirm the workflow runs on `main`, that the USD/MDL/vcpkg caches saved on `main` are reused, and delete the `device-abstraction` branch on GitHub (João's OK).
- Owner: João (billing), then the main session.

### 2. System PATH cleanup — João (needs an administrator)
- Remove three dead entries from the Machine `PATH`: `…\external_deps\OpenUSD\install\bin`, `…\external_deps\OpenUSD\install\lib`, `C:\MaterialX\bin`.

### 3. `C:\osl_install` — João
- Delete it if nothing else uses it (the sandbox can't delete folders directly under `C:\`). PrismRT doesn't use OSL.

### 4. Land the local commits
- Full serial `ctest -j 1` in the main checkout on the current local `main`/`device-abstraction` head.
- Ask João, then push to `main` (CI will not run until step 1).
- Afterwards, rename the local branch to `main` tracking `origin/main` (it is still called `device-abstraction` locally).

### 5. Cleanup (after step 4; regenerable data only)
- Remove the merged agent worktrees under `.claude/worktrees/` (26 at the time of writing, including one locked empty one) and their `worktree-agent-*` branches; keep only unmerged ones that still matter (list them first; the ALab fallback branch stays).
- Delete the scratch build folders `D:\prism-builds\*` and `D:\prism-build\*` (agent builds and baselines), and `D:\prism-deps\src` (the dependency source trees; `tools/build_deps_local.ps1` re-clones them).
- Review `local\` for obsolete folders (old backups, streaming caches, comparison renders João no longer needs) and ask before deleting anything not obviously scratch.

### 6. Phase 5 — textured emitters as lights
- Load time: stratified EDF sampling per emissive triangle through the CPU MDL backend gives an average radiance; selection weight = average × area, with a floor for any non-zero emitter; per-triangle pdfs in the light table (`src/device/light_table.h`).
- NEE: `chooseLight` evaluates the true EDF at the sampled point; MIS on BSDF hits uses the per-triangle pdf.
- GPU: the EDF callable from NEE (watch callable count and registers).
- Also closes: the CLI default-dome rule and `SceneHasEmitters` missing textured emitters; glTF/OBJ emissive textures.
- Verify: a checker-emitter fixture with a black region, convergence against a high-spp reference, CPU/Vulkan parity, the OpenPBR and Eggs emitters no longer biased dark.

### 7. Phase 6 — more AOVs and multi-output
- Albedo (first non-specular hit), diffuse and specular (per-lobe radiance, building on the Phase 4 diffuse/glossy split), world position.
- Per-lobe accumulation in the integrator, output planes in `PassRequest`, both devices, Hydra AOV descriptors, multi-part EXR in `src/output/image_writer`.
- Verify: diffuse + specular + emission ≈ colour (numeric test), CPU/Vulkan parity per AOV, no slowdown with AOVs off.
- Touches the integrator like Phase 5, so it runs after Phase 5, not in parallel.

### 8. Phase 7 — CLI parity
- Bring the standalone loader (`src/mesh/usd_mesh.h`, `src/standalone_cli.cpp`) to Hydra's level: subdivision and load-time displacement (this also makes OBJ `disp` take effect), PointInstancer and native instancing (+ per-instance primvars, invisibleIds), holes, leftHanded and negative-scale meshes, GeomSubsets, mesh purpose and visibility, `--time`, UsdSkel, camera exposure, id AOVs, the `emissive.usda` MaterialX material that fails to compile in the CLI.
- Extend the parity fixtures to every Hydra feature (one Vulkan variant under the GPU lock).
- Mostly CPU-side loader work: may run in parallel with Phase 5 or 6 using `ctest -LE gpu`, with GPU checks serialized.

### Next: Phase 8a — ShapingAPI spotlights and IES profiles
- Cone angle/softness, focus/focus tint and IES profiles as an emission factor wherever lights are sampled or hit (`shaders/integrator/lights.h`) and in the light importance; an IES parser (`src/lights/ies.{h,cpp}`) in both loaders; the IES table in the existing light-texture slot. Verified against an analytic cone and a known IES profile, CLI-vs-Hydra and CPU/Vulkan parity.
- Then the small fixes listed under "Later".

### Next: denoising (decided 2026-10-27)
- Intel Open Image Denoise (OIDN) on the CPU and Metal devices, nvpro-samples' vk_oidn (OIDN's network as Vulkan compute; Apache-2.0) on the Vulkan device; same network weights everywhere.
1. A denoiser interface on the render device; OIDN's CPU device behind it; a `denoised` output in Hydra and the CLI (`prism:denoise`, `--denoise`), usdview denoising every few passes / when idle; inputs from the existing `albedo` and normal AOVs. The raw colour stays the default.
2. vk_oidn on Vulkan: its device features, a resolve pass from our accumulation buffers to RGBA colour/albedo/normal buffers, `cmdDenoise` recorded in our command buffer; measure the NVIDIA cooperative-matrix path and the forced generic path (`VK_OIDN_COOPMAT=0`, what AMD/Intel get). May need a newer Vulkan SDK (vk_oidn was tested with 1.4.363).
3. OIDN's Metal device sharing our `MTLBuffer`s (written here, verified on João's Mac).
- Option if the generic Vulkan path is too slow on AMD/Intel: OIDN's own HIP/SYCL backends through Vulkan external memory.

### Then: USD conformance (decided 2026-10-27)
- Read the standard `UsdRenderSettings.renderingColorSpace` (`lin_rec709_scene`, `lin_ap1_scene`, `data`) in both loaders as the working space; precedence: CLI flag > `prism:colorManagement:workingSpace` > `renderingColorSpace` > default. The default stays ACEScg (Typhoon, the proposed OpenUSD reference renderer, defaults to `lin_rec709_scene`).
- Run PrismRT against Typhoon's image regression suite (https://github.com/anderslanglands/typhoon-test-suite): render every scene through Hydra on CPU, compare with its references (with `renderingColorSpace` honoured), and list the deviations as bugs, spec differences or deliberate choices.

### Then: renderer SDK and Python API (decided 2026-10-26; motion blur waits for it)
- (a) Turn the renderer into a library/SDK that the CLI and Hydra both implement: a context object (`prism::Renderer`/`Session`) owning today's process-wide state (colour management, the shared `MDLGenerator`, the texture service, SSS fallback, binding purposes, the MDL SDK); public headers separated from internals (no Embree/MDL/TBB/USD types in the public API); scene building (meshes, instances, lights, cameras, MaterialX materials) with incremental edits (Hydra needs them), rendering (devices, settings, progressive passes, AOVs, cancellation, progress), and loaders (USD, glTF, OBJ) as optional modules so the core doesn't need USD. No ABI promise until a first release.
- (b) A Python API with nanobind as a thin layer over that API: NumPy/DLPack zero-copy for mesh arrays and images, the GIL released during renders, cancellation through the progress callback, USD stages shared with `pxr` through `UsdUtils.StageCache` ids. Packaging (bundled DLLs, load order — cf. PySide6's old msvcp140) is its own task.
- Order: a design-only investigation first (`docs/sdk_design.md`: API sketch, global-state inventory, header split, module boundaries); then the core API with the CLI ported onto it (its parity tests are the safety net); then Hydra; then the Python bindings. (b) only after (a).
- Design done (`docs/sdk_design.md`, migration steps in its section 7); João's answers are in `docs/DECISIONS.md` (2026-10-27). Additions to the steps: a Linux build early (Windows, Linux, macOS from the start), a built-in MaterialX preview scene, a private namespaced USD build for the wheel (option A), the Metal shader converter's licence as an open packaging item. Target version for the first SDK/Python release: 0.2.0.
- Now: steps 1a–1e (make global state explicit: context object, colour management, options, runtime caches, progress/logging), one phase, one commit series per step.
- Reserve motion in the design even though motion blur comes later: time-sampled transforms and attributes (or velocity/acceleration) in the scene-building API, a shutter interval in the render settings, a per-sample time in the render path (`Ray.time` exists), so motion blur doesn't break the API or the Python bindings.

### Before motion blur: three smaller fixes (decided 2026-10-31)
The performance work (batches 1–3 and the render profiler) is done and pushed. Motion blur waits until these are sorted out:
1. Hydra scene capture costs about 1.8 ms per prim (30 s at 16k prims).
2. The MaterialEggs countertop differs between the CLI and Hydra (NRMSE 0.037).
3. An audit of Hydra code whose results depend on unordered-container order (the countertop's nondeterminism came from one).

### Then: motion blur (Phase 8b), two merges
Detailed plan, file references and decisions (accepted 2026-10-30): [motion_blur.md](motion_blur.md).
- A. CPU motion blur, complete: a time per sample within the shutter, both loaders reading the shutter and time samples, camera and transform motion (Embree time steps), deformation and velocities (time-sampled points, PointInstancer/mesh velocities, UsdSkel).
- B. GPU (Vulkan) minimum: one instant per pass, stratified across passes, acceleration-structure refit with interpolated transforms, interpolated vertices for deformation (no NVIDIA-only motion extension). Decided after seeing A's result.
- Metal with its native motion blur, later.

### Later (from the original plan; see docs/feature_roadmap.md)
- Curves; volumes (`docs/volumes.md`); tile texture streaming; the ALab GPU work (parked branch).
- Smaller: a glTF file-format plugin for usdview; MaterialX upstream reports (held back on purpose); a nested-dielectric stack for touching glass and objects (the MaterialEggs clamshell's dark patches, docs/DECISIONS.md 2026-11-01); absorbing transmission 1.2–1.4x brighter than Typhoon in `volume_transmission_single_cube`; destroying the Vulkan device in the background so teardown never waits for driver compiles.
