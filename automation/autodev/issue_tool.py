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
    session its ticket and the PR it opens for it, a merge or fix-up
    session its ticket's PR. Only a merge session merges, and only with a
    merge commit of the commit it tested; a breaking change only on the
    owner's /merge breaking. No session can add labels, close issues, or
    touch the board.
"""

import re

from autodev.board import issue_numbers, section
from autodev.decide import (MARKER, approves_breaking, handled_marker,
                            merge_cancelled, unhandled)
from autodev.github import (breaking, changed_files, is_ticket_pr, pr_timeline,
                            prose, record_change)
from autodev.worktrees import branch_prefix


class Refused(Exception):
    """The request breaks one of the rules above."""


# The commands each role may use. Outside a session the tool only reads.
COMMANDS = {
    "grill": {"read", "comment", "set-body", "create-ticket"},
    "ticket": {"read", "comment", "open-pr"},
    "merge": {"read", "pr-comment", "merge-pr"},
    "fixup": {"read", "pr-comment"},
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
    untrusted author withheld. A PR also shows its reviews and inline
    comments, and marks the owner's comments no bot reply has handled yet
    (#48, "PR comment bookkeeping"): a fix-up session's work."""
    data = gh.rest("GET", "%s/%d" % (_issues(config), number))
    author = data["user"]["login"]
    lines = ["#%d %s [%s]" % (number, data["title"], data["state"]),
             "author: @%s" % author, ""]
    if _trusted(config, author):
        lines.append(data["body"] or "(no body)")
    else:
        lines.append("[body withheld: written by @%s, whom autodev doesn't "
                     "trust]" % author)
    if "pull_request" in data:
        return "\n".join(lines + _said_on_pr(gh, config, number))
    comments = gh.rest_list("%s/%d/comments" % (_issues(config), number))
    shown = [c for c in comments if _trusted(config, c["user"]["login"])]
    lines += _count(config, len(shown), len(comments) - len(shown))
    for c in shown:
        lines += ["", "@%s at %s:" % (c["user"]["login"], c["created_at"]),
                  c["body"] or ""]
    return "\n".join(lines)


def _said_on_pr(gh, config, pr):
    said = pr_timeline(gh, config.repo, pr)
    left = {c.id for c in unhandled(config.owner, config.bot, said)}
    shown = [c for c in said if _trusted(config, c.author)]
    lines = _count(config, len(shown), len(said) - len(shown))
    for c in shown:
        lines += ["", "@%s at %s, %s %d%s%s:" % (
            c.author, c.at, c.kind, c.id, " on " + c.where if c.where else "",
            " [unhandled]" if c.id in left else ""), c.body]
    return lines


def _count(config, shown, withheld):
    return ["", "--- %d comment%s from @%s or @%s%s ---" % (
        shown, "" if shown == 1 else "s", config.owner, config.bot,
        "; %d comment%s by other authors withheld" % (
            withheld, "" if withheld == 1 else "s") if withheld else "")]


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


# GitHub's closing keywords: "close #12", "Fixes owner/repo#12",
# "Resolved: https://github.com/owner/repo/issues/12"...
CLOSING = re.compile(r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?):?\s+"
                     r"(?:(?:[\w.-]+/[\w.-]+)?#|https://github\.com/[\w.-]+/"
                     r"[\w.-]+/issues/)(\d+)", re.IGNORECASE)

def _section_with_text(heading):
    """A PR body's "## <heading>" with some text under it (anything but
    another heading) before the next heading or the end."""
    return re.compile(r"^## %s[ \t]*\n(?:[ \t]*\n)*[ \t]*[^#\s]" % heading,
                      re.IGNORECASE | re.MULTILINE)


WHAT_CHANGES = _section_with_text("What changes for you")
BREAKING_CHANGE = _section_with_text("Breaking change")


def _check_mention(config, body, what):
    # The @mention is how the owner hears about it (#48, "Notifications").
    if not re.search(r"@%s(?![\w-])" % re.escape(config.owner), prose(body)):
        raise Refused("the %s must @mention @%s (not in code)" % (what, config.owner))


def _check_own_pr(gh, config, ticket, pr):
    """Refuse unless `pr` is `ticket`'s open PR (the bot's or the owner's,
    from its own branch: github.is_ticket_pr). The PR number comes from the
    session's environment, which one of its commands could override, so the
    PR itself is checked. Returns GitHub's record of the PR."""
    data = gh.rest("GET", "repos/%s/pulls/%d" % (config.repo, pr))
    if not (data["state"] == "open" and is_ticket_pr(config, ticket, data)):
        raise Refused("PR #%d isn't #%d's open PR from its own %s* branch"
                      % (pr, ticket, branch_prefix(ticket)))
    return data


