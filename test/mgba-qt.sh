#!/bin/bash
# Convenience wrapper to launch the locally-built mgba-qt with
# the correct library path. This version includes our patches
# (screenshot fix, fast-forward scripting API).
#
# Usage: bash test/mgba-qt.sh [ROM_PATH]

MGBA_BUILD="/home/min/projects/minimalefforts/mgba/build"
ROM="${1:-/home/min/projects/minimalefforts/pokefirered/pokefirered.gba}"

exec env LD_LIBRARY_PATH="$MGBA_BUILD" "$MGBA_BUILD/qt/mgba-qt" "$ROM"
