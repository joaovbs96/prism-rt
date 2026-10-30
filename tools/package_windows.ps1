<#
.SYNOPSIS
  Builds the relocatable PrismRT Windows demo package (a folder plus a zip).

.DESCRIPTION
  Installs PrismRT from an existing build, copies the runtime dependencies
  (OpenUSD, MaterialX libraries, oneTBB, Embree, Open Image Denoise, MDL SDK,
  vcpkg DLLs, DXC, the MSVC runtime), bundles a private Python with PySide6, PyOpenGL and numpy
  (versions from tools/deps.json), writes the launcher .bat files and README,
  and zips the result.

  Dependency locations default from the PRISM_* variables used by
  prism_env.ps1, then from an ignored prism_local_env.ps1, then from the
  prism-runtime.json written into the build directory.
#>
param(
    [string] $BuildDir = "",
    [string] $Configuration = "RelWithDebInfo",
    [string] $OutputDir = "",
    [string] $UsdRoot = "",
    [string] $UsdviewScript = "",
    [string] $MaterialXRoot = "",
    [string] $OpenSubdivRoot = "",
    [string] $TbbRuntime = "",
    [string] $EmbreeRuntime = "",
    # Open Image Denoise's bin (the denoiser); required when the build has it.
    [string] $OidnRuntime = "",
    # vk_oidn's install (the Vulkan denoiser, linked statically: only its licenses ship).
    [string] $VkOidnRoot = "",
    [string] $MdlSdkRoot = "",
    [string] $VcpkgBin = "",
    [string] $PythonRoot = "",
    [string] $DxcDir = "",
    [string] $VulkanLoader = "",
    # The commit the zip and VERSION.txt name (any revision git rev-parse
    # accepts); default HEAD. The package is always built from the working tree
    # and the existing build: pass the commit that build corresponds to, e.g.
    # the last commit that touched code when HEAD only changed docs.
    [string] $Commit = "HEAD",
    [switch] $SkipPip,
    [switch] $SkipZip
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$templates = Join-Path $PSScriptRoot "package"
Import-Module (Join-Path $PSScriptRoot "deps.psm1") -Force
$deps = Get-PrismDeps
$PySideVersion = $deps.python.packages.PySide6
$PipPackages = Get-PrismPipRequirements -Only numpy, PyOpenGL, opencolorio
# DLL names follow the ABI: oneTBB's binary interface 12, Embree's major version.
$tbbDll = "tbb12.dll"
$embreeDll = "embree$($deps.embree.version.Split('.')[0]).dll"

# Machine-local defaults, then the build's runtime description.
$localEnv = Join-Path $root "prism_local_env.ps1"
if (Test-Path -LiteralPath $localEnv) { . $localEnv }
if (-not $BuildDir) { $BuildDir = Join-Path $root "build" }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$runtimeJson = Join-Path $BuildDir "prism-runtime.json"
$runtime = if (Test-Path -LiteralPath $runtimeJson) {
    Get-Content -LiteralPath $runtimeJson -Raw | ConvertFrom-Json
} else { [pscustomobject]@{} }

function Pick([string] $explicit, [string[]] $envNames, $fromJson) {
    if ($explicit) { return $explicit }
    foreach ($name in $envNames) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { return $value }
    }
    if ($fromJson) { return [string] $fromJson }
    return ""
}
$UsdRoot = Pick $UsdRoot @("PRISM_USD_ROOT", "PRISM_OPENUSD_ROOT") $runtime.usdRoot
$MaterialXRoot = Pick $MaterialXRoot @("PRISM_MATERIALX_ROOT") $runtime.materialXRoot
$OpenSubdivRoot = Pick $OpenSubdivRoot @("PRISM_OPENSUBDIV_ROOT") $runtime.openSubdivRoot
$TbbRuntime = Pick $TbbRuntime @("PRISM_TBB_REDIST") $runtime.tbbRuntime
$EmbreeRuntime = Pick $EmbreeRuntime @("PRISM_EMBREE_RUNTIME_DIR", "PRISM_EMBREE_BUILD_RUNTIME") `
    $(if ($runtime.embreeRuntime) { $runtime.embreeRuntime } else { $runtime.embreeBuildRuntime })
$MdlSdkRoot = Pick $MdlSdkRoot @("PRISM_MDL_SDK_ROOT") $runtime.mdlSdkRoot
$OidnRuntime = Pick $OidnRuntime @("PRISM_OIDN_RUNTIME") $runtime.oidnRuntime
# The build links OIDN when its runtime description names it.
$buildHasOidn = [bool] $runtime.oidnRuntime
$VkOidnRoot = Pick $VkOidnRoot @("PRISM_VK_OIDN_ROOT") $runtime.vkOidnRoot
$buildHasVkOidn = [bool] $runtime.vkOidnRoot
$VcpkgBin = Pick $VcpkgBin @("PRISM_VCPKG_BIN") $runtime.vcpkgRuntime
$PythonRoot = Pick $PythonRoot @("PRISM_PYTHON_ROOT") $runtime.pythonRoot
$UsdviewScript = Pick $UsdviewScript @("PRISM_USDVIEW") ""
if (-not $UsdviewScript -and $env:PRISM_USD_SOURCE_ROOT) {
    $UsdviewScript = Join-Path $env:PRISM_USD_SOURCE_ROOT "pxr\usdImaging\bin\usdview\usdview.py"
}
if (-not $UsdviewScript -or -not (Test-Path -LiteralPath $UsdviewScript)) {
    $candidate = Join-Path $UsdRoot "lib\python\usdview.py"
    if (Test-Path -LiteralPath $candidate) { $UsdviewScript = $candidate }
}
if (-not $DxcDir) {
    $DxcDir = if ($env:PRISM_DXC) { Split-Path -Parent $env:PRISM_DXC }
              elseif ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK "Bin" } else { "" }
}
if (-not $OutputDir) { $OutputDir = Join-Path $root "local\package" }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path

function Require-Path([string] $label, [string] $path) {
    if (-not $path -or -not (Test-Path -LiteralPath $path)) {
        throw "$label not found: '$path'. Pass it as a parameter or set the matching PRISM_* variable."
    }
}
Require-Path "OpenUSD install (-UsdRoot)" (Join-Path $UsdRoot "lib")
Require-Path "usdview.py (-UsdviewScript)" $UsdviewScript
Require-Path "MaterialX libraries (-MaterialXRoot)" (Join-Path $MaterialXRoot "libraries")
Require-Path "oneTBB runtime (-TbbRuntime)" (Join-Path $TbbRuntime $tbbDll)
Require-Path "Embree runtime (-EmbreeRuntime)" (Join-Path $EmbreeRuntime $embreeDll)
if ($buildHasOidn) { Require-Path "Open Image Denoise runtime (-OidnRuntime)" (Join-Path $OidnRuntime "OpenImageDenoise.dll") }
Require-Path "vcpkg runtime (-VcpkgBin)" $VcpkgBin
Require-Path "Python (-PythonRoot)" (Join-Path $PythonRoot "python.exe")
Require-Path "DXC (-DxcDir)" (Join-Path $DxcDir "dxc.exe")

# The MDL SDK DLLs live in bin\ (release zip) or nt-x86-64\lib.
$mdlBin = @("bin", "nt-x86-64\lib") | ForEach-Object { Join-Path $MdlSdkRoot $_ } |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "libmdl_sdk.dll") } | Select-Object -First 1
if (-not $mdlBin) { throw "libmdl_sdk.dll not found under MDL SDK root '$MdlSdkRoot'." }

$sha = (& git -C $root rev-parse --short "$Commit^{commit}").Trim()
if ($LASTEXITCODE -or -not $sha) { throw "git rev-parse failed for -Commit $Commit" }
$pkg = Join-Path $OutputDir "PrismRT"
$zip = Join-Path $OutputDir "PrismRT-windows-x64-$sha.zip"
Write-Host "Packaging PrismRT $sha into $pkg"
if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Path $pkg | Out-Null

# robocopy exits 0-7 on success.
function Copy-Tree([string] $src, [string] $dst, [string[]] $excludeFiles = @(), [string[]] $excludeDirs = @()) {
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    $robo = @($src, $dst, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/R:1", "/W:1")
    if ($excludeFiles.Count) { $robo += "/XF"; $robo += $excludeFiles }
    if ($excludeDirs.Count) { $robo += "/XD"; $robo += $excludeDirs }
    & robocopy @robo | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE): $src -> $dst" }
    $global:LASTEXITCODE = 0
}
function Copy-Dlls([string] $srcDir, [string] $dst, [string[]] $patterns = @("*.dll")) {
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    foreach ($pattern in $patterns) {
        Get-ChildItem -LiteralPath $srcDir -Filter $pattern -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
        }
    }
}
function Copy-IfExists([string] $src, [string] $dstDir, [string] $newName = "") {
    if ($src -and (Test-Path -LiteralPath $src -PathType Leaf)) {
        New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
        $name = if ($newName) { $newName } else { Split-Path -Leaf $src }
        Copy-Item -LiteralPath $src -Destination (Join-Path $dstDir $name) -Force
    }
}

# 1. PrismRT itself, through the project's install rules.
$stage = Join-Path $OutputDir "_install_stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
& cmake --install $BuildDir --config $Configuration --prefix $stage | Out-Null
if ($LASTEXITCODE) { throw "cmake --install failed: $LASTEXITCODE" }
New-Item -ItemType Directory -Path (Join-Path $pkg "bin") | Out-Null
Copy-Item -LiteralPath (Join-Path $stage "bin\PrismRT.exe") -Destination (Join-Path $pkg "bin")
# The shared core and USD module, used by PrismRT.exe and hdPrism.dll alike.
foreach ($dll in @("prism.dll", "prism_usd.dll")) {
    Copy-Item -LiteralPath (Join-Path $stage "bin\$dll") -Destination (Join-Path $pkg "bin")
}
Copy-Tree (Join-Path $stage "bin\assets") (Join-Path $pkg "bin\assets")
# The material preview's stage, beside prism_usd.dll (docs/material_preview.md).
Copy-Tree (Join-Path $stage "bin\resources") (Join-Path $pkg "bin\resources")
Copy-Tree (Join-Path $stage "lib\usd\plugins") (Join-Path $pkg "lib\usd\plugins") @("*.pdb", "*.lib", "*.exp")
Remove-Item -LiteralPath $stage -Recurse -Force

# 2. Runtime dependencies. Every plain DLL goes beside PrismRT.exe (bin), which
#    is also on PATH for hdPrism.dll.
$binDir = Join-Path $pkg "bin"
Copy-Dlls $TbbRuntime $binDir @($tbbDll, "tbbmalloc.dll")
Copy-Dlls $EmbreeRuntime $binDir @($embreeDll)
# The denoiser: OIDN's API, core (with the network weights) and CPU device
# DLLs. Built against the oneTBB above (tools/ci/build_oidn.ps1), so they use
# the package's tbb12.dll; a tbb DLL beside them would be a second oneTBB.
if ($buildHasOidn) {
    if (Get-ChildItem -LiteralPath $OidnRuntime -Filter "tbb*.dll" -File) {
        throw "$OidnRuntime contains a tbb DLL: use OIDN built against PrismRT's oneTBB (tools/ci/build_oidn.ps1)"
    }
    Copy-Dlls $OidnRuntime $binDir @("OpenImageDenoise*.dll")
    foreach ($dll in @("OpenImageDenoise.dll", "OpenImageDenoise_core.dll", "OpenImageDenoise_device_cpu.dll")) {
        if (-not (Test-Path -LiteralPath (Join-Path $binDir $dll))) { throw "$dll missing from $OidnRuntime" }
    }
} else {
    Write-Host "The build has no denoiser (PRISM_ENABLE_OIDN off): no OIDN DLLs"
}
Copy-Dlls $VcpkgBin $binDir
# Colour management links OpenColorIO directly; its built-in configs are
# compiled into the DLL, so no config files ship.
$ocioDll = "OpenColorIO_" + (($deps.vcpkg.opencolorio -split "\.")[0..1] -join "_") + ".dll"
if (-not (Test-Path -LiteralPath (Join-Path $binDir $ocioDll))) { throw "$ocioDll missing from $VcpkgBin" }
if ($OpenSubdivRoot -and (Test-Path -LiteralPath (Join-Path $OpenSubdivRoot "bin"))) {
    Copy-Dlls (Join-Path $OpenSubdivRoot "bin") $binDir
}
if ($MaterialXRoot -and (Test-Path -LiteralPath (Join-Path $MaterialXRoot "bin"))) {
    Copy-Dlls (Join-Path $MaterialXRoot "bin") $binDir
}

# MSVC runtime: Visual Studio's redistributable folder, else System32.
$crtDir = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = (& $vswhere -latest -products * -property installationPath).Trim()
    if ($vsPath) {
        $crtDir = Get-ChildItem (Join-Path $vsPath "VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT") -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    }
}
if (-not $crtDir) { $crtDir = Join-Path $env:SystemRoot "System32" }
Copy-Dlls $crtDir $binDir @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140*.dll", "concrt140.dll", "vcomp140.dll")
Write-Host "MSVC runtime from $crtDir"

# OpenUSD: DLLs, Python modules, plugin resources. No .lib/.pdb/headers/tools.
$usdOut = Join-Path $pkg "usd"
Copy-Tree (Join-Path $UsdRoot "lib") (Join-Path $usdOut "lib") @("*.lib", "*.pdb", "*.exp", "*.h", "*.cmake") @("__pycache__", "cmake")
if (Test-Path -LiteralPath (Join-Path $UsdRoot "plugin")) {
    Copy-Tree (Join-Path $UsdRoot "plugin") (Join-Path $usdOut "plugin") @("*.lib", "*.pdb", "*.exp")
}
Copy-IfExists $UsdviewScript $pkg "usdview.py"

# MaterialX standard libraries (incl. libraries\mdl).
Copy-Tree (Join-Path $MaterialXRoot "libraries") (Join-Path $pkg "materialx\libraries") @("*.pdb")

# MDL SDK runtime: library + plugins, loaded from MDL_SDK_ROOT\bin.
$mdlOut = Join-Path $pkg "mdl\bin"
Copy-Dlls $mdlBin $mdlOut @("libmdl_sdk.dll", "libmdl_core.dll", "nv_openimageio.dll", "dds.dll")

# DXC compiles the GPU kernel at run time.
$dxcOut = Join-Path $pkg "dxc"
foreach ($f in @("dxc.exe", "dxcompiler.dll", "dxil.dll")) { Copy-IfExists (Join-Path $DxcDir $f) $dxcOut }
# PrismRT.exe imports the Vulkan loader, which machines without a GPU driver
# lack. The SDK does not ship it; use the LunarG runtime, else the system copy.
$loader = @($VulkanLoader, $env:PRISM_VULKAN_LOADER, (Join-Path $env:SystemRoot "System32\vulkan-1.dll")) |
    Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $loader) { throw "vulkan-1.dll not found. Pass -VulkanLoader." }
Copy-IfExists $loader $binDir
Write-Host "Vulkan loader from $loader"
Copy-IfExists (Join-Path (Split-Path -Parent (Split-Path -Parent $loader)) "VulkanRT-License.txt") (Join-Path $pkg "licenses") "VulkanRT-License.txt"

# 3. Private Python.
$pyOut = Join-Path $pkg "python"
New-Item -ItemType Directory -Path $pyOut | Out-Null
foreach ($f in @("python.exe", "pythonw.exe", "python3.dll", "LICENSE.txt")) { Copy-IfExists (Join-Path $PythonRoot $f) $pyOut }
Get-ChildItem -LiteralPath $PythonRoot -Filter "python3*.dll" -File | Copy-Item -Destination $pyOut -Force
Get-ChildItem -LiteralPath $PythonRoot -Filter "vcruntime140*.dll" -File | Copy-Item -Destination $pyOut -Force
Copy-Tree (Join-Path $PythonRoot "DLLs") (Join-Path $pyOut "DLLs") @("*.pdb", "*.lib", "_tkinter.pyd", "tcl*.dll", "tk*.dll") @()
Copy-Tree (Join-Path $PythonRoot "Lib") (Join-Path $pyOut "Lib") @("*.pdb") `
    @("site-packages", "__pycache__", "test", "tests", "idlelib", "tkinter", "turtledemo", "lib2to3", "ensurepip")
