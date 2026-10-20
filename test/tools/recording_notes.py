#!/usr/bin/env python3
"""A recording's notes: checking them, and writing its notes file.

test/record_test.sh --notes FILE takes notes saying what a recording is
for, and keeps them beside the video as <name>.json, its "notes file",
which the recordings page (test/serve_recordings.py) shows on the
recording's card. This helper does both halves of that for
record_test.sh:

  python3 test/tools/recording_notes.py check [--save COPY] NOTES
      Check notes file NOTES ("-" reads stdin). Prints "Notes OK: ..." and
      exits 0, or prints every problem and exits 1. --save COPY also
      writes accepted notes to COPY, as they were read: that is how
      record_test.sh keeps notes it was given on stdin.
  python3 test/tools/recording_notes.py ticket NOTES
      Print the ticket number NOTES names (nothing when it names none).
      Exits 1, printing the problems, when the notes are refused.
  python3 test/tools/recording_notes.py write VIDEO --test TEST
          --recorded-at TIME [--results RESULTS] [--notes NOTES] [--repo DIR]
      Write VIDEO's notes file: VIDEO with .mp4 replaced by .json. VIDEO
      itself needn't exist yet.
  python3 test/tools/recording_notes.py set-result NOTES_FILE --results RESULTS
      Put the result from RESULTS into notes file NOTES_FILE, leaving the
      rest as it was.
  python3 test/tools/recording_notes.py set-pr NAME PR [--kept DIR] [--scratch DIR]
      Put PR's number into the notes file of the recording named NAME
      (as record_test.sh prints it, with or without .mp4), leaving the rest
      as it was. It looks in ~/recordings first, then /tmp/mgba-recordings,
      as the recordings page does; --kept and --scratch name other folders.
      A ticket session runs it once its PR is open (see AUTODEV SESSIONS).
  python3 test/tools/recording_notes.py add-notes NAME NOTES [--test TEST] [--kept DIR]
      Give notes NOTES ("-" reads stdin) to the recording named NAME (as
      the recordings page shows it, with or without .mp4), one kept before
      notes existed, by writing NAME.json beside NAME.mp4 in ~/recordings
      (--kept names another folder). TEST is the test it recorded. See OLD
      RECORDINGS.

record_test.sh writes the notes file into its run's own directory as the
recording starts (once it has its turn), so the branch, commit and
uncommitted changes are those of the checkout as mGBA loads the test,
even if the checkout changes while the recording runs. Once the test has
finished it adds the result with set-result, and once the video is in
place it moves the notes file beside it.

THE NOTES, as a person writes them (the ticket and the PR are examples):

  ticket: 90 Shaking patches survive a field reload
  parent: 87 The Poke Radar
  pr: 97

  ## What's tested
  The radar's four patches after closing the BAG, after Continue, ...
  ## The problem
  Rebuilding the field destroyed the patch sprites, ...
  ## What you should see
  Four patches shake. ...

  - The lines before the first "## " heading are headers, each optional
    and allowed once: ticket: and parent: take an issue number (with or
    without #), a space and its title; pr: takes a number. Any other key
    is refused, naming it.
  - The three sections are all required, each once and each with text, in
    any order. Any other "## " heading is refused, naming it.
  - Plain text: the page keeps the line breaks and renders no Markdown.

AUTODEV SESSIONS: autodev's dispatcher starts every session with
AUTODEV_ROLE and AUTODEV_ISSUE set, and AUTODEV_PR for a fix-up or a merge
(launch_command in automation/autodev/sessions.py). With AUTODEV_ROLE set,
check, ticket and write hold the notes to more rules, so every recording a
session makes says what it is for:

  - ticket: is required, and must be the session's own issue;
  - parent: is required (the feature the ticket belongs to);
  - notes without a pr: get AUTODEV_PR's number, when the session has one.

test/record_test.sh itself refuses to record without notes in a session. A
ticket session records before its PR exists, so it runs set-pr after
opening the PR. A person (no AUTODEV_ROLE) sees none of this. add-notes
doesn't follow these rules either (see OLD RECORDINGS).

OLD RECORDINGS: videos kept before notes existed (#110) have no notes
file, so their cards have no notes. add-notes gives one notes, without
moving it on the recordings page, which lists the newest first:

  - "recorded_at" comes from the stamp record_test.sh ends a video's name
    with (_YYYYMMDD_HHMMSS, when the recording started), in this machine's
    time with that date's UTC offset. With no valid stamp it is the
    video's modification time, which is when the recording ended (mv and
    test/tools/shrink_recordings.sh keep it). It is never the time the
    notes are added, which would list the video as the newest.
  - What nobody knows any more is null: "branch", "commit",
    "uncommitted_changes" and "result". "test" is TEST's path (as for
    write), or null without --test. "notes_added_at" says when the notes
    were written, and the page's card says "notes added <date>" in place
    of a result.
  - It refuses, saying why and changing nothing: a name with no such video
    in the folder; a video whose notes file record_test.sh wrote (one with
    no "notes_added_at": every fresh recording has one); a notes file it
    can't read; and notes the parser refuses, listing every problem.
  - It may rewrite a notes file it wrote itself, to fix the notes, keeping
    its "recorded_at". The rest comes from the new run, so give --test
    again.
  - An autodev session's rules (see AUTODEV SESSIONS) don't apply, because
    an old video belongs to an old ticket. Refusing notes files
    record_test.sh wrote is what keeps it off fresh recordings.

THE NOTES FILE is JSON. What the notes don't give is null:

  {
    "video": "issue-90_test_x_20261020_134554.mp4",
    "recorded_at": "2026-10-20T13:45:54+11:00",
    "test": "test/tests/test_x.lua",
    "branch": "issue-90-shaking-patches-survive-a-field",
    "commit": "cae067302",
    "uncommitted_changes": false,
    "result": {"pass": 215, "fail": 0},
    "ticket": {"number": 90, "title": "Shaking patches survive a field reload"},
    "parent": {"number": 87, "title": "The Poke Radar"},
    "pr": 97,
    "notes": {"tested": "...", "problem": "...", "expect": "..."}
  }

  - "recorded_at" is when the recording started, with this machine's UTC
    offset; the video's name holds the same time.
  - "test" is the test file's path from the repo root when it is inside
    the repo, else its full path.
  - "branch" and "commit" are the repo's (null on a detached HEAD or
    outside git), and "uncommitted_changes" is true when tracked files
    differ from that commit.
  - "result" is the test's own pass and fail counts, from the results file
    test/lib/framework.lua writes when a test finishes, and null when there
    is none: the test didn't finish.
  - "notes_added_at", the last field, is only in a notes file add-notes
    wrote: when it wrote it, with this machine's UTC offset (e.g.
    "2026-10-21T14:05:33+11:00"). record_test.sh never writes it. See OLD
    RECORDINGS.

The recordings page reads this format itself, without importing this
file (see its ONE SELF-CONTAINED FILE), so test/tests/
test_recording_notes.lua writes notes files with this helper and reads
them back through the page: that keeps the two halves agreeing.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys

# The three sections: the key each has in the notes file, and its heading.
SECTIONS = [
    ("tested", "What's tested"),
    ("problem", "The problem"),
    ("expect", "What you should see"),
]
HEADERS = ("ticket", "parent", "pr")

SECTION_LIST = '"## What\'s tested", "## The problem" and "## What you should see"'
HEADER_LIST = "ticket:, parent: and pr:"

# A header line: a key, a colon, then its value.
HEADER_LINE = re.compile(r"([^\s:]+):(.*)")
# An issue and its title: "90 Shaking patches", or "#90 Shaking patches".
ISSUE_VALUE = re.compile(r"#?(\d+)\s+(\S.*)")
PR_VALUE = re.compile(r"#?(\d+)")

# Where set-pr looks for a recording, in this order, as the recordings page
# does: the kept recordings, then record_test.sh's own folder. add-notes
# looks only in the kept one.
KEPT_DIR = os.path.expanduser("~/recordings")
SCRATCH_DIR = "/tmp/mgba-recordings"

# The checkout holding this script: a test's path is given from its root.
REPO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

# record_test.sh ends a video's name with the date and time its recording
# started: _<YYYYMMDD>_<HHMMSS>, e.g. test_x_20261020_134554.
NAME_STAMP = re.compile(r"_(\d{8}_\d{6})$")


def heading_key(heading):
    """The section key of a "## " heading, or None. Case doesn't matter,
    and a typographic apostrophe (What’s) counts as a plain one."""
    wanted = heading.strip().replace("’", "'").casefold()
    for key, title in SECTIONS:
        if wanted == title.casefold():
            return key
    return None


def section_text(lines):
    """A section's text: its lines as written, without the blank lines
    around them or spaces at the ends of lines."""
    lines = [line.rstrip() for line in lines]
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def parse_issue(key, value, number):
    """A ticket: or parent: header's value as {"number", "title"}, or a
    problem (a string)."""
    m = ISSUE_VALUE.fullmatch(value)
    if m is None or int(m.group(1)) == 0:
        return (f'line {number}: {key}: needs an issue number and its title, '
                f'as in "{key}: 90 Shaking patches survive a field reload" '
                f'(got "{value}")')
    return {"number": int(m.group(1)), "title": m.group(2).strip()}


def parse_pr(value, number):
    """A pr: header's value as a number, or a problem (a string)."""
    m = PR_VALUE.fullmatch(value)
    if m is None or int(m.group(1)) == 0:
        return (f'line {number}: pr: needs a PR number, as in "pr: 97" '
                f'(got "{value}")')
    return int(m.group(1))


