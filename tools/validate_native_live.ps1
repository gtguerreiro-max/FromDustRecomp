param(
    [int]$RunSeconds = 300,
    [int]$MinimumDrawsPerFrame = 500,
    [int]$GeometryCaptureAt = 0,
    [int]$MinimumPatchedShaders = 0,
    [switch]$Screenshot,
    [string]$Catalog = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($RunSeconds -lt 1 -or $RunSeconds -gt 1800 -or $MinimumDrawsPerFrame -lt 1) { throw 'Invalid diagnostic limits' }
if ($GeometryCaptureAt -lt 0 -or $GeometryCaptureAt -ge $RunSeconds) { throw 'Geometry capture must precede the run limit' }
if($MinimumPatchedShaders -lt 0 -or $MinimumPatchedShaders -gt 10000 -or ($MinimumPatchedShaders -and !$GeometryCaptureAt)){throw 'Invalid patched shader coverage or missing geometry capture'}
if (!$Catalog) { $Catalog = Join-Path $root 'build\native-shaders\catalog.bin' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'build\native-live-validation' }
$catalogPath = (Resolve-Path -LiteralPath $Catalog).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$reportPath = Join-Path $output 'live-shader-audit.json'
$variables = @('FD_NATIVE_SHADER_CATALOG', 'FD_NATIVE_AUDIT_REPORT', 'FD_HEADLESS', 'FD_EXIT_SECONDS', 'FD_AUTOPRESS', 'FD_FRAME_CAPTURE', 'FD_RENDERER',
    'FD_NATIVE_GEOMETRY_CAPTURE', 'FD_NATIVE_GEOMETRY_REPORT', 'FD_NATIVE_GEOMETRY_MIN_INDEXED', 'FD_CAPTURE', 'FD_CAPTURE_SECONDS',
    'FD_SETTINGS_OPEN', 'FD_SETTINGS_TAB','FD_NATIVE_GEOMETRY_MIN_PATCHED','FD_NATIVE_TEXTURE_CAPTURE','FD_NATIVE_FRAME_CAPTURE')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:FD_NATIVE_SHADER_CATALOG = $catalogPath
    $env:FD_NATIVE_AUDIT_REPORT = $reportPath
    $env:FD_HEADLESS = '1'
    $env:FD_EXIT_SECONDS = "$RunSeconds"
    if ($GeometryCaptureAt) {
        $env:FD_NATIVE_GEOMETRY_CAPTURE = "$GeometryCaptureAt"
        $env:FD_NATIVE_GEOMETRY_REPORT = Join-Path $output 'live-geometry.json'
        $env:FD_NATIVE_GEOMETRY_MIN_INDEXED = "$MinimumDrawsPerFrame"
        $env:FD_NATIVE_GEOMETRY_MIN_PATCHED="$MinimumPatchedShaders"
    }
    else { Remove-Item Env:FD_NATIVE_GEOMETRY_CAPTURE -ErrorAction SilentlyContinue }
    Remove-Item Env:FD_FRAME_CAPTURE, Env:FD_RENDERER, Env:FD_CAPTURE, Env:FD_CAPTURE_SECONDS, Env:FD_SETTINGS_OPEN, Env:FD_SETTINGS_TAB, Env:FD_NATIVE_TEXTURE_CAPTURE, Env:FD_NATIVE_FRAME_CAPTURE -ErrorAction SilentlyContinue
    if($Screenshot) {
        if($RunSeconds -lt 20){throw 'Screenshot diagnostics require at least 20 seconds'}
        $env:FD_CAPTURE=Join-Path $output 'scene.png'
        $env:FD_CAPTURE_SECONDS=[string]($RunSeconds-10)
    }
    # Intro dismissal and title confirmation are separate Start presses.
    # This remains a bounded timing-based diagnostic: the scene gate below
    # must reject a run if slower loading leaves it in the intro or menu.
    $presses = @('10:0010:600','20:0010:600','30:0010:600','40:0010:600','90:0010:600','120:0010:600')
    $presses += @(95..200 | Where-Object { ($_ - 95) % 5 -eq 0 } | ForEach-Object { "${_}:1000:600" })
    $env:FD_AUTOPRESS = $presses -join ','
    $started = Get-Date
    $process = Start-Process -FilePath (Join-Path $root 'build\FromDust.exe') -WorkingDirectory $output -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log') -PassThru
    $process.WaitForExit()
    if ($process.ExitCode) { throw "Runtime shader audit failed ($($process.ExitCode)); see $output" }
    if($Screenshot) {
        $scene=Join-Path $output 'scene.png'
        if(!(Test-Path -LiteralPath $scene) -or (Get-Item -LiteralPath $scene).LastWriteTime -lt $started){throw 'Missing or stale runtime scene screenshot'}
    }
    if (!(Test-Path -LiteralPath $reportPath) -or (Get-Item -LiteralPath $reportPath).LastWriteTime -lt $started) {
        throw 'The runtime did not produce a fresh native shader audit report'
    }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (!$report.shader_coverage_passed -or $report.unresolved_draw_calls -or !$report.completed_frames) {
        throw 'Live shader identity coverage failed'
    }
    if (!$report.indexed_calls -or $report.last_frame_draw_calls -lt $MinimumDrawsPerFrame) {
        throw 'The run did not reach the required scene complexity; intro/menu coverage is insufficient'
    }
    if ($GeometryCaptureAt) {
        $geometryPath = Join-Path $output 'live-geometry.json'
        if (!(Test-Path -LiteralPath $geometryPath) -or (Get-Item -LiteralPath $geometryPath).LastWriteTime -lt $started) {
            throw 'The runtime did not produce a fresh native geometry report'
        }
        $geometry = Get-Content -LiteralPath $geometryPath -Raw | ConvertFrom-Json
        if (!$geometry.complete -or !$geometry.passed -or $geometry.failures -or !$geometry.geometry_packets) { throw 'Live native geometry capture failed' }
        if($geometry.patched_shader_snapshots -lt $MinimumPatchedShaders){throw 'The run did not exercise vertex shader patches during submission'}
        if (!$geometry.clear_packets -or !$geometry.resolve_packets -or
            $geometry.ordered_commands -ne ($geometry.geometry_packets + $geometry.clear_packets + $geometry.resolve_packets) -or
            $geometry.state_packets -ne $geometry.ordered_commands) { throw 'Live native clear/resolve ordering coverage failed' }
        Write-Output ("Native geometry copied: {0} packets, {1} vertex bytes, {2} index bytes" -f `
            $geometry.geometry_packets, $geometry.vertex_bytes, $geometry.index_bytes)
        Write-Output ("Native ordered operations: {0} clears, {1} resolves, {2} total commands" -f `
            $geometry.clear_packets, $geometry.resolve_packets, $geometry.ordered_commands)
        Write-Output ("Completed vertex layouts captured after patching: {0}" -f $geometry.patched_shader_snapshots)
    }
    Write-Output ("Live shader identities passed: {0} frames, {1} CPU draw calls, {2} shaders, no unresolved draws" -f `
        $report.completed_frames, $report.cpu_draw_calls, $report.used_shaders.Count)
    Write-Output 'This validates the live shader registry; gameplay rendering still uses Xenia.'
}
finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
