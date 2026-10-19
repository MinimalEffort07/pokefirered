"""The autodev dispatcher: one run per timer tick.

A tick reads the board, works out what to do (decide.py), and does it:
it starts grill sessions for new parent issues, and checks finished
sessions' work on GitHub. systemd sends everything it prints to the
journal (`journalctl --user -u autodev-dispatcher`).
"""

import os
from dataclasses import fields

from autodev import board, sessions
from autodev.decide import (Comment, Finish, Launch, LinkSubIssue,
                            PlaceOnBoard, SetStatus, State, decide)
from autodev.github import GitHub
from autodev.registry import Registry

PROMPTS = os.path.join(os.path.dirname(os.path.dirname(__file__)), "prompts")


def log_board(gh, config, log):
    """Read the board and log one line per issue on it.
    Returns (exit code, board), with board None if it wasn't found."""
    number = board.find_board(gh, config)
    if number is None:
        print('error: no board titled "%s" owned by %s' % (
            config.board_title, config.owner), file=log)
        return 1, None
    snapshot = board.read_board(gh, config, number)
    header = 'board #%d "%s": %s' % (
        snapshot.number, snapshot.title, _count(len(snapshot.items), "item"))
    if snapshot.skipped:
        header += ", %s skipped" % _count(snapshot.skipped, "other card")
    print(header, file=log)
    for item in snapshot.items:
        print("#%d %s %s has_parent=%s" % (
            item.number, item.author, item.status,
            "yes" if item.has_parent else "no"), file=log)
    return 0, snapshot


def tick(gh, config, log, registry=None):
    """One dispatcher tick. Returns the process exit code."""
    code, snapshot = log_board(gh, config, log)
    if snapshot is None:
        return code
    registry = registry or Registry(session_config(config))
    github = GitHub(gh, config, snapshot)
    known = registry.load()
    outcomes = {
        s.issue: github.grill_outcome(s.issue, (s.result or {}).get("links", []))
        for s in known if s.role == "grill" and not s.running}
    state = State(owner=config.owner, bot=config.bot, items=snapshot.items,
                  sessions=known, outcomes=outcomes,
                  max_sessions=config.max_sessions,
                  resume_command=config.resume_command)
    for action in decide(state):
        print("→ %s" % _describe(action), file=log)
        _carry_out(action, github, registry, config)
    return 0


def session_config(config):
    names = {f.name for f in fields(sessions.SessionConfig)}
    return sessions.SessionConfig(**{n: getattr(config, n) for n in names})


def _carry_out(action, github, registry, config):
    if isinstance(action, SetStatus):
        github.set_status(action.issue, action.status)
    elif isinstance(action, Comment):
        github.comment(action.issue, action.body)
    elif isinstance(action, Launch):
        registry.start(action.role, action.issue, prompt(config, action.role, action.issue))
    elif isinstance(action, LinkSubIssue):
        github.link_sub_issue(action.parent, action.child)
    elif isinstance(action, PlaceOnBoard):
        github.place_on_board(action.issue, action.status)
    elif isinstance(action, Finish):
        registry.finish(action.role, action.issue)
    else:
        raise ValueError("unknown action %r" % (action,))


def prompt(config, role, issue):
    with open(os.path.join(PROMPTS, role + ".md")) as f:
        return f.read().format(issue=issue, repo=config.repo,
                               owner=config.owner, bot=config.bot)


def _describe(action):
    if isinstance(action, Comment):
        return "comment on #%d" % action.issue
    return repr(action)


def _count(n, noun):
    return "%d %s%s" % (n, noun, "" if n == 1 else "s")
