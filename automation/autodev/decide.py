"""What the dispatcher does on a tick, as a pure function.

`decide(state)` looks at everything the dispatcher knows (the board, the
sessions it has started, and what GitHub shows about finished ones) and
returns the list of actions to take. It does no I/O itself: the tick
gathers the State, and an adapter carries the actions out. That split
lets every decision be tested with a hand-built State (tests/test_decide.py).
"""

import re
from dataclasses import dataclass, field
from typing import Optional

from autodev.sessions import TIME_LIMITS
from autodev.worktrees import branch_name, branch_prefix

# Board Status options (the board's own names, set up by setup.sh).
TODO = "Todo"
GRILLING = "Grilling"
BLOCKED = "Blocked"
IN_PROGRESS = "In progress"
IN_REVIEW = "In review"
NEEDS_HUMAN = "Needs human"
DONE = "Done"


# ── What the dispatcher knows ─────────────────────────────────────────────

@dataclass(frozen=True)
class Session:
    """A Claude session the dispatcher started (one per role and issue)."""
    role: str
    issue: int
    session_id: str         # picked before launch, for `claude --resume`
    running: bool           # its systemd unit is still active
    result: Optional[dict] = None   # its final structured result, if any
    # How systemd says the unit ended once it has: "success", "exit-code",
    # "timeout" (its time limit), "signal"... None while it runs.
    ended_by: Optional[str] = None
    cwd: str = ""           # the folder it ran in; `claude --resume` needs it
    pr: Optional[int] = None                # a merge session's PR
    handled_through: Optional[int] = None   # the comment a merge answers


@dataclass(frozen=True)
class Ticket:
    """One ticket a grill session says it created, as GitHub shows it."""
    number: int
    linked: bool            # a native sub-issue of the parent
    on_board: bool
    status: Optional[str]   # its board Status, if it is on the board
    open_blockers: bool     # an issue under its "## Blocked by" is open


@dataclass(frozen=True)
class GrillOutcome:
    """What GitHub shows on a parent after its grill session ended."""
    design_comment: bool    # the bot posted the design comment
    tickets: list           # Ticket for each ticket the session reported


@dataclass(frozen=True)
class TicketOutcome:
    """What GitHub shows for a ticket after its ticket session ended."""
    pr: Optional[int]       # the bot's open PR from an issue-<T>-* branch


@dataclass(frozen=True)
class MergeOutcome:
    """What GitHub shows for a ticket's PR after its merge session ended."""
    merged: bool
    replied: bool   # the bot's reply naming the /merge it handled is there


@dataclass(frozen=True)
class PrComment:
    """Something the owner, the bot or anyone else said on a PR: a comment
    in its conversation, a review, or an inline review comment."""
    id: int
    author: str
    body: str
    at: str                 # when, as GitHub's ISO time ("2026-10-19T06:01:00Z")


@dataclass(frozen=True)
class PullRequest:
    """A ticket's PR: the bot's, from the ticket's own branch."""
    number: int
    head: str               # its branch
    merged: bool
    comments: tuple = ()    # PrComment, oldest first


@dataclass(frozen=True)
class State:
    owner: str              # the human; only their parent issues are run
    items: list             # board.Item for every issue on the board
    sessions: list = field(default_factory=list)
    # GrillOutcome or TicketOutcome per issue whose session has ended
    outcomes: dict = field(default_factory=dict)
    max_sessions: int = 2   # Claude sessions allowed to run at once
    bot: str = ""           # autodev's own account; its tickets are trusted
    # Issues known to be closed. A ticket's blocker counts as open unless
    # it is in here, so one GitHub couldn't report keeps the ticket waiting.
    closed: frozenset = frozenset()
    # PullRequest per ticket waiting in review
    prs: dict = field(default_factory=dict)
    # Tickets that have a worktree
    worktrees: frozenset = frozenset()
    # How the owner resumes a parked session; {session_id} and {cwd} are
    # filled in.
    resume_command: str = "claude --resume {session_id}"


