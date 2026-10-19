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
            running = self.is_active(sessions.unit_name(record["role"], record["issue"]))
            found.append(Session(
                role=record["role"], issue=record["issue"],
                session_id=record["session_id"], running=running,
                result=None if running else sessions.read_result(record["log"])))
        return found

    def is_active(self, unit):
        # "activating" and "active" both count: the session is under way.
        return self.run(["systemctl", "--user", "is-active", "--quiet", unit]
                        ).returncode == 0

    def start(self, role, issue, prompt):
        os.makedirs(self.config.state_dir, exist_ok=True)
        os.makedirs(self.config.log_dir, exist_ok=True)
        session_id = str(uuid.uuid4())
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
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
        return record

    def finish(self, role, issue):
        done = os.path.join(self.config.state_dir, "done")
        os.makedirs(done, exist_ok=True)
        os.replace(self._record_path(role, issue),
                   os.path.join(done, "%s-%d-%s.json" % (
                       role, issue, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))))
