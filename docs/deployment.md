# Building and installing PrismRT

PrismRT discovers OpenUSD, its OpenSubdiv dependency, oneTBB, Embree, OpenImageIO
and MaterialX through their CMake package configs and `CMAKE_PREFIX_PATH`.
OpenUSD builds with imaging enabled require the matching OpenSubdiv development
package to remain discoverable by downstream CMake projects. A machine can store package
hints in `cmake/PrismLocalDependencies.cmake`; copy
`cmake/PrismLocalDependencies.cmake.example` and set the roots for that machine.
The local file is ignored by git. `MDL_SDK_DIR` is optional and can be supplied
the same way.

Configure and build the Hydra delegate with `cmake --preset prism-hydra`, then
install with `cmake --install build --config RelWithDebInfo --prefix <prefix>`.
The installed delegate is under `lib/usd/plugins/prism`; point
`PXR_PLUGINPATH_NAME` at its `resources` directory. The Python usdview extension
is installed beside it under `usdview_plugins`. On Windows, put external SDK
runtime directories on `PATH` and `PXR_USD_WINDOWS_DLL_PATH`. Prism does not
bundle OpenUSD or its external runtime dependencies. Large model libraries are
not installed by default; enable `PRISM_INSTALL_ASSETS` to install the smaller
environment, material and texture directories.
The small exported examples under `assets/scenes` are installed independently
of that option.

Set these optional environment variables when dependency package configs are
not on the normal CMake search path: `PRISM_OPENUSD_ROOT`,
`PRISM_OPENSUBDIV_ROOT`, `PRISM_TBB_ROOT`, `PRISM_MATERIALX_ROOT`,
`PRISM_EMBREE_ROOT`, `PRISM_MDL_SDK_ROOT`,
`PRISM_PYTHON_ROOT`, `PRISM_TBB_REDIST`, and
`PRISM_VCPKG_BIN`. `PRISM_USD_SOURCE_ROOT` points to a source checkout when its
`testusdview` script is needed, or to launch usdview through its Python entry
point when the installation's Windows wrapper is unavailable. `PRISM_USDVIEW`
can explicitly select a launcher or Python entry point. The PowerShell launch helpers read those
variables and an ignored `prism_local_env.ps1` when one exists.

`tools/validate_install.ps1` builds a fresh Hydra preset, runs CTest and
standalone/Hydra image comparisons, installs into a separate prefix, relocates
it, and runs its CLI plus a packaged texture-service fixture. That fixture
checks filtered and bilinear checksums across fresh processes using only the
relocated executable directory and configured external SDK runtimes; remaining
build-tree runtime paths outside those SDK roots fail validation. The script
then runs the usdview lifecycle script against the installed plugin. Use `-SkipUsdview`
when `testusdview` is unavailable. The Python CLI, export and instance-binding
checks use the configured Python interpreter with `OpenImageIO` and `numpy`
available, and require the Hydra render helper produced by the build.
Pass `-BuildDirectory build-clean-validation` to select a new build directory
without inheriting a previous CMake cache. The override must be inside the
checkout; external SDK installations are reused. The directory can contain
spaces.
After correcting a failed check, `-Resume` keeps that build directory and
rebuilds changed targets before rerunning validation. Its result is explicitly
reported as a resumed build, rather than a new clean-build measurement.

## Windows demo package

`tools/package_windows.ps1` turns an existing build into a self-contained zip,
`PrismRT-windows-x64-<git short sha>.zip`, for demos. A user extracts it
anywhere (spaces are fine) and double-clicks `usdview.bat`, which opens the
bundled Cornell box in usdview with the PrismRT renderer. No installation or
environment variables are needed.

```powershell
.\tools\package_windows.ps1 -OutputDir local\package
```

The dependency roots default from the same `PRISM_*` variables as
`prism_env.ps1`, then from `prism_local_env.ps1`, then from
`build\prism-runtime.json`. Each can be overridden with a parameter
(`-UsdRoot`, `-MaterialXRoot`, `-TbbRuntime`, `-EmbreeRuntime`, `-MdlSdkRoot`,
`-VcpkgBin`, `-PythonRoot`, `-DxcDir`, `-VulkanLoader`, `-UsdviewScript`).
`-SkipPip` and `-SkipZip` shorten local iterations.

