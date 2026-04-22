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
#   3. When the test finishes, the Lua script writes results to /tmp/mgba-test-results.json
#   4. The Lua script sends SIGTERM to itself for a clean exit
#   5. This script reads the results file and reports pass/fail
#
# EXIT CODES:
#   0 = all tests passed
#   1 = one or more tests failed, or the test timed out
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

# Where the Lua test framework writes results.
RESULTS_FILE="/tmp/mgba-test-results.json"
EXITCODE_FILE="/tmp/mgba-test-exitcode"

# Clean up previous test results so we don't accidentally read stale data.
rm -f "$RESULTS_FILE" "$EXITCODE_FILE" /tmp/mgba-test-screenshot.png

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
# timeout prevents runaway tests from hanging forever. The Seel test
# takes about 10 seconds in headless mode. 60 seconds is a generous limit.
cd "$PROJECT_DIR"
set +e
stdbuf -oL -eL timeout 60 "$MGBA_HEADLESS" "$ROM" --script "$TEST_SCRIPT" 2>&1 \
    | grep --line-buffered -E "^Scripting:"
MGBA_EXIT=${PIPESTATUS[0]}
set -e

echo ""

# --- Check results ---

# The Lua test framework writes results to a JSON file before exiting.
# If the file doesn't exist, the test crashed or timed out before finishing.
if [ ! -f "$RESULTS_FILE" ]; then
    if [ "$MGBA_EXIT" -eq 124 ]; then
        echo "ERROR: Test timed out after 60 seconds"
    else
        echo "ERROR: Test did not produce results (mGBA exit code: $MGBA_EXIT)"
    fi
    exit 1
fi

# Show the results JSON (contains pass count, fail count, and error messages).
echo "--- Results ---"
cat "$RESULTS_FILE"
echo ""

# Show screenshot path if one was taken.
if [ -f "/tmp/mgba-test-screenshot.png" ]; then
    echo "Screenshot: /tmp/mgba-test-screenshot.png"
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
