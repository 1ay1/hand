# edge_sync.ps1 -- does the CONTENT follow the window edge during a real edge drag?
# Drags the right border through Windows' size loop (a real user drag) and, at
# every step, screen-grabs a strip across the right edge: the window frame is
# where the window is, the content is where hand's last presented frame ends.
# A frame that lags shows as a band of stale/empty pixels (or content cut off)
# at the edge. Reports how many grabbed steps were out of step.
param([string]$exe = 'C:\Develop\hand\build\hand.exe', [int]$steps = 120, [string]$side = 'right')
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class G {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
}
"@
$cfg = "C:\Develop\hand\build\live.vibe"; Set-Content $cfg "files { open_at_start false }"; $env:HAND_CONFIG = $cfg
$p = Start-Process $exe -PassThru; Start-Sleep 3; $p.Refresh(); $h = $p.MainWindowHandle
[void][G]::SetWindowPos($h, [IntPtr](-1), 100, 100, 1300, 850, 0x0040); Start-Sleep -Milliseconds 800
$clock = [Diagnostics.Stopwatch]::StartNew()
function pace([double]$ms) { $u = $clock.Elapsed.TotalMilliseconds + $ms; while ($clock.Elapsed.TotalMilliseconds -lt $u) {} }
$rc = New-Object G+RECT; [void][G]::GetWindowRect($h, [ref]$rc)
$ex = if ($side -eq 'left') { $rc.l + 3 } else { $rc.r - 3 }; $ey = [int](($rc.t + $rc.b) / 2)
# the background colour at rest, just inside the right edge
$probe = { param($x, $y) $b = New-Object Drawing.Bitmap 1, 1; $g = [Drawing.Graphics]::FromImage($b); $g.CopyFromScreen($x, $y, 0, 0, $b.Size); $c = $b.GetPixel(0, 0); $g.Dispose(); $b.Dispose(); $c }
$bg = & $probe ($(if ($side -eq 'left') { $rc.l + 12 } else { $rc.r - 12 })) ($ey)
[void][G]::SetCursorPos($ex, $ey); Start-Sleep -Milliseconds 200
[G]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
$bad = 0; $n = 0
for ($k = 0; $k -lt $steps; $k++) {
  $d = if ($k % 60 -lt 30) { $k % 60 } else { 60 - $k % 60 }
  [void][G]::SetCursorPos($(if ($side -eq 'left') { $ex + $d * 8 } else { $ex - $d * 8 }), $ey); pace 16
  if ($k % 3 -eq 2) {
    # where Windows says the client area ends now, and what is painted just inside it
    $cr = New-Object G+RECT; [void][G]::GetClientRect($h, [ref]$cr)
    $pt = New-Object G+POINT; $pt.x = $(if ($side -eq 'left') { 0 } else { $cr.r }); $pt.y = [int]($cr.b / 2); [void][G]::ClientToScreen($h, [ref]$pt)
    $c = & $probe ($(if ($side -eq 'left') { $pt.x + 6 } else { $pt.x - 6 })) ($pt.y)
    $n++
    if ([Math]::Abs($c.R - $bg.R) + [Math]::Abs($c.G - $bg.G) + [Math]::Abs($c.B - $bg.B) -gt 40) { $bad++ }
  }
}
[G]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
$p.Kill()
"edge in step: {0} of {1} sampled steps showed something other than hand's background just inside the moving edge" -f $bad, $n
