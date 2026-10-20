"""Keeping the recordings page on the clone's code (#107).

Each test makes a clone folder holding test/serve_recordings.py with a
modification time it sets, and answers systemctl with a fake.
"""

import io
import os
import subprocess
import tempfile
import unittest

from autodev.config import Config
from autodev.dispatcher import tick
from autodev.gh import GhError
from autodev.recordings import RecordingsServer
from tests.fakes import FakeGh

# When the service started, as systemd printed it on 2026-10-20.
STARTED = 1792374861
SHOW = ["systemctl", "--user", "show", "autodev-recordings.service",
        "-p", "ActiveState", "-p", "ActiveEnterTimestamp", "--timestamp=unix"]
TRY_RESTART = ["systemctl", "--user", "try-restart", "autodev-recordings.service"]


class FakeSystemctl:
    """Stands in for subprocess.run. `show` is what `systemctl show`
    prints; `failing` names the command ("show" or "try-restart") that
    exits 1, and `raises` is what every call raises instead (systemctl
    missing, or hung). Every command is recorded."""

    def __init__(self, show="ActiveState=active\nActiveEnterTimestamp=@%d\n" % STARTED,
                 failing=None, raises=None):
        self.show, self.failing, self.raises = show, failing, raises
        self.calls = []

    def __call__(self, cmd, **kwargs):
        self.calls.append(cmd)
        if self.raises:
            raise self.raises
        if cmd[2] == self.failing:
            # What systemctl says when the user's systemd can't be reached.
            return subprocess.CompletedProcess(
                cmd, 1, "", "Failed to connect to bus: No medium found\n")
        stdout = self.show if cmd[2] == "show" else ""
        return subprocess.CompletedProcess(cmd, 0, stdout, "")


class ServerFile:
    """A clone folder holding an empty test/serve_recordings.py, made fresh
    for each test."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.clone = tmp.name
        os.makedirs(os.path.join(self.clone, "test"))
        self.server = os.path.join(self.clone, "test", "serve_recordings.py")
        open(self.server, "w").close()


class Restarting(ServerFile, unittest.TestCase):
    def check(self, systemctl, modified):
        """Run the check with the server file last modified at `modified`.
        Returns (exit code, the lines it logged)."""
        os.utime(self.server, (modified, modified))
        log = io.StringIO()
        code = RecordingsServer(self.clone, run=systemctl).keep_current(log)
        return code, log.getvalue().splitlines()

    def test_b1_a_file_changed_since_the_start_restarts_the_server(self):
        systemctl = FakeSystemctl()
        code, lines = self.check(systemctl, STARTED + 60)
        self.assertEqual(code, 0)
        self.assertEqual(systemctl.calls, [SHOW, TRY_RESTART])
        self.assertEqual(lines, [
            "restarted the recordings server: "
            "test/serve_recordings.py changed since it started"])

    def test_b2_a_file_unchanged_since_the_start_is_left_alone(self):
        # The service's start time is in whole seconds, so a change within
        # that second can't be told from one just before it: it counts as
        # before. Today's real file: modified 22 s before the start.
        for modified in (STARTED - 22, STARTED, STARTED + 0.9):
            with self.subTest(modified=modified):
                systemctl = FakeSystemctl()
                code, lines = self.check(systemctl, modified)
                self.assertEqual(code, 0)
                self.assertEqual(systemctl.calls, [SHOW])
                self.assertEqual(lines, [])

    def test_b3_a_stopped_server_stays_stopped(self):
        # Stopped by the owner, failed, or never started since boot (systemd
        # then prints an empty start time, as it does for a unit it doesn't
        # know). It runs the new code whenever it next starts.
        for show in ("ActiveState=inactive\nActiveEnterTimestamp=@%d\n" % STARTED,
                     "ActiveState=failed\nActiveEnterTimestamp=@%d\n" % STARTED,
                     "ActiveState=inactive\nActiveEnterTimestamp=\n"):
            with self.subTest(show=show):
                systemctl = FakeSystemctl(show)
                code, lines = self.check(systemctl, STARTED + 60)
                self.assertEqual(code, 0)
                self.assertEqual(systemctl.calls, [SHOW])
                self.assertEqual(lines, [])

    def assert_one_error(self, code, lines):
        self.assertEqual(code, 1)
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith("error: "), lines[0])
        self.assertIn("recordings server", lines[0])

    def test_b4_systemctl_failing_is_one_error_line(self):
        for systemctl in (
                FakeSystemctl(failing="show"),
                FakeSystemctl(raises=FileNotFoundError(
                    2, "No such file or directory", "systemctl")),
                FakeSystemctl(raises=subprocess.TimeoutExpired(SHOW, 30))):
            with self.subTest(failing=systemctl.failing, raises=systemctl.raises):
                code, lines = self.check(systemctl, STARTED + 60)
                self.assert_one_error(code, lines)
                self.assertEqual(systemctl.calls, [SHOW])

    def test_b4_unreadable_output_is_one_error_line(self):
        for show in ("",
                     "garbage\n",
                     "ActiveState=active\n",
                     "ActiveEnterTimestamp=@%d\n" % STARTED,
                     # How it prints without --timestamp=unix.
                     "ActiveState=active\n"
                     "ActiveEnterTimestamp=Tue 2026-10-20 12:54:21 AEDT\n",
                     "ActiveState=inactive\nActiveEnterTimestamp=@soon\n"):
            with self.subTest(show=show):
                systemctl = FakeSystemctl(show)
                code, lines = self.check(systemctl, STARTED + 60)
                self.assert_one_error(code, lines)
                self.assertEqual(systemctl.calls, [SHOW])

    def test_b4_a_failed_restart_is_one_error_line(self):
        systemctl = FakeSystemctl(failing="try-restart")
        code, lines = self.check(systemctl, STARTED + 60)
        self.assert_one_error(code, lines)
        self.assertEqual(systemctl.calls, [SHOW, TRY_RESTART])

    def test_b4_a_missing_server_file_is_one_error_line(self):
        # Only reached while the service runs: it would fail to start again.
        os.remove(self.server)
        systemctl = FakeSystemctl()
        log = io.StringIO()
        code = RecordingsServer(self.clone, run=systemctl).keep_current(log)
        self.assert_one_error(code, log.getvalue().splitlines())
        self.assertEqual(systemctl.calls, [SHOW])


class FakeRegistry:
    """The sessions the tick would start, with none running."""

    def __init__(self):
        self.started = []

    def load(self, now=None):
        return []

    def paused_until(self):
        return 0.0

    def crashes(self):
        return {}

    def requeued(self):
        return set()

    def start(self, role, issue, prompt, pr=None, handled_through=None):
        self.started.append((role, issue))

    def prune(self):
        return 0


class UnreachableGh(FakeGh):
    """GitHub can't be reached: every request fails."""

    def graphql(self, query, **variables):
        raise GhError("error connecting to api.github.com")


