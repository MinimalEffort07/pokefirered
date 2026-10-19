"""What the dispatcher reads from GitHub after a grill session ends.

decide() only believes a session's "done" if this says the work is there,
so these checks are what stands between a session's claim and the parent
moving on (D15, D16).
"""

import os
import tempfile
import time
import unittest

from autodev.board import Board, Item
from autodev.config import Config
from autodev.github import GitHub
from autodev.registry import Registry
from autodev.sessions import SessionConfig
from tests.test_issue_tool import FakeRest, comment, issue

CONFIG = Config(owner="MinimalEffort07", repo="MinimalEffort07/pokefirered",
                board_title="pokefirered autodev", bot="MinimalEffort07-Autodev")
ISSUES = "repos/MinimalEffort07/pokefirered/issues"
BOARD = Board(3, "pokefirered autodev",
              [Item(61, "MinimalEffort07", "Grilling", False, "PVTI_61"),
               Item(70, "MinimalEffort07-Autodev", "Todo", True, "PVTI_70")], 0)
DESIGN = "Design...\n<!-- autodev:design -->\n\n<!-- autodev -->"


def ticket(number, parent, blocked_by="None (can start immediately)", state="open"):
    return issue(number, "MinimalEffort07-Autodev",
                 "## Parent\n\n#%d\n\n## What to build\n\nIt.\n\n## Blocked by\n\n%s"
                 % (parent, blocked_by), state=state)


def outcome(gets, reported):
    return GitHub(FakeRest(gets), CONFIG, BOARD).grill_outcome(61, reported)


class DesignComment(unittest.TestCase):
    def test_only_the_bots_comment_with_the_marker_counts(self):
        for comments, expected in [
            ([comment("MinimalEffort07-Autodev", DESIGN)], True),
            # The owner pasting the marker doesn't make it autodev's design.
            ([comment("MinimalEffort07", DESIGN)], False),
            ([comment("MinimalEffort07-Autodev", "Design without the marker")], False),
        ]:
            with self.subTest(comments=comments):
                result = outcome({ISSUES + "/61/comments": comments,
                                  ISSUES + "/61/sub_issues": []}, [])
                self.assertEqual(result.design_comment, expected)


class Tickets(unittest.TestCase):
    def test_reported_and_linked_issues_are_checked_against_their_parent(self):
        gets = {
            ISSUES + "/61/comments": [],
            # #71 is linked but wasn't reported; #70 was reported.
            ISSUES + "/61/sub_issues": [{"number": 71}],
            ISSUES + "/70": ticket(70, 61),
            ISSUES + "/71": ticket(71, 61, blocked_by="- #70"),
            # Reported, but its Parent section names another issue.
            ISSUES + "/72": ticket(72, 48),
            # Reported, but already closed.
            ISSUES + "/73": ticket(73, 61, state="closed"),
        }
        result = outcome(gets, [70, 72, 73])
        self.assertEqual([(t.number, t.linked, t.on_board, t.status, t.open_blockers)
                          for t in result.tickets], [
            (70, False, True, "Todo", False),
            (71, True, False, None, True),     # #70 is still open
        ])


class Pruning(unittest.TestCase):
    def test_session_files_older_than_30_days_are_deleted(self):
        with tempfile.TemporaryDirectory() as root:
            logs, done = os.path.join(root, "logs"), os.path.join(root, "state", "done")
            os.makedirs(logs)
            os.makedirs(done)
            now = time.time()
            for path, age_days in [(os.path.join(logs, "old.jsonl"), 31),
                                   (os.path.join(logs, "new.jsonl"), 29),
                                   (os.path.join(done, "old.json"), 40)]:
                open(path, "w").close()
                os.utime(path, (now - age_days * 86400,) * 2)
            registry = Registry(SessionConfig("", "", "", "", logs,
                                              os.path.join(root, "state"), ""))
            self.assertEqual(registry.prune(now=now), 2)
            self.assertEqual(os.listdir(logs), ["new.jsonl"])
            self.assertEqual(os.listdir(done), [])


if __name__ == "__main__":
    unittest.main()
