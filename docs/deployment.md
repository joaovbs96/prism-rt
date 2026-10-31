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
CTest also runs the CLI-vs-Hydra parity tests (`prism_parity_*`) and the
`validate.py`/`validate_exports.py` comparisons on the CPU device; they are
labelled `cpu`, so `ctest -LE gpu` includes them
([hydra.md](hydra.md#cli-and-hydra-parity)).
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
`-SkipPip` and `-SkipZip` shorten local iterations. The zip and `VERSION.txt`
are named after `-Commit <rev>` (default `HEAD`): the package is built from the
working tree and the existing build, so pass the commit that build was made
from (for example the last commit that changed code, when `HEAD` only touches
docs) instead of renaming the zip by hand.

The package contains `cmake --install` output (`PrismRT.exe`, the Hydra
delegate, the usdview extension, the sample scenes), the runtime DLLs of
OpenUSD, oneTBB, Embree, the MDL SDK (`mdl\bin`), vcpkg, the MSVC runtime and
the Vulkan loader, Open Image Denoise's CPU device (`OpenImageDenoise*.dll`,
when the build has the denoiser; it runs on the package's one `tbb12.dll`;
vk_oidn, the Vulkan denoiser, is linked into `PrismRT.exe` and the delegate
with its weights, so only its licenses ship), the MaterialX libraries, DXC (compiles the GPU kernel at run
time), and a private Python 3.11 with PySide6 (Essentials, 6.7.2), PyOpenGL,
numpy and PyOpenColorIO installed with pip into `python\Lib\site-packages`
(the PyOpenColorIO wheel without its own OpenColorIO DLL, see
[color_management.md](color_management.md)). `.pdb`, `.lib` and
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
is automatic; `PRISMRT_DEVICE=cpu` forces the CPU. `OCIO` is kept, and set to
`ocio://studio-config-latest` when unset, so usdview shows PrismRT's ACEScg
colour AOV through OpenColorIO (the config's default view, ACES 2.0 SDR on
sRGB like `prism_render.bat`'s images; it needs the patched OpenUSD, see
[color_management.md](color_management.md#usdview)). `usdview.bat` without a scene
opens the Cornell box through its camera; with only options it adds the same
scene; otherwise it passes its arguments to usdview. `README.txt` in the package
lists requirements and third-party notices.

To check a package, extract it to a path with a space and run the launchers with
a minimal `PATH`:
`list_devices.bat` (must list `vulkan` and `cpu`), `prism_render.bat` (writes
`cornell_box.png`) and `usdview.bat --quitAfterStartup` (logs "rendering on
vulkan device").

### Path length

Windows limits a path to 260 characters (`MAX_PATH`) unless long paths are on.
The package and the wheel carry files whose paths grow with the folder they are
extracted to, and a process that is not long-path aware cannot open them.
MaterialX reads its libraries with `std::ifstream`, so the visible failure is
material loading: `Failed to open file for reading: ...\usd\lib\usd\usdMtlx\resources\libraries\bxdf\lama\...mtlx`
(reproduced with the package extracted 188 characters deep; the scene file and
the DLLs, which are shorter, still load).

The longest file the renderer needs is 93 characters below the package root
(`\usd\lib\usd\usdMtlx\resources\libraries\stdlib\genglsl\mx_creatematrix_vector3_matrix44.glsl`),
the longest `.mtlx` 91, and 114 with the private Python's PySide6. So the
package must be extracted to a folder of at most about 166 characters (145 for
usdview), e.g. `C:\PrismRT`; a `Downloads` folder under a long user name or a
OneDrive path usually fits, a deeply nested project folder may not.

What PrismRT does about it:

- **Check at start.** `PrismRT.exe`, the Python package (`import prismrt`) and
  the Hydra delegate (when usdview creates it) compare the install path with
  the limit and, when it would be exceeded and long paths are not available to
  the process, stop (the delegate: log an error) with one message: the install
  path is too long; move the folder to a shorter path or enable long paths.
  The check reads `bin\prism_paths.txt` (`root`, `required`, `all`: the longest
  relative paths, written by `package_windows.ps1` and `build_wheel.py`), so it
  costs one small file read and no directory walk. Build trees and plain
  `cmake --install` prefixes have no such file and are not checked.
  `PRISM_SKIP_PATH_CHECK=1` turns it off.
- **Manifest.** `PrismRT.exe` and `prism_hydra_render.exe` carry a
  `longPathAware` application manifest (`resources/windows/prism.manifest`,
  `cmake/PrismLongPaths.cmake`), so with long paths enabled they open
  long paths. The bundled `python.exe` has its own.

To enable long paths (administrator, then restart or sign out): set the DWORD
`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled` to 1, from
an elevated PowerShell:

```powershell
Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem LongPathsEnabled 1
```

or with the group policy "Enable Win32 long paths". The setting only takes
effect for executables whose manifest says `longPathAware`; `cmd.exe` and
Explorer's zip extraction are not, so extract with `tar -xf` or 7-Zip when the
zip's own paths are long.

## Python wheel

`tools/build_wheel.py --build-dir <build>` turns a built tree into
`prismrt-<version>-cp311-cp311-win_amd64.whl` (the `prismrt` package with its
DLLs, the MaterialX libraries, a USD build for `prismrt.usd` and the
licences). Contents, size and limits: [python.md](python.md#wheel); the
`prism_python_wheel_install` test installs it into a clean venv.

## Dependency versions

`tools/deps.json` is the only list of dependency versions: OpenUSD, MaterialX
and OpenSubdiv tags, oneTBB, Embree, Open Image Denoise (version, source and
ISPC checksums), vk_oidn (version, tag commit, the SDK package's and
`pack_weights.py`'s checksums), the MDL SDK (tag, release build, clang
and the SDK's vcpkg baseline), Python and the PySide6/PyOpenGL/numpy/opencolorio pins, the
Vulkan SDK, PrismRT's vcpkg baseline (OpenImageIO, OpenColorIO), and `stack_revision`, a
counter that forces CI to rebuild its cached stacks. Its consumers:

- `tools/deps.psm1`: `Get-PrismDeps` and `Get-PrismCacheKey -Stack usd|mdl`
  (the versions, `stack_revision`, the runner image and a hash of the build
  scripts and, for `usd`, of the OpenUSD patches), used by every script below.
  `Get-PrismUsdPatches` lists the patches for `deps.json`'s OpenUSD version
  (`tools/ci/patches/openusd-<version>-*.patch`) and `Get-PrismUsdPatchHash`
  hashes them.
- `tools/ci/export_deps.ps1`: exports the versions and cache keys to each CI job
  (`GITHUB_ENV` and step outputs, including `setup-python`'s version).
  `windows.yml` has no version literals.
- `tools/ci/build_usd_stack.ps1`, `fetch_prebuilt.ps1`, `build_mdl_sdk.ps1`, `build_oidn.ps1`,
  `fetch_vk_oidn.ps1`,
  `tools/build_deps_local.ps1` and `tools/package_windows.ps1`.
- `cmake/PrismDeps.cmake`: the configure reports the OpenUSD, MaterialX,
  OpenSubdiv, oneTBB, Embree, OpenImageIO, OpenColorIO, Python, MDL SDK and (when enabled) Open Image
  Denoise and vk_oidn versions it found
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
   For OpenUSD, check each `tools/ci/patches/openusd-<old version>-*.patch`:
   the new version no longer applies it. Delete the ones the new release
   fixes (each patch's header names the upstream issue or PR); rebase the
   others onto the new tag as `openusd-<new version>-*.patch` (`git diff`
   against the tag). The build stops when a patch does not apply.
3. Rebuild the local stack: `tools/build_deps_local.ps1` (OpenUSD, MaterialX,
   OpenSubdiv) builds into new versioned prefixes next to the old ones; point
   `cmake/PrismLocalDependencies.cmake` and `prism_local_env.ps1` at them.
   For OpenImageIO, rerun vcpkg against the manifest (below). A new vcpkg
   baseline or OpenColorIO version also needs OpenUSD rebuilt against it
   (`-UsdOnly`); CI's OpenUSD cache key changes with both.
4. Reconfigure (the version check confirms the new stack is the one found),
   build, and run `ctest -j 1`.
5. Commit and push. The cache keys change with the versions, so CI rebuilds
   the affected stack on its first run. Bump `stack_revision` to force a rebuild
   without a version change.

### Local dependency stack

`tools/build_deps_local.ps1` builds OpenSubdiv, MaterialX and OpenUSD with the
same `tools/ci/build_usd_stack.ps1` as CI (oneTBB through
`PXR_FIND_TBB_IN_CONFIG=ON`, no legacy TBB headers, Python from `deps.json`,
imaging, usdview, MaterialX and OpenColorIO support, `RelWithDebInfo`) into
`<Root>\opensubdiv-<version>`, `<Root>\materialx-<version>` and
`<Root>\openusd-<version>-ocio<OpenColorIO version>[-p<patch hash>]`:

```powershell
.\tools\build_deps_local.ps1 -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src -VcpkgRoot D:\prism-deps\vcpkg
```

OpenUSD's OpenColorIO support (`PXR_BUILD_OPENCOLORIO_PLUGIN`, used by Hdx's
colour correction in usdview and usdrecord) links the OpenColorIO of
PrismRT's `vcpkg.json`. `tools/ci/install_ocio.ps1` installs the `opencolorio`
port, in classic mode, from a vcpkg clone at `deps.json`'s baseline
(`-VcpkgRoot`, default `<Root>\vcpkg`) into `<Root>\opencolorio-<version>-vcpkg`;
`-OcioRoot` takes an existing `x64-windows` prefix from that baseline instead.
The port, triplet and compiler are those of the manifest install, so the
package ABI is the same and a vcpkg binary cache serves both. OpenUSD only
links the import library: at run time the `OpenColorIO_2_6.dll` of PrismRT's
vcpkg install is the one loaded, and the OpenUSD install has none. After the
install, `build_usd_stack.ps1` replaces the import library's absolute path in
`pxrTargets*.cmake` with `OpenColorIO::OpenColorIO` and adds
`find_dependency(OpenColorIO ...)` to `pxrConfig.cmake`, so PrismRT links the
OpenColorIO it found itself, and checks that `usd_hdx.dll` imports
`OpenColorIO_2_6.dll`.

OpenUSD is patched: after cloning, `build_usd_stack.ps1` applies every
`tools/ci/patches/openusd-<version>-*.patch` for `deps.json`'s OpenUSD version
(`git apply --check`, then `git apply`; the clone uses `core.autocrlf=false`
so the LF patches apply on any Git setup) and fails if one does not apply. It
copies them to `<prefix>\patches`, and the package ships them next to
OpenUSD's licence. The local prefix carries their short hash
(`Get-PrismUsdPatchHash`), so a patched build never replaces an unpatched one:
OpenUSD 26.08 with the Hdx OpenColorIO LUT fix
([color_management.md](color_management.md#usdview)) is
`openusd-26.08-ocio2.6.0-p7e45c2c5`. A version without patches has no suffix.

`-UsdOnly` rebuilds OpenUSD alone against the existing OpenSubdiv and
MaterialX prefixes, for example after an OpenColorIO change:

```powershell
.\tools\build_deps_local.ps1 -UsdOnly -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src-usd
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
  minus `.pdb` files, is cached under `Get-PrismCacheKey -Stack usd`, which
  includes the OpenColorIO version, PrismRT's vcpkg baseline and the OpenUSD
  patches (`usd-26.08-p<patch hash>-...`, and in the files hash). On a hit
  the job only does the cache lookup. Otherwise it restores the `build` job's
  vcpkg binary cache (read-only) and installs vcpkg's OpenColorIO with
  `tools/ci/install_ocio.ps1` for OpenUSD to link. It does not use
  `build_usd.py`, which would fetch its own TBB (and build its own
  OpenColorIO).
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

## Older dependency stacks (best effort)

A host application (a DCC tool that loads its own USD, TBB and OpenImageIO
into one process) may force hdPrism onto older libraries than `tools/deps.json`
names. Only the `deps.json` versions are tested by CI and by
`tools/validate_install.ps1`. The older ones below build with
`-DPRISM_STRICT_DEPENDENCY_VERSIONS=OFF` (the configure then warns about each
mismatch instead of stopping) and are checked as stated; nothing here is run in
CI.

| Dependency | Tested (`deps.json`) | Best effort | What was checked |
|---|---|---|---|
| OpenImageIO | the vcpkg version | 2.5.x (source compatible, gated on `OIIO_VERSION`) | every translation unit of the core library compiled (`/Zs`) against the 2.5.18 headers, before and after the change; the unchanged sources fail to compile there. Not linked or run: building OIIO 2.5 itself needs Boost, which this stack does not have. <!-- deps: historical --> |
| oneTBB | the `deps.json` version | classic TBB 2020.x (headers) | every translation unit compiled against the official 2020.3 Windows release headers with no source change. Not linked, and not built against USD. <!-- deps: historical --> |
| OpenColorIO | the vcpkg version | 2.2.x | `src/color/color_management.cpp` compiled against the 2.2.1 headers. <!-- deps: historical --> |
| MaterialX | the `deps.json` version | none: other versions run with a warning | the `materialx_loader_*` tests pass on the tested version only (below). <!-- deps: historical --> |
| Open Image Denoise | the `deps.json` version | 2.3.x | `src/device/denoiser.cpp` compiled against the 2.3.3 headers. The Vulkan denoiser carries its own weights and is independent. <!-- deps: historical --> |

### Older OpenImageIO (2.5)

The changes are in `src/texture_service.cpp` and `src/texture_writer.cpp`.
The version boundaries come from the OIIO headers of 2.5.19.1 (the last 2.5)
and of 3.0.0.0-beta1 to 3.0.0.3: `TextureSystem::create` returns a shared
pointer and `IOProxy::tell()` becomes `const` (and `flush()` non-`const`)
from 3.0.0.0-beta1 on, and there is no 2.6 or 2.7. `texture.h` defines
`OIIO_TEXTURESYSTEM_CREATE_SHARED` for the first change; the second uses
`OIIO_VERSION >= OIIO_MAKE_VERSION(3, 0, 0)`. `TextureOpt::WrapPeriodicPow2`
exists in both generations, so it needs no gate. Versions older than 2.5 are
not checked.

### Classic TBB

Prism's TBB use (`parallel_for`, `parallel_reduce`, `blocked_range`,
`task_arena`, `task_group`) is in classic TBB too. Its headers include
`windows.h`, so Prism defines `NOMINMAX` for every target on Windows (without
it `std::min`/`std::max` break across Prism, USD and OpenImageIO headers).

To point the build at classic TBB, use the shim in `cmake/compat/classic-tbb`.
It loads the `TBBConfig.cmake` that ships with the classic release and adds
`NOMINMAX` to `TBB::tbb`:

```
cmake -B build -DPRISM_STRICT_DEPENDENCY_VERSIONS=OFF ^
  -DTBB_DIR=<prism>/cmake/compat/classic-tbb -DPRISM_CLASSIC_TBB_ROOT=<tbb-2020.3>/tbb ...
```

`<tbb-2020.3>/tbb` is the directory of the official `oneapi-src/oneTBB`
v2020.3 Windows zip that holds `include/`, `bin/` and `cmake/`. A host that
already has its own TBB package config can use that instead; any config that
defines `TBB::tbb` works. Set `PRISM_TBB_REDIST` to the classic `bin/intel64/vc14`
directory so the runtime is found.

USD's exported `TBB::tbb` sets `TBB_USE_DEBUG=1` for `RelWithDebInfo`, which
classic TBB's headers turn into a link to the debug runtime. A stack built on
classic TBB therefore has to build USD and Prism as `Release`.

### MaterialX

`DocumentLoader` (`src/materialx/loader.cpp`) repairs graphs as the 1.39.5
library writes them. The range it was tested against is
`DocumentLoader::kTestedMaterialXMin`/`kTestedMaterialXMax`, one place in
`loader.h`. Outside it, the first load in the process prints one
`[MaterialX] WARNING` saying the repairs may not apply or may apply wrongly;
nothing else changes. The range is the single version 1.39.5: the standalone
`tests/` project (the `materialx_loader_*` tests) was also built against
1.39.3 and 1.39.4. On 1.39.3 `materialx_loader_dielectric` fails (no Fresnel
layer found on glTF clearcoat) and on 1.39.4 `materialx_loader_dielectric`
and `materialx_loader_eon` fail, so neither is in the range. Renders on those
versions were not compared.
