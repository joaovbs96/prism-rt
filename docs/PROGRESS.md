# Progress

Phase reports, newest first. Detailed reports live in `local/reports/` (git-ignored).

## 2026-10-28 - Fix: loading counters missed by the mesh task
- `prism_hydra_scene_bridge_test` failed about 1 run in 5-10 with "Mesh conversion counters did not complete": the snapshot build ran mesh preparation as a `tbb::task_group` task, and the progress model and logger are bound per thread. When a TBB worker (not the build thread) took the task, `Progress::Current()` was null there: `prismMeshesTotal`/`prismMeshesDone` stayed 0 and the subdivision-budget warning went to the process logger. The task now binds the build thread's progress model and logger.
- New CTest `prism_hydra_progress_status` (`prism_hydra_scene_bridge_test progress-status`) runs the progress check alone, in seconds. Details in `local/reports/flaky_progress_counters.md`.

## 2026-10-28 — SDK step 5a: progressive session
- The Hydra delegate's progressive loop (device preparation with the GPU preparation preview, adaptive preview, refinement passes over tiles, adaptive publication, time limit, texture-streaming restarts, denoise interval) moved unchanged into the core `prism::session::ProgressiveEngine`; `SceneBridge::Render` drives it over its render buffers. Hydra still builds its own `SceneManager` (incremental commit is step 5b).
- Public progressive `Session`: `start/pause/resume/cancel/restart/wait/status/aov/pick`, `ProgressiveOptions`, `SessionStatus`, `Pick`; host films on the session's own thread; new commits and setting changes restart the accumulation. Sessions of one scene share a counted render lock: a progressive session yields to other renders and to `commit()` at pass boundaries (commit now swaps a shared revision under it).
- Render timing: Hydra stats `elapsedSeconds`, `samplesPerSecond`, `estimatedSecondsRemaining`, also in `SessionStatus`/`ProgressInfo`.
- Hydra and CLI outputs bit-identical to main (c61712d) on 15 CPU cases, every EXR channel (Hydra: colour, adaptive, denoise AOV and products, id and light AOV products, a time sample; CLI: 3). Interactivity ratios new/old (5 alternating runs, InOneWeekend, CPU): usdview first frame 0.98, camera move 1.00 (p95 0.99), preview cost 1.03, edit to converged 1.01. One Vulkan run under `VK_LAYER_KHRONOS_validation` (CornellBox): clean. New tests: `prism_session_progressive_test`, timing keys in the bridge's accumulation test. Metal unbuilt (checklist in `docs/gpu_devices.md`). Report: `local/reports/sdk_session_progressive.md`.

