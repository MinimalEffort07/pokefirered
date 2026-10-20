"""What a tick reads: the board, which it logs, and what decide() needs
beyond it.

Each test replays a recorded board (tests/fixtures/*.json) through FakeGh
and checks the exact lines the tick logs.
"""

import io
import os
import tempfile
import unittest

from autodev.config import Config
from autodev.board import read_board
from autodev.dispatcher import log_board
from autodev.gh import GhError
from tests.fakes import FakeGh, FakeRegistry

CONFIG = Config(
    owner="MinimalEffort07",
    repo="MinimalEffort07/pokefirered",
    board_title="pokefirered autodev",
)
# For read_state: a clone with no worktrees beside it.
NO_CLONE = Config(owner="MinimalEffort07", repo="MinimalEffort07/pokefirered",
                  board_title="pokefirered autodev", clone="/nonexistent/clone")


def run_tick(fixture):
    gh = FakeGh.from_fixture(fixture)
    log = io.StringIO()
    code, _ = log_board(gh, CONFIG, log)
    return code, log.getvalue().splitlines(), gh


class TickLogsTheBoard(unittest.TestCase):
    def test_parent_issue_with_no_status_is_logged_as_todo(self):
        code, lines, gh = run_tick("one_parent_no_status.json")
        self.assertEqual(code, 0)
        self.assertEqual(lines, [
            'board #7 "pokefirered autodev": 1 item',
            "#61 MinimalEffort07 Todo has_parent=no",
        ])
        self.assertEqual(gh.mutations, [])

    def test_every_page_of_the_board_is_read(self):
        # Page one holds a ticket (its body has a "## Parent" heading);
        # page two a parent whose body only mentions the word "Parent".
        code, lines, gh = run_tick("two_pages.json")
        self.assertEqual(code, 0)
        self.assertEqual(lines, [
            'board #7 "pokefirered autodev": 2 items',
            "#63 MinimalEffort07 Blocked has_parent=yes",
            "#61 MinimalEffort07 Grilling has_parent=no",
        ])
        self.assertEqual(gh.mutations, [])

    def test_drafts_pull_requests_and_other_repos_are_skipped(self):
        # Someone else's issue in autodev's repo is still listed: deciding
        # what to do with it is the dispatcher's job, not the snapshot's.
        code, lines, gh = run_tick("mixed_cards.json")
        self.assertEqual(code, 0)
        self.assertEqual(lines, [
            'board #7 "pokefirered autodev": 1 item, 3 other cards skipped',
            "#64 someone-else Todo has_parent=no",
        ])
        self.assertEqual(gh.mutations, [])

    def test_the_real_board_as_github_returned_it(self):
        # Recorded from the live board on 2026-10-19, read as the bot, while
        # it held parent #48 and its twelve tickets #49-#60 (GitHub adds a
        # parent's sub-issues along with it, and sets new cards to Todo).
        code, lines, gh = run_tick("live_board_2026-10-19.json")
        self.assertEqual(code, 0)
        self.assertEqual(lines, [
            'board #3 "pokefirered autodev": 13 items',
            "#48 MinimalEffort07 Todo has_parent=no",
        ] + ["#%d MinimalEffort07 Todo has_parent=yes" % n for n in range(49, 61)])
        self.assertEqual(gh.mutations, [])

    def test_a_tickets_parent_and_blockers_come_from_its_body(self):
        # From the same recording: #56's body ends "## Blocked by\n\n- #54\n
        # - #55"; #49's says "None (can start immediately)".
        gh = FakeGh.from_fixture("live_board_2026-10-19.json")
        items = {i.number: i for i in read_board(gh, CONFIG, 3).items}
        self.assertEqual(
            (items[56].title, items[56].parent, items[56].blocked_by,
             items[56].is_open),
            ("Autodev: ticket → PR", 48, (54, 55), True))
        self.assertEqual((items[49].parent, items[49].blocked_by), (48, ()))
        self.assertEqual((items[48].parent, items[48].blocked_by), (None, ()))

    def test_unusual_authors_and_parent_headings(self):
        # GitHub reports a deleted account's issues with no author; it
        # shows that account as "ghost". A "## Parent" heading may carry
        # the parent's number on the same line; "## Parenting" is not one.
        code, lines, gh = run_tick("odd_cards.json")
        self.assertEqual(code, 0)
        self.assertEqual(lines, [
            'board #7 "pokefirered autodev": 3 items',
            "#65 ghost Todo has_parent=no",
            "#66 MinimalEffort07 Todo has_parent=yes",
            "#67 MinimalEffort07 Todo has_parent=no",
        ])
        self.assertEqual(gh.mutations, [])

    def test_a_missing_board_is_an_error(self):
        # Only a board with exactly the configured title counts; GitHub's
        # search also returns near matches.
        code, lines, gh = run_tick("no_board.json")
        self.assertEqual(code, 1)
        self.assertEqual(lines, [
            'error: no board titled "pokefirered autodev" owned by MinimalEffort07',
        ])
        self.assertEqual(gh.mutations, [])