def session_from(env):
    """The autodev session environment `env` belongs to, as {"issue",
    "pr"}, or None outside one (no role). The issue and the PR are numbers,
    or None when missing or not a number."""
    if not env.get("AUTODEV_ROLE", ""):
        return None

    def number(name):
        value = env.get(name, "")
        return int(value) if value.isdigit() and int(value) > 0 else None

    return {"issue": number("AUTODEV_ISSUE"), "pr": number("AUTODEV_PR")}


def session_problems(headers, malformed, session):
    """What autodev session `session` refuses in notes with `headers`
    (see AUTODEV SESSIONS). A header in `malformed` already has a problem
    of its own, so it gets no second one here."""
    problems = []
    issue = session["issue"]
    ticket = headers.get("ticket")
    own_ticket_hint = (f'this autodev session is for #{issue}, so its notes say '
                       f'"ticket: {issue} <the ticket\'s title>"')
    if issue is None:
        problems.append("this autodev session's issue number isn't set, so it can't "
                        "check which ticket the notes are for")
    elif ticket is None and "ticket" not in malformed:
        problems.append(f'missing "ticket:": {own_ticket_hint}')
    elif ticket is not None and ticket["number"] != issue:
        problems.append(f'ticket: {ticket["number"]} isn\'t this session\'s ticket: {own_ticket_hint}')
    if headers.get("parent") is None and "parent" not in malformed:
        problems.append('missing "parent:": an autodev session\'s notes name the feature '
                        'its ticket belongs to (the issue under the ticket\'s "## Parent"), '
                        'as in "parent: 87 <its title>"')
    return problems