The package contains `cmake --install` output (`PrismRT.exe`, the Hydra
delegate, the usdview extension, the sample scenes), the runtime DLLs of
OpenUSD, oneTBB, Embree, the MDL SDK (`mdl\bin`), vcpkg, the MSVC runtime and
the Vulkan loader, the MaterialX libraries, DXC (compiles the GPU kernel at run
time), and a private Python 3.11 with PySide6 (Essentials, 6.7.2), PyOpenGL and
numpy installed with pip into `python\Lib\site-packages`. `.pdb`, `.lib` and
headers are left out. The extracted size is about 730 MiB (zip about 280 MiB):
the MDL SDK DLLs (about 230 MiB; built from source without CUDA, so none of
them imports `nvcuda.dll`), Python with PySide6 (about 270 MiB) and the
OpenUSD DLLs (about 160 MiB) dominate.

The launchers (`usdview.bat`, `prism_render.bat`, `list_devices.bat`; shared
setup in `env_setup.bat`, templates in `tools/package/`) use only paths relative
to themselves. They first clear inherited `PRISM_*`, `PXR_*`, `PYTHON*`, `QT_*`
and `MDL_SDK_ROOT`, replace `PATH` with the package directories plus
`System32`, and then set what usdview needs. Shader and material caches live in
`%LOCALAPPDATA%\PrismRT\cache` (set `PRISMRT_CACHE_DIR` to move them). The device
is automatic; `PRISMRT_DEVICE=cpu` forces the CPU. `usdview.bat` without a scene
opens the Cornell box through its camera; with only options it adds the same
scene; otherwise it passes its arguments to usdview. `README.txt` in the package
lists requirements and third-party notices.

