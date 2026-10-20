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
#   bash test/record_test.sh [--notes FILE] [TEST_SCRIPT]
#       Record TEST_SCRIPT (default test/tests/test_player_sprite.lua).
#       --notes FILE gives the recording's notes; FILE may be - for stdin.
#   bash test/record_test.sh --check [--notes FILE] [TEST_SCRIPT]
#       Check the arguments and the notes, print the verdict and the name
#       the video would get, and exit 0 (accepted) or 1 (refused), without
#       recording. Tests use this: a real recording closes every mGBA
#       window on the machine.
#
# NOTES: what a recording is for, in a few plain lines. Which ticket,
# feature (the parent issue) and PR it belongs to, what it tests, what the
# problem was and what you should see:
#
#   ticket: 90 Shaking patches survive a field reload
#   parent: 87 The Poke Radar
#   pr: 97
#
#   ## What's tested
#   ...
#   ## The problem
#   ...
#   ## What you should see
#   ...
#
# The headers are optional; the three sections are required whenever there
# are notes. test/tools/recording_notes.py has the rules in full and checks
# them. The notes are checked before anything else happens (before the
# recording lock), so bad notes cost a second, not a recording. When they
# name a ticket, the video's name starts with it (issue-90_test_x_...).
#
# THE NOTES FILE: every recording, with notes or without, gets <name>.json
# beside <name>.mp4, written by test/tools/recording_notes.py once the video
# is in place. It holds the notes and what this script knows itself: when
# it was recorded, the test, the branch and commit, and the run's result.
# The recordings page (test/serve_recordings.py) shows it on the
# recording's card. The last two lines printed are the command that keeps
# both files, and the recording's own page:
#   mv /tmp/mgba-recordings/<name>.mp4 /tmp/mgba-recordings/<name>.json ~/recordings/
#   http://100.116.114.81:8080/r/<name>
#
# PER-RUN OUTPUT: like test/run_test.sh, every run gets a private directory
# (mktemp -d, passed to Lua as MGBA_TEST_DIR) for the test's results,
# screenshots, the game's save file, the live capture and this script's
# logs. Only the encoded video and its notes file go to the shared
# $RECORDINGS_DIR, under a timestamped name.
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
# CAPTURE FAST, THEN ENCODE SMALL: the screen is captured live with x264's
# fastest settings (preset ultrafast, Constrained Baseline). They keep up
# with the screen even while suites run alongside, so no frame is dropped,
# but the files are big: about 3 Mbps, 40 MB for a 100-second test, too much
# to stream to a phone over Tailscale. So the capture goes into the run's
# own directory, and once it ends, test/tools/encode_recording.sh encodes
# it again, slowly and well (that script has the settings, and why each).
# The kept video is 5 to 8 times smaller and the scripting log reads the
# same. The encode doesn't need the display, so it runs after the lock is
# released, and at low CPU priority: 6 to 11 seconds per 100 seconds of
# video, while the next recording can already start. Once the encode has worked the capture is deleted; if it fails,
# the capture is kept as the video instead, and the script says so. It
# then says what it did, e.g. "captured 39.9 MB, kept 4.9 MB (0.38 Mbps)".
#
###############################################################################
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MGBA_BUILD="/home/min/projects/minimaleffort/mgba/build"
ROM="$PROJECT_DIR/pokefirered.gba"
NOTES_TOOL="$SCRIPT_DIR/tools/recording_notes.py"
RECORDINGS_DIR="/tmp/mgba-recordings"
# Where a recording is kept, and its own page on the recordings page, for
# the last lines this script prints. The ~ is printed as it is, for the
# shell of whoever pastes the line to expand.
KEPT_DIR="~/recordings/"
PAGE_URL="http://100.116.114.81:8080/r"
MGBA_QT_INI="$HOME/.config/mgba/qt.ini"
TIMEOUT=300

# --help prints the USAGE part of the comment at the top of this file.
usage() {
    sed -n '/^# USAGE:/,/^# NOTES:/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

# The arguments, in any order (see USAGE).
NOTES=""
CHECK_ONLY=0
TEST_SCRIPT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --notes)
            if [ $# -lt 2 ] || [ -z "$2" ]; then
                echo "ERROR: --notes needs a notes file, or - for stdin"; exit 1
            fi
            NOTES="$2"
            shift 2 ;;
        --check)
            CHECK_ONLY=1
            shift ;;
        -h|--help)
            usage; exit 0 ;;
        -*)
            echo "ERROR: unknown option: $1 (see --help)"; exit 1 ;;
        *)
            if [ -n "$TEST_SCRIPT" ]; then
                echo "ERROR: give one test, not two: $TEST_SCRIPT and $1"; exit 1
            fi
            TEST_SCRIPT="$1"
            shift ;;
    esac
done
TEST_SCRIPT="${TEST_SCRIPT:-$PROJECT_DIR/test/tests/test_player_sprite.lua}"

if [ ! -f "$ROM" ]; then
    echo "ERROR: ROM not found at $ROM"; exit 1
fi
if [ ! -f "$TEST_SCRIPT" ]; then
    echo "ERROR: Test script not found: $TEST_SCRIPT"; exit 1
fi

