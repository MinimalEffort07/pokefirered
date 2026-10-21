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
from autodev.gh import GhError
from autodev.github import GitHub
from autodev.registry import Registry
from autodev.sessions import SessionConfig
from tests.test_issue_tool import FakeRest, changes, comment, issue, timeline

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

    def test_143_D1_only_the_bots_design_comment_can_mark_it_breaking(self):
        # #143: a breaking design holds its parent in Paused, so the marker
        # counts only where the grill puts it, next to the design marker.
        breaking = ("Design...\n## Breaking change\n\nOld saves stop loading.\n"
                    "<!-- autodev:design -->\n<!-- autodev:breaking -->\n\n"
                    "<!-- autodev -->")
        for comments, expected in [
            ([comment("MinimalEffort07-Autodev", breaking)], True),
            ([comment("MinimalEffort07-Autodev", DESIGN)], False),
            ([], False),
            # The marker in another of the bot's comments, or in someone
            # else's comment beside the bot's design, doesn't count.
            ([comment("MinimalEffort07-Autodev", DESIGN),
              comment("MinimalEffort07-Autodev",
                      "A note.\n<!-- autodev:breaking -->\n\n<!-- autodev -->")], False),
            ([comment("MinimalEffort07-Autodev", DESIGN),
              comment("MinimalEffort07", breaking)], False),
            ([comment("someone-else", breaking)], False),
            # A design that only writes the marker in code, as #139's did
            # when describing it, shows it as text: it isn't the marker.
            ([comment("MinimalEffort07-Autodev",
                      "The hidden marker `<!-- autodev:breaking -->`.\n"
                      "<!-- autodev:design -->\n\n<!-- autodev -->")], False),
            # A stray backtick earlier on can't hide the real marker by
            # pairing with one after it: in doubt, the design is held.
            ([comment("MinimalEffort07-Autodev",
                      "Run `make.\n<!-- autodev:design -->\n<!-- autodev:breaking -->"
                      "\nThen `make check`.\n\n<!-- autodev -->")], True),
        ]:
            with self.subTest(comments=comments):
                result = outcome({ISSUES + "/61/comments": comments,
                                  ISSUES + "/61/sub_issues": []}, [])
                self.assertEqual(result.breaking, expected)


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


class SubIssues(unittest.TestCase):
    def test_a_parents_tickets_among_its_sub_issues_and_whether_each_is_open(self):
        # A parent's tickets as GitHub links them, which outlasts their
        # cards: the board archives a card as its issue closes. A sub-issue
        # counts only if it's a ticket of this parent, as on the board.
        gets = {ISSUES + "/45/sub_issues": [
            ticket(98, 45, state="closed"), ticket(99, 45),
            ticket(101, 48),                                # another parent's
            # Only the first issue a "## Parent" section names counts.
            issue(103, "MinimalEffort07-Autodev", "## Parent\n\n#48, see also #45"),
            issue(102, "MinimalEffort07", "Linked by hand.")]}  # not a ticket
        self.assertEqual(GitHub(FakeRest(gets), CONFIG, BOARD).sub_issues(45),
                         {98: False, 99: True})


class FailingRest(FakeRest):
    """FakeRest whose GET of `failing` fails as gh reports it."""

    def __init__(self, gets, failing):
        super().__init__(gets)
        self.failing = failing

    def rest(self, method, path, **fields):
        if method == "GET" and path == self.failing:
            raise GhError("HTTP 500: this diff is taking too long to generate")
        return super().rest(method, path, **fields)


class RefusingGh:
    """GraphQL that refuses every change with `message`, as gh reports it."""

    def __init__(self, message):
        self.message = message

    def graphql(self, query, **variables):
        raise GhError(self.message)


class SettingStatus(unittest.TestCase):
    STATUS_BOARD = Board(3, "pokefirered autodev",
                         [Item(45, "MinimalEffort07", "In progress", False, "PVTI_45")],
                         0, id="PVT_3", status_field_id="PVTSSF_3",
                         status_options={"Done": "opt-done"})

    def test_a_card_the_board_archived_during_the_tick_is_left_alone(self):
        # Closing #45 a moment earlier made the board archive its card, and
        # GitHub won't change an archived card. It's off the board, so
        # there's no Status to keep.
        gh = RefusingGh("The item is archived and cannot be updated")
        GitHub(gh, CONFIG, self.STATUS_BOARD).set_status(45, "Done")

    def test_any_other_refusal_still_fails(self):
        gh = RefusingGh("Could not resolve to a node with the global id")
        with self.assertRaises(GhError):
            GitHub(gh, CONFIG, self.STATUS_BOARD).set_status(45, "Done")


