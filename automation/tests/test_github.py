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
from autodev.decide import MergeOutcome, PrComment
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


PULLS = "repos/MinimalEffort07/pokefirered/pulls?state=open"


def pr(number, ref, author="MinimalEffort07-Autodev",
       repo="MinimalEffort07/pokefirered"):
    return {"number": number, "user": {"login": author},
            "head": {"ref": ref, "repo": {"full_name": repo} if repo else None}}


class TicketPRs(unittest.TestCase):
    """D15 for tickets: the session's PR, as GitHub shows it."""

    def outcome(self, pulls):
        return GitHub(FakeRest({PULLS: pulls}), CONFIG, BOARD).ticket_outcome(68)

    def test_only_the_bots_pr_from_this_tickets_branch_in_this_repo_counts(self):
        self.assertEqual(self.outcome([
            # Anyone can open a PR from a fork, with any branch name.
            pr(91, "issue-68-note", author="stranger", repo="stranger/pokefirered"),
            # A deleted fork shows no head repo at all.
            pr(92, "issue-68-note", author="stranger", repo=None),
            pr(93, "issue-680-other"),          # another ticket's branch
            pr(94, "issue-68-note", author="MinimalEffort07"),
            pr(90, "issue-68-note-the-grill-paths-live"),
        ]).pr, 90)

    def test_no_matching_pr_means_none(self):
        self.assertIsNone(self.outcome([pr(93, "issue-680-other")]).pr)


PR73 = "repos/MinimalEffort07/pokefirered/pulls/73"


def remark(id, author, body, at, when="created_at"):
    return {"id": id, "user": {"login": author}, "body": body, when: at}


class TicketPRTimeline(unittest.TestCase):
    """What the owner, the bot and others said on a ticket's PR (D9-D13)."""

    def test_comments_reviews_and_inline_comments_come_in_time_order(self):
        gets = {
            PULLS: [pr(73, "issue-68-note")],
            ISSUES + "/73/comments": [
                remark(1, "MinimalEffort07", "/merge", "2026-10-19T06:05:00Z")],
            PR73 + "/reviews": [
                remark(2, "MinimalEffort07", "Reword the note.",
                       "2026-10-19T06:03:00Z", when="submitted_at"),
                # An approval with nothing written says nothing to act on.
                remark(3, "MinimalEffort07", "", "2026-10-19T06:04:00Z",
                       when="submitted_at")],
            PR73 + "/comments": [
                remark(4, "stranger", "Inline nit", "2026-10-19T06:01:00Z")],
        }
        found = GitHub(FakeRest(gets), CONFIG, BOARD).ticket_pr(68)
        self.assertEqual((found.number, found.head, found.merged), (73, "issue-68-note", False))
        self.assertEqual(found.comments, (
            PrComment(4, "stranger", "Inline nit", "2026-10-19T06:01:00Z"),
            PrComment(2, "MinimalEffort07", "Reword the note.", "2026-10-19T06:03:00Z"),
            PrComment(1, "MinimalEffort07", "/merge", "2026-10-19T06:05:00Z"),
        ))

    def test_a_ticket_with_no_pr_of_the_bots_has_none(self):
        github = GitHub(FakeRest({PULLS: [pr(93, "issue-680-other")]}), CONFIG, BOARD)
        self.assertIsNone(github.ticket_pr(68))


class MergeOutcomes(unittest.TestCase):
    """D21 and the conflict path: what a finished merge session left."""

    def outcome(self, merged, comments):
        gets = {PR73: {"number": 73, "merged": merged},
                ISSUES + "/73/comments": comments}
        return GitHub(FakeRest(gets), CONFIG, BOARD).merge_outcome(73, 501)

    def test_merged_is_read_from_the_pr(self):
        self.assertEqual(self.outcome(True, []), MergeOutcome(merged=True, replied=False))

    def test_only_the_bots_reply_naming_that_merge_counts_as_its_answer(self):
        marker = "Resolved.\n\n<!-- autodev handled-through: %d -->"
        for comments, replied in [
            ([remark(9, "MinimalEffort07-Autodev", marker % 501, "t")], True),
            # An answer to an earlier /merge isn't an answer to this one.
            ([remark(9, "MinimalEffort07-Autodev", marker % 400, "t")], False),
            # The owner pasting the marker doesn't make it autodev's reply.
            ([remark(9, "MinimalEffort07", marker % 501, "t")], False),
        ]:
            with self.subTest(comments=comments):
                self.assertEqual(self.outcome(False, comments).replied, replied)


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
