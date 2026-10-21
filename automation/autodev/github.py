"""Carrying out decide()'s actions on GitHub, and reading what a finished
session left there.

Everything here runs as the bot: the dispatcher's unit loads the bot's
GH_TOKEN, and gh uses it for every call.
"""

import re
from dataclasses import replace

from autodev.board import issue_numbers, section
from autodev.decide import (HANDLED_THROUGH, MARKER, PARKED, FixupOutcome,
                            GrillOutcome, MergeOutcome, PrComment, PullRequest,
                            Ticket, TicketOutcome, approves_breaking,
                            merge_cancelled)
from autodev.gh import GhError
from autodev.worktrees import branch_prefix

# The grill session marks its design comment with this hidden tag
# (CLAUDE.md, "Autonomous mode"), which is how the dispatcher finds it.
DESIGN_MARKER = "<!-- autodev:design -->"
# A design that is a breaking change carries this beside the design marker
# (CLAUDE.md, step 3), so its parent waits in Paused for the owner (#143).
BREAKING_MARKER = "<!-- autodev:breaking -->"

SET_STATUS = """
mutation SetStatus($board: ID!, $item: ID!, $field: ID!, $option: String!) {
  updateProjectV2ItemFieldValue(input: {projectId: $board, itemId: $item,
      fieldId: $field, value: {singleSelectOptionId: $option}}) {
    clientMutationId
  }
}
"""

# What GitHub says when asked to change an archived card.
ARCHIVED = "The item is archived and cannot be updated"

ADD_TO_BOARD = """
mutation AddToBoard($board: ID!, $issue: ID!) {
  addProjectV2ItemById(input: {projectId: $board, contentId: $issue}) {
    item { id }
  }
}
"""


