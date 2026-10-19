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
                  "-worktrees/issue-68",
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

    def test_a_merge_session_knows_its_pr_and_the_comment_it_answers(self):
        cmd = sessions.launch_command(
            CONFIG, "merge", 68, "sid-m68",
            "/home/min/autodev/logs/merge-68.jsonl", "PROMPT",
            pr=73, handled_through=501)
        self.assertEqual(cmd[:cmd.index("--")], [
            "systemd-run", "--user", "--quiet", "--collect",
            "--expand-environment=no",
            "--unit", "autodev-merge-68",
            "-p", "Type=exec",
            "-p", "RuntimeMaxSec=2700",                 # 45 minutes
            "-p", "EnvironmentFile=/home/min/.config/autodev/env",
            # The ticket's worktree, which holds its branch.
            "-p", "WorkingDirectory=/home/min/projects/minimaleffort/pokefirered-autodev"
                  "-worktrees/issue-68",
            "-p", "StandardOutput=append:/home/min/autodev/logs/merge-68.jsonl",
            "-p", "StandardError=append:/home/min/autodev/logs/merge-68.err",
            "-p", "ExecStopPost=:/bin/sh -c 'echo \"$SERVICE_RESULT\" > "
                  "/home/min/autodev/logs/merge-68.exit'",
            "--setenv=CLAUDE_CONFIG_DIR=/home/min/.claude-personal",
            "--setenv=AUTODEV_ISSUE=68",
            "--setenv=AUTODEV_ROLE=merge",
            # issue.py's pr-comment and merge-pr act on this PR only, and
            # the reply names the /merge it answers.
            "--setenv=AUTODEV_PR=73",
            "--setenv=AUTODEV_HANDLED_THROUGH=501",
        ])
        self.assertIn("/home/min/projects/minimaleffort/pokefirered-autodev"
                      "/automation/settings/merge.json", cmd)

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
                ended_by="timeout", cwd="/clone-worktrees/issue-68")])


    def test_a_merge_sessions_pr_and_comment_are_kept_until_it_ends(self):
        # The dispatcher checks the merge session's work against them: is
        # that PR merged, or did the bot answer that /merge?
        with tempfile.TemporaryDirectory() as root:
            config = sessions.SessionConfig(
                claude="claude", claude_config_dir="", env_file="", clone="/clone",
                log_dir=os.path.join(root, "logs"),
                state_dir=os.path.join(root, "state"), model="opus")
            launched = []

            class Unit:
                stdout = "inactive\n"

            def run(cmd, **kwargs):
                launched.append(cmd)
                return Unit()
            reg = registry.Registry(config, run=run)
            reg.start("merge", 68, "PROMPT", pr=73, handled_through=501)
            self.assertIn("--setenv=AUTODEV_PR=73", launched[0])
            [found] = reg.load()
            self.assertEqual((found.role, found.issue, found.pr, found.handled_through),
                             ("merge", 68, 73, 501))


