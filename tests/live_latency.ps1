# live.ps1 -- every motion in a REAL hand window, measured from DXGI frame statistics
# (MAYAG_PRESENTSTATS=1: present->glass, frames never shown, inter-present gaps).
# Scenarios: scroll (wheel through history), drag (window edge), pane (file-pane
# grip), type (keystrokes echoing). Each checks its action really happened.
param([string]$what = 'scroll', [string]$exe = 'C:\Develop\hand\build\hand.exe')
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$cfg = "C:\Develop\hand\build\live.vibe"
Set-Content $cfg ("files { open_at_start " + $(if ($what -eq 'pane') { 'true' } else { 'false' }) + " }")
$env:HAND_CONFIG = $cfg
$env:MAYAG_PRESENTSTATS = '1'
$p = Start-Process $exe -PassThru -RedirectStandardError C:\Develop\hand\build\live_err.txt -RedirectStandardOutput C:\Develop\hand\build\live_out.txt
Start-Sleep 3
$p.Refresh(); $h = $p.MainWindowHandle
if ($h -eq 0) { "no window"; $p.Kill(); exit 1 }
[void][W]::SetForegroundWindow($h)
# topmost: Windows' focus lock lets other apps stay in front of a scripted
# window, and an occluded window is composed and mostly never shown -- the
# numbers would be of nothing the user sees
[void][W]::SetWindowPos($h, [IntPtr](-1), 100, 100, 1300, 850, 0x0040)
Start-Sleep -Milliseconds 500
$dpi = [W]::GetDpiForWindow($h) / 96.0
# Start-Sleep -Milliseconds 16 really sleeps ~31 ms (the default timer tick):
# a 'smooth 60 Hz drag' was a 32 Hz one, and 30 fps the correct answer to it.
# A real mouse reports at 125-1000 Hz; pace events like one, on a stopwatch.
$clock = [Diagnostics.Stopwatch]::StartNew()
function pace([double]$ms) { $until = $clock.Elapsed.TotalMilliseconds + $ms; while ($clock.Elapsed.TotalMilliseconds -lt $until) { } }
function if2([int]$k, [int]$n) { if ($k % $n -lt $n / 2) { $k % $n } else { $n - $k % $n } }
function key([int]$vk) { [void][W]::PostMessageW($h, 0x100, [IntPtr]$vk, [IntPtr]1); [void][W]::PostMessageW($h, 0x101, [IntPtr]$vk, [IntPtr]([int64]0xC0000001 -band 0xFFFFFFFF)) }
function text([string]$s) { foreach ($ch in [char[]]$s) { [void][W]::PostMessageW($h, 0x102, [IntPtr][int]$ch, [IntPtr]1) } }
function at([double]$x, [double]$y) { [IntPtr](([int]($y * $dpi) -shl 16) -bor ([int]($x * $dpi) -band 0xFFFF)) }
function shot { $rc = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$rc)
  $b = New-Object Drawing.Bitmap ($rc.r - $rc.l), ($rc.b - $rc.t); $g = [Drawing.Graphics]::FromImage($b); $g.CopyFromScreen($rc.l, $rc.t, 0, 0, $b.Size); $g.Dispose(); $b }
function pxdiff($a, $b) { $n = 0; for ($y = 20; $y -lt [Math]::Min($a.Height, $b.Height) - 5; $y += 6) { for ($x = 5; $x -lt [Math]::Min($a.Width, $b.Width) - 5; $x += 6) {
  $c = $a.GetPixel($x, $y); $d = $b.GetPixel($x, $y); if ([Math]::Abs($c.R - $d.R) + [Math]::Abs($c.G - $d.G) + [Math]::Abs($c.B - $d.B) -gt 60) { $n++ } } }; $n }

