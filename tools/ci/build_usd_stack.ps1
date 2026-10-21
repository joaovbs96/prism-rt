<#
.SYNOPSIS
  Builds OpenSubdiv, MaterialX and OpenUSD for the Windows CI, the way the
  development machine builds them (separate prefixes, RelWithDebInfo, oneTBB).

.DESCRIPTION
  Expects oneTBB's binary release already extracted (-TbbRoot) and the Python
  from tools/deps.json on PATH (actions/setup-python). Installs into
  <Prefix>\osd, <Prefix>\mtlx and <Prefix>\usd (or the explicit -OsdPrefix,
  -MaterialXPrefix and -UsdPrefix), and removes the .pdb files so the result is
  small enough to cache. The tags and pip versions come from tools/deps.json;
  tools/build_deps_local.ps1 runs this same script on a development machine.
#>
param(
    [string] $Prefix = "",
    [Parameter(Mandatory)] [string] $TbbRoot,
    [Parameter(Mandatory)] [string] $SourceDir,
    [string] $OsdPrefix = "",
    [string] $MaterialXPrefix = "",
    [string] $UsdPrefix = "",
    [string] $Configuration = "RelWithDebInfo",
    # Parallel build jobs; 0 lets MSBuild use every core.
    [int] $Jobs = 0,
    # Use the already-installed pip modules instead of installing the pinned
    # ones (a developer's own Python); the script still checks that they import.
    [switch] $SkipPipInstall,
    [switch] $KeepPdb
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) "deps.psm1") -Force
$deps = Get-PrismDeps
$OpenSubdivTag = $deps.opensubdiv.tag
$MaterialXTag = $deps.materialx.tag
$UsdTag = $deps.openusd.tag

