<#
.SYNOPSIS
  Installs vcpkg's OpenColorIO (x64-windows) for building OpenUSD's OCIO
  support, at the vcpkg baseline PrismRT itself uses.

.DESCRIPTION
  OpenUSD must link the same OpenColorIO as PrismRT, so that one
  OpenColorIO_2_<minor>.dll (PrismRT's, from vcpkg.json) serves usdview, the
  Hydra delegate and the CLI. This installs the opencolorio port from a vcpkg
  checkout at tools/deps.json's vcpkg baseline (the builtin-baseline of
  vcpkg.json), in classic mode: the port, triplet and compiler are the same as
  in PrismRT's manifest install, so the package ABI hash matches and a shared
  binary cache serves both. OpenUSD only links the import library; at run time
  the DLL from PrismRT's vcpkg install is loaded.

  Prints the install prefix (<InstallRoot>\x64-windows), which is what
  build_usd_stack.ps1 -OcioRoot takes.

.EXAMPLE
  ./tools/ci/install_ocio.ps1 -VcpkgRoot D:\vcpkg -InstallRoot D:\deps\ocio-vcpkg
#>
param(
    # A vcpkg clone checked out at tools/deps.json's vcpkg baseline.
    [Parameter(Mandatory)] [string] $VcpkgRoot,
    # vcpkg --x-install-root; the prefix is <InstallRoot>\x64-windows.
    [Parameter(Mandatory)] [string] $InstallRoot,
    # Clone and bootstrap vcpkg at the baseline when $VcpkgRoot does not exist.
    [switch] $Clone
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$deps = Get-PrismDeps
$baseline = $deps.vcpkg.baseline
$triplet = "x64-windows"

function Native([string] $what, [scriptblock] $block) {
    $saved = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = 0
    try { & $block } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE) { throw "$what failed: $LASTEXITCODE" }
}

if (-not (Test-Path -LiteralPath $VcpkgRoot)) {
    if (-not $Clone) { throw "No vcpkg at $VcpkgRoot (pass -Clone to fetch it)." }
    Native "clone vcpkg" { git clone --quiet --filter=blob:none https://github.com/microsoft/vcpkg $VcpkgRoot }
    Native "check out the vcpkg baseline" { git -C $VcpkgRoot checkout --quiet $baseline }
}
$head = (git -C $VcpkgRoot rev-parse HEAD).Trim()
if ($head -ne $baseline) {
    throw "vcpkg at $VcpkgRoot is $head; tools/deps.json's baseline is $baseline"
}
$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path -LiteralPath $vcpkg)) {
    Native "bootstrap vcpkg" { & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics }
}

# Classic mode: run outside any directory with a vcpkg.json (the repository's
# manifest would switch vcpkg to manifest mode).
Push-Location $VcpkgRoot
try {
    Native "vcpkg install opencolorio" {
        & $vcpkg install "opencolorio:$triplet" "--x-install-root=$InstallRoot"
    }
} finally { Pop-Location }

$prefix = Join-Path $InstallRoot $triplet
$abi = Join-Path $prefix "include\OpenColorIO\OpenColorABI.h"
if (-not (Test-Path -LiteralPath $abi)) { throw "OpenColorIO headers missing under $prefix" }
$version = (Select-String -LiteralPath $abi -Pattern '#define OCIO_VERSION_FULL_STR\s+"([0-9.]+)"').Matches[0].Groups[1].Value
if ($version -ne $deps.vcpkg.opencolorio) {
    throw "OpenColorIO $($deps.vcpkg.opencolorio) expected (tools/deps.json), vcpkg installed $version"
}
Write-Host "OpenColorIO $version -> $prefix"
