param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $UsdviewArgs
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $projectRoot "prism_env.ps1")

$usdview = $env:PRISM_USDVIEW
if ($usdview -and -not (Test-Path -LiteralPath $usdview)) { $usdview = $null }
# Prefer the Python entry point when available. Some Windows USD builds
# install a batch wrapper as the extensionless script that usdview.cmd feeds
# to Python. Running the actual entry point avoids that broken wrapper chain.
$pythonEntries = @()
if ($env:PRISM_USD_ROOT) {
    $pythonEntries += Join-Path $env:PRISM_USD_ROOT "lib\python\usdview.py"
}
if ($env:PRISM_USD_SOURCE_ROOT) {
    $pythonEntries += Join-Path $env:PRISM_USD_SOURCE_ROOT "pxr\usdImaging\bin\usdview\usdview.py"
} elseif ($env:PRISM_USD_ROOT) {
    # Conventional source/install layout; no assumption about its machine path.
    $pythonEntries += Join-Path (Split-Path -Parent $env:PRISM_USD_ROOT) "pxr\usdImaging\bin\usdview\usdview.py"
}
if (-not $usdview) {
    $usdview = $pythonEntries | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $usdview) {
    $command = Get-Command usdview -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $usdview = $command.Source }
}
if (-not $usdview -and $env:PRISM_USD_ROOT) {
    foreach ($candidate in @("bin\usdview.exe", "bin\usdview.cmd", "bin\usdview.bat",
                             "lib\python\usdview.py")) {
        $path = Join-Path $env:PRISM_USD_ROOT $candidate
        if (Test-Path -LiteralPath $path) { $usdview = $path; break }
    }
}
if (-not $usdview) {
    throw "usdview was not found. Put it on PATH or set PRISM_USDVIEW to its launcher."
}
if ($usdview -match '\.py$') {
    $python = if ($env:PRISM_PYTHON_ROOT) { Join-Path $env:PRISM_PYTHON_ROOT "python.exe" } else { "python" }
    & $python $usdview @UsdviewArgs
} else {
    & $usdview @UsdviewArgs
}
exit $LASTEXITCODE
