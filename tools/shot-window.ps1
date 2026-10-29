param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Out)
# Screenshot the main window of one specific process (PrintWindow, so it works when the window is covered).
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinShot {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[WinShot]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if ($p) { $p.Refresh() }
if (-not $p -or $p.MainWindowHandle -eq 0) { Write-Output "no window for process $ProcessId"; exit 1 }
$r = New-Object WinShot+RECT
[WinShot]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WinShot]::PrintWindow($p.MainWindowHandle, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Write-Output "$($p.MainWindowTitle) ${w}x${h} -> $Out"
