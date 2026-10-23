#!/usr/bin/env bash
# Install the APK on a connected device, push the game files and start it.
#
#   android/install.sh [apk]        # default build/android/ssx3-debug.apk
#
# The device needs USB debugging on. The game files go to the app's own
# external directory (no storage permission needed, nothing else is touched).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

ADB=${ADB:-"C:/Program Files/Unity/Hub/Editor/2019.4.2f1/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/platform-tools/adb.exe"}
APK=${1:-build/android/ssx3-debug.apk}
PKG=com.douglashalse.ssx3
DIR=/sdcard/Android/data/$PKG/files/ssx3

[ -f "$APK" ] || { echo "no $APK" >&2; exit 1; }
[ -d game/data ] || { echo "game/data missing (the PAL rip must be extracted)" >&2; exit 1; }

"$ADB" devices
"$ADB" install -r "$APK"
"$ADB" shell mkdir -p "$DIR"
echo "== pushing game/ ($(du -sh game | cut -f1))"
"$ADB" push game "$DIR/"
"$ADB" shell am start -n "$PKG/org.libsdl.app.SDLActivity"
echo
echo "logs:   \"$ADB\" logcat -s SSX3"
echo "screen: \"$ADB\" exec-out screencap -p > phone.png"
