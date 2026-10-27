# Progress

Phase reports, newest first. Detailed reports live in `local/reports/` (git-ignored).

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
