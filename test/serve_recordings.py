#!/usr/bin/env python3
"""The recordings page: a small web server for the mGBA test videos.

It lists two folders of videos made by test/record_test.sh, each newest
first:
  - kept:    ~/recordings, videos kept for good;
  - scratch: /tmp/mgba-recordings, where every fresh recording lands
             before it is moved to be kept. Cleared on reboot.

Pages:
  /                  every video, as a card: its title, when it was
                     recorded, its size and its file name.
  /r/<name>          one recording's own page (<name> is its file name
                     without .mp4). It looks in the kept folder first,
                     then scratch, so a link made before a video was
                     moved still works.
  /final/<file>.mp4  the video files themselves (kept, then scratch);
  /scratch/<file>.mp4  browsers ask for them in pieces to seek.

Run with no arguments it serves port 8080, as autodev-recordings.service
does. To preview a branch's page, run it in that checkout on a spare port:
  python3 test/serve_recordings.py --port 8081
--print PATH prints what a GET of PATH would return, without opening a
port; test/tests/test_recordings_page.lua reads the page that way.

ONE SELF-CONTAINED FILE: this server imports only Python's standard
library, and nothing else in the repo. Autodev restarts the page when a
merge changes this file (automation/autodev/recordings.py, #107), and this
file is all that check watches: code moved into another file would stay
old until the next restart.
"""

import argparse
import datetime
import html
import http.server
import os
import re
import sys
import urllib.parse

DEFAULT_PORT = 8080
DEFAULT_KEPT = "~/recordings"
DEFAULT_SCRATCH = "/tmp/mgba-recordings"

ISSUE_URL = "https://github.com/MinimalEffort07/pokefirered/issues/{}"

# How much of a video is read from disk and sent at a time. A video is sent
# piece by piece rather than read whole, so the server's memory stays the
# same size whatever the video's size.
CHUNK_BYTES = 64 * 1024

