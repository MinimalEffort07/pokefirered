"""What the dispatcher does on a tick, as a pure function.

`decide(state)` looks at everything the dispatcher knows (the board, the
sessions it has started, and what GitHub shows about finished ones) and
returns the list of actions to take. It does no I/O itself: the tick
gathers the State, and an adapter carries the actions out. That split
lets every decision be tested with a hand-built State (tests/test_decide.py).
"""

import re
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

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
PAUSED = "Paused"           # the owner's stop switch (#48, "Board")


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
    pr: Optional[int] = None                # a merge or fix-up session's PR
    handled_through: Optional[int] = None   # the newest comment it answers
    # When it stopped on the Claude usage limit: when the limit resets (as
    # seconds since 1970) and the CLI's message, as sessions.usage_limit
    # read them from its output.
    limit_reset: Optional[float] = None
    limit_note: str = ""


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
    # The owner withdrew that /merge (merge_cancelled says how), so
    # issue.py's merge-pr refused to merge.
    superseded: bool = False
    # The PR is a breaking change and the comment the session answered is a
    # plain /merge, not /merge breaking, so merge-pr refused to merge.
    breaking_unapproved: bool = False


@dataclass(frozen=True)
class FixupOutcome:
    """What GitHub shows for a ticket's PR after its fix-up session ended."""
    replied: bool   # the bot's reply naming the comment it handled is there


@dataclass(frozen=True)
class PrComment:
    """Something the owner, the bot or anyone else said on a PR: a comment
    in its conversation, a review, or an inline review comment."""
    id: int
    author: str
    body: str
    at: str                 # when, as GitHub's ISO time ("2026-10-19T06:01:00Z")
    kind: str = "comment"   # "comment", "review" or "inline comment"
    where: str = ""         # an inline comment's file and line


@dataclass(frozen=True)
class PullRequest:
    """A ticket's open PR: the bot's or the owner's, from the ticket's own
    branch."""
    number: int
    head: str               # its branch
    comments: tuple = ()    # PrComment, oldest first
    opened: str = ""        # when, as GitHub's ISO time
    # Why it is a breaking change (github.breaking), so that only the
    # owner's /merge breaking merges it; "" when it isn't one, or when its
    # comments weren't read.
    breaking: str = ""


@dataclass(frozen=True)
class State:
    owner: str              # the human; only their parent issues are run
    items: list             # board.Item for every issue on the board
    sessions: list = field(default_factory=list)
    # GrillOutcome or TicketOutcome per issue whose session has ended
    outcomes: dict = field(default_factory=dict)
    max_sessions: int = 2   # Claude sessions allowed to run at once
    bot: str = ""           # autodev's own account; its tickets are trusted
    # Issues known to be closed: waiting tickets' blockers, and tickets with
    # a worktree whose card has left the board. One GitHub couldn't report
    # counts as open, so its ticket keeps waiting and its worktree stays.
    closed: frozenset = frozenset()
    # PullRequest per ticket in Todo, In review or Needs human that has one
    # open
    prs: dict = field(default_factory=dict)
    # When autodev last parked each ticket in Needs human that it parked: the
    # time of its newest "parked" comment (PARKED), as GitHub's ISO time
    parked_at: dict = field(default_factory=dict)
    # Tickets that have a worktree
    worktrees: frozenset = frozenset()
    # For each of the owner's open parents in Todo or In progress, the
    # tickets among its sub-issues, as {number: is_open}. The board's
    # "Auto-archive items" workflow archives a ticket's card as it closes,
    # and archived cards aren't read, so this is how a parent's closed
    # tickets are known. A parent GitHub couldn't report this tick is
    # missing.
    sub_issues: dict = field(default_factory=dict)
    # How the owner resumes a parked session; {session_id}, {cwd} and {env}
    # (the session's AUTODEV_* variables, as shell assignments) are filled in.
    resume_command: str = "claude --resume {session_id}"
    now: float = 0.0            # the tick's clock, as seconds since 1970
    paused_until: float = 0.0   # no session starts before this (a usage limit)
    # How many earlier sessions of each (role, issue) crashed
    crashes: dict = field(default_factory=dict)
    # (role, issue) whose last session was re-queued (a crash or the usage
    # limit) and hasn't run again yet
    requeued: frozenset = frozenset()
    # How a time is written in a comment (the machine's local time).
    show_time: Callable = lambda t: time.strftime("%H:%M %Z on %a %d %b",
                                                  time.localtime(t))


