"""The dispatcher's decisions: given what it knows, exactly what it does.

`decide()` is a pure function, so each test builds a State by hand and
checks the exact list of actions. The D-numbers are the scenarios agreed
on issue #48.
"""

import unittest

from autodev.board import Item
from autodev.decide import (
    CloseIssue, Comment, Finish, GrillOutcome, Launch, LinkSubIssue,
    MakeWorktree, MergeOutcome, PlaceOnBoard, PrComment, PullRequest,
    RemoveWorktree, Session, SetStatus, State, Ticket, TicketOutcome, decide,
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


def state(items=(), sessions=(), outcomes=None, max_sessions=2, closed=(),
          prs=None, worktrees=()):
    return State(owner=OWNER, bot=BOT, items=list(items),
                 sessions=list(sessions), outcomes=outcomes or {},
                 max_sessions=max_sessions, closed=frozenset(closed),
                 prs=prs or {}, worktrees=frozenset(worktrees))


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
        actions = decide(state([
            item(61, is_open=False), ticket(68, is_open=False),
            ticket(70, "Blocked", is_open=False)]))
        self.assertEqual([a for a in actions if isinstance(a, Launch)], [])

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
                   ended_by=ended_by, cwd="/clone-worktrees/issue-68")


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
            "`cd /clone-worktrees/issue-68 && claude --resume sid-68`"
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


def said(id, author, body, minute):
    """A comment, review or inline comment on a PR, `minute` minutes in."""
    return PrComment(id=id, author=author, body=body,
                     at="2026-10-19T06:%02d:00Z" % minute)


def handled(id, through, minute):
    return said(id, BOT, "Done.\n\n<!-- autodev handled-through: %d -->"
                % through, minute)


def in_review(comments, number=68, pr=73):
    """Ticket `number` waiting in review, its PR saying `comments`."""
    return state([ticket(number, "In review")], prs={number: PullRequest(
        number=pr, head="issue-%d-note" % number,
        comments=tuple(comments))})


class Merging(unittest.TestCase):
    def test_D9_the_owners_newest_unhandled_comment_merge_launches_a_merge(self):
        # The session gets the PR and the comment it answers, so its reply
        # can say which comment it handled. It reuses the ticket's worktree.
        self.assertEqual(decide(in_review([said(501, OWNER, " /merge\n", 1)])), [
            MakeWorktree(68, "issue-68-note"),
            Launch("merge", 68, pr=73, handled_through=501),
            SetStatus(68, "In progress"),
        ])

    def test_D10_merge_from_anyone_but_the_owner_is_ignored(self):
        for author in ("someone-else", BOT):
            with self.subTest(author=author):
                self.assertEqual(decide(in_review([said(501, author, "/merge", 1)])), [])

    def test_D12_comments_older_than_the_last_handled_marker_are_done_with(self):
        # The last merge session hit a conflict, fixed it and replied; the
        # owner hasn't posted a fresh /merge since.
        self.assertEqual(decide(in_review([
            said(501, OWNER, "/merge", 1), handled(502, 501, 30)])), [])

    def test_a_comment_posted_while_the_reply_was_being_written_is_still_new(self):
        # The marker names the comment the session handled, not the time it
        # replied: a /merge posted while it worked starts the next session.
        self.assertEqual(decide(in_review([
            said(501, OWNER, "/merge", 1), said(503, OWNER, "/merge", 20),
            handled(502, 501, 30)]))[1],
            Launch("merge", 68, pr=73, handled_through=503))

    def test_feedback_after_merge_cancels_it(self):
        # D14: the owner changed their mind. The fix-up itself is #59.
        self.assertEqual(decide(in_review([
            said(501, OWNER, "/merge", 1),
            said(9001, OWNER, "Wait: reword the note first.", 2)])), [])

    def test_D6_a_merge_goes_before_any_ticket(self):
        # One slot free. #81's parent (#60) is older than #68's, but a merge
        # finishes work that is already done, so it goes first.
        waiting = in_review([said(501, OWNER, "/merge", 1)])
        waiting = State(**{**waiting.__dict__,
                           "items": waiting.items + [ticket(81, parent=60)],
                           "sessions": [Session("grill", 99, "sid-99", running=True)]})
        self.assertEqual([a for a in decide(waiting) if isinstance(a, Launch)],
                         [Launch("merge", 68, pr=73, handled_through=501)])

    def test_D13_nothing_starts_while_a_session_for_the_ticket_runs(self):
        busy = in_review([said(501, OWNER, "/merge", 1)])
        busy = State(**{**busy.__dict__, "sessions": [
            Session("ticket", 68, "sid-68", running=True)]})
        self.assertEqual(decide(busy), [])


