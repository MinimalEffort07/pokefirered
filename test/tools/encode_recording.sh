#!/bin/bash
###############################################################################
# encode_recording.sh — Encode one recording small enough to stream
###############################################################################
#
# test/record_test.sh captures the screen live with a fast encoder that
# keeps up with the screen but makes big files (about 3 Mbps, 40 MB for a
# 100-second test). That's too much to stream to a phone over Tailscale. So
# the video it keeps is encoded a second time, slowly and well, by this
# script: 5 to 8 times smaller, with the scripting window's log text as
# readable as before. test/tools/shrink_recordings.sh uses this script too,
# for the videos already kept, so the settings below are the only place the
# encode is set. If videos still stutter over Tailscale, raise -crf below:
# 30 to 32 is the next step, with softer text.
#
# USAGE:
#   bash test/tools/encode_recording.sh IN OUT
#       Encode video IN into OUT. IN is never changed.
#   bash test/tools/encode_recording.sh --move IN OUT
#       Like mv, but encoding on the way: once the encode works, IN is
#       deleted. If it fails, IN is moved to OUT as it is, so the video is
#       never lost. test/record_test.sh uses this for its captures.
#
# Either way, OUT gets IN's modification time, so a video still shows when
# it was recorded, not when it was encoded. On success the script prints one
# line, e.g. "captured 39.9 MB, kept 4.9 MB (0.38 Mbps)" (MB are millions of
# bytes; Mbps is the kept video's average rate, in millions of bits a
# second).
#
# The encode runs at the lowest CPU priority (nice 19): it isn't urgent, and
# test suites running alongside it on this machine shouldn't slow down.
#
# The new video is written to a hidden temporary file beside OUT and only
# renamed to OUT once it is complete, so nobody (the recordings page, a
# person) ever sees a half-written OUT, and a failed encode leaves nothing
# behind.
#
# The encode counts as failed when the new video comes out shorter than IN
# (by more than half a second). That happens when IN is cut short: damaged,
# or still being copied into its folder. ffmpeg then encodes the part it can
# read and still exits 0, which would turn a 65-second video into a
# 25-second one.
#
# EXIT CODES:
#   0   encoded: OUT is the new video
#   1   the encode failed: no OUT, IN as it was (with --move: IN moved to
#       OUT, not encoded), or OUT's folder can't be written (IN stays put)
#   2   wrong arguments, or OUT is a folder
#
###############################################################################
set -uo pipefail

# The encode settings, for a reader new to video encoding. A video is a
# series of pictures. An H.264 encoder stores most of them as what changed
# since the pictures before, and decides how much detail each one keeps.
#
# The size and frame rate are not set, so they stay the input's: 1280 wide
# at 30 frames a second for a recording from test/record_test.sh.
#
# Measured on #99's recording, a 104-second test (issue #103's design
# addendum has the other settings tried; #108's PR has the numbers for a
# second video): the live capture is 39.9 MB, 3.1 Mbps on average and 7.3
# Mbps in its busiest second. These settings give 4.9 MB, 0.38 Mbps on
# average and 1.07 Mbps in the busiest second, in 6 to 11 seconds
# (depending on how busy the machine is), and the scripting log reads the
# same.
ENCODE_SETTINGS=(
    # H.264, the video format every browser and phone plays.
    -c:v libx264

    # High profile. A profile is the set of compression tools a video may
    # use. The live capture uses Constrained Baseline, the smallest set,
    # which suits a fast encoder. High adds the tools that store the same
    # picture in fewer bytes: B-frames (pictures stored as the difference
    # from both the picture before and the one after), CABAC (a tighter way
    # of writing the numbers down) and 8x8 blocks for smooth areas. Every
    # browser and phone of the last decade plays it.
    -profile:v high

    # The preset: how hard the encoder works to find the cheapest way to
    # store each picture. The live capture's "ultrafast" barely tries, which
    # is why it keeps up with the screen and why its files are big.
    # "medium" (x264's default) tries properly: 6 to 11 seconds of work per
    # 100 seconds of video here. Slower presets save only a few percent more.
    -preset medium

    # CRF, the constant rate factor: the quality to keep, from 0 (perfect)
    # to 51 (worst). It is a quality, not a size: a still screen costs
    # almost nothing and a busy one costs more. Each +6 roughly halves the
    # size. The live capture uses the default, 23. At 28 the scripting
    # log's text still reads the same as in the capture (an SSIM of 0.958
    # over the log pane, where 1 means identical); at 32 the video is a
    # third smaller again, but the text is visibly softer.
    -crf 28

    # The peak cap. CRF alone lets a busy stretch (a battle's flashes, a
    # screen of scrolling log) take whatever it needs. -maxrate caps the
    # rate at 1 Mbit a second, averaged over -bufsize (2 Mbit, so about 2
    # seconds of video), so the busiest second stays near 1 Mbps and a
    # phone on a slow link keeps up. It only ever lowers the quality of
    # those busy moments; the rest of the video is as CRF 28 makes it.
    -maxrate 1M -bufsize 2M

    # The pixel format: brightness for every pixel, colour for every 2x2
    # block of pixels. It's the only layout every player handles, and the
    # live capture uses it too.
    -pix_fmt yuv420p

    # faststart: put the index (the "moov" box: where every frame is in the
    # file) at the front instead of the end. A browser then starts playing,
    # and can seek, before it has downloaded the whole file.
    -movflags +faststart
)

