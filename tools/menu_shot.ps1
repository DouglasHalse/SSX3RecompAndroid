# Drive the in-game options screen and capture it: boot, open it, walk to a
# row, change the value, close it.
#   powershell -File tools\menu_shot.ps1 [-After 20] [-Prefix menu] [-Env @{...}]
# Writes <Prefix>_0..4.png in the project root and stops the game.
param([int]$After = 20, [string]$Prefix = "menu", [hashtable]$Env = @{})

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$p = Start-Process -FilePath "$root\build\ssx3_recomp.exe" -WorkingDirectory $root `
        -RedirectStandardOutput "$root\stdout.txt" -RedirectStandardError "$root\stderr.txt" -PassThru
$null = $p.Handle
Start-Sleep -Seconds $After
$p.Refresh()
if ($p.HasExited) { "exited early with code 0x{0:X8}" -f $p.ExitCode; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { "no window"; Stop-Process -Id $p.Id -Force; exit 1 }
[void][Win]::SetForegroundWindow($h)

function Shot([string]$name) {
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][Win]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)
    $bmp.Save("$root\$name", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "-> $name"
}

function Key([int]$vk) {
    [void][Win]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr]0)   # WM_KEYDOWN
    Start-Sleep -Milliseconds 60
    [void][Win]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]0)   # WM_KEYUP
    Start-Sleep -Milliseconds 120
}

Shot "$($Prefix)_0.png"                       # the game, no screen
Key 0x79                                      # F10: open
Start-Sleep -Milliseconds 400
Shot "$($Prefix)_1.png"
for ($i = 0; $i -lt 10; $i++) { Key 0x28 }    # down to FRAME COUNTER
Start-Sleep -Milliseconds 200
Shot "$($Prefix)_2.png"
Key 0x27                                      # right: on
Start-Sleep -Milliseconds 300
Shot "$($Prefix)_3.png"
Key 0x79                                      # F10: close
Start-Sleep -Milliseconds 1500
Shot "$($Prefix)_4.png"                       # the counter it just turned on
Stop-Process -Id $p.Id -Force
