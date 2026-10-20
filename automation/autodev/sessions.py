"""Starting Claude sessions and reading back what they report.

Each session is a headless `claude -p` run inside its own transient
systemd user unit (`autodev-<role>-<issue>`). systemd gives three things
for free: the time limit (RuntimeMaxSec), a clean environment, and a
record of whether the session is still running (`systemctl --user
is-active`). `--collect` makes systemd forget the unit once it ends, so
no failed units pile up.

A unit started by `systemd-run` does not inherit the dispatcher's
environment, so everything a session needs is passed explicitly:
  - EnvironmentFile: GH_TOKEN, so gh and git act as the bot;
  - CLAUDE_CONFIG_DIR: the owner's Claude login and plugins (the
    grilling and to-tickets skills live there, not in ~/.claude).

The session's stream-json output goes to a log file. Its last event is a
"result" whose `structured_output` follows RESULT_SCHEMA.
"""

import datetime
import json
import os
import re
import shlex
import time
import zoneinfo
from dataclasses import dataclass

from autodev import worktrees

# Time limits per role, in seconds (#48, Sessions).
TIME_LIMITS = {"grill": 60 * 60, "ticket": 3 * 60 * 60, "merge": 45 * 60,
               "fixup": 90 * 60}

# Effort level per role (claude --effort), set here so a session never
# takes the owner's own default. Grilling gets the most: every ticket is
# built from its design.
EFFORTS = {"grill": "max", "ticket": "xhigh", "merge": "xhigh",
           "fixup": "xhigh"}

# What every session must report when it ends. The dispatcher only
# believes "done" after checking GitHub (decide.py).
RESULT_SCHEMA = json.dumps({
    "type": "object",
    "properties": {
        "outcome": {"enum": ["done", "failed"]},
        "reason": {"type": "string"},
        # Issue numbers the session created or changed, e.g. its tickets.
        "links": {"type": "array", "items": {"type": "integer"}},
    },
    "required": ["outcome", "reason", "links"],
}, separators=(",", ":"))


@dataclass(frozen=True)
class SessionConfig:
    claude: str             # absolute path: systemd's PATH lacks ~/.local/bin
    claude_config_dir: str  # the owner's Claude config directory
    env_file: str           # holds the bot's GH_TOKEN
    clone: str              # autodev's own clone; sessions run in it
    log_dir: str            # one stream-json log per session
    state_dir: str          # one JSON record per started session
    model: str


def unit_name(role, issue):
    return "autodev-%s-%d" % (role, issue)


def exit_path(log_path):
    """Where the unit's end reason goes: the log's name, ending .exit."""
    return os.path.splitext(log_path)[0] + ".exit"


def working_directory(config, role, issue):
    """A grill session reads the clone; the others work in the ticket's
    own worktree."""
    if role == "grill":
        return config.clone
    return worktrees.path(config.clone, issue)


def launch_command(config, role, issue, session_id, log_path, prompt,
                   pr=None, handled_through=None):
    """The systemd-run command that starts one session. A merge or fix-up
    session also gets its PR and the newest comment it answers."""
    err_path = os.path.splitext(log_path)[0] + ".err"
    return [
        "systemd-run", "--user", "--quiet", "--collect",
        # By default systemd-run expands $VAR in the command's arguments,
        # which would silently change any prompt that mentions one.
        "--expand-environment=no",
        "--unit", unit_name(role, issue),
        # Type=exec: systemd-run only returns once claude has really
        # started, so a bad path fails the launch instead of looking like
        # a crash later.
        "-p", "Type=exec",
        "-p", "RuntimeMaxSec=%d" % TIME_LIMITS[role],
        "-p", "EnvironmentFile=" + config.env_file,
        "-p", "WorkingDirectory=" + working_directory(config, role, issue),
        "-p", "StandardOutput=append:" + log_path,
        "-p", "StandardError=append:" + err_path,
        # However the unit ends, systemd then runs ExecStopPost with
        # SERVICE_RESULT saying how: "success", "exit-code", "timeout" (it
        # hit RuntimeMaxSec), "signal"... The leading ":" stops systemd
        # expanding $SERVICE_RESULT itself, so the shell does it.
        "-p", "ExecStopPost=:/bin/sh -c %s" % shlex.quote(
            'echo "$SERVICE_RESULT" > %s' % shlex.quote(exit_path(log_path))),
        "--setenv=CLAUDE_CONFIG_DIR=" + config.claude_config_dir,
        # The issue this session works on and its role. automation/bin/
        # issue.py only lets a session change this one issue (and a grill's
        # new tickets), with the commands its role needs.
        "--setenv=AUTODEV_ISSUE=%d" % issue,
        "--setenv=AUTODEV_ROLE=" + role,
    ] + (["--setenv=AUTODEV_PR=%d" % pr] if pr is not None else []) + (
        ["--setenv=AUTODEV_HANDLED_THROUGH=%d" % handled_through]
        if handled_through is not None else []) + [
        "--",
        config.claude, "-p", prompt,
        # Auto mode lets the safety classifier approve routine actions; with
        # prompts off, anything it won't approve is denied, not left waiting.
        "--permission-mode", "auto", "--permission-prompts", "none",
        "--settings", os.path.join(config.clone, "automation", "settings",
                                   role + ".json"),
        "--output-format", "stream-json", "--verbose",
        "--json-schema", RESULT_SCHEMA,
        "--session-id", session_id,
        "--model", config.model,
        "--effort", EFFORTS[role],
    ]


