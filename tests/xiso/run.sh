#!/bin/sh
# Builds the XDVDFS extractor against a synthetic image and checks both layouts
# a player might hand it: a trimmed .xiso and a redump-style image with the
# 405 MiB video partition in front. Needs only a C compiler, python3, diff.
#
#   tests/xiso/run.sh            (from the repo root, or anywhere: paths are
#                                 resolved from this script's own directory)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc=${CC:-cc}
"$cc" -std=gnu11 -O1 -Wall -Wextra -o "$work/xiso_test" \
      "$here/xiso_test.c" "$root/src/android/xiso.c" -I"$root/src/android"

python3 "$here/gen_xiso.py" "$work" >/dev/null

expect() {
    got=$1
    want=$2
    if [ "$got" != "$want" ]; then
        echo "FAIL: expected [$want], got [$got]"
        exit 1
    fi
}

echo "== trimmed image"
probe=$("$work/xiso_test" probe "$work/test.iso")
echo "   $probe"
case $probe in
    *"title=0x4541003C"*"pal=1"*) ;;
    *) echo "FAIL: probe did not recognise the disc"; exit 1 ;;
esac
"$work/xiso_test" extract "$work/test.iso" "$work/out" >/dev/null
diff -r "$work/expected" "$work/out"
if [ -e "$work/out/evil.txt" ]; then
    echo "FAIL: a traversal name escaped the destination"
    exit 1
fi

echo "== redump layout (partition at 0x18300000)"
python3 - "$work" <<'PY'
import sys, os
work = sys.argv[1]
src = open(os.path.join(work, "test.iso"), "rb").read()
with open(os.path.join(work, "test_redump.iso"), "wb") as f:
    f.seek(0x18300000)
    f.write(src)
PY
probe=$("$work/xiso_test" probe "$work/test_redump.iso")
echo "   $probe"
case $probe in
    *"base=0x18300000"*) ;;
    *) echo "FAIL: redump partition origin not found"; exit 1 ;;
esac
"$work/xiso_test" extract "$work/test_redump.iso" "$work/out2" >/dev/null
diff -r "$work/expected" "$work/out2"

echo "== all XDVDFS checks passed"