To check a package, extract it to a path with a space and run the launchers with
a minimal `PATH`:
`list_devices.bat` (must list `vulkan` and `cpu`), `prism_render.bat` (writes
`cornell_box.png`) and `usdview.bat --quitAfterStartup` (logs "rendering on
vulkan device").

## Dependency versions

`tools/deps.json` is the only list of dependency versions: OpenUSD, MaterialX
and OpenSubdiv tags, oneTBB, Embree, the MDL SDK (tag, release build, clang
and the SDK's vcpkg baseline), Python and the PySide6/PyOpenGL/numpy pins, the
Vulkan SDK, PrismRT's vcpkg baseline (OpenImageIO), and `stack_revision`, a
counter that forces CI to rebuild its cached stacks. Its consumers:

- `tools/deps.psm1`: `Get-PrismDeps` and `Get-PrismCacheKey -Stack usd|mdl`
  (the versions, `stack_revision`, the runner image and a hash of the build
  scripts), used by every script below.
- `tools/ci/export_deps.ps1`: exports the versions and cache keys to each CI job
  (`GITHUB_ENV` and step outputs, including `setup-python`'s version).
  `windows.yml` has no version literals.
- `tools/ci/build_usd_stack.ps1`, `fetch_prebuilt.ps1`, `build_mdl_sdk.ps1`,
  `tools/build_deps_local.ps1` and `tools/package_windows.ps1`.
- `cmake/PrismDeps.cmake`: the configure reports the OpenUSD, MaterialX,
  OpenSubdiv, oneTBB, Embree, OpenImageIO, Python and MDL SDK versions it found
  and, on Windows, fails when one differs from `deps.json`
  (`-DPRISM_STRICT_DEPENDENCY_VERSIONS=OFF` turns that into a warning, the
  default on other platforms, whose stacks may come from system packages). It also
  finds `lib/cmake/embree-<version>` for the `.example` file.
- `tools/check_deps.py` (CTest `prism_deps_consistency_test`, and a CI step):
  `vcpkg.json` and `tools/ci/mdl_vcpkg/vcpkg.json` repeat their baselines,
  because vcpkg manifests cannot import JSON, and this check fails when they
  differ (`--write` updates them). It also fails when `docs/windows_vulkan.md`,
  this file, `docs/hydra.md`, `README.md` or the package README name another
  version of a dependency.

### How to upgrade a dependency

1. Edit the version (and tag) in `tools/deps.json`. For OpenImageIO, set
   `vcpkg.baseline` to a vcpkg commit or release tag whose port has the new
   version, and `vcpkg.openimageio` to that version.
2. Run `python tools/check_deps.py --write`. It updates the vcpkg manifests
   and lists every doc line that still names the old version; fix those.
3. Rebuild the local stack: `tools/build_deps_local.ps1` (OpenUSD, MaterialX,
   OpenSubdiv) builds into new versioned prefixes next to the old ones; point
   `cmake/PrismLocalDependencies.cmake` and `prism_local_env.ps1` at them.
   For OpenImageIO, rerun vcpkg against the manifest (below).
4. Reconfigure (the version check confirms the new stack is the one found),
   build, and run `ctest -j 1`.
5. Commit and push. The cache keys change with the versions, so CI rebuilds
   the affected stack on its first run. Bump `stack_revision` to force a rebuild
   without a version change.

### Local dependency stack

`tools/build_deps_local.ps1` builds OpenSubdiv, MaterialX and OpenUSD with the
same `tools/ci/build_usd_stack.ps1` as CI (oneTBB through
`PXR_FIND_TBB_IN_CONFIG=ON`, no legacy TBB headers, Python from `deps.json`,
imaging, usdview and MaterialX support, `RelWithDebInfo`) into
`<Root>\opensubdiv-<version>`, `<Root>\materialx-<version>` and
`<Root>\openusd-<version>`:

```powershell
.\tools\build_deps_local.ps1 -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src
```

`-DownloadTbb` fetches the oneTBB release instead of `-TbbRoot`. The developer's
Python is used as is (`-InstallPip` installs the pinned modules into it), the
`.pdb` files are kept (`-StripPdb`), and `-Jobs` (default 6) bounds the
compilers. The work directory takes about 20 GB and can be deleted afterwards.
OpenUSD 26.08 installs its Python modules into `Lib\site-packages` by default;
the script keeps the `lib\python` layout that `prism_env.ps1` and the package
expect.

OpenImageIO comes from the `vcpkg.json` manifest; with vcpkg checked out at
`deps.json`'s baseline:

```powershell
vcpkg install --triplet x64-windows --x-manifest-root=. --x-install-root=build\vcpkg_installed
```

### CI

`.github/workflows/windows.yml` runs on pushes to `device-abstraction` and
`main`, and on manual dispatch, on `windows-2022`. All dependencies live on
`D:`. Each job starts with `tools/ci/export_deps.ps1`.

- `usd` job: builds OpenSubdiv, MaterialX and OpenUSD with
  `tools/ci/build_usd_stack.ps1`, like the development machine. The install,
  minus `.pdb` files, is cached under `Get-PrismCacheKey -Stack usd`. On a hit
  the job only does the cache lookup. It does not use `build_usd.py`, which
  would fetch its own TBB.
- `mdl` job (parallel to `usd`): builds the NVIDIA MDL SDK from source without
  CUDA with `tools/ci/build_mdl_sdk.ps1` (the binary release imports
  `nvcuda.dll` and cannot load on a runner without an NVIDIA driver; see
  `docs/windows_vulkan.md`). Its cache key (`-Stack mdl`) hashes that script and
  `tools/ci/mdl_vcpkg/vcpkg.json`, so it never invalidates the OpenUSD cache and
  vice versa.
- `build` job: runs `tools/check_deps.py`, restores both caches, downloads oneTBB
  and Embree (`tools/ci/fetch_prebuilt.ps1 -Components tbb,embree`), installs
  the Vulkan SDK and the LunarG Vulkan runtime (the SDK has no `vulkan-1.dll`),
  installs OpenImageIO from the `vcpkg.json` manifest with a vcpkg binary
  cache, configures `prism-hydra` with `PRISM_ENABLE_VULKAN=ON` (using
  `cmake/PrismLocalDependencies.cmake` created from the `.example`), builds
  `RelWithDebInfo`, runs CTest serially with `PRISM_DISABLE_VULKAN=1` (hosted
  runners have no GPU), runs the package script, smoke-tests the extracted
  package from a clean environment on the CPU device, and uploads the zip as an
  artifact kept for 14 days. On tags `v*` the zip is also attached to a GitHub
  release.
- The first run builds everything (several hours, mostly OpenUSD; the MDL SDK
  adds about an hour on a 4-core runner, in parallel with the USD job); later runs
  reuse the caches and take roughly the PrismRT build time plus packaging.

CTest labels every test `cpu` or `gpu`; GPU tests also share
`RESOURCE_LOCK gpu`. `ctest -LE gpu` runs only the CPU tests, so it can run
beside GPU work; within one ctest run the lock keeps GPU tests from overlapping.
Keep `-j 1` for full runs, since some Hydra tests share temporary file names.