# The notes (see NOTES), checked before anything starts. The helper saves a
# copy of accepted notes as it read them: notes on stdin can only be read
# once, and a notes file edited during the recording doesn't change what
# was checked. The copy moves into the run's directory below.
TICKET=""
NOTES_COPY=""
if [ -n "$NOTES" ]; then
    NOTES_COPY="$(mktemp -t record-notes.XXXXXX)"
    trap 'rm -f "$NOTES_COPY"' EXIT
    python3 "$NOTES_TOOL" check --save "$NOTES_COPY" "$NOTES" || exit 1
    TICKET="$(python3 "$NOTES_TOOL" ticket "$NOTES_COPY")" || exit 1
fi

# The video's name: the ticket when the notes name one, the test's name,
# and the date and time the recording started (seconds since 1970 in $1),
# e.g. issue-90_test_x_20261020_134554.
NAME_PREFIX="${TICKET:+issue-${TICKET}_}"
TEST_NAME="$(basename "$TEST_SCRIPT" .lua)"
video_name() {
    printf '%s%s_%s' "$NAME_PREFIX" "$TEST_NAME" "$(date -d "@$1" +%Y%m%d_%H%M%S)"
}

if [ "$CHECK_ONLY" = 1 ]; then
    if [ -z "$NOTES" ]; then
        echo "No notes: the recording's card will say so."
    fi
    echo "OK: would record $TEST_SCRIPT as $RECORDINGS_DIR/$(video_name "$(date +%s)").mp4"
    echo "Nothing recorded (--check)."
    exit 0
fi

RUN_DIR="$(mktemp -d /tmp/mgba-record.XXXXXX)"
export MGBA_TEST_DIR="$RUN_DIR"
RESULTS_FILE="$RUN_DIR/mgba-test-results.json"
if [ -n "$NOTES" ]; then
    mv "$NOTES_COPY" "$RUN_DIR/notes.txt"
    trap - EXIT
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

# When the recording starts: the video's name holds it, and its notes file
# too, with this machine's UTC offset (e.g. 2026-10-20T13:45:54+11:00).
STARTED="$(date +%s)"
NAME="$(video_name "$STARTED")"
RECORDED_AT="$(date -d "@$STARTED" --iso-8601=seconds)"
OUTPUT="$RECORDINGS_DIR/$NAME.mp4"
NOTES_FILE="$RECORDINGS_DIR/$NAME.json"
# The live capture, in the run's own directory until it is encoded into
# $OUTPUT (see CAPTURE FAST, THEN ENCODE SMALL).
CAPTURE="$RUN_DIR/capture.mp4"

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
# These are the fast live-capture settings (see CAPTURE FAST, THEN ENCODE
# SMALL); step 6 encodes the result small.
echo "Recording..."
ffmpeg -f x11grab -r 30 -s "${VIRT_W}x${VIRT_H}" -i :1 \
    -vf "crop=1324:${VIRT_H}:0:0,scale=1280:-2" \
    -c:v libx264 -profile:v baseline -level 3.1 \
    -preset ultrafast -pix_fmt yuv420p \
    -movflags +faststart \
    "$CAPTURE" > "$RUN_DIR/ffmpeg-record.log" 2>&1 9>&- &
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

# The display is no longer needed, so let the next recording start: release
# the machine-wide lock (flock -u) and close its descriptor.
flock -u 9
exec 9>&-

echo ""
if [ -f "$RESULTS_FILE" ]; then
    cat "$RESULTS_FILE"
    echo ""
fi

# Step 6: encode the capture small into $OUTPUT (see CAPTURE FAST, THEN
# ENCODE SMALL). --move deletes the capture once the encode has worked, and
# keeps it as the video if the encode fails; either way the script prints
# what it did.
if [ ! -f "$CAPTURE" ]; then
    echo "ERROR: no video was captured (see $RUN_DIR/ffmpeg-record.log)"
    exit 1
fi
echo "Encoding for streaming (another recording can start meanwhile)..."
bash "$SCRIPT_DIR/tools/encode_recording.sh" --move "$CAPTURE" "$OUTPUT"
if [ ! -f "$OUTPUT" ]; then
    echo "ERROR: the video couldn't be saved in $RECORDINGS_DIR; it is still at $CAPTURE"
    exit 1
fi
echo "Recording saved: $OUTPUT"

# Step 7: the notes file beside the video (see THE NOTES FILE), with or
# without notes. It is written only now that the video is in place, so the
# recordings page never lists notes without their video.
NOTES_ARGS=()
if [ -n "$NOTES" ]; then
    NOTES_ARGS=(--notes "$RUN_DIR/notes.txt")
fi
if ! python3 "$NOTES_TOOL" write "$OUTPUT" --test "$TEST_SCRIPT" \
        --recorded-at "$RECORDED_AT" --results "$RESULTS_FILE" \
        --repo "$PROJECT_DIR" "${NOTES_ARGS[@]}"; then
    echo "ERROR: the notes file couldn't be written; the video is at $OUTPUT"
    exit 1
fi

# The last two lines: the command that keeps both files, and the
# recording's own page (its name URL-encoded, as the page links it).
PAGE_NAME="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$NAME")"
echo ""
echo "To keep it (both files), and to watch it:"
echo "mv $(printf '%q' "$OUTPUT") $(printf '%q' "$NOTES_FILE") $KEPT_DIR"
echo "$PAGE_URL/$PAGE_NAME"
