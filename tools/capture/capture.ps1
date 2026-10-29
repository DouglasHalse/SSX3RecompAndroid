<#
    Record a ride: the game's own mix plus markers for what happened.

    Start the port with the audio tap on, ride, and press a marker key when
    something happens. What comes out is mix.wav (what the game played) and
    markers.json (when you said it happened), which analyse.py pairs up.

        tools\capture\capture.ps1 -Seconds 180
        python tools\capture\analyse.py capture\<stamp>

    Marker keys (they work while the game has focus; the game uses F3 and F11):
        F5  zone    the rider entered a different part of the course
        F6  jump    the rider left the ground
        F7  land    the rider landed
        F8  bail    the rider crashed
        F9  trick   a trick landed
        F4  finish  the run ended

    Press the key as the thing happens, not after thinking about it: the
    analysis measures the change that follows the mark, so a late mark shows up
    as a late latency.
#>
param(
    [int]$Seconds = 180,
    [string]$Out,
    [string]$Exe = 'build\ssx3_recomp.exe',
    [switch]$MusicCapture,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

if (-not $Out) { $Out = Join-Path $root ('capture\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$wav     = Join-Path $Out 'mix.wav'
$jsonl   = Join-Path $Out 'music.jsonl'
$markers = Join-Path $Out 'markers.json'
$errlog  = Join-Path $Out 'stderr.txt'
$outlog  = Join-Path $Out 'stdout.txt'
$manifest = Join-Path $Out 'capture.json'

$keys = [ordered]@{
    0x74 = 'zone'
    0x75 = 'jump'
    0x76 = 'land'
    0x77 = 'bail'
    0x78 = 'trick'
    0x73 = 'finish'
}

$env:RECOMP_AUDIO_WAV = $wav
$env:RECOMP_KEYBOARD = '1'
if ($MusicCapture) { $env:RECOMP_MUSIC_CAPTURE = $jsonl }

if ($DryRun) {
    Write-Host "exe        $Exe"
    Write-Host "out        $Out"
    Write-Host "mix        $wav"
    Write-Host "markers    $markers"
    Write-Host "music      $(if ($MusicCapture) { $jsonl } else { '(not built into this binary)' })"
    Write-Host "seconds    $Seconds"
    Write-Host ("keys       " + (($keys.GetEnumerator() | ForEach-Object { ('0x{0:X2}={1}' -f $_.Key, $_.Value) }) -join ' '))
    return
}

if (-not (Test-Path $Exe)) { throw "no $Exe - run tools\build.bat first" }

$sig = @'
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
'@
$K = Add-Type -MemberDefinition $sig -Name KeyPoll -Namespace Win32 -PassThru

$started = Get-Date
$p = Start-Process -FilePath $Exe -WorkingDirectory $root -PassThru `
        -RedirectStandardError $errlog -RedirectStandardOutput $outlog

Write-Host "recording for up to $Seconds s into $Out"
Write-Host "F5 zone   F6 jump   F7 land   F8 bail   F9 trick   F4 finish"

$sw = [Diagnostics.Stopwatch]::StartNew()
$prev = @{}
foreach ($vk in $keys.Keys) { $prev[$vk] = $false }
$list = New-Object System.Collections.ArrayList

while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Seconds) {
    foreach ($vk in @($keys.Keys)) {
        $down = (($K::GetAsyncKeyState($vk) -band 0x8000) -ne 0)
        if ($down -and -not $prev[$vk]) {
            $t = [math]::Round($sw.Elapsed.TotalSeconds, 3)
            $null = $list.Add([pscustomobject]@{ t = $t; label = $keys[$vk] })
            Write-Host ("{0,7:N2}s  {1}" -f $t, $keys[$vk])
        }
        $prev[$vk] = $down
    }
    Start-Sleep -Milliseconds 10
}

if (-not $p.HasExited) {
    $p.Kill()
    [void]$p.WaitForExit(5000)
}

$json = '[' + (($list | ForEach-Object { '{"t":' + $_.t + ',"label":"' + $_.label + '"}' }) -join ',') + ']'
Set-Content -Path $markers -Value $json -Encoding UTF8

$cap = [pscustomobject]@{
    started   = $started.ToString('s')
    seconds   = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    exe       = $Exe
    wav       = $wav
    music     = if ($MusicCapture) { $jsonl } else { $null }
    markers   = $list.Count
    stderr    = $errlog
}
$cap | ConvertTo-Json | Set-Content -Path $manifest -Encoding UTF8

Write-Host ""
Write-Host ("$($list.Count) markers, {0:N1} s of audio" -f $sw.Elapsed.TotalSeconds)
Write-Host "next: python tools\capture\analyse.py `"$Out`""
