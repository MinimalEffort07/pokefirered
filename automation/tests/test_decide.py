"""The dispatcher's decisions: given what it knows, exactly what it does.

`decide()` is a pure function, so each test builds a State by hand and
checks the exact list of actions. The D-numbers are the scenarios agreed
on issue #48.
"""

import unittest

from autodev.board import Item
from autodev.decide import (
    CloseIssue, Comment, Finish, FixupOutcome, GrillOutcome, Launch,
    LinkSubIssue, MakeWorktree, MergeOutcome, PauseLaunches, PlaceOnBoard, PrComment,
    PullRequest, RemoveWorktree, Session, SetStatus, State, Ticket,
    TicketOutcome, decide, plan,
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
        # Tickets before grills, and among tickets the parent grilled first:
        # #79 (parent #76, first ticket #79), then #81 (parent #74, first
        # ticket #81), though #74 is the lower number. Parent #61 waits.
        self.assertEqual(decide(state([item(61), ticket(79, parent=76),
                                       ticket(81, parent=74)])), [
            MakeWorktree(79, "issue-79-do-the-thing"),
            Launch("ticket", 79), SetStatus(79, "In progress"),
            MakeWorktree(81, "issue-81-do-the-thing"),
            Launch("ticket", 81), SetStatus(81, "In progress"),
        ])


RESUME = "claude --resume {session_id}"


def finished_grill(result):
    return Session("grill", 61, "sid-61", running=False, result=result)


def needs_human(reason):
    return Comment(61, (
        "@MinimalEffort07 autodev parked this issue: %s\n\n"
        "To pick up where the grill session stopped: `claude --resume sid-61`"
        "\n\nOnce it's sorted, move the card back to `Todo`."
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

    def test_a_grill_stopped_at_its_time_limit_needs_a_human(self):
        # systemd says how the unit ended: "timeout" means it was stopped at
        # its 60-minute limit. (A crash is re-queued once: see Crashes.)
        for ended_by, reason in [
            ("timeout", "the grill session hit its 60-minute time limit"),
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
            "\n\nOnce it's sorted, it comes back by itself when its PR is "
            "opened or you comment on its PR, or move the card back to `Todo`."
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

    def test_D6_a_merge_goes_before_any_ticket(self):
        # One slot free. #66's parent (#60) was grilled before #68's (#67),
        # but a merge finishes work that is already done, so it goes first.
        waiting = in_review([said(501, OWNER, "/merge", 1)])
        waiting = State(**{**waiting.__dict__,
                           "items": waiting.items + [ticket(66, parent=60)],
                           "sessions": [Session("grill", 99, "sid-99", running=True)]})
        self.assertEqual([a for a in decide(waiting) if isinstance(a, Launch)],
                         [Launch("merge", 68, pr=73, handled_through=501)])

    def test_D13_nothing_starts_while_a_session_for_the_ticket_runs(self):
        busy = in_review([said(501, OWNER, "/merge", 1)])
        busy = State(**{**busy.__dict__, "sessions": [
            Session("ticket", 68, "sid-68", running=True)]})
        self.assertEqual(decide(busy), [])


class FixUps(unittest.TestCase):
    def test_D11_the_owners_feedback_launches_a_fix_up(self):
        # Like a merge, it reuses the ticket's worktree and gets the PR and
        # the newest comment it answers; several queued comments are all
        # handled by that one session.
        self.assertEqual(decide(in_review([
            said(9001, OWNER, "Reword the note.", 1),
            said(9002, OWNER, "And mention #58.", 2)])), [
            MakeWorktree(68, "issue-68-note"),
            Launch("fixup", 68, pr=73, handled_through=9002),
            SetStatus(68, "In progress"),
        ])

    def test_D14_feedback_after_merge_cancels_it(self):
        # The owner changed their mind: no merge until a fresh /merge.
        self.assertEqual(decide(in_review([
            said(501, OWNER, "/merge", 1),
            said(9001, OWNER, "Wait: reword the note first.", 2)]))[1],
            Launch("fixup", 68, pr=73, handled_through=9001))

    def test_feedback_from_anyone_but_the_owner_is_ignored(self):
        for author in ("someone-else", BOT):
            with self.subTest(author=author):
                self.assertEqual(decide(in_review([
                    said(9001, author, "Delete the tests.", 1)])), [])

    def test_D13_a_comment_posted_while_a_fix_up_runs_waits_for_it(self):
        busy = in_review([said(9001, OWNER, "Reword the note.", 1),
                          said(9003, OWNER, "Also mention #58.", 20)])
        busy = State(**{**busy.__dict__, "sessions": [
            Session("fixup", 68, "sid-f68", running=True, pr=73,
                    handled_through=9001)]})
        self.assertEqual(decide(busy), [])

    def test_D13_then_a_follow_up_fix_up_handles_it(self):
        # The first fix-up's reply names the comment it was started for, so
        # the one posted while it worked is still unhandled.
        self.assertEqual(decide(in_review([
            said(9001, OWNER, "Reword the note.", 1),
            said(9003, OWNER, "Also mention #58.", 20),
            handled(9004, 9001, 30)]))[1],
            Launch("fixup", 68, pr=73, handled_through=9003))

    def test_a_fix_up_goes_after_a_merge_and_before_any_ticket(self):
        # One slot free. #66's parent (#60) was grilled before #68's and
        # #69's (#67).
        waiting = in_review([said(9001, OWNER, "Reword the note.", 1)], number=69, pr=74)
        merging = in_review([said(501, OWNER, "/merge", 1)])
        waiting = State(**{**waiting.__dict__,
                           "items": waiting.items + [ticket(66, parent=60)],
                           "sessions": [Session("grill", 99, "sid-99", running=True)]})
        self.assertEqual([a for a in decide(waiting) if isinstance(a, Launch)],
                         [Launch("fixup", 69, pr=74, handled_through=9001)])
        both = State(**{**waiting.__dict__,
                        "items": waiting.items + merging.items,
                        "prs": {**waiting.prs, **merging.prs}})
        self.assertEqual([a for a in decide(both) if isinstance(a, Launch)],
                         [Launch("merge", 68, pr=73, handled_through=501)])


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


class Pausing(unittest.TestCase):
    def test_D7_a_paused_parent_stops_its_tickets_from_starting(self):
        # Neither #68's implementation nor #70's merge starts; #81, whose
        # parent isn't paused, does.
        paused = State(**{**in_review([said(501, OWNER, "/merge", 1)], number=70).__dict__,
                          "items": [item(67, status="Paused"),
                                    ticket(68, parent=67),
                                    ticket(70, "In review", parent=67),
                                    ticket(81, parent=60)]})
        self.assertEqual([a for a in decide(paused) if isinstance(a, Launch)],
                         [Launch("ticket", 81)])

    def test_a_paused_ticket_does_not_start(self):
        self.assertEqual(decide(state([ticket(68, "Paused")])), [])

    def test_D8_a_resumed_parent_goes_back_to_work_not_to_grilling(self):
        # Moved from Paused back to Todo. It has tickets, so it was grilled
        # already: Todo now means "carry on". Its ticket starts at once.
        self.assertEqual(decide(state([item(67), ticket(68, parent=67)])), [
            SetStatus(67, "In progress"),
            MakeWorktree(68, "issue-68-do-the-thing"),
            Launch("ticket", 68), SetStatus(68, "In progress"),
        ])

    def test_a_requeued_grill_is_grilled_again_though_some_tickets_exist(self):
        # The grill crashed or hit the usage limit after creating #68; the
        # board adds sub-issues as they're created. Its tickets wait for it.
        self.assertEqual(decide(State(
            owner=OWNER, bot=BOT, items=[item(67), ticket(68, parent=67)],
            requeued=frozenset([("grill", 67)]))), [
            Launch("grill", 67), SetStatus(67, "Grilling"),
        ])

    def test_tickets_wait_while_their_parent_is_being_grilled(self):
        # The board adds each ticket as the grill creates it, before the
        # grill has finished checking the breakdown.
        self.assertEqual(decide(state([item(67, "Grilling"),
                                       ticket(68, parent=67)])), [])

    def test_D8_a_resumed_ticket_with_a_pr_goes_back_to_review(self):
        # Its PR is already open (paused while in review, or re-queued after
        # opening it), so implementing it again would be wrong.
        resumed = in_review([])
        resumed = State(**{**resumed.__dict__, "items": [ticket(68, "Todo")]})
        self.assertEqual(decide(resumed), [SetStatus(68, "In review")])

    def test_a_session_ending_while_its_card_is_paused_leaves_the_card_paused(self):
        # Its comment still posts; the owner sees the outcome when resuming.
        done = Session("ticket", 68, "sid-68", running=False,
                       result={"outcome": "done"}, cwd="/w")
        failed = Session("ticket", 68, "sid-68", running=False,
                         result={"outcome": "failed", "reason": "no"}, cwd="/w")
        for session, outcome, expected in [
            (done, TicketOutcome(pr=73), [Finish("ticket", 68)]),
            (failed, TicketOutcome(pr=None), ["comment", Finish("ticket", 68)]),
        ]:
            with self.subTest(result=session.result):
                actions = decide(state([ticket(68, "Paused")], sessions=[session],
                                       outcomes={68: outcome}))
                self.assertEqual(["comment" if isinstance(a, Comment) else a
                                  for a in actions], expected)

    def test_running_sessions_are_left_to_finish(self):
        running = [Session("ticket", 68, "sid-68", running=True)]
        self.assertEqual(decide(state([item(67, status="Paused"),
                                       ticket(68, "In progress", parent=67)],
                                      sessions=running)), [])


NOW = 1_800_000_000.0   # the tick's clock, as seconds since 1970


def at(t):
    """How the tests show a time: minutes from NOW."""
    return "NOW+%dmin" % ((t - NOW) // 60)


def limited(role, issue, resets_in_min, note="You've hit your limit · resets 3pm"):
    return Session(role, issue, "sid-%d" % issue, running=False, result=None,
                   ended_by="exit-code", cwd="/w", pr=73 if role == "merge" else None,
                   handled_through=501 if role == "merge" else None,
                   limit_reset=NOW + resets_in_min * 60, limit_note=note)


class UsageLimits(unittest.TestCase):
    def run_tick(self, items, sessions=(), outcomes=None, paused_until=0.0, now=NOW):
        return decide(State(owner=OWNER, bot=BOT, items=items, sessions=list(sessions),
                            outcomes=outcomes or {}, now=now,
                            paused_until=paused_until, show_time=at))

    def test_D18_a_usage_limit_pauses_every_launch_and_requeues_the_work(self):
        # #81 is ready too, but nothing starts until the limit resets plus
        # five minutes. #68 goes back to Todo and continues from its branch.
        self.assertEqual(self.run_tick(
            [ticket(68, "In progress"), ticket(81)],
            sessions=[limited("ticket", 68, 40)],
            outcomes={68: TicketOutcome(pr=None)}), [
            # The status changes before the comment, so a retry of this
            # group never @mentions the owner twice.
            PauseLaunches(NOW + 45 * 60),
            SetStatus(68, "Todo"),
            Comment(68, "@MinimalEffort07 autodev hit the Claude usage limit "
                        "(the ticket session said: \u201cYou've hit your limit "
                        "\u00b7 resets 3pm\u201d), so it is paused until "
                        "NOW+45min. #68 goes back to Todo and starts again then."
                        "\n\n<!-- autodev -->"),
            Finish("ticket", 68, requeued=True),
        ])

    def test_a_paused_card_hit_by_the_limit_is_told_it_stays_paused(self):
        actions = self.run_tick([ticket(68, "Paused")],
                                sessions=[limited("ticket", 68, 40)],
                                outcomes={68: TicketOutcome(pr=None)})
        self.assertEqual([type(a).__name__ for a in actions],
                         ["PauseLaunches", "Comment", "Finish"])
        self.assertIn("#68 stays Paused; it starts again once you move it back "
                      "to Todo and the pause is over.", actions[1].body)

    def test_a_merge_that_hit_the_limit_goes_back_to_review(self):
        # Its /merge is still the owner's newest unhandled comment, so the
        # merge starts again once the pause is over.
        actions = self.run_tick([ticket(68, "In progress")],
                                sessions=[limited("merge", 68, 10)],
                                outcomes={68: MergeOutcome(merged=False, replied=False)})
        self.assertIn(SetStatus(68, "In review"), actions)

    def test_while_paused_nothing_starts_but_the_board_is_kept_up(self):
        self.assertEqual(self.run_tick(
            [ticket(81), ticket(82, "Blocked", blocked_by=[55])],
            paused_until=NOW + 60), [])
        self.assertEqual(decide(State(
            owner=OWNER, bot=BOT, items=[ticket(82, "Blocked", blocked_by=[55])],
            closed=frozenset([55]), now=NOW, paused_until=NOW + 60)),
            [SetStatus(82, "Todo")])

    def test_once_the_pause_is_over_the_work_starts_again(self):
        self.assertEqual(self.run_tick([ticket(68)], paused_until=NOW - 1)[1],
                         Launch("ticket", 68))


def crashed(role, issue, ended_by="signal"):
    return Session(role, issue, "sid-%d" % issue, running=False, result=None,
                   ended_by=ended_by, cwd="/w", pr=73 if role == "merge" else None,
                   handled_through=501 if role == "merge" else None)


class Crashes(unittest.TestCase):
    def run_tick(self, session, crashes=None, outcome=None):
        return decide(State(
            owner=OWNER, bot=BOT, items=[ticket(68, "In progress")],
            sessions=[session], crashes=crashes or {},
            outcomes={68: outcome or TicketOutcome(pr=None)},
            resume_command="cd {cwd} && claude --resume {session_id}"))

    def test_D19_an_unexplained_crash_is_requeued_once(self):
        # No report, no usage-limit message, not the time limit. A unit that
        # simply vanished (a reboot; systemd never said how it ended) too.
        for ended_by, how in (("signal", "systemd says \u201csignal\u201d"),
                              ("exit-code", "systemd says \u201cexit-code\u201d"),
                              (None, "its unit vanished, e.g. in a reboot")):
            with self.subTest(ended_by=ended_by):
                self.assertEqual(self.run_tick(crashed("ticket", 68, ended_by)), [
                    SetStatus(68, "Todo"),
                    Comment(68, "The ticket session ended without reporting a "
                                "result (%s), so autodev will run it once more."
                                "\n\n<!-- autodev -->" % how),
                    Finish("ticket", 68, crashed=True, requeued=True),
                ])

    def test_a_crash_after_the_work_shows_on_github_counts_as_done(self):
        # The ticket session opened its PR, or the merge session posted its
        # conflict reply, then crashed before reporting: no retry used up.
        self.assertEqual(self.run_tick(crashed("ticket", 68),
                                       outcome=TicketOutcome(pr=73)),
                         [SetStatus(68, "In review"), Finish("ticket", 68)])
        self.assertEqual(self.run_tick(crashed("merge", 68),
                                       outcome=MergeOutcome(merged=False, replied=True)),
                         [SetStatus(68, "In review"), Finish("merge", 68)])

    def test_a_crashed_merge_goes_back_to_review(self):
        actions = self.run_tick(crashed("merge", 68),
                                outcome=MergeOutcome(merged=False, replied=False))
        self.assertIn(SetStatus(68, "In review"), actions)

    def test_D20_a_second_crash_needs_a_human(self):
        # Counted per role and issue, however long ago the first one was.
        self.assertEqual(self.run_tick(crashed("ticket", 68),
                                       crashes={("ticket", 68): 1}), [
            SetStatus(68, "Needs human"),
            Comment(68, "@MinimalEffort07 autodev parked this issue: the ticket "
                        "session crashed a second time (systemd says "
                        "\u201csignal\u201d)\n\nTo pick up where the ticket session "
                        "stopped: `cd /w && claude --resume sid-68`"
                        "\n\nOnce it's sorted, it comes back by itself when its PR is "
            "opened or you comment on its PR, or move the card back to `Todo`."
                        "\n\n<!-- autodev -->"),
            Finish("ticket", 68, crashed=True),
        ])

    def test_a_merge_crash_does_not_count_against_the_ticket_session(self):
        actions = self.run_tick(crashed("merge", 68), crashes={("ticket", 68): 1},
                                outcome=MergeOutcome(merged=False, replied=False))
        self.assertIn(SetStatus(68, "In review"), actions)


class FinishedMerges(unittest.TestCase):
    def run_finished(self, result, merged=False, replied=False, ticket_open=True,
                     ended_by="success", superseded=False):
        return decide(State(
            owner=OWNER, bot=BOT,
            items=[ticket(68, "In progress", is_open=ticket_open)],
            sessions=[Session("merge", 68, "sid-m68", running=False,
                              result=result, ended_by=ended_by,
                              cwd="/clone-worktrees/issue-68", pr=73,
                              handled_through=501)],
            outcomes={68: MergeOutcome(merged=merged, replied=replied,
                                       superseded=superseded)},
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
                "\n\nOnce it's sorted, it comes back by itself when its PR is "
            "opened or you comment on its PR, or move the card back to `Todo`."
                "\n\n<!-- autodev -->"))])

    def test_a_merge_stopped_at_its_time_limit_needs_a_human(self):
        self.assertIn("the merge session hit its 45-minute time limit",
                      self.run_finished(None, ended_by="timeout")[1].body)

    def test_D14_feedback_during_the_merge_sends_it_back_to_review(self):
        # merge-pr refused because the owner commented after their /merge,
        # so the session failed; a fix-up picks the comments up from review.
        for result, ended_by in (({"outcome": "failed"}, "success"),
                                 ({"outcome": "done"}, "success"),
                                 (None, "signal")):
            with self.subTest(result=result):
                self.assertEqual(
                    self.run_finished(result, ended_by=ended_by, superseded=True),
                    [SetStatus(68, "In review"), Finish("merge", 68)])

    def test_a_merged_pr_stays_merged_whatever_was_said_after(self):
        self.assertEqual(self.run_finished({"outcome": "done"}, merged=True,
                                           superseded=True)[-2:],
                         [SetStatus(68, "Done"), Finish("merge", 68)])


class FinishedFixUps(unittest.TestCase):
    def run_finished(self, result, replied=False, ended_by="success",
                     limit_reset=None, crashes=None):
        return decide(State(
            owner=OWNER, bot=BOT, items=[ticket(68, "In progress")],
            sessions=[Session("fixup", 68, "sid-f68", running=False,
                              result=result, ended_by=ended_by,
                              cwd="/clone-worktrees/issue-68", pr=73,
                              handled_through=9001, limit_reset=limit_reset,
                              limit_note="You've hit your limit")],
            outcomes={68: FixupOutcome(replied=replied)},
            resume_command="cd {cwd} && claude --resume {session_id}",
            crashes=crashes or {}, show_time=lambda t: "T+%d" % t))

    def test_D15_a_fix_up_whose_reply_is_there_goes_back_to_review(self):
        # The reply naming the comment it was started for is the work the
        # owner reads before their next /merge. A question needs no commit.
        self.assertEqual(self.run_finished({"outcome": "done"}, replied=True), [
            SetStatus(68, "In review"), Finish("fixup", 68)])

    def test_a_done_claim_without_the_reply_needs_a_human(self):
        self.assertEqual(self.run_finished({"outcome": "done"})[:2], [
            SetStatus(68, "Needs human"), Comment(68, (
                "@MinimalEffort07 autodev parked this issue: the fix-up session "
                "said it was done, but GitHub shows no reply on PR #73 marked as "
                "handling comment 9001\n\n"
                "To pick up where the fix-up session stopped: "
                "`cd /clone-worktrees/issue-68 && claude --resume sid-f68`"
                "\n\nOnce it's sorted, it comes back by itself when its PR is "
            "opened or you comment on its PR, or move the card back to `Todo`."
                "\n\n<!-- autodev -->"))])

    def test_a_fix_up_that_replied_and_then_crashed_is_done(self):
        self.assertEqual(self.run_finished(None, replied=True, ended_by="signal"), [
            SetStatus(68, "In review"), Finish("fixup", 68)])

    def test_a_crash_before_the_reply_runs_the_fix_up_once_more(self):
        # Back to review, where the feedback is still the newest unhandled.
        self.assertEqual(self.run_finished(None, ended_by="signal"), [
            SetStatus(68, "In review"),
            Comment(68, "The fix-up session ended without reporting a result "
                        "(systemd says “signal”), so autodev will run it "
                        "once more.\n\n<!-- autodev -->"),
            Finish("fixup", 68, crashed=True, requeued=True)])

    def test_a_fix_up_that_hit_the_usage_limit_goes_back_to_review(self):
        actions = self.run_finished(None, ended_by="exit-code", limit_reset=1000)
        self.assertEqual(actions[:2], [PauseLaunches(1300), SetStatus(68, "In review")])
        self.assertEqual(actions[-1], Finish("fixup", 68, requeued=True))

    def test_a_fix_up_stopped_at_its_time_limit_needs_a_human(self):
        self.assertIn("the fix-up session hit its 90-minute time limit",
                      self.run_finished(None, ended_by="timeout")[1].body)


class ResumeCommands(unittest.TestCase):
    """The resume command in a parked comment reopens the session with what
    issue.py needs to know it is that session: its role, its issue and, for
    a merge or fix-up, its PR and the comment it answers. Without them the
    resumed session can only read, so it could never finish the work."""

    def parked_body(self, session):
        return [a for a in decide(State(
            owner=OWNER, bot=BOT, items=[ticket(68, "In progress")],
            sessions=[session], outcomes={68: TicketOutcome(pr=None)
                                          if session.role == "ticket"
                                          else FixupOutcome(replied=False)},
            resume_command="cd {cwd} && {env} claude --resume {session_id}"))
            if isinstance(a, Comment)][0].body

    def test_a_ticket_session_resumes_as_that_ticket_session(self):
        body = self.parked_body(Session(
            "ticket", 68, "sid-68", running=False,
            result={"outcome": "failed", "reason": "stuck"}, cwd="/w"))
        self.assertIn("`cd /w && AUTODEV_ROLE=ticket AUTODEV_ISSUE=68 "
                      "claude --resume sid-68`", body)

    def test_a_fix_up_session_also_gets_its_pr_and_comment(self):
        body = self.parked_body(Session(
            "fixup", 68, "sid-f68", running=False,
            result={"outcome": "failed", "reason": "stuck"}, cwd="/w", pr=73,
            handled_through=9001))
        self.assertIn("`cd /w && AUTODEV_ROLE=fixup AUTODEV_ISSUE=68 AUTODEV_PR=73 "
                      "AUTODEV_HANDLED_THROUGH=9001 claude --resume sid-f68`", body)

    def test_the_configured_command_runs_as_the_bot_in_a_subshell(self):
        # config.json's command: the bot's token (GH_TOKEN, from the env
        # file) so gh and git act as the bot, as the session did; loaded in
        # a subshell, so it doesn't stay in the owner's own shell.
        # The raw file: its paths keep their ~ for the shell to expand.
        import json
        import os
        path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "config.json")
        with open(path) as f:
            raw = json.load(f)
        command = raw["resume_command"].format(
            cwd="/w", session_id="sid-68",
            env="AUTODEV_ROLE=ticket AUTODEV_ISSUE=68")
        self.assertRegex(command, r"^cd /w && \(.*\)$")
        self.assertIn("set -a && . %s && set +a" % raw["env_file"], command)
        self.assertIn("AUTODEV_ROLE=ticket AUTODEV_ISSUE=68 ", command)
        self.assertIn("claude --resume sid-68", command)


def parked(pr_comments=(), opened=1, parked_at=10, author=BOT, number=68):
    """Ticket `number` in Needs human, autodev having parked it at
    `parked_at` minutes in (None: nobody but the owner did), with its PR
    opened at `opened` minutes in (None: no PR) saying `pr_comments`."""
    prs = {} if opened is None else {number: PullRequest(
        number=73, head="issue-%d-note" % number, comments=tuple(pr_comments),
        opened="2026-10-19T06:%02d:00Z" % opened)}
    return State(owner=OWNER, bot=BOT,
                 items=[ticket(number, "Needs human", author=author)], prs=prs,
                 parked_at={} if parked_at is None
                 else {number: "2026-10-19T06:%02d:00Z" % parked_at})


class ParkedTickets(unittest.TestCase):
    """A ticket autodev parked in Needs human comes back once the owner has
    dealt with it: they say so on its PR, or its PR appears (the owner, or
    the resumed session, finished the work). Anything from before it was
    parked is what it was parked over, so it never starts the work again."""

    def test_the_owners_merge_on_its_pr_after_parking_launches_a_merge(self):
        self.assertEqual(decide(parked([said(501, OWNER, "/merge", 20)])), [
            MakeWorktree(68, "issue-68-note"),
            Launch("merge", 68, pr=73, handled_through=501),
            SetStatus(68, "In progress"),
        ])

    def test_the_owners_feedback_after_parking_launches_a_fix_up(self):
        self.assertEqual(decide(parked([said(9001, OWNER, "Reword it.", 20)])), [
            MakeWorktree(68, "issue-68-note"),
            Launch("fixup", 68, pr=73, handled_through=9001),
            SetStatus(68, "In progress"),
        ])

    def test_feedback_from_before_it_was_parked_does_not_start_it_again(self):
        # A fix-up for it failed: that is why it was parked.
        self.assertEqual(decide(parked([said(9001, OWNER, "Reword it.", 5)])), [])

    def test_a_pr_opened_since_it_was_parked_sends_it_to_review(self):
        # The resumed ticket session, or the owner, opened it.
        self.assertEqual(decide(parked(opened=20)), [SetStatus(68, "In review")])

    def test_its_pr_appearing_and_a_merge_after_parking_launch_the_merge(self):
        # Issue #88: parked with no PR; then the PR, then the owner's /merge.
        self.assertEqual(decide(parked([said(501, OWNER, "/merge", 30)], opened=20))[1],
                         Launch("merge", 68, pr=73, handled_through=501))

    def test_a_pr_that_was_open_when_it_was_parked_keeps_it_parked(self):
        self.assertEqual(decide(parked(opened=1)), [])

    def test_with_no_pr_it_stays_parked(self):
        self.assertEqual(decide(parked(opened=None)), [])

    def test_others_comments_on_its_pr_are_ignored(self):
        for author in ("someone-else", BOT):
            with self.subTest(author=author):
                self.assertEqual(decide(parked([said(501, author, "/merge", 20)])), [])

    def test_a_card_the_owner_moved_to_needs_human_is_left_alone(self):
        # Autodev always comments when it parks something, so a card with no
        # such comment is the owner's own; Paused is the stop switch, but
        # this is theirs too.
        self.assertEqual(decide(parked([said(501, OWNER, "/merge", 20)],
                                       opened=20, parked_at=None)), [])

    def test_someone_elses_ticket_stays_parked(self):
        # Parked for its author (D2): nothing on a PR changes that.
        self.assertEqual(decide(parked([said(501, OWNER, "/merge", 20)],
                                       author="someone-else")), [])


if __name__ == "__main__":
    unittest.main()
