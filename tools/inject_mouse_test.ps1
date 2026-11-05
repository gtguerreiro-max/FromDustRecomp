# Attaches to a running FromDust (for example tools/validate_native_session.ps1
# -Visible) and, <At> seconds after the process started, holds the left button
# (absorb / LT), moves the mouse and turns the wheel, capturing the window
# before and after each step.
param([int]$At = 335, [string]$OutputDirectory = 'build\mouse-inject')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$OutputDirectory = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FdInject {
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
$p = $null
while (-not $p) { $p = Get-Process FromDust -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1; if (-not $p) { Start-Sleep 1 } }
$wait = $At - ((Get-Date) - $p.StartTime).TotalSeconds
if ($wait -gt 0) { Start-Sleep -Seconds $wait }
[FdInject]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500
$shot = { param($name) & "$root\tools\capture_window.ps1" -DelaySeconds 0 -Output "$OutputDirectory\$name.png" | Out-Null }
& $shot 'step0-start'
[FdInject]::Send(0, 0, 2, 0); Start-Sleep -Seconds 5; [FdInject]::Send(0, 0, 4, 0); Start-Sleep -Seconds 2
& $shot 'step1-after-left-button'
for ($i = 0; $i -lt 150; $i++) { [FdInject]::Send(14, -6, 1, 0); Start-Sleep -Milliseconds 16 }
& $shot 'step2-after-move'
for ($i = 0; $i -lt 3; $i++) { [FdInject]::Send(0, 0, 0x0800, [uint32]4294967176); Start-Sleep -Milliseconds 120 }
Start-Sleep -Milliseconds 600
& $shot 'step3-after-wheel'
"done -> $OutputDirectory"
