# Interactive session: start the game detached with a live pad file, take
# screenshots of the running window, press buttons, stop it.
#   powershell -File tools\live.ps1 start               launch; pad file = pad_live.txt
#   powershell -Command "& .\tools\live.ps1 start -Env @{RECOMP_WATCH='0x3A4190'}"
#       -File passes arguments as strings, so a hashtable only binds through
#       -Command; with -File the -Env switch fails on every launch.
#   powershell -File tools\live.ps1 snap  [-Out snap.png] screenshot the running window
#   powershell -File tools\live.ps1 press -Buttons "a"    append "a" (any RECOMP_PAD_SCRIPT button syntax)
#   powershell -File tools\live.ps1 key -Key 0x0D [-Hold 200]  post a key to the window (RECOMP_KEYBOARD=1 path)
#   powershell -File tools\live.ps1 dump [-Mem "va:len,..."] all-thread stack dump (+ guest memory) into stderr.txt
#   powershell -File tools\live.ps1 stop
# stdout/stderr go to stdout.txt / stderr.txt in the project root as usual.
# -Dir <dir>: run as a separate instance in <dir> (created on start, with a
# junction game -> <project>\game): its own pad file, pid, logs, triggers and
# game_state.txt, so several sessions can run at once (tools\race_par.ps1).
# -Save <name>: the game's saves (UDATA/TDATA) for this run are a fresh copy
# of the fixture work\saves\<name> in <dir>\save (RECOMP_SAVE_DIR), so
# scripted runs start from a known profile and never write the player's
# saves in game\UDATA. Default "unlocked" (everything unlocked by cheat);
# "none" = empty (first boot), "user" = the player's own game\UDATA.
param([Parameter(Position = 0)][string]$Cmd = "snap", [string]$Out = "snap.png",
      [string]$Buttons = "", [string]$Mem = "", [hashtable]$Env = @{},
      [string]$Key = "0x0D", [int]$Hold = 200, [string]$Dir = "", [string]$Save = "unlocked")

$root = Split-Path -Parent $PSScriptRoot
$work = if ($Dir) { $Dir } else { $root }
$pad = Join-Path $work "pad_live.txt"
$pidFile = Join-Path $work "live.pid"

function Get-Game {
    if (-not (Test-Path $pidFile)) { return $null }
    $id = [int](Get-Content $pidFile)
    Get-Process -Id $id -ErrorAction SilentlyContinue
}

