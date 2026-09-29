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