class GitHub:
    def __init__(self, gh, config, board):
        self.gh = gh
        self.config = config
        self.board = board
        self.items = {item.number: item for item in board.items}
        self._open_pulls = None     # read once per tick, when first needed

    def _issue(self, number):
        return self.gh.rest("GET", "repos/%s/issues/%d" % (self.config.repo, number))

    def is_open(self, number):
        return self._issue(number)["state"] == "open"

    def sub_issues(self, parent):
        """{number: is_open} for each of `parent`'s sub-issues that is one of
        its tickets: the first issue its "## Parent" section names is
        `parent`, as on the board. A sub-issue the owner linked by hand
        isn't one."""
        subs = self.gh.rest_list("repos/%s/issues/%d/sub_issues"
                                 % (self.config.repo, parent))
        return {s["number"]: s["state"] == "open" for s in subs
                if issue_numbers(section(s["body"], "Parent"))[:1] == [parent]}

    # ── actions ───────────────────────────────────────────────────────────

    def set_status(self, issue, status):
        self._set_item_status(self.items[issue].item_id, status)

    def _set_item_status(self, item_id, status):
        try:
            self.gh.graphql(SET_STATUS, board=self.board.id, item=item_id,
                            field=self.board.status_field_id,
                            option=self.board.status_options[status])
        except GhError as e:
            # The board archives a card as its issue closes, so a card read
            # at the start of the tick may be archived by the time its
            # Status is set, just after the tick closed its issue. It's off
            # the board, so there's no Status left to keep.
            if ARCHIVED not in str(e):
                raise

    def close_issue(self, issue):
        self.gh.rest("PATCH", "repos/%s/issues/%d" % (self.config.repo, issue),
                     state="closed", state_reason="completed")

    def comment(self, issue, body):
        self.gh.rest("POST", "repos/%s/issues/%d/comments" % (self.config.repo, issue),
                     body=body)

    def link_sub_issue(self, parent, child):
        # The sub-issues API wants the child's numeric id, not its number.
        child_id = self._issue(child)["id"]
        self.gh.rest("POST", "repos/%s/issues/%d/sub_issues" % (self.config.repo, parent),
                     sub_issue_id=child_id)

    def place_on_board(self, issue, status):
        if issue in self.items:
            item_id = self.items[issue].item_id
        else:
            # Adding an issue that is already on the board just returns
            # its existing card, so this is safe to repeat.
            data = self.gh.graphql(ADD_TO_BOARD, board=self.board.id,
                                   issue=self._issue(issue)["node_id"])
            item_id = data["data"]["addProjectV2ItemById"]["item"]["id"]
        self._set_item_status(item_id, status)

    # ── reading a finished grill ──────────────────────────────────────────

    def grill_outcome(self, parent, reported):
        """What GitHub shows for `parent` after its grill session ended.
        `reported` is the ticket numbers the session listed in its result;
        the parent's sub-issues are checked too, in case it missed one."""
        repo = self.config.repo
        comments = self.gh.rest_list("repos/%s/issues/%d/comments" % (repo, parent))
        designs = [c["body"] for c in comments if c["user"]["login"] == self.config.bot
                   and DESIGN_MARKER in (c["body"] or "")]
        # Only the design comment can mark the design breaking, and only
        # outside code: written in code, the marker is text describing it.
        # A re-queued grill may have posted two; either one holds the parent.
        breaking = any(BREAKING_MARKER in prose(body) for body in designs)
        subs = self.gh.rest_list("repos/%s/issues/%d/sub_issues" % (repo, parent))
        linked = {s["number"] for s in subs}
        tickets = []
        for number in sorted(set(reported) | linked):
            issue = self._issue(number)
            # Only an open issue whose "## Parent" section names this
            # parent counts as one of its tickets.
            if issue["state"] != "open" or parent not in issue_numbers(
                    section(issue["body"], "Parent")):
                continue
            blockers = issue_numbers(section(issue["body"], "Blocked by"))
            item = self.items.get(number)
            tickets.append(Ticket(
                number=number,
                linked=number in linked,
                on_board=item is not None,
                status=item.status if item else None,
                open_blockers=any(self._issue(b)["state"] == "open"
                                  for b in blockers),
            ))
        return GrillOutcome(design_comment=bool(designs), tickets=tickets,
                            breaking=breaking)

    # ── a ticket's PR ─────────────────────────────────────────────────────

    def _find_ticket_pr(self, number):
        """Ticket `number`'s open PR as GitHub lists it (is_ticket_pr), or
        None."""
        if self._open_pulls is None:
            self._open_pulls = self.gh.rest_list(
                "repos/%s/pulls?state=open" % self.config.repo)
        for pr in self._open_pulls:
            if is_ticket_pr(self.config, number, pr):
                return pr
        return None

    def ticket_outcome(self, number):
        """What GitHub shows for a ticket after its ticket session ended."""
        pr = self._find_ticket_pr(number)
        return TicketOutcome(pr=pr["number"] if pr else None)

    def ticket_pr(self, number, comments=True):
        """The bot's open PR for ticket `number`, with everything said on it
        and why it is a breaking change (unless `comments` is False), or
        None."""
        pr = self._find_ticket_pr(number)
        if pr is None:
            return None
        found = PullRequest(number=pr["number"], head=pr["head"]["ref"],
                            opened=pr["created_at"])
        if not comments:
            return found
        repo = self.config.repo
        said = pr_timeline(self.gh, repo, pr["number"])
        return replace(found, comments=said, breaking=breaking_reason(
            self.config, changed_files(self.gh, repo, pr["head"]["sha"]),
            pr["body"], said))

    def parked_at(self, number):
        """When autodev last parked issue `number` in Needs human: the time
        of the bot's newest comment that says so (decide.PARKED), or "" if
        there is none."""
        comments = self.gh.rest_list("repos/%s/issues/%d/comments"
                                     % (self.config.repo, number))
        return max((c["created_at"] for c in comments
                    if c["user"]["login"] == self.config.bot
                    and PARKED in (c["body"] or "") and MARKER in (c["body"] or "")),
                   default="")

    def merge_outcome(self, pr, handled_through):
        """Whether the PR is merged, whether the bot replied on it naming
        the /merge comment the merge session answered, whether the owner
        has withdrawn that /merge since, and whether the PR is a breaking
        change that /merge, being a plain one, couldn't merge."""
        repo = self.config.repo
        data = self.gh.rest("GET", "repos/%s/pulls/%d" % (repo, pr))
        said = pr_timeline(self.gh, repo, pr)
        return MergeOutcome(
            merged=data["merged"], replied=self._replied(said, handled_through),
            superseded=merge_cancelled(self.config.owner, said,
                                       handled_through) is not None,
            breaking_unapproved=not data["merged"] and bool(needs_merge_breaking(
                self.gh, self.config, data["head"]["sha"], data["body"], said,
                handled_through)))

    def fixup_outcome(self, pr, handled_through):
        """Whether the bot replied on the PR naming the comment the fix-up
        session was started for."""
        said = pr_timeline(self.gh, self.config.repo, pr)
        return FixupOutcome(replied=self._replied(said, handled_through))

    def _replied(self, said, comment_id):
        return any(c.author == self.config.bot and _handles(c.body, comment_id)
                   for c in said)


def is_ticket_pr(config, ticket, pr):
    """Whether `pr` (GitHub's record of a PR) is ticket `ticket`'s: from the
    ticket's own branch, in this repo, opened by the bot or by the owner
    (who may finish a parked ticket by hand). Anyone can open a PR from a
    fork with any branch name, so a fork's never counts."""
    head = pr["head"]
    return (head["ref"].startswith(branch_prefix(ticket))
            and (head["repo"] or {}).get("full_name") == config.repo
            and pr["user"]["login"] in (config.bot, config.owner))