# ── What it can do ────────────────────────────────────────────────────────

@dataclass(frozen=True)
class SetStatus:
    issue: int
    status: str


@dataclass(frozen=True)
class Launch:
    role: str               # "grill", "ticket", "merge" or "fixup"
    issue: int
    pr: Optional[int] = None        # a merge or fix-up: the ticket's PR
    # A merge or fix-up: the owner's newest comment it answers. Its reply on
    # the PR says so with a handled-through marker, and later comments are
    # new work.
    handled_through: Optional[int] = None


@dataclass(frozen=True)
class MakeWorktree:
    """Give a ticket its worktree and branch, unless it already has them."""
    issue: int
    branch: str


@dataclass(frozen=True)
class PauseLaunches:
    """Start no session of any role before `until` (#48, "Failure
    handling": usage limit). Kept on disk across ticks."""
    until: float


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
    """Forget a session that has ended, once its outcome is handled. A
    crash is remembered, so a second one parks the work (D20)."""
    role: str
    issue: int
    crashed: bool = False
    requeued: bool = False  # its work goes round again: see State.requeued


# Every comment the bot writes ends with this hidden marker, so a later
# tick can tell autodev's own comments apart from anyone else's.
MARKER = "<!-- autodev -->"


def plan(state):
    """The tick's actions, in groups: one group per finished session or
    board issue, each in the order it must run. The tick skips the rest of
    a group when one of its actions fails, but carries on with the others,
    so one broken issue can't stall the whole board."""
    groups = []
    on_board = {i.number for i in state.items}
    for session in state.sessions:
        if not session.running and session.role in DONE_CHECKS:
            groups.append(_without_gone_cards(on_board, _keep_paused(
                state, session.issue, _finished(state, session))))
    free = state.max_sessions - sum(1 for s in state.sessions if s.running)
    has_session = {s.issue for s in state.sessions}
    # A paused parent stops all of its tickets from starting (a paused
    # ticket stops itself, since nothing starts from Paused). So does one
    # still being grilled: the board adds each ticket as the grill creates
    # it, before the grill has finished.
    held = {i.number for i in state.items if i.status in (PAUSED, GRILLING)
            or (i.status == TODO and ("grill", i.number) in state.requeued)}
    places = _places(state)
    # (priority, group) for each group that launches a session, or holds a
    # slot for one
    ready = []
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
        elif item.has_parent and item.status == NEEDS_HUMAN:
            new = _parked_ticket(state, item)
        elif not item.has_parent and item.status == IN_PROGRESS:
            new = _parent_in_progress(state, item)
        elif item.status == TODO:
            new = _new_parent(state, item)
        else:
            continue
        launch = next((a for a in new if isinstance(a, Launch)), None)
        if launch is None:
            groups.append(new)
        if item.parent in held:
            continue
        if launch is not None:
            ready.append((_priority(launch.role, item, places), new))
        elif _just_unblocked(state, item):
            # The held slot: this tick only moves the ticket back to Todo,
            # and the next one launches it. Meanwhile it takes the free slot
            # it will need, with nothing in it, so a later feature's ticket
            # or a grill can't start there first.
            ready.append((_priority("ticket", item, places), []))
    # A ticket closed by hand usually leaves the board before any tick sees
    # its card closed, so its worktree is found from the worktrees instead.
    groups += [[RemoveWorktree(n)] for n in sorted(state.worktrees - on_board)
               if n in state.closed and n not in has_session]
    # While a usage limit lasts nothing starts, including the moment it is
    # found; the board is still kept up to date.
    limit_ends = max([state.paused_until] + [a.until for g in groups for a in g
                                             if isinstance(a, PauseLaunches)])
    if state.now < limit_ends:
        ready = []
    # The rest stay in Todo for a later tick. A held slot's empty group is
    # dropped below, so it only keeps the slot from going to anything else.
    groups += [new for _, new in sorted(ready, key=lambda r: r[0])[:max(free, 0)]]
    return [g for g in groups if g]


