<#
.SYNOPSIS
  Builds Intel Open Image Denoise (OIDN) with only its CPU device, from Intel's
  source release, against PrismRT's oneTBB.

.DESCRIPTION
  Versions and checksums come from tools/deps.json (oidn). The script
  downloads, from Intel's GitHub releases only,
    https://github.com/RenderKit/oidn/releases/download/v<version>/oidn-<version>.src.tar.gz
    https://github.com/ispc/ispc/releases/download/v<ispc>/ispc-v<ispc>-windows.zip
  verifies their SHA-256 and builds Release into -Prefix
  (bin\OpenImageDenoise*.dll, lib\cmake\OpenImageDenoise-<version>).

  Why not Intel's prebuilt Windows package: it is built against oneTBB 2023.1
  (TBB interface 12190) and ships that tbb12.dll. PrismRT's process already
  loads oneTBB 2022.2 (interface 12160); Windows reuses an already loaded
  tbb12.dll, so the prebuilt CPU device would run on an older runtime than it
  was compiled for, which oneTBB does not support (only the reverse). Built
  here, OIDN uses the same oneTBB as the renderer and there is one tbb12.dll
  (docs/DECISIONS.md, "Denoising: OIDN built from source").

  The CPU device only: no SYCL, CUDA, HIP (part 2 uses vk_oidn on Vulkan) and
  no RTLightmap weights. The build tree under -WorkDir can be deleted
  afterwards (~1 GB).

.EXAMPLE
  ./tools/ci/build_oidn.ps1 -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src -Prefix D:\prism-deps\oidn-2.5.1
#>
param(
    # oneTBB binary release root (include\oneapi\tbb\version.h, lib\intel64).
    [Parameter(Mandatory)] [string] $TbbRoot,
    # Downloads, sources and the build tree.
    [Parameter(Mandatory)] [string] $WorkDir,
    [Parameter(Mandatory)] [string] $Prefix,
    # An ISPC release root (bin\ispc.exe); default: downloaded into -WorkDir.
    [string] $IspcRoot = "",
    [int] $Jobs = 4
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$oidn = (Get-PrismDeps).oidn
$ProgressPreference = "SilentlyContinue"
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$WorkDir = [IO.Path]::GetFullPath($WorkDir)

function Get-Verified([string] $url, [string] $file, [string] $sha256) {
    if (-not (Test-Path -LiteralPath $file)) {
        Write-Host "Downloading $url"
        if ($PSVersionTable.PSVersion.Major -ge 7) {
            Invoke-WebRequest -Uri $url -OutFile $file -MaximumRetryCount 3 -RetryIntervalSec 5
        } else {
            Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing # Windows PowerShell
        }
    }
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($hash -ne $sha256) { throw "$file has SHA-256 $hash, tools/deps.json says $sha256" }
}

$tbbVersionHeader = Join-Path $TbbRoot "include\oneapi\tbb\version.h"
if (-not (Test-Path -LiteralPath $tbbVersionHeader)) { throw "Not a oneTBB root: $TbbRoot" }

if (-not $IspcRoot) {
    $zip = Join-Path $WorkDir "ispc-v$($oidn.ispc)-windows.zip"
    Get-Verified "https://github.com/ispc/ispc/releases/download/v$($oidn.ispc)/ispc-v$($oidn.ispc)-windows.zip" `
        $zip $oidn.ispc_sha256
    $IspcRoot = Join-Path $WorkDir "ispc-v$($oidn.ispc)-windows"
    if (-not (Test-Path -LiteralPath (Join-Path $IspcRoot "bin\ispc.exe"))) {
        Expand-Archive -LiteralPath $zip -DestinationPath $WorkDir -Force
    }
}
$ispc = Join-Path $IspcRoot "bin\ispc.exe"
if (-not (Test-Path -LiteralPath $ispc)) { throw "ispc.exe not found in $IspcRoot\bin" }

$archive = Join-Path $WorkDir "oidn-$($oidn.version).src.tar.gz"
Get-Verified "https://github.com/RenderKit/oidn/releases/download/$($oidn.tag)/oidn-$($oidn.version).src.tar.gz" `
    $archive $oidn.source_sha256
$source = Join-Path $WorkDir "oidn-$($oidn.version)"
if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))) {
    tar -xzf $archive -C $WorkDir
    if ($LASTEXITCODE -ne 0) { throw "tar failed on $archive" }
}

$build = Join-Path $WorkDir "oidn-$($oidn.version)-build"
cmake -S $source -B $build -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_INSTALL_PREFIX=$Prefix" `
    "-DTBB_ROOT=$TbbRoot" "-DISPC_EXECUTABLE=$ispc" `
    -DOIDN_DEVICE_CPU=ON -DOIDN_DEVICE_SYCL=OFF -DOIDN_DEVICE_CUDA=OFF -DOIDN_DEVICE_HIP=OFF `
    -DOIDN_DEVICE_METAL=OFF -DOIDN_FILTER_RT=ON -DOIDN_FILTER_RTLIGHTMAP=OFF `
    -DOIDN_APPS=OFF -DOIDN_INSTALL_DEPENDENCIES=OFF
if ($LASTEXITCODE -ne 0) { throw "OIDN configure failed" }
cmake --build $build --config Release --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "OIDN build failed" }
cmake --install $build --config Release
if ($LASTEXITCODE -ne 0) { throw "OIDN install failed" }
# The tbb12.dll of the process is PrismRT's: never install a second one.
if (Get-ChildItem -LiteralPath (Join-Path $Prefix "bin") -Filter "tbb*.dll" -ErrorAction SilentlyContinue) {
    throw "$Prefix\bin contains a tbb DLL; OIDN must use PrismRT's oneTBB"
}
Write-Host "OIDN $($oidn.version) (CPU device) installed to $Prefix"
Get-ChildItem -LiteralPath (Join-Path $Prefix "bin") -Filter "*.dll" | ForEach-Object { Write-Host "  $($_.Name)" }
