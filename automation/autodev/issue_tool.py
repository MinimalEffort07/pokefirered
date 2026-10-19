"""What a Claude session may see and do on GitHub (automation/bin/issue.py).

A session's role settings deny every plain `gh` command, so this module is
its only door to GitHub. It enforces two rules from #48 ("Identity and
trust") in code, not just in instructions:

  - A session only ever sees text written by the owner or the bot. The
    repo is public, so anyone can comment, and a comment is the easiest
    way to slip instructions into an unattended agent.
  - A session only changes its own issue (AUTODEV_ISSUE, set by the
    dispatcher), with the commands its role needs (AUTODEV_ROLE): a grill
    session its parent and the tickets it creates under it, a ticket
    session its ticket and the PR it opens for it, a merge session its
    ticket's PR. Only a merge session merges, and only with a merge commit
    of the commit it tested. No session can add labels, close issues, or
    touch the board.
"""

import re

from autodev.board import issue_numbers, section
from autodev.decide import MARKER
from autodev.worktrees import branch_prefix


class Refused(Exception):
    """The request breaks one of the rules above."""


# The commands each role may use. Outside a session the tool only reads.
COMMANDS = {
    "grill": {"read", "comment", "set-body", "create-ticket"},
    "ticket": {"read", "comment", "open-pr"},
    "merge": {"read", "pr-comment", "merge-pr"},
}


def check_role(role, command):
    if command not in COMMANDS.get(role, {"read"}):
        raise Refused("a %s session can't use %s" % (role or "non-autodev", command))


def _trusted(config, login):
    return login in (config.owner, config.bot)


def _issues(config):
    return "repos/%s/issues" % config.repo


def read(gh, config, number):
    """The issue (or PR) and its comments, as text, with anything by an
    untrusted author withheld."""
    data = gh.rest("GET", "%s/%d" % (_issues(config), number))
    author = data["user"]["login"]
    lines = ["#%d %s [%s]" % (number, data["title"], data["state"]),
             "author: @%s" % author, ""]
    if _trusted(config, author):
        lines.append(data["body"] or "(no body)")
    else:
        lines.append("[body withheld: written by @%s, whom autodev doesn't "
                     "trust]" % author)
    comments = gh.rest_list("%s/%d/comments" % (_issues(config), number))
    shown = [c for c in comments if _trusted(config, c["user"]["login"])]
    withheld = len(comments) - len(shown)
    lines += ["", "--- %d comment%s from @%s or @%s%s ---" % (
        len(shown), "" if len(shown) == 1 else "s", config.owner, config.bot,
        "; %d comment%s by other authors withheld" % (
            withheld, "" if withheld == 1 else "s") if withheld else "")]
    for c in shown:
        lines += ["", "@%s at %s:" % (c["user"]["login"], c["created_at"]),
                  c["body"] or ""]
    return "\n".join(lines)


def comment(gh, config, issue, body):
    if MARKER not in body:
        body = body.rstrip() + "\n\n" + MARKER
    gh.rest("POST", "%s/%d/comments" % (_issues(config), issue), body=body)


def set_body(gh, config, issue, body):
    gh.rest("PATCH", "%s/%d" % (_issues(config), issue), body=body)


def create_ticket(gh, config, parent, title, body):
    """Create a ticket under `parent` and link it as a native sub-issue.
    Returns the new issue's number."""
    if parent not in issue_numbers(section(body, "Parent")):
        raise Refused("a ticket's \"## Parent\" section must name #%d" % parent)
    created = gh.rest("POST", _issues(config), title=title, body=body)
    # The sub-issues API wants the child's numeric id, not its number.
    gh.rest("POST", "%s/%d/sub_issues" % (_issues(config), parent),
            sub_issue_id=created["id"])
    return created["number"]


# GitHub's closing keywords: "close", "fixes", "Resolved: owner/repo#12"...
CLOSING = re.compile(r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?):?\s+"
                     r"(?:[\w.-]+/[\w.-]+)?#(\d+)", re.IGNORECASE)


def open_pr(gh, config, ticket, title, head, body):
    """Open the PR for `ticket` from its own branch into master. Returns
    GitHub's record of the new PR."""
    if not head.startswith(branch_prefix(ticket)):
        raise Refused("a PR for #%d must come from its own %s* branch, not %s"
                      % (ticket, branch_prefix(ticket), head))
    # "Fixes #T" closes the ticket when the PR merges (#57). The @mention
    # is how the owner hears that the PR is ready (#48, "Notifications").
    # GitHub ignores both inside code, so fenced blocks and `spans` don't
    # count.
    prose = re.sub(r"```.*?```|`[^`]*`", "", body, flags=re.DOTALL)
    if not re.search(r"\bFixes #%d\b" % ticket, prose):
        raise Refused("the PR body must say \"Fixes #%d\" (not in code)" % ticket)
    if not re.search(r"@%s(?![\w-])" % re.escape(config.owner), prose):
        raise Refused("the PR body must @mention @%s (not in code)" % config.owner)
    # Merging closes every issue a closing keyword names. Checked in the
    # whole body, code included, to be safe.
    others = sorted({int(n) for n in CLOSING.findall(body)} - {ticket})
    if others:
        raise Refused("the PR body may close only #%d, but a closing keyword "
                      "also names %s; reword it (e.g. \"see #N\")" % (
                          ticket, ", ".join("#%d" % n for n in others)))
    return gh.rest("POST", "repos/%s/pulls" % config.repo, title=title,
                   head=head, base="master", body=body)


def pr_comment(gh, config, pr, handled_through, body):
    """Reply on the session's PR. The reply ends with a marker naming the
    newest of the owner's comments the session dealt with, so the
    dispatcher treats only later ones as new (#48, "PR comment
    bookkeeping"). The marker also marks the reply as autodev's."""
    marker = "<!-- autodev handled-through: %d -->" % handled_through
    gh.rest("POST", "%s/%d/comments" % (_issues(config), pr),
            body=body.rstrip() + "\n\n" + marker)


def merge_pr(gh, config, pr, head):
    """Merge the PR with a merge commit (never squash or rebase), pinned to
    `head`, the commit the session built and tested."""
    # The merge commit's tree is `head`'s only if the branch already holds
    # all of master. If master has moved on, GitHub would mix in commits
    # nobody tested.
    behind = gh.rest("GET", "repos/%s/compare/%s...master" % (config.repo, head))["ahead_by"]
    if behind:
        raise Refused("master has %d commit%s your branch doesn't: merge master "
                      "in again, re-test, push, and try again" % (
                          behind, "" if behind == 1 else "s"))
    # With `sha`, GitHub refuses if the PR's head is no longer `head`.
    return gh.rest("PUT", "repos/%s/pulls/%d/merge" % (config.repo, pr),
                   merge_method="merge", sha=head)
