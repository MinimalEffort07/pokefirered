#!/usr/bin/env python3
"""A Claude session's only way to read or change GitHub issues.

  python3 automation/bin/issue.py read <number>          an issue and its comments,
                                                          untrusted authors withheld
  python3 automation/bin/issue.py comment < body.md       comment on your own issue
  python3 automation/bin/issue.py set-body < body.md      replace your issue's body
  python3 automation/bin/issue.py create-ticket --title T < body.md
                                                          a ticket under your issue,
                                                          linked as a sub-issue
  python3 automation/bin/issue.py open-pr --title T < body.md
                                                          your ticket's PR into master,
                                                          from the branch checked out
                                                          in the current folder
  python3 automation/bin/issue.py pr-comment < body.md   reply on your PR, marked as
                                                          handling the /merge you answer
  python3 automation/bin/issue.py merge-pr               merge your PR with a merge
                                                          commit of the commit checked
                                                          out here (it must be pushed
                                                          and hold all of master)

"Your issue" is AUTODEV_ISSUE, which the dispatcher sets for each session.
Grill sessions may comment, set-body and create-ticket; ticket sessions
may comment and open-pr; merge sessions may pr-comment and merge-pr
(AUTODEV_ROLE). A merge session's PR and the comment it answers are
AUTODEV_PR and AUTODEV_HANDLED_THROUGH. Every rule is enforced in
automation/autodev/issue_tool.py.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from autodev import config as config_module, issue_tool  # noqa: E402
from autodev.gh import GhCli, GhError  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("read").add_argument("number", type=int)
    sub.add_parser("comment")
    sub.add_parser("set-body")
    sub.add_parser("create-ticket").add_argument("--title", required=True)
    sub.add_parser("open-pr").add_argument("--title", required=True)
    sub.add_parser("pr-comment")
    sub.add_parser("merge-pr")
    args = parser.parse_args()

    config = config_module.load(os.path.join(os.path.dirname(HERE), "config.json"))
    gh = GhCli()
    try:
        issue_tool.check_role(os.environ.get("AUTODEV_ROLE"), args.command)
        if args.command == "read":
            print(issue_tool.read(gh, config, args.number))
            return 0
        issue = int(os.environ["AUTODEV_ISSUE"])
        if args.command == "merge-pr":
            issue_tool.merge_pr(gh, config, int(os.environ["AUTODEV_PR"]), _git("rev-parse", "HEAD"))
            print("merged")
            return 0
        body = sys.stdin.read()
        if args.command == "comment":
            issue_tool.comment(gh, config, issue, body)
        elif args.command == "set-body":
            issue_tool.set_body(gh, config, issue, body)
        elif args.command == "pr-comment":
            issue_tool.pr_comment(gh, config, int(os.environ["AUTODEV_PR"]),
                                  int(os.environ["AUTODEV_HANDLED_THROUGH"]), body)
        elif args.command == "open-pr":
            created = issue_tool.open_pr(gh, config, issue, args.title,
                                         _git("branch", "--show-current"), body)
            print("#%d %s" % (created["number"], created["html_url"]))
        else:
            print("#%d" % issue_tool.create_ticket(gh, config, issue, args.title, body))
        return 0
    except KeyError as e:
        print("error: %s is not set (only autodev sessions may write)" % e,
              file=sys.stderr)
    except (issue_tool.Refused, GhError) as e:
        print("error: %s" % e, file=sys.stderr)
    return 1


def _git(*args):
    """A git answer about the current folder (the session's worktree)."""
    return subprocess.run(["git", *args], capture_output=True, text=True,
                          check=True).stdout.strip()


if __name__ == "__main__":
    sys.exit(main())
