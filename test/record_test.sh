#!/bin/bash
###############################################################################
# record_test.sh — Run a Lua test in the mGBA GUI and record it to video
###############################################################################
#
# Kills any stale mGBA, starts Xvfb + openbox if needed, forces mGBA to use
# the Qt software renderer (displayDriver=0 in qt.ini) so the game surface
# paints correctly on Xvfb (which has no GPU — OpenGL shows black), then
# positions both windows side-by-side and records. 3 extra seconds are
# captured after the test finishes so the final game state is clearly visible.
#
# USAGE:
#   bash test/record_test.sh [TEST_SCRIPT]
#
# PER-RUN OUTPUT: like test/run_test.sh, every run gets a private directory
# (mktemp -d, passed to Lua as MGBA_TEST_DIR) for the test's results,
# screenshots, the game's save file and this script's logs. Only the video
# goes to the shared $RECORDINGS_DIR, under a timestamped name.
#
# ONE AT A TIME: only one recording can run at a time on a machine, even
# from different worktrees. It records the whole virtual display :1, which
# it restarts if its size is wrong, and it kills every running mgba-qt
# before starting. So the script takes a machine-wide lock first
# ($RECORD_LOCK, see below): a second recording started meanwhile prints
# "Waiting for another recording..." and starts once the first is done.
# Autodev runs ticket sessions side by side, and each may record. Headless
# runs (test/run_test.sh) don't touch :1 or mgba-qt, so they can run
# alongside a recording.
#
###############################################################################
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MGBA_BUILD="/home/min/projects/minimaleffort/mgba/build"
ROM="$PROJECT_DIR/pokefirered.gba"
TEST_SCRIPT="${1:-$PROJECT_DIR/test/tests/test_player_sprite.lua}"
RECORDINGS_DIR="/tmp/mgba-recordings"
RUN_DIR="$(mktemp -d /tmp/mgba-record.XXXXXX)"
export MGBA_TEST_DIR="$RUN_DIR"
RESULTS_FILE="$RUN_DIR/mgba-test-results.json"
MGBA_QT_INI="$HOME/.config/mgba/qt.ini"
TIMEOUT=300

if [ ! -f "$ROM" ]; then
    echo "ERROR: ROM not found at $ROM"; exit 1
fi
if [ ! -f "$TEST_SCRIPT" ]; then
    echo "ERROR: Test script not found: $TEST_SCRIPT"; exit 1
fi

# The machine-wide lock (see ONE AT A TIME above). flock(1) locks the file
# open on descriptor 9 and the lock lasts until every process holding that
# descriptor has exited. Background programs started below inherit open
# descriptors, so each one closes 9 (`9>&-`): Xvfb and openbox outlive this
# script and would otherwise hold the lock forever.
RECORD_LOCK=/tmp/autodev-record.lock
exec 9> "$RECORD_LOCK"
if ! flock -n 9; then
    echo "Waiting for another recording to finish (lock: $RECORD_LOCK)..."
    flock 9
fi

# Virtual display: wide enough for game (480px) + scripting (843px) side by side.
# Recording is cropped to 1324x640 then scaled to 1280-wide output.
VIRT_W=1920
VIRT_H=640

# Restart Xvfb only if dimensions changed or it isn't running
CURRENT_GEOM=$(DISPLAY=:1 xdpyinfo 2>/dev/null | awk '/dimensions/{print $2}')
if [ "$CURRENT_GEOM" != "${VIRT_W}x${VIRT_H}" ]; then
    echo "Starting Xvfb :1 at ${VIRT_W}x${VIRT_H}..."
    pkill -x Xvfb 2>/dev/null || true
    pkill -x openbox 2>/dev/null || true
    sleep 1
    # Xvfb and openbox outlive this script. Detach their stdout/stderr so a
    # caller piping our output (e.g. `record_test.sh ... | tail`) isn't held
    # open forever by these background processes inheriting the pipe.
    Xvfb :1 -screen 0 "${VIRT_W}x${VIRT_H}x24" > /dev/null 2>&1 9>&- &
    sleep 1
fi

if ! DISPLAY=:1 pgrep -x openbox > /dev/null 2>&1; then
    DISPLAY=:1 openbox > /dev/null 2>&1 9>&- &
    sleep 1
fi

mkdir -p "$RECORDINGS_DIR"

TEST_NAME="$(basename "$TEST_SCRIPT" .lua)"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTPUT="$RECORDINGS_DIR/${TEST_NAME}_${TIMESTAMP}.mp4"

# Kill any stale mGBA so its final screen doesn't bleed into the recording.
pkill -x mgba-qt 2>/dev/null || true
sleep 1

# If a .sav or auto-state file sits next to the ROM, the game boots into the
# "CONTINUE" main menu with the cursor on CONTINUE, so the generic "press A
# to select NEW GAME" boot sequence in test/lib/character_select.lua would
# instead load the save and skip character-select entirely, making sentinel
# detection fail. So mGBA keeps the save and save states of this run in the
# run's own directory (-C savegamePath/savestatePath below), which starts
# empty; any files next to the ROM (e.g. the user's own) are never read or
# changed.
SAVE_OPTIONS=(-C "savegamePath=$RUN_DIR" -C "savestatePath=$RUN_DIR")

echo "=== mGBA Test Recorder ==="
echo "Test:   $TEST_NAME"
echo "Output: $OUTPUT"
echo "Run files: $RUN_DIR"
echo ""