## 2026-10-28 — SDK step 4: batch session and prism_io
- `include/prism/`: public `RendererOptions` (`options.h`), shared setting types (`settings.h`), `Session` with `render()` → `RenderResult` (colour and AOVs by Hydra token, `Int32` ids), render settings, crop and pixel aspect ratio, exposure, denoising, time limit, a progress callback and `cancel()` (`session.h`); `prism::io::Write` for EXR layers and 8-bit through the view (`io.h`); `Scene::commit` takes a progress callback. Camera rays come from `CameraDesc` (Gf's and CameraUtil's arithmetic, 20000 cameras bit-identical in `prism_camera_projection_test`).
- The CLI is parsing → scene → one session → `prism_io` (render products through it); Hydra unchanged; the core still builds without USD (`prism_session_test` links the core and `prism_io` only).
- CLI outputs bit-identical to main (1be9f0d) on 12 CPU cases (products, multi-part EXR, PNG, crops, denoise aov/replace, camera exposure, env exposure, glTF, OBJ, adaptive); end-to-end time ratios 1.00, 1.00 (and a 2 s case within the timer's granularity). Focused CPU suites 234/234; full serial ctest 480/480 (GPU tests included). Render settings still pass through the committed `SceneManager` (sessions of one scene render one at a time, enforced per scene). Metal unbuilt (checklist in `docs/gpu_devices.md`). Report: `local/reports/sdk_session_batch.md`.

## 2026-10-28 — SDK step 3: public scene API
- `include/prism/`: `Scene` with handles for meshes, spheres, instances, materials, lights, environments and cameras; `commit()` rebuilds the internal `SceneManager` with the loaders' conversions moved into the core (USD shapes and subdivision budget, OBJ displacement, lights, domes, default dome); a minimal `Renderer`. Errors are `prism::Error`; mesh data is copied at commit.
- glTF, OBJ and USD load through `prism::gltf::Load`, `prism::obj::Load`, `prism::usd::Load`; the CLI builds its scene through the API; Hydra unchanged; the core still builds without USD (`prism_scene_api_test` links the core alone).
- CLI images bit-identical to main (d622a78) on 29 CPU scenes; focused suites and the full serial ctest in the report. Not done: public `RendererOptions`, a commit progress callback, camera rays from `CameraDesc` (step 4). Metal unbuilt (checklist in `docs/gpu_devices.md`). Report: `local/reports/sdk_scene_api.md`.
## 2026-10-28 � SDK step 2: core without USD
- CMake targets `prism` (core, no pxr: configure check and `prism_core_without_usd_test`), `prism_io`, `prism_usd`, `prism_gltf`, `prism_obj`; the CLI and hdPrism link the modules they use. Camera and AOV math on a std-only `Matrix4d` (Gf's arithmetic, bit-identical); a std-only `MeshSource` with a Vt adapter; `SceneManager` without loader includes; glTF/OBJ material resources without Ar.
- Not done: render settings off `SceneManager` (deferred to the `Session` steps; `docs/DECISIONS.md`).
- New tests: `prism_matrix4d_test`, `prism_mesh_source_test`, `prism_core_without_usd_test`, plain-file checks in `prism_resolved_material_resource_test`. Images bit-identical to main (aa65105) at every commit: 10 CLI scenes (USD, glTF, OBJ, subdivision, displacement) and 3 Hydra renders on the CPU. Metal unbuilt (checklist in `docs/gpu_devices.md`). Report: `local/reports/sdk_step2.md`.
## 2026-10-28 — SDK steps 1a–1e: per-renderer state
- `RendererContext` with `RendererOptions::FromEnvironment()` (only the CLI and Hydra read the API-level env vars); colour management, SSS fallback, binding purposes, logger, progress and the varying-emission note belong to the renderer; process runtime for the cache root and budgets (first renderer wins); texture services per option set; Vulkan stack-size entry point per device.
- Fixed: compiled-material, early-association and template cache keys lacked the working space and SSS fallback (`prism_material_identity_test`).
- Images bit-identical at every step (8 CPU scenes, 1 Vulkan); load times ~1.00 (teapot cold cache 1.35, within its 12–25 s run-to-run swing; unverified). Metal unbuilt (checklist in `docs/gpu_devices.md`). 471/471 tests on merged `main`. Report: `local/reports/sdk_step1.md`.
## 2026-10-27 — Typhoon suite fixes: dome orientation, triplanarprojection, EON, declarations
- Domes follow UsdLux's lat-long orientation (longitude 0 = +Z at the image's centre); PrismRT was a quarter turn off (`usdlux/dome` FLIP 0.883 -> 0.014). Lookups and sampling, CPU and GPU, every dome source; textured-dome images change. Rect light textures now read upright, as UsdLux and Typhoon have them.
- triplanarprojection with tagged files crashed both loaders (MaterialX 1.39.5 use after free with several output colour transforms, fixed upstream since): one transform kept in `DocumentLoader`; asset inputs other than `file` are now materialized.
- EON energy compensation tinted by the albedo (MaterialX's MDL library left it white: up to 2.3x too bright in a dark channel); `eon_diffuse_openpbr_compare` with sphere meshes 0.99/0.99/1.00 of Typhoon (was 1.00/1.14/1.34).
- Six suite scenes rendered magenta ("Could not find a nodedef"): versioned definitions, enum names for integers, OpenPBR's `normal`, `UsdPrimvarReader_matrix` (its fallback) now repaired.
- Whole suite on the CPU: FLIP pass 66 -> 76 of 474 (test-suite 19 -> 28), no crashes (2 before), error material in 2 scenes (unsupported string primvars; 8 before). 469/469 tests. Report: `local/reports/typhoon_fixes.md`. Status: worktree branch, not merged.
## 2026-10-27 — USD conformance: renderingColorSpace, Typhoon suite, SDK design
- `UsdRenderSettings.renderingColorSpace` is the working space in both loaders (flag > `prism:colorManagement:workingSpace` > attribute > ACEScg); `data`/`raw` render in linear Rec.709. Scenes without it bit-identical to c0e44c1.
- Typhoon's regression suite run on CPU through Hydra (`tools/conformance/run_typhoon_suite.py`): 474 cases, classified. Bugs being fixed next: domes 90° off the UsdLux lat-long convention, a `triplanarprojection` crash, EON diffuse's white energy-compensation lobe in MaterialX's MDL, 8 "doesn't match declaration" magenta scenes. Report: `local/reports/conformance.md`.
- `docs/sdk_design.md`: the SDK/Python design (global-state inventory, API sketch, modules, motion reservation, migration steps, 17 open questions).
- 435/435 tests in the main checkout (with part 3). Status: merged locally, not pushed.

## 2026-10-27 — Denoising, part 3: OIDN's Metal device (unbuilt)
- The Metal device denoises with OIDN 2.5.1's Metal device on its own queue and shared `MTLBuffer`s (resolve kernel from the accumulation, no host round trip; host images for the delegate), falling back to OIDN's CPU device. `tools/ci/build_oidn_macos.sh` builds OIDN with CPU + Metal devices. Apple-only tests: `prism_denoise_*metal*`.
- Written on Windows: Metal code never compiled or run. Windows build and tests unaffected. Mac checklist: docs/gpu_devices.md. Report: `local/reports/denoise_part3.md`.

## 2026-10-27 — `--env` convention and tagged uniform colours
- The CLI's `--env` dropped a historical ×2 (`EnvironmentMap`'s default multiplier) and now matches a UsdLux dome of the same HDR (NRMSE 3.7e-8); USD domes, `--env-color` and the default dome were already unscaled. Earlier `--env` renders: add `--env-exposure 1` to reproduce.
- A colour-space tag on a constant feeding a uniform MDL input (`gltf_pbr` `attenuation_color`) failed with C219 and rendered magenta in USD scenes (found converting DragonAttenuation for Hydra); `DocumentLoader` converts such constants to the working space up front, and such materials stay out of templates.
- 414/414 tests in the main checkout. Report: `local/reports/env_uniform_fix.md`. Status: merged locally, not pushed.

## 2026-10-27 — Denoising, part 2: vk_oidn on the Vulkan device
- vk_oidn 0.2.0 (NVIDIA's prebuilt SDK package, OIDN 2.5.1 weights; `tools/ci/fetch_vk_oidn.ps1`) with OIDN's albedo/normal prefilter networks packed for it; the Vulkan device enables its features when the GPU has them, denoises whole-image renders on its accumulation (resolve pass) and the delegate's host images by upload; OIDN's CPU device on host copies otherwise (`PRISM_GPU_DENOISER=cpu` forces it). The CLI and the delegate name the denoiser.
- 1080p high, RTX 4070 Ti, first denoise with set-up: 177 ms (GPU 36 ms), 481 ms prefiltered (GPU 88 ms); generic convolutions 92 / 360 ms; OIDN CPU 448 / 2866 ms. GPU vs CPU denoised on the same raw render: NRMSE 1.3e-3–1.4e-3. 16 spp error ratio 0.274 (CPU 0.264).
- Validation (GPU-AV, sync) clean on denoise.usda and the host-image test; denoise off bit-identical to 90d1abc (CPU and Vulkan, chess/egg_gold/Teapot). Report: `local/reports/denoise_part2.md`. Not verified: usdview by hand, a GPU without vk_oidn's features, Metal.

## 2026-10-27 — Denoising, part 1: interface and OIDN on the CPU
- OIDN 2.5.1 built from source against our oneTBB (`tools/ci/build_oidn.ps1`); a denoiser interface on the render device; guides from the `albedo` AOV and a world-space shading normal. Vulkan/Metal denoise through host copies until parts 2–3.
- CLI `--denoise none|aov|replace`, `--denoise-quality`, `--denoise-prefilter`; `prism:denoise*` settings; a `denoised` AOV; usdview shows a denoised frame every 16 passes, on pause and at convergence (off the render thread).
- 16 spp NRMSE 0.246 → 0.065. 1080p CPU: 0.5 s, 3.1 s with guide prefiltering (default on). Denoise off: bit-identical to ca2ed1a.
- 391/391 tests in the main checkout. Report: `local/reports/denoise_part1.md`. Not verified: usdview by hand, Metal. Status: merged locally, not pushed.

## 2026-10-26 — Fixes after Phases 5–7
- **usdview:** Prism → Environment… (a session-layer dome; the stage's domes deactivated in the session layer, "Off" restores them); PyOpenColorIO 2.6.0 pinned (OCIO menus populated, warning gone, one OpenColorIO DLL in usdview); the time-limit setting is a float (usdview has no double editor); the session dome authors `info:implementationSource` (an OpenUSD 26.08 warning otherwise).
- **Adaptive sampling in Hydra:** converged pixels were dropped from published buffers when the host still held the previous one (usdview on Vulkan lost the dome background around "87%"); fixed with a regression test on CPU and Vulkan.
- **OpenPBR on Vulkan:** the NVIDIA shader compiler ran out of memory (glass repairs × all 53 materials in the CLI, up to 16 parallel driver compiles). Fixed by a compile memory budget, a SPIR-V size limit with CPU fallback, then smaller programs: repairs rewrite a private copy of the library graph (CLI glass programs 7–15 MB → ~2 MB), and the CLI compiles USD materials through the delegate's material service (now `src/materialx/material_service.*`). OpenPBR cold/warm on Vulkan: CLI 76.8/41.3 s → 29.7/13.1 s, Hydra ~31/14 s. The textured-emitter raygen is prewarmed while materials load (Hydra cold 39.3 → 29.3 s under load).
- 347/347 tests. Reports: `local/reports/` (usdview_env_ocio, adaptive_hydra_background, openpbr_vulkan_crash, openpbr_vulkan_time, openpbr_load_time, cli_material_path, raygen_prewarm). Pushed (686dd68).

## 2026-10-25 — Phase 6: light AOVs and multi-part EXR
- New AOVs on CPU and Vulkan (Metal: same code, compile-checked only), in Hydra and the CLI: `albedo` (denoising albedo through sharp bounces), `diffuse`, `specular`, `emission` (split by the first scattering vertex; their sum is the colour to < 1.1e-6 relative), `Pworld` (also `P`). Multi-part EXR per product (`prism:exr:multiPart`).
- Without light AOVs: images bit-identical to bd75e3e (chess, egg_gold, OpenPBR, Teapot on CPU; chess, egg_gold, Teapot on Vulkan) and the Vulkan raygen/callable SPIR-V byte-identical; CPU time unchanged (alternating pairs, +0.3%). With them: CPU +9%, Vulkan +28% GPU time on chess (one run each).
- OpenPBR on Vulkan crashed in the NVIDIA driver's shader compiler (LLVM out of memory in nvoglv64.dll) for both bd75e3e and the new build: not compared.
- 336/336 tests (full serial run; 35 new). Report: `local/reports/phase6_aovs.md`. Status: worktree branch, not merged.

## 2026-10-25 — Phase 5: textured emitters as lights
- Varying-emission materials get per-triangle records (stratified EDF estimate × area, with a floor); NEE evaluates the true EDF; BSDF hits use the per-triangle pdf; CPU and Vulkan (Metal shares the code, untested). `SceneHasEmitters` counts them (default-dome rule). glTF emissive textures and OBJ `map_Ke` become lights.
- Unbiased against a 64k-spp BSDF-only reference (floor bands −0.11…+0.43%); ~24× lower variance. OpenPBR's 157k-triangle emitter costs 1.2 s per load (no cache yet).
- Fixed a pre-existing bias: light tables over ~4096 entries picked lights with wrong probabilities (OpenPBR in Hydra was 7.8% too bright; now within 0.04% of a reference).
- Fixed after review: OpenPBR's MEET MAT was left out of the CLI's light table (its glow was there, only BSDF-sampled), not missing: see `local/reports/meetmat_cli_emission.md` and `docs/DECISIONS.md`.
- 298/298 tests in the main checkout with Phases 5 and 7 merged. Report: `local/reports/phase5_textured_emitters.md`. Status: merged locally, not pushed.

## 2026-10-25 — Phase 7: CLI parity
- The CLI's USD loader reaches the delegate's level: subdivision and displacement (OBJ `disp` too), instancing with per-instance primvars and invisibleIds, holes, leftHanded/negative scale, GeomSubsets, purpose, `--time`, UsdSkel, camera exposure, id AOVs; materials from the composed UsdShade network first (fixes `emissive.usda` and the OpenPBR materials that failed in the CLI). Mesh conversion moved from `scene_bridge.cpp` into `src/mesh/`.
- 288 tests (agent's full run); main checkout 256/256 CPU tests, GPU tests pending. New parity fixtures ≤1e-7 NRMSE except linking 0.0074, native instance bindings 0.013, animation 0.010. CLI vs Hydra: egg 0.069→0.0005, chess 0.20→0.030, Teapot 0.48→0.0044, OpenPBR 1.43→0.30 (2.4% mean left, not investigated).
- Report: `local/reports/phase7_cli_parity.md`. Status: merged locally, not pushed.

## 2026-10-24/25 — Glass energy loss
- Root cause: MaterialX genmdl layers glass as a Schlick `custom_curve_layer` over transmission; libbsdf weights the base by `1 − max(curve(k1), curve(k2))`, losing light that leaves the medium (R+T 0.64 at 41° inside IOR 1.5 glass). libbsdf also skips multiscatter compensation for rough transmit-only lobes.
- Repaired in `DocumentLoader` (exact where possible, logged otherwise); furnace sphere 0.987 → 1.0000. Tests: `prism_dielectric_albedo_test`, `materialx_loader_fresnel`, `materialx_loader_dielectric`, tightened furnace checks. 228 tests pass.
- Independent proof with NVIDIA's `df_native`: `local/upstream/repro/`.
- Status: merged locally, not pushed.

## 2026-10-24 — Legacy removal and OBJ
- Legacy glTF path, hard-coded scenes, legacy materials and `cpu-reference` removed (~1750 lines); OBJ/MTL → MaterialX. 225 tests pass; USD/glTF images bit-identical.

## 2026-10-23 — Intermission
- glTF → MaterialX `gltf_pbr`; default dome light in usdview and the CLI; adaptive sampling defaults, time limit, settings parity, CLI progress line; failed GPU renders are errors. 209 tests.

## 2026-10-22 — Phases 3 and 4
- Phase 3: RenderSettings/Product/Var, shared image writer, CLI-vs-Hydra parity harness. 136 tests.
- Phase 4: light camera visibility, dome colour temperature, invisibleIds, displayColor interpolation, light diffuse/specular, doubleSided, per-instance primvars. 180 tests.
- A GPU driver fault froze the PC during a Vulkan perf loop; perf loops now need João's OK.

## 2026-10-21 — Phases 0–2
- Phase 0: `docs/volumes.md`.
- Phase 1: dependency upgrade (USD 26.08, MaterialX 1.39.5, OIIO 3.1.14), `tools/deps.json`. 59 tests.
- Phase 2: OpenColorIO, ACEScg working space. 82 tests. USD rebuilt with OCIO + an Hdx patch.