# ── What it can do ────────────────────────────────────────────────────────

@dataclass(frozen=True)
class SetStatus:
    issue: int
    status: str


@dataclass(frozen=True)
class Launch:
    role: str               # "grill", "ticket" or "merge"
    issue: int
    pr: Optional[int] = None        # a merge: the ticket's PR
    # A merge: the owner's comment it answers. Its reply on the PR says so
    # with a handled-through marker, and later comments are new work.
    handled_through: Optional[int] = None


@dataclass(frozen=True)
class MakeWorktree:
    """Give a ticket its worktree and branch, unless it already has them."""
    issue: int
    branch: str


@dataclass(frozen=True)
class CloseIssue:
    issue: int


@dataclass(frozen=True)
class RemoveWorktree:
    """Delete a ticket's worktree and its local branch, if they exist."""
    issue: int


@dataclass(frozen=True)
class Comment:
    issue: int
    body: str


@dataclass(frozen=True)
class LinkSubIssue:
    parent: int
    child: int


@dataclass(frozen=True)
class PlaceOnBoard:
    """Add the issue to the board if it isn't there, and set its Status."""
    issue: int
    status: str


@dataclass(frozen=True)
class Finish:
    """Forget a session that has ended, once its outcome is handled."""
    role: str
    issue: int


# Every comment the bot writes ends with this hidden marker, so a later
# tick can tell autodev's own comments apart from anyone else's.
MARKER = "<!-- autodev -->"


def plan(state):
    """The tick's actions, in groups: one group per finished session or
    board issue, each in the order it must run. The tick skips the rest of
    a group when one of its actions fails, but carries on with the others,
    so one broken issue can't stall the whole board."""
    groups = []
    for session in state.sessions:
        if not session.running and session.role in _DONE_CHECKS:
            groups.append(_finished(state, session))
    free = state.max_sessions - sum(1 for s in state.sessions if s.running)
    has_session = {s.issue for s in state.sessions}
    ready = []      # (priority, group) for each group that launches a session
    for item in sorted(state.items, key=lambda i: i.number):
        if item.number in has_session:
            continue
        # A closed issue is resolved, whatever its card still says.
        if not item.is_open:
            if item.has_parent:
                groups.append(_closed_ticket(state, item))
            continue
        if is_waiting_ticket(item):
            new = _waiting_ticket(state, item)
        elif item.has_parent and item.status == IN_REVIEW:
            new = _in_review(state, item)
        elif not item.has_parent and item.status == IN_PROGRESS:
            new = _parent_in_progress(state, item)
        elif item.status == TODO:
            new = _new_parent(state, item)
        else:
            continue
        launch = next((a for a in new if isinstance(a, Launch)), None)
        if launch is None:
            groups.append(new)
        else:
            ready.append((_priority(launch.role, item), new))
    # The rest stay in Todo for a later tick.
    groups += [new for _, new in sorted(ready, key=lambda r: r[0])[:max(free, 0)]]
    return [g for g in groups if g]


# When there are more sessions to start than free slots, lower ranks go
# first (#48, "Dispatcher"). Merge and fix-up sessions come with #57 and #59.
RANKS = {"merge": 0, "fixup": 1, "ticket": 2, "grill": 3}


def _priority(role, item):
    # Among tickets the oldest parent goes first, so one feature's tickets
    # finish before the next feature's start; then the lowest number.
    return (RANKS[role], item.parent or item.number, item.number)


def decide(state):
    """Every action of plan(state), in order."""
    return [action for group in plan(state) for action in group]


def _new_parent(state, item):
    # The repo is public, so anyone can open an issue. Only the owner's
    # parent issues are ever handed to a Claude session (#48, "Identity
    # and trust"); anything else is parked without its text being read.
    if item.author != state.owner:
        return _untrusted(state, item)
    # Launch first: if the Status update then fails, the session record
    # still stops a second launch on the next tick.
    return [Launch("grill", item.number), SetStatus(item.number, GRILLING)]


