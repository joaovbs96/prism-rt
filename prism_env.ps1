# Load this script before running PrismRT or usdview from a configured build.
# Optional per-machine overrides belong in ignored prism_local_env.ps1 or the
# PRISM_* environment variables documented in README.md.
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$localEnv = Join-Path $projectRoot "prism_local_env.ps1"
if (Test-Path -LiteralPath $localEnv) { . $localEnv }

$runtimeConfig = Join-Path $projectRoot "prism-runtime.json"
if (-not (Test-Path -LiteralPath $runtimeConfig)) {
    $runtimeConfig = Join-Path $projectRoot "build\prism-runtime.json"
}
if (Test-Path -LiteralPath $runtimeConfig) {
    $runtime = Get-Content -LiteralPath $runtimeConfig -Raw | ConvertFrom-Json
    foreach ($setting in @(
        @{ Name = "PRISM_USD_ROOT"; Value = $runtime.usdRoot },
        @{ Name = "PRISM_OPENSUBDIV_ROOT"; Value = $runtime.openSubdivRoot },
        @{ Name = "PRISM_MATERIALX_ROOT"; Value = $runtime.materialXRoot },
        @{ Name = "PRISM_MATERIALX_LIBRARY_PATH"; Value = $runtime.materialXLibraries },
        @{ Name = "PRISM_MATERIALX_MDL"; Value = $runtime.materialXMdl },
        @{ Name = "PRISM_MDL_SDK_ROOT"; Value = $runtime.mdlSdkRoot },
        @{ Name = "PRISM_TBB_REDIST"; Value = $runtime.tbbRuntime },
        @{ Name = "PRISM_EMBREE_RUNTIME_DIR"; Value = $runtime.embreeRuntime },
        @{ Name = "PRISM_EMBREE_BUILD_RUNTIME"; Value = $runtime.embreeBuildRuntime },
        @{ Name = "PRISM_VCPKG_BIN"; Value = $runtime.vcpkgRuntime },
        @{ Name = "PRISM_PYTHON_ROOT"; Value = $runtime.pythonRoot }
    )) {
        if (-not [Environment]::GetEnvironmentVariable($setting.Name) -and $setting.Value) {
            [Environment]::SetEnvironmentVariable($setting.Name, $setting.Value, "Process")
        }
    }
}

function Get-PrismSubPath([string] $root, [string] $child) {
    if ($root) { return Join-Path $root $child }
    return $null
}

