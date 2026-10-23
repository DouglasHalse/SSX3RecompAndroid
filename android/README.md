# SSX 3 on Android

The static recompilation is plain C, so it builds for arm64 like any other
target: the guest's code is native code, the NV2A executor renders on Vulkan,
and SDL2 does window, input and audio. Nothing about the game is emulated
here that is not emulated on the desktop build.

## How the Android build differs

| Piece | Desktop | Android |
|---|---|---|
| Entry | `WinMain` from `main()` | `src/android/sdl_main.c`: SDL2's Java activity calls `SDL_main` |
| main.c | normal `main` | compiled with `-Dmain=ssx3_main`, so `sdl_main.c` owns the entry |
| SDL window | its own thread (`fb_present_sdl.c`) | the SDL thread: the game runs on a worker, the presenter loop runs on the thread SDL owns (`xbox_FramebufferWindowRunHere`) |
| Window | 640x480 resizable | fullscreen, the phone's screen |
| Vulkan | 1.4 core push descriptors | 1.3 + `VK_KHR_push_descriptor`, resolved with `vkGetDeviceProcAddr` |
| D3D8/GL layer | `d3d8_gl.c` (libepoxy) | not built: SSX 3 draws through the NV2A executor |
| Crypto | OpenSSL (desktop Linux) | not needed, `kernel_crypto.c` is self-contained |
| Video | Media Foundation / none | none (the intro movies are skipped anyway) |
| Launcher | `launcher.exe` | none: the game folder is fixed, see below |
| Logs | stdout/stderr | both are piped into logcat, tag `SSX3` |

The renderer is offscreen on every platform (it renders at `RECOMP_RES` and the
presenter blits the frame), which is why the phone only needs a window and no
Vulkan surface.

## Storage layout

```
/sdcard/Android/data/com.douglashalse.ssx3/files/ssx3/
    game/            default.xbe + data/  (your PAL rip, ~2.2 GB)
        UDATA/       saves written by the game
    home/.local/share/xboxrecomp/   system partition images
```

or, when that folder cannot be written (scoped storage blocks adb and file
managers there on retail phones):

```
/sdcard/Download/ssx3/game/default.xbe + data/
```

The app prefers its own folder and falls back to `Download/ssx3` when the XBE
there really opens, which needs the optional "All files access" grant
(Settings > Apps > SSX 3 > Special access > All files access). Either way
`src/android/sdl_main.c` chdir's there and points `HOME` / `XDG_DATA_HOME` at
`home/`, so nothing outside those directories is touched.

## Status (2026-10-23)

Verified on an Android 14 x86_64 emulator (WHPX): installs, launches, loads the
PAL XBE (3,428,352 bytes), maps the guest memory, boots the frontend and draws
the title screen ("Press START"). The emulator's SwiftShader reports Vulkan 1.2,
so the renderer falls back to the software rasteriser there and logs
`[VKR] init failed`; a phone with Vulkan 1.3 + `VK_KHR_push_descriptor` (Adreno
7xx, Mali-G7xx) takes the GPU path. Not yet run on real hardware.


## Build

```bash
# toolchain (once): NDK r27, build-tools 34, platform 34, SDL2 for Android
#   NDK        C:/Android/android-ndk-r27c
#   buildtools C:/Android/android-14
#   platform   C:/Android/sdk/platforms/android-34/android.jar
#   SDL2       C:/Android/sdl-prefix (arm64-v8a) and sdl-prefix-x64 (x86_64)

cmake -S . -B build-android -G Ninja \
  -DCMAKE_MAKE_PROGRAM="<vs>/ninja.exe" \
  -DCMAKE_TOOLCHAIN_FILE=C:/Android/android-ndk-r27c/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=C:/Android/sdl-prefix \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DVulkan_INCLUDE_DIR=<ndk>/sysroot/usr/include \
  -DVulkan_LIBRARY=<ndk>/sysroot/usr/lib/aarch64-linux-android/34/libvulkan.so \
  -DGLSLC=C:/VulkanSDK/1.4.363.0/Bin/glslc.exe
cmake --build build-android --target ssx3_recomp --parallel 16

android/build_apk.sh          # -> build/android/ssx3-debug.apk
```

`x86_64` (for the emulator) is the same with `-DANDROID_ABI=x86_64`,
`-DCMAKE_PREFIX_PATH=C:/Android/sdl-prefix-x64`, the `x86_64-linux-android`
Vulkan stub, and

```bash
ABI=x86_64 NATIVE=build-android-x64/libmain.so \
SDL_LIB=C:/Android/sdl-prefix-x64/lib/libSDL2.so \
OUT=build/android-x64 APK_NAME=ssx3-x64-debug.apk android/build_apk.sh
```

The APK is packaged without Gradle: aapt2, JDK 8 javac, D8 (build-tools 30),
zipalign, apksigner. The Java side is SDL2's own `org.libsdl.app` sources
(`android/java`, copied from the SDL2 tree).

Note for whoever touches the packaging: javac 23's class files make D8 8.2
(build-tools 34) die with an internal NPE in its dex writer, which is why
`build_apk.sh` compiles with the JDK 8 that ships with Unity's Android player.

## Install

```bash
android/install.sh            # adb install + push game/ + launch
```

or by hand:

```bash
adb install -r build/android/ssx3-debug.apk
adb shell mkdir -p /sdcard/Android/data/com.douglashalse.ssx3/files/ssx3
adb push game /sdcard/Android/data/com.douglashalse.ssx3/files/ssx3/
adb shell am start -n com.douglashalse.ssx3/org.libsdl.app.SDLActivity
adb logcat -s SSX3            # the port's own log
```

## Controls

An Xbox/XInput-style pad works (SDL game controller: Bluetooth or USB). There
are no touch controls yet: the game needs a pad (or a keyboard, if one is
attached — `RECOMP_KEYBOARD=1` is set).

## Tuning

The defaults are set in `sdl_main.c`: `RECOMP_RES=720` (internal height),
`RECOMP_WIDESCREEN=auto`, `RECOMP_SKIP_INTRO=1`, `RECOMP_HDTEX=hd-textures`.
`adb shell am start -n ... --es ...` cannot set env vars; change the defaults
and rebuild, or push a wrapper later. A 1080p internal resolution is usually
still 60 FPS on an Adreno 7xx.

## Known gaps

- No touch controls, no launcher (settings are compile-time defaults).
- The HD texture pack is not shipped in the APK (push `hd-textures/` next to
  `game/` and it is picked up).
- Hardware TSO does not exist on Android (`prctl` fails, one warning at boot).
  The guest lock still serialises guest code; the desktop x86 build is the
  reference if an ordering bug ever shows up.
