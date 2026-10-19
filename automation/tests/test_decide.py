"""The dispatcher's decisions: given what it knows, exactly what it does.

`decide()` is a pure function, so each test builds a State by hand and
checks the exact list of actions. The D-numbers are the scenarios agreed
on issue #48.
"""

import unittest

from autodev.board import Item
from autodev.decide import (
    Comment, Finish, GrillOutcome, Launch, LinkSubIssue, PlaceOnBoard,
    Session, SetStatus, State, Ticket, decide,
)

OWNER = "MinimalEffort07"
BOT = "MinimalEffort07-Autodev"


def item(number, status="Todo", author=OWNER, has_parent=False):
    return Item(number=number, author=author, status=status,
                has_parent=has_parent)


def state(items=(), sessions=(), outcomes=None, max_sessions=2):
    return State(owner=OWNER, bot=BOT, items=list(items),
                 sessions=list(sessions), outcomes=outcomes or {},
                 max_sessions=max_sessions)


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

    def test_tickets_are_left_for_a_later_role(self):
        # Implementing tickets is #56; until then they wait in Todo.
        self.assertEqual(decide(state([item(70, has_parent=True)])), [])


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
            owner=OWNER, bot=BOT, items=[item(61, status="Grilling")],
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
        # It crashed, or systemd stopped it at its 60-minute limit.
        self.assertEqual(self.run_finished(None, GrillOutcome(False, [])), [
            SetStatus(61, "Needs human"),
            needs_human("the grill session ended without reporting a result "
                        "(it crashed or hit its time limit)"),
            Finish("grill", 61),
        ])


if __name__ == "__main__":
    unittest.main()
