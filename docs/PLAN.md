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

## Current state (2026-10-25)

- `main` is the development branch (fast-forwarded from `device-abstraction`; GitHub's `main` = fb2b235).
- 11 local commits on top of it are not pushed: the layered-dielectric energy repairs and the albedo survey tool.
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

### Later (from the original plan; see docs/feature_roadmap.md)
- Phase 8a: ShapingAPI spotlights and IES profiles. Phase 8b: motion blur.
- Curves; volumes (`docs/volumes.md`); tile texture streaming; the ALab GPU work (parked branch).
- Smaller: the OpenPBR wide-shot CPU/Vulkan brightness gap (~3–4%); a timeout on the CLI's GPU fence wait; coated OpenPBR glass reflecting twice (R 0.078 instead of 0.04); the repeated "approximate repair" log line; a glTF file-format plugin for usdview; MaterialX upstream reports (held back on purpose).
