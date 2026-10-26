#!/usr/bin/env bash
# Changelog for one release, from the previous android-v tag to a ref.
#
#   scripts/changelog.sh <prev_tag> <prev_toolkit_commit> <ref>
#
# Both repositories ship in one APK, so both ranges are read. Commits that only
# touch prose (README, docs, comments) are dropped: a player does not read them
# and they would bury the changes that matter.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

BASE=${1:-}
BASE_TOOLKIT=${2:-}
REF=${3:-HEAD}
TOOLKIT_REF=${4:-HEAD}

# A commit is prose-only if the subject says so or every file it touches is
# documentation.
prose_only() {
    local subject=$1 paths
    shift
    case "$subject" in
        README*|README:*|docs:*|Docs:*|comments:*|Comments:*|\
        "cut commentary"*|"drop commentary"*|"keep only what is not obvious"*)
            return 0 ;;
    esac
    paths=$(git show --pretty=format: --name-only "$@" 2>/dev/null | sed '/^$/d')
    [ -n "$paths" ] || return 1
    if printf '%s\n' "$paths" | grep -qvE '^([^/]*\.(md|txt)|docs/|LICENSE)'; then
        return 1
    fi
    return 0
}

changelog() {   # <directory> <base> <ref> <prefix>
    local dir=$1 base=$2 ref=$3 prefix=$4 range subject c
    [ -d "$dir" ] || return 0
    if [ -n "$base" ]; then
        range="$base..$ref"
    else
        range=$ref
    fi
    # A native git.exe cannot take an MSYS path after -C, so cd instead.
    while IFS='|' read -r c subject; do
        [ -n "$c" ] || continue
        if (cd "$dir" && prose_only "$subject" "$c"); then
            continue
        fi
        printf -- '- %s%s\n' "$prefix" "$subject"
    done < <(cd "$dir" && git log --no-merges --pretty='%h|%s' "$range")
}

[ -n "$BASE" ] || exit 0        # first release: no range to describe

changelog "$ROOT" "$BASE" "$REF" ""
changelog "$ROOT/../xboxrecomp" "$BASE_TOOLKIT" "$TOOLKIT_REF" "toolkit: "
