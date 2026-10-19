"""How a Claude session is started, and how its result is read back.

The dispatcher starts each session as its own systemd unit. These tests
pin down the parts of that command that encode a decision from #48: the
bot's identity, the owner's Claude login, the time limit, the role's
permissions, and the session id used to resume it.
"""

import json
import os
import tempfile
import unittest

from autodev import registry, sessions
from autodev.decide import Session

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
AUTOMATION = os.path.dirname(os.path.dirname(__file__))

CONFIG = sessions.SessionConfig(
    claude="/home/min/.local/bin/claude",
    claude_config_dir="/home/min/.claude-personal",
    env_file="/home/min/.config/autodev/env",
    clone="/home/min/projects/minimaleffort/pokefirered-autodev",
    log_dir="/home/min/autodev/logs",
    state_dir="/home/min/.local/state/autodev/sessions",
    model="opus",
)


class LaunchCommand(unittest.TestCase):
    def test_a_grill_session_runs_as_the_bot_with_its_limits(self):
        cmd = sessions.launch_command(
            CONFIG, "grill", 61, "0b6f5a2e-0000-4000-8000-000000000061",
            "/home/min/autodev/logs/grill-61.jsonl", "PROMPT")
        self.assertEqual(cmd, [
            "systemd-run", "--user", "--quiet", "--collect",
            # Pass the prompt through untouched: by default systemd-run
            # would expand any $VAR in the command's arguments.
            "--expand-environment=no",
            "--unit", "autodev-grill-61",
            # Type=exec makes systemd-run itself fail if claude can't be
            # started, instead of the session looking like a crash later.
            "-p", "Type=exec",
            # systemd stops the session after 60 minutes (#48, Sessions).
            "-p", "RuntimeMaxSec=3600",
            # GH_TOKEN for gh and git: the bot's, never the owner's.
            "-p", "EnvironmentFile=/home/min/.config/autodev/env",
            "-p", "WorkingDirectory=/home/min/projects/minimaleffort/pokefirered-autodev",
            "-p", "StandardOutput=append:/home/min/autodev/logs/grill-61.jsonl",
            "-p", "StandardError=append:/home/min/autodev/logs/grill-61.err",
            # When the unit ends, for any reason, systemd runs this with
            # SERVICE_RESULT set to how it ended ("timeout" at the limit).
            "-p", "ExecStopPost=:/bin/sh -c 'echo \"$SERVICE_RESULT\" > "
                  "/home/min/autodev/logs/grill-61.exit'",
            # The owner's Claude login and plugins (the grilling skill).
            "--setenv=CLAUDE_CONFIG_DIR=/home/min/.claude-personal",
            # The only issue the session may change, and which of
            # automation/bin/issue.py's commands it may use.
            "--setenv=AUTODEV_ISSUE=61",
            "--setenv=AUTODEV_ROLE=grill",
            "--",
            "/home/min/.local/bin/claude", "-p", "PROMPT",
            "--permission-mode", "auto", "--permission-prompts", "none",
            "--settings", "/home/min/projects/minimaleffort/pokefirered-autodev"
                          "/automation/settings/grill.json",
            "--output-format", "stream-json", "--verbose",
            "--json-schema", sessions.RESULT_SCHEMA,
            "--session-id", "0b6f5a2e-0000-4000-8000-000000000061",
            "--model", "opus",
        ])

    def test_a_ticket_session_runs_in_its_worktree_for_up_to_3_hours(self):
        cmd = sessions.launch_command(
            CONFIG, "ticket", 68, "sid-68",
            "/home/min/autodev/logs/ticket-68.jsonl", "PROMPT")
        self.assertEqual(cmd, [
            "systemd-run", "--user", "--quiet", "--collect",
            "--expand-environment=no",
            "--unit", "autodev-ticket-68",
            "-p", "Type=exec",
            "-p", "RuntimeMaxSec=10800",
            "-p", "EnvironmentFile=/home/min/.config/autodev/env",
            # The ticket's own worktree, made by the dispatcher just before.
            "-p", "WorkingDirectory=/home/min/projects/minimaleffort/pokefirered-autodev"
                  "/.worktrees/issue-68",
            "-p", "StandardOutput=append:/home/min/autodev/logs/ticket-68.jsonl",
            "-p", "StandardError=append:/home/min/autodev/logs/ticket-68.err",
            "-p", "ExecStopPost=:/bin/sh -c 'echo \"$SERVICE_RESULT\" > "
                  "/home/min/autodev/logs/ticket-68.exit'",
            "--setenv=CLAUDE_CONFIG_DIR=/home/min/.claude-personal",
            "--setenv=AUTODEV_ISSUE=68",
            "--setenv=AUTODEV_ROLE=ticket",
            "--",
            "/home/min/.local/bin/claude", "-p", "PROMPT",
            "--permission-mode", "auto", "--permission-prompts", "none",
            # The clone's copy, on master: the worktree's copy is on the
            # ticket's branch, where the session itself could change it.
            "--settings", "/home/min/projects/minimaleffort/pokefirered-autodev"
                          "/automation/settings/ticket.json",
            "--output-format", "stream-json", "--verbose",
            "--json-schema", sessions.RESULT_SCHEMA,
            "--session-id", "sid-68",
            "--model", "opus",
        ])

    def test_the_result_schema_asks_for_an_outcome_and_a_reason(self):
        schema = json.loads(sessions.RESULT_SCHEMA)
        self.assertEqual(schema["properties"]["outcome"]["enum"],
                         ["done", "failed"])
        self.assertEqual(schema["required"], ["outcome", "reason", "links"])


