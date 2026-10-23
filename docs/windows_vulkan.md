# Building PrismRT with the Vulkan device on Windows

This is the setup validated on Windows 11 x64 with an NVIDIA RTX 4070 Ti. See
[gpu_devices.md](gpu_devices.md) for how the devices work and
[deployment.md](deployment.md) for installation.

## Toolchain

| Tool | Version used |
| --- | --- |
| Visual Studio 2022 | 17.14, MSVC 14.44 (v143), Windows SDK 10.0.26100 |
| CMake | 3.31.4 |
| Git / Git LFS | 2.46 / 3.5.1 |
| Vulkan SDK | 1.4.321.0, which sets `VULKAN_SDK` and provides `Bin\dxc.exe` (1.8) |
| NVIDIA driver | 610.88 (Vulkan 1.4.341) |
| Python | 3.11.9 x64 with PySide6 6.7.2, PyOpenGL 3.1.7, numpy |
| vcpkg | checked out at `tools/deps.json`'s baseline; OpenImageIO 3.1.14 from the manifest |

The GPU must expose `VK_KHR_ray_query` and `VK_KHR_acceleration_structure`
(`vulkaninfo`). CMake finds DXC through `$ENV{VULKAN_SDK}/bin`.

Use the Python that OpenUSD was built against. usdview loads USD's Python
modules, so the interpreter's major and minor version must match the USD build
(3.11 here).

## Dependencies

The versions are those of `tools/deps.json`, the single version list; see
"How to upgrade a dependency" in [deployment.md](deployment.md). CMake checks
the versions it finds against it, and `prism_deps_consistency_test` checks this
table.

