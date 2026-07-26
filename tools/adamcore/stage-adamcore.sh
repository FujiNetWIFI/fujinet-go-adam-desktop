#!/usr/bin/env bash
# Stage the adamcore emulator sources for the desktop build. adamcore is the
# clean-room GPLv3 ADAM/ColecoVision core; see COMPLIANCE.md.
#
# Sources come from the third_party/adamcore submodule, pinned in
# cmake/Dependencies.cmake (override the location with ADAMCORE_SRC=/path to
# build against a working checkout). The build normally calls this itself; run
# it by hand only to refresh the staged tree. The staged tree is git-ignored.
# The system ROMs live in tools/adamcore/roms and are embedded into the
# binaries by the build (tools/adamcore/embed-roms.py).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${ADAMCORE_SRC:-$ROOT/third_party/adamcore}"
GEN="$ROOT/core/adamcore-generated"

if [ ! -f "$SRC/src/machine.c" ]; then
    echo "error: adamcore sources not found at $SRC" >&2
    echo "       run: git submodule update --init third_party/adamcore" >&2
    echo "       (or set ADAMCORE_SRC=/path/to/adamcore)" >&2
    exit 1
fi

echo "Staging adamcore from $SRC"
rm -rf "$GEN"
mkdir -p "$GEN"
cp -r "$SRC/include" "$SRC/src" "$GEN/"
git -C "$SRC" rev-parse HEAD > "$GEN/.source-info" 2>/dev/null || true

echo "adamcore staging complete"
