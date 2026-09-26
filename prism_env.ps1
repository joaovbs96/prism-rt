# Dot-source this script before running PrismRT/Hydra validation binaries from
# PowerShell, for example: . .\prism_env.ps1; .\build\hydra\plugin\prism_hydra_render.exe ...
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectsRoot = "C:\Users\joaov\Desktop\Programming_Projects"
$depsRoot = Join-Path $projectsRoot "external_deps"
$usdInstall = Join-Path $depsRoot "OpenUSD\install"
$pythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python\Python311"
$materialXInstall = Join-Path $depsRoot "MaterialX\install"
$mdlSdk = Join-Path $projectsRoot "mdl-sdk"
$pythonSite = Join-Path $pythonRoot "Lib\site-packages"

$runtimeDirs = @(
    (Join-Path $usdInstall "bin"),
    (Join-Path $usdInstall "lib"),
    $pythonRoot,
    (Join-Path $pythonRoot "DLLs"),
    (Join-Path $pythonRoot "Scripts"),
    (Join-Path $pythonSite "PySide6"),
    (Join-Path $projectRoot "build\vcpkg_installed\x64-windows\bin"),
    (Join-Path $projectRoot "build\hydra\plugin"),
    (Join-Path $projectRoot "build\RelWithDebInfo"),
    (Join-Path $materialXInstall "bin"),
    (Join-Path $materialXInstall "lib"),
    (Join-Path $projectsRoot "include\tbb\redist\intel64\vc14"),
    (Join-Path $projectsRoot "Embree\build\Release"),
    (Join-Path $projectsRoot "include\Embree\bin"),
    (Join-Path $mdlSdk "bin"),
    (Join-Path $mdlSdk "nt-x86-64\lib")
) | Where-Object { Test-Path -LiteralPath $_ }
$env:PATH = (($runtimeDirs + @($env:PATH)) -join ";")
$usdPythonDllDirs = @(
    (Join-Path $usdInstall "bin"),
    (Join-Path $usdInstall "lib"),
    $pythonRoot,
    (Join-Path $pythonRoot "DLLs"),
    (Join-Path $projectsRoot "include\tbb\redist\intel64\vc14")
) | Where-Object { Test-Path -LiteralPath $_ }
$env:PXR_USD_WINDOWS_DLL_PATH = ($usdPythonDllDirs -join ";")

$qtPlugins = Join-Path $pythonSite "PySide6\Qt\plugins"
if (Test-Path -LiteralPath $qtPlugins) { $env:QT_PLUGIN_PATH = $qtPlugins }

$pythonPaths = @(
    (Join-Path $usdInstall "lib\python"),
    (Join-Path $usdInstall "lib"),
    (Join-Path $projectRoot "tools\usdview_plugins")
) | Where-Object { Test-Path -LiteralPath $_ }
if ($env:PYTHONPATH) { $pythonPaths += $env:PYTHONPATH }
$env:PYTHONPATH = ($pythonPaths -join ";")
# The installed PyOpenGL-accelerate 3.1.7 extension was built against the
# NumPy 1.x ABI, while usdview uses NumPy 2.x. The pure-Python PyOpenGL path is
# ABI-independent and avoids a dtype-size failure during viewport presentation.
$env:PYOPENGL_USE_ACCELERATE = "0"

$env:PRISM_MATERIALX_LIBRARY_PATH = Join-Path $materialXInstall "libraries"
$env:PRISM_MATERIALX_MDL = Join-Path $materialXInstall "libraries\mdl"
$env:PRISM_MATERIALX_MDL_LIBRARIES = $env:PRISM_MATERIALX_MDL
$env:MDL_SDK_ROOT = $mdlSdk

$pluginInfoRoot = Join-Path $projectRoot "build\hydra\resources"
if (Test-Path -LiteralPath $pluginInfoRoot) {
    $pluginPaths = @($pluginInfoRoot, (Join-Path $projectRoot "tools\usdview_plugins\prismUsdview"))
    if ($env:PXR_PLUGINPATH_NAME) { $pluginPaths += $env:PXR_PLUGINPATH_NAME.Split(";") }
    $env:PXR_PLUGINPATH_NAME = ($pluginPaths -join ";")
}
