# Ride capture

Records what the game plays while a person rides and marks the moments that matter, then pairs the
two up. The reasoning, the ride protocol and the engine-hook layer are in
[`../../docs/capture.md`](../../docs/capture.md).

```powershell
tools\capture\capture.ps1 -Seconds 180       # ride; F6 jump, F8 bail, F9 trick, F7 land, F5 zone, F4 finish
python tools\capture\analyse.py capture\<stamp>
python tools\capture\analyse.py --selftest   # proves the analyser on a synthetic mix
```

`capture\<stamp>\` holds:

| file | what |
|---|---|
| `mix.wav` | what the port sent to the audio device — the soundtrack itself, 48 kHz stereo |
| `markers.json` | `{t, label}` per key press, seconds since the game started |
| `music.jsonl` | engine signals, only when the binary was built with the capture hooks |
| `capture.json` | what was recorded, for how long, from which build |
| `stderr.txt` | the port's own log |
| `analysis.json` | the analyser's table: per moment, marks, median latency, how many reacted |

`capture.ps1 -DryRun` prints the paths and the key map without starting anything.
