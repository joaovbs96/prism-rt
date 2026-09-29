param(
    [string] $BuildPreset = "prism-hydra-fresh",
    [string] $BuildDirectory = "",
    [string] $Configuration = "RelWithDebInfo",
    [ValidateRange(1, 1000000)] [int] $SamplesPerPixel = 128,
    [ValidateRange(1, 1000000)] [int] $ExportSamplesPerPixel = 1024,
    [switch] $Resume,
    [switch] $SkipUsdview
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root "prism_env.ps1")
$buildDir = if ($BuildPreset -eq "prism-hydra-fresh") {
    Join-Path $root "build-fresh-validation"
} else { Join-Path $root "build" }

function Resolve-ChildPath([string] $parent, [string] $candidate) {
    $parentFull = [IO.Path]::GetFullPath($parent).TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $candidateFull = [IO.Path]::GetFullPath($candidate)
    if (-not $candidateFull.StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Validation path escapes the build directory: $candidateFull"
    }
    return $candidateFull
}

if ($BuildDirectory) {
    $candidateBuild = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
        $BuildDirectory
    } else { Join-Path $root $BuildDirectory }
    $buildDir = Resolve-ChildPath $root $candidateBuild
}

if (-not $Resume -and (Test-Path -LiteralPath (Join-Path $buildDir "CMakeCache.txt"))) {
    cmake --build $buildDir --config $Configuration --target clean
    if ($LASTEXITCODE) { throw "CMake clean failed: $LASTEXITCODE" }
}
cmake --preset $BuildPreset -B $buildDir
if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
cmake --build $buildDir --config $Configuration --parallel 2
if ($LASTEXITCODE) { throw "CMake build failed: $LASTEXITCODE" }
$env:PXR_PLUGINPATH_NAME = Join-Path $buildDir "hydra\resources"
ctest --test-dir $buildDir -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw "CTest failed: $LASTEXITCODE" }

$stamp = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), [guid]::NewGuid().ToString("N")
$env:PRISM_CACHE_DIR = Join-Path $buildDir "install-validation\cache-$stamp"
$stage = Resolve-ChildPath $buildDir (Join-Path $buildDir "install-stage-$stamp")
$relocated = Resolve-ChildPath $buildDir (Join-Path $buildDir "install relocated $stamp")
if (Test-Path -LiteralPath $stage) { throw "Install staging directory already exists: $stage" }
if (Test-Path -LiteralPath $relocated) { throw "Relocated prefix already exists: $relocated" }
cmake --install $buildDir --config $Configuration --prefix $stage
if ($LASTEXITCODE) { throw "CMake install failed: $LASTEXITCODE" }
Move-Item -LiteralPath $stage -Destination $relocated
if ((Test-Path -LiteralPath $stage) -or -not (Test-Path -LiteralPath $relocated)) {
    throw "Install prefix relocation did not complete cleanly. Stage: $stage; destination: $relocated"
}

$env:PXR_PLUGINPATH_NAME = ""
. (Join-Path $relocated "bin\prism_env.ps1")
$pluginRoot = Join-Path $relocated "lib\usd\plugins\prism"
$pluginResources = Join-Path $pluginRoot "resources"
$env:PXR_PLUGINPATH_NAME = $pluginResources

# These are validation helpers, not installed product binaries. Place them
# beside the relocated launcher so Windows loads shared libraries through the
# relocated runtime PATH, never from the build helper's application directory.
$installedBin = Join-Path $relocated "bin"
$hydraHelper = Join-Path $installedBin "prism_hydra_render.exe"
$compareHelper = Join-Path $installedBin "prism_compare_images.exe"
$textureHelper = Join-Path $installedBin "prism_texture_cache_benchmark.exe"
foreach ($helper in @(
    @{ Source = Join-Path $buildDir "hydra\plugin\prism_hydra_render.exe"; Target = $hydraHelper },
    @{ Source = Join-Path $buildDir "hydra\plugin\prism_compare_images.exe"; Target = $compareHelper },
    @{ Source = Join-Path $buildDir "$Configuration\prism_texture_cache_benchmark.exe"; Target = $textureHelper }
)) {
    if (-not (Test-Path -LiteralPath $helper.Source)) { throw "Validation helper missing: $($helper.Source)" }
    if (Test-Path -LiteralPath $helper.Target) { throw "Unexpected helper collision in installed prefix: $($helper.Target)" }
    Copy-Item -LiteralPath $helper.Source -Destination $helper.Target
}

