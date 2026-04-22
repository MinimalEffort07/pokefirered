#!/bin/bash
# Convenience wrapper to launch the locally-built mgba-qt with
# the correct library path. This version includes our patches
# (screenshot fix, fast-forward scripting API).
#
# Usage: bash test/mgba-qt.sh [ROM_PATH] [TEST_SCRIPT]

MGBA_BUILD="/home/min/projects/minimaleffort/mgba/build"
ROM="${1:-/home/min/projects/minimaleffort/pokefirered/pokefirered.gba}"
TEST_SCRIPT="${2:-}"

SCRIPT_ARG=()
if [ -n "$TEST_SCRIPT" ]; then
    SCRIPT_ARG=(--script "$TEST_SCRIPT")
fi

exec env LD_LIBRARY_PATH="$MGBA_BUILD" \
    "$MGBA_BUILD/qt/mgba-qt" "$ROM" "${SCRIPT_ARG[@]}"
