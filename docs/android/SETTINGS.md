# Options, in the game

Every setting is a screen inside the game: while playing, the **OPTIONS** button
at the top of the screen opens it (F10, or a pad's Back, on a desktop). The list
is the one the launcher used to hold — the launcher is now only *Select game
image* and *Play*.

The screen writes `ssx3.cfg` next to `game/`, and the game reads it before it
reads anything else:

| App | File |
| --- | --- |
| `com.douglashalse.ssx3` (port) | `/sdcard/Android/data/com.douglashalse.ssx3/files/ssx3/ssx3.cfg` |
| `com.douglashalse.ssx3.bundled` | `/sdcard/Android/data/com.douglashalse.ssx3.bundled/files/ssx3/ssx3.cfg` |

A line there beats the launcher and the environment, so what the screen says is
what the game does. Only the rows actually changed in game are written, so a
value nobody touched still follows the platform default (or an `adb shell`
one-off).

## Rows

| Row | Values | Variable |
| --- | --- | --- |
| Resolution | Auto / 480p / 720p / 1080p / 1440p / 2160p | `RECOMP_RES` |
| Screen aspect | Auto / 16:9 / 21:9 / 32:9 / 4:3 | `RECOMP_WIDESCREEN` |
| Antialiasing | Off / 2x / 4x / 8x | `RECOMP_MSAA` |
| Anisotropic filtering | Off / 4x / 8x / 16x | `RECOMP_ANISO` |
| Draw distance | Original / Far / Maximum | `RECOMP_DRAW_DIST` |
| Shadow quality | 128 / 512 / 1024 | `RECOMP_SHADOW_RES` |
| Textures | Original / HD pack | `RECOMP_HDTEX` |
| CRT filter | Off / On | `RECOMP_CRT` |
| Motion blur | Off / On | `RECOMP_MBLUR` |
| HUD at screen edges | Off / On | `RECOMP_HUD_EDGES` |
| Frame counter | Off / On | `RECOMP_FPS_OVERLAY` |
| Audio | Stereo / 5.1 | `RECOMP_AUDIO_CHANNELS` |
| Skip intro | Off / On | `RECOMP_SKIP_INTRO` |
| Fast loading | Off / On | `RECOMP_FAST_BOOT` |
| Debug menu | Off / On | `RECOMP_DEBUG_MENU` |
| Money cheat | Off / On | `RECOMP_MONEY_CHEAT` |
| Log detail | Errors … Everything | `RECOMP_LOG_LEVEL` |

Each row writes the variable in its own grammar: `RECOMP_HDTEX` takes a
directory name (`hd-textures`, or `0` for off), `RECOMP_MBLUR` a strength
(`0.5`), `RECOMP_FAST_BOOT` the multiplier the front end's fixed pauses run at
(`4`).

## What applies when

Draw distance, HUD placement and the frame counter take effect as the value
changes: their readers look at the variable again on the next frame.

The rest are read while the renderer or the title starts, so changing one marks
the row with a small square and shows *restart needed* under the list. They take
effect the next time the game starts.

## Render height

The resolution row is offered like on a desktop; *Auto* is the display's short
side, taken from `WindowManager.getMaximumWindowMetrics()` (the activity's own
metrics exclude the system bars and render smaller for no reason).

## Older files

`recomp.cfg`, written by the launcher's old Settings screen, is still read
first, and anything in `ssx3.cfg` overrides it. Delete either file to get the
defaults back.
