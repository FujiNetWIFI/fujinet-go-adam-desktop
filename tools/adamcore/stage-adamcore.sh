#!/usr/bin/env bash
# Stage the adamcore emulator sources for the desktop build. adamcore is the
# clean-room GPLv3 ADAM/ColecoVision core; see COMPLIANCE.md.
#
# Sources come from a git checkout of the adamcore repository, pinned by
# SOURCE_COMMIT (override the location with ADAMCORE_SRC=/path). The staged
# tree is git-ignored. System ROMs are NOT staged here: the apps load them at
# runtime from $XDG_DATA_HOME/fujinet-go-adam/roms (dev fallback:
# tools/adamcore/roms, which is git-ignored too).
set -euo pipefail

SOURCE_BRANCH="main"
SOURCE_COMMIT="HEAD"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${ADAMCORE_SRC:-$HOME/Workspace/adamcore}"
GEN="$ROOT/core/adamcore-generated"

if [ ! -f "$SRC/src/machine.c" ]; then
    echo "error: adamcore sources not found at $SRC (set ADAMCORE_SRC=)" >&2
    exit 1
fi

if [ "$SOURCE_COMMIT" != "HEAD" ]; then
    have="$(git -C "$SRC" rev-parse HEAD)"
    if [ "$have" != "$SOURCE_COMMIT" ]; then
        echo "warning: adamcore checkout at $have, pinned $SOURCE_COMMIT" >&2
    fi
fi

echo "Staging adamcore from $SRC"
rm -rf "$GEN"
mkdir -p "$GEN"
cp -r "$SRC/include" "$SRC/src" "$GEN/"
git -C "$SRC" rev-parse HEAD > "$GEN/.source-info" 2>/dev/null || true

echo "adamcore staging complete"
