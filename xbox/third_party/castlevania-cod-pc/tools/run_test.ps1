# Test run: starts the built exe, screenshots its window at the given seconds
# (copied from the screen, so D3D12 output is captured), then kills it and
# prints the tail of logs\castlevania.log. Shots land in tools\shots\<name>_t<s>.png.
param(
    [string]$Name = "run",
    [int[]]$At = @(10, 20, 30),
    [int]$Seconds = 0,
    [int[]]$F3At = @(),        # press F3 then: guest stacks into the log, a frame capture if XBE_CAPTURE_DIR is set
    [int[]]$F10At = @(),       # press F10 then: the settings menu opens or closes
    [int[]]$KeyAt = @(),       # press -Key then as a real key (keybd_event), the way a player does
    [int]$Key = 0x79,          # virtual key for -KeyAt, F10 by default
    [string[]]$Env = @()       # NAME=value pairs for this run
)
foreach ($kv in $Env) { $k, $v = $kv -split "=", 2; Set-Item "env:$k" $v }
$dir = Join-Path $PSScriptRoot "..\out\build\release" | Resolve-Path
$shots = Join-Path $PSScriptRoot "shots"
New-Item -ItemType Directory -Force $shots | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$p = Start-Process -FilePath "$dir\Castlevania.exe" -WorkingDirectory $dir -PassThru
$end = if ($Seconds -gt 0) { $Seconds } else { ($At | Measure-Object -Maximum).Maximum + 2 }
for ($t = 1; $t -le $end; $t++) {
    Start-Sleep 1
    if ($p.HasExited) { "exited after $t s, code $($p.ExitCode)"; break }
    if ($F3At -contains $t) {
        $p.Refresh()
        [void][W2]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr]0x72, [IntPtr]::Zero)  # WM_KEYDOWN VK_F3
        "t=$t F3"
    }
    if ($F10At -contains $t) {
        $p.Refresh()
        [void][W2]::PostMessage($p.MainWindowHandle, 0x104, [IntPtr]0x79, [IntPtr]::Zero)  # WM_SYSKEYDOWN VK_F10
        "t=$t F10"
    }
    if ($KeyAt -contains $t) {
        $p.Refresh(); [void][W2]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 200
        [W2]::keybd_event([byte]$Key, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
        [W2]::keybd_event([byte]$Key, 0, 2, [UIntPtr]::Zero)
        "t=$t key $Key"
    }
    if ($At -contains $t) {
        $p.Refresh(); $h = $p.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { "t=$t no window yet"; continue }
        [void][W2]::SetForegroundWindow($h)
        $r = New-Object W2+RECT; [void][W2]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $w, $ht
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
        # keep files small: scale to 960 wide
        $sw = [Math]::Min(960, $w); $sh = [int]($ht * $sw / $w)
        $small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
        $out = Join-Path $shots "$($Name)_t$t.png"; $small.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $small.Dispose(); $bmp.Dispose()
        "t=$t shot $out (${w}x${ht})"
    }
}
if (-not $p.HasExited) {
    # close politely first: a killed process loses the log lines still buffered
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force }
}
Get-Content "$dir\logs\castlevania.log" -Tail 25