# The initial source-tree prism_env may have put build-tree DLL directories on
# PATH. Remove every such entry, retaining the configured external SDK roots.
$buildFullPaths = @($buildDir, (Join-Path $root "build")) | ForEach-Object {
    [IO.Path]::GetFullPath($_).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
}
$dependencyRuntimePaths = @(
    (Get-PrismSubPath $env:PRISM_USD_ROOT "bin"),
    (Get-PrismSubPath $env:PRISM_USD_ROOT "lib"),
    (Get-PrismSubPath $env:PRISM_OPENSUBDIV_ROOT "bin"),
    $env:PRISM_PYTHON_ROOT,
    (Get-PrismSubPath $env:PRISM_PYTHON_ROOT "DLLs"),
    (Get-PrismSubPath $env:PRISM_PYTHON_ROOT "Scripts"),
    (Get-PrismSubPath $env:PRISM_PYTHON_ROOT "Lib\site-packages\PySide6"),
    $env:PRISM_VCPKG_BIN,
    (Get-PrismSubPath $env:PRISM_MATERIALX_ROOT "bin"),
    (Get-PrismSubPath $env:PRISM_MATERIALX_ROOT "lib"),
    $env:PRISM_TBB_REDIST, $env:PRISM_EMBREE_RUNTIME_DIR,
    $env:PRISM_EMBREE_BUILD_RUNTIME,
    (Get-PrismSubPath $env:PRISM_MDL_SDK_ROOT "bin"),
    (Get-PrismSubPath $env:PRISM_MDL_SDK_ROOT "nt-x86-64\lib")
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | ForEach-Object {
    [IO.Path]::GetFullPath($_).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
}
$env:PATH = (($env:PATH -split ';' | Where-Object {
    if (-not $_) { return $false }
    $entry = [IO.Path]::GetFullPath($_).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    foreach ($buildPath in $buildFullPaths) {
        if ($entry.StartsWith($buildPath, [StringComparison]::OrdinalIgnoreCase)) {
            return $dependencyRuntimePaths -contains $entry
        }
    }
    return $true
}) -join ';')
$env:PATH = @($installedBin, (Join-Path $pluginRoot "plugin"), $env:PATH) -join ";"
$remainingBuildPaths = @($env:PATH -split ';' | Where-Object {
    if (-not $_) { return $false }
    $entry = [IO.Path]::GetFullPath($_).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    # The relocated installation itself lives below the validation directory.
    # It is the subject of this check, not an accidental build-tree dependency.
    $relocatedRoot = [IO.Path]::GetFullPath($relocated).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($entry.StartsWith($relocatedRoot, [StringComparison]::OrdinalIgnoreCase)) { return $false }
    foreach ($buildPath in $buildFullPaths) {
        if ($entry.StartsWith($buildPath, [StringComparison]::OrdinalIgnoreCase)) {
            return $dependencyRuntimePaths -notcontains $entry
        }
    }
    return $false
})
if ($remainingBuildPaths.Count) {
    throw "Build-tree runtime paths remain outside configured external SDK directories: $($remainingBuildPaths -join '; ')"
}

$installedExe = Join-Path $relocated "bin\PrismRT.exe"
if (-not (Test-Path -LiteralPath $installedExe)) { throw "Installed executable missing: $installedExe" }
& $installedExe --help
if ($LASTEXITCODE) { throw "Installed PrismRT launcher failed: $LASTEXITCODE" }

$python = if ($env:PRISM_PYTHON_ROOT) {
    Join-Path $env:PRISM_PYTHON_ROOT "python.exe"
} else { "python" }
& $python (Join-Path $root "tests\cli_smoke.py") $installedExe
if ($LASTEXITCODE) { throw "Installed CLI smoke failed: $LASTEXITCODE" }
$textureValidationDir = Join-Path $buildDir "install-validation\texture-cache-$stamp"
& $python (Join-Path $root "tests\benchmark_scalability.py") `
    --executable $textureHelper --output $textureValidationDir --samples 1024 `
    --cache-mb 32 --parity-only
if ($LASTEXITCODE) { throw "Installed texture service/runtime parity fixture failed: $LASTEXITCODE" }
& $python (Join-Path $root "tests\hydra\validate.py") --build $buildDir --config $Configuration --spp $SamplesPerPixel --standalone $installedExe --hydra $hydraHelper --compare $compareHelper
if ($LASTEXITCODE) { throw "Standalone/Hydra parity validation failed: $LASTEXITCODE" }
& $python (Join-Path $root "tests\hydra\validate_exports.py") --build $buildDir --config $Configuration --spp $SamplesPerPixel --inoneweekend-spp $ExportSamplesPerPixel --standalone $installedExe --hydra $hydraHelper --compare $compareHelper
if ($LASTEXITCODE) { throw "Installed standalone/Hydra export comparisons failed: $LASTEXITCODE" }
& $python (Join-Path $root "tests\hydra\validate_native_instance_bindings.py") `
    $hydraHelper
if ($LASTEXITCODE) { throw "Installed native-instance binding validation failed: $LASTEXITCODE" }
& $python (Join-Path $root "tests\hydra\test_summarize_loading.py")
if ($LASTEXITCODE) { throw "Loading-summary synthetic tests failed: $LASTEXITCODE" }

if (-not $SkipUsdview) {
    $testUsdviewScript = $null
    if ($env:PRISM_USD_SOURCE_ROOT) {
        $candidateScript = Join-Path $env:PRISM_USD_SOURCE_ROOT "pxr\usdImaging\bin\testusdview\testusdview.py"
        if (Test-Path -LiteralPath $candidateScript) { $testUsdviewScript = $candidateScript }
    }
    $testUsdview = Get-Command testusdview -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $testUsdview -and $env:PRISM_USD_ROOT) {
        foreach ($candidate in @("bin\testusdview.exe", "bin\testusdview.cmd", "bin\testusdview.bat")) {
            $candidatePath = Join-Path $env:PRISM_USD_ROOT $candidate
            if (Test-Path -LiteralPath $candidatePath) { $testUsdview = [pscustomobject]@{ Source = $candidatePath }; break }
        }
    }
    if (-not $testUsdview -and -not $testUsdviewScript) {
        throw "testusdview not found. Set PRISM_USD_ROOT or PRISM_USD_SOURCE_ROOT, or use -SkipUsdview."
    }

    $env:PXR_PLUGINPATH_NAME = @((Join-Path $pluginRoot "resources"),
        (Join-Path $pluginRoot "usdview_plugins\prismUsdview")) -join ";"
    $usdPython = Get-PrismSubPath $env:PRISM_USD_ROOT "lib\python"
    $env:PYTHONPATH = @((Join-Path $pluginRoot "usdview_plugins"),
        $usdPython, $env:PYTHONPATH) -join ";"
    $env:PATH = @((Join-Path $pluginRoot "plugin"), $env:PATH) -join ";"
    $fixture = Join-Path $root "tests\hydra\fixtures\parity.usda"
    $smoke = Join-Path $root "tests\hydra\usdview_smoke.py"
    $motion = Join-Path $root "tests\hydra\usdview_motion.py"
    $instancesFixture = Join-Path $root "tests\hydra\fixtures\instances.usda"
    $instancesTest = Join-Path $root "tests\hydra\instancing_smoke.py"

    function Invoke-TestUsdview([string] $scene, [string] $script, [string] $outputName) {
        $env:PRISM_VALIDATION_DIR = Join-Path $buildDir "usdview-validation\$outputName"
        New-Item -ItemType Directory -Path $env:PRISM_VALIDATION_DIR -Force | Out-Null
        if ($testUsdviewScript) {
            & $python $testUsdviewScript $scene --renderer HdPrismRendererPlugin --testScript $script
        } else {
            & $testUsdview.Source $scene --renderer HdPrismRendererPlugin --testScript $script
        }
        if ($LASTEXITCODE) { throw "usdview test failed ($outputName): $LASTEXITCODE" }
    }

    Invoke-TestUsdview $fixture $smoke "lifecycle"
    Invoke-TestUsdview $fixture $motion "motion"
    Invoke-TestUsdview $instancesFixture $instancesTest "instances"

    # Exercise the installed usdview extension module with the relocated SDK
    # environment and no build-tree plugin paths in the registry.
    & $python (Join-Path $root "tests\hydra\test_usdview_selection_plugin.py")
    if ($LASTEXITCODE) { throw "usdview selection-plugin tests failed: $LASTEXITCODE" }
}
$buildDescription = if ($Resume) { "Resumed build" } else { "Clean build" }
Write-Host "$buildDescription, relocation, CTest, parity, synthetic summaries and requested usdview checks passed. Prefix: $relocated"
