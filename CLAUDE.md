# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PrismRT is a CPU-based physically-based path tracer written in C++20. It renders 3D scenes from USD, glTF, and OBJ formats using Intel Embree for BVH-accelerated ray tracing, oneTBB for parallelism, OpenImageIO for image I/O, and the NVIDIA MDL SDK for MaterialX-based material compilation.

## Build System

**Requirements:** CMake 3.20+, Visual Studio 2022, vcpkg (`VCPKG_ROOT` env var set; `vcpkg.json` provides OpenImageIO etc.)

**External dependencies (not in repo; use CMake package discovery, `-D` overrides, or the ignored `cmake/PrismLocalDependencies.cmake`):**
- Embree v4.x → `embree_DIR` (`Embree/build/Release`)
- oneTBB → `TBB_DIR` (`include/tbb/lib/cmake/tbb`)
- OpenUSD → `pxr_DIR` (`external_deps/OpenUSD/install`)
- MaterialX → `MaterialX_DIR` (`external_deps/MaterialX/install/lib/cmake/MaterialX`)
- MDL SDK → `MDL_SDK_DIR` (`mdl-sdk`) — headers only; `libmdl_sdk.dll` and the `nv_openimageio` plugin are loaded at runtime (`src/mdl/loader.cpp`) from the working directory / `PATH`

`build_script.py` rebuilds the *dependencies* (OpenSubdiv → MaterialX → OpenUSD, all `RelWithDebInfo`) — it does not build PrismRT. USD must be built with `-DPXR_FIND_TBB_IN_CONFIG=ON` so it uses oneTBB rather than legacy TBB; the script also deletes legacy `include/tbb` headers from the USD install. If you see `tbb::task` / `split` ambiguity errors, a stale USD install or legacy TBB headers are being picked up.

**Git submodules** (tinygltf, tinyobjloader in `external/`):
```
git submodule update --init --recursive
```

**Configure** (the full set of args lives in `.vscode/settings.json` → `cmake.configureArgs`; the essentials):
```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -Dembree_DIR=<embree_cmake_dir>
```

**Build and run:**
```
cmake --build build --config RelWithDebInfo
build/RelWithDebInfo/PrismRT.exe
```

- `RelWithDebInfo` matches the dependency builds and the VS Code launch config; `Release` also works (imported configs are mapped in `CMakeLists.txt`).
- Sources are globbed (`src/*.cpp`, `src/*.h`, `CONFIGURE_DEPENDS`) — new files need no CMake edits.
- The `copy_assets` target copies `assets/` next to the executable on every build.
- USD's `bin` and `lib` directories (and the MDL SDK DLLs) must be on `PATH` at runtime.
- `output.png` is written to the **current working directory**, not the exe directory.

## Tests

The renderer itself has no automated tests — verify by inspecting the rendered `output.png`. There is one standalone regression test project for the MaterialX loader that deliberately avoids the USD/MDL/Embree dependencies:

```
cmake -S tests -B build/sss_tests -DMaterialX_DIR=<MaterialX install>/lib/cmake/MaterialX
cmake --build build/sss_tests --config Release
ctest --test-dir build/sss_tests -C Release --output-on-failure
```

Each test is its own executable (`materialx_loader_sss`, `materialx_loader_clamp`); run one with `ctest ... -R <name>`. Tests compile `src/materialx/loader.cpp` directly, so keep that file free of renderer/MDL includes.

## Architecture

Most of the renderer is header-only (`src/*.h`); only the MDL/MaterialX pipeline has `.cpp` files.

### Rendering Pipeline

`main.cpp` drives everything:
1. Initialize the MDL SDK (`mdl::Context` — loads the DLL, loads plugins *before* `neuray->start()`, acquires interfaces)
2. Instantiate a `SceneManager` subclass. Scenes are hardcoded C++ classes, **all defined in `src/scene.h`**; the active scene, `samples`, and `max_depth` are hardcoded in `main()`. To add a scene: subclass `SceneManager` in `scene.h`, populate geometry/camera/`environment` in the constructor, and swap the `new ...` line in `main.cpp`.
3. `commitScene()` applies transforms and commits the Embree BVH
4. Nested `tbb::parallel_for` over rows/columns; per pixel, `cam.get_ray()` + `SceneManager::trace()` per sample, with a TEA-hash RNG seed per pixel
5. `FrameBuffer::applyGammaCorrection()` then save `output.png` via OpenImageIO

`SceneManager::trace()` is the original recursive integrator, kept as the `cpu-reference` device. Every other device runs the shared integrator in `shaders/integrator/path_tracer.h`, compiled as C++ (CPU backend, `src/device/cpu_backend.h`) and as HLSL (`shaders/gpu/gpu_backend.hlsl`): NEE with power-heuristic MIS over a light table (`shaders/integrator/lights.h`, built by `src/device/light_table.h`) holding UsdLux sphere/rect/disk/cylinder/distant lights, constant-emission triangles and the domes (several domes combine as one `CompositeEnvironment` entry), light and shadow linking, and random-walk media (SSS and absorbing transmission) from MDL volume coefficients.

### Key Subsystems

