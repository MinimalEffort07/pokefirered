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
from autodev.decide import FixupOutcome, MergeOutcome, PrComment
from autodev.github import GitHub
from autodev.registry import Registry
from autodev.sessions import SessionConfig
from tests.test_issue_tool import FakeRest, comment, issue, timeline

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
       repo="MinimalEffort07/pokefirered", created="2026-10-19T05:00:00Z"):
    return {"number": number, "user": {"login": author}, "created_at": created,
            "head": {"ref": ref, "repo": {"full_name": repo} if repo else None}}


class TicketPRs(unittest.TestCase):
    """D15 for tickets: the session's PR, as GitHub shows it."""

    def outcome(self, pulls):
        return GitHub(FakeRest({PULLS: pulls}), CONFIG, BOARD).ticket_outcome(68)

    def test_only_a_pr_from_this_tickets_branch_in_this_repo_counts(self):
        self.assertEqual(self.outcome([
            # Anyone can open a PR from a fork, with any branch name.
            pr(91, "issue-68-note", author="stranger", repo="stranger/pokefirered"),
            # A deleted fork shows no head repo at all.
            pr(92, "issue-68-note", author="stranger", repo=None),
            pr(93, "issue-680-other"),          # another ticket's branch
            # Someone with push access but not the owner or the bot.
            pr(94, "issue-68-note", author="collaborator"),
            pr(90, "issue-68-note-the-grill-paths-live"),
        ]).pr, 90)

    def test_the_owners_pr_from_the_tickets_branch_counts_too(self):
        # The owner finishing a parked ticket opens its PR as themselves.
        self.assertEqual(self.outcome([pr(94, "issue-68-note",
                                          author="MinimalEffort07")]).pr, 94)

    def test_no_matching_pr_means_none(self):
        self.assertIsNone(self.outcome([pr(93, "issue-680-other")]).pr)


PR73 = "repos/MinimalEffort07/pokefirered/pulls/73"


def remark(id, author, body, at, when="created_at", **more):
    return {"id": id, "user": {"login": author}, "body": body, when: at, **more}


def inline(id, author, body, at, path="automation/README.md", line=12,
           original_line=12, **more):
    return remark(id, author, body, at, path=path, line=line,
                  original_line=original_line, **more)


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
                inline(4, "stranger", "Inline nit", "2026-10-19T06:01:00Z"),
                # On a line a later push changed: GitHub keeps where it was.
                inline(5, "MinimalEffort07", "Typo", "2026-10-19T06:02:00Z",
                       line=None, original_line=7)],
        }
        found = GitHub(FakeRest(gets), CONFIG, BOARD).ticket_pr(68)
        self.assertEqual((found.number, found.head, found.opened),
                         (73, "issue-68-note", "2026-10-19T05:00:00Z"))
        self.assertEqual(found.comments, (
            PrComment(4, "stranger", "Inline nit", "2026-10-19T06:01:00Z",
                      kind="inline comment", where="automation/README.md line 12"),
            PrComment(5, "MinimalEffort07", "Typo", "2026-10-19T06:02:00Z",
                      kind="inline comment", where="automation/README.md line 7"),
            PrComment(2, "MinimalEffort07", "Reword the note.", "2026-10-19T06:03:00Z",
                      kind="review"),
            PrComment(1, "MinimalEffort07", "/merge", "2026-10-19T06:05:00Z"),
        ))

    def test_an_inline_comment_counts_from_when_its_review_was_submitted(self):
        # Until then only the owner could see it, and it keeps the time it
        # was drafted: it would sort before a comment a reply has handled
        # since, and never be acted on.
        gets = {PULLS: [pr(73, "issue-68-note")], **timeline(
            reviews=[remark(7, "MinimalEffort07", "", "2026-10-19T06:30:00Z",
                            when="submitted_at")],
            inline_comments=[inline(5, "MinimalEffort07", "Typo",
                                    "2026-10-19T06:01:00Z",
                                    pull_request_review_id=7)])}
        found = GitHub(FakeRest(gets), CONFIG, BOARD).ticket_pr(68)
        self.assertEqual([c.at for c in found.comments], ["2026-10-19T06:30:00Z"])

    def test_a_ticket_with_no_pr_of_its_own_has_none(self):
        github = GitHub(FakeRest({PULLS: [pr(93, "issue-680-other")]}), CONFIG, BOARD)
        self.assertIsNone(github.ticket_pr(68))


