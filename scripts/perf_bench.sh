#!/usr/bin/env bash
# One measured run of a race, for comparing render settings on a phone.
#
#   scripts/perf_bench.sh <label> <RECOMP_RES> <RECOMP_MSAA> [seconds]
#
# Writes recomp.cfg for the run, starts the game activity directly (the launcher
# would rewrite the file from its own settings), lets RECOMP_AUTONAV drive boot
# to Snow Jam, then samples the race: FPS from the presenter, GPU time from the
# renderer's profiler, battery temperature from dumpsys.
#
# SERIAL=<adb serial> selects the device; default is whatever adb has.
set -u

LABEL=${1:?label}
RES=${2:?render height, 480/720/1080}
MSAA=${3:?msaa samples, 0/2/4/8}
SECS=${4:-75}
PKG=com.douglashalse.ssx3
ROOT=/sdcard/Android/data/$PKG/files/ssx3
OUT=${OUT:-build/perf}

ADB=${ADB:-adb}
adb_() { if [ -n "${SERIAL:-}" ]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi; }

mkdir -p "$OUT"
CFG=$OUT/cfg-$LABEL.txt
LOG=$OUT/log-$LABEL.txt
CSV=$OUT/temp-$LABEL.csv

cat > "$CFG" <<EOF
# perf_bench.sh $LABEL: RECOMP_RES=$RES RECOMP_MSAA=$MSAA
RECOMP_RES=$RES
RECOMP_WIDESCREEN=auto
RECOMP_MSAA=$MSAA
RECOMP_ANISO=16
RECOMP_DRAW_DIST=2
RECOMP_SHADOW_RES=8
RECOMP_SKIP_INTRO=1
RECOMP_FAST_BOOT=4
RECOMP_LOG_LEVEL=info
RECOMP_AUTONAV=race
RECOMP_VKR_GPUPROF=8
RECOMP_FPS_LOG=1
EOF

adb_ shell am force-stop $PKG >/dev/null 2>&1
adb_ push "$CFG" $ROOT/recomp.cfg >/dev/null 2>&1
adb_ logcat -c >/dev/null 2>&1
adb_ shell am start -n $PKG/org.libsdl.app.SDLActivity >/dev/null 2>&1

for _ in $(seq 1 45); do
    sleep 2
    if adb_ logcat -d -s SSX3:V 2>/dev/null | grep -q "at the briefing"; then
        sleep 12
        break
    fi
done

echo "t,battery_c,zone_max_c" > "$CSV"
for i in $(seq 1 $((SECS / 5))); do
    bat=$(adb_ shell dumpsys battery 2>/dev/null | tr -d '\r' | awk -F': ' '/^  temperature/ {print $2/10; exit}')
    z=$(adb_ shell "cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null" 2>/dev/null | tr -d '\r' | sort -n | tail -1)
    echo "$((i * 5)),$bat,$(( ${z:-0} / 1000 ))" >> "$CSV"
    sleep 5
done

adb_ logcat -d -s SSX3:V SSX3PERF:V > "$LOG" 2>/dev/null
adb_ shell am force-stop $PKG >/dev/null 2>&1

echo "== $LABEL (RES=$RES MSAA=$MSAA)"
grep -a "\[FPS\]" "$LOG" | awk '{ s += $NF; n++; v[n] = $NF }
    END { if (!n) { print "no FPS samples"; exit }
          asort(v); printf "FPS: mean %.1f, p10 %.1f, min %.0f, %d samples\n",
                 s/n, v[int(n*0.10)+1], v[1], n }'
grep -a "GPUPROF] 120 frames" "$LOG" | tail -3
awk -F, 'NR > 1 { s += $2; n++; if ($2 > mx) mx = $2 }
         END { if (n) printf "battery: mean %.1f C, max %.1f C\n", s/n, mx }' "$CSV"
echo "log: $LOG"