| Dependency | Version | Notes |
| --- | --- | --- |
| OpenUSD | 26.08 | `tools/build_deps_local.ps1` (the CI script) with Python, imaging, usdview, MaterialX and OpenColorIO (`-DPXR_BUILD_OPENCOLORIO_PLUGIN=ON`, linked to vcpkg's OpenColorIO below). Uses oneTBB (`-DPXR_FIND_TBB_IN_CONFIG=ON`); the install must not contain legacy `include\tbb` headers. |
| OpenSubdiv | 3.6.1 | Built by the same script. On Windows it builds static libraries (`osdCPU`); static-only packages exporting `osdCPU_static` also work. |
| MaterialX | 1.39.5 | Built by the same script (the version OpenUSD 26.08 builds against); PrismRT needs `libraries\` and `libraries\mdl\`. |
| oneTBB | 2022.2.0 | Binary release, shared by USD and PrismRT (`tbb12.dll`). |
| Embree | 4.4.0 | Intel binary release. |
| NVIDIA MDL SDK | 2026.0.2 | Built from source without CUDA, see below. Do not use the binary release zip. |
| OpenImageIO | 3.1.14 | vcpkg, installed into `build\vcpkg_installed\x64-windows`. |
| OpenColorIO | 2.6.0 | vcpkg (also an OpenImageIO dependency). Colour management ([color_management.md](color_management.md)); its built-in configs ship inside `OpenColorIO_2_6.dll`. OpenUSD's Hdx (usdview's OpenColorIO colour correction) links the same version, so one `OpenColorIO_2_6.dll`, vcpkg's, serves both. |

### OpenUSD, MaterialX and OpenSubdiv

```powershell
.\tools\build_deps_local.ps1 -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src -VcpkgRoot D:\prism-deps\vcpkg
```

builds the three with `tools/ci/build_usd_stack.ps1`, exactly as CI does, into
`D:\prism-deps\opensubdiv-3.6.1`, `materialx-1.39.5` and
`openusd-26.08-ocio2.6.0-p7e45c2c5` (about 100 minutes with `-Jobs 6`, almost
all of it OpenUSD; the work directory, about 20 GB, can be deleted afterwards;
`-p<hash>` names the OpenUSD patches from `tools/ci/patches` it applies, such
as the fix that lets usdview show the ACES 2.0 views). First it
installs vcpkg's OpenColorIO (`tools/ci/install_ocio.ps1`, from the vcpkg clone
at `deps.json`'s baseline) into `D:\prism-deps\opencolorio-2.6.0-vcpkg` for
OpenUSD to link. `-UsdOnly` rebuilds only OpenUSD against the existing
OpenSubdiv and MaterialX prefixes. Then point `cmake\PrismLocalDependencies.cmake` and
`prism_local_env.ps1` at those prefixes (see their `.example` files). See
[deployment.md](deployment.md#local-dependency-stack) for the options.

### MDL SDK: build from source, without CUDA

The binary release `MDL-SDK-2026.0.2-...-nt-x86-64.zip` links its GPU baker
statically: `libmdl_sdk.dll` imports 23 `cu*` functions from `nvcuda.dll`
(`dumpbin /dependents libmdl_sdk.dll`). On a machine without an NVIDIA driver
(GitHub runners, AMD or Intel PCs) Windows cannot load it and `LoadLibrary`
fails with error 126. PrismRT only uses the native (LLVM) and HLSL backends,
never PTX, so it builds the SDK itself with the GPU baker off:

```powershell
.\tools\ci\build_mdl_sdk.ps1 -Prefix C:\deps\mdl-sdk-2026.0.2-nocuda -WorkDir D:\mdl-work
```

The script (also used by CI) needs Visual Studio 2022, CMake, git, Python 3 and
7-Zip, and downloads the rest: the SDK at the tag, clang 12.0.1 (mandatory, it
compiles the LLVM 12 bitcode libraries; the LLVM inside the SDK is vendored),
vcpkg at the SDK's tested baseline with Boost headers and a static OpenImageIO
(`tools/ci/mdl_vcpkg/vcpkg.json`, triplet `x64-windows-static`). Roughly 10
minutes of vcpkg and 10 minutes of SDK build with 8 jobs, 10 GB of work files
(delete `-WorkDir` afterwards). CMake options that matter:

- `-DMDL_ENABLE_GPU_BAKER=OFF`: removes the only `nvcuda.dll` import.
- `-DMDL_ENABLE_CUDA_EXAMPLES=OFF`, `MDL_BUILD_SDK_EXAMPLES=OFF`,
  `MDL_BUILD_CORE_EXAMPLES=OFF`, `MDL_BUILD_DOCUMENTATION=OFF`,
  `MDL_ENABLE_UNIT_TESTS=OFF`, `MDL_ENABLE_PYTHON_BINDINGS=OFF`, the other
  `MDL_ENABLE_*_EXAMPLES` and `MDL_BUILD_ARNOLD_PLUGIN=OFF`: not needed.
- `-DMDL_BUILD_OPENIMAGEIO_PLUGIN=ON`: `nv_openimageio.dll`, which PrismRT loads.
- Release configuration; install with `cmake --install`.

The result has `bin\` with `libmdl_sdk.dll`, `libmdl_core.dll`,
`nv_openimageio.dll`, `dds.dll`, `mdl_distiller.dll` and the command line tools,
plus `include\`. The script fails if any file in `bin\` imports `nvcuda.dll`.
`prism_env.ps1` searches both `bin` and `nt-x86-64\lib`.

**The MDL SDK found at run time must be the one PrismRT was compiled against.**
Headers from one SDK version combined with `libmdl_sdk.dll` from another load
without an error, but they give wrong material evaluation;
`prism_hydra_materials_test` fails with "Reload mutated a retained binding". Set
`MDL_SDK_DIR` at configure time and `PRISM_MDL_SDK_ROOT` at run time to the same
SDK. Don't leave older SDK DLLs next to `PrismRT.exe`.

Copy `cmake\PrismLocalDependencies.cmake.example` to
`cmake\PrismLocalDependencies.cmake` (ignored by git) and fill in the roots.
Runtime overrides go in the ignored `prism_local_env.ps1`, which `prism_env.ps1`
loads first.

## Configure, build and test

```powershell
cmake --preset prism-hydra -DPRISM_ENABLE_VULKAN=ON "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DMDL_SDK_DIR=C:/deps/mdl-sdk-2026.0.2-nocuda
cmake --build --preset prism-hydra
. .\prism_env.ps1
.\build\RelWithDebInfo\PrismRT.exe --list-devices
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

- A full build takes about 10 minutes on a 28-thread machine.
- If CMake complains about a stale compiler after a Visual Studio update, delete
  `build\CMakeCache.txt` and `build\CMakeFiles`. Keep `build\vcpkg_installed`.
- `--list-devices` should print `vulkan: <GPU name> (Vulkan) (hardware ray tracing)`.
- Run ctest serially. Several Hydra tests use fixed file names in the temporary
  and cache directories, so concurrent runs interfere.
- Hydra tests choose the device with `auto`, which picks Vulkan when it is built.
  `PRISM_DEVICE=cpu` does not reach every test render. Set
  `PRISM_DISABLE_VULKAN=1` to test the CPU path only.

## Vulkan diagnostics

- Validation: `$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"`.
  - GPU-assisted validation: also set `VK_LAYER_GPUAV_ENABLE=true`.
  - Synchronization validation: needs a `vk_layer_settings.txt` with
    `khronos_validation.validate_sync = true`.
- `PRISM_GPU_VERBOSE=1` prints shader setup times (MDL code, SPIR-V, pipeline)
  and upload times (buffers, images, acceleration structures).
- The SPIR-V cache in `%LOCALAPPDATA%\PrismRT\vulkan` is keyed on the kernel
  source, not on the DXC flags. Clear it after changing those flags.
- The first render of a new material set compiles its shaders with DXC and
  the driver, cached afterwards. Most scenes compile one callable shader per
  MDL program, in parallel, plus a raygen shader that depends on no scene and
  compiles in the background from device creation (about 10 s the first time
  on a machine, then from its pipeline cache file): OpenChessSet's first GPU
  sample comes 10–11 s after start cold, against 52 s with the compute
  kernel (26 s DXC, 19 s driver). See
  [GPU devices](gpu_devices.md#material-programs-as-callable-shaders-vulkan).
- The SDK's DXC 1.8 (1.8.0.4973) is enough. Large kernels (over about 4M
  SPIR-V ids during legalization) need `-fspv-max-id`, which the device
  passes. Without it, DXC 1.8 fails with "Internal compiler error: stack
  overflow" and DXC 1.9.2609 crashes with an access violation. To use another
  DXC, set `PRISM_DXC` at run time or `-DPRISM_DXC_EXECUTABLE=<dxc.exe>` at
  configure time.
- If the kernel fails to compile, the device prints the DXC error once and
  renders the scene on the CPU device instead.
- Kernels above about 1 MB of HLSL (Stirling 1.4 MB, OpenPBR playground
  3.0 MB, MaterialEggs countertop 3.1 MB) are too large for DXC's SPIR-V
  legalization. It inlines everything into one function and its SSA rewrite
  grows superlinearly (more than 28 GB for OpenPBR). DXC is stopped at
  `PRISM_DXC_MEMORY_MIB` (default min(8 GiB, RAM/4)); these scenes use callable
  shaders, or render on the CPU on devices without ray tracing pipelines
  (and with `PRISM_VULKAN_CALLABLES=0`). A `.too-large` marker in the shader cache, keyed on the kernel
  source and the DXC command, skips the attempt on later runs; set
  `PRISM_VULKAN_RETRY_LARGE=1` to try again. Marking the material functions
  `[noinline]` makes DXC fast (OpenPBR
  in 45 s, 1.1 GB), but the NVIDIA 610.88 pipeline compile of that SPIR-V
  then needs more than 12 GB, and for OpenChessSet it took 41 s and lost the
  device at dispatch.
- Textures beyond the texture budget (`prism:gpuTextureBudgetMiB` /
  `--gpu-texture-budget-mib`, default `PRISM_GPU_TEXTURE_BUDGET_MIB`, else 45%
  of device memory) are uploaded at reduced resolution, reported as
  `PrismRT Vulkan: N of M images uploaded at reduced resolution to fit GPU memory (prism:gpuTextureBudgetMiB)`.