New-Item -ItemType Directory -Path (Join-Path $pyOut "Lib\site-packages") -Force | Out-Null
if (-not $SkipPip) {
    $site = Join-Path $pyOut "Lib\site-packages"
    $pythonExe = Join-Path $PythonRoot "python.exe"
    $savedPreference = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & $pythonExe -m pip install --target $site --no-warn-script-location --disable-pip-version-check `
        --only-binary=:all: --no-deps "PySide6-Essentials==$PySideVersion" "shiboken6==$PySideVersion" @PipPackages
    $pipExit = $LASTEXITCODE; $ErrorActionPreference = $savedPreference
    if ($pipExit) { throw "pip install failed: $pipExit" }
    # Qt developer files and tools are not needed to run usdview.
    foreach ($dir in @("include", "typesystems", "glue", "scripts", "translations", "examples")) {
        $p = Join-Path $site "PySide6\$dir"
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }
    Get-ChildItem (Join-Path $site "PySide6") -Include "*.exe", "*.lib", "*.pyi" -File -Recurse -ErrorAction SilentlyContinue |
        Remove-Item -Force
    Get-ChildItem $site -Directory -Filter "bin" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    # PyOpenColorIO's wheel carries its own OpenColorIO_2_6.dll and command line
    # tools in PyOpenColorIO\bin. Drop them: the module imports the DLL by name,
    # and usdview has already loaded the package's vcpkg one (Hdx) by then.
    $ocioBin = Join-Path $site "PyOpenColorIO\bin"
    if (Test-Path -LiteralPath $ocioBin) { Remove-Item -LiteralPath $ocioBin -Recurse -Force }
    # The shiboken generator and its headers are build-time only.
    foreach ($dir in @("shiboken6_generator", "shiboken6\include")) {
        $p = Join-Path $site $dir
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }
}

# OpenUSD's Hdx (usdview's OpenColorIO colour correction) links the same
# OpenColorIO as PrismRT and loads the vcpkg DLL in bin: exactly one copy ships
# (the PyOpenColorIO wheel's own copy was removed above).
$ocioCopies = @(Get-ChildItem -LiteralPath $pkg -Recurse -Filter "OpenColorIO*.dll" -File)
if ($ocioCopies.Count -ne 1) {
    throw "Expected one OpenColorIO DLL in the package, found: $($ocioCopies.FullName -join ', ')"
}
# USD, Embree and OIDN share one oneTBB.
$tbbCopies = @(Get-ChildItem -LiteralPath $pkg -Recurse -Filter $tbbDll -File)
if ($tbbCopies.Count -ne 1) {
    throw "Expected one $tbbDll in the package, found: $($tbbCopies.FullName -join ', ')"
}

# 4. Launchers and README (CRLF so cmd.exe is happy whatever the checkout's EOLs).
foreach ($name in @("usdview.bat", "prism_render.bat", "list_devices.bat", "env_setup.bat", "README.txt")) {
    $text = [IO.File]::ReadAllText((Join-Path $templates $name))
    $versions = @(
        "  OpenUSD $($deps.openusd.version), MaterialX $($deps.materialx.version), OpenSubdiv $($deps.opensubdiv.version)",
        ("  oneTBB $($deps.onetbb.version), Embree $($deps.embree.version), MDL SDK $($deps.mdl_sdk.version)" +
            $(if ($buildHasOidn) { ", Open Image Denoise $($deps.oidn.version)" } else { "" }) +
            $(if ($buildHasVkOidn) { ", vk_oidn $($deps.vk_oidn.version)" } else { "" })),
        "  OpenImageIO $($deps.vcpkg.openimageio), OpenColorIO $($deps.vcpkg.opencolorio), Python $($deps.python.version), PySide6 $PySideVersion"
    ) -join "`n"
    $text = $text.Replace("@PRISM_DEPENDENCY_VERSIONS@", $versions) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $pkg $name), $text, (New-Object Text.UTF8Encoding($false)))
}
"$sha`r`n" | Set-Content -LiteralPath (Join-Path $pkg "VERSION.txt") -NoNewline -Encoding ascii