class FakeGitHub:
    """read_state's view of GitHub: each ticket's PR and when it was parked,
    each parent's sub-issues and which issues are open, recording what was
    asked for. An issue in `failing` can't be read."""

    def __init__(self, prs=None, parked=None, subs=None, open_issues=(),
                 failing=()):
        self.prs, self.parked, self.asked = prs or {}, parked or {}, []
        self.subs, self.open_issues, self.failing = subs or {}, open_issues, failing

    def ticket_pr(self, number, comments=True):
        self.asked.append(("pr", number, comments))
        return self.prs.get(number)

    def parked_at(self, number):
        self.asked.append(("parked_at", number))
        return self.parked.get(number, "")

    def sub_issues(self, number):
        self.asked.append(("sub_issues", number))
        if number in self.failing:
            raise GhError("HTTP 502")
        return self.subs.get(number, {})

    def is_open(self, number):
        self.asked.append(("is_open", number))
        return number in self.open_issues


class ReadState(unittest.TestCase):
    def test_a_parked_tickets_pr_and_when_it_was_parked_are_read(self):
        # So the owner can hand it back from its PR. A ticket in review gets
        # its PR's comments, one in Todo only whether it has a PR, and a
        # parked one with no PR doesn't need its parked time.
        from autodev.board import Board, Item
        from autodev.decide import PullRequest
        from autodev.dispatcher import read_state
        pr = PullRequest(number=92, head="issue-88-note", opened="2026-10-20T00:07:22Z")
        github = FakeGitHub({88: pr}, {88: "2026-10-19T21:05:37Z"})
        snapshot = Board(3, "pokefirered autodev", [
            Item(88, "MinimalEffort07-Autodev", "Needs human", True),
            Item(89, "MinimalEffort07-Autodev", "Needs human", True),
            Item(90, "MinimalEffort07-Autodev", "Todo", True),
            Item(91, "MinimalEffort07-Autodev", "In review", True),
        ], 0)
        code, state = read_state(github, NO_CLONE, snapshot, FakeRegistry(),
                                 io.StringIO())
        self.assertEqual(code, 0)
        self.assertEqual(github.asked, [
            ("pr", 88, True), ("parked_at", 88), ("pr", 89, True),
            ("pr", 90, False), ("pr", 91, True)])
        self.assertEqual(state.prs, {88: pr})
        self.assertEqual(state.parked_at, {88: "2026-10-19T21:05:37Z"})

    def test_the_owners_open_parents_sub_issues_are_read(self):
        # Their tickets' cards leave the board as they close (the board's
        # "Auto-archive items"), so whether a parent is done, or was grilled
        # already, comes from its sub-issues. Only parents in Todo or In
        # progress need it; one that can't be read is left out and logged.
        from autodev.board import Board, Item
        from autodev.dispatcher import read_state
        github = FakeGitHub(subs={45: {98: False, 100: False}}, failing=(46,))
        snapshot = Board(3, "pokefirered autodev", [
            Item(45, "MinimalEffort07", "In progress", False),
            Item(46, "MinimalEffort07", "In progress", False),
            Item(61, "MinimalEffort07", "Todo", False),
            Item(62, "MinimalEffort07", "Grilling", False),
            Item(63, "MinimalEffort07", "In progress", False, is_open=False),
            Item(64, "someone-else", "Todo", False),
        ], 0)
        log = io.StringIO()
        code, state = read_state(github, NO_CLONE, snapshot, FakeRegistry(), log)
        self.assertEqual(code, 1)
        self.assertEqual(github.asked, [
            ("sub_issues", 45), ("sub_issues", 46), ("sub_issues", 61)])
        self.assertEqual(state.sub_issues, {45: {98: False, 100: False}, 61: {}})
        self.assertEqual(log.getvalue().splitlines(),
                         ["error: reading #46's sub-issues: HTTP 502"])

    def test_a_worktree_whose_card_left_the_board_is_checked_on_github(self):
        # #70 closed and the board archived its card; #71 is open but off
        # the board; #72's card is still there, so the board says.
        from autodev.board import Board, Item
        from autodev.dispatcher import read_state
        with tempfile.TemporaryDirectory() as root:
            clone = os.path.join(root, "clone")
            for n in (70, 71, 72):
                os.makedirs(os.path.join(clone + "-worktrees", "issue-%d" % n))
            config = Config(owner="MinimalEffort07", repo="MinimalEffort07/pokefirered",
                            board_title="pokefirered autodev", clone=clone)
            github = FakeGitHub(open_issues=(71,))
            snapshot = Board(3, "pokefirered autodev", [
                Item(72, "MinimalEffort07-Autodev", "Done", True, is_open=False)], 0)
            code, state = read_state(github, config, snapshot, FakeRegistry(),
                                     io.StringIO())
        self.assertEqual(code, 0)
        self.assertEqual(github.asked, [("is_open", 70), ("is_open", 71)])
        self.assertEqual(state.closed, frozenset([70]))
        self.assertEqual(state.worktrees, frozenset([70, 71, 72]))


if __name__ == "__main__":
    unittest.main()
