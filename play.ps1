# Launch the interactive runtime without the diagnostic runner's time limit.
param([ValidateSet('d3d12','native')][string]$Renderer='d3d12')
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'build\FromDust.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw 'Build the runtime first with .\build_all.ps1 -SkipRecomp'
}
$env:FD_RENDERER = $Renderer
$env:FD_HEADLESS = '0'
$env:FD_HIDE_WINDOW = '0'
$env:FD_TRACE = '0'
$env:FD_WATCHDOG = '0'
# An interactive launch must not inherit unattended test timers or inputs.
foreach ($name in @('FD_CAPTURE', 'FD_CAPTURE_SECONDS', 'FD_EXIT_SECONDS', 'FD_AUTOPRESS',
    'FD_FRAME_CAPTURE', 'FD_NATIVE_SHADER_CATALOG', 'FD_NATIVE_AUDIT_REPORT',
    'FD_NATIVE_GEOMETRY_CAPTURE', 'FD_NATIVE_GEOMETRY_REPORT', 'FD_NATIVE_GEOMETRY_MIN_INDEXED',
    'FD_SETTINGS_OPEN', 'FD_SETTINGS_TAB','FD_NATIVE_GEOMETRY_MIN_PATCHED','FD_NATIVE_TEXTURE_CAPTURE','FD_NATIVE_FRAME_CAPTURE',
    'FD_NATIVE_DUMP_AT','FD_NATIVE_PROFILE','FD_TRACE_FRAME_COUNTS','FD_GPU_QUERY_TRACE','FD_GPU_EVENT_TRACE',
    'FD_REPLAY_DEBUG','FD_REPLAY_TEST','FD_SETTINGS_FILE',
    'FD_PROFILE', 'FD_PROFILE_DELAY', 'FD_PROFILE_THREAD', 'FD_FUNC_COUNTS')) {
    Remove-Item "Env:\$name" -ErrorAction SilentlyContinue
}
if($Renderer -eq 'native') {
    $catalog=Join-Path $PSScriptRoot 'build\native-shaders-live\catalog.bin'
    if(!(Test-Path -LiteralPath $catalog)){throw 'Falta o catalogo nativo. Gera-o com python tools/build_native_shaders.py --out build/native-shaders-live'}
    $env:FD_NATIVE_SHADER_CATALOG=$catalog
    Write-Host 'From Dust interativo: render nativo Direct3D 12 (experimental, resolucao interna 1x).'
} else {
    Write-Host 'From Dust interativo: Xenia/Direct3D 12.'
}
Push-Location (Join-Path $PSScriptRoot 'build')
try {
    & $exe (Join-Path $PSScriptRoot 'game')
} finally {
    Pop-Location
}