# 5. Licenses where the dependencies ship them.
$lic = Join-Path $pkg "licenses"
Copy-IfExists (Join-Path $root "LICENSE") $lic "PrismRT-LICENSE.txt"
Copy-IfExists (Join-Path $root "NOTICE") $lic "PrismRT-NOTICE.txt"
Copy-IfExists (Join-Path $root "resources\shaderball\LICENCE") $lic "StandardShaderBall-LICENCE.txt"
$usdSource = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $UsdviewScript))))
foreach ($c in @((Join-Path $usdSource "LICENSE.txt"), (Join-Path $UsdRoot "LICENSE.txt"))) { Copy-IfExists $c $lic "OpenUSD-LICENSE.txt" }
Copy-IfExists (Join-Path $UsdRoot "NOTICE.txt") $lic "OpenUSD-NOTICE.txt"
# The changes PrismRT's build made to OpenUSD (build_usd_stack.ps1).
Get-ChildItem -LiteralPath (Join-Path $UsdRoot "patches") -Filter "*.patch" -File -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-IfExists $_.FullName (Join-Path $lic "OpenUSD-patches") $_.Name }
Copy-IfExists (Join-Path $MaterialXRoot "LICENSE") $lic "MaterialX-LICENSE.txt"
Copy-IfExists (Join-Path $MaterialXRoot "THIRD-PARTY.md") $lic "MaterialX-THIRD-PARTY.md"
foreach ($n in @("LICENSE.md", "LICENSE_THIRDPARTY.md", "LICENSE.txt", "LICENSE")) { Copy-IfExists (Join-Path $MdlSdkRoot $n) $lic "MDL-SDK-$n" }
$embreeRoot = Split-Path -Parent $EmbreeRuntime
foreach ($n in @("LICENSE.txt", "doc\LICENSE.txt", "third-party-programs.txt", "doc\third-party-programs.txt")) {
    Copy-IfExists (Join-Path $embreeRoot $n) $lic ("Embree-" + (Split-Path -Leaf $n))
}
if ($buildHasOidn) {
    $oidnDoc = Join-Path (Split-Path -Parent $OidnRuntime) "share\doc\OpenImageDenoise"
    foreach ($n in @("LICENSE.txt", "third-party-programs.txt", "third-party-programs-oneTBB.txt")) {
        Copy-IfExists (Join-Path $oidnDoc $n) $lic ("OpenImageDenoise-" + $n)
    }
}
# vk_oidn (Apache-2.0, derived from OIDN) and the OIDN weights packed for its prefilters.
if ($buildHasVkOidn) {
    Copy-IfExists (Join-Path $VkOidnRoot "LICENSE") $lic "vk_oidn-LICENSE.txt"
    Copy-IfExists (Join-Path $VkOidnRoot "NOTICE") $lic "vk_oidn-NOTICE.txt"
    Copy-IfExists (Join-Path $VkOidnRoot "prefilter\LICENSE-oidn-weights.txt") $lic "oidn-weights-LICENSE.txt"
}
$tbbRoot = $TbbRuntime
for ($i = 0; $i -lt 3; $i++) { $tbbRoot = Split-Path -Parent $tbbRoot }
foreach ($n in @("LICENSE.txt", "licensing\LICENSE.txt", "share\doc\TBB\LICENSE.txt", "share\doc\tbb\LICENSE.txt")) {
    Copy-IfExists (Join-Path $tbbRoot $n) $lic ("oneTBB-" + (Split-Path -Leaf $n))
}
Copy-IfExists (Join-Path $PythonRoot "LICENSE.txt") $lic "Python-LICENSE.txt"
$vcpkgShare = Join-Path (Split-Path -Parent $VcpkgBin) "share"
if (Test-Path -LiteralPath $vcpkgShare) {
    Get-ChildItem -LiteralPath $vcpkgShare -Directory | ForEach-Object {
        Copy-IfExists (Join-Path $_.FullName "copyright") (Join-Path $lic "vcpkg") ($_.Name + "-copyright.txt")
    }
}
if (-not $SkipPip) {
    Get-ChildItem (Join-Path $pyOut "Lib\site-packages") -Directory -Filter "*.dist-info" | ForEach-Object {
        $name = $_.Name -replace "\.dist-info$", ""
        foreach ($sub in @("", "licenses")) {
            Get-ChildItem (Join-Path $_.FullName $sub) -File -Filter "LICENSE*" -ErrorAction SilentlyContinue |
                ForEach-Object { Copy-IfExists $_.FullName (Join-Path $lic "python") ($name + "-" + $_.Name) }
        }
    }
}

