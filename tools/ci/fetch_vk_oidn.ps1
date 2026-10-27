<#
.SYNOPSIS
  Installs nvpro-samples' vk_oidn (Open Image Denoise's network as Vulkan
  compute) for the Vulkan device's denoiser, and packs the guide prefilter
  weights it does not embed.

.DESCRIPTION
  Versions and checksums come from tools/deps.json (vk_oidn, oidn). The
  script downloads
    https://github.com/nvpro-samples/vk_oidn/releases/download/<tag>/vk_oidn-<version>-windows-x64.zip
      the prebuilt SDK package: header, static libraries with the compiled
      kernels and the colour filter weights embedded, CMake package files,
      LICENSE and NOTICE (Apache-2.0)
    https://raw.githubusercontent.com/nvpro-samples/vk_oidn/<commit>/tools/pack_weights.py
      vk_oidn's converter from Open Image Denoise weights (.tza) to its format
  verifies their SHA-256, extracts the package into -Prefix and packs OIDN's
  albedo and normal prefilter networks (rt_alb, rt_alb_large, rt_nrm,
  rt_nrm_large, from the OIDN source release that tools/ci/build_oidn.ps1
  verifies; downloaded into -WorkDir when it is not there) into
  -Prefix\prefilter\*.vkoidnw, which the build embeds in PrismRT.

  Why the prebuilt package and not a source build: building vk_oidn's Slang
  kernels needs a slangc with -denorm-mode-fp16 (Vulkan SDK 1.4.363, Slang
  2026.17); the package needs only the Vulkan headers of the SDK PrismRT
  already uses. Its weights are OIDN 2.5.1's (oidn-weights 28883d17), the
  same as the OIDN CPU device's.

  Needs Python 3 (pack_weights.py; -Python, default python on PATH).

.EXAMPLE
  ./tools/ci/fetch_vk_oidn.ps1 -WorkDir D:\prism-deps\src -Prefix D:\prism-deps\vk_oidn-0.2.0
#>
param(
    # Downloads (and the OIDN source release, when it is not there yet).
    [Parameter(Mandatory)] [string] $WorkDir,
    [Parameter(Mandatory)] [string] $Prefix,
    [string] $Python = "python"
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$deps = Get-PrismDeps
$vk = $deps.vk_oidn
$oidn = $deps.oidn
$ProgressPreference = "SilentlyContinue"
New-Item -ItemType Directory -Force -Path $WorkDir, $Prefix | Out-Null
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$Prefix = [IO.Path]::GetFullPath($Prefix)

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

# 1. The SDK package.
$name = "vk_oidn-$($vk.version)-windows-x64"
$zip = Join-Path $WorkDir "$name.zip"
Get-Verified "https://github.com/nvpro-samples/vk_oidn/releases/download/$($vk.tag)/$name.zip" $zip $vk.package_sha256
$extracted = Join-Path $WorkDir $name
if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
Expand-Archive -LiteralPath $zip -DestinationPath $WorkDir -Force
Copy-Item -Path (Join-Path $extracted "*") -Destination $Prefix -Recurse -Force
Remove-Item -LiteralPath $extracted -Recurse -Force

# 2. The prefilter weights: OIDN's albedo and normal networks, packed.
$packer = Join-Path $WorkDir "vk_oidn-$($vk.version)-pack_weights.py"
Get-Verified "https://raw.githubusercontent.com/nvpro-samples/vk_oidn/$($vk.commit)/tools/pack_weights.py" `
    $packer $vk.pack_weights_sha256
$weights = Join-Path $WorkDir "oidn-$($oidn.version)\weights"
$models = @("rt_alb", "rt_alb_large", "rt_nrm", "rt_nrm_large")
if (@($models | Where-Object { -not (Test-Path -LiteralPath (Join-Path $weights "$_.tza")) }).Count) {
    $archive = Join-Path $WorkDir "oidn-$($oidn.version).src.tar.gz"
    Get-Verified "https://github.com/RenderKit/oidn/releases/download/$($oidn.tag)/oidn-$($oidn.version).src.tar.gz" `
        $archive $oidn.source_sha256
    $members = @($models | ForEach-Object { "oidn-$($oidn.version)/weights/$_.tza" }) +
        @("oidn-$($oidn.version)/weights/LICENSE.txt")
    tar -xzf $archive -C $WorkDir @members
    if ($LASTEXITCODE -ne 0) { throw "tar failed on $archive" }
}
$prefilter = Join-Path $Prefix "prefilter"
New-Item -ItemType Directory -Force -Path $prefilter | Out-Null
foreach ($model in $models) {
    # -I: the packer runs isolated from the working directory's modules.
    & $Python -I $packer (Join-Path $weights "$model.tza") -o $prefilter
    if ($LASTEXITCODE -ne 0) { throw "pack_weights.py failed on $model" }
}
Copy-Item -LiteralPath (Join-Path $weights "LICENSE.txt") -Destination (Join-Path $prefilter "LICENSE-oidn-weights.txt")

Write-Host "vk_oidn $($vk.version) installed to $Prefix (vk_oidn_DIR = $Prefix\lib\cmake\vk_oidn)"
Get-ChildItem -LiteralPath $prefilter -Filter "*.vkoidnw" | ForEach-Object { Write-Host "  prefilter\$($_.Name)" }
