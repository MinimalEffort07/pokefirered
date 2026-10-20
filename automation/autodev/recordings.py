"""Keeping the recordings page on the clone's code (#107).

`autodev-recordings.service` runs `test/serve_recordings.py` from
autodev's clone and keeps running. Every tick pulls the clone, but a
running program keeps the code it started with: Python reads the file
once, at start. So once a merge changes the server, the page only shows
the change after the service restarts.

Each tick asks systemd when the service started and compares that with
when the clone's copy of the file last changed. A file changed since the
start means the server is running old code, and the tick restarts it.
The server is one file that imports only Python's standard library
(#103 keeps it that way), so that file is all there is to watch.
"""

import os
import re
import subprocess

UNIT = "autodev-recordings.service"
SERVER = "test/serve_recordings.py"     # from the clone's top folder

# A tick runs every 2 minutes, and systemctl answers in milliseconds; one
# that hangs (a stuck user systemd) mustn't hold the tick up for long.
TIMEOUT_SECONDS = 30


class RecordingsServer:
    def __init__(self, clone, run=subprocess.run):
        self.clone = clone      # autodev's clone, the service's working folder
        self.run = run

    def keep_current(self, log):
        """Restart the service if it's running and its file changed since
        it started. Never raises: a failure is one error line in `log`.
        Returns the exit code the tick should pass on (0, or 1 for a
        failure)."""
        try:
            if not self._running_old_code():
                return 0
        except Exception as e:
            print("error: checking the recordings server: %s" % e, file=log)
            return 1
        try:
            # try-restart restarts only a running unit, so a server stopped
            # in the moment since the check above stays stopped.
            self._systemctl("try-restart", UNIT)
        except Exception as e:
            print("error: restarting the recordings server: %s" % e, file=log)
            return 1
        print("restarted the recordings server: %s changed since it started"
              % SERVER, file=log)
        return 0

    def _running_old_code(self):
        """Whether the service is running and its file changed after it
        started."""
        started = self._started_at()
        if started is None:
            return False
        modified = os.path.getmtime(os.path.join(self.clone, SERVER))
        # systemd gives the start in whole seconds, so a change in that same
        # second can't be told from one just before it, and counts as before.
        return int(modified) > started

    def _started_at(self):
        """When the running service started, in whole seconds since 1970,
        or None if it isn't running."""
        # --timestamp=unix prints the start as "@1792374861"; an empty value
        # means it hasn't started since boot.
        shown = self._systemctl("show", UNIT, "-p", "ActiveState",
                                "-p", "ActiveEnterTimestamp", "--timestamp=unix")
        values = dict(line.split("=", 1) for line in shown.splitlines()
                      if "=" in line)
        state = values.get("ActiveState")
        since = values.get("ActiveEnterTimestamp")
        if not state or since is None or not re.fullmatch(r"(@\d+)?", since):
            raise ValueError("systemctl show printed %r" % shown)
        # Only "active" counts as running. A service starting or stopping
        # right now reads the file afresh whenever it next runs.
        if state != "active" or not since:
            return None
        return int(since[1:])

    def _systemctl(self, *args):
        """`systemctl --user <args>`'s output; raises if it fails."""
        done = self.run(["systemctl", "--user", *args], capture_output=True,
                        text=True, timeout=TIMEOUT_SECONDS)
        if done.returncode != 0:
            raise RuntimeError(done.stderr.strip()
                               or "systemctl %s exited %d" % (args[0], done.returncode))
        return done.stdout
