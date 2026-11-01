# Logging: plan and tracker

Goal: a log a person can read after a play session -- what the player did,
on which settings, and what went wrong -- with levels, so a normal session
writes a short story and a bug hunt can turn everything back on. Work
queue for the logging loop: one item per commit, first unticked box next;
tick it (and add a dated line to the log at the bottom) in the commit that
lands it.

## Design

**Levels** (lowest to highest): `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`,
`CRITICAL`. `RECOMP_LOG_LEVEL=trace|debug|info|warn|error|critical`
(default `info`); the launcher's "Log detail" dropdown sets it.

**Line format**: `HH:MM:SS.mmm +SSSS.s LEVEL [CAT] message` -- wall clock,
seconds since start, level, category. The session story uses `SYSTEM`,
`SETTINGS`, `PLAY`, `SAVE`, `PERF`, `INPUT`, `MARK` and `SESSION`; the
rest is a diagnostic's own tag (`VKR`, `HDTEX`, `FILE`, `XA2`, ...).

**Two sources, one filter.** New code logs through `log_msg(level, cat,
fmt, ...)` (src/log.c). The ~840 existing `fprintf(stderr, "[TAG] ...")`
lines (game code and toolkit) stay as they are: the log pump (src/log.c,
started by main.c once stderr is the session file) grades each untagged
line by its `[TAG]` prefix from a table (`[KERNEL]`, `[CS]`, `[PATH]`,
`[FILE]` -> TRACE; `[STATE]`, `[DRAWDIST]` and any other tag -> DEBUG;
`[GATHER]`, `[NV2A-WARN]` -> WARN; failures -> ERROR; `[CRASH]` ->
CRITICAL), indented continuation lines take the level of the line above,
and drops what is below the level. The same message again is written once
with "(repeated N more times)". Switching on a `RECOMP_*_TRACE` /
`*_STATS` / `*_DIAG` diagnostic without a level makes `debug` the default.

**Flight recorder.** Lines below the level are not lost: the pump keeps the
last 2000 in memory and writes them out ahead of an `ERROR` or `CRITICAL`
line, or a player mark, so a crash at `info` still comes with the detail
that led up to it.

