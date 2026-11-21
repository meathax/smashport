# Development helper: launch SmashTV.exe, optionally send keys, and save the
# window contents (PrintWindow) as PNG. Used to check the PC overlay and video.
#   tools/host_capture.ps1 -Exe build/gsp/SmashTV.exe -Out shot.png [-Wait 6] [-Keys '{ESC}']
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Out,
    [double]$Wait = 6,
    [string]$Keys = ''
)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HostWin {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
$p = Start-Process -FilePath $Exe -PassThru
try {
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh(); $h = $p.MainWindowHandle }
    if ($h -eq [IntPtr]::Zero) { throw 'no window' }
    [HostWin]::SetForegroundWindow($h) | Out-Null   # focus loss pauses the game
    Start-Sleep -Seconds $Wait
    if ($Keys) {
        [HostWin]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 300
        [System.Windows.Forms.SendKeys]::SendWait($Keys)
        Start-Sleep -Milliseconds 700
    }
    $r = New-Object HostWin+RECT
    [HostWin]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [HostWin]::PrintWindow($h, $dc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $bmp.Save([IO.Path]::GetFullPath($Out), [System.Drawing.Imaging.ImageFormat]::Png)
    "saved $Out ($($r.R)x$($r.B))"
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
