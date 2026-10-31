# Settings

The launcher has a **Settings** screen: one row per knob, each opening a list of
values. Picking a value writes `<root>/recomp.cfg`, which `src/android/sdl_main.c`
loads before anything else starts, so a change applies from the next Play and
needs no rebuild.

The file lives next to `game/`:

| App | Root |
| --- | --- |
| `com.douglashalse.ssx3` (port) | `/sdcard/Android/data/com.douglashalse.ssx3/files/ssx3/` |
| `com.douglashalse.ssx3.bundled` | `/sdcard/Android/data/com.douglashalse.ssx3.bundled/files/ssx3/` |

The launcher owns that file: it is rewritten on every Play and after every
change on the settings screen. A real environment variable of the same name
still wins (`setenv(..., 0)` in `load_env_file`), which is how an `adb shell`
experiment overrides a saved choice.

## Rows

| Group | Row | Values | Variable | Default |
| --- | --- | --- | --- | --- |
| Display | Aspect ratio | Fill screen / 4:3 (original) | `RECOMP_WIDESCREEN` | `auto` |
| Display | Modern camera | Off / On | `RECOMP_CAM` | off |
| Display | FPS counter | Off / On | `RECOMP_FPS_OVERLAY` | off |
| Graphics | Anti-aliasing | Off / 2x / 4x / 8x | `RECOMP_MSAA` | off |
| Graphics | Anisotropic filtering | Trilinear / 4x / 8x / 16x | `RECOMP_ANISO` | 4x |
| Graphics | Draw distance | Original / Far / Maximum | `RECOMP_DRAW_DIST` | original |
| Graphics | Shadows | Original / High / Ultra | `RECOMP_SHADOW_RES` | original |
| Graphics | HD textures | Off / On | `RECOMP_HDTEX` | on (`hd-textures`) |
| Graphics | HUD edges | Off / On | `RECOMP_HUD_EDGES` | on |
| Graphics | CRT filter | Off / On | `RECOMP_CRT` | off |
| Graphics | Motion blur | Off / On | `RECOMP_MBLUR` | off |
| Game | Skip intro | Off / On | `RECOMP_SKIP_INTRO` | on |
| Game | Fast boot | Off / On | `RECOMP_FAST_BOOT` | on (4x) |
| Game | Debug menu | Off / On | `RECOMP_DEBUG_MENU` | off |
| Game | Money cheat | Off / On | `RECOMP_MONEY_CHEAT` | off |
| Advanced | Log level | Errors / Warnings / Normal / Detailed / Everything | `RECOMP_LOG_LEVEL` | normal |

`RECOMP_HDTEX` takes a directory name, not a flag: `hd-textures` or `0`.
`RECOMP_MBLUR` is a strength, so the row writes `0.5` rather than `1`.
`RECOMP_FAST_BOOT` is the multiplier the frontend pauses run at; the row uses
the port's 4x.

## Render height

There is no resolution row. `RECOMP_RES` is written as the display's short side
(the window manager's, not the activity's — the activity excludes the system
bars and would shrink the render), and SDL scales that to the panel. Anti-
aliasing defaults to off at that size because it is the setting that costs the
most fill rate.

## Not offered

Rows the Windows launcher has that a phone does not need: the resolution list,
21:9 and 32:9 aspects, 5.1 audio, and a fullscreen toggle (the game is always
fullscreen). The game folder is on the launcher's main screen rather than in
settings.