def parse(text, session=None):
    """Notes text -> (fields, problems). `fields` has "ticket", "parent",
    "pr" and "notes" as the notes file holds them (None when not given);
    `problems` lists what is wrong, in plain words, and is empty when the
    notes are accepted. Notes written in autodev session `session` (from
    session_from) also follow its rules, and get its PR when they name
    none."""
    problems = []
    headers = {}
    malformed = set()       # headers given with a value that can't be read
    sections = {}           # key -> the section's lines
    current = None          # where lines go: None = the headers, else a key
    SKIP = object()         # under a refused heading: lines are dropped
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("## ") or line.rstrip() == "##":
            heading = line[3:].strip()
            key = heading_key(heading)
            if key is None:
                problems.append(f'line {number}: unknown heading "## {heading}" '
                                f'(the sections are {SECTION_LIST})')
                current = SKIP
            elif key in sections:
                problems.append(f'line {number}: section "## {dict(SECTIONS)[key]}" '
                                f'is given twice')
                current = SKIP
            else:
                sections[key] = []
                current = key
            continue
        if current is SKIP:
            continue
        if current is not None:
            sections[current].append(line)
            continue
        # Before the first heading: headers and blank lines only.
        stripped = line.strip()
        if not stripped:
            continue
        m = HEADER_LINE.fullmatch(stripped)
        if m is None:
            problems.append(f'line {number}: "{stripped}" is not a header '
                            f'({HEADER_LIST}); text goes under one of the '
                            f'sections, {SECTION_LIST}')
            continue
        key, value = m.group(1), m.group(2).strip()
        if key not in HEADERS:
            problems.append(f'line {number}: unknown header "{key}:" '
                            f'(the headers are {HEADER_LIST})')
            continue
        if key in headers:
            problems.append(f'line {number}: header "{key}:" is given twice')
            continue
        parsed = parse_pr(value, number) if key == "pr" else parse_issue(key, value, number)
        if isinstance(parsed, str):
            problems.append(parsed)
            malformed.add(key)
        else:
            headers[key] = parsed

    if session is not None:
        problems += session_problems(headers, malformed, session)
        if "pr" not in headers and "pr" not in malformed and session["pr"] is not None:
            headers["pr"] = session["pr"]

    notes = {}
    for key, title in SECTIONS:
        if key not in sections:
            problems.append(f'missing section "## {title}"')
            continue
        notes[key] = section_text(sections[key])
        if not notes[key]:
            problems.append(f'section "## {title}" is empty')

    fields = {key: headers.get(key) for key in HEADERS}
    fields["notes"] = notes
    return fields, problems


