"""How a Claude session is started, and how its result is read back.

The dispatcher starts each session as its own systemd unit. These tests
pin down the parts of that command that encode a decision from #48: the
bot's identity, the owner's Claude login, the time limit, the role's
permissions, and the session id used to resume it.
"""

import json
import os
import unittest

from autodev import registry, sessions

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
            # The owner's Claude login and plugins (the grilling skill).
            "--setenv=CLAUDE_CONFIG_DIR=/home/min/.claude-personal",
            # The only issue the session may change (automation/bin/issue.py).
            "--setenv=AUTODEV_ISSUE=61",
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

    def test_the_grill_role_reaches_github_only_through_issue_py(self):
        # automation/bin/issue.py withholds untrusted authors' text and only
        # changes the session's own issue; a plain gh allow rule would let
        # the session around both.
        allow = self.rules("grill")["allow"]
        self.assertIn("Bash(python3 automation/bin/issue.py:*)", allow)
        self.assertEqual([r for r in allow if r.startswith("Bash(gh")], [])

if __name__ == "__main__":
    unittest.main()
