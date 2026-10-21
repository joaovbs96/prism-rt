<#
.SYNOPSIS
  Builds the NVIDIA MDL SDK from source without any CUDA dependency and installs
  libmdl_sdk, libmdl_core, nv_openimageio, dds, mdl_distiller and the headers.

.DESCRIPTION
  The binary MDL SDK 2026.0.x release statically imports nvcuda.dll (the GPU baker),
  so libmdl_sdk.dll fails to load (error 126) on machines without an NVIDIA driver.
  PrismRT only uses the native and HLSL backends, so this script builds the SDK with
  MDL_ENABLE_GPU_BAKER=OFF, which removes the CUDA driver import.

  Needs Visual Studio 2022, CMake, git, Python 3 and 7-Zip on PATH. Everything else
  is fetched: the MDL SDK at the tag, clang 12.0.1 (mandatory: it compiles the LLVM 12
  bitcode libraries), and vcpkg at the baseline the SDK was tested with (Boost headers
  and a static OpenImageIO for the image plugin, from tools/ci/mdl_vcpkg/vcpkg.json).

  The tag, clang version and vcpkg baseline come from tools/deps.json (mdl_sdk);
  the workflow's cache key is derived from them. Layout of -Prefix: bin\ with
  the DLLs and tools, include\, LICENSE*.md. Work files go to -WorkDir, which can be
  deleted afterwards (~10 GB).
