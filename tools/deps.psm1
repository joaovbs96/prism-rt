<#
.SYNOPSIS
  Reads tools/deps.json, the single list of PrismRT dependency versions.

.EXAMPLE
  Import-Module ./tools/deps.psm1
  (Get-PrismDeps).openusd.tag          # v26.08
  Get-PrismCacheKey -Stack usd         # usd-26.08-p<patch hash>-osd-3.6.1-...-ocio-2.6.0-vcpkg-...-<script hash>
#>
$script:DepsJson = Join-Path $PSScriptRoot "deps.json"
$script:RepoRoot = Split-Path -Parent $PSScriptRoot

function Get-PrismDeps {
    param([string] $Path = $script:DepsJson)
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

# "name==version" for each pip package, in a stable order.
function Get-PrismPipRequirements {
    param([string[]] $Only = @())
    $packages = (Get-PrismDeps).python.packages
    $names = $packages.PSObject.Properties.Name | Sort-Object
    if ($Only.Count) { $names = $names | Where-Object { $Only -contains $_ } }
    return @($names | ForEach-Object { "$_==$($packages.$_)" })
}

# Hash of the files that shape a cache entry. Line endings are normalised so a
# local checkout and the CI runner produce the same key.
function Get-PrismFilesHash {
    param([Parameter(Mandatory)] [string[]] $Files)
    $sha = [Security.Cryptography.SHA256]::Create()
    $all = New-Object System.Collections.Generic.List[byte]
    foreach ($file in $Files) {
        $text = [IO.File]::ReadAllText((Join-Path $script:RepoRoot $file)) -replace "`r`n", "`n"
        $all.AddRange([Text.Encoding]::UTF8.GetBytes("$file`n$text`n"))
    }
    $hash = $sha.ComputeHash($all.ToArray())
    return (($hash | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, 16)
}

<#
  The patches tools/ci/build_usd_stack.ps1 applies to OpenUSD: every
  tools/ci/patches/openusd-<version>-*.patch for deps.json's OpenUSD version,
  as repository-relative paths in the order they are applied. A version
  upgrade therefore drops them; each patch says when to delete it.
#>
function Get-PrismUsdPatches {
    $version = (Get-PrismDeps).openusd.version
    $dir = Join-Path $script:RepoRoot "tools\ci\patches"
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    return @(Get-ChildItem -LiteralPath $dir -Filter "openusd-$version-*.patch" -File |
        Sort-Object Name | ForEach-Object { "tools/ci/patches/$($_.Name)" })
}

# Short hash of the OpenUSD patches ("" without any): it names the local
# OpenUSD prefix and goes into the usd cache key.
function Get-PrismUsdPatchHash {
    $patches = Get-PrismUsdPatches
    if (-not $patches.Count) { return "" }
    return (Get-PrismFilesHash $patches).Substring(0, 8)
}

<#
  Cache key of a CI-built dependency stack: the versions that go into it, the
  stack revision (bump it to force a rebuild), the runner image and a hash of
  the scripts that build it (and of the OpenUSD patches they apply).
#>
function Get-PrismCacheKey {
    param(
        [Parameter(Mandatory)] [ValidateSet("usd", "mdl")] [string] $Stack,
        [string] $Platform = $(if ($env:ImageOS) { $env:ImageOS } else { "local" })
    )
    $d = Get-PrismDeps
    switch ($Stack) {
        "usd" {
            # OpenUSD links vcpkg's OpenColorIO (tools/ci/install_ocio.ps1).
            $files = @("tools/ci/build_usd_stack.ps1", "tools/ci/fetch_prebuilt.ps1",
                "tools/ci/install_ocio.ps1") + @(Get-PrismUsdPatches)
            $patchHash = Get-PrismUsdPatchHash
            $usd = "usd-$($d.openusd.version)" + $(if ($patchHash) { "-p$patchHash" } else { "" })
            $parts = @($usd, "osd-$($d.opensubdiv.version)",
                "mtlx-$($d.materialx.version)", "tbb-$($d.onetbb.version)",
                "ocio-$($d.vcpkg.opencolorio)", "vcpkg-$($d.vcpkg.baseline.Substring(0, 10))",
                "py$($d.python.version)", ("pip-" + ((Get-PrismPipRequirements) -join "_").Replace("==", "")))
        }
        "mdl" {
            $files = @("tools/ci/build_mdl_sdk.ps1", "tools/ci/mdl_vcpkg/vcpkg.json")
            $parts = @("mdl-$($d.mdl_sdk.tag)", "nocuda", "clang-$($d.mdl_sdk.clang)",
                "vcpkg-$($d.mdl_sdk.vcpkg_baseline.Substring(0, 10))")
        }
    }
    $parts += @("r$($d.stack_revision)", $Platform, (Get-PrismFilesHash $files))
    return ($parts -join "-")
}

Export-ModuleMember -Function Get-PrismDeps, Get-PrismPipRequirements, Get-PrismFilesHash, Get-PrismCacheKey,
    Get-PrismUsdPatches, Get-PrismUsdPatchHash
