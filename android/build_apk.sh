#!/usr/bin/env bash
# Build the SSX 3 Android APK from the native build in build-android/.
#
#   android/build_apk.sh            # debug-signed APK, ready to install
#
# Needs: the NDK build of libmain.so (build-android/), libSDL2.so from the
# SDL2 Android build, a JDK, and the Android build-tools + platform jars.
# Everything is packaged without Gradle: aapt2, javac, d8, zipalign, apksigner.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BT=${BT:-C:/Android/android-14}                     # build-tools 34
PLATFORM=${PLATFORM:-C:/Android/sdk/platforms/android-34/android.jar}
ABI=${ABI:-arm64-v8a}
SDL_LIB=${SDL_LIB:-C:/Android/sdl-prefix/lib/libSDL2.so}
OUT=${OUT:-build/android}
APK_NAME=${APK_NAME:-ssx3-debug.apk}
PKG=com.douglashalse.ssx3

NATIVE=${NATIVE:-build-android/libmain.so}
[ -f "$NATIVE" ] || { echo "missing $NATIVE: run the NDK build first" >&2; exit 1; }
[ -f "$SDL_LIB" ] || { echo "missing $SDL_LIB" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/dex" "$OUT/lib/$ABI"

echo "== resources"
"$BT/aapt2.exe" compile --dir android/res -o "$OUT/res/resources.zip"

echo "== manifest + resources -> base.apk"
"$BT/aapt2.exe" link -o "$OUT/base.apk" \
    -I "$PLATFORM" \
    --manifest android/AndroidManifest.xml \
    --java "$OUT/gen" \
    --min-sdk-version 30 --target-sdk-version 34 \
    "$OUT/res/resources.zip"

echo "== java"
# javac 23's class files make D8 (build-tools 34) die with an internal NPE in
# its dex writer, so the Java side is compiled with a JDK 8 and dexed by the
# D8 shipped with build-tools 30. JDK8 / D8_JAR can point elsewhere.
JDK8=${JDK8:-C:/Program Files/Unity/Hub/Editor/2019.4.2f1/Editor/Data/PlaybackEngines/AndroidPlayer/OpenJDK}
D8_JAR=${D8_JAR:-C:/Program Files/Unity/Hub/Editor/2019.4.2f1/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/build-tools/30.0.2/lib/d8.jar}
"$JDK8/bin/javac.exe" -nowarn -encoding UTF-8 -source 8 -target 8 \
    -classpath "$PLATFORM" -d "$OUT/classes" \
    $(find android/java "$OUT/gen" -name '*.java')

echo "== dex"
OUT_ABS=$(cd "$OUT" && pwd -W)   # C:/... : d8 rejects MSYS /c/... paths
"$JDK8/bin/java.exe" -cp "$D8_JAR" com.android.tools.r8.D8 --min-api 30 \
    --lib "$PLATFORM" --output "$OUT_ABS/dex" $(find "$OUT/classes" -name '*.class')

echo "== package"
cp "$NATIVE"  "$OUT/lib/$ABI/libmain.so"
cp "$SDL_LIB" "$OUT/lib/$ABI/libSDL2.so"
STRIP=${STRIP:-C:/Android/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe}
[ -x "$STRIP" ] && "$STRIP" --strip-unneeded "$OUT/lib/$ABI/libmain.so" \
                          "$OUT/lib/$ABI/libSDL2.so"
python - "$OUT/base.apk" "$OUT/dex/classes.dex" \
    "$OUT/lib/$ABI/libmain.so" "$OUT/lib/$ABI/libSDL2.so" "$ABI" <<'PY'
import sys, zipfile
apk, dex, libmain, libsdl, abi = sys.argv[1:6]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
    z.write(dex, "classes.dex")
    z.write(libmain, "lib/%s/libmain.so" % abi)
    z.write(libsdl, "lib/%s/libSDL2.so" % abi)
PY

echo "== align + sign"
KS=android/ssx3-debug.keystore
if [ ! -f "$KS" ]; then
    "$JDK8/bin/keytool.exe" -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias ssx3 -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=SSX 3 Android port, O=local"
    echo "   created $KS"
fi
"$BT/zipalign.exe" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner.bat" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled false --v2-signing-enabled true \
    --out "$OUT/$APK_NAME" "$OUT/aligned.apk"
"$BT/apksigner.bat" verify --print-certs "$OUT/$APK_NAME" | head -3

ls -la "$OUT/$APK_NAME"
echo
echo "install:  adb install -r $OUT/$APK_NAME"
echo "data:     adb push game <device>:/sdcard/Android/data/$PKG/files/ssx3/"