class ClosedTickets(unittest.TestCase):
    def test_D22_a_ticket_closed_without_a_pr_is_done_and_unblocks_others(self):
        # The owner closed #70 by hand (or merged its PR themselves). #71
        # waited on it alone.
        self.assertEqual(decide(state(
            [ticket(70, "In review", is_open=False),
             ticket(71, "Blocked", blocked_by=[70])],
            closed=[70], worktrees=[70])), [
            RemoveWorktree(70), SetStatus(70, "Done"),
            SetStatus(71, "Todo"),
        ])

    def test_the_boards_own_automation_may_have_marked_it_done_already(self):
        # The board's "Item closed" workflow sets Done; the worktree is
        # still autodev's to remove, and only once.
        self.assertEqual(decide(state([ticket(70, "Done", is_open=False)],
                                      worktrees=[70])), [RemoveWorktree(70)])
        self.assertEqual(decide(state([ticket(70, "Done", is_open=False)])), [])

    def test_a_worktree_in_use_by_a_session_is_left_alone(self):
        running = [Session("merge", 70, "sid-70", running=True)]
        self.assertEqual(decide(state([ticket(70, "In progress", is_open=False)],
                                      sessions=running, worktrees=[70])), [])


class Parents(unittest.TestCase):
    def test_D23_a_parent_whose_tickets_are_all_closed_is_done(self):
        self.assertEqual(decide(state([
            item(67, status="In progress"),
            ticket(68, "Done", parent=67, is_open=False),
            ticket(72, "Done", parent=67, is_open=False)])), [
            # Closed first: if that fails, the next tick tries again without
            # having @mentioned the owner twice.
            CloseIssue(67),
            Comment(67, "@MinimalEffort07 every ticket of this issue is closed "
                        "(#68, #72), so autodev is done with it.\n\n<!-- autodev -->"),
            SetStatus(67, "Done"),
        ])

    def test_only_the_owners_parents_are_closed(self):
        self.assertEqual(decide(state([
            item(67, status="In progress", author="someone-else"),
            ticket(68, "Done", parent=67, is_open=False)])), [])

    def test_a_parent_with_an_open_ticket_or_none_on_the_board_stays(self):
        for tickets in ([ticket(68, "Done", parent=67, is_open=False),
                         ticket(72, "In review", parent=67)],
                        [ticket(68, "Done", parent=48, is_open=False)]):
            with self.subTest(tickets=tickets):
                self.assertEqual(decide(state(
                    [item(67, status="In progress")] + tickets)), [])


class FinishedMerges(unittest.TestCase):
    def run_finished(self, result, merged=False, replied=False, ticket_open=True,
                     ended_by="success"):
        return decide(State(
            owner=OWNER, bot=BOT,
            items=[ticket(68, "In progress", is_open=ticket_open)],
            sessions=[Session("merge", 68, "sid-m68", running=False,
                              result=result, ended_by=ended_by,
                              cwd="/clone-worktrees/issue-68", pr=73,
                              handled_through=501)],
            outcomes={68: MergeOutcome(merged=merged, replied=replied)},
            worktrees=frozenset([68]),
            resume_command="cd {cwd} && claude --resume {session_id}"))

    def test_D21_a_merged_pr_closes_the_ticket_and_removes_its_worktree(self):
        # "Fixes #68" normally closes the ticket as the PR merges; if GitHub
        # hasn't (yet), the dispatcher does.
        done = {"outcome": "done", "links": [73]}
        self.assertEqual(self.run_finished(done, merged=True), [
            CloseIssue(68), RemoveWorktree(68), SetStatus(68, "Done"),
            Finish("merge", 68)])
        self.assertEqual(self.run_finished(done, merged=True, ticket_open=False), [
            RemoveWorktree(68), SetStatus(68, "Done"), Finish("merge", 68)])

    def test_a_merged_pr_counts_even_if_the_session_then_failed(self):
        # It merged, then timed out or crashed before reporting. GitHub
        # showing the PR merged is what matters (#48, Sessions).
        for result, ended_by in ((None, "timeout"), ({"outcome": "failed"}, "success")):
            with self.subTest(result=result):
                self.assertEqual(self.run_finished(result, merged=True, ended_by=ended_by), [
                    CloseIssue(68), RemoveWorktree(68), SetStatus(68, "Done"),
                    Finish("merge", 68)])

    def test_a_conflict_resolved_on_the_branch_waits_for_a_fresh_merge(self):
        # The session resolved it, re-tested, pushed and explained how in a
        # reply; the owner's approval has to cover that resolution.
        self.assertEqual(self.run_finished({"outcome": "done"}, replied=True), [
            SetStatus(68, "In review"), Finish("merge", 68)])

    def test_a_done_claim_github_does_not_back_up_needs_a_human(self):
        self.assertEqual(self.run_finished({"outcome": "done"})[:2], [
            SetStatus(68, "Needs human"), Comment(68, (
                "@MinimalEffort07 autodev parked this issue: the merge session "
                "said it was done, but GitHub shows PR #73 neither merged nor "
                "answered about a conflict\n\n"
                "To pick up where the merge session stopped: "
                "`cd /clone-worktrees/issue-68 && claude --resume sid-m68`"
                "\n\n<!-- autodev -->"))])

    def test_a_merge_stopped_at_its_time_limit_needs_a_human(self):
        self.assertIn("the merge session hit its 45-minute time limit",
                      self.run_finished(None, ended_by="timeout")[1].body)


if __name__ == "__main__":
    unittest.main()
