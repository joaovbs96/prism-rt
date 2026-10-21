<#
.SYNOPSIS
  Reads tools/deps.json, the single list of PrismRT dependency versions.

.EXAMPLE
  Import-Module ./tools/deps.psm1
  (Get-PrismDeps).openusd.tag          # v26.08
  Get-PrismCacheKey -Stack usd         # usd-26.08-osd-3.6.1-...-<script hash>
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
  Cache key of a CI-built dependency stack: the versions that go into it, the
  stack revision (bump it to force a rebuild), the runner image and a hash of
  the scripts that build it.
#>
function Get-PrismCacheKey {
    param(
        [Parameter(Mandatory)] [ValidateSet("usd", "mdl")] [string] $Stack,
        [string] $Platform = $(if ($env:ImageOS) { $env:ImageOS } else { "local" })
    )
    $d = Get-PrismDeps
    switch ($Stack) {
        "usd" {
            $files = @("tools/ci/build_usd_stack.ps1", "tools/ci/fetch_prebuilt.ps1")
            $parts = @("usd-$($d.openusd.version)", "osd-$($d.opensubdiv.version)",
                "mtlx-$($d.materialx.version)", "tbb-$($d.onetbb.version)",
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

Export-ModuleMember -Function Get-PrismDeps, Get-PrismPipRequirements, Get-PrismFilesHash, Get-PrismCacheKey
