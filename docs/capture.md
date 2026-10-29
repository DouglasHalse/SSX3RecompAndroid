# Capturing the soundtrack's signals

The creator needs one fact it does not have: which moments the game tells the music about, and how
soon the music answers. Five moments are known from the engine code — start (event 0), the value
ramp, impacts as value resets, course triggers (`N` / `N+1`) and the finish (event 10). Airborne and
big air are not measured at all. This tool measures behaviour from the outside first, then from the
inside.

## Layer 1 — the mix, and a person marking moments

Needs no reverse engineering and no regen.

- `RECOMP_AUDIO_WAV=<path>` (toolkit) writes exactly what the port sends to the audio device:
  48 kHz, 16-bit stereo, post-mix. The recording is the game's own soundtrack, not a microphone.
- `tools\capture\capture.ps1 -Seconds 180` starts the build with that on and listens for marker keys
  while you ride: **F5** zone, **F6** jump, **F7** land, **F8** bail, **F9** trick, **F4** finish.
  Each press appends `{t, label}` to `markers.json`.
- `python tools\capture\analyse.py capture\<stamp>` computes envelope, high-frequency content and
  zero-crossing features on 20 ms frames, a change curve (each window against the window 1.2 s
  earlier), the bar period from onset autocorrelation, and for every mark the change that follows
  within ±2 s: latency, strength against the local floor. It prints a per-moment table and writes
  `analysis.json`.
- `python tools\capture\analyse.py --selftest` proves the analyser on a synthetic mix: a texture
  change at 6.0 s with a mark at 5.9 s is reported as +0.10 s.

What layer 1 can conclude: *"the music changes after a jump, typically 120 ms later, in five of six
marks"*, or *"nothing changes after a bail"*. That is enough to know whether a moment exists as a
signal at all.

What it cannot conclude: which event number fired, or whether the change came from an event or from
a value reset.

## The ride protocol

One marker means nothing — the music changes constantly at bar boundaries. So, five runs, same
course, same song, three minutes each:

| run | what you do | what it establishes |
|---|---|---|
| A, flow | ride for 60 s, mark nothing | the bar period and the natural change rate — the floor every other run is compared against |
| B, jumps | 8-10 marked jumps off the *same* kicker, nothing else | whether leaving the ground moves the music, and how consistently |
| C, falls | 6-8 deliberate crashes; `BACK` (reset boarder) is not a crash | whether a bail moves the music |
| D, landings | 6-8 marks as a trick completes and the board touches down | whether a landing moves the music, as opposed to the jump |
| E, finish | ride to the line and mark it; mark a mid-run section change if you can feel one | whether the finish and any trigger are audible |

Marking rules: press the key at the instant, never afterwards to "correct" it — the analysis measures
the change *after* the mark, so a late mark reads as a late latency. Write anything unusual into
`notes.txt` in the capture folder; a human note explains an outlier the numbers cannot.

Reading the result: a moment whose median latency is under ~200 ms and which reacted in most of its
marks is a real signal and belongs in the creator as a cue slot. A moment that reacted in none of
them either has no event or the song ignores it — and then it must not appear in the tool at all.

## Layer 2 — the engine's own signals

`src/ssx3_music_capture.c` writes one JSONL line per call through the four functions the soundtrack
passes:

| guest | what it is |
|---|---|
| `sub_0018A6E0` | send a numbered event |
| `sub_0018A8C0` | write the control value |
| `sub_00111F40` | reset it, with a 0-1 strength |
| `sub_0010F370` | map a course trigger message to an event number |

Each line carries the argument candidates that were readable at the call — `ecx`, `edx`, `[esp+4]`,
`[esp+8]` — because the game's calling convention for these is not documented. The reader picks the
slot that behaves like an event number; `tools\capture\analyse.py` and a quick `jq` pass make that
obvious after one run.

It is compiled in but inert: the wrappers need each function's generated body lifted to a `_gen`
name first, which is what `scripts/regen.sh` does when it sees a hand-written `void sub_XXXXXXXX(void)`
(CLAUDE.md, "Wrapping a function with many direct callers"). So:

1. build with `-DRECOMP_MUSIC_CAPTURE_HOOKS`, run `scripts/regen.sh`, rebuild;
2. run with `RECOMP_MUSIC_CAPTURE=<path>`;
3. layer 1's *"something changed"* becomes *"event 7 fired 40 ms after the mark, three times in a
   row"*.

## Android

Both knobs go in `recomp.cfg` beside the app's files, which the port reads before anything else:

```
RECOMP_AUDIO_WAV=/sdcard/Android/data/com.douglashalse.ssx3/files/ssx3/mix.wav
RECOMP_MUSIC_CAPTURE=/sdcard/Android/data/com.douglashalse.ssx3/files/ssx3/music.jsonl
```

There are no marker keys on a phone, so the markers come from the ride script's own timestamps —
`adb shell input swipe` taps at known times are accurate to a few tens of milliseconds, which is
inside what layer 1 can measure.

## Limits, stated plainly

- A person's mark is 150-250 ms late on average, so layer-1 latencies are upper bounds. Good enough
  for "does it react"; use layer 2 for exact wiring.
- The mix is post-volume: turning the game down quietens the WAV. The features are relative, so the
  analysis still works, but keep the volume steady within a run.
- A killed process can leave the WAV header short; the reader trusts the header and ignores the tail
  beyond it.