PAGE = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
* {{ box-sizing: border-box; margin: 0; padding: 0; }}
body {{ background: #0d0d0d; color: #e0e0e0; font-family: monospace; padding: 1em; }}
a {{ color: inherit; }}
h1 {{ color: #00ff88; margin-bottom: 1em; font-size: 1.2em; }}
h2 {{ color: #00c0ff; margin-top: 1.5em; margin-bottom: 0.2em; font-size: 1em; }}
.section-note {{ color: #777; font-size: 0.8em; margin-bottom: 1em; }}
.empty {{ color: #555; font-size: 0.85em; margin-bottom: 1em; }}
.back {{ font-size: 0.85em; margin-bottom: 1em; }}
.card {{ background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 6px;
         margin-bottom: 1.5em; padding: 1em; }}
.title {{ color: #00ff88; font-size: 1em; margin-bottom: 0.4em; }}
.facts {{ color: #888; font-size: 0.8em; margin-bottom: 0.8em; }}
video {{ width: 100%; border-radius: 4px; background: #000; }}
</style>
</head>
<body>
{body}
</body>
</html>"""


class Source:
    """One folder of videos: its URL prefix, where it is, and how the page
    describes it."""

    def __init__(self, label, directory, heading, note):
        self.label = label          # the URL prefix of its videos: /<label>/<file>
        self.directory = directory
        self.heading = heading
        self.note = note


def make_sources(kept, scratch):
    """The two folders, kept first. The labels "final" and "scratch" are
    the URL prefixes the page has always used, so old links still work."""
    return [
        Source("final", os.path.expanduser(kept), "Kept for good",
               "Recordings moved here to keep."),
        Source("scratch", os.path.expanduser(scratch),
               "Not kept yet, cleared on reboot",
               "Every fresh recording lands here first."),
    ]


# record_test.sh ends each video's name with the date and time it was run:
# _<YYYYMMDD>_<HHMMSS>, e.g. test_x_20261020_134554.
STAMP = re.compile(r"_\d{8}_\d{6}$")
# A video named for its ticket: issue-<N>-<words> or issue-<N>_<words>.
ISSUE_NAME = re.compile(r"issue-(\d+)[-_](.+)")


def title_of(name):
    """A card's title, from a video's name (its file name without .mp4), as
    (title, issue number or None):
      issue-24-safety-net              -> ("#24 safety net", 24)
      issue-90_test_x_20261020_134554  -> ("#90 test_x", 90)
      test_x_20261020_134554           -> ("test_x", None)
      anything else                    -> (the name, None)
    The time stamp is left out because the card shows the time anyway. In
    an issue name, the words' hyphens show as spaces."""
    base = STAMP.sub("", name) or name
    m = ISSUE_NAME.fullmatch(base)
    if m:
        number = int(m.group(1))
        words = m.group(2).replace("-", " ").strip()
        return (f"#{number} {words}" if words else f"#{number}"), number
    return base, None


class Video:
    """One .mp4 file in a source folder."""

    def __init__(self, source, filename, size, mtime):
        self.source = source
        self.filename = filename    # e.g. test_x_20261020_134554.mp4
        self.name = filename[:-4]   # without .mp4: its page is /r/<name>
        self.size = size
        self.mtime = mtime          # seconds since 1970: when the recording ended
        self.title, self.issue = title_of(self.name)


def list_videos(source):
    """The folder's own .mp4 files (not those in subfolders), newest first.

    A video's modification time is when its recording ended: ffmpeg writes
    the file until the recording stops, and `mv` keeps the time. A folder
    that doesn't exist yet has no videos, and a file that vanishes while
    the folder is read (a recording being moved) is skipped, so no single
    file can break the page."""
    videos = []
    try:
        entries = list(os.scandir(source.directory))
    except OSError:
        return []
    for entry in entries:
        if not entry.name.endswith(".mp4"):
            continue
        try:
            if not entry.is_file():
                continue
            st = entry.stat()
        except OSError:
            continue
        videos.append(Video(source, entry.name, st.st_size, st.st_mtime))
    # Newest first; two videos with the same time go by name, so the order
    # never changes between page loads.
    videos.sort(key=lambda v: (-v.mtime, v.name))
    return videos


def esc(text):
    """`text` made safe to put inside HTML, quotes included."""
    return html.escape(text, quote=True)


def url_part(text):
    """`text` URL-encoded as one piece of a path, so a name holding / ? # %
    or & can't change where a link leads. A name that isn't valid UTF-8
    (os.scandir passes such bytes through as "surrogate escapes") is
    encoded from its raw bytes."""
    return urllib.parse.quote(text.encode("utf-8", "surrogateescape"), safe="")


def format_size(size):
    """A file size in plain words, e.g. "39.9 MB"."""
    if size < 1000:
        return "1 byte" if size == 1 else f"{size} bytes"
    if size < 1000 * 1000:
        return f"{size / 1000:.1f} KB"
    return f"{size / 1e6:.1f} MB"


def format_time(mtime):
    """When a video was recorded, in this machine's local time, to the
    minute: "2026-10-20 15:01"."""
    return datetime.datetime.fromtimestamp(mtime).strftime("%Y-%m-%d %H:%M")


def find_video(sources, filename):
    """The video called `filename` in the first of `sources` that lists it,
    or None. Only a file the folder's own listing holds can match, so a
    name such as "../x.mp4" or "sub/x.mp4" never reaches a file."""
    for source in sources:
        for video in list_videos(source):
            if video.filename == filename:
                return video
    return None


def card(video, page_link=True):
    """One video's card. Its title links to its issue, when it has one, and
    (on the main page) its facts end with a link to its own page."""
    title = esc(video.title)
    if video.issue is not None:
        title = f'<a href="{esc(ISSUE_URL.format(video.issue))}">{title}</a>'
    own_page = (f' · <a href="/r/{url_part(video.name)}">own page</a>'
                if page_link else "")
    return (
        '<div class="card">\n'
        f'<div class="title">{title}</div>\n'
        f'<div class="facts">Recorded {format_time(video.mtime)} · '
        f'{format_size(video.size)} · '
        f'<span class="file">{esc(video.filename)}</span>{own_page}</div>\n'
        f'<video controls preload="metadata">'
        f'<source src="/{video.source.label}/{url_part(video.filename)}" '
        f'type="video/mp4"></video>\n'
        '</div>'
    )


def index_page(sources):
    """The main page: one section per folder, kept first."""
    parts = ["<h1>mGBA Test Recordings</h1>"]
    for source in sources:
        parts.append(
            f'<h2 id="{source.label}">{esc(source.heading)}</h2>\n'
            f'<div class="section-note">{esc(source.note)} '
            f'In {esc(source.directory)}</div>')
        videos = list_videos(source)
        if not videos:
            parts.append('<p class="empty">(no recordings)</p>')
        parts.extend(card(v) for v in videos)
    return PAGE.format(title="mGBA Test Recordings", body="\n".join(parts))


def recording_page(video):
    """/r/<name>: one recording's card and video."""
    body = ('<p class="back"><a href="/">All recordings</a></p>\n'
            + card(video, page_link=False))
    return PAGE.format(title=esc(video.title), body=body)


def not_found_page():
    return PAGE.format(title="Not found",
                       body='<h1>Not found</h1>\n'
                            '<p class="back"><a href="/">All recordings</a></p>')


def unquote(part):
    """Undo url_part: a %-encoded piece of a path back to a file name."""
    return urllib.parse.unquote(part, errors="surrogateescape")


def render(path, sources):
    """The page for a GET of `path`, as (status, HTML), or None when `path`
    isn't a page (it may be a video). Shared by the server and --print.

    The path is split at its slashes before %-decoding, so an encoded slash
    (%2F) stays part of a name and can't climb out of a folder."""
    path = path.split("?", 1)[0]
    if path == "/":
        return 200, index_page(sources)
    if path.startswith("/r/"):
        video = find_video(sources, unquote(path[len("/r/"):]) + ".mp4")
        if video is None:
            return 404, not_found_page()
        return 200, recording_page(video)
    return None


RANGE = re.compile(r"bytes=(\d*)-(\d*)")


def byte_range(header, size):
    """Which bytes of a `size`-byte file a Range header asks for, as
    (status, first, last), both ends counted in:
      no header, or one this server doesn't handle  -> (200, 0, size - 1)
      "bytes=100-199"  bytes 100 to 199             -> (206, 100, 199)
      "bytes=100-"     from byte 100 to the end     -> (206, 100, size - 1)
      "bytes=-500"     the last 500 bytes           -> (206, size - 500, size - 1)
      a range starting at or past the end           -> (416, None, None)
    A header asking for several ranges at once, or one that doesn't parse,
    is ignored, and the whole file goes: HTTP allows that, and browsers
    playing a video ask for one range at a time."""
    whole = (200, 0, size - 1)
    m = RANGE.fullmatch((header or "").strip())
    if m is None or (m.group(1) == "" and m.group(2) == ""):
        return whole
    first, last = m.group(1), m.group(2)
    if first == "":
        # A suffix: the last N bytes.
        count = int(last)
        if count == 0 or size == 0:
            return 416, None, None
        return 206, max(0, size - count), size - 1
    first = int(first)
    if first >= size:
        return 416, None, None
    last = size - 1 if last == "" else int(last)
    if last < first:
        return whole
    return 206, first, min(last, size - 1)


def video_file(path, sources):
    """The file a GET of /<label>/<file>.mp4 asks for, or None."""
    parts = path.split("?", 1)[0].split("/")
    if len(parts) != 3 or parts[0] != "":
        return None
    source = next((s for s in sources if s.label == parts[1]), None)
    if source is None:
        return None
    video = find_video([source], unquote(parts[2]))
    if video is None:
        return None
    return os.path.join(source.directory, video.filename)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        sources = self.server.sources
        page = render(self.path, sources)
        if page is not None:
            status, text = page
            self._send_html(status, text)
            return
        filepath = video_file(self.path, sources)
        if filepath is not None:
            self._send_video(filepath)
            return
        self._send_html(404, not_found_page())

    def _send_video(self, filepath):
        """Send a video, or the part of it the browser asked for.

        A browser playing a video asks for it in parts with a Range header
        ("Range: bytes=1000-"), so that it can start playing before the
        whole file arrives and can jump ahead: a seek is a new request
        starting further in. The answer is 206 Partial Content with just
        those bytes. Without a Range header the whole file goes, as 200.

        Either way the bytes go out CHUNK_BYTES at a time, read from disk
        just before they're sent, so a 40 MB video never sits in memory.
        Each request has its own thread (see main), so while one client
        downloads slowly the others are answered at once."""
        try:
            f = open(filepath, "rb")
        except OSError:
            self._send_html(404, not_found_page())
            return
        with f:
            size = os.fstat(f.fileno()).st_size
            status, start, end = byte_range(self.headers.get("Range"), size)
            if status == 416:
                # The range starts past the end of the file.
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            remaining = end - start + 1
            self.send_response(status)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(remaining))
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            f.seek(start)
            try:
                while remaining > 0:
                    chunk = f.read(min(CHUNK_BYTES, remaining))
                    if not chunk:
                        break       # the file got shorter while being sent
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            except ConnectionError:
                # The browser hung up: it does that to every request it no
                # longer needs, e.g. when the viewer seeks. Nothing to do.
                pass

    def _send_html(self, status, text):
        # A name that isn't valid UTF-8 becomes "?" rather than an error.
        data = text.encode("utf-8", "replace")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass  # suppress per-request noise


def print_page(path, sources):
    """--print: write what a GET of `path` would return to stdout: the
    status code on the first line, then the body. Returns the exit code:
    0 once a response is printed, whatever its status; 2 for a video's
    path, since --print shows pages only."""
    page = render(path, sources)
    if page is None:
        if video_file(path, sources) is not None:
            print(f"--print shows pages (/ and /r/<name>), not videos: {path}",
                  file=sys.stderr)
            return 2
        page = 404, not_found_page()
    status, text = page
    out = sys.stdout.buffer
    out.write(f"{status}\n".encode())
    out.write(text.encode("utf-8", "replace"))
    out.flush()
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Serve the mGBA test recordings as a web page.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"port to serve on (default {DEFAULT_PORT})")
    parser.add_argument("--kept", default=DEFAULT_KEPT,
                        help=f"folder of kept videos (default {DEFAULT_KEPT})")
    parser.add_argument("--scratch", default=DEFAULT_SCRATCH,
                        help=f"folder of fresh videos (default {DEFAULT_SCRATCH})")
    parser.add_argument("--print", dest="print_path", metavar="PATH",
                        help="print what a GET of PATH (/ or /r/<name>) would "
                             "return, status code first, and exit without "
                             "opening a port")
    args = parser.parse_args()
    sources = make_sources(args.kept, args.scratch)

    if args.print_path is not None:
        sys.exit(print_page(args.print_path, sources))

    for source in sources:
        os.makedirs(source.directory, exist_ok=True)
    # ThreadingHTTPServer answers each request in a thread of its own, so a
    # slow download (a phone over Tailscale) never holds up another request:
    # the page, a seek, or a second video. Its threads are "daemon" threads,
    # which don't keep the program running once the server is stopped.
    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    server.sources = sources
    print(f"Serving recordings at http://0.0.0.0:{args.port}")
    for source in sources:
        print(f"  [{source.label}] {source.directory}")
    server.serve_forever()


if __name__ == "__main__":
    main()
