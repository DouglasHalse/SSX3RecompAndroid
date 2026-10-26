# SSX3RecompAndroid

[![Listed on PlayPorted](https://playported.com/badge.svg)](https://playported.com/ports/434-ssx-3/)

An **Android port** of [ssx3-recomp](https://github.com/koenhendriks/SSX3-recomp), the static
recompilation of **SSX 3** (original Xbox, PAL): the game's PowerPC code is recompiled to
native ARM64 and the Xbox GPU is emulated on Vulkan. **There is no emulator in between** — it
runs on the phone itself, with on-screen controls, saves written to the device, and no PC in
the loop once the game data is there.

Upstream targets Windows and Linux and supplies almost everything here: the recompiled code,
the Vulkan renderer, the launcher, HD textures, widescreen. This fork adds the Android platform
layer and what a phone needs. The toolkit side of it lives in
[DouglasHalse/xboxrecomp](https://github.com/DouglasHalse/xboxrecomp) (branch `android-port`).

## Made with AI

This project is **heavily AI-assisted**. The Android work here was written by an AI agent
from goals, reports and verification by a human running the port on a real phone. Upstream's
code, the game recompilation and the renderer, is not: see
[koenhendriks/SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp).

## Install on a phone

Download `ssx3-android-<version>.apk` from the [Releases](../../releases) tab and open it on the
phone (arm64, Android 11 or newer). Then:

1. Open **SSX 3**. The launcher says which build it is: a release says `Version 0.1.0`, any
   other build says `Build <commit> · <date>`.
2. Tap **Select game image (.iso)** and pick **your own** SSX 3 disc image — `.iso` or `.xiso`,
   trimmed or full redump. No game data is included in the APK and none is downloaded: the app
   reads the image, checks it against the PAL disc this port was compiled from, and refuses
   anything else, saying which disc it actually found.
3. It extracts the disc into its own storage (about 2.1 GB, with a progress bar) and **Play**
   appears when that finishes.

The imported game, your saves and the settings live under
`Android/data/com.douglashalse.ssx3/files/ssx3/`, so uninstalling the app removes them;
re-importing the same image later keeps your saves, because they are separate files.

Releases are signed with the same key every time (the one committed in
`android/ssx3-debug.keystore`), so a newer APK installs over an older one and keeps the data.

## What this fork adds

| | |
|---|---|
| **An Android build.** | `android/` packages the port with the NDK and **no Gradle**: aapt2, javac, d8, zipalign, apksigner. arm64-v8a, minSdk 30, `libmain.so` + `libSDL2.so`, Vulkan through the phone's own driver. |
| **It boots on a phone.** | The guest's 4 GB are reserved at a low base, because Android hands out 39-bit user addresses and refuses the fixed mappings a desktop takes for granted; the framebuffer is presented through SDL after SDL_main owns the window and the game runs on its own thread; storage is rooted in the app's own folder, which needs no permission at all (the manifest still asks for all-files access for the adb-pushed development path under `/sdcard/Download`, which the importer replaces); everything the game prints goes to logcat. |
| **On-screen controls.** | An Xbox pad drawn over the frame — floating left stick, D-pad, the ABXY diamond with its letter colours, the shoulder cluster, BACK/START — reported as pad 0 through the same XID report a physical controller uses, so the game reads it exactly like real hardware. The layout, colours and hit rules are [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid)'s gamepad overlay ported line for line (see [Inspiration](#inspiration)), including its rule that the pad **disappears while a real controller is connected** and returns when it goes. `RECOMP_TOUCH_PAD=0` turns it off; `RECOMP_TOUCH_TRACE=1` logs what each touch hit. |
| **Saving, loading and deleting on POSIX.** | The toolkit's file layer resolved only absolute names. A title deletes a save by opening each file *through the save folder's handle*, so on Android every delete failed while the files stayed on disk — the Windows backend had handled it from the start. Also: directory scans restart when a pattern is supplied, free their slot on close, and match with a portable glob (`fnmatch(..., FNM_CASEFOLD)` treats `*.*` literally). |
| **Audio that keeps up.** | Throttle debt drains at a bounded rate instead of one lump after a stall, the audio thread runs at audio priority, the voice lock is taken only when a voice changes: 188 buffers/s (48 kHz), 0 underruns. |
| **Disc image import** (in progress). | `src/android/xiso.c` reads an XDVDFS image **in place** through the file picker's descriptor — no 2 GB temporary copy — finds the volume descriptor in either the trimmed (`0x10000`) or redump (`0x18300000`) layout, fingerprints `default.xbe` against the PAL disc so a different region is refused *before* extracting 2 GB, and extracts the tree. The extractor is done and tested (`tests/xiso/run.sh`); the launcher screen around it is next, following the installer flow of [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid) (see [Inspiration](#inspiration)). |
| **Crash reports that name a caller.** | `RECOMP_NO_CRASH_HANDLER=1` hands the fault to the platform's debuggerd, whose tombstone carries a symbolized backtrace. That is how a boot crash in the log pump was traced to the guest CRT's `isatty` shadowing libc's. |

## Compatibility

**Verified** on this device:

| Device | GPU | Vulkan | What was checked |
|---|---|---|---|
| Samsung Galaxy S24 (SM-S921B), Android 16, arm64 | Samsung Xclipse 940 (AMD RDNA 3 based) | 1.3 | Boot, menus, races, save, load, in-game delete, audio at 48 kHz, on-screen controls |
| Android emulator, x86-64 | SwiftShader (software Vulkan) | 1.3 | Used to test logic and input; far too slow to play |

**Expected to work** (not tested here): any arm64 device with **Vulkan 1.3** and
`VK_KHR_push_descriptor`, on Android 11 or newer —

- Snapdragon 8 Gen 1 / 2 / 3 (Adreno 730 / 740 / 750)
- Snapdragon 888 / 870 (Adreno 660 / 650)
- Exynos 2200 / 2400 and Tensor G3 / G4 (Xclipse, Mali-G715 / G730)
- Dimensity 9200 / 9300 (Immortalis-G715 / G720)

Mali parts that lack BC (DXT) texture compression are expected to run: the renderer asks
the driver and decodes those textures in software when it cannot, so expect slower frames rather
than a refusal.

**Not expected to work:**

- **Vulkan 1.2 or older** (older midrange Adreno and Mali parts): the renderer needs dynamic
  rendering, `synchronization2` and push descriptors from Vulkan 1.3.
- **32-bit-only devices**: the APK ships `arm64-v8a` only.
- **Windows and Linux**: nothing here improves on upstream — use
  [koenhendriks/SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp) for those.

**You need:** Android 11 (API 30) or newer, arm64-v8a, about 2.5 GB free for the game data,
and your own rip of the **PAL** disc. A USA or Japanese disc is refused: the recompiled code is
the PAL build, and the two are different code at the same addresses.

## Inspiration

The shape of this port follows
[**LibertyRecompAndroid**](https://github.com/vaduur/LibertyRecompAndroid), the Android port of
GTA IV's static recompilation. It is the clearest demonstration that a recompiled Xbox title can
run on a phone with no emulator in between, and three of its decisions are the ones taken here:

- a **launcher that imports the game image**, instead of expecting the data to be pushed onto
  the device from a PC first;
- an **on-screen touch pad** laid out the same way — analog stick and D-pad on the left, face
  buttons and the shoulder cluster on the right, BACK and START centred at the top;
- writing down **what was measured on which device**, so that a "works on my phone" claim
  carries its GPU with it.

Their [`docs/android/PORTING.md`](https://github.com/vaduur/LibertyRecompAndroid/blob/main/docs/android/PORTING.md)
is the best write-up of the platform work in general (installer, file picker, Vulkan driver
choice, per-thread tuning) and is worth reading even though SSX 3 runs on a different runtime.
The code here is our own — SSX 3 rides this project's own recompiler and toolkit, so the
Android layer lives in [DouglasHalse/xboxrecomp](https://github.com/DouglasHalse/xboxrecomp)
rather than in a ReXGlue fork.

## Upstream: the Windows and Linux port

The rest of this README is upstream's. It describes the Windows and Linux port this fork is
built on, including the parts that already work on Android unchanged.

| Platform | Status |
|---|---|
| **Windows** (x86-64) | Fully supported: the release download, the launcher, everything below. |
| **Linux** (x86-64) | Boots to a race at 60 FPS on the Vulkan renderer, with its own launcher. Build it yourself (see [Build](#build)). |
| **Linux on Apple Silicon Macs** (arm64, [Asahi Linux](https://asahilinux.org)) | Boots through the intro to the menus. Gameplay not checked yet. |

Native macOS is not supported. On a Mac, run it under Asahi Linux.

![Snow Jam in 32:9 at 5120x1440](docs/screenshots/33-widescreen-32x9-snow-jam.png)
<sub>Snow Jam on a 5120x1440 (32:9) monitor: the game's camera is widened, so nothing is stretched, and the HUD keeps its shape.</sub>

No game data or game code is in this repository. You need your own rip of the
disc (`default.xbe` + `data/`); everything derived from it is regenerated by
the scripts below.

<img src="docs/screenshots/44-launcher.png" width="420" alt="The launcher: game folder, display options, controllers">

<sub>The launcher: point it at your SSX 3 files, pick the display options and press Play. Download it with the game from the [latest release](https://github.com/koenhendriks/SSX3-recomp/releases/latest).</sub>

## What works

- **The whole game.** Boot, menus, every Race and Freestyle event on all
  three peaks, Conquer the Mountain (free-roam, transport), and
  2-player split-screen with two controllers.
- **Saving and loading.** Saves go to a hard-disk save in `game\UDATA`,
  and local time zones are respected.
- **60 FPS.** The game's vertex programs and pixel combiners run on the GPU.
  A 2-hour automated soak test passed with no crash, and 48 of 48 parallel
  boots reached a race.
- **Accurate visuals**, checked frame by frame against the
  [xemu](https://xemu.app) emulator: lighting, colours, snow spray,
  board trails and the display gamma ramp.
- **Modern display.** The game renders at your monitor's height (1440p, 4K,
  ...) in your monitor's shape. Fullscreen is F11 or Alt+Enter.
- **Audio** through the emulated Xbox sound chip, plus rumble on an XInput
  controller.
- **Keyboard and controller**, with remappable keys and buttons
  (`keys.ini`).

## What's new

| | |
|---|---|
| **HD textures.** Bl4ckH4nd's HD texture pack for the PS2 version ([gbatemp](https://gbatemp.net/threads/my-ps2-packs.621422/)) is included: the renderer swaps each matched texture for its HD version, mipmaps included. Left: the original; right: HD. | ![HD textures](docs/screenshots/45-hd-textures.png) |
| **True widescreen.** The game's camera projection (D3DX `PerspectiveFovLH`) is widened to your monitor's aspect, so you see more of the mountain instead of a stretched picture. The HUD keeps its 4:3 proportions, and menus are pillarboxed. | ![16:9](docs/screenshots/34-widescreen-16x9-snow-jam.png) |
| **Native resolution.** The Vulkan renderer draws at your monitor's height (5120x1440 above, 1920x1080 here) and hands the full image to the window. The game itself still sees 640x480. | ![1080p](docs/screenshots/38-character-select-1080p.png) |
| **Lighting matches the hardware.** Riders were washed out because vertex colours were not clamped the way the Xbox GPU clamps them. Left: xemu; right: this project. | ![lighting](docs/screenshots/39-lighting-vs-xemu.png) |
| **Every peak.** All Peak 2 and 3 races (Ruthless Ridge, Intimidator, Ruthless, Gravitude, The Throne) and the Freestyle events on every peak load and run. | ![peaks](docs/screenshots/40-peaks-2-3-races.png) |
| **Freestyle**: halfpipe, slopestyle, big air and the night event with fireworks. | ![freestyle](docs/screenshots/41-freestyle-all-peaks.png) |
| **Trails and snow spray.** The game draws board trails as lines and snow spray as point sprites. The GPU renderer now draws both, following xemu's rules. | ![trails](docs/screenshots/42-intimidator-board-trails.png) |
| **2-player split-screen** (Multi Play) works when two controllers are plugged in at start. | ![split](docs/screenshots/43-split-screen.png) |
| **Conquer the Mountain** free-roam and stations, in widescreen. | ![ctm](docs/screenshots/36-widescreen-conquer-the-mountain.png) |
| **Race briefing**: the 3D scene fills the screen and the panel stays centred. | ![briefing](docs/screenshots/35-widescreen-briefing.png) |
| **Menus** keep their 4:3 artwork, pillarboxed. | ![menu](docs/screenshots/37-menu-pillarboxed.png) |

Other new things:

- **Native Windows build.** No WSL is needed: `setup.ps1`, `regen.ps1`
  and `build.bat`, verified from a fresh clone.
- **Linux on x86-64.** Device registers are trapped and decoded on x86-64
  as well as arm64, and the launcher has an SDL2 version.
- **Test tooling.**
  - The game can drive its own menus (`RECOMP_AUTONAV`), getting from boot
    to a race in about 27 s.
  - Every scripted run gets its own save (with everything unlocked) and its
    own emulated hard disk.
- **xemu as a reference.** `tools\xemu.ps1` and the `xemu` agent skill boot
  xemu with the ISO, press buttons, capture frames, restore snapshots
  (character select, race start, ...) in a second and read the game's frame
  counter.

## Play

On Windows, after building, start **`build\launcher.exe`**: browse to the
folder of your SSX 3 rip (it checks that `default.xbe` is SSX 3 PAL), pick
resolution, aspect ratio, fullscreen, CRT shader, FPS counter, intro
movies and the debug quick-start menu, see which controllers are
connected, and press Play. The choices are kept in `launcher.ini`.
`tools\package.ps1` builds a release folder and zip (`launcher.exe`,
`ssx3_recomp.exe`, a player README; no game files) for running it without
a build environment: unzip, run `launcher.exe`, play.

From a console, `tools\play.bat` does the same with the defaults, which
follow your monitor.

On Linux, start **`build-linux/launcher`**. It has the same settings and
`launcher.ini`, except Fullscreen, the FPS counter and 5.1 audio, which the
Linux build doesn't have yet. `tools/play.sh` starts the game directly with
the defaults.

The launcher's choices are these environment variables:

| Setting | Default | |
|---|---|---|
| `RECOMP_WIDESCREEN` | `auto` | `auto` = the window's shape (your monitor's at start, then follows resizes); `16:9`, `21:9`, any `W:H`; `0` = original 4:3 |
| `RECOMP_RES` | monitor height | internal resolution as a height (`1080`, `1440`, `2160`) or `480` for the original |
| `RECOMP_SKIP_INTRO` | `1` | `0` plays the EA/THX/intro movies |
| `RECOMP_GPU` | Vulkan | `sw` = the software rasteriser (reference/fallback) |
| `RECOMP_PRESENT_FILTER` | linear | `point` for sharp pixels |
| `RECOMP_FULLSCREEN` | `0` | `1` = start in fullscreen (F11 toggles); Windows |
| `RECOMP_CRT` | `0` | `1` = CRT TV shader (needs a resolution above 480); tune with `RECOMP_CRT_PARAMS` |
| `RECOMP_DEBUG_MENU` | `0` | `1` = EA's quick-start menu (Start on the title screen) |
| `RECOMP_MONEY_CHEAT` | `0` | `1` = for testing: each Home key press gives player 1's rider 200,000 cash (spendable cash only; the career total earned is left alone) |
| `RECOMP_GAME_DIR` | `game` | the folder with `default.xbe` and `data\` |
| `RECOMP_ANISO` | `16` | anisotropic filtering (`0` = trilinear); the game's textures also get mip chains (`RECOMP_TEX_MIPS=0` turns those off) |
| `RECOMP_DRAW_DIST` | `1.5` | view distance in races, times the original (`1` = original, up to `2`) |
| `RECOMP_HUD_EDGES` | `1` | widescreen: the race HUD at the screen edges instead of the 4:3 middle (`0` = middle); `RECOMP_HUD_MARGIN` insets it (% of the way) |
| `RECOMP_SHADOW_RES` | `4` | rider shadow maps drawn on the GPU at this times 128x128 (`1`, `4`, `8`; `0` = the old software path) |
| `RECOMP_FAST_BOOT` | `4` | the title's intro wait and the "Checking hard disk" screen run this many times faster (`0` = off); boot to the main menu ~4.6 s instead of ~12.7 s |
| `RECOMP_AUDIO_CHANNELS` | `2` | `6` = 5.1 surround output (the game mixes front, centre and rear speakers; Windows); stereo uses a proper downmix |
| `RECOMP_MSAA` | `4` | anti-aliasing: `0` off, `2`, `4`, `8` samples (as far as the GPU goes); cut-out edges (trees, fences) are smoothed too |
| `RECOMP_HDTEX` | `hd-textures` | the HD texture pack (Bl4ckH4nd's, see below) and its `ssx3_map.txt`; `0` = off |
| `RECOMP_FPS30` | `0` | `1` = keep the game's own 30 FPS stretches (split screen, Metro-City's tunnel); by default they render at 60 |
| `RECOMP_FPS_OVERLAY` | `0` | `1` = start with the FPS counter on (F3 toggles it): game FPS, frames shown, slowest frame; Windows |

**Keyboard.** The game window must have focus. Remap the keys by copying
`keys.example.ini` to `keys.ini` and editing it.

| Key | Pad | Key | Pad |
|---|---|---|---|
| Enter | Start | Backspace | Back |
| Z X A S | A B X Y | Q E | White / Black |
| 1 3 | Left / right trigger | Arrows | D-pad |
| Numpad 8/4/2/6 | Left stick | I J K L | Right stick |
| F11 / Alt+Enter | Fullscreen | F3 | FPS counter |
| F12 | Mark a problem in the log | | |

F11 and F3 work on Windows only.

**Controller.** An Xbox/XInput controller works without setup, rumble
included; two controllers enable Multi Play.

**Logs.** Each run's log is kept in `logs\` (the last 20; `logs\latest.txt`
names the newest). At the default level it reads as the session's story --
settings, menus, events with results and frame rate, saves, money spent,
warnings, how it ended; the launcher's "Log detail" (or
`RECOMP_LOG_LEVEL=trace|debug|info|warn|error|critical`) turns detail up or
down. F12, or both thumbsticks clicked, writes a marked line plus a
snapshot (`mark_<time>.bmp`). A crash also leaves a minidump there
(`crash_<time>.dmp`, open it in Visual Studio next to
`build\ssx3_recomp.pdb`). Details: [docs/LOGGING.md](docs/LOGGING.md).

On Linux, any controller SDL recognises works: wired over USB, or over
Bluetooth. The Xbox Wireless Adapter needs the
[xone](https://github.com/dlundqvist/xone) driver (Arch:
`yay -S xone-dkms xone-dongle-firmware`). Started from the launcher without
a terminal, the log goes to `logs/` beside the game folder.

## Build

**Windows** (no WSL). You need Visual Studio 2022 with the C++ workload,
the Vulkan SDK (`winget install KhronosGroup.VulkanSDK`), git and
Python 3.9+.

```powershell
git clone https://github.com/koenhendriks/SSX3-recomp.git ssx3-recomp; cd ssx3-recomp
# put your rip in game\ : game\default.xbe and game\data\
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1   # toolkit (pinned + patched) in ..\xboxrecomp, Python venv
powershell -ExecutionPolicy Bypass -File scripts\regen.ps1   # analysis, decomp\ split, src\recomp\gen\ C lift (~7 min)
tools\build.bat                                              # MSVC + Ninja, ~2 min on 32 threads
tools\play.bat
```

**From WSL**, which is how the project is developed. The build itself is the
same MSVC build on the Windows side:

```bash
scripts/setup.sh                 # toolkit clone (pinned + patched) and Python venv
scripts/regen.sh                 # needs the rip in ../SSX3/
scripts/sync-windows.sh --game   # copy to C:\Code\Xbox for the MSVC build
cmd.exe /c 'C:\Code\Xbox\ssx3-recomp\tools\build.bat'
```

**Linux** (x86-64, or arm64 on Asahi). You need GCC or Clang, CMake,
Ninja, SDL2, SDL2_ttf, fontconfig, the Vulkan headers, glslc, git and
Python 3.9+ (Arch: `sdl2 sdl2_ttf fontconfig vulkan-headers shaderc cmake
ninja`).

```bash
git clone https://github.com/koenhendriks/SSX3-recomp.git ssx3-recomp && cd ssx3-recomp
# put your rip in game/ : game/default.xbe and game/data/
scripts/setup.sh                 # toolkit (pinned + patched) in ../xboxrecomp, Python venv
XBE=game/default.xbe scripts/regen.sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-linux
build-linux/launcher
```

[docs/LINUX.md](docs/LINUX.md) has the details: what the port does,
its status per platform and what is not ported yet.

The toolkit is pinned at v0.13.1 (`193e299`), with this project's fixes in
`patches/xboxrecomp-local.patch`.

### Cutting a release

`scripts/release.sh 0.1.0` builds the APK from the tree you are in, tags `android-v0.1.0` and
publishes the APK to GitHub Releases with install notes. It refuses a dirty tree, an existing
tag, or a missing `src/recomp/gen`: the recompiled game code is not in git, so a release is
built where the disc rip and the regen output are — GitHub Actions cannot build this APK, and
does not pretend to (`--dry-run` builds and prints what would be published, without touching
GitHub).

The APK is stamped with what it came from in three places that agree: the manifest
(`versionName`, and `versionCode` from the commit count, so it always increases), a generated
`BuildInfo` class that the launcher prints, and the release notes. A release build says
`Version 0.1.0`; every other build says `Build <short commit> · <date>`.

## Testing

Scripted runs are silent, ignore a real controller, render at 640x480 in
4:3, and each gets its own copy of a save fixture, so your saves are never
touched and the PC stays usable while they run. The fixture is
`work\saves\unlocked\` (not in git): copy your `game\UDATA` there,
ideally from a save with every peak unlocked; without it runs start with
no save. Run these from the repo root on Windows:

| Command | What it does |
|---|---|
| `tools\race_test.ps1 -Runs 1 -RaceSec 20 -Snap x.png [-Rider N -Peak N -Mode N -Event N]` | boot to a race, ride, screenshot |
| `tools\race_par.ps1 -Instances 8 -Runs 3` | parallel boot-to-race batches (24 runs in ~3 min) |
| `tools\cycle_test.ps1 -Cycles 12` | race, pause, quit to the title, again (leak/soak) |
| `tools\menu_test.ps1 -Steps "y,wait,down,a"` | menu presses with a screenshot per step (`p2-a` for player 2) |
| `tools\burst.ps1 -From 15 -To 40 -Every 2` | screenshot burst during a race |
| `tools\bench.ps1 -Seconds 60 -Peak N -Event N` | frame-time benchmark (`logs\bench_*.json`) |
| `tools\xemu.ps1 start -Snapshot race_go -Paused` / `cap` / `press` | reference frames from xemu |

## Known issues

- **Effects DSP.** The game's audio effects processor (reverb etc.) is a
  stub, so there is no reverb. The level meter shows sound in races, menus
  and the intro movies.
- **Peaks 2 and 3** are locked on a fresh save, as in the original game.
  Unlock them in game or with the cheat.
- **Untested.** Audio hasn't been checked by ear in every mode.

## Screenshots of the journey

| | | |
|---|---|---|
| ![](docs/screenshots/01-black-window-no-draws.png) | ![](docs/screenshots/05-intro-movie.png) | ![](docs/screenshots/12-gameplay-race.png) |
| first boot: a black window | the intro movie plays | first race (software renderer, ~5 FPS) |
| ![](docs/screenshots/21-race-async-11fps.png) | ![](docs/screenshots/27-vulkan-race-57fps.png) | ![](docs/screenshots/29-metro-city-60fps.png) |
| asynchronous GPU executor, 11 FPS | Vulkan renderer, 57 FPS | Metro-City at 60 FPS |
| ![](docs/screenshots/17-title-screen.png) | ![](docs/screenshots/30-equip-gear.png) | ![](docs/screenshots/31-load-game.png) |
| title screen | Equip Gear | loading a hard-disk save |

## Legal

This is an unofficial fan project, not affiliated with or endorsed by
Electronic Arts or Microsoft. SSX and SSX 3 are trademarks of Electronic
Arts. The repository holds only original code, tooling and notes: no game
code or data. The build is generated from a copy of the game you own, and
the screenshots show this project running. The one set of assets is
`hd-textures/`: Bl4ckH4nd's HD texture pack, included with EA's approval.

## License

GPL-2.0-or-later (see [LICENSE](LICENSE)), copyright Koen Hendriks and
Corné Dorrestijn. One exception: `patches/xboxrecomp-local.patch` changes
the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit and is
offered under the toolkit's own licenses (MIT, and LGPL-2.1 for its APU
files), so the fixes can go upstream. The DDS files in `hd-textures/` are
not covered by the GPL: they are Bl4ckH4nd's work (see Credits).

## Credits

- **HD textures**: [Bl4ckH4nd](https://gbatemp.net/threads/my-ps2-packs.621422/),
  the SSX 3 HD texture pack made for the PS2 version (PCSX2), in
  `hd-textures/`. `tools/hdtex_match.py` pairs its files with the Xbox
  game's textures (`hd-textures/ssx3_map.txt`).

## More

- [docs/ROADMAP.md](docs/ROADMAP.md): the plan and tracker (phases, gates,
  progress log).
- [CLAUDE.md](CLAUDE.md): the bring-up history, every fix, the debugging
  workflow and the open investigations.
- [docs/VULKAN.html](docs/VULKAN.html) and [docs/LINUX.md](docs/LINUX.md):
  the Vulkan renderer and the Linux port, by Corné Dorrestijn.