def read_notes(path, save=None, session_rules=True):
    """Notes file `path` ("-" for stdin) -> (fields, problems), as parse.
    A file that can't be read is one problem. With `save`, accepted notes
    are also written to that file, byte for byte. session_rules=False
    leaves out an autodev session's rules (add-notes)."""
    try:
        if path == "-":
            data = sys.stdin.buffer.read()
        else:
            with open(path, "rb") as f:
                data = f.read()
    except FileNotFoundError:
        return None, [f"notes file not found: {path}"]
    except OSError as e:
        return None, [f"can't read notes file {path}: {e.strerror}"]
    try:
        # utf-8-sig drops the byte-order mark some editors start a file with.
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return None, [f"notes file {path} isn't UTF-8 text"]
    fields, problems = parse(text, session_from(os.environ) if session_rules else None)
    if save is not None and not problems:
        try:
            with open(save, "wb") as f:
                f.write(data)
        except OSError as e:
            problems.append(f"can't save a copy of the notes to {save}: {e.strerror}")
    return fields, problems


def summary(fields):
    """One line saying what accepted notes hold."""
    parts = []
    for key, label in (("ticket", "ticket"), ("parent", "part of")):
        issue = fields[key]
        parts.append(f'{label} #{issue["number"]}' if issue else f"no {key}")
    parts.append(f'PR #{fields["pr"]}' if fields["pr"] else "no PR")
    return "Notes OK: " + ", ".join(parts) + ", and all three sections."


def report_problems(problems, path):
    """Print why the notes at `path` were refused, one problem a line."""
    where = "stdin" if path == "-" else path
    print(f"The notes ({where}) can't be used:", file=sys.stderr)
    for problem in problems:
        print(f"  - {problem}", file=sys.stderr)


def cmd_check(args):
    """check: the verdict on the notes, and their copy with --save."""
    fields, problems = read_notes(args.notes, args.save)
    if problems:
        report_problems(problems, args.notes)
        return 1
    print(summary(fields))
    return 0


