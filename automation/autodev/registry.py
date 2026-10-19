"""The sessions the dispatcher has started, kept on disk between ticks.

Each tick is a fresh process, so the dispatcher remembers its sessions in
small JSON files (one per session) in the state directory. A session's
systemd unit says whether it is still running; once it has ended, its log
holds its result. `finish()` moves the record into done/ so the session
is handled exactly once; a crashed one is marked there, because a second
crash in a row parks the work (D20). A usage-limit pause (D18) is kept in the file
`paused-until`, so it lasts across ticks and reboots.
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

    def load(self, now=None):
        now = time.time() if now is None else now
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
            result = None if running else sessions.read_result(record["log"])
            limit_reset, limit_note = None, ""
            if not running and result is None:
                text = sessions.limit_text(record["log"])
                # "resets 3pm" means the 3pm after the session stopped, not
                # after this tick: by now it may be 3:01pm, or tomorrow.
                limit_reset = sessions.usage_limit(text, _ended_at(record["log"], now))
                if limit_reset is not None:
                    limit_note = sessions.limit_line(text)
            found.append(Session(
                role=role, issue=issue,
                session_id=record["session_id"], running=running, result=result,
                ended_by=None if running else _ended_by(record["log"]),
                cwd=sessions.working_directory(self.config, role, issue),
                pr=record.get("pr"), handled_through=record.get("handled_through"),
                limit_reset=limit_reset, limit_note=limit_note))
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

    def start(self, role, issue, prompt, pr=None, handled_through=None):
        os.makedirs(self.config.state_dir, exist_ok=True)
        os.makedirs(self.config.log_dir, exist_ok=True)
        session_id = str(uuid.uuid4())
        stamp = _stamp()
        log = os.path.join(self.config.log_dir, "%s-%d-%s.jsonl" % (role, issue, stamp))
        record = {"role": role, "issue": issue, "session_id": session_id,
                  "log": log, "started": stamp}
        if pr is not None:      # a merge or fix-up's PR and the comment it answers
            record.update(pr=pr, handled_through=handled_through)
        with open(self._record_path(role, issue), "w") as f:
            json.dump(record, f, indent=2)
        try:
            self.run(sessions.launch_command(self.config, role, issue, session_id,
                                             log, prompt, pr=pr,
                                             handled_through=handled_through),
                     check=True)
        except Exception:
            os.remove(self._record_path(role, issue))
            raise

    def finish(self, role, issue, crashed=False, requeued=False):
        """Move the session's record into done/, marked if it crashed (a
        second crash parks the work) or its work was re-queued."""
        done = os.path.join(self.config.state_dir, "done")
        os.makedirs(done, exist_ok=True)
        path = self._record_path(role, issue)
        with open(path) as f:
            record = json.load(f)
        record.update(finished=time.time(), crashed=crashed, requeued=requeued)
        # Rewrite it in place first, atomically, so the move into done/
        # below stays a single step: never handled twice, never lost.
        with open(path + ".tmp", "w") as f:
            json.dump(record, f, indent=2)
        os.replace(path + ".tmp", path)
        # The session id keeps two sessions finished in the same second apart.
        os.replace(path, os.path.join(done, "%s-%d-%s-%s.json" % (
            role, issue, _stamp(), record["session_id"][:8])))

    def _done_records(self):
        """(path, record) for every finished session, oldest first. A file
        that isn't a record autodev wrote is skipped."""
        done = os.path.join(self.config.state_dir, "done")
        if not os.path.isdir(done):
            return []
        records = []
        for name in os.listdir(done):
            path = os.path.join(done, name)
            try:
                with open(path) as f:
                    records.append((path, json.load(f)))
            except ValueError:
                continue
        # Records from before #58 have no "finished" time; they go first.
        return sorted(records, key=lambda r: r[1].get("finished", 0))

    def _crash_streaks(self):
        """{(role, issue): the records of its crashes in a row lately}. A run
        that ends any other way (done or parked) starts the count again: a PR
        gets a fresh fix-up or merge session for each comment, and one
        crashing long after another isn't the same work crashing twice. A
        usage-limit re-queue neither counts nor resets it."""
        streaks = {}
        for path, record in self._done_records():
            key = (record["role"], record["issue"])
            if record.get("crashed"):
                streaks.setdefault(key, []).append(path)
            elif not record.get("requeued"):
                streaks.pop(key, None)
        return streaks

    def crashes(self):
        """How many sessions of each (role, issue) crashed in a row lately
        (a second parks the work, D20)."""
        return {key: len(paths) for key, paths in self._crash_streaks().items()}

    def requeued(self):
        """The (role, issue) pairs whose last session's work was re-queued
        and hasn't run again since."""
        last = {}
        for _, record in self._done_records():
            last[(record["role"], record["issue"])] = record.get("requeued", False)
        return {key for key, requeued in last.items() if requeued}

    def _pause_path(self):
        return os.path.join(self.config.state_dir, "paused-until")

    def paused_until(self):
        """No session starts before this time (seconds since 1970); 0 if no
        usage limit is in force."""
        try:
            with open(self._pause_path()) as f:
                return float(f.read().strip() or 0)
        except (FileNotFoundError, ValueError):
            return 0.0      # no pause, or a file nobody can read: no pause

    def pause(self, until):
        """Start no session before `until`. A pause only ever gets longer."""
        until = max(until, self.paused_until())    # read before opening to write
        os.makedirs(self.config.state_dir, exist_ok=True)
        with open(self._pause_path(), "w") as f:
            f.write("%.0f\n" % until)

    def prune(self, days=30, now=None):
        """Delete session logs and handled records older than `days`
        (#48 keeps logs for 30 days). Returns how many files went."""
        cutoff = (now if now is not None else time.time()) - days * 24 * 3600
        # A crash still counts however old it is, until a later run of that
        # role and issue finishes (D20).
        counting = {p for paths in self._crash_streaks().values() for p in paths}
        removed = 0
        for folder in (self.config.log_dir, os.path.join(self.config.state_dir, "done")):
            if not os.path.isdir(folder):
                continue
            for name in os.listdir(folder):
                path = os.path.join(folder, name)
                if (os.path.isfile(path) and os.path.getmtime(path) < cutoff
                        and path not in counting):
                    os.remove(path)
                    removed += 1
        return removed


def _ended_at(log, now):
    """When the session stopped: when systemd wrote its end reason, else
    when its log last changed, else `now`."""
    for path in (sessions.exit_path(log), log):
        try:
            return os.path.getmtime(path)
        except FileNotFoundError:
            continue
    return now


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
