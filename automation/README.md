# Autodev

Autodev runs this repo's GitHub Issue Workflow (CLAUDE.md steps 1–13) on its own. You add an issue to the **pokefirered autodev** board. A headless Claude Code session grills it, taking every recommended answer, and breaks it into tickets. Other sessions implement the tickets and open PRs. Nothing reaches master until you comment `/merge` on the PR. The full design is on issue #48.

> **Status:** grilling works (#55). An issue you add to the board gets grilled into a design and tickets. The tickets then wait in `Todo`: implementation (#56), merging (#57), pausing and failure handling (#58) and fix-ups (#59) come next.

## What happens to an issue you add

1. **On the next tick** (at most 2 minutes), the dispatcher sees your issue on the board with no Status (or `Todo`). An issue with no `## Parent` heading is a **parent**:
   - It starts a grill session for it and moves the card to `Grilling`.
   - At most 2 sessions run at once; the rest wait their turn, oldest issue first.
   - An issue written by anyone but you is never handed to a session. It goes to `Needs human`, and the bot comments: "Autodev only runs issues written by @MinimalEffort07; restate it as your own issue to run it."
2. **The grill session** follows CLAUDE.md's "Autonomous mode" for the grill role:
   - It grills the issue by itself, taking every recommended answer, and posts the design comment. That comment has a "Least-confident defaults" list and every round in a collapsed `<details>` block.
   - It rewrites the issue body into a scope summary, then creates the tickets and links each one as a sub-issue.
   - It is read-only on the code and has 60 minutes.
3. **On the first tick after it ends**, the dispatcher checks GitHub: is the design comment there, and is there at least one ticket?
   - **Yes:** each ticket goes on the board, as `Blocked` if an issue under its `## Blocked by` is still open, otherwise `Todo`. The parent moves to `In progress`.
   - **No**, or the session reported a failure, or it ended without a result (it crashed or hit its time limit): the parent goes to `Needs human`. The bot comments with the reason, @mentions you, and gives the command that resumes the session.

## How it runs

| Piece | What it is |
|---|---|
| Bot account | A separate GitHub account. Autodev reads and writes as the bot, never as you, so its PRs and comments notify you, and its token can only reach this repo. |
| `~/.config/autodev/env` | The bot's token (`GH_TOKEN`) and login (`BOT_LOGIN`). Mode 600, never committed. |
| `~/projects/minimaleffort/pokefirered-autodev` | Autodev's own clone, kept apart from the checkout you work in. It pushes as the bot over HTTPS, using `gh` as git's credential helper. |
| `autodev.timer` | A systemd user timer that starts `autodev-dispatcher.service` every 2 minutes. |
| `autodev-dispatcher.service` | One dispatcher tick: `git pull` the clone, then `python3 -m autodev tick` (code in `automation/autodev/`). |
| `autodev-<role>-<issue>.service` | One Claude session, e.g. `autodev-grill-67`. It's a transient unit that the dispatcher starts with `systemd-run`, and systemd forgets it once it ends. It runs `claude -p` in the clone as the bot (the env file), with your Claude login and plugins (`CLAUDE_CONFIG_DIR=~/.claude-personal`), the role's permissions (`automation/settings/<role>.json`) and the role's time limit. |
| `~/autodev/logs/` | Each session's full stream-json log (`<role>-<issue>-<time>.jsonl`, with stderr in `.err`). The last `result` event holds what the session reported. |
| `~/.local/state/autodev/sessions/` | One small record per session the dispatcher started: its session id and log path. Once a session's outcome is handled, its record moves to `done/`. |
| `autodev-recordings.service` | `test/serve_recordings.py` on port 8080, so the recording links in PRs always work. |
| Linger | Keeps systemd user units running when no terminal is open. |
| Windows logon keepalive | Starts a hidden `wsl.exe … sleep infinity` at every Windows logon, because WSL shuts down when no Windows program is using it. It's a Task Scheduler task when Windows allows one, and otherwise a "WSL autodev keepalive" shortcut in your Startup folder (Task Scheduler usually wants admin for logon tasks). |
| Master rulesets | "autodev: master history" blocks force-pushes and deleting master, with no exceptions. "autodev: master via PR" requires a PR from anyone who isn't an admin, so the bot can only change master by merging one. |

## Setup

1. **Run the wizard** (human-only steps: create the bot account and its token, add the `project` scope to your `gh` login, add the Windows logon task):

   ```bash
   bash automation/wizard.sh
   ```

2. **Run the setup script** (everything scriptable: the board and its Status options, the bot's access to the repo and the board, the rulesets, the clone, the systemd units, linger):

   ```bash
   bash automation/setup.sh
   ```

   Both scripts are safe to run again. A second `setup.sh` run ends with `setup: no changes`. If `setup.sh` can't create the rulesets through the API, it tells you to run the wizard again, whose last stage walks you through creating them by hand.

## Watching it

```bash
systemctl --user list-timers autodev.timer        # when the next tick is due
journalctl --user -u autodev-dispatcher -f        # each tick: snapshot, then actions
systemctl --user list-units 'autodev-*'           # sessions running right now
systemctl --user stop autodev.timer               # stop new ticks (running sessions carry on)
systemctl --user stop autodev-grill-67            # stop one running session
systemctl --user start autodev.timer              # start again
```

A tick logs one line per issue on the board. The line gives the issue number, its author, its Status (a card with no Status shows as `Todo`), and whether the body has a `## Parent` section (that makes it a ticket; anything else is a parent to be grilled):

```
board #7 "pokefirered autodev": 2 items
#63 MinimalEffort07 Blocked has_parent=yes
#61 MinimalEffort07 Todo has_parent=no
```

After the snapshot, the tick logs each action it takes, e.g. `→ Launch(role='grill', issue=61)` and then `→ SetStatus(issue=61, status='Grilling')`.

### Resuming a parked session

A `Needs human` comment ends with a command like this one, which reopens the session interactively exactly where it stopped:

```bash
cd ~/projects/minimaleffort/pokefirered-autodev && CLAUDE_CONFIG_DIR=~/.claude-personal claude --resume <session-id>
```

## Tests

```bash
cd automation && python3 -m unittest
```

What they cover:
- **`test_tick.py`:** replays recorded GitHub responses (`tests/fixtures/`) through a fake `gh`, and checks the board snapshot the tick logs.
- **`test_decide.py`:** checks every decision (`decide()` is a pure function) against a hand-built state, one test per scenario from #48 (D1, D2, D15, D16, D24).
- **`test_sessions.py`:** pins down the session unit's command line: the bot's identity, your Claude login, the time limit and the role's permissions. It also checks how a session's result is read back from its log, and that the grill role keeps its deny rules (D25).
