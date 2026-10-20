"""Carrying out decide()'s actions on GitHub, and reading what a finished
session left there.

Everything here runs as the bot: the dispatcher's unit loads the bot's
GH_TOKEN, and gh uses it for every call.
"""

from autodev.board import issue_numbers, section
from autodev.decide import (HANDLED_THROUGH, MARKER, PARKED, FixupOutcome,
                            GrillOutcome, MergeOutcome, PrComment, PullRequest,
                            Ticket, TicketOutcome, merge_cancelled)
from autodev.worktrees import branch_prefix

# The grill session marks its design comment with this hidden tag
# (CLAUDE.md, "Autonomous mode"), which is how the dispatcher finds it.
DESIGN_MARKER = "<!-- autodev:design -->"

SET_STATUS = """
mutation SetStatus($board: ID!, $item: ID!, $field: ID!, $option: String!) {
  updateProjectV2ItemFieldValue(input: {projectId: $board, itemId: $item,
      fieldId: $field, value: {singleSelectOptionId: $option}}) {
    clientMutationId
  }
}
"""

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
        its tickets: its "## Parent" section names `parent`, as on the
        board. A sub-issue the owner linked by hand isn't one."""
        subs = self.gh.rest_list("repos/%s/issues/%d/sub_issues"
                                 % (self.config.repo, parent))
        return {s["number"]: s["state"] == "open" for s in subs
                if parent in issue_numbers(section(s["body"], "Parent"))}

    # ── actions ───────────────────────────────────────────────────────────

    def set_status(self, issue, status):
        self._set_item_status(self.items[issue].item_id, status)

    def _set_item_status(self, item_id, status):
        self.gh.graphql(SET_STATUS, board=self.board.id, item=item_id,
                        field=self.board.status_field_id,
                        option=self.board.status_options[status])

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
        design = any(c["user"]["login"] == self.config.bot
                     and DESIGN_MARKER in (c["body"] or "") for c in comments)
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
        return GrillOutcome(design_comment=design, tickets=tickets)

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
        (unless `comments` is False), or None."""
        pr = self._find_ticket_pr(number)
        if pr is None:
            return None
        return PullRequest(
            number=pr["number"], head=pr["head"]["ref"], opened=pr["created_at"],
            comments=pr_timeline(self.gh, self.config.repo, pr["number"])
            if comments else ())

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
        the /merge comment the merge session answered, and whether the
        owner has withdrawn that /merge since."""
        repo = self.config.repo
        merged = self.gh.rest("GET", "repos/%s/pulls/%d" % (repo, pr))["merged"]
        said = pr_timeline(self.gh, repo, pr)
        return MergeOutcome(
            merged=merged, replied=self._replied(said, handled_through),
            superseded=merge_cancelled(self.config.owner, said,
                                       handled_through) is not None)

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


def _handles(body, comment_id):
    """Whether a reply's handled-through marker names `comment_id`."""
    match = HANDLED_THROUGH.search(body or "")
    return match is not None and int(match.group(1)) == comment_id