$usdRoot = $env:PRISM_USD_ROOT
$mtlxRoot = $env:PRISM_MATERIALX_ROOT
$mdlRoot = $env:PRISM_MDL_SDK_ROOT
$pythonRoot = $env:PRISM_PYTHON_ROOT
$runtimeRoots = @(
    (Get-PrismSubPath $usdRoot "bin"), (Get-PrismSubPath $usdRoot "lib"),
    (Get-PrismSubPath $env:PRISM_OPENSUBDIV_ROOT "bin"),
    $pythonRoot, (Get-PrismSubPath $pythonRoot "DLLs"),
    (Get-PrismSubPath $pythonRoot "Scripts"),
    (Get-PrismSubPath $pythonRoot "Lib\site-packages\PySide6"),
    $env:PRISM_VCPKG_BIN,
    (Join-Path $projectRoot "build\hydra\plugin"),
    (Join-Path $projectRoot "build\RelWithDebInfo"),
    (Get-PrismSubPath $mtlxRoot "bin"), (Get-PrismSubPath $mtlxRoot "lib"),
    $env:PRISM_TBB_REDIST, $env:PRISM_EMBREE_BUILD_RUNTIME,
    $env:PRISM_EMBREE_RUNTIME_DIR,
    (Get-PrismSubPath $mdlRoot "bin"), (Get-PrismSubPath $mdlRoot "nt-x86-64\lib")
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$env:PATH = (($runtimeRoots + @($env:PATH)) -join ";")

if ($usdRoot) {
    $env:PYTHONPATH = @((Get-PrismSubPath $usdRoot "lib\python"),
        (Get-PrismSubPath $usdRoot "lib"), $env:PYTHONPATH) -join ";"
}
if (-not $env:PXR_USD_WINDOWS_DLL_PATH) {
    $usdDllRoots = @((Get-PrismSubPath $usdRoot "bin"),
        (Get-PrismSubPath $usdRoot "lib"),
        (Get-PrismSubPath $env:PRISM_OPENSUBDIV_ROOT "bin"), $pythonRoot,
        (Get-PrismSubPath $pythonRoot "DLLs"), $env:PRISM_TBB_REDIST,
        $env:PRISM_VCPKG_BIN, (Get-PrismSubPath $mtlxRoot "bin"),
        (Get-PrismSubPath $mtlxRoot "lib"), $env:PRISM_EMBREE_BUILD_RUNTIME,
        $env:PRISM_EMBREE_RUNTIME_DIR, (Get-PrismSubPath $mdlRoot "bin"),
        (Get-PrismSubPath $mdlRoot "nt-x86-64\lib"))
    $env:PXR_USD_WINDOWS_DLL_PATH = ($usdDllRoots | Where-Object {
        $_ -and (Test-Path -LiteralPath $_)
    }) -join ";"
}
if ($pythonRoot) {
    $site = Get-PrismSubPath $pythonRoot "Lib\site-packages"
    $qtPlugins = Get-PrismSubPath $site "PySide6\Qt\plugins"
    if (Test-Path -LiteralPath $qtPlugins) { $env:QT_PLUGIN_PATH = $qtPlugins }
    $env:PYTHONPATH = @((Get-PrismSubPath $site "PySide6"), $env:PYTHONPATH) -join ";"
}
$sourceUsdviewPlugins = Join-Path $projectRoot "tools\usdview_plugins"
$installedPluginRoot = Join-Path (Split-Path -Parent $projectRoot) "lib\usd\plugins\prism"
$pluginRoots = @()
if (Test-Path -LiteralPath (Join-Path $projectRoot "build\hydra\resources")) {
    $pluginRoots += Join-Path $projectRoot "build\hydra\resources"
}
$hasInstalledPlugin = Test-Path -LiteralPath (Join-Path $installedPluginRoot "resources\plugInfo.json")
if (Test-Path -LiteralPath $sourceUsdviewPlugins) {
    $env:PYTHONPATH = @($sourceUsdviewPlugins, $env:PYTHONPATH) -join ";"
    # usdview discovers the extension through its plugInfo.json, not PYTHONPATH.
    if (-not $hasInstalledPlugin) {
        $pluginRoots += Join-Path $sourceUsdviewPlugins "prismUsdview"
    }
}
if ($hasInstalledPlugin) {
    $pluginRoots += Join-Path $installedPluginRoot "resources"
    $pluginRoots += Join-Path $installedPluginRoot "usdview_plugins\prismUsdview"
    $env:PYTHONPATH = @((Join-Path $installedPluginRoot "usdview_plugins"), $env:PYTHONPATH) -join ";"
    $runtimeRoots += Join-Path $installedPluginRoot "plugin"
    $env:PATH = (($runtimeRoots + @($env:PATH)) -join ";")
}
if ($env:PRISM_MATERIALX_LIBRARY_PATH) {
    $env:PRISM_MATERIALX_MDL = if ($env:PRISM_MATERIALX_MDL) {
        $env:PRISM_MATERIALX_MDL
    } else { Join-Path $env:PRISM_MATERIALX_LIBRARY_PATH "mdl" }
}
if ($mdlRoot) {
    $env:MDL_SDK_ROOT = $mdlRoot
    if ($env:PRISM_MATERIALX_MDL) { $env:PRISM_MATERIALX_MDL_LIBRARIES = $env:PRISM_MATERIALX_MDL }
}
if (-not $env:PYOPENGL_USE_ACCELERATE) { $env:PYOPENGL_USE_ACCELERATE = "0" }
# PrismRT's default config, made explicit so that usdview's OpenColorIO colour
# correction (Hdx) shows the colour AOV through the same display and view.
if (-not $env:OCIO) { $env:OCIO = "ocio://studio-config-latest" }

if ($pluginRoots.Count -gt 0) {
    $pluginPaths = $pluginRoots
    if ($env:PXR_PLUGINPATH_NAME) { $pluginPaths += $env:PXR_PLUGINPATH_NAME.Split(";") }
    $env:PXR_PLUGINPATH_NAME = ($pluginPaths -join ";")
}