# When there are more sessions to start than free slots, lower ranks go
# first (#48, "Dispatcher"): finishing work beats starting more.
RANKS = {"merge": 0, "fixup": 1, "ticket": 2, "grill": 3}


def _places(state):
    """Each parent's place in line: the number of its lowest ticket, open or
    closed (#102), on the board or among its sub-issues (whose closed cards
    the board archives). Issue numbers only go up, so the parent whose
    first ticket is lowest was grilled first, whatever its own number: an
    old issue added to the board late lines up behind the features already
    in flight. Only tickets the owner or the bot wrote count, so nobody
    else can move a parent up by naming it in an issue of theirs; only
    someone who can write to the repo can link a sub-issue."""
    places = {parent: min(tickets) for parent, tickets in state.sub_issues.items()
              if tickets}
    for i in state.items:
        if i.has_parent and i.parent is not None and _trusted(state, i):
            places[i.parent] = min(places.get(i.parent, i.number), i.number)
    return places


def _priority(role, item, places):
    # Within each rank, features finish in the order they were grilled:
    # the parent's place in line first, then the lowest number. It decides
    # only who gets a free slot, so a later feature's tickets still start
    # while the earlier ones have none ready. A grill, or a ticket whose
    # "## Parent" names no issue, goes by its own number.
    return (RANKS[role], places.get(item.parent, item.number), item.number)


def decide(state):
    """Every action of plan(state), in order."""
    return [action for group in plan(state) for action in group]


def _new_parent(state, item):
    # The repo is public, so anyone can open an issue. Only the owner's
    # parent issues are ever handed to a Claude session (#48, "Identity
    # and trust"); anything else is parked without its text being read.
    if item.author != state.owner:
        return _untrusted(state, item)
    if ("grill", item.number) not in state.requeued:
        if _tickets(state, item.number):
            # It has tickets and its grill wasn't cut short, so it was
            # grilled already: the owner moved it from Paused back to Todo,
            # which means "carry on" (D8).
            return [SetStatus(item.number, IN_PROGRESS)]
        if item.number not in state.sub_issues:
            # Its tickets' cards may all be archived, and GitHub couldn't
            # say this tick. Grilling it again would make every ticket
            # twice, so it waits for a tick that can tell.
            return []
    # Launch first: if the Status update then fails, the session record
    # still stops a second launch on the next tick.
    return [Launch("grill", item.number), SetStatus(item.number, GRILLING)]


def is_waiting_ticket(item):
    """A ticket whose card says it hasn't started: its blockers decide
    what happens next."""
    return item.has_parent and item.status in (TODO, BLOCKED)


def _open_blockers(state, item):
    return [b for b in item.blocked_by if b not in state.closed]


def _trusted(state, item):
    # Grill sessions write tickets as the bot, so the bot's are trusted
    # too. Anyone else's "## Parent" heading doesn't make an issue a ticket.
    return item.author in (state.owner, state.bot)


def _just_unblocked(state, item):
    """A trusted ticket whose card still says Blocked though nothing it
    waits on is open: its last blocker closed since the last tick, usually
    as its sibling's PR merged. This tick moves it back to Todo."""
    return (item.has_parent and item.status == BLOCKED and _trusted(state, item)
            and not _open_blockers(state, item))


def _untrusted(state, item):
    return [SetStatus(item.number, NEEDS_HUMAN),
            Comment(item.number, _bot_comment(
                "Autodev only runs issues written by @%s; restate it as "
                "your own issue to run it." % state.owner))]


