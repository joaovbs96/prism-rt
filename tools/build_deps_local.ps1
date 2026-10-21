<#
.SYNOPSIS
  Builds the OpenSubdiv + MaterialX + OpenUSD stack on a development machine
  with the same script the Windows CI uses (tools/ci/build_usd_stack.ps1).

.DESCRIPTION
  Versions come from tools/deps.json. Each component goes to its own versioned
  prefix, so an upgrade never overwrites the stack in use:

    <Root>\opensubdiv-<version>   <Root>\materialx-<version>   <Root>\openusd-<version>

  Like CI: oneTBB (PXR_FIND_TBB_IN_CONFIG=ON, no legacy TBB headers), Python
  from deps.json, imaging, usdview and MaterialX support on, RelWithDebInfo.
  Unlike CI, the .pdb files are kept (pass -StripPdb to drop them) and the
  developer's Python is used as is (pass -InstallPip to install the pinned
  PySide6/PyOpenGL/numpy into it).

  Afterwards point cmake/PrismLocalDependencies.cmake and prism_local_env.ps1
  at the printed prefixes (see the .example files).

.EXAMPLE
  ./tools/build_deps_local.ps1 -TbbRoot C:\deps\oneapi-tbb-2022.2.0 -WorkDir D:\prism-deps\src
  ./tools/build_deps_local.ps1 -DownloadTbb -WorkDir D:\prism-deps\src
#>
param(
    # oneTBB binary release root (with lib\cmake\tbb). See -DownloadTbb.
    [string] $TbbRoot = "",
    # Clones and build trees; can be deleted afterwards (~40 GB).
    [Parameter(Mandatory)] [string] $WorkDir,
    # Where the versioned prefixes go.
    [string] $Root = "",
    [string] $PythonRoot = "",
    [int] $Jobs = 6,
    # Fetch the oneTBB release named in deps.json into <Root>\onetbb-<version>.
    [switch] $DownloadTbb,
    [switch] $InstallPip,
    [switch] $StripPdb
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "deps.psm1") -Force
$deps = Get-PrismDeps
if (-not $Root) { $Root = Split-Path -Parent ([IO.Path]::GetFullPath($WorkDir)) }
$Root = [IO.Path]::GetFullPath($Root)

if ($DownloadTbb) {
    $tbbPrefix = Join-Path $Root "onetbb-$($deps.onetbb.version)"
    & (Join-Path $PSScriptRoot "ci\fetch_prebuilt.ps1") -Prefix $tbbPrefix -Components tbb
    $TbbRoot = Join-Path $tbbPrefix "tbb\oneapi-tbb-$($deps.onetbb.version)"
}
if (-not $TbbRoot) { throw "Pass -TbbRoot or -DownloadTbb." }
$tbbVersionHeader = Join-Path $TbbRoot "include\oneapi\tbb\version.h"
if (-not (Test-Path -LiteralPath $tbbVersionHeader)) { throw "Not a oneTBB root: $TbbRoot" }
$v = Get-Content -LiteralPath $tbbVersionHeader | Select-String "#define TBB_VERSION_(MAJOR|MINOR|PATCH) (\d+)" |
    ForEach-Object { $_.Matches[0].Groups[2].Value }
if (($v -join ".") -ne $deps.onetbb.version) {
    throw "oneTBB $($deps.onetbb.version) expected (tools/deps.json), $TbbRoot is $($v -join '.')"
}

if (-not $PythonRoot) {
    $PythonRoot = Join-Path $env:LOCALAPPDATA ("Programs\Python\Python" + $deps.python.version.Replace(".", ""))
}
if (-not (Test-Path -LiteralPath (Join-Path $PythonRoot "python.exe"))) { throw "python.exe not found in $PythonRoot" }
$env:PATH = "$PythonRoot;$(Join-Path $PythonRoot 'Scripts');$env:PATH"

# OpenUSD's MSVC flags add /MP (one compiler per core in every project), so
# MSBuild's /m alone does not bound the load: _CL_ is appended last and wins.
if ($Jobs -gt 0) { $env:_CL_ = "/MP1" }

$prefixes = @{
    Osd  = Join-Path $Root "opensubdiv-$($deps.opensubdiv.version)"
    Mtlx = Join-Path $Root "materialx-$($deps.materialx.version)"
    Usd  = Join-Path $Root "openusd-$($deps.openusd.version)"
}
$sw = [Diagnostics.Stopwatch]::StartNew()
$arguments = @{
    TbbRoot = $TbbRoot; SourceDir = $WorkDir; Jobs = $Jobs
    OsdPrefix = $prefixes.Osd; MaterialXPrefix = $prefixes.Mtlx; UsdPrefix = $prefixes.Usd
    SkipPipInstall = (-not $InstallPip); KeepPdb = (-not $StripPdb)
}
& (Join-Path $PSScriptRoot "ci\build_usd_stack.ps1") @arguments
Write-Host ("Stack built in {0:N1} min:" -f $sw.Elapsed.TotalMinutes)
$prefixes.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Host "  $($_.Value)" }
Write-Host "  oneTBB: $TbbRoot"
Write-Host "  Python: $PythonRoot"