class Remembering(unittest.TestCase):
    """What the registry keeps between ticks for D18-D20."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.config = sessions.SessionConfig(
            claude="claude", claude_config_dir="", env_file="", clone="/clone",
            log_dir=os.path.join(tmp.name, "logs"),
            state_dir=os.path.join(tmp.name, "state"), model="opus")

        class Ended:
            stdout = "inactive\n"
        self.registry = registry.Registry(self.config, run=lambda *a, **k: Ended())

    def test_crashes_are_counted_per_role_and_issue(self):
        for role, crashed in (("ticket", True), ("ticket", False), ("merge", True),
                              ("ticket", True)):
            self.registry.start(role, 68, "PROMPT")
            self.registry.finish(role, 68, crashed=crashed)
        self.assertEqual(self.registry.crashes(), {("ticket", 68): 2, ("merge", 68): 1})

    def test_a_pause_is_kept_on_disk_and_only_ever_lengthened(self):
        self.assertEqual(self.registry.paused_until(), 0.0)
        self.registry.pause(1_800_003_600.0)
        self.registry.pause(1_800_000_000.0)    # a second limit, resetting sooner
        self.assertEqual(self.registry.paused_until(), 1_800_003_600.0)

    def test_a_session_that_stopped_on_the_usage_limit_says_when_it_resets(self):
        self.registry.start("ticket", 68, "PROMPT")
        [record] = os.listdir(self.config.state_dir)
        with open(os.path.join(self.config.state_dir, record)) as f:
            log = json.load(f)["log"]
        with open(log, "w") as f:
            f.write(json.dumps({"type": "result", "is_error": True, "result":
                                "Claude AI usage limit reached|1800009000"}) + "\n")
        [found] = self.registry.load(now=1_800_000_000.0)
        self.assertEqual((found.limit_reset, found.limit_note),
                         (1_800_009_000.0, "Claude AI usage limit reached|1800009000"))


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

    def test_the_last_report_counts_even_if_the_session_woke_again_after(self):
        # Seen live on #73's merge: after reporting, a background job's
        # notification woke the session once more, and that turn ended with
        # a result event carrying no report. An earlier "failed" sent while
        # a command was still running is overridden by the later "done".
        events = [
            {"type": "result", "subtype": "success", "is_error": False,
             "structured_output": {"outcome": "failed", "reason": "too early"}},
            {"type": "result", "subtype": "success", "is_error": False,
             "structured_output": {"outcome": "done", "reason": "merged"}},
            {"type": "result", "subtype": "success", "is_error": False,
             "structured_output": None},
        ]
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("\n".join(json.dumps(e) for e in events) + "\n")
        try:
            self.assertEqual(sessions.read_result(f.name),
                             {"outcome": "done", "reason": "merged"})
        finally:
            os.remove(f.name)

    def test_a_missing_log_has_no_result(self):
        self.assertIsNone(sessions.read_result("/nonexistent/grill-61.jsonl"))


class UsageLimits(unittest.TestCase):
    """D18: reading when the Claude usage limit resets from what the CLI
    printed. The wording isn't documented, so several forms are accepted."""

    SYDNEY = "Australia/Sydney"

    def at(self, text, zone="UTC"):
        """`text` as a time in `zone`, as seconds since 1970."""
        import datetime, zoneinfo
        return datetime.datetime.strptime(text, "%Y-%m-%d %H:%M").replace(
            tzinfo=zoneinfo.ZoneInfo(zone)).timestamp()

    def test_the_reset_time_is_read_in_its_own_zone_or_the_machines(self):
        now = self.at("2026-10-19 14:10", self.SYDNEY)
        for text, zone, expected in [
            ("You've hit your limit \u00b7 resets 3pm (Australia/Sydney)", "UTC",
             self.at("2026-10-19 15:00", self.SYDNEY)),
            # A time already past today means tomorrow.
            ("You've hit your 5-hour limit \u00b7 resets 10:30am", self.SYDNEY,
             self.at("2026-10-20 10:30", self.SYDNEY)),
            ("Weekly usage limit reached \u2219 resets Oct 23, 9am (UTC)", self.SYDNEY,
             self.at("2026-10-23 09:00")),
            # Older CLIs printed the reset as seconds since 1970.
            ("Claude AI usage limit reached|1792400000", self.SYDNEY, 1792400000.0),
        ]:
            with self.subTest(text=text):
                self.assertEqual(sessions.usage_limit(text, now, zone), expected)

    def test_a_limit_without_a_readable_time_pauses_for_an_hour(self):
        now = 1_800_000_000.0
        self.assertEqual(sessions.usage_limit("You've hit your usage limit.", now),
                         now + 3600)

    def test_other_failures_are_not_usage_limits(self):
        for text in ("Error: the 45-minute time limit was hit",
                     "API Error: 500 Internal Server Error", ""):
            with self.subTest(text=text):
                self.assertIsNone(sessions.usage_limit(text, 1_800_000_000.0))

    def test_the_message_is_looked_for_in_the_final_result_and_stderr(self):
        with tempfile.TemporaryDirectory() as root:
            log = os.path.join(root, "ticket-68.jsonl")
            with open(log, "w") as f:
                f.write(json.dumps({"type": "assistant", "message": {"content": [
                    {"type": "text", "text": "This ticket is about usage limits."}]}}) + "\n")
                f.write(json.dumps({"type": "result", "is_error": True,
                                    "result": "You've hit your limit \u00b7 resets 3pm"}) + "\n")
            with open(os.path.join(root, "ticket-68.err"), "w") as f:
                f.write("some warning\n")
            self.assertEqual(sessions.limit_text(log).strip(),
                             "You've hit your limit \u00b7 resets 3pm\nsome warning")


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
            "Bash(git* upstream*)",                    # or its remote
            "Bash(git remote:*)",
            "WebFetch", "WebSearch",                   # no untrusted web text
            # issue.py trusts AUTODEV_ISSUE and AUTODEV_ROLE: no overriding
            # them for one command
            "Bash(*AUTODEV_*)",
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
                # master: `git push origin master`, `... HEAD:master`,
                # `... HEAD:refs/heads/master`
                "Bash(git push* master*)", "Bash(git push*:master*)",
                "Bash(git push*/master*)",
                # `git -C . push --force` would slip past every rule above
                "Bash(git -C*)", "Bash(git -c*)",
                # force: --force(-with-lease), -f, a "+branch" refspec
                "Bash(git push*--force*)", "Bash(git push* -f*)",
                "Bash(git push*+*)",
                # deleting a branch: --delete, -d, an empty ":branch" source
                "Bash(git push*--delete*)", "Bash(git push* -d*)",
                "Bash(git push* :*)",
                "Bash(git push*--mirror*)", "Bash(git push*--all*)",
                "Bash(git remote:*)", "Bash(git config:*)",
                "Bash(*pret/pokefirered*)",     # never the upstream repo
                "Bash(git* upstream*)",         # or its remote
                # The worktree's issue.py is the ticket branch's, which the
                # session can edit: only the clone's copy guards GitHub.
                "Bash(python3 automation/bin/issue.py*)",
                "Bash(python3 ./automation/bin/issue.py*)",
                "Bash(*python3 bin/issue.py*)",
                "Bash(*-worktrees/*issue.py*)",
                # issue.py trusts the AUTODEV_* variables the dispatcher sets
                "Bash(*AUTODEV_*)",
                # rm -rf only inside its own worktree
                "Bash(rm -rf /*)", "Bash(rm -rf ~*)", "Bash(rm -rf ..*)",
                "Bash(rm -rf $HOME*)",
            ]:
                with self.subTest(role=role, rule=rule):
                    self.assertIn(rule, deny)

    def test_D25_the_merge_role_has_the_ticket_roles_limits_and_no_rebase(self):
        # It merges only through issue.py's merge-pr: a merge commit of the
        # commit it tested. gh stays denied, so no `gh pr merge --squash`.
        merge = set(self.rules("merge")["deny"])
        self.assertLessEqual(set(self.rules("ticket")["deny"]), merge)
        self.assertIn("Bash(git rebase:*)", merge)
        self.assertIn("Bash(python3 *automation/bin/issue.py *)",
                      self.rules("merge")["allow"])

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
