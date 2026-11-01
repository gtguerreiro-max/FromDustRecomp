# Boots the game for a while and summarizes the log.
# Usage: .\run.ps1 [-Seconds 15] [-Trace] [-Watchdog 0] [-Headless] [-Capture frame.png]
param(
    [ValidateRange(1, 3600)][int]$Seconds = 15,
    [switch]$Trace,
    [int]$Watchdog = 0,
    [ValidateSet('d3d12', 'null')][string]$Renderer = 'd3d12',
    [switch]$Headless,
    [string]$Capture,
    [ValidateRange(1, 3600)][int]$CaptureSeconds = 10
)
$ErrorActionPreference = 'Stop'
$build = "$PSScriptRoot\build"
$env:FD_TRACE = if ($Trace) { "1" } else { "" }
$env:FD_WATCHDOG = if ($Watchdog) { "$Watchdog" } else { "" }
$env:FD_RENDERER = $Renderer
$env:FD_HEADLESS = if ($Headless) { '1' } else { '' }
$env:FD_CAPTURE = if ($Capture) { [IO.Path]::GetFullPath($Capture) } else { '' }
$env:FD_CAPTURE_SECONDS = "$CaptureSeconds"
Push-Location $build
$p = Start-Process -FilePath .\FromDust.exe -ArgumentList "`"$PSScriptRoot\game`"" -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput out.txt -RedirectStandardError err.txt
if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force; "still running after $Seconds s (stopped)" }
else { "process exited with code $($p.ExitCode)" }
$log = Get-Content FromDust.log
Pop-Location

"log lines: $($log.Count)"
$crash = ($log | Select-String '=== crash' | Select-Object -First 1).LineNumber
if ($crash) { $log[($crash - 1)..([Math]::Min($log.Count - 1, $crash + 14))] }
$log | Select-String 'FATAL|gpu: \d+ fps|thread \d+ (created|exited)|XexLoadImage\(|thread name' | Select-Object -Last 12 | ForEach-Object { $_.Line }
$log | Select-String 'gpu:.*(initialized|capture|image available|failed)' | ForEach-Object { $_.Line }
$stubs = $log | Select-String 'unimplemented:' | ForEach-Object { ($_.Line -split 'unimplemented: ')[1] } | Select-Object -Unique
"unimplemented: " + ($stubs -join ', ')
if ($Watchdog) {
    $log | Select-String 'guest thread \d+ ---|  #\d+ ' | Select-Object -Last 40 | ForEach-Object { $_.Line }
}