def _waiting_ticket(state, item):
    """A ticket in Todo or Blocked: launch it once nothing it waits on is
    open, and keep its card's Status in step with its blockers."""
    if not _trusted(state, item):
        return _untrusted(state, item)
    blocked = bool(_open_blockers(state, item))
    if item.status == BLOCKED:
        # Launched on the next tick, once it is back in Todo.
        return [] if blocked else [SetStatus(item.number, TODO)]
    if blocked:
        return [SetStatus(item.number, BLOCKED)]
    if item.number in state.prs:
        # Its PR is open already (it was paused in review, or re-queued
        # after opening it): back to review, not a second implementation.
        return [SetStatus(item.number, IN_REVIEW)]
    # Launch before the Status update, for the same reason as a grill.
    return [MakeWorktree(item.number, branch_name(item.number, item.title)),
            Launch("ticket", item.number), SetStatus(item.number, IN_PROGRESS)]


# Every reply autodev posts on a PR ends with this, naming the newest of
# the owner's comments it has dealt with (#48, "PR comment bookkeeping").
HANDLED_THROUGH = re.compile(r"<!-- autodev handled-through: (\d+) -->")


def handled_marker(comment_id):
    return "<!-- autodev handled-through: %d -->" % comment_id


def unhandled(owner, bot, comments):
    """The owner's comments on a PR (PrComment, oldest first) that came
    after everything a bot reply says it handled. Nobody else's count."""
    handled_at = ""
    for c in comments:
        match = HANDLED_THROUGH.search(c.body) if c.author == bot else None
        if match:
            # The comment it names, or the reply itself if that's gone.
            named = next((x for x in comments if x.id == int(match.group(1))), c)
            handled_at = max(handled_at, named.at)
    return [c for c in comments if c.author == owner and c.at > handled_at]


def _newest_unhandled(state, pr):
    left = unhandled(state.owner, state.bot, pr.comments)
    return left[-1] if left else None


# The owner's approval of a PR, and the one a breaking change needs (#139).
MERGE = "/merge"
MERGE_BREAKING = "/merge breaking"


def is_merge(comment):
    """Whether a comment is the owner's approval: exactly "/merge" or
    exactly "/merge breaking"."""
    return comment.body.strip() in (MERGE, MERGE_BREAKING)


def approves_breaking(comment):
    """Whether a comment approves a breaking change too: exactly
    "/merge breaking". Only this merges a PR that is one."""
    return comment.body.strip() == MERGE_BREAKING


def merge_cancelled(owner, comments, merge_id):
    """Why the owner's /merge `merge_id` on a PR no longer stands, or None:
    they deleted it, or they have said more since that isn't another
    /merge, which cancels it (D14)."""
    merge = next((c for c in comments if c.id == merge_id), None)
    if merge is None:
        return "the /merge (comment %d) was deleted" % merge_id
    later = [c for c in comments
             if c.author == owner and c.at > merge.at and not is_merge(c)]
    if later:
        return "@%s has said more since the /merge (%s)" % (
            owner, ", ".join("%s %d" % (c.kind, c.id) for c in later))
    return None


def _tickets(state, parent):
    """A parent's tickets, as {number: is_open}: the issues whose
    "## Parent" names it, among its sub-issues on GitHub (the grill links
    every ticket as one) and on the board. The board alone isn't enough:
    it archives a ticket's card as the ticket closes, and archived cards
    aren't read."""
    tickets = dict(state.sub_issues.get(parent, {}))
    tickets.update((i.number, i.is_open) for i in state.items
                   if i.has_parent and i.parent == parent)
    return tickets