def read_result(log_path):
    """The session's last structured result, or None if it never reported
    one (it crashed, was stopped at its time limit, or is still running).

    A session can log more than one "result" event: each turn of a `-p`
    session ends with one, and a background job finishing after the
    session reported wakes it for another turn. So the newest event that
    carries a report wins, and later events without one are skipped."""
    try:
        with open(log_path) as f:
            lines = f.readlines()
    except FileNotFoundError:
        return None
    for line in reversed(lines):
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if (event.get("type") == "result" and not event.get("is_error")
                and event.get("structured_output") is not None):
            return event["structured_output"]
    return None


# ── the Claude usage limit (D18) ──────────────────────────────────────────
#
# The CLI documents no exit code for running out of usage (#48), so its
# message is matched. Its wording isn't documented either; these cover the
# forms known: "You've hit your limit · resets 3pm (Australia/Sydney)",
# "5-hour limit reached ∙ resets 10:30am", and older CLIs' "Claude AI usage
# limit reached|1760000000" (the reset as seconds since 1970).
LIMIT = re.compile(r"you've hit your\b[^\n]{0,40}\blimit|usage limit reached"
                   r"|limit reached\s*[|\u2219\u00b7]|limit\s*[\u2219\u00b7]\s*resets",
                   re.IGNORECASE)
LIMIT_EPOCH = re.compile(r"limit reached\|(\d{9,11})", re.IGNORECASE)
# "resets [Oct 23[, 2026], ][at ]9[:30][am|pm][ (Zone/Name)]"
LIMIT_RESETS = re.compile(
    r"resets?\s+(?:at\s+)?(?:([A-Za-z]{3,9})\.?\s+(\d{1,2})(?:,?\s+(\d{4}))?,?\s+"
    r"(?:at\s+)?)?(\d{1,2})(?::(\d{2}))?\s*(am|pm)?(?:\s*\(([^)]+)\))?",
    re.IGNORECASE)
# With a limit message but no time that can be read, try again in an hour.
LIMIT_FALLBACK = 60 * 60


def usage_limit(text, now, zone=None):
    """When the usage limit `text` reports resets, as seconds since 1970,
    or None if `text` holds no usage-limit message. A time with no zone in
    the message is in `zone` (by default the machine's)."""
    if not LIMIT.search(text or ""):
        return None
    epoch = LIMIT_EPOCH.search(text)
    if epoch:
        return float(epoch.group(1))
    found = LIMIT_RESETS.search(text)
    if found:
        try:
            return _reset_time(found.groups(), now, zone)
        except (ValueError, zoneinfo.ZoneInfoNotFoundError):
            pass
    return now + LIMIT_FALLBACK


def _reset_time(parts, now, zone):
    month, day, year, hour, minute, ampm, named = parts
    hour, minute = int(hour), int(minute or 0)
    if ampm:
        if not 1 <= hour <= 12:
            raise ValueError("no such hour: %d%s" % (hour, ampm))
        hour = hour % 12 + (12 if ampm.lower() == "pm" else 0)
    name = named or zone
    if name:
        tz = zoneinfo.ZoneInfo(name)
        current = datetime.datetime.fromtimestamp(now, tz)
    else:
        # The machine's own zone, as wall-clock times: time.mktime below
        # applies daylight saving for the day the reset falls on, which a
        # fixed UTC offset taken from today would get wrong.
        tz = None
        current = datetime.datetime(*time.localtime(now)[:6])
    if month:
        number = datetime.datetime.strptime(month[:3].title(), "%b").month
        reset = current.replace(year=int(year or current.year), month=number,
                                day=int(day), hour=hour, minute=minute,
                                second=0, microsecond=0)
        if not year and reset < current - datetime.timedelta(days=1):
            reset = reset.replace(year=reset.year + 1)     # "Jan 2" seen in December
    else:
        reset = current.replace(hour=hour, minute=minute, second=0, microsecond=0)
        if reset <= current:
            reset += datetime.timedelta(days=1)         # a time already past: tomorrow
    return reset.timestamp() if tz else time.mktime(reset.timetuple())


def limit_line(text):
    """The line of `text` that holds the usage-limit message, for the bot's
    comment."""
    for line in text.splitlines():
        if LIMIT.search(line):
            return line.strip()[:200]
    return ""


def limit_text(log_path):
    """Where a usage-limit message would be: the text of the session's
    error results, and its stderr. Not its other results, which carry the
    session's own closing words, and those may well discuss usage limits."""
    texts = []
    try:
        with open(log_path) as f:
            for line in f:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                if (event.get("type") == "result" and event.get("is_error")
                        and isinstance(event.get("result"), str)):
                    texts.append(event["result"])
    except FileNotFoundError:
        pass
    try:
        with open(os.path.splitext(log_path)[0] + ".err") as f:
            texts.append(f.read())
    except FileNotFoundError:
        pass
    return "\n".join(texts)
