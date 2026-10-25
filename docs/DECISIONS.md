# Decisions

Design decisions and deliberate deviations, newest first. Each entry: date, decision, why.

## 2026-10-25
- **The CLI shares the delegate's mesh conversion and UsdPreviewSurface rewrite** (phase 7): `src/mesh/mesh_conversion.cpp` (subdivision, displacement, refinement budget, triangulation) and `src/materialx/preview_surface.h` were moved out of the delegate unchanged; each loader still reads the stage its own way (UsdGeom in the CLI, scene indices in the delegate). The CLI keeps its own polygon conversion for meshes that do not refine or displace, so its earlier images stay bit-identical; refined and displaced meshes use the shared one.
- **The CLI places every gprim on its own** (phase 7), as the delegate does, instead of one merged mesh: needed for light-link masks, instance primvars, per-prim primvars and ids. Polygon meshes stay in world space (bit-identical to before), prototypes are Embree instances.
- **The CLI reads USD materials from the composed UsdShade network first** (as the delegate), falling back to reading a referenced `.mtlx` file directly only when no network is composed: the direct read failed on the Teapot (material names) and the OpenPBR playground (relative texture paths in string MDL modules).
- **CLI fallback grey is the delegate's 0.5** (was 0.7) for geometry without a material or `displayColor`.
- **Prim ids are compared up to a renumbering** (`prism_compare_images --relabel`): each loader numbers its prims; element and instance ids compare exactly.
- **Textured emitters (Phase 5): the light sample looks the emission up with the footprint of the BSDF ray in its direction**, not at the finest texture level. MIS needs both strategies to estimate the same integrand; with the finest level, texture filtering of BSDF hits made the checker fixture's floor 6–10% too bright where black and bright texels meet.
- **Varying emission includes directional and cutout emission**, not only textures: a coated OpenPBR emitter (directional EDF) or an emitter with a varying cutout is now a light whose emission the light sample evaluates, instead of being left to BSDF sampling. Images of such scenes change (less noise; same expectation).
- **Triangles whose estimated emission is zero get no light record** (BSDF sampling only, still unbiased); a material's emission counts as zero when it is zero at the centres of up to 64 of its triangles on a placement. This probe also decides `SceneHasEmitters` (the default dome rule), so both agree. Other triangles weigh at least 10% of their material's mean.
- **The estimate is capped** (2^20 triangles, about 4M evaluations: 1 to 16 stratified points per face) and reruns at each scene change; no cache yet.
- **CI paused until the repository goes public.** GitHub Actions stopped on billing; public repositories get free minutes.
- **UsdPreviewSurface rough glass is left as MaterialX builds it** (rough specular over smooth transmission). The UsdPreviewSurface spec says "the transmissive lobe (if any) always assumes roughness 0.0", so the alternatives that conserve energy (one rough lobe: frosted; a smooth lobe: drops the specular roughness) contradict it. The remaining energy loss is MaterialX/MDL's to fix. Comparison: `local/comparisons/usdps_glass/`.
- **The opaque layering loss (~4.6%) is not repaired.** No MDL construct reproduces OpenPBR's energy-conserving layering; a diffuse scale-up would be wrong at grazing angles and change nearly every material. Upstream only.
- **MaterialX upstream reports are held back** for now (list in the session memory; drafts in `local/upstream/`).
- **Process standards adopted from the gesso project** (CLAUDE.md "Working standards"; this file, `docs/PLAN.md`, `docs/PROGRESS.md`; subagent reports in `local/reports/`).

## 2026-10-24
- **Layered dielectric energy loss is repaired in `DocumentLoader`** where an exact construction exists (one reflect-and-transmit lobe, or MDL `fresnel_layer` over smooth transmission); everything else logs "only partly repaired". Proven independent of PrismRT with NVIDIA's `df_native` (`local/upstream/repro/`).
- **`main` is the reference.** `device-abstraction` was fast-forwarded into `main`; no separate reference branch. The `cpu-reference` device and the recursive `SceneManager::trace` were removed.
- **Legacy C++ materials removed**: the glTF legacy path, the hard-coded scenes, `Metal`/`Glass`/`Dispersive`, quads and boxes. Built-ins left: `Diffuse` (fallback), `Emissive`, `ErrorMaterial`.
- **OBJ/MTL → MaterialX `open_pbr_surface`**, following Adobe's USD file-format plugin mapping (reimplemented, cited in `docs/obj.md`); `disp` is converted to a displacement shader although the CLI doesn't displace yet.
- **A failed GPU image render is an error** (exit 1, no image, a descriptive message) instead of a black image.
- **The CLI lights a scene that has no lights, no constant-emission geometry and no `--env`** with USD's default dome (`--default-dome false` to opt out); Hydra uses usdview's default dome by the same rule.

## 2026-10-23
- **Adaptive sampling on by default:** max 2048 spp, threshold 0.01, minimum 64 samples (from a CPU study against 16384-spp references); no batch time limit by default; `prism:timeLimitSeconds` / `--time-limit`.
- **Env-var-only render settings promoted to `prism:*` settings with CLI flags** (precedence: flag > RenderSettings > env var > default).
- **glTF materials load as MaterialX `gltf_pbr` through MDL**; the `gltf_pbr` volume is connected by a loader repair (MaterialX leaves it unconnected).
- **usdview starts with "Enable Default Dome Light" on**; the delegate ignores it when the stage has its own lights.

## 2026-10-22
- **USD is built with OpenColorIO** (vcpkg's, one OCIO DLL for everything) and a patch for Hdx's OCIO 1D/2D LUT textures; usdview starts in the config's default view (ACES 2.0 SDR).
- **`prism:honorDoubleSided`, default off.**
- **PointInstancer `invisibleIds` keep compacted instance numbering** (deviation from the plan), because usdview's picking and selection use the compacted index.

## 2026-10-21
- **Dependencies:** OpenUSD 26.08, MaterialX 1.39.5, OpenSubdiv 3.6.1, OpenImageIO 3.1.14 (vcpkg), Python 3.11; `tools/deps.json` is the single source of versions.
- **Colour management:** ACEScg working space by default, ACES 2.0 SDR view for display and 8-bit outputs; EXR and AOVs stay linear. Untagged MaterialX colours are treated as working-space values (what MaterialX's generator does).
- **Two USD loaders are kept** (Hydra and the CLI) with CLI-vs-Hydra parity tests and a rule that features update both.
- **OpenPBR differences against the Arnold reference are not "fixed":** USD composition wins (a connection beats an authored value), and MaterialX's coat darkening of transmission is followed.

## Earlier
- **ALab GPU work is parked** on its own branch until the rest is done.