class RunningOrNot(unittest.TestCase):
    """Whether a session's unit is still under way, from systemd's
    ActiveState. A unit systemd has forgotten (--collect) is inactive."""

    def registry_seeing(self, active_state):
        class Done:
            stdout = active_state + "\n"
            returncode = 0
        return registry.Registry(CONFIG, run=lambda *a, **k: Done())

    def test_a_starting_or_stopping_unit_still_counts_as_running(self):
        for active_state in ("active", "activating", "deactivating", "reloading"):
            with self.subTest(active_state=active_state):
                self.assertTrue(self.registry_seeing(active_state)
                                .is_running("autodev-grill-61"))

    def test_an_ended_or_forgotten_unit_is_not_running(self):
        for active_state in ("inactive", "failed"):
            with self.subTest(active_state=active_state):
                self.assertFalse(self.registry_seeing(active_state)
                                 .is_running("autodev-grill-61"))


class EndedSessions(unittest.TestCase):
    def test_how_a_session_ended_and_where_it_ran_are_read_back(self):
        with tempfile.TemporaryDirectory() as root:
            config = sessions.SessionConfig(
                claude="", claude_config_dir="", env_file="", clone="/clone",
                log_dir=os.path.join(root, "logs"),
                state_dir=os.path.join(root, "state"), model="")
            os.makedirs(config.log_dir)
            os.makedirs(config.state_dir)
            log = os.path.join(config.log_dir, "ticket-68-20261019-150000.jsonl")
            with open(os.path.join(config.state_dir, "ticket-68.json"), "w") as f:
                json.dump({"role": "ticket", "issue": 68, "session_id": "sid-68",
                           "log": log, "started": "20261019-150000"}, f)
            # Written by the unit's ExecStopPost (see launch_command).
            with open(log[:-len(".jsonl")] + ".exit", "w") as f:
                f.write("timeout\n")

            class Ended:
                stdout = "inactive\n"
            found = registry.Registry(config, run=lambda *a, **k: Ended()).load()
            self.assertEqual(found, [Session(
                "ticket", 68, "sid-68", running=False, result=None,
                ended_by="timeout", cwd="/clone/.worktrees/issue-68")])


