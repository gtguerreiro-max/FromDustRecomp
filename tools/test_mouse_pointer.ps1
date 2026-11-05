# Mouse pointing check: runs a first-level session and moves the Windows
# cursor over the game window at fixed times (fractions of the client area),
# capturing the window after each move.
# Usage: tools/test_mouse_pointer.ps1 [-OutputDirectory dir]
param([string]$OutputDirectory = '', [int]$StartAt = 300,
      [string]$Points = '0.5,0.6;0.25,0.6;0.75,0.6;0.5,0.35;0.5,0.85')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'build\mouse-pointer' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class FdMouse {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
}
'@
$list = $Points.Split(';') | ForEach-Object { , ($_.Split(',') | ForEach-Object { [double]$_ }) }
$runSeconds = $StartAt + 6 * $list.Count + 5
$capture = @(); for ($i = 0; $i -lt $list.Count; $i++) { $capture += $StartAt + 6 * $i + 4 }
$job = Start-Job -ScriptBlock {
    param($root, $output, $run, $capture)
    $env:FD_TRACE_POINTER = '1'
    & "$root\tools\test_frame_generation.ps1" -Frames 0 -Dlaa 0 -Schedule '10:0010:150,40:0010:150,60:1000:150,65:1000:150,70:1000:150,230:0:30000:255:0,310:4000:150' -RunSeconds $run -CaptureAt $capture -OutputDirectory $output | Out-Null
} -ArgumentList $root, $output, $runSeconds, $capture
$start = Get-Date
foreach ($i in 0..($list.Count - 1)) {
    $at = $StartAt + 6 * $i
    while (((Get-Date) - $start).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 200 }
    $p = Get-Process FromDust -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$p) { continue }
    $r = New-Object FdMouse+RECT; [FdMouse]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $pt = New-Object FdMouse+POINT; $pt.X = [int]($r.R * $list[$i][0]); $pt.Y = [int]($r.B * $list[$i][1])
    [FdMouse]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
    # A small wiggle registers keyboard / mouse as the active device.
    [FdMouse]::SetCursorPos($pt.X + 8, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 100
    [FdMouse]::SetCursorPos($pt.X, $pt.Y) | Out-Null
}
Wait-Job $job | Out-Null; Receive-Job $job | Out-Null
Get-ChildItem "$output\*.png" | ForEach-Object Name
