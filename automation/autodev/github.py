"""Carrying out decide()'s actions on GitHub, and reading what a finished
grill session left there.

Everything here runs as the bot: the dispatcher's unit loads the bot's
GH_TOKEN, and gh uses it for every call.
"""

import re

from autodev.decide import GrillOutcome, Ticket

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


def section(body, heading):
    """The text under a "## <heading>" line, up to the next "## " heading."""
    match = re.search(r"^##\s+%s\b(.*?)(?=^##\s|\Z)" % re.escape(heading),
                      body or "", re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def issue_numbers(text):
    return [int(n) for n in re.findall(r"#(\d+)\b", text or "")]


class GitHub:
    def __init__(self, gh, config, board):
        self.gh = gh
        self.config = config
        self.board = board
        self.items = {item.number: item for item in board.items}

    def _issue(self, number):
        return self.gh.rest("GET", "repos/%s/issues/%d" % (self.config.repo, number))

    # ── actions ───────────────────────────────────────────────────────────

    def set_status(self, issue, status):
        self._set_item_status(self.items[issue].item_id, status)

    def _set_item_status(self, item_id, status):
        self.gh.graphql(SET_STATUS, board=self.board.id, item=item_id,
                        field=self.board.status_field_id,
                        option=self.board.status_options[status])

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
        comments = self.gh.rest("GET", "repos/%s/issues/%d/comments?per_page=100"
                                % (repo, parent)) or []
        design = any(c["user"]["login"] == self.config.bot
                     and DESIGN_MARKER in (c["body"] or "") for c in comments)
        subs = self.gh.rest("GET", "repos/%s/issues/%d/sub_issues?per_page=100"
                            % (repo, parent)) or []
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
