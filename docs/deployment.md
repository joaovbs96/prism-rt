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
the MDL SDK DLLs (about 230 MiB), Python with PySide6 (about 270 MiB) and the
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

### CI

`.github/workflows/windows.yml` runs on pushes to `device-abstraction` and
`main`, pull requests and manual dispatch, on `windows-2022`. All dependencies
live on `D:`.

- `usd` job: builds OpenSubdiv 3.6.1, MaterialX 1.39.4 and OpenUSD 26.05 with
  `tools/ci/build_usd_stack.ps1`, mirroring the development machine (separate
  prefixes, `RelWithDebInfo`, oneTBB through `PXR_FIND_TBB_IN_CONFIG`, Python 3.11,
  imaging and usdview). The install, minus `.pdb` files, is cached under a key
  made of the versions and the hash of the two CI scripts. On a hit the job only
  does the cache lookup. It does not use `build_usd.py`, which would fetch its
  own TBB.
- `build` job: restores that cache, downloads oneTBB 2022.2.0, Embree 4.4.0 and
  the MDL SDK 2026.0.2 (`tools/ci/fetch_prebuilt.ps1`), installs the Vulkan SDK
  1.4.321.0 and the LunarG Vulkan runtime (the SDK has no `vulkan-1.dll`),
  installs OpenImageIO from the `vcpkg.json` manifest with a vcpkg binary cache,
  configures `prism-hydra` with `PRISM_ENABLE_VULKAN=ON` (using
  `cmake/PrismLocalDependencies.cmake` created from the `.example`), builds
  `RelWithDebInfo`, runs CTest serially with `PRISM_DISABLE_VULKAN=1` (hosted
  runners have no GPU), runs the package script, smoke-tests the extracted
  package from a clean environment on the CPU device, and uploads the zip as an
  artifact kept for 14 days. On tags `v*` the zip is also attached to a GitHub
  release.
- The first run builds everything (several hours, mostly OpenUSD); later runs
  reuse the caches and take roughly the PrismRT build time plus packaging.
  Change the versions in `windows.yml` and the scripts together; the cache key
  follows the script hashes.
