# Progress

Phase reports, newest first. Detailed reports live in `local/reports/` (git-ignored).

## 2026-10-25 — Phase 5: textured emitters as lights
- Varying-emission materials get per-triangle records (stratified EDF estimate × area, with a floor); NEE evaluates the true EDF; BSDF hits use the per-triangle pdf; CPU and Vulkan (Metal shares the code, untested). `SceneHasEmitters` counts them (default-dome rule). glTF emissive textures and OBJ `map_Ke` become lights.
- Unbiased against a 64k-spp BSDF-only reference (floor bands −0.11…+0.43%); ~24× lower variance. OpenPBR's 157k-triangle emitter costs 1.2 s per load (no cache yet).
- Fixed a pre-existing bias: light tables over ~4096 entries picked lights with wrong probabilities (OpenPBR in Hydra was 7.8% too bright; now within 0.04% of a reference).
- Open: OpenPBR's MEET MAT emits nothing in the CLI since Phase 7 (fine in Hydra).
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