PULLS = "repos/MinimalEffort07/pokefirered/pulls?state=open"


def pr(number, ref, author="MinimalEffort07-Autodev",
       repo="MinimalEffort07/pokefirered", created="2026-10-19T05:00:00Z",
       body="Fixes #68", sha="abc123"):
    return {"number": number, "user": {"login": author}, "created_at": created,
            "body": body, "head": {"ref": ref, "sha": sha,
                                   "repo": {"full_name": repo} if repo else None}}


# What GitHub's compare says commit abc123 changes since it left master.
COMPARE = "repos/MinimalEffort07/pokefirered/compare/master...abc123"


NOTHING_BREAKS = {COMPARE: changes("automation/README.md")}


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
            PULLS: [pr(73, "issue-68-note")], **NOTHING_BREAKS,
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
        gets = {PULLS: [pr(73, "issue-68-note")], **NOTHING_BREAKS, **timeline(
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


SECTION = ("## Breaking change\n\nA save made before loses its PC items. "
           "Comment `/merge breaking` to approve it.")


class BreakingPRs(unittest.TestCase):
    """B9: which ticket PRs merge only on the owner's exact /merge breaking
    (CLAUDE.md, "Breaking changes"), and why, as the tick reads them."""

    def breaking(self, compare=None, body="Fixes #68", comments=()):
        gets = {PULLS: [pr(73, "issue-68-note", body=body)],
                COMPARE: changes("automation/README.md") if compare is None else compare,
                **timeline(comments)}
        return GitHub(FakeRest(gets), CONFIG, BOARD).ticket_pr(68).breaking

    def test_a_pr_that_changes_the_save_format_record_in_any_way_is_breaking(self):
        record = "test/save_format.txt"
        for change in [{"filename": record, "status": status}
                       for status in ("added", "modified", "removed")] + [
                {"filename": record, "status": "renamed",
                 "previous_filename": "test/old_format.txt"},
                {"filename": "test/old_format.txt", "status": "renamed",
                 "previous_filename": record}]:
            with self.subTest(change=change):
                self.assertEqual(self.breaking(changes("README.md", change)),
                                 "it changes `test/save_format.txt`")

    def test_a_pr_that_leaves_the_record_alone_breaks_nothing(self):
        self.assertEqual(self.breaking(changes(
            "test/tools/save_format.py", "test/tests/test_save_format.lua",
            {"filename": "test/save_format.txt.orig", "status": "added"})), "")

    def test_C7_a_pr_that_removes_a_line_from_the_id_record_is_breaking(self):
        # A recorded saved ID whose number changed, or that is gone, loses
        # its line in test/save_ids.txt (#142); GitHub counts the lines a
        # branch removes from each file.
        ids = "test/save_ids.txt"
        for change in [
                # ITEM_POTION renumbered: its line goes, a new one comes.
                {"filename": ids, "status": "modified", "additions": 1, "deletions": 1},
                {"filename": ids, "status": "modified", "additions": 0, "deletions": 3},
                {"filename": ids, "status": "removed", "additions": 0, "deletions": 4749},
                # Moved away, the record no longer stands where it's checked.
                {"filename": "test/old_ids.txt", "status": "renamed",
                 "previous_filename": ids, "additions": 0, "deletions": 0}]:
            with self.subTest(change=change):
                self.assertEqual(self.breaking(changes("README.md", change)),
                                 "it removes a line from `test/save_ids.txt`")
        # Without GitHub's count, it can't tell, so it counts as breaking.
        self.assertEqual(
            self.breaking(changes({"filename": ids, "status": "modified"})),
            "GitHub doesn't say whether it removes a line from `test/save_ids.txt`")

    def test_C7_a_pr_that_only_adds_lines_to_the_id_record_breaks_nothing(self):
        # A new ID only adds a line, and isn't a breaking change.
        for change in [
                {"filename": "test/save_ids.txt", "status": "modified",
                 "additions": 2, "deletions": 0},
                # The PR that first adds the record.
                {"filename": "test/save_ids.txt", "status": "added",
                 "additions": 4749, "deletions": 0}]:
            with self.subTest(change=change):
                self.assertEqual(self.breaking(changes("include/constants/items.h",
                                                       change)), "")

    def test_a_pr_whose_files_github_cant_all_list_is_breaking(self):
        # The compare lists at most 300 files, so 300 may not be all.
        many = ["src/file_%d.c" % n for n in range(300)]
        cant_tell = ("GitHub can't list all of its changed files, so autodev "
                     "can't tell whether it changes `test/save_format.txt` or "
                     "removes a line from `test/save_ids.txt`")
        for compare in (changes(*many), {"files": None}, {}):
            with self.subTest(files=len(compare.get("files") or [])):
                self.assertEqual(self.breaking(compare), cant_tell)
        # One it could list says more.
        self.assertEqual(self.breaking(changes(*many[1:] + ["test/save_format.txt"])),
                         "it changes `test/save_format.txt`")
        # Nor can it when the compare fails (a diff too big to work out, or
        # GitHub having a bad minute): the PR is still answered, as breaking,
        # rather than dropped from the tick.
        gets = {PULLS: [pr(73, "issue-68-note")], **timeline()}
        github = GitHub(FailingRest(gets, COMPARE), CONFIG, BOARD)
        self.assertEqual(github.ticket_pr(68).breaking, cant_tell)

    def test_a_breaking_change_section_in_its_body_or_a_bot_reply_is_breaking(self):
        self.assertEqual(self.breaking(body="Fixes #68\n\n" + SECTION),
                         "its body has a Breaking change section")
        for kind in ("comments", "reviews"):
            with self.subTest(kind=kind):
                reply = remark(9, "MinimalEffort07-Autodev",
                               "Did it.\n\n" + SECTION, "2026-10-19T06:01:00Z",
                               when="created_at" if kind == "comments" else "submitted_at")
                gets = {PULLS: [pr(73, "issue-68-note")], COMPARE: changes("README.md"),
                        **timeline(**{kind: [reply]})}
                self.assertEqual(
                    GitHub(FakeRest(gets), CONFIG, BOARD).ticket_pr(68).breaking,
                    "a reply of autodev's on it has a Breaking change section")

    def test_the_owners_comment_or_a_heading_in_code_is_not_a_section(self):
        # Only the PR's own words count: the owner quoting the heading
        # doesn't make it a breaking change, nor does a heading GitHub shows
        # as code, nor someone else's comment.
        for body, comments in [
                ("Fixes #68", [remark(9, "MinimalEffort07", SECTION, "t")]),
                ("Fixes #68", [remark(9, "stranger", SECTION, "t")]),
                ("Fixes #68\n\n```\n" + SECTION + "\n```", []),
                ("Fixes #68 and a `## Breaking change` heading", []),
                ("Fixes #68\n\nBreaking change: none.", [])]:
            with self.subTest(body=body, comments=comments):
                self.assertEqual(self.breaking(body=body, comments=comments), "")

    def test_without_its_comments_the_tick_doesnt_ask(self):
        # A ticket in Todo only needs to know whether it has a PR.
        github = GitHub(FakeRest({PULLS: [pr(73, "issue-68-note")]}), CONFIG, BOARD)
        self.assertEqual(github.ticket_pr(68, comments=False).breaking, "")


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

    def outcome(self, merged, comments, merge=MERGE, compare=None, **said):
        gets = {PR73: {"number": 73, "merged": merged, "body": "Fixes #68",
                       "head": {"ref": "issue-68-note", "sha": "abc123"}},
                COMPARE: changes("automation/README.md") if compare is None else compare,
                **timeline(([merge] if merge else []) + comments, **said)}
        return GitHub(FakeRest(gets), CONFIG, BOARD).merge_outcome(73, 501)

    def test_a_deleted_merge_no_longer_stands(self):
        self.assertTrue(self.outcome(False, [], merge=None).superseded)

    def test_B8_a_breaking_pr_answered_with_a_plain_merge_wasnt_approved(self):
        # merge-pr refuses then, and the card goes back to review, where the
        # dispatcher's reply explains.
        record = changes("test/save_format.txt")
        breaking_merge = remark(501, "MinimalEffort07", "/merge breaking",
                                "2026-10-19T06:10:00Z")
        for merged, merge, compare, unapproved in [
                (False, MERGE, record, True),
                (False, breaking_merge, record, False),
                # A PR that breaks nothing needs no more than /merge.
                (False, MERGE, None, False),
                # Merged is merged.
                (True, MERGE, record, False)]:
            with self.subTest(merged=merged, merge=merge["body"], compare=compare):
                self.assertEqual(self.outcome(merged, [], merge=merge,
                                              compare=compare).breaking_unapproved,
                                 unapproved)

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
