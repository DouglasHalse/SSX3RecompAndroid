#!/usr/bin/env bash
# The body of a GitHub release: what changed, how to install it, what it was
# built from.
#
#   scripts/release_notes.sh <version> <commit> <version_code> <toolkit_commit> \
#                            <sha256> <prev_tag> <prev_toolkit>
#
# release.sh calls this when cutting a release; the same call regenerates the
# notes of an older tag.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

VERSION=$1
COMMIT=$2
VERSION_CODE=$3
TOOLKIT=$4
SHA=$5
PREV_TAG=${6:-}
PREV_TOOLKIT=${7:-}

cat <<HEAD
SSX 3, statically recompiled to native ARM, running on Android.

### Changes

HEAD

CHANGES=""
if [ -n "$PREV_TAG" ]; then
    CHANGES=$(bash "$ROOT/scripts/changelog.sh" "$PREV_TAG" "$PREV_TOOLKIT" "android-v$VERSION" "$TOOLKIT")
fi
if [ -n "$CHANGES" ]; then
    printf '%s\n' "$CHANGES"
elif [ -n "$PREV_TAG" ]; then
    echo "Bug fixes and internal work; nothing player-visible changed."
else
    echo "First release."
fi

cat <<TAIL

### Install

Download \`ssx3-android-$VERSION.apk\` below and open it on the phone (Android 11 or
newer, arm64). The first time, Android asks whether to allow installs from this app.

Open the app, tap **Select game image (.iso)** and pick your own SSX 3 disc image. The
app checks that it is the PAL disc this port was built from, extracts it into its own
storage with a progress bar (about 2.1 GB), and **Play** appears when that finishes.
No game data is included here, and none is downloaded.

Built from commit $COMMIT (version code $VERSION_CODE), toolkit $TOOLKIT.
sha256 \`$SHA\`
TAIL
