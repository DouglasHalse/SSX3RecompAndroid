#!/usr/bin/env bash
# Where the guest's own time goes, measured on the phone.
#
#   scripts/profile_guest.sh [seconds]
#
# simpleperf samples the app for <seconds> while RECOMP_AUTONAV drives boot to a
# Snow Jam race, then reports the hottest symbols. The .so the app loads is
# stripped, so the report resolves through a symfs mirror of the unstripped
# build-android/libmain.so: recompiled functions and toolkit functions come back
# by name instead of as offsets.
#
# SERIAL=<adb serial>, ADB=<adb path>, SP=<simpleperf for arm64>.
set -u

SECS=${1:-20}
PKG=com.douglashalse.ssx3
ROOT=/sdcard/Android/data/$PKG/files/ssx3
DEV=/data/local/tmp/guestprof
OUT=${OUT:-build/perf}

ADB=${ADB:-adb}
SP=${SP:-$ANDROID_NDK/simpleperf/bin/android/arm64/simpleperf}
SO=${SO:-build-android/libmain.so}
adb_() { if [ -n "${SERIAL:-}" ]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi; }

mkdir -p "$OUT"
[ -f "$SP" ] || { echo "no simpleperf at $SP (set SP=)"; exit 1; }
[ -f "$SO" ] || { echo "no unstripped $SO (build it, and do not rebuild before reporting)"; exit 1; }

apkpath=$(adb_ shell pm path $PKG 2>/dev/null | tr -d '\r' | sed 's/^package://' | head -1)
[ -n "$apkpath" ] || { echo "app is not installed"; exit 1; }
libdev=${apkpath%/base.apk}/lib/arm64
libdir=${libdev%lib/arm64}

adb_ shell mkdir -p $DEV/bin $DEV/symfs$libdev
adb_ push "$SP" $DEV/bin/simpleperf >/dev/null
adb_ shell chmod 755 $DEV/bin/simpleperf
adb_ push "$SO" $DEV/symfs$libdev/libmain.so >/dev/null

cat > "$OUT/cfg-profile.txt" <<EOF
# profile_guest.sh: the cheapest present path, so the sample is the guest's work
RECOMP_RES=480
RECOMP_MSAA=0
RECOMP_WIDESCREEN=auto
RECOMP_SKIP_INTRO=1
RECOMP_FAST_BOOT=4
RECOMP_LOG_LEVEL=warn
RECOMP_AUTONAV=race
EOF
adb_ shell am force-stop $PKG >/dev/null 2>&1
adb_ push "$OUT/cfg-profile.txt" $ROOT/recomp.cfg >/dev/null
adb_ logcat -c >/dev/null 2>&1
adb_ shell am start -n $PKG/org.libsdl.app.SDLActivity >/dev/null 2>&1

for _ in $(seq 1 50); do
    sleep 2
    if adb_ logcat -d -s SSX3:V 2>/dev/null | grep -q "at the briefing"; then break; fi
done
pid=$(adb_ shell pidof $PKG | tr -d '\r' | awk '{print $1}')
[ -n "$pid" ] || { echo "game did not start"; exit 1; }
sleep 6

echo "sampling $pid for $SECS s"
adb_ shell "$DEV/bin/simpleperf record -p $pid -f 999 --duration $SECS -o $DEV/perf.data" >/dev/null 2>&1
adb_ shell "$DEV/bin/simpleperf report -i $DEV/perf.data --symfs $DEV --sort dso,symbol --percent-limit 0.5" > "$OUT/guest-$SECS.txt" 2>&1
adb_ shell am force-stop $PKG >/dev/null 2>&1

sed -n '/Overhead/,$p' "$OUT/guest-$SECS.txt" | head -45
echo "(full report: $OUT/guest-$SECs.txt)"