def _parent_in_progress(state, item):
    """Once every one of a parent's tickets is closed, autodev is done with
    the parent (#48, "Flow" step 7). Until GitHub has reported its
    sub-issues this tick, it may have an open ticket nobody can see."""
    tickets = _tickets(state, item.number)
    if (item.author != state.owner or item.number not in state.sub_issues
            or not tickets or any(tickets.values())):
        return []
    # Closed before the comment: if closing fails, the next tick tries
    # again without having @mentioned the owner twice.
    return [CloseIssue(item.number),
            Comment(item.number, _bot_comment(
                "@%s every ticket of this issue is closed (%s), so autodev is "
                "done with it." % (state.owner, ", ".join(
                    "#%d" % n for n in sorted(tickets))))),
            SetStatus(item.number, DONE)]


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
    """A ticket whose PR waits for the owner. Their newest unhandled comment
    decides: "/merge" starts a merge session, anything else is feedback and
    starts a fix-up session, which handles every comment queued before it.
    Feedback after a /merge therefore cancels it (D14). A breaking change
    merges only on "/merge breaking": a plain "/merge" gets a reply saying
    so (#141)."""
    pr = state.prs.get(item.number)
    if pr is None:
        return []
    newest = _newest_unhandled(state, pr)
    if newest is None:
        return []
    return _answer(state, item, pr, newest)


def _answer(state, item, pr, comment):
    """Start the session that answers the owner's `comment` on the PR, or,
    for a plain /merge on a breaking change, say why it doesn't merge."""
    if is_merge(comment) and pr.breaking and not approves_breaking(comment):
        # The reply handles that /merge, so it is answered once, and the
        # card waits in review for /merge breaking or feedback. A parked
        # card moves there first: if the reply then fails, the /merge is
        # still unhandled, and the next tick answers it from review.
        moved = [] if item.status == IN_REVIEW else [SetStatus(item.number, IN_REVIEW)]
        return moved + [Comment(pr.number, _breaking_reply(state, pr, comment))]
    role = "merge" if is_merge(comment) else "fixup"
    return [MakeWorktree(item.number, pr.head),
            Launch(role, item.number, pr=pr.number, handled_through=comment.id),
            SetStatus(item.number, IN_PROGRESS)]


def _breaking_reply(state, pr, merge):
    """The answer to the owner's plain /merge `merge` on breaking PR `pr`."""
    return ("@%s this PR is a breaking change (%s), so a plain `/merge` doesn't "
            "merge it. Read its Breaking change section, then comment exactly "
            "`/merge breaking` to approve it and merge, or leave feedback.\n\n%s" % (
                state.owner, pr.breaking, handled_marker(merge.id)))


def _parked_ticket(state, item):
    """A ticket autodev parked in Needs human comes back once the owner has
    dealt with it. Their comment on its PR since it was parked is answered
    as in review (a plain /merge on a breaking change gets its reply, and the
    card moves to review); a PR opened since it was parked (the resumed
    session, or the owner, finished the work) sends it to review. Anything
    from before
    is what it was parked over, so it never starts the same work again. A
    card with no "parked" comment is the owner's own, and someone else's
    ticket stays parked for its author (D2)."""
    since = state.parked_at.get(item.number)
    pr = state.prs.get(item.number)
    if since is None or pr is None or not _trusted(state, item):
        return []
    newest = _newest_unhandled(state, pr)
    if newest is not None and newest.at > since:
        return _answer(state, item, pr, newest)
    if pr.opened > since:
        return [SetStatus(item.number, IN_REVIEW)]
    return []


def _without_gone_cards(on_board, actions):
    """A finished session's actions, less the Status of any card no longer
    on the board (`on_board` holds the issue numbers of the cards there):
    the board archives a ticket's card as it closes, often while its merge
    session is still finishing, and an archived card isn't read, so there
    is nothing to set. Trying failed the rest of the group, Finish
    included, so every tick tried again (2026-10-20)."""
    return [a for a in actions
            if not (isinstance(a, SetStatus) and a.issue not in on_board)]


def _keep_paused(state, issue, actions):
    """A card the owner paused stays Paused when its session ends: the
    session's comments still post, and the D8 rules sort the card out when
    it is resumed. A closed ticket still becomes Done: closed is resolved."""
    if not any(i.number == issue and i.status == PAUSED for i in state.items):
        return actions
    return [a for a in actions if not (isinstance(a, SetStatus)
                                       and a.issue == issue and a.status != DONE)]


