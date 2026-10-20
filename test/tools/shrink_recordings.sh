#!/bin/bash
###############################################################################
# shrink_recordings.sh — Shrink the recordings already kept, once
###############################################################################
#
# Since issue #108, test/record_test.sh encodes every recording small enough
# to stream (test/tools/encode_recording.sh has the settings and why). This
# tool does the same for the videos made before that, and for the ones
# recorded by a branch that doesn't have #108 yet.
#
# USAGE:
#   bash test/tools/shrink_recordings.sh [FOLDER]     # default: ~/recordings
#
# WHAT IT DOES, for each .mp4 file directly in FOLDER (not in a folder
# under it):
#   - A video that is already H.264 High profile is left alone: it's either
#     encoded this way already or was made small some other way.
#   - Any other video is encoded with test/tools/encode_recording.sh. The
#     new video keeps the old one's name and modification time, so the
#     recordings page (test/serve_recordings.py), which lists the newest
#     first, keeps its order.
#   - The original is moved into FOLDER/originals/, unchanged. The page lists
#     only FOLDER's own files, so originals don't show there. Put one back
#     with `mv FOLDER/originals/NAME FOLDER/`, or delete them all with
#     `rm -r FOLDER/originals` once you're happy with the new videos.
#   - A video whose encode fails is left as it is, and the tool says so. So
#     is a video whose name is already taken in originals/: an original is
#     never written over.
#
# A second run changes nothing: every video it would touch is High profile
# by then. It ends with the totals, e.g. "Done: 24 shrunk, 16 already High
# profile, 0 not shrunk", and the size of FOLDER's videos before and after.
#
# EXIT CODES:
#   0   every video is now High profile (or there were none)
#   1   at least one video was left as it was (see the messages)
#   2   wrong arguments, or FOLDER isn't a folder
#
###############################################################################
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENCODE="$SCRIPT_DIR/encode_recording.sh"

if [ $# -gt 1 ]; then
    echo "usage: bash test/tools/shrink_recordings.sh [FOLDER]" >&2
    exit 2
fi
FOLDER="${1:-$HOME/recordings}"
if [ ! -d "$FOLDER" ]; then
    echo "shrink_recordings.sh: not a folder: $FOLDER" >&2
    exit 2
fi
ORIGINALS="$FOLDER/originals"

# The H.264 profile of the first video stream of file $1 ("High",
# "Constrained Baseline", ...), or nothing if ffprobe can't read it.
profile_of() {
    ffprobe -v error -select_streams v:0 -show_entries stream=profile \
        -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null | head -n 1
}

# Millions of bytes, to one decimal place.
mb() {
    awk -v bytes="$1" 'BEGIN { printf "%.1f", bytes / 1e6 }'
}

echo "Shrinking the recordings in $FOLDER (originals go to $ORIGINALS)"

SHRUNK=0      # videos encoded and replaced
ALREADY=0     # videos already High profile
LEFT=0        # videos left as they were: the encode failed, or a clash
BEFORE=0      # bytes of FOLDER's videos before this run
AFTER=0       # bytes of FOLDER's videos after it

# nullglob: a folder without .mp4 files gives an empty list instead of the
# pattern itself. The pattern doesn't match hidden files (a leading "."), so
# encode_recording.sh's temporary files are never picked up.
shopt -s nullglob
for VIDEO in "$FOLDER"/*.mp4; do
    # Only plain files: not a folder or a link that happens to end in .mp4.
    if [ ! -f "$VIDEO" ] || [ -L "$VIDEO" ]; then
        continue
    fi
    NAME="$(basename "$VIDEO")"
    BYTES=$(stat -c %s "$VIDEO")
    BEFORE=$((BEFORE + BYTES))

    if [ "$(profile_of "$VIDEO")" = "High" ]; then
        ALREADY=$((ALREADY + 1))
        AFTER=$((AFTER + BYTES))
        continue
    fi
    if [ -e "$ORIGINALS/$NAME" ]; then
        echo "$NAME: left as it was: $ORIGINALS/$NAME already exists, and an original is never written over"
        LEFT=$((LEFT + 1))
        AFTER=$((AFTER + BYTES))
        continue
    fi

    # Encode into a hidden file beside the video (no .mp4 ending, so the
    # recordings page never lists it). Only once that has worked does the
    # original move to originals/ and the new video take its name.
    SHRUNK_FILE="$FOLDER/.shrunk-$NAME.partial"
    if ! RESULT=$(bash "$ENCODE" "$VIDEO" "$SHRUNK_FILE" 2>&1); then
        # ffmpeg's own messages, indented, without encode_recording.sh's
        # last line, which the line above already says.
        echo "$NAME: encode failed, left as it was:"
        printf '%s\n' "$RESULT" | sed '/^encode failed: /d; s/^/    /'
        LEFT=$((LEFT + 1))
        AFTER=$((AFTER + BYTES))
        continue
    fi
    if ! { mkdir -p "$ORIGINALS" && mv -n "$VIDEO" "$ORIGINALS/$NAME" && [ ! -e "$VIDEO" ]; }; then
        rm -f "$SHRUNK_FILE"
        echo "$NAME: left as it was: couldn't move it into $ORIGINALS"
        LEFT=$((LEFT + 1))
        AFTER=$((AFTER + BYTES))
        continue
    fi
    mv "$SHRUNK_FILE" "$VIDEO"
    echo "$NAME: $RESULT"
    SHRUNK=$((SHRUNK + 1))
    AFTER=$((AFTER + $(stat -c %s "$VIDEO")))
done

echo "Done: $SHRUNK shrunk, $ALREADY already High profile, $LEFT not shrunk (see above)."
echo "The videos in $FOLDER: $(mb "$BEFORE") MB before, $(mb "$AFTER") MB after."
if [ "$SHRUNK" -gt 0 ]; then
    echo "The originals are in $ORIGINALS. Put one back with: mv $ORIGINALS/NAME $FOLDER/"
    echo "Delete them all once you're happy with the new videos: rm -r $ORIGINALS"
fi
if [ "$LEFT" -gt 0 ]; then
    exit 1
fi
exit 0
