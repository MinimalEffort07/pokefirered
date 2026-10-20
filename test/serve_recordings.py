#!/usr/bin/env python3
"""The recordings page: a small web server for the mGBA test videos.

It lists two folders of videos made by test/record_test.sh, each newest
first:
  - kept:    ~/recordings, videos kept for good;
  - scratch: /tmp/mgba-recordings, where every fresh recording lands
             before it is moved to be kept. Cleared on reboot.

Pages:
  /                  every video, as a card: its title, its notes, when it
                     was recorded, its size and its file name.
  /r/<name>          one recording's own page (<name> is its file name
                     without .mp4). It looks in the kept folder first,
                     then scratch, so a link made before a video was
                     moved still works.
  /final/<file>.mp4    a kept video file, and
  /scratch/<file>.mp4  a scratch one. Browsers ask for them in pieces,
                       to start playing early and to seek.

NOTES: record_test.sh writes <name>.json beside each <name>.mp4, the
video's notes file (test/tools/recording_notes.py has its format). It
holds the ticket, the parent issue and the PR the recording belongs to,
three short notes (what's tested, the problem, what you should see) and
what the recorder knew itself: when, the test, the branch and commit, and
the run's result. A card shows all of it. A video without a notes file
(one recorded before notes existed) gets a card made from its file name.
A notes file that can't be read or used gets that card too, saying so: no
single file can break the page.

`recording_notes.py add-notes` gives such an old video notes later. Its
notes file has null for what nobody knows any more (the branch, commit
and result) and a notes_added_at time, so its card says "notes added
<date>" (this machine's time, to the minute) where a result would be, and
makes no claim about how the run ended. Its recorded_at is the time in
the video's name, else the video's modification time, so the card stays
where it was on the page.

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
import json
import os
import re
import sys
import urllib.parse

DEFAULT_PORT = 8080
DEFAULT_KEPT = "~/recordings"
DEFAULT_SCRATCH = "/tmp/mgba-recordings"

ISSUE_URL = "https://github.com/MinimalEffort07/pokefirered/issues/{}"
PULL_URL = "https://github.com/MinimalEffort07/pokefirered/pull/{}"

# A notes file is a few hundred bytes. One bigger than this isn't read, so
# a stray huge file can't slow the page down.
NOTES_LIMIT = 1000 * 1000

# A recording's three notes: their key in the notes file, and the heading
# the card shows them under, in the card's order.
NOTE_SECTIONS = [
    ("tested", "What's tested"),
    ("problem", "The problem"),
    ("expect", "What you should see"),
]

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
.links {{ color: #bbb; font-size: 0.85em; margin-bottom: 0.4em; }}
.facts {{ color: #888; font-size: 0.8em; margin-bottom: 0.8em; }}
.no-notes {{ color: #777; font-size: 0.8em; margin-bottom: 0.8em; }}
video {{ width: 100%; border-radius: 4px; background: #000; }}
/* The notes sit above the video on a phone, and beside it on a wide
   screen: a grid of one column, or of two (notes, then video). */
.main {{ display: grid; gap: 1em; }}
@media (min-width: 900px) {{
  .main {{ grid-template-columns: minmax(0, 2fr) minmax(0, 3fr); align-items: start; }}
}}
.notes h3 {{ color: #00c0ff; font-size: 0.85em; margin: 0.9em 0 0.2em; }}
.notes h3:first-child {{ margin-top: 0; }}
/* pre-wrap keeps the notes' own line breaks, and still wraps long lines. */
.notes p {{ white-space: pre-wrap; font-size: 0.85em; line-height: 1.4; }}
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


class NotesUnreadable(Exception):
    """A notes file is there but can't be used: it can't be read, isn't
    JSON, is too big, or holds a field of the wrong kind."""


def parse_time(text):
    """A notes file's time (recorded_at or notes_added_at), e.g.
    "2026-10-20T13:45:54+11:00", as seconds since 1970. A time without a
    UTC offset is taken as this machine's. The time must also be one this
    machine can show (year 1 can't, in Sydney)."""
    if not isinstance(text, str):
        raise NotesUnreadable
    try:
        seconds = datetime.datetime.fromisoformat(text).timestamp()
        format_time(seconds)
    except (ValueError, OverflowError, OSError):
        raise NotesUnreadable
    return seconds


def optional(data, key, kind):
    """data[key] when it is a `kind`, None when it is null or missing."""
    value = data.get(key)
    if value is None:
        return None
    # JSON's true and false are Python bools, which also count as ints.
    if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
        raise NotesUnreadable
    return value


def whole_number(value, least):
    """`value` when it is a whole number of at least `least`."""
    if not isinstance(value, int) or isinstance(value, bool) or value < least:
        raise NotesUnreadable
    return value


def read_notes_file(path):
    """The notes file at `path`, checked, as a dict (a video's "record")
    with every field of the format (None where the file has null) plus
    "recorded", recorded_at in seconds since 1970, and "notes_added",
    notes_added_at in seconds (None when not set). Its "notes" are the
    three notes themselves. None when there is no such file. Raises
    NotesUnreadable when the file is there but can't be used."""
    try:
        with open(path, "rb") as f:
            raw = f.read(NOTES_LIMIT + 1)
    except FileNotFoundError:
        return None
    except OSError:
        raise NotesUnreadable
    if len(raw) > NOTES_LIMIT:
        raise NotesUnreadable
    try:
        data = json.loads(raw.decode("utf-8"))
    except (ValueError, RecursionError):    # UnicodeDecodeError is a ValueError
        raise NotesUnreadable
    if not isinstance(data, dict):
        raise NotesUnreadable

    record = {"recorded": parse_time(data.get("recorded_at"))}
    for key in ("test", "branch", "commit"):
        record[key] = optional(data, key, str)
    record["uncommitted_changes"] = optional(data, "uncommitted_changes", bool)
    record["result"] = optional(data, "result", dict)
    if record["result"] is not None:
        record["result"] = {key: whole_number(record["result"].get(key), 0)
                            for key in ("pass", "fail")}
    for key in ("ticket", "parent"):
        issue = optional(data, key, dict)
        if issue is not None:
            title = issue.get("title")
            if not isinstance(title, str):
                raise NotesUnreadable
            issue = {"number": whole_number(issue.get("number"), 1), "title": title}
        record[key] = issue
    pr = data.get("pr")
    record["pr"] = None if pr is None else whole_number(pr, 1)
    record["notes"] = optional(data, "notes", dict)
    if record["notes"] is not None:
        for key, _ in NOTE_SECTIONS:
            if not isinstance(record["notes"].get(key), str):
                raise NotesUnreadable
    # Set only by `recording_notes.py add-notes`, for a recording kept
    # before notes existed (see NOTES).
    added = data.get("notes_added_at")
    record["notes_added"] = None if added is None else parse_time(added)
    return record


class Video:
    """One .mp4 file in a source folder, and its notes file."""

    def __init__(self, source, filename, size, mtime, record=None,
                 notes_unreadable=False):
        self.source = source
        self.filename = filename    # e.g. test_x_20261020_134554.mp4
        self.name = filename[:-4]   # without .mp4: its page is /r/<name>
        self.size = size
        self.mtime = mtime          # seconds since 1970: when the recording ended
        self.record = record        # its notes file (read_notes_file), or None
        self.notes_unreadable = notes_unreadable    # a notes file it can't use
        # When it was recorded: its notes file says, else its file's time.
        self.recorded = record["recorded"] if record else mtime
        self.title, self.title_link = self._title()

    def _title(self):
        """The card's title and the link it carries (None for none): the
        ticket the notes name, else the test's name; with no notes file,
        the title made from the file name (title_of)."""
        record = self.record
        if record is not None and record["ticket"] is not None:
            number = record["ticket"]["number"]
            return f'#{number} {record["ticket"]["title"]}', ISSUE_URL.format(number)
        if record is not None and record["test"]:
            test = os.path.basename(record["test"])
            if test.endswith(".lua"):
                test = test[:-len(".lua")]
            if test:
                return test, None
        title, issue = title_of(self.name)
        return title, (ISSUE_URL.format(issue) if issue is not None else None)


def list_videos(source, with_notes=True):
    """The folder's own .mp4 files (not those in subfolders), newest first,
    each with its notes file read (with_notes=False skips that, when only
    the file is wanted).

    "Newest" goes by when a video was recorded: the time in its notes
    file, or, for a video without one, its modification time. That is when
    its recording ended: ffmpeg writes the file until the recording stops,
    and `mv` keeps the time. A folder that doesn't exist yet has no videos,
    and a file that vanishes while the folder is read (a recording being
    moved) is skipped, so no single file can break the page. A notes file
    without its video isn't listed: only .mp4 files are."""
    videos = []
    try:
        entries = list(os.scandir(source.directory))
    except OSError:
        return []
    for entry in entries:
        # A file called just ".mp4" has no name to give its page, /r/<name>.
        if not entry.name.endswith(".mp4") or entry.name == ".mp4":
            continue
        try:
            if not entry.is_file():
                continue
            st = entry.stat()
        except OSError:
            continue
        record, unreadable = None, False
        if with_notes:
            try:
                record = read_notes_file(os.path.join(source.directory,
                                                      entry.name[:-4] + ".json"))
            except NotesUnreadable:
                unreadable = True
        videos.append(Video(source, entry.name, st.st_size, st.st_mtime,
                            record, unreadable))
    # Newest first; two videos with the same time go by name, so the order
    # never changes between page loads.
    videos.sort(key=lambda v: (-v.recorded, v.name))
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
    """A file size in plain words, e.g. "39.9 MB". The unit goes by the
    rounded number, so 999999 bytes is "1.0 MB", not "1000.0 KB"."""
    if size < 1000:
        return "1 byte" if size == 1 else f"{size} bytes"
    kb = f"{size / 1000:.1f}"
    if float(kb) < 1000:
        return f"{kb} KB"
    return f"{size / 1e6:.1f} MB"


def format_time(mtime):
    """When a video was recorded, in this machine's local time, to the
    minute: "2026-10-20 15:01"."""
    return datetime.datetime.fromtimestamp(mtime).strftime("%Y-%m-%d %H:%M")


def find_video(sources, filename, with_notes=True):
    """The video called `filename` in the first of `sources` that lists it,
    or None. Only a file the folder's own listing holds can match, so a
    name such as "../x.mp4" or "sub/x.mp4" never reaches a file."""
    for source in sources:
        for video in list_videos(source, with_notes):
            if video.filename == filename:
                return video
    return None


def result_text(result):
    """The run's result in plain words, from a notes file's "result"."""
    if result is None:
        return "no result: the test didn't finish"
    passed, failed = result["pass"], result["fail"]
    total = passed + failed
    if total == 0:
        return "no checks: a demo or probe"
    checks = "check" if total == 1 else "checks"
    if failed:
        return f"{failed} of {total} {checks} failed"
    return f"{passed} of {total} {checks} passed"


def source_text(record):
    """Where the test was run from, e.g. "branch issue-90-x at cae067302,
    with uncommitted changes", or "" when the notes file doesn't know."""
    branch, commit = record["branch"], record["commit"]
    if branch and commit:
        text = f"branch {branch} at {commit}"
    elif commit:
        text = f"commit {commit}"
    elif branch:
        text = f"branch {branch}"
    else:
        return ""
    if record["uncommitted_changes"]:
        text += ", with uncommitted changes"
    return text


def facts(video, page_link):
    """A card's facts line, as HTML: when it was recorded, and, from its
    notes file, the test, where it was run from and the result; then its
    size, its file name and (on the main page) a link to its own page.
    Notes added to an old recording say when they were added instead of
    a result: nobody knows how that run ended."""
    items = [f"Recorded {format_time(video.recorded)}"]
    record = video.record
    if record is not None:
        if record["test"]:
            items.append(esc(record["test"]))
        source = source_text(record)
        if source:
            items.append(esc(source))
        if record["notes_added"] is not None:
            items.append(f"notes added {format_time(record['notes_added'])}")
        else:
            items.append(esc(result_text(record["result"])))
    items.append(format_size(video.size))
    items.append(f'<span class="file">{esc(video.filename)}</span>')
    if page_link:
        items.append(f'<a href="/r/{url_part(video.name)}">own page</a>')
    return " · ".join(items)


def under_title(record):
    """The line under a card's title: "Part of #87 <title>" and "PR #97",
    each linked, when the notes file names them ("" when neither)."""
    if record is None:
        return ""
    items = []
    parent = record["parent"]
    if parent is not None:
        number = parent["number"]
        text = esc(f"#{number} {parent['title']}")
        items.append(f'Part of <a href="{esc(ISSUE_URL.format(number))}">{text}</a>')
    if record["pr"] is not None:
        items.append(f'<a href="{esc(PULL_URL.format(record["pr"]))}">'
                     f'PR #{record["pr"]}</a>')
    return " · ".join(items)


def card(video, page_link=True):
    """One video's card: its title (linked to its ticket, when it has one),
    the parent and PR, its facts line, its notes and its video. On the main
    page its facts end with a link to its own page."""
    title = esc(video.title)
    if video.title_link is not None:
        title = f'<a href="{esc(video.title_link)}">{title}</a>'
    parts = ['<div class="card">', f'<div class="title">{title}</div>']
    links = under_title(video.record)
    if links:
        parts.append(f'<div class="links">{links}</div>')
    parts.append(f'<div class="facts">{facts(video, page_link)}</div>')
    tag = (f'<video controls preload="metadata">'
           f'<source src="/{video.source.label}/{url_part(video.filename)}" '
           f'type="video/mp4"></video>')
    notes = video.record["notes"] if video.record is not None else None
    if video.notes_unreadable:
        parts.append(f'<div class="no-notes">No notes: notes file unreadable '
                     f'({esc(video.name)}.json).</div>')
    elif notes is None:
        parts.append('<div class="no-notes">No notes for this recording.</div>')
    if notes is None:
        parts.append(tag)
    else:
        # The text is escaped and shown as written: no Markdown.
        sections = "\n".join(f'<h3>{esc(heading)}</h3>\n<p>{esc(notes[key])}</p>'
                             for key, heading in NOTE_SECTIONS)
        parts.append(f'<div class="main">\n<div class="notes">\n{sections}\n</div>\n'
                     f'{tag}\n</div>')
    parts.append('</div>')
    return "\n".join(parts)


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
    return PAGE.format(title="mGBA Recordings", body="\n".join(parts))


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

    /r/<name> only finds a name the folders' own listings hold (find_video),
    so a decoded "../x" or "sub/x" is just a name that isn't there: 404."""
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
    """The file a GET of /<label>/<file>.mp4 asks for, or None. The path is
    split at its slashes before %-decoding, so an encoded slash (%2F) stays
    part of the file name, and find_video then finds no such file."""
    parts = path.split("?", 1)[0].split("/")
    if len(parts) != 3 or parts[0] != "":
        return None
    source = next((s for s in sources if s.label == parts[1]), None)
    if source is None:
        return None
    video = find_video([source], unquote(parts[2]), with_notes=False)
    if video is None:
        return None
    return os.path.join(source.directory, video.filename)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            self._answer()
        except ConnectionError:
            # The browser hung up: it does that to every request it no
            # longer needs, e.g. when the viewer seeks. Nothing to do, and
            # no traceback in the service's journal.
            pass

    def _answer(self):
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
            while remaining > 0:
                chunk = f.read(min(CHUNK_BYTES, remaining))
                if not chunk:
                    break           # the file got shorter while being sent
                self.wfile.write(chunk)
                remaining -= len(chunk)

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
    # Under systemd, print's output waits in a buffer until it fills; flush
    # so the service's journal shows these lines at once.
    sys.stdout.flush()
    server.serve_forever()


if __name__ == "__main__":
    main()
