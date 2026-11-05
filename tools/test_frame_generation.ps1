# Bounded visible native session with DLSS Frame Generation on: checks that
# Streamline loads, frames are tagged and generated frames are presented, and
# captures the window at a few points.
# Usage: tools/test_frame_generation.ps1 [-Frames 1..3] [-Dlaa 1] [-RunSeconds 150]
param([ValidateRange(0,3)][int]$Frames=1, [ValidateRange(0,5)][int]$Dlaa=1,
      [ValidateRange(20,1800)][int]$RunSeconds=150, [int[]]$CaptureAt=@(70,110,140),
      [string]$Schedule='10:0010:600', [string]$OutputDirectory='', [switch]$Settings, [string]$Graphics='', [int]$GuestCaptureAt=0, [int]$DumpAt=0, [int]$FrameLimit=30, [int]$VblankHz=0, [switch]$KeepSave, [string]$Renderer='native', [switch]$NativeProfile, [int]$VSync=1, [string]$SaveFrom='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'build\frame-generation'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force|Out-Null
foreach($n in 'FD_NATIVE_DUMP_AT','FD_NATIVE_PROFILE','FD_REPLAY_DEBUG','FD_NATIVE_UPSCALER','FD_CAPTURE','FD_CAPTURE_SECONDS',
    'FD_SETTINGS_OPEN','FD_SETTINGS_TAB','FD_HIDE_WINDOW','FD_FRAME_GENERATION'){Remove-Item "Env:$n" -ErrorAction SilentlyContinue}
if($Settings){$env:FD_SETTINGS_OPEN='1';$env:FD_SETTINGS_TAB='graphics'}
if($GuestCaptureAt){$env:FD_CAPTURE=Join-Path $output 'guest.png';$env:FD_CAPTURE_SECONDS=[string]$GuestCaptureAt}
if($DumpAt){$env:FD_NATIVE_DUMP_AT=[string]$DumpAt}
if($VblankHz){$env:FD_VBLANK_HZ=[string]$VblankHz}else{Remove-Item Env:FD_VBLANK_HZ -ErrorAction SilentlyContinue}
# A clean save per session keeps the menu flow (Play -> intro -> level 1) reproducible.
$env:FD_USER_DIR=Join-Path $output 'user'
if(!$KeepSave){Remove-Item -Recurse -Force $env:FD_USER_DIR -ErrorAction SilentlyContinue}
# -SaveFrom <user folder>: start from a copy of an existing profile / save
# (for example build\saves\level1), so sessions skip the menus and intro.
if($SaveFrom){Remove-Item -Recurse -Force $env:FD_USER_DIR -ErrorAction SilentlyContinue; Copy-Item -Recurse -Force $SaveFrom $env:FD_USER_DIR}
if($NativeProfile){$env:FD_NATIVE_PROFILE='1'}
$env:FD_RENDERER=$Renderer;$env:FD_HEADLESS='0';$env:FD_EXIT_SECONDS=[string]$RunSeconds;$env:FD_AUTOPRESS=$Schedule
$env:FD_NATIVE_SHADER_CATALOG=Join-Path $root 'build\native-shaders-live\catalog.bin'
$env:FD_NATIVE_AUDIT_REPORT=Join-Path $output 'native-session.json';$env:FD_NATIVE_GEOMETRY_REPORT=Join-Path $output 'geometry.json'
$env:FD_SETTINGS_FILE=Join-Path $output 'settings.ini'
[IO.File]::WriteAllText($env:FD_SETTINGS_FILE,"[Graphics]`nScale=1`nVSync=$VSync`nFrameLimit=$FrameLimit`nAnisotropy=-1`nUpscaler=$Dlaa`nFrameGeneration=$Frames`n$Graphics`n[Display]`nShowFps=1`n")
$p=Start-Process -FilePath "$root\build\FromDust.exe" -ArgumentList ('"'+"$root\game"+'"') -WorkingDirectory $output -PassThru `
    -RedirectStandardOutput "$output\stdout.log" -RedirectStandardError "$output\stderr.log"
$start=Get-Date
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class FdFocus { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
'@
# Frame generation pauses while the window is not focused.
for($i=0;$i -lt 60 -and !$p.HasExited;$i++){ $p.Refresh(); if($p.MainWindowHandle -ne 0){break}; Start-Sleep 1 }
[FdFocus]::SetForegroundWindow($p.MainWindowHandle)|Out-Null
foreach($t in $CaptureAt){
    $wait=$t-((Get-Date)-$start).TotalSeconds
    if($wait -gt 0){Start-Sleep -Seconds $wait}
    if($p.HasExited){break}
    & "$root\tools\capture_window.ps1" -DelaySeconds 0 -Output "$output\window-$t.png"|Out-Null
}
$p.WaitForExit()
"exit $($p.ExitCode)"
Select-String -Path "$output\stderr.log","$output\stdout.log" -Pattern 'frame generation|streamline|Streamline' |
    Select-Object -First 40|ForEach-Object{$_.Line}