def cmd_ticket(args):
    """ticket: the number of the ticket the notes name, for the video's
    name."""
    fields, problems = read_notes(args.notes)
    if problems:
        report_problems(problems, args.notes)
        return 1
    if fields["ticket"]:
        print(fields["ticket"]["number"])
    return 0


def read_result(path):
    """The test's pass and fail counts from its results file, as
    {"pass", "fail"}, or None when there is no such file (the test didn't
    finish) or it holds no counts. test/lib/framework.lua writes an error
    message's line breaks as they are, which strict JSON forbids, so they
    are allowed here (strict=False)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            data = json.loads(f.read(), strict=False)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    counts = {key: data.get(key) for key in ("pass", "fail")}
    for value in counts.values():
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            return None
    return counts


def git(repo, *args):
    """What `git -C repo ARGS` prints, without the final line break, or
    None when it fails (not a git repo, a detached HEAD for symbolic-ref).
    --no-optional-locks: git status would otherwise rewrite the repo's
    index, which a session building in that checkout may be using."""
    try:
        done = subprocess.run(["git", "--no-optional-locks", "-C", repo] + list(args),
                              capture_output=True, text=True)
    except OSError:
        return None
    if done.returncode != 0:
        return None
    return done.stdout.rstrip("\n")


def test_path(test, repo):
    """The test file's path from the repo's root when it is inside the
    repo, else its full path. Links are followed first, so a test reached
    through a symlinked folder still counts as inside."""
    test = os.path.realpath(test)
    root = os.path.realpath(repo)
    if os.path.commonpath([test, root]) == root:
        return os.path.relpath(test, root)
    return test


def notes_file_path(video):
    """The notes file of video VIDEO.mp4: VIDEO.json beside it."""
    return video[:-len(".mp4")] + ".json"


def save(record, path):
    """Write notes file `path`. It is written beside its final place under
    a hidden name, then renamed into place in one step, so the recordings
    page never reads half a file. Returns an exit code."""
    part = os.path.join(os.path.dirname(path), "." + os.path.basename(path) + ".part")
    try:
        with open(part, "w", encoding="utf-8") as f:
            json.dump(record, f, indent=2, ensure_ascii=False)
            f.write("\n")
        os.replace(part, path)
    except OSError as e:
        print(f"can't write {path}: {e.strerror}", file=sys.stderr)
        return 1
    return 0


def is_time(text):
    """Whether `text` is a time the recordings page reads, such as
    2026-10-20T13:45:54+11:00."""
    try:
        datetime.datetime.fromisoformat(text)
    except (TypeError, ValueError):     # TypeError: not a string at all
        return False
    return True


def cmd_write(args):
    """write: a video's notes file, from the notes (if any), the test, the
    time, the results file (if any) and the checkout."""
    if not args.video.endswith(".mp4"):
        print(f"not a video (.mp4): {args.video}", file=sys.stderr)
        return 1
    # The recordings page sorts by this time, so it must be one it reads.
    if not is_time(args.recorded_at):
        print(f"--recorded-at needs a time like 2026-10-20T13:45:54+11:00, "
              f"not {args.recorded_at!r}", file=sys.stderr)
        return 1
    fields = dict.fromkeys(HEADERS + ("notes",))      # no notes: all null
    if args.notes is not None:
        fields, problems = read_notes(args.notes)
        if problems:
            report_problems(problems, args.notes)
            return 1
    status = git(args.repo, "status", "--porcelain", "--untracked-files=no")
    record = {
        "video": os.path.basename(args.video),
        "recorded_at": args.recorded_at,
        "test": test_path(args.test, args.repo),
        "branch": git(args.repo, "symbolic-ref", "--short", "-q", "HEAD"),
        "commit": git(args.repo, "rev-parse", "--short=9", "HEAD"),
        "uncommitted_changes": None if status is None else status != "",
        "result": read_result(args.results) if args.results else None,
        **fields,       # ticket, parent, pr and notes
    }
    return save(record, notes_file_path(args.video))


def load(path):
    """The record in notes file `path`, or None, saying why, when it can't
    be read or isn't a notes file."""
    try:
        with open(path, encoding="utf-8") as f:
            record = json.load(f)
    except (OSError, ValueError) as e:
        print(f"can't read notes file {path}: {e}", file=sys.stderr)
        return None
    if not isinstance(record, dict):
        print(f"not a notes file: {path}", file=sys.stderr)
        return None
    return record


