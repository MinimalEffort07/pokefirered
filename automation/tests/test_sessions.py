"""How a Claude session is started, and how its result is read back.

The dispatcher starts each session as its own systemd unit. These tests
pin down the parts of that command that encode a decision from #48: the
bot's identity, the owner's Claude login, the time limit, the role's
permissions, and the session id used to resume it.
"""

import json
import os
import unittest

from autodev import sessions

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
            "--unit", "autodev-grill-61",
            # systemd stops the session after 60 minutes (#48, Sessions).
            "-p", "RuntimeMaxSec=3600",
            # GH_TOKEN for gh and git: the bot's, never the owner's.
            "-p", "EnvironmentFile=/home/min/.config/autodev/env",
            "-p", "WorkingDirectory=/home/min/projects/minimaleffort/pokefirered-autodev",
            "-p", "StandardOutput=append:/home/min/autodev/logs/grill-61.jsonl",
            "-p", "StandardError=append:/home/min/autodev/logs/grill-61.err",
            # The owner's Claude login and plugins (the grilling skill).
            "--setenv=CLAUDE_CONFIG_DIR=/home/min/.claude-personal",
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

    def deny_rules(self, role):
        with open(os.path.join(AUTOMATION, "settings", role + ".json")) as f:
            return json.load(f)["permissions"]["deny"]

    def test_D25_the_grill_role_is_read_only_and_cannot_merge_or_push(self):
        deny = self.deny_rules("grill")
        for rule in [
            "Edit", "Write", "NotebookEdit",           # read-only on the repo
            "Bash(git commit:*)", "Bash(git push:*)",  # no commits or pushes
            "Bash(gh pr merge:*)",                     # only the merge role
            "Bash(gh repo edit:*)", "Bash(gh repo delete:*)",
            "Bash(*pret/pokefirered*)",                # never the upstream repo
            "Bash(git remote:*)",
        ]:
            self.assertIn(rule, deny)


if __name__ == "__main__":
    unittest.main()
