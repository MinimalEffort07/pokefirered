"""The dispatcher's decisions: given what it knows, exactly what it does.

`decide()` is a pure function, so each test builds a State by hand and
checks the exact list of actions. The D-numbers are the scenarios agreed
on issue #48.
"""

import unittest

from autodev.board import Item
from autodev.decide import (
    Comment, Finish, GrillOutcome, Launch, LinkSubIssue, MakeWorktree,
    PlaceOnBoard, Session, SetStatus, State, Ticket, TicketOutcome, decide,
    plan,
)

OWNER = "MinimalEffort07"
BOT = "MinimalEffort07-Autodev"


def item(number, status="Todo", author=OWNER, has_parent=False, is_open=True):
    return Item(number=number, author=author, status=status,
                has_parent=has_parent, is_open=is_open)


def ticket(number, status="Todo", parent=67, blocked_by=(), author=BOT,
           title="Do the thing", is_open=True):
    return Item(number=number, author=author, status=status, has_parent=True,
                title=title, parent=parent, blocked_by=tuple(blocked_by),
                is_open=is_open)


def state(items=(), sessions=(), outcomes=None, max_sessions=2, closed=()):
    return State(owner=OWNER, bot=BOT, items=list(items),
                 sessions=list(sessions), outcomes=outcomes or {},
                 max_sessions=max_sessions, closed=frozenset(closed))


class Plans(unittest.TestCase):
    def test_actions_come_in_one_group_per_issue_or_session(self):
        # The tick runs each group in order and, if one action fails, skips
        # the rest of that group only, so one broken issue can't stall the
        # others on every tick.
        outcome = GrillOutcome(design_comment=True, tickets=[
            Ticket(70, linked=False, on_board=True, status="Todo",
                   open_blockers=False)])
        groups = plan(State(
            owner=OWNER, items=[item(60, status="Grilling"), item(61)],
            sessions=[Session("grill", 60, "sid-60", running=False,
                              result={"outcome": "done"})],
            outcomes={60: outcome}))
        self.assertEqual(groups, [
            [LinkSubIssue(60, 70), SetStatus(60, "In progress"),
             Finish("grill", 60)],
            [Launch("grill", 61), SetStatus(61, "Grilling")],
        ])


class ParentIssues(unittest.TestCase):
    def test_D1_the_owners_parent_issue_in_todo_is_grilled(self):
        # The session starts before the card moves, so a failed Status
        # update can't leave a Grilling card with nothing running.
        self.assertEqual(decide(state([item(61)])), [
            Launch("grill", 61),
            SetStatus(61, "Grilling"),
        ])

    def test_an_issue_that_already_has_a_session_is_not_launched_again(self):
        # Its card is still Todo because last tick's Status update failed.
        running = [Session("grill", 61, "sid-61", running=True)]
        self.assertEqual(decide(state([item(61)], sessions=running)), [])

    def test_D2_someone_elses_issue_is_parked_with_an_explanation(self):
        # The repo is public: an issue written by anyone but the owner never
        # reaches a Claude session. Not even one written by the bot itself.
        expected_body = (
            "Autodev only runs issues written by @MinimalEffort07; restate it "
            "as your own issue to run it.\n\n<!-- autodev -->")
        for author in ("someone-else", BOT):
            with self.subTest(author=author):
                self.assertEqual(decide(state([item(64, author=author)])), [
                    SetStatus(64, "Needs human"),
                    Comment(64, expected_body),
                ])

    def test_a_parent_already_grilling_is_left_alone(self):
        self.assertEqual(decide(state([item(61, status="Grilling")])), [])

    def test_no_more_than_max_sessions_run_at_once(self):
        # Oldest issue first. #63 stays in Todo for a later tick.
        self.assertEqual(decide(state([item(63), item(61), item(62)])), [
            Launch("grill", 61), SetStatus(61, "Grilling"),
            Launch("grill", 62), SetStatus(62, "Grilling"),
        ])

    def test_running_sessions_count_towards_the_limit(self):
        busy = [Session("grill", 60, "sid-60", running=True)]
        self.assertEqual(
            decide(state([item(60, status="Grilling"), item(61), item(62)],
                         sessions=busy)),
            [Launch("grill", 61), SetStatus(61, "Grilling")])


