# Launch the interactive runtime without the diagnostic runner's time limit.
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'build\FromDust.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw 'Build the runtime first with .\build_all.ps1 -SkipRecomp'
}
$env:FD_RENDERER = 'd3d12'
$env:FD_HEADLESS = '0'
$env:FD_HIDE_WINDOW = '0'
$env:FD_TRACE = '0'
$env:FD_WATCHDOG = '0'
Remove-Item Env:\FD_CAPTURE -ErrorAction SilentlyContinue
Push-Location (Join-Path $PSScriptRoot 'build')
try {
    & $exe (Join-Path $PSScriptRoot 'game')
} finally {
    Pop-Location
}
