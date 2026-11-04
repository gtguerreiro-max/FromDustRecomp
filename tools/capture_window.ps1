# Captures the game window (including the F1 overlay) after a delay.
# Usage: tools/capture_window.ps1 -DelaySeconds 300 -Output build\shot.png [-ProcessName FromDust]
param([int]$DelaySeconds = 60, [string]$Output = 'window.png', [string]$ProcessName = 'FromDust')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FdWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
}
'@
Start-Sleep -Seconds $DelaySeconds
$process = Get-Process -Name $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$hwnd = $process.MainWindowHandle
$rect = New-Object FdWindow+RECT
[FdWindow]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
# PW_RENDERFULLCONTENT (2) captures DirectX / flip-model content.
$hdc = $graphics.GetHdc()
$ok = [FdWindow]::PrintWindow($hwnd, $hdc, 2)
$graphics.ReleaseHdc($hdc)
if (-not $ok) { $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size) }
$bitmap.Save((Resolve-Path -LiteralPath (Split-Path $Output -Parent)).Path + '\' + (Split-Path $Output -Leaf), [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()
"captured $width x $height -> $Output"
