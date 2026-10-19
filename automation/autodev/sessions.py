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

import json
import os
import shlex
from dataclasses import dataclass

from autodev import worktrees

# Time limits per role, in seconds (#48, Sessions).
TIME_LIMITS = {"grill": 60 * 60, "ticket": 3 * 60 * 60}

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


def launch_command(config, role, issue, session_id, log_path, prompt):
    """The systemd-run command that starts one session."""
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
    ]


def read_result(log_path):
    """The session's structured result, or None if it never reported one
    (it crashed, was stopped at its time limit, or is still running)."""
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
        if event.get("type") == "result":
            if event.get("is_error"):
                return None
            return event.get("structured_output")
    return None