def cmd_set_result(args):
    """set-result: the run's result into an existing notes file."""
    record = load(args.notes_file)
    if record is None:
        return 1
    record["result"] = read_result(args.results)
    return save(record, args.notes_file)


def recording_name(given, command):
    """A recording's name as `command` was given it (with or without .mp4),
    without the ending, or None, saying why, when it is not a plain name: a
    path can't lead the command outside its folder."""
    name = given
    for ending in (".mp4", ".json"):
        if name.endswith(ending):
            name = name[:-len(ending)]
    if not name or "/" in name or name.startswith("."):
        print(f'{command} needs a recording\'s name, its file name with or without .mp4 '
              f'(e.g. issue-90_test_x_20261020_134554), not "{given}"', file=sys.stderr)
        return None
    return name


def cmd_set_pr(args):
    """set-pr: a PR's number into the notes file of a recording, found by
    its name in the kept folder first, then in scratch, as the recordings
    page looks."""
    m = PR_VALUE.fullmatch(args.pr)
    if m is None or int(m.group(1)) == 0:
        print(f'set-pr needs a PR number, as in "set-pr NAME 97" (got "{args.pr}")',
              file=sys.stderr)
        return 1
    name = recording_name(args.name, "set-pr")
    if name is None:
        return 1
    folders = (args.kept, args.scratch)
    for folder in folders:
        path = os.path.join(folder, name + ".json")
        if os.path.isfile(path):
            break
    else:
        print(f"no notes file {name}.json in {' or '.join(folders)}", file=sys.stderr)
        return 1
    record = load(path)
    if record is None:
        return 1
    record["pr"] = int(m.group(1))
    if save(record, path) != 0:
        return 1
    print(f"PR #{record['pr']} set in {path}")
    return 0


def when_recorded(name, video):
    """When recording `name` (video file `video`) was recorded, as an ISO
    time with this machine's UTC offset on that date: when it started, from
    the stamp its name ends with, or, with no valid stamp, its video's
    modification time, which is when it ended (mv and the shrink tool keep
    that time). Never the time now: the page would list it as new."""
    m = NAME_STAMP.search(name)
    if m is not None:
        try:
            started = datetime.datetime.strptime(m.group(1), "%Y%m%d_%H%M%S")
            # A naive time's astimezone() takes it as this machine's time,
            # with the UTC offset in force on that date (summer or not).
            return started.astimezone().isoformat(timespec="seconds")
        except (ValueError, OverflowError, OSError):
            pass        # no such date or time (month 13): no stamp
    ended = datetime.datetime.fromtimestamp(os.stat(video).st_mtime, datetime.timezone.utc)
    return ended.astimezone().isoformat(timespec="seconds")