# 5b. The longest installed paths, for the start-up check against Windows'
#     260-character limit (docs/deployment.md, "Path length"; src/renderer/
#     install_paths.h). "required" covers what the renderer opens (not the
#     private Python, licenses or .pyc caches); "all" everything but .pyc caches.
$pkgFull = (Resolve-Path -LiteralPath $pkg).Path.TrimEnd('\')
$longestRequired = 0; $longestAll = 0
Get-ChildItem -LiteralPath "\\?\$pkgFull" -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring(4 + $pkgFull.Length)   # "\bin\prism.dll"
    if ($rel -like "*\__pycache__\*") { return }
    $longestAll = [Math]::Max($longestAll, $rel.Length)
    if ($rel -notlike "\python\*" -and $rel -notlike "\licenses\*") {
        $longestRequired = [Math]::Max($longestRequired, $rel.Length)
    }
}
[IO.File]::WriteAllText((Join-Path $pkg "bin\prism_paths.txt"),
    "root=..`r`nrequired=$longestRequired`r`nall=$longestAll`r`n", (New-Object Text.UTF8Encoding($false)))
Write-Host "Longest installed path: $longestRequired characters (renderer), $longestAll (with Python)"

# 6. Zip. tar handles archives above 2 GB, unlike Compress-Archive.
$size = (Get-ChildItem -LiteralPath $pkg -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Package folder: {0:N0} MiB" -f ($size / 1MB))
if (-not $SkipZip) {
    & tar -a -c -f $zip -C $OutputDir "PrismRT"
    if ($LASTEXITCODE) { throw "tar failed: $LASTEXITCODE" }
    Write-Host ("Zip: {0} ({1:N0} MiB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB))
}