def open_pr(gh, config, ticket, title, head, body):
    """Open the PR for `ticket` from its own branch into master. Returns
    GitHub's record of the new PR."""
    if not head.startswith(branch_prefix(ticket)):
        raise Refused("a PR for #%d must come from its own %s* branch, not %s"
                      % (ticket, branch_prefix(ticket), head))
    # "Fixes #T" closes the ticket when the PR merges (#57); the @mention
    # tells the owner the PR is ready. Neither counts inside code.
    if not re.search(r"\bFixes #%d\b" % ticket, prose(body)):
        raise Refused("the PR body must say \"Fixes #%d\" (not in code)" % ticket)
    _check_mention(config, body, "PR body")
    # The owner keeps up with what each PR changes for them by reading this
    # section (CLAUDE.md, "System map" and step 11), so it must be a real
    # heading with something under it.
    if not WHAT_CHANGES.search(prose(body)):
        raise Refused("the PR body needs a \"## What changes for you\" section "
                      "(not in code): two or three plain sentences on what is "
                      "different for @%s, or \"Nothing: internal only.\""
                      % config.owner)
    # Merging closes every issue a closing keyword names. Checked in the
    # whole body, code included, to be safe.
    others = sorted({int(n) for n in CLOSING.findall(body)} - {ticket})
    if others:
        raise Refused("the PR body may close only #%d, but a closing keyword "
                      "also names %s; reword it (e.g. \"see #N\")" % (
                          ticket, ", ".join("#%d" % n for n in others)))
    # A branch that changes a record is a breaking change, which the owner
    # approves with /merge breaking after reading this section (CLAUDE.md,
    # "Breaking changes"). The pushed branch is what's compared, with where
    # it left master. A section the records don't call for is fine: some
    # breaks only a session can see.
    reason = record_change(changed_files(gh, config.repo, head) or [])
    if reason and not BREAKING_CHANGE.search(prose(body)):
        raise Refused(
            "this PR is a breaking change (%s; see CLAUDE.md, \"Breaking "
            "changes\"), so the body needs a \"## Breaking change\" section (not "
            "in code) saying what stops working and what happens to a save made "
            "before, why there was no compatible way, what @%s should do, and "
            "\"Comment /merge breaking to approve it.\"" % (reason, config.owner))
    return gh.rest("POST", "repos/%s/pulls" % config.repo, title=title,
                   head=head, base="master", body=body)


def pr_comment(gh, config, ticket, pr, handled_through, body):
    """Reply on `ticket`'s PR, @mentioning the owner: a merge session's
    account of a resolved conflict, or a fix-up's of what it changed. The
    reply ends with a marker naming the newest of the owner's comments the
    session dealt with, so the dispatcher treats only later ones as new
    (#48, "PR comment bookkeeping"). The marker also marks the reply as
    autodev's."""
    _check_mention(config, body, "reply")
    _check_own_pr(gh, config, ticket, pr)
    gh.rest("POST", "%s/%d/comments" % (_issues(config), pr),
            body=body.rstrip() + "\n\n" + handled_marker(handled_through))


def merge_pr(gh, config, ticket, pr, head, handled_through):
    """Merge `ticket`'s PR with a merge commit (never squash or rebase),
    pinned to `head`, the commit the session built and tested, on the
    owner's /merge `handled_through`. A breaking change merges only on
    /merge breaking."""
    data = _check_own_pr(gh, config, ticket, pr)
    said = pr_timeline(gh, config.repo, pr)
    # Feedback after the /merge cancels it (D14), even feedback posted while
    # this session was already running; so does deleting the /merge.
    cancelled = merge_cancelled(config.owner, said, handled_through)
    if cancelled:
        raise Refused("%s, which cancels it: don't merge. Report \"failed\"; "
                      "autodev sends the PR back to review" % cancelled)
    # The merge commit's tree is `head`'s only if the branch already holds
    # all of master. If master has moved on, GitHub would mix in commits
    # nobody tested.
    behind = gh.rest("GET", "repos/%s/compare/%s...master" % (config.repo, head))["ahead_by"]
    if behind:
        raise Refused("master has %d commit%s your branch doesn't: merge master "
                      "in again, re-test, push, and try again" % (
                          behind, "" if behind == 1 else "s"))
    # The dispatcher only starts a merge on a plain /merge for a PR that
    # wasn't a breaking change, but merging master in, or a reply since, may
    # have made it one: checked again, on the very commit being merged.
    reason = breaking(config, changed_files(gh, config.repo, head), data["body"], said)
    answered = next(c for c in said if c.id == handled_through)
    if reason and not (answered.author == config.owner and approves_breaking(answered)):
        raise Refused("this PR is a breaking change (%s), and the /merge it "
                      "answers is a plain /merge: only @%s's /merge breaking "
                      "merges it. Don't merge and don't reply; report failed, and "
                      "autodev sends it back to review and explains." % (
                          reason, config.owner))
    # With `sha`, GitHub refuses if the PR's head is no longer `head`.
    return gh.rest("PUT", "repos/%s/pulls/%d/merge" % (config.repo, pr),
                   merge_method="merge", sha=head)
