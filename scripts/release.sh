#!/usr/bin/env bash
# Cut a release: build the APK from this tree, tag the commit, publish both.
#
#   scripts/release.sh 0.1.0                 # tag android-v0.1.0, build, publish
#   scripts/release.sh 0.1.0 --notes notes.md
#   scripts/release.sh 0.1.0 --dry-run        # build + print, publish nothing
#
# The APK lands in GitHub's Releases tab, so a player downloads one file and
# never sees a build tool: install it, tap Select game image, pick their own
# disc image, play.
#
# The recompiled game code (src/recomp/gen, 77 files) is not in git, so this
# cannot run in CI: the release is built here, where the disc rip and the regen
# output exist, and published with gh. Everything the build needs is checked
# before anything is tagged, so a failed build never leaves a release behind.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

VERSION=${1:-}
[ -n "$VERSION" ] || { sed -n '2,12p' "$0"; exit 2; }
VERSION=${VERSION#v}
TAG="android-v$VERSION"

NOTES=""
DRY=0
while [ $# -gt 1 ]; do
    case "$2" in
        --notes) NOTES=$3; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        *) echo "unknown argument: $2" >&2; exit 2 ;;
    esac
done

REPO=${REPO:-DouglasHalse/SSX3RecompAndroid}
REMOTE=${REMOTE:-mine}

say() { printf '\n== %s\n' "$*"; }

# ---- refuse to cut a release from a tree that cannot be rebuilt ------------
say "checking the tree"
[ -z "$(git status --porcelain)" ] || {
    echo "working tree is dirty; commit before releasing" >&2
    git status --short >&2
    exit 1
}
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && {
    echo "tag $TAG already exists" >&2
    exit 1
}
[ -d src/recomp/gen ] && [ -n "$(ls -A src/recomp/gen 2>/dev/null)" ] || {
    echo "src/recomp/gen is missing: run scripts/regen.sh first (it needs the disc rip)" >&2
    exit 1
}
# Bump the version in one place and the release notes can quote it.
COMMIT=$(git rev-parse --short HEAD)
VERSION_CODE=$(git rev-list --count HEAD)

say "building the APK for $TAG (commit $COMMIT, version code $VERSION_CODE)"
cmake --build build-android --target ssx3_recomp ssx3import --parallel 16 >/dev/null
RELEASE=1 VERSION_NAME="$VERSION" VERSION_CODE="$VERSION_CODE" COMMIT="$COMMIT" \
    OUT=build/release APK_NAME="ssx3-android-$VERSION.apk" \
    bash android/build_apk.sh | tail -4

APK="build/release/ssx3-android-$VERSION.apk"
[ -f "$APK" ] || { echo "the build did not produce $APK" >&2; exit 1; }
SHA=$(sha256sum "$APK" | cut -d' ' -f1)
SIZE=$(du -h "$APK" | cut -f1)

say "$APK  ($SIZE, sha256 $SHA)"

if [ -n "$NOTES" ]; then
    [ -f "$NOTES" ] || { echo "no such notes file: $NOTES" >&2; exit 1; }
fi

if [ "$DRY" = "1" ]; then
    echo
    echo "dry run: not tagging, not publishing. Would create $TAG on $COMMIT"
    echo "and upload $APK to $REPO."
    exit 0
fi

# ---- tag, publish ---------------------------------------------------------
say "tagging $TAG"
git tag -a "$TAG" -m "SSX 3 Android $VERSION" "$COMMIT"
git push "$REMOTE" "$TAG"

if [ -z "$NOTES" ]; then
    NOTES=$(mktemp)
    cat > "$NOTES" <<RELEASE
SSX 3, statically recompiled to native ARM, running on Android.

**Install:** download \`ssx3-android-$VERSION.apk\` below and open it on the phone
(Android 11 or newer, arm64). Your phone will ask to allow installing from this
app the first time.

**Then:** open the app, tap **Select game image (.iso)**, and pick your own
SSX 3 disc image. The app checks that the image is the PAL disc this port was
built from, extracts it into its own storage with a progress bar (about 2.1 GB),
and **Play** appears when it is done. No game data is included here, and none is
downloaded.

**What is in this build:** everything in the readme — the first-run importer,
the on-screen pad laid out like the GTA IV Android port's (which hides itself
while a real controller is connected), saves, and the touch controls.

Built from commit $COMMIT (version code $VERSION_CODE).
sha256 \`$SHA\`
RELEASE
fi

say "publishing to $REPO"
gh release create "$TAG" "$APK#SSX 3 Android APK (arm64)" \
    --repo "$REPO" \
    --title "SSX 3 Android $VERSION" \
    --notes-file "$NOTES" \
    --latest

say "done"
gh release view "$TAG" --repo "$REPO" --json url,assets \
    -q '.url, (.assets[] | "  " + .name + "  " + (.size|tostring) + " bytes")'
