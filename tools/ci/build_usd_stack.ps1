<#
.SYNOPSIS
  Builds OpenSubdiv, MaterialX and OpenUSD for the Windows CI, the way the
  development machine builds them (separate prefixes, RelWithDebInfo, oneTBB).

.DESCRIPTION
  Expects oneTBB's binary release already extracted (-TbbRoot) and Python 3.11
  on PATH (actions/setup-python). Installs into <Prefix>\osd, <Prefix>\mtlx and
  <Prefix>\usd, and removes the .pdb files so the result is small enough to cache.
  Bump the versions below together with the cache key in the workflow.
#>
param(
    [Parameter(Mandatory)] [string] $Prefix,
    [Parameter(Mandatory)] [string] $TbbRoot,
    [Parameter(Mandatory)] [string] $SourceDir,
    [string] $OpenSubdivTag = "v3_6_1",
    [string] $MaterialXTag = "v1.39.4",
    [string] $UsdTag = "v26.05",
    [string] $Configuration = "RelWithDebInfo"
)
$ErrorActionPreference = "Stop"
function Run([string] $what, [scriptblock] $block) {
    Write-Host "::group::$what"
    & $block
    if ($LASTEXITCODE) { throw "$what failed: $LASTEXITCODE" }
    Write-Host "::endgroup::"
}
function Clone([string] $url, [string] $tag, [string] $dest) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Run "clone $url $tag" { git clone --quiet --depth 1 --branch $tag $url $dest }
}
function Fwd([string] $path) { return $path.Replace('\', '/') }

New-Item -ItemType Directory -Force -Path $Prefix, $SourceDir | Out-Null
$python = (Get-Command python).Source
$pythonRoot = Split-Path -Parent $python
$tbbDir = Fwd (Join-Path $TbbRoot "lib\cmake\tbb")
$osdPrefix = Join-Path $Prefix "osd"
$mtlxPrefix = Join-Path $Prefix "mtlx"
$usdPrefix = Join-Path $Prefix "usd"
$generator = @("-G", "Visual Studio 17 2022", "-A", "x64")

# usdview is only built when its Python modules are importable.
Run "pip modules for the usdview build" {
    python -m pip install --disable-pip-version-check PySide6==6.7.2 PyOpenGL==3.1.7 numpy==2.2.6
}

# OpenSubdiv
$osdSrc = Join-Path $SourceDir "OpenSubdiv"
Clone "https://github.com/PixarAnimationStudios/OpenSubdiv" $OpenSubdivTag $osdSrc
Run "configure OpenSubdiv" {
    cmake -S $osdSrc -B "$osdSrc\build" @generator "-DCMAKE_INSTALL_PREFIX=$(Fwd $osdPrefix)" `
        -DBUILD_SHARED_LIBS=ON -DNO_TUTORIALS=1 -DNO_EXAMPLES=1 -DNO_REGRESSION=1 -DNO_TESTS=1 `
        -DNO_DOC=1 -DNO_CLEW=1 -DNO_CUDA=1 -DNO_OPENCL=1 -DNO_DX=1 -DNO_PTEX=1 -DNO_GLFW=1 -DNO_TBB=1
}
Run "build OpenSubdiv" { cmake --build "$osdSrc\build" --target install --config $Configuration --parallel }

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
Run "build MaterialX" { cmake --build "$mtlxSrc\build" --target install --config $Configuration --parallel }

# OpenUSD
$usdSrc = Join-Path $SourceDir "OpenUSD"
Clone "https://github.com/PixarAnimationStudios/OpenUSD" $UsdTag $usdSrc
Run "configure OpenUSD" {
    cmake -S $usdSrc -B "$usdSrc\build" @generator "-DCMAKE_INSTALL_PREFIX=$(Fwd $usdPrefix)" `
        "-DPython3_EXECUTABLE=$(Fwd $python)" "-DPython3_ROOT_DIR=$(Fwd $pythonRoot)" `
        "-DTBB_DIR=$tbbDir" -DPXR_FIND_TBB_IN_CONFIG=ON `
        "-DOpenSubdiv_DIR=$(Fwd $osdPrefix)/lib/cmake/OpenSubdiv" `
        "-DOPENSUBDIV_INCLUDE_DIR=$(Fwd $osdPrefix)/include/opensubdiv" `
        "-DMaterialX_DIR=$(Fwd $mtlxPrefix)/lib/cmake/MaterialX" `
        -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF `
        -DPXR_BUILD_DOCUMENTATION=OFF -DPXR_BUILD_HTML_DOCUMENTATION=OFF `
        -DPXR_BUILD_IMAGING=ON -DPXR_BUILD_USD_IMAGING=ON -DPXR_BUILD_USDVIEW=ON `
        -DPXR_BUILD_GPU_SUPPORT=ON -DPXR_ENABLE_PYTHON_SUPPORT=ON `
        -DPXR_ENABLE_MATERIALX_SUPPORT=ON -DPXR_BUILD_USD_TOOLS=ON
}
Run "build OpenUSD" { cmake --build "$usdSrc\build" --target install --config $Configuration --parallel }

# The package script needs the usdview entry point, which the install only
# provides as an extensionless wrapper.
Copy-Item (Join-Path $usdSrc "pxr\usdImaging\bin\usdview\usdview.py") (Join-Path $usdPrefix "lib\python\usdview.py")
Copy-Item (Join-Path $usdSrc "LICENSE.txt") (Join-Path $usdPrefix "LICENSE.txt")
Copy-Item (Join-Path $usdSrc "NOTICE.txt") (Join-Path $usdPrefix "NOTICE.txt") -ErrorAction SilentlyContinue

if (Test-Path -LiteralPath (Join-Path $usdPrefix "include\tbb")) {
    throw "The OpenUSD install contains legacy TBB headers; it was built against the wrong TBB."
}
Get-ChildItem $Prefix -Recurse -Filter *.pdb -File | Remove-Item -Force
Get-ChildItem $Prefix -Directory | ForEach-Object {
    "{0}: {1:N0} MiB" -f $_.Name, ((Get-ChildItem $_.FullName -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
}
