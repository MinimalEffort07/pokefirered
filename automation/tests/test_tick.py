"""The dispatcher's tick: read the board, log what is on it, change nothing.

Each test replays a recorded board (tests/fixtures/*.json) through FakeGh
and checks the exact lines the tick logs.
"""

import io
import unittest

from autodev.config import Config
from autodev.dispatcher import tick
from tests.fakes import FakeGh

CONFIG = Config(
    owner="MinimalEffort07",
    repo="MinimalEffort07/pokefirered",
    board_title="pokefirered autodev",
)


def run_tick(fixture):
    gh = FakeGh.from_fixture(fixture)
    log = io.StringIO()
    code = tick(gh, CONFIG, log)
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

    def test_a_missing_board_is_an_error(self):
        # Only a board with exactly the configured title counts; GitHub's
        # search also returns near matches.
        code, lines, gh = run_tick("no_board.json")
        self.assertEqual(code, 1)
        self.assertEqual(lines, [
            'error: no board titled "pokefirered autodev" owned by MinimalEffort07',
        ])
        self.assertEqual(gh.mutations, [])


if __name__ == "__main__":
    unittest.main()