def _finished(state, session):
    issue = session.issue
    outcome = state.outcomes.get(issue)
    if outcome is None:
        return []   # the tick couldn't read this from GitHub; next tick retries
    if session.role == "merge" and outcome.merged:
        # Merged is merged, even if the session then failed or ran out of
        # time before it could say so.
        return _merged(state, issue) + [Finish(session.role, issue)]
    if session.role == "merge" and (outcome.superseded or outcome.breaking_unapproved):
        # The owner withdrew their /merge, usually by saying more after it
        # (D14), or it was a plain /merge on a breaking change (#141), so
        # merge-pr refused. Whatever the session then reported, the card goes
        # back to review, where a fix-up picks new comments up, or the reply
        # to a plain /merge explains.
        return _limit_pause(session) + [SetStatus(issue, IN_REVIEW),
                                        Finish(session.role, issue)]
    result = session.result
    if result is None and session.role in ("ticket", "merge", "fixup"):
        # It reported nothing, but its work may be on GitHub anyway: the
        # ticket session opened its PR, or the merge or fix-up session
        # posted its reply, before it crashed or ran out of usage.
        problem, actions = DONE_CHECKS[session.role](state, session, outcome)
        if problem is None:
            return _limit_pause(session) + actions + [Finish(session.role, issue)]
    if result is None and session.limit_reset is not None:
        return _usage_limited(state, session)
    if result is None and session.ended_by != "timeout":
        return _crashed(state, session)
    if result is None:
        problem = _timed_out(session)
    elif result.get("outcome") != "done":
        problem = "%s failed: %s" % (_the_session(session.role),
                                     result.get("reason", ""))
    else:
        # A session's own "done" is only believed once GitHub shows the work.
        problem, actions = DONE_CHECKS[session.role](state, session, outcome)
        if problem is None:
            return actions + [Finish(session.role, issue)]
    return [SetStatus(issue, NEEDS_HUMAN),
            Comment(issue, _parked(state, session, problem)),
            Finish(session.role, issue)]


# Where re-queued work goes back to wait: a merge or fix-up to review,
# where the comment it answers is still the owner's newest unhandled one.
REQUEUE = {"grill": TODO, "ticket": TODO, "merge": IN_REVIEW, "fixup": IN_REVIEW}

# How long after the limit resets sessions start again (#48).
LIMIT_MARGIN = 5 * 60


def _limit_pause(session):
    """Holding every launch back until the usage limit the session hit is
    over, if it hit one."""
    if session.limit_reset is None:
        return []
    return [PauseLaunches(session.limit_reset + LIMIT_MARGIN)]


def _the_session(role):
    """How comments name a role's session: "the fix-up session"."""
    return "the %s session" % {"fixup": "fix-up"}.get(role, role)


def _requeue(state, session, says, crashed=False):
    """Send the session's work round again: back to where it waits, then a
    comment (status first, so a retry of this group never posts the
    comment twice). `says` is the comment for a card that is running and
    for one the owner paused, which stays Paused."""
    issue, role = session.issue, session.role
    paused = any(i.number == issue and i.status == PAUSED for i in state.items)
    return [SetStatus(issue, REQUEUE[role]),
            Comment(issue, _bot_comment(says[paused])),
            Finish(role, issue, crashed=crashed, requeued=True)]


def _usage_limited(state, session):
    until = session.limit_reset + LIMIT_MARGIN
    hit = ("@%s autodev hit the Claude usage limit (%s said: "
           "\u201c%s\u201d), so it is paused until %s." % (
               state.owner, _the_session(session.role), session.limit_note,
               state.show_time(until)))
    issue = session.issue
    return [PauseLaunches(until)] + _requeue(state, session, (
        "%s #%d goes back to %s and starts again then." % (
            hit, issue, REQUEUE[session.role]),
        "%s #%d stays Paused; it starts again once you move it back to Todo "
        "and the pause is over." % (hit, issue)))


