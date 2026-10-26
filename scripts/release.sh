#!/usr/bin/env bash
# Cut a release: build the APK from this tree, tag the commit, publish both.
#
#   scripts/release.sh 0.1.0                 # tag android-v0.1.0, build, publish
#   scripts/release.sh 0.1.0 --notes notes.md
#   scripts/release.sh 0.1.0 --dry-run        # build + print, publish nothing
#
# The release body is a changelog of both repositories since the previous tag,
# an install note, and the build stamp; scripts/release_notes.sh writes it.
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
TOOLKIT_COMMIT=$( (cd "$ROOT/../xboxrecomp" && git rev-parse --short HEAD) 2>/dev/null || echo unknown )
VERSION_CODE=$(git rev-list --count HEAD)

# The changelog reads from the previous release: this repo since its tag, the
# toolkit since the commit that release was built with. The tag annotation
# carries that commit; tags cut before it did not, so their release body does.
BASE_TAG=$(git describe --tags --abbrev=0 --match 'android-v*' HEAD 2>/dev/null || true)
BASE_TOOLKIT=""
if [ -n "$BASE_TAG" ]; then
    BASE_TOOLKIT=$(git tag -l --format='%(contents)' "$BASE_TAG" \
        | sed -n 's/.*toolkit \([0-9a-f]\{7,40\}\).*/\1/p' | head -1)
    if [ -z "$BASE_TOOLKIT" ]; then
        BASE_TOOLKIT=$(gh release view "$BASE_TAG" --repo "$REPO" --json body -q .body 2>/dev/null \
            | sed -n 's/.*toolkit \([0-9a-f]\{7,40\}\).*/\1/p' | head -1)
    fi
fi

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
    say "changes since ${BASE_TAG:-the first release} (toolkit ${BASE_TOOLKIT:-none})"
    bash scripts/changelog.sh "$BASE_TAG" "$BASE_TOOLKIT" "$COMMIT" "$TOOLKIT_COMMIT"
    echo
    echo "dry run: not tagging, not publishing. Would create $TAG on $COMMIT"
    echo "and upload $APK to $REPO."
    exit 0
fi

# ---- tag, publish ---------------------------------------------------------
say "tagging $TAG"
git tag -a "$TAG" -m "SSX 3 Android $VERSION (toolkit $TOOLKIT_COMMIT)" "$COMMIT"
git push "$REMOTE" "$TAG"

if [ -z "$NOTES" ]; then
    # A native gh.exe cannot read an MSYS /tmp path, so the generated notes
    # file lives in the build tree, beside the APK it describes.
    NOTES="build/release/RELEASE_NOTES_$VERSION.md"
    mkdir -p "$(dirname "$NOTES")"
    bash scripts/release_notes.sh "$VERSION" "$COMMIT" "$VERSION_CODE" \
        "$TOOLKIT_COMMIT" "$SHA" "$BASE_TAG" "$BASE_TOOLKIT" > "$NOTES"
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
