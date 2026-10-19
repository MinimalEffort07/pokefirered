#!/bin/bash
###############################################################################
# run_test.sh — Run a Lua test script in mGBA headless mode
###############################################################################
#
# This script launches mGBA in headless mode (no GUI window) with a Lua test
# script. The test drives the emulated GBA, checks game state, and writes
# results to a JSON file. This script reads those results and reports pass/fail.
#
# USAGE:
#   bash test/run_test.sh                          # runs the default test
#   bash test/run_test.sh test/tests/test_seel_character.lua  # runs a specific test
#
# HOW IT WORKS:
#   1. Launches mgba-headless with the pokefirered ROM and the test script
#   2. The Lua script controls the emulator (presses buttons, reads memory)
#   3. When the test finishes, the Lua script writes its results to a JSON
#      file in this run's private output directory (see PER-RUN OUTPUT)
#   4. The Lua script sends SIGTERM to itself for a clean exit
#   5. This script reads the results file and reports pass/fail
#
# PER-RUN OUTPUT:
#   Every run gets its own fresh directory, /tmp/mgba-test.XXXXXX, made with
#   mktemp -d and passed to Lua in the MGBA_TEST_DIR environment variable.
#   (It is made in $TMPDIR instead of /tmp when that variable is set, as
#   most Unix tools do; test/tests/test_run_suite.lua uses this to give a
#   suite run a folder of its own.)
#   Everything a test writes goes there: the results JSON, the exit-code
#   file and screenshots (test/lib/framework.lua's fw.out_path). So any
#   number of tests can run at the same time, from one checkout or from
#   several worktrees of it, without reading each other's results. The
#   directory is kept after the run (its path is printed) so screenshots can
#   be looked at; /tmp is cleared on reboot.
#
#   In-game saves need nothing extra: mgba-headless never loads or writes a
#   .sav file (only mGBA's GUI frontends do), so the game's save memory only
#   exists inside the emulator for one run, survives emu:reset() for
#   Continue tests, and is gone when the run ends.
#
# EXIT CODES:
#   0   = all tests passed
#   1   = one or more tests failed, or the test crashed before writing results
#   124 = the test timed out: it was still running after MGBA_TEST_TIMEOUT
#         seconds (124 is the exit code `timeout` gives in that case)
#
# REQUIREMENTS:
#   - mgba-headless built at /home/min/projects/minimalefforts/mgba/build/
#   - pokefirered.gba built in the project root
#
###############################################################################

set -euo pipefail

# Resolve the project root directory (one level up from this script).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Path to the mGBA headless binary (no GUI, runs as fast as possible).
MGBA_HEADLESS="/home/min/projects/minimaleffort/mgba/build/mgba-headless"

# Path to the compiled ROM.
ROM="$PROJECT_DIR/pokefirered.gba"

# The test script to run. Defaults to the basic player sprite test.
TEST_SCRIPT="${1:-$PROJECT_DIR/test/tests/test_player_sprite.lua}"

# This run's private output directory (see PER-RUN OUTPUT above). The file
# names inside it are the ones every test used directly in /tmp before.
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mgba-test.XXXXXX")"
export MGBA_TEST_DIR="$RUN_DIR"
RESULTS_FILE="$RUN_DIR/mgba-test-results.json"
EXITCODE_FILE="$RUN_DIR/mgba-test-exitcode"
SCREENSHOT_FILE="$RUN_DIR/mgba-test-screenshot.png"

# Longest a test may run, in seconds. Most tests finish in under 15 s; long
# sweeps (e.g. the Mt. Moon property test) raise it:
#   MGBA_TEST_TIMEOUT=900 bash test/run_test.sh test/tests/<long_test>.lua
TIMEOUT_SECONDS="${MGBA_TEST_TIMEOUT:-60}"

# The game's save file. mgba-headless never loads or writes one (see
# PER-RUN OUTPUT), so a pokefirered.sav next to the ROM (e.g. the user's own,
# from the GUI) is never read or changed. In case a test ever asks mGBA to
# load one (emu:autoloadSave()), its save directory still points at this
# run's directory, which starts empty.
SAVE_OPTION="savegamePath=$RUN_DIR"