#>
param(
    [Parameter(Mandatory)] [string] $Prefix,
    [Parameter(Mandatory)] [string] $WorkDir,
    [string] $Configuration = "Release",
    [int] $Jobs = 0
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$deps = Get-PrismDeps
$MdlTag = $deps.mdl_sdk.tag
$VcpkgBaseline = $deps.mdl_sdk.vcpkg_baseline
$ClangVersion = $deps.mdl_sdk.clang
$ProgressPreference = "SilentlyContinue"
function Run([string] $what, [scriptblock] $block) {
    Write-Host "::group::$what"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $block
    if ($LASTEXITCODE) { throw "$what failed: $LASTEXITCODE" }
    Write-Host ("{0} took {1:N0} s" -f $what, $sw.Elapsed.TotalSeconds)
    Write-Host "::endgroup::"
}
function Fwd([string] $path) { return $path.Replace('\', '/') }
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$total = [Diagnostics.Stopwatch]::StartNew()
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$Prefix = [IO.Path]::GetFullPath($Prefix)
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$ciDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $WorkDir "MDL-SDK"
$build = Join-Path $WorkDir "build"

# 1. Source at the tag. The LFS payloads are examples and test data, so skip them.
if (-not (Test-Path -LiteralPath (Join-Path $src "CMakeLists.txt"))) {
    if (Test-Path -LiteralPath $src) { Remove-Item -LiteralPath $src -Recurse -Force }
    $env:GIT_LFS_SKIP_SMUDGE = "1"
    Run "clone MDL SDK $MdlTag" {
        git clone --quiet --depth 1 --branch $MdlTag https://github.com/NVIDIA/MDL-SDK.git $src
    }
}

# 2. clang 12.0.1 from the LLVM installer (extracted, not installed).
$clangRoot = Join-Path $WorkDir "clang-$ClangVersion"
$clang = Join-Path $clangRoot "bin\clang.exe"
if (-not (Test-Path -LiteralPath $clang)) {
    $installer = Join-Path $WorkDir "LLVM-$ClangVersion-win64.exe"
    Run "download clang $ClangVersion" {
        Invoke-WebRequest -Uri "https://github.com/llvm/llvm-project/releases/download/llvmorg-$ClangVersion/LLVM-$ClangVersion-win64.exe" -OutFile $installer -MaximumRetryCount 3 -RetryIntervalSec 5
        $global:LASTEXITCODE = 0
    }
    $sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
    if (-not $sevenZip) { $sevenZip = "C:\Program Files\7-Zip\7z.exe" }
    Run "extract clang $ClangVersion" {
        & $sevenZip x $installer "-o$clangRoot" -y "bin/clang.exe" "lib/clang/*" '-x!$PLUGINSDIR' | Out-Null
    }
    Remove-Item -LiteralPath $installer -Force
}
& $clang --version | Select-Object -First 1 | Write-Host
if ((& $clang --version | Select-Object -First 1) -notmatch [regex]::Escape($ClangVersion)) {
    throw "clang $ClangVersion expected at $clang"
}

# 3. vcpkg at the baseline, Boost headers and OpenImageIO as static libraries.
$vcpkgRoot = Join-Path $WorkDir "vcpkg-src"
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
if (-not (Test-Path -LiteralPath $vcpkgExe)) {
    if (Test-Path -LiteralPath $vcpkgRoot) { Remove-Item -LiteralPath $vcpkgRoot -Recurse -Force }
    Run "clone vcpkg $VcpkgBaseline" {
        git init --quiet $vcpkgRoot
        git -C $vcpkgRoot fetch --quiet --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgBaseline
        git -C $vcpkgRoot checkout --quiet FETCH_HEAD
    }
    Run "bootstrap vcpkg" { & (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics }
}
$vcpkgInstalled = Join-Path $WorkDir "vcpkg_installed"
$triplet = "x64-windows-static"
$env:VCPKG_MAX_CONCURRENCY = "$Jobs"
Run "vcpkg install (Boost, OpenImageIO)" {
    & $vcpkgExe install --triplet $triplet --disable-metrics --clean-after-build `
        "--x-manifest-root=$(Join-Path $ciDir 'mdl_vcpkg')" "--x-install-root=$vcpkgInstalled" `
        "--downloads-root=$(Join-Path $WorkDir 'vcpkg_downloads')" `
        "--x-buildtrees-root=$(Join-Path $WorkDir 'vcpkg_buildtrees')" `
        "--x-packages-root=$(Join-Path $WorkDir 'vcpkg_packages')"
}

# Build trees and package copies are ~9 GB; the install tree is all that is needed.
Remove-Item -LiteralPath (Join-Path $WorkDir "vcpkg_buildtrees"), (Join-Path $WorkDir "vcpkg_packages") -Recurse -Force -ErrorAction SilentlyContinue

# 4. Configure. MDL_ENABLE_GPU_BAKER=OFF is what drops the nvcuda.dll import; the rest
#    only skips what PrismRT does not ship.
$python = (Get-Command python).Source
Run "configure MDL SDK" {
    cmake -S $src -B $build -G "Visual Studio 17 2022" -A x64 -T host=x64 `
        "-DCMAKE_TOOLCHAIN_FILE=$(Fwd (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'))" `
        -DVCPKG_MANIFEST_MODE=OFF "-DVCPKG_INSTALLED_DIR=$(Fwd $vcpkgInstalled)" "-DVCPKG_TARGET_TRIPLET=$triplet" `
        "-Dclang_PATH=$(Fwd $clang)" "-Dpython_PATH=$(Fwd $python)" `
        "-DCMAKE_INSTALL_PREFIX=$(Fwd $Prefix)" `
        -DMDL_ENABLE_GPU_BAKER=OFF -DMDL_ENABLE_CUDA_EXAMPLES=OFF `
        -DMDL_BUILD_SDK_EXAMPLES=OFF -DMDL_BUILD_CORE_EXAMPLES=OFF `
        -DMDL_ENABLE_D3D12_EXAMPLES=OFF -DMDL_ENABLE_OPENGL_EXAMPLES=OFF -DMDL_ENABLE_VULKAN_EXAMPLES=OFF `
        -DMDL_ENABLE_OPTIX7_EXAMPLES=OFF -DMDL_ENABLE_QT_EXAMPLES=OFF -DMDL_ENABLE_AXF_EXAMPLES=OFF `
        -DMDL_ENABLE_MATERIALX=OFF -DMDL_BUILD_ARNOLD_PLUGIN=OFF `
        -DMDL_BUILD_DOCUMENTATION=OFF -DMDL_ENABLE_UNIT_TESTS=OFF -DMDL_ENABLE_PYTHON_BINDINGS=OFF `
        -DMDL_BUILD_OPENIMAGEIO_PLUGIN=ON
}

# 5. Build and install.
Run "build MDL SDK ($Configuration)" {
    cmake --build $build --config $Configuration --parallel $Jobs
}
Run "install MDL SDK" { cmake --install $build --config $Configuration }

# The install tree has no license files; PrismRT's package script looks for them.
foreach ($n in @("LICENSE.md", "LICENSE_IMAGES.md")) {
    Copy-Item -LiteralPath (Join-Path $src $n) -Destination $Prefix -Force
}
Copy-Item -LiteralPath (Join-Path $src "LICENSE_THIRD_PARTY.md") -Destination (Join-Path $Prefix "LICENSE_THIRDPARTY.md") -Force

# 6. Guard: the point of this build is that nothing imports the CUDA driver.
$dumpbin = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe not found; cannot verify the build has no nvcuda import." }
$bad = @()
Get-ChildItem (Join-Path $Prefix "bin") -Include *.dll, *.exe -Recurse | ForEach-Object {
    $deps = & $dumpbin.FullName /dependents $_.FullName
    if ($deps -match 'nvcuda') { $bad += $_.Name }
}
if ($bad) { throw "nvcuda.dll is still imported by: $($bad -join ', ')" }
Get-ChildItem (Join-Path $Prefix "bin") | ForEach-Object { Write-Host ("{0,-28} {1,12:N0}" -f $_.Name, $_.Length) }
Write-Host ("MDL SDK $MdlTag built without CUDA in {0:N1} min -> $Prefix" -f $total.Elapsed.TotalMinutes)
