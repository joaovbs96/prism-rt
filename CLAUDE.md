# CLAUDE.md — PrismRT

PrismRT is a physically based path tracer in C++20 that renders on the CPU or the GPU (Vulkan, Metal), with a Hydra render delegate (hdPrism, usdview) and a standalone CLI. It renders USD, glTF and OBJ scenes; Embree traces on the CPU, oneTBB parallelises, OpenImageIO handles images, OpenColorIO the colour management, and the NVIDIA MDL SDK compiles MaterialX materials.

These rules apply to every session in this repo. The first half is how we work; the second half is how the code is built and organised.

## Working standards

### Role: orchestrator
- The main session orchestrates; its context is the expensive resource.
- Delegate execution to subagents. Keep raw logs, JSON and images out of the main context.
- Subagents do NOT read the handover (`docs/PLAN.md`). Give each a self-contained brief: goal, exact commands or files, the constraints that apply (copy the relevant rules from this file), definition of done, return format.
- Subagents write details to `local/reports/<topic>.md` (git-ignored; a worktree agent writes into its own worktree's `local/reports/` and the main session copies the file over) and return at most ~150 words: verdict, key numbers, evidence paths, anything unverified or odd.
- Treat reports as evidence, not truth: spot-check anything surprising before acting.

### Model selection
- Haiku: very simple or mechanical tasks.
- Sonnet: general / standard-complexity implementation.
- Opus: when Sonnet failed at a diagnosis, or the problem needs deep reasoning across components (GPU, integrator, MDL/MaterialX).
- Fable: only as an advisor to an Opus subagent, with minimal context (cost). Never a direct subagent. Opus consults Fable when stuck, and also reviews its plan with Fable before implementing whenever a problem has already gone through a few fix/review iterations.

### Failure handling
- On a concrete failure: stop the batch, diagnose from evidence, fix narrowly with a regression test, rerun the focused suite, then resume.
- No speculative fixes.

### Machine safety (GPU faults have frozen this PC)
- One GPU process at a time across the whole machine (renders, usdview, Vulkan tests); check for other `PrismRT`/`prism_*`/`usdview`/`ctest` processes first (ones running with `--device cpu` don't count).
- Renders go through `local\alab\run_capped.ps1 -LimitGB <n> -Exe <exe> -Arguments "..."` (a job-object memory cap). Never wrap powershell/cmd/python in the cap; no retry loops.
- No GPU performance loops or A/B matrices without João's OK. New Vulkan paths run first under `VK_LAYER_KHRONOS_validation` (+ GPU-AV) on a small scene.
- On a device-lost, TDR, hang, empty log or a slow system: stop all GPU work and wait for João before restarting anything.
- Keep C: above 60 GB free. D: is a slow USB HDD: fine for builds and dependency installs, not for shader or texture caches.

### Git
- Agents commit small local commits freely, with short imperative subjects in the existing style.
- Never push, merge into `main` or open PRs without João's explicit approval.
- Commits and PR descriptions carry no attribution or co-author lines for Claude or any code agent.
- Stage explicit paths (`git add <path>`); never `git commit -a` and never override `core.autocrlf`. Check `git diff --cached --stat` before committing and `git diff --stat origin/main..HEAD` before pushing: the file count must match the intended change (a past commit rewrote 365 files' line endings).
- Don't touch João's untracked files in the checkout (`HYDRA_DELEGATE_PLAN.md`, `USD-EXPORT-FINDINGS.md`, `analytic-spheres.usda`, `docs/gpu_backend.md`). Never rewrite source asset files.

### Communication
- Short, simple technical English. Prefer concise bullet points unless more depth is asked for.

### Project workflow
- Follow the working agreement in `docs/PLAN.md` (phase by phase, stop for review).
- Deviations and design decisions go in `docs/DECISIONS.md`; phase reports in `docs/PROGRESS.md`.

### Change rules
- Scene features change **both loaders** (Hydra `src/hydra/scene_bridge.cpp` and the CLI `src/standalone_cli.cpp` / `src/mesh/*`) and add a CLI-vs-Hydra parity fixture (`prism_add_parity_fixture` in `CMakeLists.txt`).
- Integrator and material changes must keep **CPU/Vulkan parity** (`prism_compare_images`, against a noise floor); changes that shouldn't alter images are checked bit-identical (`--exact`) against the previous build.
- Changes to code the Metal device shares (`shaders/`, `src/device/gpu/`, `src/hydra/`, `src/materialx/`) get an entry in the Metal checklist in `docs/gpu_devices.md`.
- Tests and comparisons pin `--adaptive-threshold 0` and a fixed sample count (adaptive sampling is on by default).
- Merge gate: the full serial `ctest --test-dir build -C RelWithDebInfo -j 1`, run once at the end.

## Build

**Requirements:** CMake 3.20+, Visual Studio 2022, vcpkg, Python 3.11 (usdview), the Vulkan SDK.

**Dependencies.** `tools/deps.json` is the single source of dependency versions (checked by `tools/check_deps.py` and the `prism_deps_consistency_test`). `tools/build_deps_local.ps1` builds OpenSubdiv, MaterialX and OpenUSD (with OpenColorIO and the patches in `tools/ci/patches/`) into versioned prefixes under `D:\prism-deps`; the MDL SDK is built from source without CUDA (`C:\deps\mdl-sdk-2026.0.2-nocuda`); OpenImageIO and OpenColorIO come from vcpkg (the clone at `D:\prism-deps\vcpkg`, whose baseline matches `vcpkg.json`). The machine-specific roots live in the git-ignored `cmake/PrismLocalDependencies.cmake` and `prism_local_env.ps1` (templates: `*.example`). `build_script.py` is the old dependency script; don't use it.

**Git submodules** (tinygltf, tinyobjloader in `external/`): `git submodule update --init --recursive`.

**Configure, build, run** (VS Code's `.vscode/settings.json` holds the same arguments):
```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=D:/prism-deps/vcpkg/scripts/buildsystems/vcpkg.cmake -DPRISM_BUILD_HYDRA=ON
cmake --build build --config RelWithDebInfo --parallel
. .\prism_env.ps1          # always, before running anything
build\RelWithDebInfo\PrismRT.exe --scene <file> --camera <path> --output out.png
.\run_usdview.ps1 <scene.usda>
```
- `PRISM_BUILD_HYDRA` defaults to OFF; a fresh configure without it silently skips the delegate.
- `prism_env.ps1` puts the right USD/MaterialX/MDL/vcpkg/TBB/Embree directories first on `PATH`, sets `OCIO` and the usdview plugin path. Without it, an old USD on the system `PATH` causes "entry point not found" (0xc0000139).
- In Python, import `pxr` before PySide6: PySide6 ships an old `msvcp140.dll` that breaks USD's DLLs if loaded first.
- `RelWithDebInfo` matches the dependency builds; `Release` also works. Sources are globbed (`CONFIGURE_DEPENDS`), so new files need no CMake edits.

## Tests

CTest registers the renderer's tests (`ctest --test-dir build -C RelWithDebInfo -j 1`). Tests that may open a GPU device carry the `gpu` label and `RESOURCE_LOCK gpu`, so `-LE gpu` runs the CPU ones. There is also a standalone project for the MaterialX loader that avoids the USD/MDL/Embree dependencies:
```
cmake -S tests -B build/sss_tests -DMaterialX_DIR=<MaterialX install>/lib/cmake/MaterialX
cmake --build build/sss_tests --config Release
ctest --test-dir build/sss_tests -C Release --output-on-failure
```
Each test is its own executable (`materialx_loader_*`); these compile `src/materialx/loader.cpp` directly, so keep that file free of renderer/MDL includes. `prism_dielectric_albedo_test` measures a compiled material's reflected + transmitted energy per angle (`-v`, `-s <case>`, `-f <file.mtlx>`).

## Architecture

### Rendering pipeline
Two front ends load scenes into a `SceneManager` and hand it to a render device (`src/device/device.h`):
- the standalone CLI (`main.cpp` → `runStandaloneCli` in `src/standalone_cli.cpp`): one USD (`USDMesh`), glTF (`glTFMesh`) or OBJ (`ObjMesh`) file, a USD camera or `--camera-position/--camera-target`, render settings and products, output through OpenImageIO, a progress line, and USD's default dome for scenes with no light;
- the Hydra delegate (`src/hydra/`, `scene_bridge.cpp`): usdview and `prism_hydra_render`.

There are no hard-coded C++ scenes; `assets/scenes` holds USD scenes (InOneWeekend, CornellBox, QuadTest).

Every device (`cpu`, `vulkan`, `metal`) runs the shared integrator in `shaders/integrator/path_tracer.h`, compiled as C++ (`src/device/cpu_backend.h`, `cpu_device.cpp`) and as HLSL (`shaders/gpu/gpu_backend.hlsl`; DXC → SPIR-V for Vulkan, compute megakernel or callable shaders per MDL program). It does NEE with power-heuristic MIS over a light table (`shaders/integrator/lights.h`, built by `src/device/light_table.h`) holding UsdLux lights, constant-emission triangles and the domes; light and shadow linking; random-walk media (SSS and absorbing transmission) from MDL volume coefficients; adaptive sampling (`src/device/adaptive_sampling.h`; defaults in `src/render_defaults.h`). Colour is managed in an ACEScg working space through OpenColorIO (`src/color/`, docs/color_management.md). CPU render parallelism is `tbb::parallel_for` over pixels.

### Key subsystems
**Scene (`src/scene.h`)** — `SceneManager` owns geometry (keyed by Embree `geomID`), placements (transforms, materials, primvars) and the environment; front ends attach loaded meshes (`attachImportedMesh`) and analytic USD spheres (`Sphere`, `BatchedSpheres`). Materials are looked up per hit with `getMaterial(primid, instid)`.

**Materials (`src/material.h`, `src/mdl_material.h`)** — every scene material is MaterialX compiled through MDL (`MDLMaterial`): USD MaterialX/UsdShade networks, glTF materials (`src/materialx/gltf_material.cpp`, `gltf_pbr`, docs/gltf.md) and OBJ/MTL materials (`src/materialx/obj_material.cpp`, `open_pbr_surface`, docs/obj.md). `src/material.h` keeps only the built-ins: `Diffuse` (the fallback for geometry without a material, `displayColor`/`displayOpacity`), `Emissive`, and `ErrorMaterial` (magenta, for materials that fail to load). `RayHit` normals arrive already flipped toward the ray, with the side in `front_face` — MDL code relies on this; don't re-flip.

**MaterialX → MDL pipeline (`src/materialx/`, `src/mdl/`)**, run inside the `MDLMaterial` constructor: `MDLGenerator::shared(path)` (owns the stdlib; constructing one reloads it) + `DocumentLoader::load()` (in-memory compatibility repairs, stdlib import, validation) → `MDLGenerator::generate()` → `mdl::MaterialCompiler` → `mdl::BackendCompiler` (native CPU code; HLSL for the GPU).

**Compatibility preprocessing** — `DocumentLoader` repairs documents in memory (never the files): colour-space aliases, input types, layers over mixes, thin-walled OpenPBR/standard_surface, the uncoated transmission IOR, glTF volume attenuation, layered-dielectric energy loss, the standard_surface conductor clamp, and the opt-in SSS fallback. Each is documented, with what is deliberately not repaired, in `docs/materialx_compatibility.md`.

**Hydra lights, media and geometry** — `scene_bridge.cpp` converts UsdLux lights, light/shadow linking, subdivision (OpenSubdiv), load-time displacement and mesh primvars for MDL scene-data lookups. Render settings, their CLI flags and precedence (flag > RenderSettings `prism:*` > env var > default) are tabulated in `docs/hydra.md`. Deferred features: `docs/feature_roadmap.md`.

### Asset resolution
Scene files resolve their own assets (USD through Ar; glTF images and OBJ maps relative to their files, published into the material cache). The MaterialX libraries come from `PRISM_MATERIALX_LIBRARY_PATH` (set by `prism_env.ps1`).

## Known limitations
- Many paths are user-specific (`.vscode/`, the local dependency config, `local\`).
- The README's build instructions are partly outdated — trust `CMakeLists.txt`, this file and `docs/windows_vulkan.md`.

## Constraints
- Use the custom `Float2/3/4` math types from `src/float2.h` etc. (`Color3` is a `Float3`) — do not introduce glm, Eigen, or similar.
- Render parallelism goes through `tbb::parallel_for` — do not use `std::thread` or OpenMP for rendering work (the CLI's progress monitor thread in `standalone_cli.cpp` is not rendering work).
- Embree scenes must be committed (`rtcCommitScene`) before any intersection call.
- `MDLMaterial` compilation is expensive — it happens at load time only, never in the render loop.
- Compatibility fixes for `.mtlx` content belong in `DocumentLoader`'s in-memory preprocessing — never rewrite source asset files.
