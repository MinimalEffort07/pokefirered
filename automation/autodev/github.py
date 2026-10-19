"""Carrying out decide()'s actions on GitHub, and reading what a finished
session left there.

Everything here runs as the bot: the dispatcher's unit loads the bot's
GH_TOKEN, and gh uses it for every call.
"""

from autodev.board import issue_numbers, section
from autodev.decide import (HANDLED_THROUGH, GrillOutcome, MergeOutcome,
                            PrComment, PullRequest, Ticket, TicketOutcome)
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
        """The bot's open PR for ticket `number` as GitHub lists it, or None.
        Anyone can open a PR from a fork with any branch name, so only a
        branch in this repo, pushed and opened by the bot, counts."""
        prefix = branch_prefix(number)
        if self._open_pulls is None:
            self._open_pulls = self.gh.rest_list(
                "repos/%s/pulls?state=open" % self.config.repo)
        for pr in self._open_pulls:
            head = pr["head"]
            if (head["ref"].startswith(prefix)
                    and (head["repo"] or {}).get("full_name") == self.config.repo
                    and pr["user"]["login"] == self.config.bot):
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
        return PullRequest(number=pr["number"], head=pr["head"]["ref"],
                           comments=self._timeline(pr["number"]) if comments else ())

    def _timeline(self, pr):
        """Everything said on a PR, oldest first: comments in its
        conversation, reviews, and inline review comments. GitHub keeps
        the three apart, and a review is dated by when it was submitted."""
        repo = self.config.repo
        said = [PrComment(c["id"], c["user"]["login"], c["body"] or "", c["created_at"])
                for path in ("repos/%s/issues/%d/comments" % (repo, pr),
                             "repos/%s/pulls/%d/comments" % (repo, pr))
                for c in self.gh.rest_list(path)]
        # An approval with nothing written says nothing to act on.
        said += [PrComment(r["id"], r["user"]["login"], r["body"], r["submitted_at"])
                 for r in self.gh.rest_list("repos/%s/pulls/%d/reviews" % (repo, pr))
                 if r["body"]]
        return tuple(sorted(said, key=lambda c: c.at))

    def merge_outcome(self, pr, handled_through):
        """Whether the PR is merged, and whether the bot replied on it
        naming the /merge comment the merge session answered."""
        repo = self.config.repo
        merged = self.gh.rest("GET", "repos/%s/pulls/%d" % (repo, pr))["merged"]
        replied = any(
            c["user"]["login"] == self.config.bot
            and _handles(c["body"], handled_through)
            for c in self.gh.rest_list("repos/%s/issues/%d/comments" % (repo, pr)))
        return MergeOutcome(merged=merged, replied=replied)


def _handles(body, comment_id):
    """Whether a reply's handled-through marker names `comment_id`."""
    match = HANDLED_THROUGH.search(body or "")
    return match is not None and int(match.group(1)) == comment_id