def cmd_add_notes(args):
    """add-notes: notes for a recording kept before notes existed, without
    moving it on the recordings page (see OLD RECORDINGS)."""
    name = recording_name(args.name, "add-notes")
    if name is None:
        return 1
    video = os.path.join(args.kept, name + ".mp4")
    path = notes_file_path(video)
    if not os.path.isfile(video):
        print(f"no video {name}.mp4 in {args.kept}: add-notes gives notes to a "
              f"kept recording, named as the recordings page shows it", file=sys.stderr)
        return 1
    # A notes file record_test.sh wrote has the recording's own facts (its
    # branch, commit and result), and every fresh recording has one: only
    # a notes file add-notes wrote itself (with notes_added_at) is rewritten,
    # and it keeps the time it gave the recording then. One whose times the
    # page can't read counts as unreadable, as on the page.
    recorded_at = None
    if os.path.lexists(path):
        record = load(path)
        if record is None:
            return 1
        added = record.get("notes_added_at")
        if added is None:
            print(f"{path} has no notes_added_at, so add-notes didn't write it: "
                  f"record_test.sh writes one for every recording, with the run's own "
                  f"facts. It is left as it is: add-notes only gives notes to a "
                  f"recording kept before notes existed", file=sys.stderr)
            return 1
        recorded_at = record.get("recorded_at")
        for key, value in (("notes_added_at", added), ("recorded_at", recorded_at)):
            if not is_time(value):
                print(f"can't read notes file {path}: its {key}, {value!r}, isn't a "
                      f"time (delete the file to start again)", file=sys.stderr)
                return 1
    fields, problems = read_notes(args.notes, session_rules=False)
    if problems:
        report_problems(problems, args.notes)
        return 1
    record = {
        "video": name + ".mp4",
        "recorded_at": recorded_at or when_recorded(name, video),
        "test": test_path(args.test, REPO_DIR) if args.test else None,
        "branch": None,
        "commit": None,
        "uncommitted_changes": None,
        "result": None,
        **fields,       # ticket, parent, pr and notes
        "notes_added_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
    }
    if save(record, path) != 0:
        return 1
    print(f"Notes added: {path}")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Check a recording's notes, or write its notes file.")
    commands = parser.add_subparsers(dest="command", required=True)

    p = commands.add_parser("check", help="check notes; exit 0 if accepted")
    p.add_argument("--save", metavar="COPY",
                   help="also write accepted notes to COPY, as read")
    p.add_argument("notes", help='the notes file, or "-" for stdin')
    p.set_defaults(run=cmd_check)

    p = commands.add_parser("ticket", help="print the ticket number notes name")
    p.add_argument("notes", help='the notes file, or "-" for stdin')
    p.set_defaults(run=cmd_ticket)

    p = commands.add_parser("write", help="write a video's notes file")
    p.add_argument("video", help="the video, VIDEO.mp4; its notes file is VIDEO.json")
    p.add_argument("--test", required=True, help="the test file recorded")
    p.add_argument("--recorded-at", required=True, metavar="TIME",
                   help="when the recording started, e.g. 2026-10-20T13:45:54+11:00")
    p.add_argument("--results",
                   help="the run's results file (it may not exist; none: "
                        "the result is null)")
    p.add_argument("--notes", help="the notes, already checked (none: no notes)")
    p.add_argument("--repo", default=REPO_DIR,
                   help="the git checkout for the branch and commit "
                        "(default: the one holding this script)")
    p.set_defaults(run=cmd_write)

    p = commands.add_parser("set-result", help="put the run's result into a notes file")
    p.add_argument("notes_file", help="the notes file, VIDEO.json")
    p.add_argument("--results", required=True,
                   help="the run's results file (it may not exist: the result is null)")
    p.set_defaults(run=cmd_set_result)

    p = commands.add_parser("set-pr", help="put a PR's number into a recording's notes file")
    p.add_argument("name", help="the recording's name, e.g. issue-90_test_x_20261020_134554")
    p.add_argument("pr", help="the PR's number")
    p.add_argument("--kept", default=KEPT_DIR,
                   help="the folder of kept recordings, looked in first (default: %(default)s)")
    p.add_argument("--scratch", default=SCRATCH_DIR,
                   help="record_test.sh's folder, looked in next (default: %(default)s)")
    p.set_defaults(run=cmd_set_pr)

    p = commands.add_parser("add-notes",
                            help="give notes to a recording kept before notes existed")
    p.add_argument("name", help="the recording's name, e.g. issue-24-safety-net")
    p.add_argument("notes", help='the notes file, or "-" for stdin')
    p.add_argument("--test", help="the test it recorded (none: the test is null)")
    p.add_argument("--kept", default=KEPT_DIR,
                   help="the folder the recording is kept in (default: %(default)s)")
    p.set_defaults(run=cmd_add_notes)

    args = parser.parse_args()
    sys.exit(args.run(args))


if __name__ == "__main__":
    main()
