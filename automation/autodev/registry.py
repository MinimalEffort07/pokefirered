"""The sessions the dispatcher has started, kept on disk between ticks.

Each tick is a fresh process, so the dispatcher remembers its sessions in
small JSON files (one per session) in the state directory. A session's
systemd unit says whether it is still running; once it has ended, its log
holds its result. `finish()` moves the record into done/ so the session
is handled exactly once.
"""

import datetime
import json
import os
import subprocess
import time
import uuid

from autodev import sessions
from autodev.decide import Session


class Registry:
    def __init__(self, config, run=subprocess.run):
        self.config = config    # sessions.SessionConfig
        self.run = run

    def _record_path(self, role, issue):
        return os.path.join(self.config.state_dir, "%s-%d.json" % (role, issue))

    def load(self):
        found = []
        if not os.path.isdir(self.config.state_dir):
            return found
        for name in sorted(os.listdir(self.config.state_dir)):
            if not name.endswith(".json"):
                continue
            with open(os.path.join(self.config.state_dir, name)) as f:
                record = json.load(f)
            role, issue = record["role"], record["issue"]
            running = self.is_running(sessions.unit_name(role, issue))
            found.append(Session(
                role=role, issue=issue,
                session_id=record["session_id"], running=running,
                result=None if running else sessions.read_result(record["log"]),
                ended_by=None if running else _ended_by(record["log"]),
                cwd=sessions.working_directory(self.config, role, issue)))
        return found

    # systemd's ActiveState for a unit that is still under way. A unit that
    # has ended is "inactive" or "failed"; one systemd has already
    # forgotten (--collect) also shows as "inactive".
    RUNNING_STATES = {"active", "activating", "deactivating", "reloading",
                      "refreshing"}

    def is_running(self, unit):
        state = self.run(["systemctl", "--user", "show", "--property",
                          "ActiveState", "--value", unit],
                         capture_output=True, text=True).stdout.strip()
        return state in self.RUNNING_STATES

    def start(self, role, issue, prompt):
        os.makedirs(self.config.state_dir, exist_ok=True)
        os.makedirs(self.config.log_dir, exist_ok=True)
        session_id = str(uuid.uuid4())
        stamp = _stamp()
        log = os.path.join(self.config.log_dir, "%s-%d-%s.jsonl" % (role, issue, stamp))
        record = {"role": role, "issue": issue, "session_id": session_id,
                  "log": log, "started": stamp}
        with open(self._record_path(role, issue), "w") as f:
            json.dump(record, f, indent=2)
        try:
            self.run(sessions.launch_command(self.config, role, issue, session_id,
                                             log, prompt), check=True)
        except Exception:
            os.remove(self._record_path(role, issue))
            raise

    def finish(self, role, issue):
        done = os.path.join(self.config.state_dir, "done")
        os.makedirs(done, exist_ok=True)
        os.replace(self._record_path(role, issue),
                   os.path.join(done, "%s-%d-%s.json" % (role, issue, _stamp())))

    def prune(self, days=30, now=None):
        """Delete session logs and handled records older than `days`
        (#48 keeps logs for 30 days). Returns how many files went."""
        cutoff = (now if now is not None else time.time()) - days * 24 * 3600
        removed = 0
        for folder in (self.config.log_dir, os.path.join(self.config.state_dir, "done")):
            if not os.path.isdir(folder):
                continue
            for name in os.listdir(folder):
                path = os.path.join(folder, name)
                if os.path.isfile(path) and os.path.getmtime(path) < cutoff:
                    os.remove(path)
                    removed += 1
        return removed


def _ended_by(log):
    """How systemd said the unit ended (sessions.exit_path), or None if it
    didn't say."""
    try:
        with open(sessions.exit_path(log)) as f:
            return f.read().strip() or None
    except FileNotFoundError:
        return None


def _stamp():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
