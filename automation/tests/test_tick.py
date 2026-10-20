"""The first part of every tick: read the board and log what is on it.

Each test replays a recorded board (tests/fixtures/*.json) through FakeGh
and checks the exact lines the tick logs.
"""

import io
import unittest

from autodev.config import Config
from autodev.board import read_board
from autodev.dispatcher import log_board
from tests.fakes import FakeGh

CONFIG = Config(
    owner="MinimalEffort07",
    repo="MinimalEffort07/pokefirered",
    board_title="pokefirered autodev",
)


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
    recording what was asked for."""

    def __init__(self, prs, parked):
        self.prs, self.parked, self.asked = prs, parked, []

    def ticket_pr(self, number, comments=True):
        self.asked.append(("pr", number, comments))
        return self.prs.get(number)

    def parked_at(self, number):
        self.asked.append(("parked_at", number))
        return self.parked.get(number, "")


class FakeRegistry:
    def load(self, now=None):
        return []

    def paused_until(self):
        return 0.0

    def crashes(self):
        return {}

    def requeued(self):
        return set()


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
        config = Config(owner="MinimalEffort07", repo="MinimalEffort07/pokefirered",
                        board_title="pokefirered autodev", clone="/nonexistent/clone")
        code, state = read_state(github, config, snapshot, FakeRegistry(), io.StringIO())
        self.assertEqual(code, 0)
        self.assertEqual(github.asked, [
            ("pr", 88, True), ("parked_at", 88), ("pr", 89, True),
            ("pr", 90, False), ("pr", 91, True)])
        self.assertEqual(state.prs, {88: pr})
        self.assertEqual(state.parked_at, {88: "2026-10-19T21:05:37Z"})


if __name__ == "__main__":
    unittest.main()