class Tickets(unittest.TestCase):
    def test_D3_a_todo_ticket_with_every_blocker_closed_is_launched(self):
        # The worktree comes first: the session runs inside it. The branch
        # name is the ticket's number and the first words of its title.
        t = ticket(68, blocked_by=[55], title=(
            "Note the grill path's live check at the end of automation/README.md"))
        self.assertEqual(decide(state([t], closed=[55])), [
            MakeWorktree(68, "issue-68-note-the-grill-paths-live"),
            Launch("ticket", 68),
            SetStatus(68, "In progress"),
        ])

    def test_D4_a_ticket_with_an_open_blocker_waits_in_blocked(self):
        # #55 is closed but #54 isn't. An issue the tick couldn't look up
        # isn't in `closed` either, so it keeps the ticket waiting too.
        t = ticket(70, blocked_by=[54, 55])
        self.assertEqual(decide(state([t], closed=[55])), [
            SetStatus(70, "Blocked"),
        ])

    def test_D5_when_its_last_blocker_closes_a_ticket_goes_back_to_todo(self):
        # #70 waited on #55 alone; #71 still waits on #54 as well. #70 is
        # launched on the next tick, by the D3 rule.
        self.assertEqual(decide(state([ticket(70, "Blocked", blocked_by=[55]),
                                       ticket(71, "Blocked", blocked_by=[54, 55])],
                                      closed=[55])), [
            SetStatus(70, "Todo"),
        ])

    def test_D2_someone_elses_ticket_is_parked_but_the_bots_and_owners_run(self):
        # Grill sessions write tickets as the bot, so those are trusted;
        # anyone else's "## Parent" heading doesn't make an issue one.
        parked = [SetStatus(72, "Needs human"), Comment(72, (
            "Autodev only runs issues written by @MinimalEffort07; restate it "
            "as your own issue to run it.\n\n<!-- autodev -->"))]
        for status in ("Todo", "Blocked"):
            with self.subTest(status=status):
                self.assertEqual(decide(state([ticket(
                    72, status, author="someone-else")])), parked)
        for author in (BOT, OWNER):
            with self.subTest(author=author):
                self.assertEqual(decide(state([ticket(72, author=author)]))[1],
                                 Launch("ticket", 72))

    def test_a_closed_issue_still_in_todo_is_never_launched(self):
        # Closing an issue is how the owner skips it (#48, "Board"); its
        # card may still say Todo or Blocked.
        self.assertEqual(decide(state([
            item(61, is_open=False), ticket(68, is_open=False),
            ticket(70, "Blocked", is_open=False)])), [])

    def test_D6_free_slots_go_to_the_top_items_by_priority(self):
        # Tickets before grills, and among tickets the oldest parent first:
        # #81 (parent #74), then #79 (parent #76). Parent #61 waits.
        self.assertEqual(decide(state([item(61), ticket(79, parent=76),
                                       ticket(81, parent=74)])), [
            MakeWorktree(81, "issue-81-do-the-thing"),
            Launch("ticket", 81), SetStatus(81, "In progress"),
            MakeWorktree(79, "issue-79-do-the-thing"),
            Launch("ticket", 79), SetStatus(79, "In progress"),
        ])


RESUME = "claude --resume {session_id}"


def finished_grill(result):
    return Session("grill", 61, "sid-61", running=False, result=result)


def needs_human(reason):
    return Comment(61, (
        "@MinimalEffort07 autodev parked this issue: %s\n\n"
        "To pick up where the grill session stopped: `claude --resume sid-61`"
        "\n\n<!-- autodev -->") % reason)