switch ($Cmd) {
    "start" {
        $g = Get-Game; if ($g) { Stop-Process -Id $g.Id -Force; Start-Sleep -Milliseconds 500 }
        if ($work -ne $root) {
            New-Item -ItemType Directory -Force -Path $work | Out-Null
            if (-not (Test-Path (Join-Path $work "game"))) {
                New-Item -ItemType Junction -Path (Join-Path $work "game") -Target (Join-Path $root "game") | Out-Null
            }
        }
        . "$PSScriptRoot\archive_logs.ps1"; Save-PreviousLogs $work
        Set-Content -Path $pad -Value $null
        # The last run's game_state.txt would read as this one's until the game
        # writes its own (a script then acts on a screen that is not up yet).
        Remove-Item -Path (Join-Path $work "game_state.txt") -ErrorAction SilentlyContinue
        $env:RECOMP_PAD_LIVE = $pad
        # Scripted runs ignore the machine's real controllers (someone playing
        # or holding a pad must not steer them); -Env @{RECOMP_NO_HOST_PAD='0'}
        # turns the real pad back on. tools\play.bat does not use this script.
        $env:RECOMP_NO_HOST_PAD = "1"
        # ...and are silent (RECOMP_MUTE='0' in -Env for sound).
        $env:RECOMP_MUTE = "1"
        # ...and log everything, as before the log levels (docs/LOGGING.md):
        # the test scripts read [HUD], [GLITCH], [UI] ... lines.
        # -Env @{RECOMP_LOG_LEVEL='info'} for a player's log.
        $env:RECOMP_LOG_LEVEL = "trace"
        # The emulated hard disk's partition images (and cache) per run
        # directory: instances booting together collided on one shared
        # Partition5.img and the loser quit to the dashboard.
        $env:RECOMP_SYSTEM_DIR = Join-Path $work "system"
        # Native 640x480 rendering for scripted runs (fast, small screenshots);
        # -Env @{RECOMP_RES='1080'} for the full-HD path players get.
        $env:RECOMP_RES = "1"
        $env:RECOMP_WIDESCREEN = "0"
        if ($Save -ne "user") {
            $sd = Join-Path $work "save"
            Remove-Item -Recurse -Force $sd -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force -Path $sd | Out-Null
            if ($Save -ne "none") {
                $fx = Join-Path $root "work\saves\$Save"
                if (Test-Path $fx) { Copy-Item -Recurse -Path "$fx\*" -Destination $sd }
                else { Write-Warning "save fixture $fx not found: starting with no save" }
            }
            $env:RECOMP_SAVE_DIR = $sd
        } else { Remove-Item env:RECOMP_SAVE_DIR -ErrorAction SilentlyContinue }
        $env:RECOMP_THREAD_DUMP_TRIGGER = (Join-Path $work "dump.trigger")
        $env:RECOMP_SNAP_TRIGGER = (Join-Path $work "snap.trigger")
        foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }
        $p = Start-Process -FilePath "$root\build\ssx3_recomp.exe" -WorkingDirectory $work `
                -RedirectStandardOutput "$work\stdout.txt" -RedirectStandardError "$work\stderr.txt" -PassThru
        Set-Content -Path $pidFile -Value $p.Id
        "started pid $($p.Id)"
    }
    "press" {
        Add-Content -Path $pad -Value $Buttons
        "pressed $Buttons"
    }
    "snap" {
        $g = Get-Game
        if (-not $g) { "not running"; exit 1 }
        Add-Type -AssemblyName System.Drawing
        Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class LiveWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
        $g.Refresh()
        $h = $g.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { "no window yet"; exit 1 }
        # From inside the game (RECOMP_SNAP_TRIGGER): the frame being shown,
        # whatever presents it -- PrintWindow cannot read a D3D11 swap chain.
        $bmpPath = Join-Path $work "snap_tmp.bmp"
        Remove-Item $bmpPath -ErrorAction SilentlyContinue
        Set-Content -Path (Join-Path $work "snap.trigger") -Value $bmpPath -NoNewline
        $t0 = Get-Date
        while (-not (Test-Path $bmpPath) -and ((Get-Date) - $t0).TotalSeconds -lt 3) { Start-Sleep -Milliseconds 100 }
        $saved = $false
        if (Test-Path $bmpPath) {
            Start-Sleep -Milliseconds 100
            try {
                $img = [System.Drawing.Image]::FromFile($bmpPath)
                $img.Save((Join-Path $work $Out), [System.Drawing.Imaging.ImageFormat]::Png)
                $img.Dispose(); $saved = $true
            } catch { }
            Remove-Item $bmpPath -ErrorAction SilentlyContinue
        }
        if (-not $saved) {
            Remove-Item (Join-Path $work "snap.trigger") -ErrorAction SilentlyContinue
            $r = New-Object LiveWin+RECT
            [void][LiveWin]::GetWindowRect($h, [ref]$r)
            $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
            $gr = [System.Drawing.Graphics]::FromImage($bmp)
            $hdc = $gr.GetHdc(); [void][LiveWin]::PrintWindow($h, $hdc, 2); $gr.ReleaseHdc($hdc)
            $bmp.Save((Join-Path $work $Out), [System.Drawing.Imaging.ImageFormat]::Png)
        }
        "'{0}' -> {1}" -f $g.MainWindowTitle, $Out
    }
    "key" {
        $g = Get-Game
        if (-not $g) { "not running"; exit 1 }
        Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class LiveKey {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
        $g.Refresh()
        $h = $g.MainWindowHandle
        $vk = [int]$Key
        [void][LiveKey]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr]1)
        Start-Sleep -Milliseconds $Hold
        [void][LiveKey]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]0xC0000001)
        "key $Key"
    }
    "dump" {
        Set-Content -Path (Join-Path $work "dump.trigger") -Value $Mem -NoNewline
        Start-Sleep -Seconds 2
        "dump requested"
    }
    "stop" {
        $g = Get-Game; if ($g) { Stop-Process -Id $g.Id -Force; "stopped" } else { "not running" }
        Remove-Item $pidFile -ErrorAction SilentlyContinue
    }
    "close" {
        # as a player ends it: close the window (the game logs its session
        # summary), killed only if it is still there after 10 s
        $g = Get-Game
        if (-not $g) { "not running"; break }
        [void]$g.CloseMainWindow()
        if ($g.WaitForExit(10000)) { "closed" } else { Stop-Process -Id $g.Id -Force; "close timed out: stopped" }
        Remove-Item $pidFile -ErrorAction SilentlyContinue
    }
}
