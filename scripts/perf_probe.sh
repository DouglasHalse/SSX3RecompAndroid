#!/usr/bin/env bash
# Sample what a play session costs the phone: temperature, thermal state, the
# CPU frequencies it settles on, and the frames the game actually showed.
#
#   scripts/perf_probe.sh [seconds] [out-dir]
#
# Run it while the game is in a race. It reads only, so it can sit next to a
# session you are playing; stop it early with Ctrl-C and the files stay.
set -u

SECS=${1:-120}
OUT=${2:-build/perf}
ADB=${ADB:-adb}
SERIAL=${SERIAL:-}

adb_() { if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi; }

mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=$OUT/$STAMP.log
CSV=$OUT/$STAMP.csv

echo "sampling ${SECS}s into $OUT/$STAMP.*"

adb_ logcat -c
( adb_ logcat -s SSX3:V > "$LOG" 2>&1 ) &
LOGPID=$!
trap 'kill $LOGPID 2>/dev/null' EXIT

echo "t,ms,battery_c,thermal_status,cpu_max_mhz,gpu_mhz,fps_line" > "$CSV"
start=$(date +%s)
while :; do
    now=$(date +%s)
    [ $((now - start)) -ge "$SECS" ] && break

    ms=$(adb_ shell dumpsys SurfaceFlinger --latency-clear 2>/dev/null; echo ok)
    bat=$(adb_ shell dumpsys battery 2>/dev/null | tr -d '\r' | awk -F': ' '/temperature/ {print $2/10}')
    therm=$(adb_ shell dumpsys thermalservice 2>/dev/null | tr -d '\r' \
            | awk -F'mStatus=' '/Thermal Status/ {print $2+0; exit}')
    cpu=$(adb_ shell "cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq 2>/dev/null" \
          | tr -d '\r' | sort -n | tail -1)
    cpu=$(( ${cpu:-0} / 1000 ))
    gpu=$(adb_ shell "cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null || cat /sys/class/devfreq/*.mali/cur_freq 2>/dev/null" \
          | tr -d '\r' | head -1)
    gpu=$(( ${gpu:-0} / 1000000 ))
    fps=$(grep -o 'FPS[^ ]* [0-9.]*' "$LOG" 2>/dev/null | tail -1)

    echo "$((now - start)),$ms,$bat,$therm,$cpu,$gpu,$fps" >> "$CSV"
    sleep 5
done

echo
echo "== thermometer summary ($CSV)"
awk -F, 'NR > 1 { if ($3+0 > mx) mx = $3+0; s += $3+0; n++ }
         END { if (n) printf "battery: start %.1f C, max %.1f C, mean %.1f C over %d samples\n",
                          $3+0, mx, s/n, n }' "$CSV"
echo "== GPU frame time ([GPUPROF] lines, if RECOMP_VKR_GPUPROF was set)"
grep -h "GPUPROF" "$LOG" | tail -5
echo "== event performance reports"
grep -h "PERF" "$LOG" | tail -10
echo "== full log: $LOG"