class FinishedGrills(unittest.TestCase):
    def run_finished(self, result, outcome):
        return decide(State(
            owner=OWNER, items=[item(61, status="Grilling")],
            sessions=[finished_grill(result)],
            outcomes={61: outcome} if outcome else {},
            resume_command=RESUME))

    def test_D15_a_confirmed_grill_moves_the_parent_to_in_progress(self):
        outcome = GrillOutcome(design_comment=True, tickets=[
            Ticket(70, linked=True, on_board=True, status="Todo",
                   open_blockers=False)])
        self.assertEqual(self.run_finished({"outcome": "done"}, outcome), [
            SetStatus(61, "In progress"),
            Finish("grill", 61),
        ])

    def test_D24_every_ticket_ends_up_linked_and_on_the_board(self):
        # #70 is already right. #71 was never linked or added, and waits on
        # an open blocker. GitHub put #72 in Todo, but it is blocked too.
        outcome = GrillOutcome(design_comment=True, tickets=[
            Ticket(70, linked=True, on_board=True, status="Todo",
                   open_blockers=False),
            Ticket(71, linked=False, on_board=False, status=None,
                   open_blockers=True),
            Ticket(72, linked=True, on_board=True, status="Todo",
                   open_blockers=True),
        ])
        self.assertEqual(self.run_finished({"outcome": "done"}, outcome), [
            LinkSubIssue(61, 71),
            PlaceOnBoard(71, "Blocked"),
            PlaceOnBoard(72, "Blocked"),
            SetStatus(61, "In progress"),
            Finish("grill", 61),
        ])

    def test_D16_a_done_claim_github_does_not_back_up_needs_a_human(self):
        for outcome, missing in [
            (GrillOutcome(design_comment=False, tickets=[
                Ticket(70, True, True, "Todo", False)]),
             "the grill session said it was done, but GitHub shows no "
             "design comment"),
            (GrillOutcome(design_comment=True, tickets=[]),
             "the grill session said it was done, but GitHub shows no "
             "tickets"),
        ]:
            with self.subTest(missing=missing):
                self.assertEqual(
                    self.run_finished({"outcome": "done"}, outcome), [
                        SetStatus(61, "Needs human"),
                        needs_human(missing),
                        Finish("grill", 61),
                    ])

    def test_a_grill_that_reports_failure_needs_a_human(self):
        result = {"outcome": "failed", "reason": "the issue names no feature"}
        self.assertEqual(self.run_finished(result, GrillOutcome(False, [])), [
            SetStatus(61, "Needs human"),
            needs_human("the grill session failed: the issue names no feature"),
            Finish("grill", 61),
        ])

    def test_a_grill_that_ends_without_a_result_needs_a_human(self):
        # systemd says how the unit ended: "timeout" means it was stopped at
        # its 60-minute limit; anything else is a crash or a manual stop.
        for ended_by, reason in [
            ("timeout", "the grill session hit its 60-minute time limit"),
            ("exit-code", "the grill session ended without reporting a "
                          "result (it crashed or was stopped)"),
        ]:
            with self.subTest(ended_by=ended_by):
                self.assertEqual(decide(State(
                    owner=OWNER, items=[item(61, status="Grilling")],
                    sessions=[Session("grill", 61, "sid-61", running=False,
                                      ended_by=ended_by)],
                    outcomes={61: GrillOutcome(False, [])},
                    resume_command=RESUME)), [
                    SetStatus(61, "Needs human"),
                    needs_human(reason),
                    Finish("grill", 61),
                ])


def finished_ticket(result, ended_by="success"):
    return Session("ticket", 68, "sid-68", running=False, result=result,
                   ended_by=ended_by, cwd="/clone/.worktrees/issue-68")


class FinishedTickets(unittest.TestCase):
    def run_finished(self, session, pr):
        return decide(State(
            owner=OWNER, bot=BOT, items=[ticket(68, "In progress")],
            sessions=[session], outcomes={68: TicketOutcome(pr=pr)},
            resume_command="cd {cwd} && claude --resume {session_id}"))

    def parked(self, reason):
        # The session ran in the ticket's worktree, and `claude --resume`
        # only finds a session from the folder it ran in.
        return [SetStatus(68, "Needs human"), Comment(68, (
            "@MinimalEffort07 autodev parked this issue: %s\n\n"
            "To pick up where the ticket session stopped: "
            "`cd /clone/.worktrees/issue-68 && claude --resume sid-68`"
            "\n\n<!-- autodev -->") % reason), Finish("ticket", 68)]

    def test_D15_a_done_ticket_whose_pr_github_shows_goes_to_in_review(self):
        done = finished_ticket({"outcome": "done", "links": [90]})
        self.assertEqual(self.run_finished(done, pr=90), [
            SetStatus(68, "In review"),
            Finish("ticket", 68),
        ])

    def test_D16_a_done_claim_with_no_pr_on_github_needs_a_human(self):
        done = finished_ticket({"outcome": "done", "links": [90]})
        self.assertEqual(self.run_finished(done, pr=None), self.parked(
            "the ticket session said it was done, but GitHub shows no open "
            "PR from an `issue-68-*` branch"))

    def test_D17_a_session_stopped_at_its_time_limit_needs_a_human(self):
        # systemd reports how the unit ended; "timeout" is RuntimeMaxSec.
        timed_out = finished_ticket(None, ended_by="timeout")
        self.assertEqual(self.run_finished(timed_out, pr=None), self.parked(
            "the ticket session hit its 3-hour time limit"))

    def test_a_ticket_session_that_reports_failure_needs_a_human(self):
        failed = finished_ticket({"outcome": "failed", "reason": "build broke"})
        self.assertEqual(self.run_finished(failed, pr=None), self.parked(
            "the ticket session failed: build broke"))


if __name__ == "__main__":
    unittest.main()
