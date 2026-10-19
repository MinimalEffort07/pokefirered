"""The autodev dispatcher: one run per timer tick.

A tick reads the board, works out what to do (decide.py), and does it:
it starts grill sessions for new parent issues, and checks finished
sessions' work on GitHub. systemd sends everything it prints to the
journal (`journalctl --user -u autodev-dispatcher`).
"""

import os

from autodev import board
from autodev.decide import (Comment, Finish, Launch, LinkSubIssue,
                            PlaceOnBoard, SetStatus, State, plan)
from autodev.github import GitHub
from autodev.gh import GhError
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
    registry = registry or Registry(config.session_config())
    github = GitHub(gh, config, snapshot)
    known = registry.load()
    outcomes = {}
    for s in known:
        if s.role == "grill" and not s.running:
            try:
                outcomes[s.issue] = github.grill_outcome(
                    s.issue, (s.result or {}).get("links", []))
            except GhError as e:
                # Leave this session for the next tick; the others go on.
                print("error: reading #%d's grill outcome: %s" % (s.issue, e),
                      file=log)
                code = 1
    state = State(owner=config.owner, items=snapshot.items, sessions=known,
                  outcomes=outcomes, max_sessions=config.max_sessions,
                  resume_command=config.resume_command)
    for group in plan(state):
        for action in group:
            print("→ %s" % _describe(action), file=log)
            try:
                _carry_out(action, github, registry, config)
            except Exception as e:
                # Skip the rest of this issue's actions, not everyone's.
                print("error: %s failed: %s" % (_describe(action), e), file=log)
                code = 1
                break
    removed = registry.prune()
    if removed:
        print("pruned %d session file%s older than 30 days" % (
            removed, "" if removed == 1 else "s"), file=log)
    return code


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
    """The role's prompt with its {issue}, {repo}, {owner} and {bot}
    placeholders filled in. Plain replacement, so any other braces in the
    Markdown are left alone."""
    with open(os.path.join(PROMPTS, role + ".md")) as f:
        text = f.read()
    for name, value in (("issue", issue), ("repo", config.repo),
                        ("owner", config.owner), ("bot", config.bot)):
        text = text.replace("{%s}" % name, str(value))
    return text


def _describe(action):
    if isinstance(action, Comment):
        return "comment on #%d" % action.issue
    return repr(action)


def _count(n, noun):
    return "%d %s%s" % (n, noun, "" if n == 1 else "s")
