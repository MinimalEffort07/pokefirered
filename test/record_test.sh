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
###############################################################################
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MGBA_BUILD="/home/min/projects/minimaleffort/mgba/build"
ROM="$PROJECT_DIR/pokefirered.gba"
TEST_SCRIPT="${1:-$PROJECT_DIR/test/tests/test_player_sprite.lua}"
RECORDINGS_DIR="/tmp/mgba-recordings"
RESULTS_FILE="/tmp/mgba-test-results.json"
MGBA_QT_INI="$HOME/.config/mgba/qt.ini"
TIMEOUT=300

if [ ! -f "$ROM" ]; then
    echo "ERROR: ROM not found at $ROM"; exit 1
fi
if [ ! -f "$TEST_SCRIPT" ]; then
    echo "ERROR: Test script not found: $TEST_SCRIPT"; exit 1
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
    Xvfb :1 -screen 0 "${VIRT_W}x${VIRT_H}x24" &
    sleep 1
fi

if ! DISPLAY=:1 pgrep -x openbox > /dev/null 2>&1; then
    DISPLAY=:1 openbox &
    sleep 1
fi

mkdir -p "$RECORDINGS_DIR"

TEST_NAME="$(basename "$TEST_SCRIPT" .lua)"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTPUT="$RECORDINGS_DIR/${TEST_NAME}_${TIMESTAMP}.mp4"

# Kill any stale mGBA so its final screen doesn't bleed into the recording.
pkill -x mgba-qt 2>/dev/null || true
sleep 1

rm -f "$RESULTS_FILE" /tmp/mgba-test-exitcode

# If a .sav or auto-state file sits next to the ROM, the game boots into the
# "CONTINUE" main menu with the cursor on CONTINUE — so the generic "press A
# to select NEW GAME" boot sequence in test/lib/character_select.lua would
# instead load the save and skip character-select entirely, making sentinel
# detection fail. Move them aside for the duration of the recording and
# restore on exit so the user's saved progress is preserved either way.
ROM_SAV="${ROM%.gba}.sav"
ROM_AUTO_STATE="${ROM%.gba}.ss1"
SAV_BACKUP=""
AUTO_STATE_BACKUP=""
if [ -f "$ROM_SAV" ]; then
    SAV_BACKUP="$(mktemp --suffix=.sav)"
    mv "$ROM_SAV" "$SAV_BACKUP"
fi
if [ -f "$ROM_AUTO_STATE" ]; then
    AUTO_STATE_BACKUP="$(mktemp --suffix=.ss1)"
    mv "$ROM_AUTO_STATE" "$AUTO_STATE_BACKUP"
fi
restore_save() {
    [ -n "$SAV_BACKUP" ] && [ -f "$SAV_BACKUP" ] && mv "$SAV_BACKUP" "$ROM_SAV"
    [ -n "$AUTO_STATE_BACKUP" ] && [ -f "$AUTO_STATE_BACKUP" ] && \
        mv "$AUTO_STATE_BACKUP" "$ROM_AUTO_STATE"
}
trap restore_save EXIT

echo "=== mGBA Test Recorder ==="
echo "Test:   $TEST_NAME"
echo "Output: $OUTPUT"
echo ""

# Give mGBA a private XDG_CONFIG_HOME for recording so it reads/writes a
# temp config and never modifies the user's real ~/.config/mgba/. We copy the
# user's config as a base, then set displayDriver=0 (Qt software renderer —
# hardware OpenGL unavailable on Xvfb shows black) in the temp copy.
RECORDING_XDG="/tmp/mgba-recording-xdg"
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
    "$OUTPUT" > /tmp/ffmpeg-record.log 2>&1 &
FFMPEG_PID=$!

# Step 2: launch mGBA with the private config dir.
DISPLAY=:1 XDG_CONFIG_HOME="$RECORDING_XDG" LD_LIBRARY_PATH="$MGBA_BUILD" \
    "$MGBA_BUILD/qt/mgba-qt" "$ROM" --script "$TEST_SCRIPT" \
    > /tmp/mgba-gui.log 2>&1 &
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