# --- Prerequisite checks ---

if [ ! -x "$MGBA_HEADLESS" ]; then
    echo "ERROR: mgba-headless not found at $MGBA_HEADLESS"
    echo "Build it with: cd mgba/build && cmake -DBUILD_HEADLESS=ON . && make mgba-headless"
    exit 1
fi

if [ ! -f "$ROM" ]; then
    echo "ERROR: ROM not found at $ROM"
    echo "Build it with: make -j\$(nproc) firered"
    exit 1
fi

if [ ! -f "$TEST_SCRIPT" ]; then
    echo "ERROR: Test script not found: $TEST_SCRIPT"
    exit 1
fi

echo "=== mGBA Test Runner ==="
echo "ROM:    $ROM"
echo "Script: $TEST_SCRIPT"
echo "Output: $RUN_DIR"
echo ""

# --- Run the test ---

# We pipe mGBA's output through grep to show only lines from our test
# framework (prefixed with "Scripting:"). mGBA is very verbose about
# GBA hardware events (DMA, SWI, Serial I/O) which would drown out
# our test output.
#
# `stdbuf -oL` forces mGBA's stdout to be line-buffered instead of
# block-buffered. Without it, the last ~1-2KB of Scripting: output gets
# truncated because mGBA exits (via clean_shutdown's SIGTERM-self) before
# its stdout buffer is flushed. grep --line-buffered ensures the pipeline
# from grep onward is also line-buffered.
#
# TMPDIR="$RUN_DIR": the stdbuf on this machine (uutils coreutils, written
# in Rust) leaves an empty .tmpXXXXXX folder in $TMPDIR every time it runs.
# Pointing TMPDIR at this run's own folder puts that leftover there, so it
# is deleted along with the folder instead of piling up in /tmp.
#
# timeout prevents runaway tests from hanging forever. The Seel test
# takes about 10 seconds in headless mode; the default 60 seconds is a
# generous limit (override with MGBA_TEST_TIMEOUT, see above).
cd "$PROJECT_DIR"
set +e
TMPDIR="$RUN_DIR" stdbuf -oL -eL timeout "$TIMEOUT_SECONDS" "$MGBA_HEADLESS" -C "$SAVE_OPTION" "$ROM" --script "$TEST_SCRIPT" 2>&1 \
    | grep --line-buffered -E "^Scripting:"
MGBA_EXIT=${PIPESTATUS[0]}
set -e

echo ""

# --- Check results ---

# The Lua test framework writes results to a JSON file before exiting.
# If the file doesn't exist, the test crashed or timed out before finishing.
#
# A timeout exits with 124, the code `timeout` itself gives when its time
# limit stops the command, so a caller such as test/run_suite.sh can tell a
# test that ran out of time from one that failed. The code says what
# happened; elapsed time could not, because the PC may sleep mid-run, and
# time asleep shows on the clock but doesn't count towards the limit.
if [ ! -f "$RESULTS_FILE" ]; then
    if [ "$MGBA_EXIT" -eq 124 ]; then
        echo "ERROR: Test timed out after $TIMEOUT_SECONDS seconds"
        exit 124
    fi
    echo "ERROR: Test did not produce results (mGBA exit code: $MGBA_EXIT)"
    exit 1
fi

# Show the results JSON (contains pass count, fail count, and error messages).
echo "--- Results ---"
cat "$RESULTS_FILE"
echo ""

# Show screenshot path if one was taken.
if [ -f "$SCREENSHOT_FILE" ]; then
    echo "Screenshot: $SCREENSHOT_FILE"
fi

# --- Determine exit code ---

# The Lua framework writes the desired exit code (0=pass, 1=fail) to a file
# because we can't control the process exit code from within Lua in mGBA.
if [ -f "$EXITCODE_FILE" ]; then
    TEST_EXIT=$(cat "$EXITCODE_FILE")
    exit "$TEST_EXIT"
fi

# Fallback: parse the JSON for the fail count.
if grep -q '"fail": 0' "$RESULTS_FILE"; then
    exit 0
else
    exit 1
fi
