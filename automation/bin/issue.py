#!/usr/bin/env python3
"""A Claude session's only way to read or change GitHub issues.

  python3 automation/bin/issue.py read <number>          an issue and its comments,
                                                          untrusted authors withheld
  python3 automation/bin/issue.py comment < body.md       comment on your own issue
  python3 automation/bin/issue.py set-body < body.md      replace your issue's body
  python3 automation/bin/issue.py create-ticket --title T < body.md
                                                          a ticket under your issue,
                                                          linked as a sub-issue

"Your issue" is AUTODEV_ISSUE, which the dispatcher sets for each session.
Every rule is enforced in automation/autodev/issue_tool.py.
"""

import argparse
import os
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
    args = parser.parse_args()

    config = config_module.load(os.path.join(os.path.dirname(HERE), "config.json"))
    gh = GhCli()
    try:
        if args.command == "read":
            print(issue_tool.read(gh, config, args.number))
            return 0
        issue = int(os.environ["AUTODEV_ISSUE"])
        body = sys.stdin.read()
        if args.command == "comment":
            issue_tool.comment(gh, config, issue, body)
        elif args.command == "set-body":
            issue_tool.set_body(gh, config, issue, body)
        else:
            print("#%d" % issue_tool.create_ticket(gh, config, issue, args.title, body))
        return 0
    except KeyError:
        print("error: AUTODEV_ISSUE is not set (only autodev sessions may write)",
              file=sys.stderr)
    except (issue_tool.Refused, GhError) as e:
        print("error: %s" % e, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
