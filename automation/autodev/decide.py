"""What the dispatcher does on a tick, as a pure function.

`decide(state)` looks at everything the dispatcher knows (the board, the
sessions it has started, and what GitHub shows about finished ones) and
returns the list of actions to take. It does no I/O itself: the tick
gathers the State, and an adapter carries the actions out. That split
lets every decision be tested with a hand-built State (tests/test_decide.py).
"""

from dataclasses import dataclass, field
from typing import Optional

from autodev.sessions import TIME_LIMITS
from autodev.worktrees import branch_name

# Board Status options (the board's own names, set up by setup.sh).
TODO = "Todo"
GRILLING = "Grilling"
BLOCKED = "Blocked"
IN_PROGRESS = "In progress"
IN_REVIEW = "In review"
NEEDS_HUMAN = "Needs human"


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
    role: str               # "grill" or "ticket"
    issue: int


@dataclass(frozen=True)
class MakeWorktree:
    """Give a ticket its worktree and branch, unless it already has them."""
    issue: int
    branch: str


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
        if not session.running and session.role in ("grill", "ticket"):
            groups.append(_finished(state, session))
    free = state.max_sessions - sum(1 for s in state.sessions if s.running)
    has_session = {s.issue for s in state.sessions}
    ready = []      # (priority, group) for each group that launches a session
    for item in sorted(state.items, key=lambda i: i.number):
        # A closed issue is resolved, whatever its card still says.
        if item.number in has_session or not item.is_open:
            continue
        if item.has_parent and item.status in (TODO, BLOCKED):
            new = _waiting_ticket(state, item)
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
        problem, actions = (_grill_done if session.role == "grill"
                            else _ticket_done)(session, outcome)
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


def _grill_done(session, outcome):
    """(problem, actions): what a grill that says it's done leads to."""
    if not outcome.design_comment:
        return ("the grill session said it was done, but GitHub shows no "
                "design comment"), []
    if not outcome.tickets:
        return ("the grill session said it was done, but GitHub shows no "
                "tickets"), []
    return None, (_place_tickets(session.issue, outcome.tickets)
                  + [SetStatus(session.issue, IN_PROGRESS)])


def _ticket_done(session, outcome):
    """(problem, actions): what a ticket that says it's done leads to."""
    if outcome.pr is None:
        return ("the ticket session said it was done, but GitHub shows no "
                "open PR from an `issue-%d-*` branch" % session.issue), []
    return None, [SetStatus(session.issue, IN_REVIEW)]


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
