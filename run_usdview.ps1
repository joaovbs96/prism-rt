param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $UsdviewArgs
)

$ErrorActionPreference = "Stop"
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "prism_env.ps1")

$usdInstall = Join-Path $projectsRoot "external_deps\OpenUSD\install"
$usdSource = Join-Path $projectsRoot "external_deps\OpenUSD"
$pythonExe = Join-Path $pythonRoot "python.exe"
$usdBin = Join-Path $usdInstall "bin"
$usdview = $null
$usdviewPython = @(
    (Join-Path $usdInstall "lib\python\usdview.py"),
    (Join-Path $usdSource "pxr\usdImaging\bin\usdview\usdview.py")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $usdviewPython) {
    foreach ($candidate in @("usdview.cmd", "usdview.bat")) {
        $candidatePath = Join-Path $usdBin $candidate
        if (Test-Path -LiteralPath $candidatePath) {
            $usdview = $candidatePath
            break
        }
    }
}

if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw "Python 3.11 was not found at $pythonExe. OpenUSD's Python modules and usdview use this interpreter."
}
if (-not $usdviewPython -and -not $usdview) {
    throw "usdview was not found under $usdBin. Rebuild the existing OpenUSD installation with usdview enabled."
}

if ($usdviewPython) {
    & $pythonExe $usdviewPython @UsdviewArgs
} elseif ($usdview -match '\.(cmd|bat)$') {
    & $usdview @UsdviewArgs
} else {
    & $pythonExe $usdview @UsdviewArgs
}
exit $LASTEXITCODE
