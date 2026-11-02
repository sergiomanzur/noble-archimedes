# Like run_test.ps1, but saves the window at full size: for comparing detail (texture packs, render scale).
param([string]$Name = "full", [int[]]$At = @(80), [string[]]$Env = @())
foreach ($kv in $Env) { $k, $v = $kv -split "=", 2; Set-Item "env:$k" $v }
$dir = Join-Path $PSScriptRoot "..\out\build\release" | Resolve-Path
$shots = Join-Path $PSScriptRoot "shots"
New-Item -ItemType Directory -Force $shots | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W11 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$p = Start-Process -FilePath "$dir\Castlevania.exe" -WorkingDirectory $dir -PassThru
foreach ($t in $At) {
    while ($p.StartTime.AddSeconds($t) -gt (Get-Date)) { Start-Sleep -Milliseconds 200 }
    $p.Refresh(); $h = $p.MainWindowHandle; [void][W11]::SetForegroundWindow($h); Start-Sleep -Milliseconds 300
    $r = New-Object W11+RECT; [void][W11]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
    $out = Join-Path $shots "$($Name)_t$t.png"; $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "t=$t shot $out"
}
if (-not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force } }