**Where things are.** The session header and saves: src/journal.c (the
toolkit's `xbox_file_hook` reports every file open). States, events,
results, quits, load times: src/game_state.c. Screens, game mode, money,
stations: src/ssx3_overrides.c. The mark key and the host's controllers:
src/mark.c. Frame rate: src/perf.c (fed by the renderer's flip). The end:
`session_end` in main.c (the toolkit's exits and the crash handler) writes
the summary; the crash report adds where the game was and the last
`[PLAY]` lines. Files: `logs\` beside the game folder, the newest 20
sessions, 10 crash dumps and 30 marks kept, `latest.txt` names the current
log; the launcher's "Open logs" selects it.

## Work items

### A. The logging core
- [x] **A1** `src/log.c/.h`: levels, `log_msg()`, `RECOMP_LOG_LEVEL`, line
      format with timestamps; usable from game code, overrides and src/vk.
- [x] **A2** Pump filter (main.c): tag -> level table for existing lines,
      level threshold, timestamps added to untagged lines, flight-recorder
      ring flushed on ERROR/CRITICAL. Linux: the same through a pipe (or
      in-process when stderr is not a file).
- [x] **A3** Noise: `[STATE]` sub-state flips (60% of a session log) and
      the boot-time `[KERNEL]`/`[CS]`/`[HEAP]`/`[PATH]` detail graded to
      DEBUG/TRACE; check a 20-minute session at `info` stays short.
- [x] **A4** Log files: keep the last 20 sessions in `logs\` (delete
      older, crash dumps too), name stays `ssx3_<date-time>_stderr.txt`;
      a `latest.txt` copy/pointer for "send me your log".
- [x] **A5** Launcher: "Log detail" dropdown (Errors only / Warnings /
      Normal / Detailed / Everything) saved in launcher.ini, sets
      `RECOMP_LOG_LEVEL`; "Open logs" opens the folder with the newest
      file selected.

### B. The session story (`INFO [PLAY]` unless noted)
- [x] **B1** Session header (`[SYSTEM]`, `[SETTINGS]`): build version (git
      commit + date), Windows/Linux version, CPU, RAM, GPU name, driver
      and Vulkan version, monitor size and refresh, window/fullscreen,
      game folder and the XBE's title id/version, every launcher setting
      and every `RECOMP_*` in effect, controllers at start.
- [x] **B2** Top-level flow: boot, intro, title, frontend, loading,
      in game, results, replay -- one line per change with time spent in
      the previous one (sub-states only at DEBUG).
- [x] **B3** Menu screens by name (main menu, Select Character, Setup
      Character, Select Peak/Mode/Event, My Rules, Options and its pages,
      Lodge and its pages, pause menu, Quit dialog, ...) from the screen
      tracker (sub_00202460), unknown ones as `screen <vt>:<id>`.
- [x] **B4** Game mode at the start of play: Single Event, Conquer the
      Mountain, Freeride, Multiplayer (and the debug quick-start menu).
- [x] **B5** Event start: peak, kind (Race, Slopestyle, Big Air, Super
      Pipe, ...), course name, rider, opponents (needs finding where the
      game keeps the selected event; fall back to the world file loaded).
- [x] **B6** Event end: finishing place / score / medal, time, restarted
      or quit (pause menu -> Restart/Quit).
- [x] **B7** Saves and loads (`[SAVE]`): game saved / loaded / failed,
      slot and profile name, from UDATA file activity.
- [x] **B8** Money and purchases: cash changes with the screen they
      happened on (a drop on Buy Attributes / Buy Gear = a purchase, with
      the amount); the money cheat key logged as such.
- [x] **B9** Lodge and stations: entering a lodge, station prompts
      (lodge / transport), MCOMM Transport destination.
- [x] **B10** Session end: how it ended (window closed, quit to
      dashboard, crash), duration, events played, summary of warnings.

### C. Things that help when something went wrong
- [x] **C1** Mark key: a key (F12 / a controller combination) writes
      `INFO [PLAY] *** player mark ***` with the current screen/event and
      saves a snapshot of the shown frame next to the log -- "I saw it
      here".
- [x] **C2** Performance (`[PERF]`): per event average FPS, 1% low, worst
      frame and GPU time; frames over 100 ms as WARN with what was on
      screen; a once-a-minute summary at DEBUG.
- [x] **C3** Window and display: resizes, fullscreen toggles, focus lost /
      regained, aspect changes, internal resolution rebuilds.
- [x] **C4** Input: controller connected / disconnected / battery low,
      keyboard in use.
- [x] **C5** Audio: output device and channels, underruns and drops as
      WARN (rate-limited).
- [x] **C6** Graphics: Vulkan device lost / errors as ERROR/CRITICAL,
      pipeline build failures, HD textures found / missing, draw
      distance trimming (DEBUG; a full render list WARN).
- [x] **C7** Loading: world files loaded and load time per event; missing
      or unreadable game files as ERROR with the path.
- [x] **C8** Crash report: the CRITICAL block (fault, guest and native
      stacks, minidump path) plus the current screen/event, the last
      `[PLAY]` lines and the flight recorder.
- [x] **C9** Existing diagnostics graded: `[GATHER]`, `[DRAWDIST]`,
      `[BLOCK-RESCUE]`, `[BLOCK-STUCK]`, `[HUD-FLICKER]`, `[GLITCH]`,
      `[FPS30]`, `[HDTEX]`, `[VKR]` ... each with a sensible level.

### D. Finish
- [x] **D1** docs: this file's design section kept current; the player
      README explains the "Log detail" option, the mark key and which
      file to send.
- [x] **D2** Test: scripted runs at each level (a race, a freestyle, the
      lodge via the debug menu), a forced crash (`RECOMP_TEST_CRASH`),
      check the `info` log reads as the story and stays short.

## Progress log
- 2026-10-24: plan written.
- 2026-10-24: A1 -- src/log.c/.h (levels, log_msg, RECOMP_LOG_LEVEL, timestamped lines); main.c's first line uses it.
- 2026-10-24: A2 -- the pump moved to src/log.c (Windows and POSIX), grades lines by tag, timestamps them, keeps a 2000-line flight recorder written out before ERROR/CRITICAL, collapses repeats. A scripted race at info: 6 lines plus the flight recorder; RECOMP_TEST_CRASH: the crash block CRITICAL with its lead-up. Found on the way: `[VKR] upload buffer full` ~900 times while a race loads in scripted runs (none in the user's session) -- draws dropped; to look into separately.
- 2026-10-24: A3 -- the user's 20-minute session log (3,482 lines) graded with the table: 0 lines at info (everything was STATE sub-flips, kernel, file and renderer detail). [BLOCK-RESCUE] down to DEBUG, a full [DRAWDIST] list up to WARN.
- 2026-10-24: A4 -- logs\ keeps the last 20 session logs and 10 crash dumps, latest.txt names the current one, crash dumps go to the session's logs folder (they went to build\logs). Found: started from the launcher (no console) the pump never ran -- it checked GetStdHandle, which stays empty when stderr is freopen'ed; now the CRT's stderr handle. Played sessions had synchronous log writes until now.
- 2026-10-24: A5 -- "Log detail" in both launchers (Errors only / Warnings and errors / Normal / Detailed / Everything, default Normal), saved as log_level, sets RECOMP_LOG_LEVEL; "Open logs" opens the folder with this session's file selected (latest.txt). The SDL launcher change is not compiled here (no SDL headers in WSL).
- 2026-10-24: B1 -- src/journal.c: build (commit + date: cmake/build_info.cmake, from git or the BUILD_INFO sync-windows.sh writes), OS, CPU, RAM, monitor, the XBE's name/title id/version, the settings in words (two lines), every RECOMP_* at DEBUG; the GPU line ([VKR] device, now with the driver version) at INFO. Controllers at start: with C4.
- 2026-10-24: B2 -- game_state.c: one INFO [PLAY] line per top-level state (starting up, intro movies, menus, entering an event, loading, in game, debug menu, leaving the event) with the time spent in the one before; [STATE] sub-flips DEBUG. Briefing/race/results/replay: by screen (B3).
- 2026-10-24: B3 -- screen_note (ssx3_overrides.c, before and after each screen state change): INFO [PLAY] "screen: <name>" -- menus by id (title ... Credits, lodge 39), in game by vtable only (ids there are garbage): briefings, pause, quit dialog, lodge/station prompt, run result 002984B0, event results 00298378; unknown ones as "in-game screen <vt> (not named yet)" so sessions collect them.
- 2026-10-24: B4 -- "game mode: Single Event / Conquer the Mountain / Multi Play / Previews" from the main menu's list (+0x95, 4 rows) when the main menu closes (state 6; the next screen reuses its memory). The list is found among those sub_00205D40 has set up, by its parent being a child of the main menu. Checked with menu_test: down,a = Conquer; down,down,a = Previews (Multi Play is skipped without 2 pads); a = Single Event. The debug quick-start menu shows as its own state (B2).
- 2026-10-24: B5 -- game_state.c: on entering in game, "event: Peak N, <kind>, <course>" from the event number at 0x346440 (17 events: 5 races, 3 slopestyle, 3 big air, 3 super pipe, 3 peak runs); 17 = "free ride (no event)". Rider and opponents not logged yet (where the game keeps them is not found).
- 2026-10-24: B7 -- toolkit hook xbox_file_hook (kernel_file.c, every NtCreateFile with its access and status); journal.c logs Data.ssx writes "game saved", reads "save data read", failed writes WARN. The game opens the file relative to the save folder handle, so the path is just the name; it also reads the save just before overwriting it (read + saved = a save). Checked: lodge -> Save Game -> overwrite Yes.
- 2026-10-24: B8 -- player 1's cash (0x3474B4 + (slot*10 + rider) * 0xF88, profile record [0x346400]*32 + 0x345948) checked after each frontend update: "spent 250 (money 200000 -> 199750, on Buy Attributes)"; the money key: "money key: +200000, now 200000". Screens named on the way: Buy Attributes, dialog, name keyboard, message box; the title only by its vtable (id 0 is reused by dialogs).
- 2026-10-24: B6 -- when the event results screen comes up, player 1's result as the screen's setup (sub_000A4620) reads it: "event result: place 6 of 6, time 4:00.17" (race; time in 1/60 s), "place N of 6, score S, gold medal" (freestyle), won/lost for the two-rider challenges, "did not finish", race rounds 1-2 of 3, platinum. Leaving an event without results: "left Snow Jam without a result"; the pause menu's Quit passes a state of its own (0029AD64, "quitting the event"); the same event entered again without the menus in between: "event restarted". Checked: Super Pipe and Snow Jam runs (score/time), cycle_test 2 quits. Restart itself not driven by a script yet. Screen 00298448 = "race end".
- 2026-10-24: B9 -- station gates (sub_000D5A60: +0x10 state 0 at the gate, 1 prompt up, 2 answered, 3 said no; +0x14 kind 0 lodge; +0x18 station id, 0x17 = Green Base Station) and the in-game screen opener (sub_000C4C20 type 0x1F lodge prompt, 0x21 transport): "at the lodge of Green Base Station: enter? (prompt)", "said no to the lodge of ...", Yes shows as leaving the event + "screen: lodge". Checked with a debug start: Yes, Return to Game, No. Only station 0x17 has a name; the transport prompt and its destination are not checked (no scripted route to a transport gate). Also: cash picked up in game is no longer one INFO line per coin -- DEBUG per change, one "picked up N on the mountain" line on leaving the run.
- 2026-10-24: B10 -- main.c session_end (the toolkit's exits through xbox_before_exit, and the crash report): drains the pump, then journal_session_end writes "session ended: window closed / the game ended itself (...) / crash, after H:MM:SS; N events started, M with a result" and "this log has W warnings, E errors, C critical" (WARN when any). The pump counts lines per level and notes the [EXIT] / window closed / [CRASH] line (both now INFO / CRITICAL). A process killed from outside (Task Manager, live.ps1 stop) has no summary. live.ps1 close ends a run as a player does. Checked: window close, RECOMP_TEST_CRASH. log.h/journal.h now included on every platform (they were Windows-only while the pump calls are not).
- 2026-10-24: C1 -- src/mark.c: F12, or both thumbsticks clicked together on any controller, writes "INFO [MARK] *** player mark N (F12) *** <state / event>; <screen>; snapshot logs\mark_<time>_N.bmp" and saves what the window shows (new toolkit xbox_FramebufferSnapshot, split out of the snapshot trigger; SDL and web too). Its own host thread (50 ms), so it works while the game hangs; RECOMP_MARK=0 off. The pump writes the flight recorder before a [MARK] line (the detail of the seconds before). logs\ keeps the newest 30 marks. Checked: F12 on the title (640x480 snapshot in a scripted run). The controller combination is not testable in scripted runs (they ignore the machine's pads).
- 2026-10-24: C2 -- src/perf.c, fed by vkr_backend.c be_flip at every presented frame: per event (game_state.c starts it on entering in game, reports on leaving) "INFO [PERF] Snow Jam: 60.0 FPS average, 1% low 52.6 FPS, worst frame 23 ms (2116 frames, 35 s)"; a frame over 100 ms in game: WARN with the event and screen (one line a second, the rest counted); the last minute at DEBUG. An event's first 2 s are left out (the first frame after a load took 160 ms every time). GPU time is not in it: the timestamps exist only with RECOMP_VKR_GPUPROF (which logs its own [GPUPROF] lines). Software renderer (RECOMP_GPU=sw): no [PERF].
- 2026-10-24: C3 -- toolkit window: "[FBWIN] fullscreen on/off" (F11 / Alt+Enter, and at start with RECOMP_FULLSCREEN), "[FBWIN] focus lost / gained" (Win32 and SDL); graded INFO with "[VKR] internal resolution" (rebuilds of the render target); "[VKR] window WxH: frame aspect" (resizes, widescreen auto) was INFO already. Checked: focus gained at start, window close. Fullscreen not toggled in a script (it would cover the screen).
- 2026-10-24: C4 -- mark.c's thread also logs [INPUT]: "controller N connected at start (gamepad, battery full / wired)", later connects, "controller N disconnected" (WARN), battery dropping to low / empty (WARN, checked every 10 s), "no controller connected at start", "keyboard in use (first key 0xNN)", and in scripted runs "the machine's controllers are ignored (RECOMP_NO_HOST_PAD)" (=0 now counts as not set, as in main.c). Checked: keyboard, no controller, scripted. A connected controller was not available to test (none plugged in during the loop).
- 2026-10-24: C5 -- toolkit audio: "[XA2] output device: 2 channels at 48000 Hz (speaker mask 0x3)" after the XAudio2 init line (both INFO); without RECOMP_APU_LEVEL, "[XA2] audio output: N underruns, M dropped buffers in the last 10 s" every 10 s when there were any (WARN); XAudio2/SDL/waveOut failures ERROR, falling back to waveOut WARN. Checked: one unmuted start (stereo device).
- 2026-10-24: C6 -- vkr.c: a failed Vulkan call (CK, and now the per-frame end/submit/fence wait, which returned silently) logs ERROR with the result in words, CRITICAL for VK_ERROR_DEVICE_LOST ("GPU driver reset or crashed"); per-frame failures the first time and every 600th. Init/shader/memory failures ERROR or CRITICAL, missing BC7 / depth-stencil / pipeline cache full WARN by tag. HD textures: "507 replacement textures mapped" INFO, "no HD texture pack at ...: the game's own textures" INFO, unreadable or wrong-format files WARN, loads DEBUG. Draw-distance trimming was graded in A3. vkr_selftest links log.c. Checked: a start with the pack (507 mapped).
- 2026-10-24: C7 -- journal.c's file hook: "loading world file BAM.big" (a data\worlds\*.big other than the last one), a disc file (D:\data\... / \Device\CdRom0\...) that does not open for reading: ERROR "game file missing or unreadable: data\... (status)", once per path; other failed opens DEBUG (a race cycle has exactly one, t:\$u\contentmeta.xbx, the game probing). game_state.c times entering an event (or loading, which the 100 ms sampler sees when it misses the short first state) to its briefing screen: "event loaded in 2.1 s". Free ride has no briefing, so no load time.
- 2026-10-24: C8 -- the crash report (Windows VEH and POSIX signal handler) now has "while: in game, Peak 1 Race Snow Jam" (state, event, screen) and the last 8 INFO [PLAY] lines ("recent: 02:37:48.914 screen: race briefing", ...) as indented lines of the CRITICAL block (log.c keeps them as the pump passes them); the flight recorder before it and the minidump path were there already, the session summary after it (B10; the POSIX handler now writes it too). Checked: RECOMP_TEST_CRASH in a race.
- 2026-10-24: C9 -- went through every [TAG] the game and toolkit print (~170). Opt-in traces stay DEBUG (any [TAG] line); the always-on ones graded: [NV2A-WARN], [APU-WARN], [ABI], [IRQLBUG], "[GPU] REFUSING", missing TSO / flat dispatch WARN; [ALLOC] FAILED, [CONTIG] arena exhausted, intro-movie failures ([VIDEO] File not found / could not open / ... failed) ERROR; [BOOT] widescreen, [RECORD], [DEBUGMENU] INFO; [GATHER] / [BLOCK-STUCK] WARN and [DRAWDIST] / [BLOCK-RESCUE] / [STATE] DEBUG from A3. New: with a RECOMP_*_TRACE / *_STATS / *_DIAG switch on and no RECOMP_LOG_LEVEL, the default level is debug -- a trace that was asked for shows (checked with RECOMP_UI_TRACE=1).
- 2026-10-24: D1 -- design section brought up to date (pump in log.c, categories, repeats, trace default, mark, where each part lives, files kept); PLAYER-README.txt: "Reporting a problem" (which file to send, F12 / both sticks, Log detail, crash dump); README: a Logs paragraph and F12 in the key table.
- 2026-10-24: D2 -- scripted runs at info: a Super Pipe event 2:39 long = 38 lines (header, settings, menus, game mode, event, load time, run result, event result, session end); Snow Jam with the window closed in the race: the event's [PERF] line now comes at the session end ("until the session ended"); the lodge (B9), a forced crash (C8) and window close (B10) at info earlier in this loop; trace is what every scripted run uses. At warn the file was empty -- [SESSION] lines (the build line, now with the log level, and the summary) are now written at any level. Settings in words: "resolution native 480p, aspect 4:3 (widescreen off)". Found: cycle_test/to_race need the DEBUG "[AUTONAV] ... at the briefing" line, so they cannot run at info (live.ps1's trace default covers them). Still open, separately: `[VKR] upload buffer full` ~560x while a race loads in scripted runs (the per-frame upload buffer, draws dropped), which also pulls the flight recorder (~1200 lines) into an info log; never seen in the user's sessions.
- 2026-10-24: C2 follow-up (user: "will it log when and where low FPS occurred?") -- perf.c also finds slow stretches: consecutive seconds of an event under RECOMP_PERF_SLOW_FPS (default 50), 2 s or longer, one WARN when it ends (or when the event / session ends): "slow for 34 s in Snow Jam: 41.3 FPS average, lowest second 35 FPS; from 2 s into the event (+15.2) (rider at x, y, z) to 36 s (rider at x, y, z)" -- +15.2 matches the uptime column. Positions are player 1's world coordinates (ssx3_PlayerPos), comparable between runs of the same course. The long-frame lines now carry the seconds into the event and the rider position too. Checked with RECOMP_PERF_SLOW_FPS=61 (every second "slow") on Snow Jam.
- 2026-10-27: live settings (user: black ground at Gravitude's start in race 1, gone in race 2; wants to try settings as it happens) -- src/live.c, polled by mark.c's thread: F4 HD textures on/off (vkr_hdtex.c hdtex_request, applied on the render thread; the texture cache is emptied at the next flip, vkr_tex_purge, so every texture is made again), F5 draw distance Normal 1x / Far 1.5x / Maximum 2x (ssx3_LiveDrawDistNext; the adaptive view starts from 1x and grows), F6 rider shadows off (draws into the swizzled shadow maps dropped: the maps stay cleared and transparent). Each one on screen for 3 s (new toolkit xbox_FramebufferOsd, under the FPS line; the overlay font now has A-Z) and logged "INFO [SETTINGS] live: ... (F4)". Checked in a race: all five presses logged, messages shown, textures back after the purge.
- 2026-10-27: each player mark also writes "mark N state: HD textures on, draw distance 1.5x (now 1.2x, adapted), rider shadows off, MSAA 4x, 640x480 | 60.2 FPS last second | 21 s into Snow Jam, rider at x, y, z | texture cache 34 MB" -- the settings in effect (F4-F6 may have changed them), the draw distance actually used, the frame rate, where, and how full the texture cache is (emptied and made again above 768 MB). Checked in a race after F6.
- 2026-10-27: marks are PNG on Windows (user: does BMP help with debugging? -- no, both lossless). The presenter's capture (toolkit gpu_present.c) now only copies the back buffer on the window thread; a thread of its own encodes it with Windows' WIC PNG encoder (24-bit, ~half the BMP's size: 5.7 MB at 4693x1320) or writes the BMP for a .bmp path, under "<name>.part" renamed when complete. No frame over 100 ms around a mark in a 32:9 race. Linux keeps BMP (no encoder in the SDL presenter); logs\ prunes PNG and BMP marks.