class EveryTick(ServerFile, unittest.TestCase):
    """The check runs first on every tick, whatever the board does, and
    the board's work goes on whatever the check does."""

    CONFIG = Config(owner="MinimalEffort07", repo="MinimalEffort07/pokefirered",
                    board_title="pokefirered autodev", bot="MinimalEffort07-Autodev",
                    clone="/nonexistent/clone")

    def tick(self, gh, systemctl):
        """Run a tick with the server file changed since the start. Returns
        (exit code, the lines it logged, the sessions it started)."""
        os.utime(self.server, (STARTED + 60, STARTED + 60))
        log, registry = io.StringIO(), FakeRegistry()
        code = tick(gh, self.CONFIG, log, registry=registry,
                    recordings=RecordingsServer(self.clone, run=systemctl))
        return code, log.getvalue().splitlines(), registry.started

    def test_b4_a_failed_check_still_does_the_boards_work(self):
        # A parent issue in Todo: the tick starts its grill and marks it.
        gh = FakeGh.from_fixture("one_parent_no_status.json")
        code, lines, started = self.tick(gh, FakeSystemctl(failing="show"))
        self.assertEqual(code, 1)
        self.assertEqual(
            [line for line in lines if line.startswith("error: ")],
            ["error: checking the recordings server: "
             "Failed to connect to bus: No medium found"])
        self.assertEqual(started, [("grill", 61)])
        self.assertEqual([m[1] for m in gh.mutations], ["SetStatus"])

    def test_the_restart_line_is_logged_with_the_boards(self):
        gh = FakeGh.from_fixture("one_parent_no_status.json")
        systemctl = FakeSystemctl()
        code, lines, started = self.tick(gh, systemctl)
        self.assertEqual(code, 0)
        self.assertEqual(systemctl.calls, [SHOW, TRY_RESTART])
        self.assertEqual(lines[:3], [
            "restarted the recordings server: "
            "test/serve_recordings.py changed since it started",
            'board #7 "pokefirered autodev": 1 item',
            "#61 MinimalEffort07 Todo has_parent=no"])
        self.assertEqual(started, [("grill", 61)])

    def test_b5_no_board_found_still_runs_the_check(self):
        systemctl = FakeSystemctl()
        code, lines, started = self.tick(FakeGh.from_fixture("no_board.json"),
                                         systemctl)
        self.assertEqual(code, 1)
        self.assertEqual(systemctl.calls, [SHOW, TRY_RESTART])
        self.assertEqual(lines, [
            "restarted the recordings server: "
            "test/serve_recordings.py changed since it started",
            'error: no board titled "pokefirered autodev" owned by MinimalEffort07'])

    def test_b5_a_board_that_cant_be_read_still_runs_the_check(self):
        # The tick gives up on GitHub's error (`python3 -m autodev tick`
        # logs it and exits 1), but only after the check.
        systemctl = FakeSystemctl()
        with self.assertRaises(GhError):
            self.tick(UnreachableGh({}), systemctl)
        self.assertEqual(systemctl.calls, [SHOW, TRY_RESTART])


if __name__ == "__main__":
    unittest.main()