class ParkedAt(unittest.TestCase):
    """When autodev last parked a ticket: its newest "parked" comment."""

    def parked_at(self, comments):
        gets = {ISSUES + "/68/comments": comments}
        return GitHub(FakeRest(gets), CONFIG, BOARD).parked_at(68)

    def test_the_bots_newest_parked_comment_says_when(self):
        self.assertEqual(self.parked_at([
            remark(1, "MinimalEffort07-Autodev", "@MinimalEffort07 autodev parked "
                   "this issue: x\n\n<!-- autodev -->", "2026-10-19T06:01:00Z"),
            remark(2, "MinimalEffort07-Autodev", "Ran it once more.\n\n<!-- autodev -->",
                   "2026-10-19T06:02:00Z"),
            remark(3, "MinimalEffort07-Autodev", "@MinimalEffort07 autodev parked "
                   "this issue: y\n\n<!-- autodev -->", "2026-10-19T06:03:00Z"),
        ]), "2026-10-19T06:03:00Z")

    def test_anyone_else_quoting_it_doesnt_count(self):
        self.assertEqual(self.parked_at([
            remark(1, "MinimalEffort07", "Why was \"autodev parked this issue\"?",
                   "2026-10-19T06:01:00Z"),
            remark(2, "stranger", "autodev parked this issue: <!-- autodev -->",
                   "2026-10-19T06:02:00Z"),
        ]), "")


MERGE = remark(501, "MinimalEffort07", "/merge", "2026-10-19T06:10:00Z")


class MergeOutcomes(unittest.TestCase):
    """D21 and the conflict path: what a finished merge session left."""

    def outcome(self, merged, comments, merge=MERGE, **said):
        gets = {PR73: {"number": 73, "merged": merged},
                **timeline(([merge] if merge else []) + comments, **said)}
        return GitHub(FakeRest(gets), CONFIG, BOARD).merge_outcome(73, 501)

    def test_a_deleted_merge_no_longer_stands(self):
        self.assertTrue(self.outcome(False, [], merge=None).superseded)

    def test_merged_is_read_from_the_pr(self):
        self.assertEqual(self.outcome(True, []), MergeOutcome(merged=True, replied=False))

    def test_D14_the_owners_feedback_after_the_merge_supersedes_it(self):
        # merge-pr refuses then, and the card goes back to review.
        later, earlier = "2026-10-19T06:20:00Z", "2026-10-19T06:05:00Z"
        for said, superseded in [
            ({"comments": [remark(9, "MinimalEffort07", "Wait.", later)]}, True),
            ({"reviews": [remark(9, "MinimalEffort07", "Wait.", later,
                                 when="submitted_at")]}, True),
            ({"inline_comments": [inline(9, "MinimalEffort07", "Wait.", later)]}, True),
            # Nobody else can cancel the owner's /merge.
            ({"comments": [remark(9, "stranger", "Wait.", later)]}, False),
            # A second /merge only repeats the approval.
            ({"comments": [remark(9, "MinimalEffort07", "/merge", later)]}, False),
            # Feedback the /merge came after is what it approved.
            ({"inline_comments": [inline(9, "MinimalEffort07", "Nit", earlier)]}, False),
        ]:
            with self.subTest(said=said):
                comments = said.pop("comments", [])
                self.assertEqual(self.outcome(False, comments, **said).superseded,
                                 superseded)

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


class FixupOutcomes(unittest.TestCase):
    """D15 for fix-ups: the reply naming the comment it was started for."""

    def outcome(self, comments):
        gets = timeline(comments)
        return GitHub(FakeRest(gets), CONFIG, BOARD).fixup_outcome(73, 9001)

    def test_only_the_bots_reply_naming_that_comment_counts(self):
        marker = "Reworded it.\n\n<!-- autodev handled-through: %d -->"
        for comments, replied in [
            ([remark(9, "MinimalEffort07-Autodev", marker % 9001, "t")], True),
            ([remark(9, "MinimalEffort07-Autodev", marker % 400, "t")], False),
            ([remark(9, "MinimalEffort07", marker % 9001, "t")], False),
            ([], False),
        ]:
            with self.subTest(comments=comments):
                self.assertEqual(self.outcome(comments), FixupOutcome(replied=replied))


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