usage() {
    echo "usage: bash test/tools/encode_recording.sh [--move] IN OUT" >&2
}

MOVE=0
if [ "${1:-}" = "--move" ]; then
    MOVE=1
    shift
fi
if [ $# -ne 2 ]; then
    usage
    exit 2
fi
IN=$1
OUT=$2

if [ ! -f "$IN" ]; then
    echo "encode_recording.sh: no such file: $IN" >&2
    exit 1
fi
if [ "$IN" -ef "$OUT" ]; then
    echo "encode_recording.sh: IN and OUT are the same file: $IN" >&2
    exit 2
fi
if [ -d "$OUT" ]; then
    echo "encode_recording.sh: OUT must be a file name, not a folder: $OUT" >&2
    exit 2
fi

# Millions of bytes, to one decimal place.
mb() {
    awk -v bytes="$1" 'BEGIN { printf "%.1f", bytes / 1e6 }'
}

# How long video $1 lasts, in seconds (e.g. "103.633333"), or nothing if
# ffprobe can't read it.
duration_of() {
    ffprobe -v error -show_entries format=duration \
        -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null
}

IN_BYTES=$(stat -c %s "$IN")

# The hidden temporary file beside OUT (see above). mktemp makes it with a
# random name, readable only by its owner; it gets IN's permissions once the
# encode is done.
PARTIAL=$(mktemp "$(dirname "$OUT")/.encoding.XXXXXX") || {
    echo "encode_recording.sh: can't write to the folder of $OUT; $IN is left where it was" >&2
    exit 1
}
trap 'rm -f "$PARTIAL"' EXIT

# Encode IN into $PARTIAL, check it, and rename it to OUT. Returns 0 only
# if OUT is now the finished new video.
#
# -nostdin: never read the terminal (ffmpeg otherwise takes keys from it).
# -loglevel error: print only what went wrong. -y: write over $PARTIAL,
# which mktemp has just made empty. -f mp4: the format, which ffmpeg would
# otherwise guess from the file name's ending.
encode() {
    nice -n 19 ffmpeg -nostdin -hide_banner -loglevel error -y -i "$IN" \
        "${ENCODE_SETTINGS[@]}" -f mp4 "$PARTIAL" || return 1

    # As long as IN, give or take half a second (see above).
    local want got
    want=$(duration_of "$IN")
    got=$(duration_of "$PARTIAL")
    if ! awk -v want="${want:-0}" -v got="${got:-0}" \
            'BEGIN { exit !(got + 0 > 0 && got + 0.5 >= want + 0) }'; then
        echo "encode_recording.sh: the new video lasts ${got:-?} s but $IN lasts ${want:-?} s: is it damaged, or still being copied?" >&2
        return 1
    fi

    chmod --reference="$IN" "$PARTIAL" &&
        touch -r "$IN" "$PARTIAL" &&
        mv -f "$PARTIAL" "$OUT"
}

if encode; then
    if [ "$MOVE" -eq 1 ]; then
        rm -f "$IN"
    fi
    OUT_BYTES=$(stat -c %s "$OUT")
    awk -v a="$IN_BYTES" -v b="$OUT_BYTES" -v s="$(duration_of "$OUT")" 'BEGIN {
        printf "captured %.1f MB, kept %.1f MB", a / 1e6, b / 1e6
        if (s + 0 > 0) printf " (%.2f Mbps)", b * 8 / s / 1e6
        printf "\n"
    }'
    exit 0
fi

if [ "$MOVE" -eq 1 ]; then
    if mv -f "$IN" "$OUT"; then
        echo "encode failed, so the video is kept as it was captured: $OUT ($(mb "$IN_BYTES") MB)" >&2
    else
        echo "encode failed, and moving the capture failed too: it is still at $IN" >&2
    fi
else
    echo "encode failed: $IN is left as it was" >&2
fi
exit 1
