<#
.SYNOPSIS
  Exports tools/deps.json to a GitHub Actions job: the cache keys and the
  versions as environment variables (GITHUB_ENV) and step outputs
  (GITHUB_OUTPUT), so the workflow holds no version literals.

.DESCRIPTION
  Environment and outputs:
    USD_STACK_KEY / usd_key   OpenSubdiv + MaterialX + OpenUSD cache key
    MDL_SDK_KEY   / mdl_key   MDL SDK cache key
    PRISM_DEP_<NAME>          one per version (ONETBB, EMBREE, PYTHON, ...)
    python                    for actions/setup-python
  Outside Actions (no GITHUB_ENV) it prints them, as a dry run.
#>
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$d = Get-PrismDeps
$values = [ordered]@{
    USD_STACK_KEY             = Get-PrismCacheKey -Stack usd
    MDL_SDK_KEY               = Get-PrismCacheKey -Stack mdl
    PRISM_DEP_OPENUSD         = $d.openusd.version
    PRISM_DEP_MATERIALX       = $d.materialx.version
    PRISM_DEP_OPENSUBDIV      = $d.opensubdiv.version
    PRISM_DEP_ONETBB          = $d.onetbb.version
    PRISM_DEP_EMBREE          = $d.embree.version
    PRISM_DEP_MDL_SDK         = $d.mdl_sdk.version
    PRISM_DEP_PYTHON          = $d.python.version
    PRISM_DEP_VULKAN_SDK      = $d.vulkan_sdk.version
    PRISM_DEP_VCPKG_BASELINE  = $d.vcpkg.baseline
}
$outputs = [ordered]@{
    usd_key = $values.USD_STACK_KEY
    mdl_key = $values.MDL_SDK_KEY
    python  = $d.python.version
}
if ($env:GITHUB_ENV) {
    foreach ($k in $values.Keys) { "$k=$($values[$k])" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
    foreach ($k in $outputs.Keys) { "$k=$($outputs[$k])" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8 }
}
foreach ($k in $values.Keys) { Write-Host "$k=$($values[$k])" }
foreach ($k in $outputs.Keys) { Write-Host "output $k=$($outputs[$k])" }
