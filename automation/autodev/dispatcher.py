"""The autodev dispatcher: one run per timer tick.

A tick reads the board, works out what to do (decide.py), and does it:
it starts grill sessions for new parent issues, ticket sessions for
tickets with nothing left blocking them, and merge sessions for PRs the
owner has approved with /merge. It keeps tickets' Blocked/Todo status in
step with their blockers, checks finished sessions' work on GitHub, and
tidies up after closed tickets and finished parents. systemd sends
everything it prints to the journal (`journalctl --user -u
autodev-dispatcher`).
"""

import os

from autodev import board, worktrees
from autodev.decide import (IN_REVIEW, CloseIssue, Comment, Finish, Launch,
                            LinkSubIssue, MakeWorktree, PlaceOnBoard,
                            RemoveWorktree, SetStatus, State,
                            is_waiting_ticket, plan)
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
    read_code, state = read_state(github, config, snapshot, registry, log)
    code = code or read_code
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


def read_state(github, config, snapshot, registry, log):
    """Everything decide() needs beyond the board: the sessions, what
    GitHub shows for the ones that ended, which of the waiting tickets'
    blockers are closed, the PRs waiting in review and what was said on
    them, and which tickets have a worktree. Returns (exit code, State)."""
    code = 0
    known = registry.load()
    outcomes = {}
    for s in known:
        if s.running or s.role not in _OUTCOMES:
            continue
        try:
            outcomes[s.issue] = _OUTCOMES[s.role](github, s)
        except GhError as e:
            # Leave this session for the next tick; the others go on.
            print("error: reading #%d's %s outcome: %s" % (s.issue, s.role, e),
                  file=log)
            code = 1
    closed = set()
    on_board = {item.number: item for item in snapshot.items}
    blockers = {b for item in snapshot.items if is_waiting_ticket(item)
                for b in item.blocked_by}
    for number in sorted(blockers):
        if number in on_board:
            is_open = on_board[number].is_open
        else:
            try:
                is_open = github.is_open(number)
            except GhError as e:
                # Counted as open: its tickets wait for a later tick.
                print("error: reading #%d's state: %s" % (number, e), file=log)
                code = 1
                continue
        if not is_open:
            closed.add(number)
    prs = {}
    for item in snapshot.items:
        if item.has_parent and item.is_open and item.status == IN_REVIEW:
            try:
                pr = github.ticket_pr(item.number)
            except GhError as e:
                print("error: reading #%d's PR: %s" % (item.number, e), file=log)
                code = 1
                continue
            if pr is not None:
                prs[item.number] = pr
    return code, State(
        owner=config.owner, bot=config.bot, items=snapshot.items,
        sessions=known, outcomes=outcomes, max_sessions=config.max_sessions,
        closed=frozenset(closed), prs=prs,
        worktrees=frozenset(worktrees.tickets_with_worktrees(config.clone)),
        resume_command=config.resume_command)


# What GitHub shows once a session of each role has ended.
_OUTCOMES = {
    "grill": lambda github, s: github.grill_outcome(
        s.issue, (s.result or {}).get("links", [])),
    "ticket": lambda github, s: github.ticket_outcome(s.issue),
    "merge": lambda github, s: github.merge_outcome(s.pr, s.handled_through),
}


def _carry_out(action, github, registry, config):
    if isinstance(action, SetStatus):
        github.set_status(action.issue, action.status)
    elif isinstance(action, Comment):
        github.comment(action.issue, action.body)
    elif isinstance(action, MakeWorktree):
        worktrees.make(config.clone, action.issue, action.branch)
    elif isinstance(action, RemoveWorktree):
        worktrees.remove(config.clone, action.issue)
    elif isinstance(action, CloseIssue):
        github.close_issue(action.issue)
    elif isinstance(action, Launch):
        registry.start(action.role, action.issue,
                       prompt(config, action.role, action.issue, pr=action.pr,
                              comment=action.handled_through),
                       pr=action.pr, handled_through=action.handled_through)
    elif isinstance(action, LinkSubIssue):
        github.link_sub_issue(action.parent, action.child)
    elif isinstance(action, PlaceOnBoard):
        github.place_on_board(action.issue, action.status)
    elif isinstance(action, Finish):
        registry.finish(action.role, action.issue)
    else:
        raise ValueError("unknown action %r" % (action,))


def prompt(config, role, issue, pr=None, comment=None):
    """The role's prompt with its {issue}, {repo}, {owner}, {bot} and
    {clone} placeholders filled in, and a merge session's {pr} and
    {comment}. Plain replacement, so any other braces in the Markdown are
    left alone."""
    with open(os.path.join(PROMPTS, role + ".md")) as f:
        text = f.read()
    for name, value in (("issue", issue), ("repo", config.repo),
                        ("owner", config.owner), ("bot", config.bot),
                        ("clone", config.clone), ("pr", pr), ("comment", comment)):
        text = text.replace("{%s}" % name, str(value))
    return text


def _describe(action):
    if isinstance(action, Comment):
        return "comment on #%d" % action.issue
    return repr(action)


def _count(n, noun):
    return "%d %s%s" % (n, noun, "" if n == 1 else "s")