function Run([string] $what, [scriptblock] $block) {
    Write-Host "::group::$what"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    # Windows PowerShell turns redirected native stderr (CMake warnings) into
    # terminating errors under "Stop"; the exit code is the real verdict.
    $saved = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = 0
    try { & $block } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE) { throw "$what failed: $LASTEXITCODE" }
    Write-Host ("{0} took {1:N0} s" -f $what, $sw.Elapsed.TotalSeconds)
    Write-Host "::endgroup::"
}
function Clone([string] $url, [string] $tag, [string] $dest) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Run "clone $url $tag" { git clone --quiet --depth 1 --branch $tag $url $dest }
}
function Fwd([string] $path) { return $path.Replace('\', '/') }

if (-not $Prefix -and -not ($OsdPrefix -and $MaterialXPrefix -and $UsdPrefix)) {
    throw "Pass -Prefix, or all of -OsdPrefix, -MaterialXPrefix and -UsdPrefix."
}
$osdPrefix = if ($OsdPrefix) { $OsdPrefix } else { Join-Path $Prefix "osd" }
$mtlxPrefix = if ($MaterialXPrefix) { $MaterialXPrefix } else { Join-Path $Prefix "mtlx" }
$usdPrefix = if ($UsdPrefix) { $UsdPrefix } else { Join-Path $Prefix "usd" }
New-Item -ItemType Directory -Force -Path $osdPrefix, $mtlxPrefix, $usdPrefix, $SourceDir | Out-Null
$python = (Get-Command python).Source
$pythonRoot = Split-Path -Parent $python
$pyVersion = (& $python -c "import sys; print('%d.%d' % sys.version_info[:2])").Trim()
if ($pyVersion -ne $deps.python.version) {
    throw "Python $($deps.python.version) expected (tools/deps.json), found $pyVersion at $python"
}
$tbbDir = Fwd (Join-Path $TbbRoot "lib\cmake\tbb")
if (-not (Test-Path -LiteralPath (Join-Path $TbbRoot "lib\cmake\tbb"))) { throw "No oneTBB CMake package under $TbbRoot" }
$generator = @("-G", "Visual Studio 17 2022", "-A", "x64")
# @() outside the if: a one-element array returned from it unwraps to a string,
# and splatting a string passes it one character at a time.
$parallel = @(if ($Jobs -gt 0) { "--parallel", "$Jobs" } else { "--parallel" })
Write-Host "OpenSubdiv $OpenSubdivTag -> $osdPrefix"
Write-Host "MaterialX $MaterialXTag -> $mtlxPrefix"
Write-Host "OpenUSD $UsdTag -> $usdPrefix"
Write-Host "oneTBB $TbbRoot, Python $pyVersion $python"

# usdview is only built when its Python modules are importable.
if (-not $SkipPipInstall) {
    $requirements = Get-PrismPipRequirements
    Run "pip modules for the usdview build" {
        python -m pip install --disable-pip-version-check @requirements
    }
}
Run "check the usdview Python modules" {
    python -c "import PySide6, OpenGL; print('PySide6', PySide6.__version__, 'PyOpenGL', OpenGL.__version__)"
}

# OpenSubdiv
$osdSrc = Join-Path $SourceDir "OpenSubdiv"
Clone "https://github.com/PixarAnimationStudios/OpenSubdiv" $OpenSubdivTag $osdSrc
Run "configure OpenSubdiv" {
    cmake -S $osdSrc -B "$osdSrc\build" @generator "-DCMAKE_INSTALL_PREFIX=$(Fwd $osdPrefix)" `
        -DBUILD_SHARED_LIBS=ON -DNO_TUTORIALS=1 -DNO_EXAMPLES=1 -DNO_REGRESSION=1 -DNO_TESTS=1 `
        -DNO_DOC=1 -DNO_CLEW=1 -DNO_CUDA=1 -DNO_OPENCL=1 -DNO_DX=1 -DNO_PTEX=1 -DNO_GLFW=1 -DNO_TBB=1
}
Run "build OpenSubdiv" { cmake --build "$osdSrc\build" --target install --config $Configuration @parallel }

# MaterialX
$mtlxSrc = Join-Path $SourceDir "MaterialX"
Clone "https://github.com/AcademySoftwareFoundation/MaterialX" $MaterialXTag $mtlxSrc
Run "configure MaterialX" {
    cmake -S $mtlxSrc -B "$mtlxSrc\build" @generator "-DCMAKE_INSTALL_PREFIX=$(Fwd $mtlxPrefix)" `
        -DMATERIALX_BUILD_GEN_MDL=ON -DMATERIALX_BUILD_GEN_GLSL=ON -DMATERIALX_BUILD_GEN_OSL=ON `
        -DMATERIALX_BUILD_GEN_ESSL=ON -DMATERIALX_BUILD_GEN_MSL=ON -DMATERIALX_BUILD_PYTHON=OFF `
        -DMATERIALX_BUILD_VIEWER=OFF -DMATERIALX_BUILD_GRAPH_EDITOR=OFF -DMATERIALX_BUILD_RENDER=ON `
        -DMATERIALX_BUILD_TESTS=OFF -DMATERIALX_INSTALL_PYTHON=OFF -DMATERIALX_BUILD_DOCS=OFF
}
Run "build MaterialX" { cmake --build "$mtlxSrc\build" --target install --config $Configuration @parallel }

# OpenUSD. Since 26.08 the Python modules default to Lib\site-packages on
# Windows; PXR_PYTHON_INSTALL_DIR keeps the lib\python layout that
# prism_env.ps1, the package and the tests expect.
$usdSrc = Join-Path $SourceDir "OpenUSD"
Clone "https://github.com/PixarAnimationStudios/OpenUSD" $UsdTag $usdSrc
Run "configure OpenUSD" {
    cmake -S $usdSrc -B "$usdSrc\build" @generator "-DCMAKE_INSTALL_PREFIX=$(Fwd $usdPrefix)" `
        "-DPython3_EXECUTABLE=$(Fwd $python)" "-DPython3_ROOT_DIR=$(Fwd $pythonRoot)" `
        "-DTBB_DIR=$tbbDir" -DPXR_FIND_TBB_IN_CONFIG=ON `
        -DPXR_PYTHON_INSTALL_DIR=lib/python `
        "-DOpenSubdiv_DIR=$(Fwd $osdPrefix)/lib/cmake/OpenSubdiv" `
        "-DOPENSUBDIV_INCLUDE_DIR=$(Fwd $osdPrefix)/include/opensubdiv" `
        "-DMaterialX_DIR=$(Fwd $mtlxPrefix)/lib/cmake/MaterialX" `
        -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF `
        -DPXR_BUILD_DOCUMENTATION=OFF -DPXR_BUILD_HTML_DOCUMENTATION=OFF `
        -DPXR_BUILD_IMAGING=ON -DPXR_BUILD_USD_IMAGING=ON -DPXR_BUILD_USDVIEW=ON `
        -DPXR_BUILD_GPU_SUPPORT=ON -DPXR_ENABLE_PYTHON_SUPPORT=ON `
        -DPXR_ENABLE_MATERIALX_SUPPORT=ON -DPXR_BUILD_USD_TOOLS=ON
}
Run "build OpenUSD" { cmake --build "$usdSrc\build" --target install --config $Configuration @parallel }

# The package script needs the usdview entry point, which the install only
# provides as an extensionless wrapper.
Copy-Item (Join-Path $usdSrc "pxr\usdImaging\bin\usdview\usdview.py") (Join-Path $usdPrefix "lib\python\usdview.py")
Copy-Item (Join-Path $usdSrc "LICENSE.txt") (Join-Path $usdPrefix "LICENSE.txt")
Copy-Item (Join-Path $usdSrc "NOTICE.txt") (Join-Path $usdPrefix "NOTICE.txt") -ErrorAction SilentlyContinue

if (Test-Path -LiteralPath (Join-Path $usdPrefix "include\tbb")) {
    throw "The OpenUSD install contains legacy TBB headers; it was built against the wrong TBB."
}
if (-not (Test-Path -LiteralPath (Join-Path $usdPrefix "lib\python\pxr\Usdviewq"))) {
    throw "The OpenUSD install has no usdview (pxr.Usdviewq); were PySide6 and PyOpenGL importable?"
}
foreach ($dir in @($osdPrefix, $mtlxPrefix, $usdPrefix)) {
    if (-not $KeepPdb) { Get-ChildItem $dir -Recurse -Filter *.pdb -File | Remove-Item -Force }
    "{0}: {1:N0} MiB" -f $dir, ((Get-ChildItem $dir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
}