# history to move through
text '1..600 | % { "line $_ " + ("x" * ($_ % 50)) }'; key 0x0D
Start-Sleep 6
$before = shot
$mark = (Get-Content C:\Develop\hand\build\live_err.txt -ErrorAction SilentlyContinue | Measure-Object).Count
switch ($what) {
  'scroll' { for ($k = 0; $k -lt 60; $k++) { $dir = if (($k % 20) -lt 10) { 120 } else { -120 }; [void][W]::PostMessageW($h, 0x20A, [IntPtr]($dir -shl 16), (at 700 400)); pace 100
               if ($k -eq 9) { Start-Sleep -Milliseconds 300; $mid = shot } }
             Start-Sleep -Milliseconds 300 }
  'drag'   { for ($k = 0; $k -lt 360; $k++) { $d = (if2 $k 120) / 2; [void][W]::SetWindowPos($h, [IntPtr]::Zero, 100, 100, 1300 - $d * 12, 850 - $d * 6, 0x14); pace 8 }
             # after the drag: motion must be back to full speed (the last stats window)
             Start-Sleep -Milliseconds 400
             for ($k = 0; $k -lt 40; $k++) { $dir = if (($k % 20) -lt 10) { 120 } else { -120 }; [void][W]::PostMessageW($h, 0x20A, [IntPtr]($dir -shl 16), (at 700 400)); pace 100 } }
  'edge'   { # a REAL edge drag: Windows' own size/move loop, as a user's mouse drives it
             Add-Type -Namespace E -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y); [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, System.UIntPtr e);'
             $rc = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$rc)
             $ex = $rc.r - 3; $ey = [int](($rc.t + $rc.b) / 2)
             [void][E.U]::SetCursorPos($ex, $ey); Start-Sleep -Milliseconds 200
             [E.U]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # left down on the border
             for ($k = 0; $k -lt 360; $k++) { $d = (if2 $k 120) * 4; [void][E.U]::SetCursorPos($ex - $d, $ey); pace 8
               if ($k -eq 50) { $mid = shot } }
             [E.U]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) }
  'sbar'   { $sx = 1300 / $dpi - 14; $sy = 120
             [void][W]::PostMessageW($h, 0x200, [IntPtr]0, (at $sx $sy)); Start-Sleep -Milliseconds 300
             [void][W]::PostMessageW($h, 0x201, [IntPtr]1, (at $sx $sy)); Start-Sleep -Milliseconds 30
             for ($k = 0; $k -lt 360; $k++) { $d = (if2 $k 180) / 2
               [void][W]::PostMessageW($h, 0x200, [IntPtr]1, (at $sx ($sy + $d * 8))); pace 8
               if ($k -eq 40) { $mid = shot } }
             [void][W]::PostMessageW($h, 0x202, [IntPtr]0, (at $sx $sy)) }
  'pane'   { $gx = 297.5; $gy = 300
             [void][W]::PostMessageW($h, 0x200, [IntPtr]0, (at $gx $gy)); Start-Sleep -Milliseconds 50
             [void][W]::PostMessageW($h, 0x201, [IntPtr]1, (at $gx $gy)); Start-Sleep -Milliseconds 30
             for ($k = 0; $k -lt 360; $k++) { $d = (if2 $k 120) / 2
               [void][W]::PostMessageW($h, 0x200, [IntPtr]1, (at ($gx + $d * 5) $gy)); pace 8
               if ($k -eq 25) { $mid = shot } }
             [void][W]::PostMessageW($h, 0x202, [IntPtr]0, (at $gx $gy)) }
  'type'   { foreach ($ch in [char[]]'echo the quick brown fox jumps over the lazy dog 0123456789') { text ([string]$ch); Start-Sleep -Milliseconds 60 }
             $mid = shot; for ($k = 0; $k -lt 60; $k++) { key 0x08; Start-Sleep -Milliseconds 30 } }
}
Start-Sleep -Milliseconds 800
$after = if ($mid) { $mid } else { shot }
$changed = pxdiff $before $after
$p.Kill(); Start-Sleep -Milliseconds 300
$lines = Get-Content C:\Develop\hand\build\live_err.txt | Select-Object -Skip $mark
$v = $lines | ? { $_ -like '*present->screen*' } | % {
  if ($_ -match 'p50 ([\d.]+)  p90 ([\d.]+)  max ([\d.]+).*?\((\d+) shown, (\d+) never.*?gaps p50 ([\d.]+) p90 ([\d.]+) max ([\d.]+)') {
    'glass p50 {0} p90 {1} max {2} ms, {3} shown {4} lost, gap p90 {6} max {7}' -f $matches[1], $matches[2], $matches[3], $matches[4], $matches[5], $matches[6], $matches[7], $matches[8] } }
$mode = ($lines | ? { $_ -like '*mode *' } | % { ($_ -split 'mode ')[1] } | select -Unique) -join ','
$jud = $lines | ? { $_ -like '*judder*' } | select -Last 1 | % { ($_ -split 'judder ')[1] }
"{0,-6} judder: {1}" -f $what, $jud
"{0,-6} [{1}] screen changed {2} px-samples | {3}" -f $what, $mode, $changed, ($v -join ' | ')
if ($changed -lt 20) { "INVALID: the window did not visibly change (occluded?)"; exit 2 }