# Give mGBA a private XDG_CONFIG_HOME for recording so it reads/writes a
# temp config and never modifies the user's real ~/.config/mgba/. We copy the
# user's config as a base, then set displayDriver=0 (Qt software renderer —
# hardware OpenGL unavailable on Xvfb shows black) in the temp copy.
RECORDING_XDG="$RUN_DIR/xdg"
RECORDING_CONFIG="$RECORDING_XDG/mgba"
mkdir -p "$RECORDING_CONFIG"
cp "$MGBA_QT_INI" "$RECORDING_CONFIG/qt.ini" 2>/dev/null || true
cp "$HOME/.config/mgba/config.ini" "$RECORDING_CONFIG/config.ini" 2>/dev/null || true
python3 - "$RECORDING_CONFIG/qt.ini" <<'PYEOF'
import sys, re
path = sys.argv[1]
try:
    with open(path) as f:
        content = f.read()
except FileNotFoundError:
    content = "[General]\n"
content = re.sub(r'^displayDriver=.*', 'displayDriver=0', content, flags=re.MULTILINE)
if not re.search(r'^displayDriver=', content, re.MULTILINE):
    if re.search(r'^windowPos=', content, re.MULTILINE):
        content = re.sub(r'^(windowPos=.*)', r'\1\ndisplayDriver=0', content, flags=re.MULTILINE)
    else:
        content = re.sub(r'^\[General\]', '[General]\ndisplayDriver=0', content, flags=re.MULTILINE)
with open(path, 'w') as f:
    f.write(content)
PYEOF

export DISPLAY=:1

# Step 1: start recording before mGBA opens so the full boot sequence is captured.
echo "Recording..."
ffmpeg -f x11grab -r 30 -s "${VIRT_W}x${VIRT_H}" -i :1 \
    -vf "crop=1324:${VIRT_H}:0:0,scale=1280:-2" \
    -c:v libx264 -profile:v baseline -level 3.1 \
    -preset ultrafast -pix_fmt yuv420p \
    -movflags +faststart \
    "$OUTPUT" > "$RUN_DIR/ffmpeg-record.log" 2>&1 9>&- &
FFMPEG_PID=$!

# Step 2: launch mGBA with the private config dir.
#
# QT_QPA_PLATFORM=xcb forces Qt onto X11. Under WSL, WSLg exports
# WAYLAND_DISPLAY (e.g. wayland-0), and Qt prefers Wayland when it is set, so
# without this the mGBA window opens on the WSLg desktop instead of Xvfb's
# DISPLAY=:1 and ffmpeg's x11grab records a black screen.
#
# SDL_AUDIODRIVER=dummy gives mGBA SDL's fake sound output, which plays
# nothing and never contacts the host's audio server (PipeWire, PulseAudio,
# ALSA). mGBA paces the game by its sound: each frame it hands over a
# frame's worth of audio and waits until the output has used it up. When
# no real output opens (on 2026-10-20 WSLg's PulseAudio was hung), this
# mgba-qt build never starts its own fallback output, nothing uses up the
# audio, and the game stops before frame 1: the recording shows a blank
# window until the timeout. SDL's dummy output uses audio at real playback
# speed, so the game still runs at 60 fps (and faster under the tests'
# fast-forward, which turns the waiting off). The video loses nothing:
# ffmpeg records only the screen.
DISPLAY=:1 QT_QPA_PLATFORM=xcb SDL_AUDIODRIVER=dummy XDG_CONFIG_HOME="$RECORDING_XDG" LD_LIBRARY_PATH="$MGBA_BUILD" \
    "$MGBA_BUILD/qt/mgba-qt" "${SAVE_OPTIONS[@]}" "$ROM" --script "$TEST_SCRIPT" \
    > "$RUN_DIR/mgba-gui.log" 2>&1 9>&- &
MGBA_PID=$!

# Step 3: identify windows by title and move them into position.
# Qt software renderer (no OpenGL) means windowmove is safe — the widget
# repaints itself after moving without losing its content.
# Game window:     title starts with "mGBA"
# Scripting window: title is exactly "Scripting"
echo "Positioning windows..."
GAME_WID=""
SCRIPT_WID=""
for i in $(seq 1 20); do
    ALL_WIDS=$(xdotool search --pid "$MGBA_PID" 2>/dev/null || true)
    for wid in $ALL_WIDS; do
        NAME=$(xdotool getwindowname "$wid" 2>/dev/null || true)
        if echo "$NAME" | grep -q "^mGBA"; then
            GAME_WID=$wid
        elif [ "$NAME" = "Scripting" ]; then
            SCRIPT_WID=$wid
        fi
    done
    [ -n "$GAME_WID" ] && [ -n "$SCRIPT_WID" ] && break
    sleep 1
done

if [ -n "$GAME_WID" ]; then
    xdotool windowmove "$GAME_WID" 0 0
    echo "Game window -> (0, 0)"
else
    echo "WARNING: game window not found"
fi
if [ -n "$SCRIPT_WID" ]; then
    xdotool windowmove "$SCRIPT_WID" 480 0
    echo "Scripting window -> (480, 0)"
else
    echo "WARNING: scripting window not found"
fi

# Step 4: poll for test completion. The Lua framework restores 60fps rendering
# after fw.finish(), so the game window becomes fully visible.
ELAPSED=0
while [ ! -f "$RESULTS_FILE" ] && kill -0 "$MGBA_PID" 2>/dev/null; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        echo "ERROR: Test timed out after ${TIMEOUT}s"
        break
    fi
done

# Record 3 more seconds of the final game state at normal 60fps speed.
sleep 3

# Step 5: stop recording cleanly (SIGINT lets ffmpeg finalize the MP4).
kill -INT "$FFMPEG_PID" 2>/dev/null
wait "$FFMPEG_PID" 2>/dev/null || true
kill "$MGBA_PID" 2>/dev/null || true

echo ""
if [ -f "$RESULTS_FILE" ]; then
    cat "$RESULTS_FILE"
    echo ""
fi
echo "Recording saved: $OUTPUT"
