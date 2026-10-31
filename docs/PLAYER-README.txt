SSX 3 -- static recompilation for Windows
=========================================

1. Unzip this folder anywhere (keep launcher.exe next to ssx3_recomp.exe).

2. You need your own copy of SSX 3 for the original Xbox, PAL version,
   as files: default.xbe and the data folder next to it (for example
   extracted from your disc image with extract-xiso). No game files are
   included here.

3. Run launcher.exe, press Browse and pick the folder that holds
   default.xbe. The launcher checks that it is SSX 3 (PAL).
   Choose resolution, aspect ratio, CRT shader and so on, then press Play.

Requirements
  - Windows 10 or 11, 64-bit
  - A graphics card with Vulkan support and an up-to-date driver
  - Windows "N" editions: install the Media Feature Pack

Controls
  An Xbox / XInput controller works without setup (rumble included);
  plug in two or more before pressing Play for Multi Play.
  Keyboard (player 1): Enter = Start, Z X A S = A B X Y, arrows = d-pad,
  numpad 8/4/2/6 = left stick, 1 / 3 = triggers. Copy keys.example.ini to
  keys.ini to change the keys.
  F11 or Alt+Enter: fullscreen.  F3: FPS counter.
  F12 (or both thumbsticks clicked together): mark a problem, see below.
  While playing, to try a setting on a problem as it happens (shown on
  screen and written to the log; the next start uses the launcher's again):
    F4 HD textures on/off, F5 draw distance Normal / Far / Maximum,
    F6 rider shadows on/off.

HD textures
  The hd-textures folder holds Bl4ckH4nd's HD texture pack for SSX 3
  (made for the PS2 version, used with permission:
  https://gbatemp.net/threads/my-ps2-packs.621422/). The game draws the
  matching textures from it; the "HD textures" box in the launcher turns
  them off.

Saves are written next to the game files (UDATA). Logs go to a logs
folder next to the game folder; "Open logs" in the launcher shows them.

Reporting a problem
  Every session writes a log: the settings, the menus and events you
  played, results, saves, frame rate per event, warnings and errors, and
  how the session ended. "Open logs" in the launcher opens the folder with
  the newest log selected (latest.txt names it too) -- send that file.
  - Saw something wrong? Press F12 right away (or click both thumbsticks).
    That marks the moment in the log and saves a picture of the screen
    next to it (mark_<time>.png); send that too.
  - "Log detail" in the launcher: Normal is right for playing. Detailed or
    Everything write much more (and bigger files); use them when asked to.
  - After a crash the log ends with the crash report, and a crash_<time>.dmp
    is left next to it; send both.
