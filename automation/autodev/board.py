"""Reading the autodev board from GitHub's GraphQL API.

The board is a user-owned Projects (v2) board. GitHub models it as a
project whose "items" are cards; each card points at its "content" (an
issue, a pull request, or a draft note) and carries field values such as
Status. Only issues from autodev's own repo are kept.
"""

import re
from dataclasses import dataclass

# A card with no Status yet (the owner only just added it) waits for the
# dispatcher exactly like one in Todo, so it is reported as Todo.
DEFAULT_STATUS = "Todo"

# Tickets are recognised by a "## Parent" heading in their body (the
# to-tickets format); every other issue is a parent to be grilled.
_PARENT_HEADING = re.compile(r"^##\s+Parent\s*$", re.MULTILINE)

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


@dataclass(frozen=True)
class Item:
    number: int
    author: str
    status: str
    has_parent: bool


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
        items = data["data"]["user"]["projectV2"]["items"]
        nodes.extend(items["nodes"])
        if not items["pageInfo"]["hasNextPage"]:
            break
        cursor = items["pageInfo"]["endCursor"]
    items = [_parse_item(node) for node in nodes
             if _is_repo_issue(node, config.repo)]
    return Board(number, config.board_title, items, len(nodes) - len(items))


def _is_repo_issue(node, repo):
    content = node["content"] or {}
    return (content.get("__typename") == "Issue"
            and content["repository"]["nameWithOwner"] == repo)


def _parse_item(node):
    content = node["content"]
    status = (node["status"] or {}).get("name") or DEFAULT_STATUS
    return Item(
        number=content["number"],
        author=content["author"]["login"],
        status=status,
        has_parent=bool(_PARENT_HEADING.search(content["body"])),
    )