def is_waiting_ticket(item):
    """A ticket whose card says it hasn't started: its blockers decide
    what happens next."""
    return item.has_parent and item.status in (TODO, BLOCKED)


def _open_blockers(state, item):
    return [b for b in item.blocked_by if b not in state.closed]


def _untrusted(state, item):
    return [SetStatus(item.number, NEEDS_HUMAN),
            Comment(item.number, _bot_comment(
                "Autodev only runs issues written by @%s; restate it as "
                "your own issue to run it." % state.owner))]


def _waiting_ticket(state, item):
    """A ticket in Todo or Blocked: launch it once nothing it waits on is
    open, and keep its card's Status in step with its blockers."""
    # Grill sessions write tickets as the bot, so the bot's are trusted
    # too. Anyone else's "## Parent" heading doesn't make an issue a ticket.
    if item.author not in (state.owner, state.bot):
        return _untrusted(state, item)
    blocked = bool(_open_blockers(state, item))
    if item.status == BLOCKED:
        # Launched on the next tick, once it is back in Todo.
        return [] if blocked else [SetStatus(item.number, TODO)]
    if blocked:
        return [SetStatus(item.number, BLOCKED)]
    # Launch before the Status update, for the same reason as a grill.
    return [MakeWorktree(item.number, branch_name(item.number, item.title)),
            Launch("ticket", item.number), SetStatus(item.number, IN_PROGRESS)]


# Every reply autodev posts on a PR ends with this, naming the newest of
# the owner's comments it has dealt with (#48, "PR comment bookkeeping").
HANDLED_THROUGH = re.compile(r"<!-- autodev handled-through: (\d+) -->")


def _newest_unhandled(state, pr):
    """The owner's newest comment on the PR that came after everything a
    bot reply says it handled, or None. Nobody else's comments count."""
    handled_at = ""
    for c in pr.comments:
        match = HANDLED_THROUGH.search(c.body) if c.author == state.bot else None
        if match:
            # The comment it names, or the reply itself if that's gone.
            named = next((x for x in pr.comments
                          if x.id == int(match.group(1))), c)
            handled_at = max(handled_at, named.at)
    unhandled = [c for c in pr.comments
                 if c.author == state.owner and c.at > handled_at]
    return unhandled[-1] if unhandled else None


def _parent_in_progress(state, item):
    """Once every one of a parent's tickets is closed, autodev is done with
    the parent (#48, "Flow" step 7). Its tickets are the board's issues
    whose "## Parent" names it: the grill puts every ticket on the board."""
    tickets = [i for i in state.items if i.has_parent and i.parent == item.number]
    if not tickets or any(t.is_open for t in tickets):
        return []
    return [Comment(item.number, _bot_comment(
                "@%s every ticket of this issue is closed (%s), so autodev is "
                "done with it." % (state.owner, ", ".join(
                    "#%d" % t.number for t in sorted(tickets, key=lambda t: t.number))))),
            CloseIssue(item.number), SetStatus(item.number, DONE)]


def _closed_ticket(state, item):
    """However it was closed (its PR merged, or the owner closed it), a
    closed ticket is done: its worktree goes and its card says Done. The
    board's own "Item closed" workflow may have set Done already."""
    actions = []
    if item.number in state.worktrees:
        actions.append(RemoveWorktree(item.number))
    if item.status != DONE:
        actions.append(SetStatus(item.number, DONE))
    return actions


def _in_review(state, item):
    """A ticket whose PR waits for the owner: their "/merge" starts a merge
    session, as long as nothing they said after it is still unhandled."""
    pr = state.prs.get(item.number)
    if pr is None or pr.merged:
        return []
    newest = _newest_unhandled(state, pr)
    if newest is None or newest.body.strip() != "/merge":
        return []   # feedback waits for the fix-up role (#59)
    return [MakeWorktree(item.number, pr.head),
            Launch("merge", item.number, pr=pr.number,
                   handled_through=newest.id),
            SetStatus(item.number, IN_PROGRESS)]


