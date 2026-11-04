param(
    [Parameter(Mandatory=$true)][string]$ReplayDir,
    [Parameter(Mandatory=$true)][string]$Reference,
    [string]$Target = 'frame.png',
    [string]$CaptureDir,
    [string]$RenderDocCapture,
    [string]$ShaderCatalog,
    [switch]$AllowRdcPrefix,
    [double]$MinimumPsnr = 0,
    [double]$MaximumOutlierPercent = 100,
    [switch]$RequirePassAgreement
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$replay = (Resolve-Path -LiteralPath $ReplayDir).Path
$referencePath = (Resolve-Path -LiteralPath $Reference).Path
$imagePath = Join-Path $replay $Target
$started = Get-Date
$nativeArguments = @()
if ($ShaderCatalog) { $nativeArguments += @('--shader-catalog', (Resolve-Path -LiteralPath $ShaderCatalog).Path) }
$nativeArguments += $replay
& (Join-Path $root 'build\fd_replay\fd_replay.exe') @nativeArguments > (Join-Path $replay 'validation-native.log') 2>&1
if ($LASTEXITCODE -ne 0) { throw "Native replay failed ($LASTEXITCODE); see validation-native.log" }
$nativeReportPath = Join-Path $replay 'native-report.json'
if (!(Test-Path -LiteralPath $nativeReportPath) -or (Get-Item -LiteralPath $nativeReportPath).LastWriteTime -lt $started) {
    throw 'Native replay did not produce a fresh execution report'
}
$nativeReport = Get-Content -LiteralPath $nativeReportPath -Raw | ConvertFrom-Json
$index = Get-Content -LiteralPath (Join-Path $replay 'replay-index.json') -Raw | ConvertFrom-Json
$expectedDraws = @($index.records | Where-Object { $_.kind -eq 'draw' }).Count
if (!$nativeReport.full_frame -or $nativeReport.game_draws -ne $expectedDraws -or $nativeReport.debug_errors) {
    throw 'Native execution was partial, skipped draws, or reported D3D12 errors'
}
if ($env:FD_REPLAY_DEBUG -and !$nativeReport.debug_active) { throw 'Requested D3D12 debug validation was not active' }
if ($ShaderCatalog -and !$nativeReport.shader_catalog) { throw 'Requested native shader catalog was not used' }
if (!(Test-Path -LiteralPath $imagePath) -or (Get-Item -LiteralPath $imagePath).LastWriteTime -lt $started) {
    throw "The replay did not produce a new $Target"
}
& python (Join-Path $PSScriptRoot 'img_compare.py') $imagePath $referencePath --strict-size --require-opaque --json (Join-Path $replay 'validation-image.json') --min-psnr $MinimumPsnr --max-outlier-percent $MaximumOutlierPercent
if ($LASTEXITCODE -ne 0) { throw 'Image comparison failed' }
& python (Join-Path $PSScriptRoot 'validate_effect_regions.py') $imagePath $referencePath (Join-Path $replay 'replay-index.json') --json (Join-Path $replay 'validation-effects.json')
if ($LASTEXITCODE -ne 0) { throw 'Effect region comparison failed' }
if ($CaptureDir) {
    & python (Join-Path $PSScriptRoot 'compare_replay_passes.py') --capture-dir $CaptureDir --replay-dir $replay --json (Join-Path $replay 'validation-passes.json')
    if ($LASTEXITCODE -ne 0) { throw 'Pass comparison failed' }
}
if ($RenderDocCapture) {
    if (!$CaptureDir) { throw '-RenderDocCapture also requires -CaptureDir from the same frame' }
    $rdocArgs = @((Join-Path $PSScriptRoot 'rdoc\compare_passes.py'), $RenderDocCapture,
        '--capture-dir', $CaptureDir, '--replay-dir', $replay)
    if ($AllowRdcPrefix) { $rdocArgs += '--allow-prefix' }
    & python @rdocArgs
    if ($LASTEXITCODE -ne 0) { throw 'Exact-event comparison failed' }
    if ($RequirePassAgreement) {
        $report = Get-Content -LiteralPath (Join-Path $replay 'validation-rdoc.json') -Raw | ConvertFrom-Json
        if ($report.unmatched_draws.Count -or $report.unverified_resolves.Count -or $report.first_difference -or !$report.compared_resolves) {
            throw 'The exact-event comparison is incomplete or a checkpoint exceeds the error threshold'
        }
    }
}
elseif ($RequirePassAgreement) { throw '-RequirePassAgreement requires -RenderDocCapture' }
