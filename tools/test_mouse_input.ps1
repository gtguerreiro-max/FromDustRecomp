# Interactive check of the PC-style mouse: after the game reaches a level,
# moves the mouse (relative SendInput) and captures the window before / after.
# Usage: tools/test_mouse_input.ps1 -At 325 -OutputDirectory build\mouse-test
param([int]$At = 325, [int]$RunSeconds = 345, [string]$OutputDirectory = '',
      [string]$Schedule = '10:0010:150,40:0010:150,60:1000:150,65:1000:150,230:0:30000:255:0,310:4000:150')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'build\mouse-test' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FdMouse {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; public uint pad0, pad1; }
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    public static void Send(int dx, int dy, uint flags, uint data) {
        var input = new INPUT[1]; input[0].type = 0;
        input[0].mi.dx = dx; input[0].mi.dy = dy; input[0].mi.flags = flags; input[0].mi.data = data;
        SendInput(1, input, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
foreach ($n in 'FD_NATIVE_DUMP_AT','FD_NATIVE_PROFILE','FD_REPLAY_DEBUG','FD_NATIVE_UPSCALER','FD_CAPTURE','FD_CAPTURE_SECONDS','FD_SETTINGS_OPEN','FD_SETTINGS_TAB') { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
$env:FD_RENDERER = 'native'; $env:FD_HEADLESS = '0'; $env:FD_EXIT_SECONDS = [string]$RunSeconds; $env:FD_AUTOPRESS = $Schedule
$env:FD_NATIVE_SHADER_CATALOG = Join-Path $root 'build\native-shaders-live\catalog.bin'
$env:FD_NATIVE_AUDIT_REPORT = Join-Path $OutputDirectory 'native-session.json'; $env:FD_NATIVE_GEOMETRY_REPORT = Join-Path $OutputDirectory 'geometry.json'
$env:FD_SETTINGS_FILE = Join-Path $OutputDirectory 'settings.ini'
[IO.File]::WriteAllText($env:FD_SETTINGS_FILE, "[Graphics]`nScale=1`nVSync=1`nFrameLimit=30`nAnisotropy=-1`nUpscaler=1`n")
$p = Start-Process -FilePath "$root\build\FromDust.exe" -ArgumentList ('"' + "$root\game" + '"') -WorkingDirectory $OutputDirectory -PassThru `
    -RedirectStandardOutput "$OutputDirectory\stdout.log" -RedirectStandardError "$OutputDirectory\stderr.log"
Start-Sleep -Seconds ($At - 2)
[FdMouse]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Seconds 2
# Hold the left button (absorb / LT) for 5 s: in the tutorial this creates the Breath.
[FdMouse]::Send(0, 0, 2, 0); Start-Sleep -Seconds 5; [FdMouse]::Send(0, 0, 4, 0); Start-Sleep -Seconds 2
& "$root\tools\capture_window.ps1" -DelaySeconds 0 -Output "$OutputDirectory\before.png" | Out-Null
# 2.4 s of steady rightward / upward motion (MOUSEEVENTF_MOVE = 1).
for ($i = 0; $i -lt 150; $i++) { [FdMouse]::Send(14, -6, 1, 0); Start-Sleep -Milliseconds 16 }
& "$root\tools\capture_window.ps1" -DelaySeconds 0 -Output "$OutputDirectory\after-move.png" | Out-Null
# Wheel: three notches towards the user (zoom out with the default mapping).
for ($i = 0; $i -lt 3; $i++) { [FdMouse]::Send(0, 0, 0x0800, [uint32]4294967176); Start-Sleep -Milliseconds 120 }
Start-Sleep -Milliseconds 600
& "$root\tools\capture_window.ps1" -DelaySeconds 0 -Output "$OutputDirectory\after-wheel.png" | Out-Null
$p.WaitForExit(); "exit $($p.ExitCode)"