class ReadingResults(unittest.TestCase):
    def test_the_structured_result_is_read_from_the_log(self):
        # Recorded from a real `claude -p --json-schema` run.
        self.assertEqual(
            sessions.read_result(os.path.join(FIXTURES, "session_done.jsonl")),
            {"outcome": "done", "reason": "format probe"})

    def test_a_log_with_no_result_event_has_no_result(self):
        # The session was killed (time limit) or crashed before finishing.
        path = os.path.join(FIXTURES, "session_done.jsonl")
        with open(path) as f:
            cut = [line for line in f if '"type": "result"' not in line]
        tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"),
                           "autodev-test-cut.jsonl")
        with open(tmp, "w") as f:
            f.writelines(cut)
        try:
            self.assertIsNone(sessions.read_result(tmp))
        finally:
            os.remove(tmp)

    def test_a_missing_log_has_no_result(self):
        self.assertIsNone(sessions.read_result("/nonexistent/grill-61.jsonl"))


class RolePermissions(unittest.TestCase):
    """D25: each role's settings file carries its deny rules."""

    def rules(self, role):
        with open(os.path.join(AUTOMATION, "settings", role + ".json")) as f:
            return json.load(f)["permissions"]

    def test_D25_the_grill_role_is_read_only_and_cannot_merge_or_push(self):
        deny = self.rules("grill")["deny"]
        for rule in [
            "Edit", "Write", "NotebookEdit",           # read-only on the repo
            "Bash(gh:*)",                              # no direct GitHub access
            "Bash(git commit:*)", "Bash(git push:*)",  # no commits or pushes
            "Bash(git checkout:*)", "Bash(git reset:*)", "Bash(git clean:*)",
            "Bash(*pret/pokefirered*)",                # never the upstream repo
            "Bash(git remote:*)",
            "WebFetch", "WebSearch",                   # no untrusted web text
        ]:
            self.assertIn(rule, deny)

    def test_D25_ticket_and_fixup_roles_cannot_merge_or_touch_master(self):
        # In auto mode an allow rule only skips the safety classifier; it
        # never forbids anything. So these limits are all deny rules, and
        # deny beats allow. The master ruleset on GitHub is the backstop.
        for role in ("ticket", "fixup"):
            deny = self.rules(role)["deny"]
            for rule in [
                "Bash(gh:*)",                   # incl. gh pr merge; use issue.py
                "Bash(curl:*)", "Bash(wget:*)", "WebFetch", "WebSearch",
                # master: `git push origin master`, `... HEAD:master`
                "Bash(git push* master*)", "Bash(git push*:master*)",
                # force: --force(-with-lease), -f, a "+branch" refspec
                "Bash(git push*--force*)", "Bash(git push* -f*)",
                "Bash(git push*+*)",
                # deleting a branch: --delete, -d, an empty ":branch" source
                "Bash(git push*--delete*)", "Bash(git push* -d*)",
                "Bash(git push* :*)",
                "Bash(git push*--mirror*)", "Bash(git push*--all*)",
                "Bash(git remote:*)", "Bash(git config:*)",
                "Bash(*pret/pokefirered*)",     # never the upstream repo
                # rm -rf only inside its own worktree
                "Bash(rm -rf /*)", "Bash(rm -rf ~*)", "Bash(rm -rf ..*)",
                "Bash(rm -rf $HOME*)",
            ]:
                with self.subTest(role=role, rule=rule):
                    self.assertIn(rule, deny)

    def test_ticket_and_fixup_roles_reach_github_only_through_issue_py(self):
        # They run the clone's copy by its full path (prompts/ticket.md):
        # the worktree's copy is on the ticket's branch, which the session
        # can change and which may predate the dispatcher's.
        for role in ("ticket", "fixup"):
            with self.subTest(role=role):
                allow = self.rules(role)["allow"]
                self.assertIn("Bash(python3 *automation/bin/issue.py *)", allow)
                self.assertEqual([r for r in allow if r.startswith("Bash(gh")], [])

    def test_the_grill_role_reaches_github_only_through_issue_py(self):
        # automation/bin/issue.py withholds untrusted authors' text and only
        # changes the session's own issue; a plain gh allow rule would let
        # the session around both.
        allow = self.rules("grill")["allow"]
        self.assertIn("Bash(python3 automation/bin/issue.py:*)", allow)
        self.assertEqual([r for r in allow if r.startswith("Bash(gh")], [])

if __name__ == "__main__":
    unittest.main()
