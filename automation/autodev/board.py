"""Reading the autodev board from GitHub's GraphQL API.

The board is a user-owned Projects (v2) board. GitHub models it as a
project whose "items" are cards; each card points at its "content" (an
issue, a pull request, or a draft note) and carries field values such as
Status. Only issues from autodev's own repo are kept.
"""

import re
from dataclasses import dataclass, field
from typing import Optional

# A card with no Status yet (the owner only just added it) waits for the
# dispatcher exactly like one in Todo, so it is reported as Todo.
DEFAULT_STATUS = "Todo"



def section(body, heading):
    """The text under a "## <heading>" line, up to the next "## " heading,
    or None if the body has no such heading. The heading may carry text on
    the same line ("## Parent #48"), but "## Parenting" is a different one.
    """
    match = re.search(r"^##\s+%s\b(.*?)(?=^##\s|\Z)" % re.escape(heading),
                      body or "", re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def issue_numbers(text):
    """Every "#123" reference in the text, as numbers."""
    return [int(n) for n in re.findall(r"#(\d+)\b", text or "")]


# GitHub reports no author for an issue whose account was deleted, and
# shows such issues as written by "ghost".
DELETED_AUTHOR = "ghost"

FIND_BOARD = """
query FindBoard($owner: String!, $title: String!) {
  user(login: $owner) {
    projectsV2(first: 20, query: $title) { nodes { number title } }
  }
}
"""

BOARD_ITEMS = """
query BoardItems($owner: String!, $number: Int!, $cursor: String) {
  user(login: $owner) {
    projectV2(number: $number) {
      id
      field(name: "Status") {
        ... on ProjectV2SingleSelectField { id options { id name } }
      }
      items(first: 100, after: $cursor) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id
          type
          status: fieldValueByName(name: "Status") {
            ... on ProjectV2ItemFieldSingleSelectValue { name }
          }
          content {
            __typename
            ... on Issue {
              number title body state
              author { login }
              repository { nameWithOwner }
            }
          }
        }
      }
    }
  }
}
"""


@dataclass(frozen=True)
class Board:
    number: int
    title: str
    items: list         # Item for each issue from autodev's repo
    skipped: int        # other cards: drafts, pull requests, other repos' issues
    # GraphQL ids needed to change a card's Status: the board's own id, its
    # Status field's id, and each Status option's id by name.
    id: str = ""
    status_field_id: str = ""
    status_options: dict = field(default_factory=dict)


@dataclass(frozen=True)
class Item:
    number: int
    author: str
    status: str
    has_parent: bool
    item_id: Optional[str] = None   # the card's id on the board
    title: str = ""
    parent: Optional[int] = None    # the issue its "## Parent" section names
    blocked_by: tuple = ()          # the issues its "## Blocked by" lists
    is_open: bool = True


def find_board(gh, config):
    """Return the number of the board titled config.board_title, or None.

    GitHub's `query` filter is a fuzzy search, so the exact title is
    checked here."""
    data = gh.graphql(FIND_BOARD, owner=config.owner, title=config.board_title)
    for node in data["data"]["user"]["projectsV2"]["nodes"]:
        if node["title"] == config.board_title:
            return node["number"]
    return None


def read_board(gh, config, number):
    nodes = []
    cursor = None
    while True:
        data = gh.graphql(BOARD_ITEMS, owner=config.owner, number=number,
                          cursor=cursor)
        project = data["data"]["user"]["projectV2"]
        page = project["items"]
        nodes.extend(page["nodes"])
        if not page["pageInfo"]["hasNextPage"]:
            break
        cursor = page["pageInfo"]["endCursor"]
    items = [_parse_item(node) for node in nodes
             if _is_repo_issue(node, config.repo)]
    status = project["field"]
    return Board(number, config.board_title, items, len(nodes) - len(items),
                 id=project["id"], status_field_id=status["id"],
                 status_options={o["name"]: o["id"] for o in status["options"]})


def _is_repo_issue(node, repo):
    content = node["content"] or {}
    return (content.get("__typename") == "Issue"
            and content["repository"]["nameWithOwner"] == repo)


def _parse_item(node):
    content = node["content"]
    status = (node["status"] or {}).get("name") or DEFAULT_STATUS
    parent = section(content["body"], "Parent")
    return Item(
        number=content["number"],
        author=(content["author"] or {}).get("login", DELETED_AUTHOR),
        status=status,
        # Tickets carry a "## Parent" section (the to-tickets format);
        # every other issue is a parent to be grilled.
        has_parent=parent is not None,
        item_id=node["id"],
        title=content["title"],
        parent=next(iter(issue_numbers(parent)), None),
        blocked_by=tuple(issue_numbers(section(content["body"], "Blocked by"))),
        is_open=content["state"] == "OPEN",
    )
