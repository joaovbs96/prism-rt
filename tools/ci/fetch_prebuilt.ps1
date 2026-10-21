<#
.SYNOPSIS
  Downloads the prebuilt dependencies for the Windows CI into <Prefix>:
  oneTBB (tbb), Embree (embree) and the MDL SDK (mdl; headers and DLLs only).
  The versions come from tools/deps.json. The MDL SDK binary release imports
  nvcuda.dll; CI builds the SDK instead (build_mdl_sdk.ps1).
#>
param(
    [Parameter(Mandatory)] [string] $Prefix,
    [string[]] $Components = @("tbb", "embree", "mdl")
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$deps = Get-PrismDeps
$TbbVersion = $deps.onetbb.version
$EmbreeVersion = $deps.embree.version
$MdlRelease = $deps.mdl_sdk.tag
$MdlBuild = $deps.mdl_sdk.build
$ProgressPreference = "SilentlyContinue"
New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
$tmp = Join-Path $Prefix "_download"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Get-Zip([string] $url, [string] $name) {
    $file = Join-Path $tmp $name
    Write-Host "Downloading $url"
    if ($PSVersionTable.PSVersion.Major -ge 7) {
        Invoke-WebRequest -Uri $url -OutFile $file -MaximumRetryCount 3 -RetryIntervalSec 5
    } else {
        Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing # Windows PowerShell
    }
    return $file
}

# oneTBB: <Prefix>\tbb\oneapi-tbb-<version>\{include,lib,redist}
if ($Components -contains "tbb") {
    $zip = Get-Zip "https://github.com/uxlfoundation/oneTBB/releases/download/v$TbbVersion/oneapi-tbb-$TbbVersion-win.zip" "tbb.zip"
    Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $Prefix "tbb") -Force
}

# Embree: the zip has no top-level folder.
if ($Components -contains "embree") {
    $zip = Get-Zip "https://github.com/embree/embree/releases/download/v$EmbreeVersion/embree-$EmbreeVersion.x64.windows.zip" "embree.zip"
    Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $Prefix "embree") -Force
}

# MDL SDK: keep only what PrismRT compiles and runs against.
if ($Components -contains "mdl") {
$zip = Get-Zip "https://github.com/NVIDIA/MDL-SDK/releases/download/$MdlRelease/MDL-SDK-$MdlRelease-$MdlBuild-nt-x86-64.zip" "mdl.zip"
$mdlTmp = Join-Path $tmp "mdl"
Expand-Archive -LiteralPath $zip -DestinationPath $mdlTmp -Force
$inner = Get-ChildItem $mdlTmp -Directory | Select-Object -First 1
$mdl = Join-Path $Prefix "mdl"
New-Item -ItemType Directory -Force -Path $mdl | Out-Null
foreach ($item in @("include", "bin", "LICENSE.md", "LICENSE_THIRDPARTY.md")) {
    Copy-Item -LiteralPath (Join-Path $inner.FullName $item) -Destination $mdl -Recurse -Force
}
}
Remove-Item -LiteralPath $tmp -Recurse -Force
Get-ChildItem $Prefix -Directory | ForEach-Object { Write-Host "$($_.Name): $((Get-ChildItem $_.FullName -Name | Select-Object -First 8) -join ', ')" }