def _finished(state, session):
    issue = session.issue
    outcome = state.outcomes.get(issue)
    if outcome is None:
        return []   # the tick couldn't read this from GitHub; next tick retries
    result = session.result
    if result is None:
        problem = _ended_without_result(session)
    elif result.get("outcome") != "done":
        problem = "the %s session failed: %s" % (session.role,
                                                 result.get("reason", ""))
    else:
        # A session's own "done" is only believed once GitHub shows the work.
        problem, actions = _DONE_CHECKS[session.role](state, session, outcome)
        if problem is None:
            return actions + [Finish(session.role, issue)]
    return [SetStatus(issue, NEEDS_HUMAN),
            Comment(issue, _parked(state, session, problem)),
            Finish(session.role, issue)]


def _ended_without_result(session):
    if session.ended_by == "timeout":
        return "the %s session hit its %s time limit" % (
            session.role, _duration(TIME_LIMITS[session.role]))
    return ("the %s session ended without reporting a result (it crashed "
            "or was stopped)" % session.role)


def _duration(seconds):
    # As #48's Sessions table words them: "60-minute", "90-minute", "3-hour".
    if seconds > 3600 and seconds % 3600 == 0:
        return "%d-hour" % (seconds // 3600)
    return "%d-minute" % (seconds // 60)


def _grill_done(state, session, outcome):
    """(problem, actions): what a grill that says it's done leads to."""
    if not outcome.design_comment:
        return ("the grill session said it was done, but GitHub shows no "
                "design comment"), []
    if not outcome.tickets:
        return ("the grill session said it was done, but GitHub shows no "
                "tickets"), []
    return None, (_place_tickets(session.issue, outcome.tickets)
                  + [SetStatus(session.issue, IN_PROGRESS)])


def _ticket_done(state, session, outcome):
    """(problem, actions): what a ticket that says it's done leads to."""
    if outcome.pr is None:
        return ("the ticket session said it was done, but GitHub shows no "
                "open PR from an `%s*` branch" % branch_prefix(session.issue)), []
    return None, [SetStatus(session.issue, IN_REVIEW)]


def _merge_done(state, session, outcome):
    """(problem, actions): what a merge that says it's done leads to."""
    issue = session.issue
    if outcome.merged:
        # "Fixes #T" normally closed the ticket as the PR merged.
        item = next((i for i in state.items if i.number == issue), None)
        close = [CloseIssue(issue)] if item is None or item.is_open else []
        return None, close + [RemoveWorktree(issue), SetStatus(issue, DONE)]
    if outcome.replied:
        # It resolved a conflict on the branch: the owner's next /merge
        # approves that resolution too.
        return None, [SetStatus(issue, IN_REVIEW)]
    return ("the merge session said it was done, but GitHub shows PR #%d "
            "neither merged nor answered about a conflict" % session.pr), []


# How each role's "done" is checked against what GitHub shows.
_DONE_CHECKS = {"grill": _grill_done, "ticket": _ticket_done,
                "merge": _merge_done}


def _place_tickets(parent, tickets):
    """Each ticket is a sub-issue of its parent and sits on the board:
    Blocked while an issue it waits on is open, otherwise Todo."""
    actions = []
    for t in tickets:
        if not t.linked:
            actions.append(LinkSubIssue(parent, t.number))
        status = BLOCKED if t.open_blockers else TODO
        if not t.on_board or t.status != status:
            actions.append(PlaceOnBoard(t.number, status))
    return actions


def _parked(state, session, problem):
    return _bot_comment(
        "@%s autodev parked this issue: %s\n\n"
        "To pick up where the %s session stopped: `%s`" % (
            state.owner, problem, session.role,
            state.resume_command.format(session_id=session.session_id,
                                        cwd=session.cwd)))


def _bot_comment(text):
    return text + "\n\n" + MARKER