def pr_timeline(gh, repo, pr):
    """Everything said on a PR (PrComment), oldest first: comments in its
    conversation, reviews, and inline review comments. GitHub keeps the
    three apart, and a review is dated by when it was submitted."""
    said = [PrComment(c["id"], c["user"]["login"], c["body"] or "", c["created_at"])
            for c in gh.rest_list("repos/%s/issues/%d/comments" % (repo, pr))]
    reviews = gh.rest_list("repos/%s/pulls/%d/reviews" % (repo, pr))
    submitted = {r["id"]: r["submitted_at"] or "" for r in reviews}
    # An inline comment made in a review keeps the time it was drafted, but
    # nobody else saw it until the review was submitted, so that's when it
    # was said. One on a line a later push changed has no `line` any more;
    # `original_line` is where it was made.
    said += [PrComment(c["id"], c["user"]["login"], c["body"] or "",
                       max(c["created_at"],
                           submitted.get(c.get("pull_request_review_id"), "")),
                       kind="inline comment", where="%s line %s" % (
                           c["path"], c.get("line") or c.get("original_line")))
             for c in gh.rest_list("repos/%s/pulls/%d/comments" % (repo, pr))]
    # An approval with nothing written says nothing to act on.
    said += [PrComment(r["id"], r["user"]["login"], r["body"], r["submitted_at"],
                       kind="review")
             for r in reviews if r["body"]]
    return tuple(sorted(said, key=lambda c: c.at))


def prose(body):
    """The body without fenced blocks and `code spans`: GitHub neither
    notifies a mention, closes an issue nor shows a heading written inside
    code."""
    return re.sub(r"```.*?```|`[^`]*`", "", body or "", flags=re.DOTALL)


# ── breaking changes ─────────────────────────────────────────────────────
#
# A breaking change (CLAUDE.md, "Breaking changes") merges only on the
# owner's exact "/merge breaking" (#139). The save format record catches a
# change to how the save is laid out; the rest only a session can see, so
# it declares them in a "## Breaking change" section.

# The save format, as agbcc lays it out (test/tools/save_format.py). Any
# change to it is a breaking change.
SAVE_FORMAT = "test/save_format.txt"

# GitHub's compare lists at most this many of the files a branch changes,
# so a list this long may not be all of them.
COMPARE_FILE_LIMIT = 300

# A "## Breaking change" heading line. On GitHub a line starting "## " is
# a heading even indented by up to three spaces; "changes", and more words
# after it, count too: whatever the owner would read as that heading is.
BREAKING_HEADING = r"^ {0,3}##[ \t]+Breaking changes?\b[^\n]*"


def changed_files(gh, repo, head):
    """The files commit or branch `head` changes since it left master, as
    GitHub's compare lists them (each with its "filename", its "status",
    and a renamed file's "previous_filename"), at most COMPARE_FILE_LIMIT
    of them, or None if it lists none: the compare failed (a diff too big
    to work out, or GitHub having a bad minute). Three dots compare with
    where `head` left master, so what master changed since isn't counted
    as the branch's."""
    try:
        return gh.rest("GET", "repos/%s/compare/master...%s" % (repo, head)).get("files")
    except GhError:
        return None


def record_change(files):
    """Why a branch whose changed_files are `files` is a breaking change by
    the save format record, or "": it adds, changes, removes or renames
    it."""
    if any(f["filename"] == SAVE_FORMAT or f.get("previous_filename") == SAVE_FORMAT
           for f in files or []):
        return "it changes `%s`" % SAVE_FORMAT
    return ""


def has_breaking_section(text):
    """Whether `text` has a "## Breaking change" heading outside code."""
    return re.search(BREAKING_HEADING, prose(text),
                     re.IGNORECASE | re.MULTILINE) is not None


def breaking_reason(config, files, body, said):
    """Why a ticket's PR is a breaking change, or "" if it isn't one: its
    branch's `files` (changed_files) change the save format record, or
    GitHub couldn't list them all, or its `body` or one of the bot's
    replies or reviews among `said` (PrComment) has a "## Breaking change"
    section. The owner's own comments don't count: only the PR's own words
    do."""
    reason = record_change(files)
    if reason:
        return reason
    if files is None or len(files) >= COMPARE_FILE_LIMIT:
        return ("GitHub can't list all of its changed files, so autodev can't "
                "tell whether it changes `%s`" % SAVE_FORMAT)
    if has_breaking_section(body):
        return "its body has a Breaking change section"
    if any(c.author == config.bot and has_breaking_section(c.body) for c in said):
        return "a reply of autodev's on it has a Breaking change section"
    return ""


def needs_merge_breaking(gh, config, head, body, said, answered_id):
    """Why a ticket's PR, at commit `head`, can't merge on comment
    `answered_id` among `said`: the PR is a breaking change (breaking_reason),
    and that comment isn't the owner's exact /merge breaking. "" when it
    can. GitHub is asked only when it matters."""
    answered = next((c for c in said if c.id == answered_id), None)
    if (answered is not None and answered.author == config.owner
            and approves_breaking(answered)):
        return ""
    return breaking_reason(config, changed_files(gh, config.repo, head), body, said)


def _handles(body, comment_id):
    """Whether a reply's handled-through marker names `comment_id`."""
    match = HANDLED_THROUGH.search(body or "")
    return match is not None and int(match.group(1)) == comment_id
