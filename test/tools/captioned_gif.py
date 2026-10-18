#!/usr/bin/env python3
"""
captioned_gif.py: turn a test's frame dump into a README GIF with captions.

test/lib/frame_capture.lua saves every second frame of a test run as
frame_NNNNN.png (240 x 160, the GBA screen, 30 frames per second of game
time) plus captions.tsv, one line per frame: "<frame number><TAB><caption>".
This tool makes a GIF of them: each frame scaled up 2x without smoothing
(480 x 320, so the pixel art stays sharp), with a black bar underneath that
shows the caption of that frame in white. Frames with an empty caption get
an empty bar.

USAGE (python3 standard library and ffmpeg; run from anywhere)

    python3 -I test/tools/captioned_gif.py FRAME_DIR OUTPUT.gif [--font FONT.ttf]

e.g., after a demo recorded with MGBA_FRAME_DIR=/tmp/follower_demo:

    python3 -I test/tools/captioned_gif.py /tmp/follower_demo gifs/WalkWithPokemon.gif

The GIF plays at 15 frames per second (every second captured frame), at
real game speed, with one colour palette for the whole GIF made from all
its frames (ffmpeg's palettegen/paletteuse, no dithering), as the other
README GIFs are.

FONT: DejaVu Sans Bold by default (the fonts-dejavu-core package on Debian
and Ubuntu); --font takes any TrueType file. ffmpeg needs its drawtext
filter, which comes with builds linked against libfreetype.
"""

import argparse
import os
import subprocess
import sys
import tempfile

SCALE = 2                 # 240 x 160 -> 480 x 320
GBA_W, GBA_H = 240, 160
BAR_H = 36                # height of the caption bar, in output pixels
FONT_SIZE = 18
DEFAULT_FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def caption_runs(path):
    """[(first frame, last frame, caption)] for each run of equal captions."""
    runs = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            number, _, text = line.partition("\t")
            n = int(number)
            if runs and runs[-1][2] == text and runs[-1][1] == n - 1:
                runs[-1] = (runs[-1][0], n, text)
            else:
                runs.append((n, n, text))
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("frame_dir", help="directory with frame_NNNNN.png and captions.tsv")
    parser.add_argument("output", help="the GIF to write")
    parser.add_argument("--font", default=DEFAULT_FONT, help="TrueType font for the captions")
    args = parser.parse_args()

    captions = os.path.join(args.frame_dir, "captions.tsv")
    if not os.path.exists(captions):
        sys.exit("no captions.tsv in %s" % args.frame_dir)
    if not os.path.exists(args.font):
        sys.exit("font not found: %s (pass --font)" % args.font)

    w, h = GBA_W * SCALE, GBA_H * SCALE
    with tempfile.TemporaryDirectory() as tmp:
        # Scale up, then add the black bar below the picture.
        filters = [
            "scale=%d:%d:flags=neighbor" % (w, h),
            "pad=%d:%d:0:0:black" % (w, h + BAR_H),
        ]
        # One drawtext per caption, shown on its frames only (n is the input
        # frame number, the same number as in captions.tsv). The text goes
        # through a file so quotes and commas in it need no escaping.
        for i, (first, last, text) in enumerate(caption_runs(captions)):
            if not text:
                continue
            text_file = os.path.join(tmp, "caption%d.txt" % i)
            with open(text_file, "w", encoding="utf-8") as f:
                f.write(text)
            filters.append(
                "drawtext=fontfile=%s:textfile=%s:fontsize=%d:fontcolor=white"
                ":x=(w-text_w)/2:y=%d+(%d-text_h)/2:enable='between(n,%d,%d)'"
                % (args.font, text_file, FONT_SIZE, h, BAR_H, first, last))
        # Every second frame (15 fps), one shared palette, no dithering.
        graph = (",".join(filters) + ",fps=15,split[a][b];"
                 "[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=none")
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-framerate", "30",
             "-i", os.path.join(args.frame_dir, "frame_%05d.png"),
             "-filter_complex", graph, args.output],
            check=True)
    print("wrote %s" % args.output)


if __name__ == "__main__":
    main()
