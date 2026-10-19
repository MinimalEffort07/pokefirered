"""What the dispatcher does on a tick, as a pure function.

`decide(state)` looks at everything the dispatcher knows (the board, the
sessions it has started, and what GitHub shows about finished ones) and
returns the list of actions to take. It does no I/O itself: the tick
gathers the State, and an adapter carries the actions out. That split
lets every decision be tested with a hand-built State (tests/test_decide.py).
"""

from dataclasses import dataclass, field
from typing import Optional

# Board Status options (the board's own names, set up by setup.sh).
TODO = "Todo"
GRILLING = "Grilling"
BLOCKED = "Blocked"
IN_PROGRESS = "In progress"
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
class State:
    owner: str              # the human; only their parent issues are run
    items: list             # board.Item for every issue on the board
    sessions: list = field(default_factory=list)
    # GrillOutcome per parent whose grill session has ended
    outcomes: dict = field(default_factory=dict)
    max_sessions: int = 2   # Claude sessions allowed to run at once
    # How the owner resumes a parked session; {session_id} is filled in.
    resume_command: str = "claude --resume {session_id}"


# ── What it can do ────────────────────────────────────────────────────────

@dataclass(frozen=True)
class SetStatus:
    issue: int
    status: str


@dataclass(frozen=True)
class Launch:
    role: str               # "grill" (later tickets add more roles)
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
        if not session.running and session.role == "grill":
            groups.append(_finished_grill(state, session))
    free = state.max_sessions - sum(1 for s in state.sessions if s.running)
    has_session = {s.issue for s in state.sessions}
    for item in sorted(state.items, key=lambda i: i.number):
        if (item.status == TODO and not item.has_parent
                and item.number not in has_session):
            new = _new_parent(state, item)
            launches = sum(1 for a in new if isinstance(a, Launch))
            if launches > free:
                continue    # no free slot: it stays in Todo for a later tick
            free -= launches
            groups.append(new)
    return [g for g in groups if g]


def decide(state):
    """Every action of plan(state), in order."""
    return [action for group in plan(state) for action in group]


def _new_parent(state, item):
    # The repo is public, so anyone can open an issue. Only the owner's
    # parent issues are ever handed to a Claude session (#48, "Identity
    # and trust"); anything else is parked without its text being read.
    if item.author != state.owner:
        return [SetStatus(item.number, NEEDS_HUMAN),
                Comment(item.number, _bot_comment(
                    "Autodev only runs issues written by @%s; restate it as "
                    "your own issue to run it." % state.owner))]
    # Launch first: if the Status update then fails, the session record
    # still stops a second launch on the next tick.
    return [Launch("grill", item.number), SetStatus(item.number, GRILLING)]


def _finished_grill(state, session):
    parent = session.issue
    outcome = state.outcomes.get(parent)
    if outcome is None:
        return []   # the tick couldn't read this from GitHub; next tick retries
    result = session.result
    if result is None:
        problem = ("the grill session ended without reporting a result "
                   "(it crashed or hit its time limit)")
    elif result.get("outcome") != "done":
        problem = "the grill session failed: %s" % result.get("reason", "")
    # A session's own "done" is only believed once GitHub shows the work.
    elif not outcome.design_comment:
        problem = ("the grill session said it was done, but GitHub shows "
                   "no design comment")
    elif not outcome.tickets:
        problem = ("the grill session said it was done, but GitHub shows "
                   "no tickets")
    else:
        return (_place_tickets(parent, outcome.tickets)
                + [SetStatus(parent, IN_PROGRESS), Finish("grill", parent)])
    return [SetStatus(parent, NEEDS_HUMAN),
            Comment(parent, _parked(state, session, problem)),
            Finish("grill", parent)]


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
            state.resume_command.format(session_id=session.session_id)))


def _bot_comment(text):
    return text + "\n\n" + MARKER