def _crashed(state, session):
    """No report, no usage limit, not the time limit: the session crashed,
    was stopped, or its unit vanished in a reboot. The first time, the
    work is re-queued; the second time for that role and issue, a human
    looks (#48, "Failure handling")."""
    issue, role = session.issue, session.role
    how = ("its unit vanished, e.g. in a reboot" if session.ended_by is None
           else "systemd says \u201c%s\u201d" % session.ended_by)
    if state.crashes.get((role, issue), 0) == 0:
        ended = "%s ended without reporting a result (%s)" % (
            _the_session(role).capitalize(), how)
        return _requeue(state, session, (
            "%s, so autodev will run it once more." % ended,
            "%s. #%d stays Paused; it runs once more when you move it back to "
            "Todo." % (ended, issue)), crashed=True)
    return [SetStatus(issue, NEEDS_HUMAN),
            Comment(issue, _parked(state, session, "%s crashed a second time "
                                   "(%s)" % (_the_session(role), how))),
            Finish(role, issue, crashed=True)]


def _timed_out(session):
    """Why a session that reported nothing at its time limit needs a human
    (crashes and usage limits are handled before this)."""
    return "%s hit its %s time limit" % (
        _the_session(session.role), _duration(TIME_LIMITS[session.role]))


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


def _merged(state, issue):
    """The ticket's PR is merged: the ticket is done."""
    # "Fixes #T" normally closed the ticket as the PR merged.
    item = next((i for i in state.items if i.number == issue), None)
    close = [CloseIssue(issue)] if item is None or item.is_open else []
    return close + [RemoveWorktree(issue), SetStatus(issue, DONE)]


def _merge_done(state, session, outcome):
    """(problem, actions): what a merge that says it's done, but whose PR
    isn't merged (_finished checks that first), leads to."""
    if outcome.replied:
        # It resolved a conflict on the branch: the owner's next /merge
        # approves that resolution too.
        return None, [SetStatus(session.issue, IN_REVIEW)]
    return ("the merge session said it was done, but GitHub shows PR #%d "
            "neither merged nor answered about a conflict" % session.pr), []


def _fixup_done(state, session, outcome):
    """(problem, actions): what a fix-up that says it's done leads to. Its
    reply is what the owner reads before their next /merge; commits are
    optional, since a question needs only an answer."""
    if outcome.replied:
        return None, [SetStatus(session.issue, IN_REVIEW)]
    return ("the fix-up session said it was done, but GitHub shows no reply "
            "on PR #%d marked as handling comment %d" % (
                session.pr, session.handled_through)), []


# How each role's "done" is checked against what GitHub shows.
DONE_CHECKS = {"grill": _grill_done, "ticket": _ticket_done,
                "merge": _merge_done, "fixup": _fixup_done}


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


# How every comment that parks an issue in Needs human starts, after the
# @mention; the dispatcher finds when a ticket was parked by it.
PARKED = "autodev parked this issue"


# How a parked issue comes back, by role: a grill's parent only when the
# owner moves it, a ticket also by itself (_parked_ticket).
HAND_BACK = {
    "grill": "Once it's sorted, move the card back to `Todo`.",
    "ticket": ("Once it's sorted, it comes back by itself when its PR is "
               "opened or you comment on its PR, or move the card back to `Todo`."),
}


def _parked(state, session, problem):
    return _bot_comment(
        "@%s %s: %s\n\n"
        "To pick up where %s stopped: `%s`\n\n%s" % (
            state.owner, PARKED, problem, _the_session(session.role),
            state.resume_command.format(session_id=session.session_id,
                                        cwd=session.cwd,
                                        env=_session_env(session)),
            HAND_BACK.get(session.role, HAND_BACK["ticket"])))


def _session_env(session):
    """The variables a session's issue.py reads to know which session it is
    (the dispatcher sets them on its unit), as shell assignments, so a
    resumed session can finish its work through issue.py too."""
    env = "AUTODEV_ROLE=%s AUTODEV_ISSUE=%d" % (session.role, session.issue)
    if session.pr is not None:
        env += " AUTODEV_PR=%d" % session.pr
    if session.handled_through is not None:
        env += " AUTODEV_HANDLED_THROUGH=%d" % session.handled_through
    return env


def _bot_comment(text):
    return text + "\n\n" + MARKER