**Scene (`src/scene.h`)** — `SceneManager` owns geometry (`std::unordered_map<int, Geometry*>` keyed by Embree `geomID`), camera, and environment. `add_mesh()` dispatches on file extension to `ObjMesh` / `glTFMesh` / `USDMesh`; `add_sphere/quad/box` create analytic shapes. Each `Geometry` registers itself with Embree via `setupEmbree()`. Materials are looked up per hit with `getMaterial(primid, instid)`, so meshes can carry multiple materials; `material_override` replaces them all.

**Materials (`src/material.h`, `src/mdl_material.h`)** — `Material::sample()` is the entry point; the default implementation composes `sample_direction()`, `evaluate_color()`, `evaluate_emission()`. `MDLMaterial` overrides `sample()` wholesale and executes MDL-generated native code (init → BSDF sample → EDF evaluate). `RayHit` normals arrive already flipped toward the ray, with the side recorded in `front_face` — MDL code relies on this; don't re-flip. An invalid `MDLMaterial` renders as emissive magenta rather than failing.

**MaterialX → MDL pipeline (`src/materialx/`, `src/mdl/`)**, run inside the `MDLMaterial` constructor:
1. `MDLGenerator` (owns the MaterialX stdlib; obtain it via `MDLGenerator::shared(path)` — constructing one reloads the whole library) + `DocumentLoader::load()` — loads the `.mtlx`, applies in-memory compatibility preprocessing, imports the stdlib, validates
2. `MDLGenerator::generate()` — MaterialX → MDL source
3. `mdl::MaterialCompiler` — loads the MDL module into the SDK and compiles it (also resolves texture paths relative to the `.mtlx`)
4. `mdl::BackendCompiler` — native CPU target code

`USDMesh` runs the same pipeline for materials bound in USD (referenced `.mtlx` files or UsdShade networks), caching compiled materials per load by `(canonical file path, material name)`.

**Compatibility preprocessing** — see `docs/materialx_compatibility.md`. `DocumentLoader` repairs documents in memory before generation: a **conductor clamp** (`standard_surface` `base_color`/`specular_color` limited to `[0, 0.9999]` for an upstream `artistic_ior` bug), OCIO **color space aliases** mapped to MaterialX names, **input type repairs** (extract/convert nodes for mismatched connections, inputs retyped to their node definition), and **layers distributed over mixed tops** (MaterialX's MDL generator drops the base otherwise). The **SSS-to-diffuse fallback** is opt-in (`prism:sssFallback` / `--sss-fallback true`, default from `PRISM_SSS_FALLBACK=1`) now that the integrator traces SSS. `mdl_report.md` holds further notes on MDL pipeline issues.

**Lights, media and geometry (Hydra).** `scene_bridge.cpp` converts UsdLux lights into `SceneManager::lights` (`src/lights.h`), resolves light/shadow-link categories from `HdsiLightLinkingSceneIndex` (added in `plugin.cpp`), refines subdivision surfaces with OpenSubdiv (`src/mesh/subdivision.cpp`; `prism:subdivisionLevel`, default 2, and `prism:subdivisionMaxFaces` per mesh, whose defaults come from `PRISM_SUBDIVISION_LEVEL`/`_MAX_FACES`), displaces meshes by their material's `geometry.displacement` at load time (`prism:displacementLevel`/`MaxFaces`, defaults from `PRISM_DISPLACEMENT_LEVEL`/`_MAX_FACES`), and carries named mesh primvars to MDL scene data lookups (`geompropvalue`) on both devices. GPU textures beyond `prism:gpuTextureBudgetMiB` (`--gpu-texture-budget-mib`; default `PRISM_GPU_TEXTURE_BUDGET_MIB`, else 45% of device memory) upload at reduced resolution. Render settings, their CLI flags and precedence (flag > RenderSettings `prism:*` > env var > default, shared defaults in `src/render_defaults.h`) are tabulated in `docs/hydra.md`. Deferred features are listed in `docs/feature_roadmap.md`.

### Asset Resolution

Assets resolve relative to the executable: `{exe_dir}/assets/{models,textures,environment}` via `SceneManager::getAssetsPath()`. Exceptions still hardcoded in `scene.h`: the MaterialX `libraries` path (`mx_libraries_path` / `mx_mdl_path` globals) and the `.mtlx` path in the `MaterialXTest` scene.

## Known Limitations

- Little error handling for missing assets — `add_mesh()` throws on a missing file, most other loads crash
- `setPosition()` on AABBs only works correctly when geometry starts at origin
- Many paths are user-specific (`CMakeLists.txt`, `build_script.py`, `.vscode/`, `scene.h`)
- README build instructions are WIP and partly outdated (e.g. TBB location) — trust `CMakeLists.txt`

## Git

- Commits and PR descriptions must not contain any attribution to, or mention of, AI agents or assistants — no `Co-Authored-By` trailers, no "Generated with" lines. Write plain messages in the existing style (short imperative subject).
- Commit locally only. Never push unless explicitly asked.

## Constraints

- Use the custom `Float2/3/4` math types from `src/float2.h` etc. (`Color3` is a `Float3`) — do not introduce glm, Eigen, or similar
- Render parallelism goes through `tbb::parallel_for` — do not use `std::thread` or OpenMP for rendering work (the one `std::thread` in `main.cpp` is only the progress-bar monitor)
- Embree scenes must be committed (`rtcCommitScene`) before any intersection call
- `MDLMaterial` compilation is expensive — it happens at load time only, never in the render loop
- Compatibility fixes for `.mtlx` content belong in `DocumentLoader`'s in-memory preprocessing — never rewrite source asset files
