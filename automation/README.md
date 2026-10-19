# Autodev

Autodev runs this repo's GitHub Issue Workflow (CLAUDE.md steps 1–13) on its own. You add an issue to the **pokefirered autodev** board. A headless Claude Code session grills it, taking every recommended answer, and breaks it into tickets. Other sessions implement the tickets and open PRs. Nothing reaches master until you comment `/merge` on the PR. The full design is on issue #48.

> **Status:** foundation only (#53). The dispatcher reads the board every 2 minutes and logs what it sees. It doesn't act on anything yet. Later tickets add grilling (#55), implementation (#56), merging (#57), pausing and failure handling (#58), and fix-ups (#59).

## How it runs

| Piece | What it is |
|---|---|
| Bot account | A separate GitHub account. Autodev reads and writes as the bot, never as you, so its PRs and comments notify you, and its token can only reach this repo. |
| `~/.config/autodev/env` | The bot's token (`GH_TOKEN`) and login (`BOT_LOGIN`). Mode 600, never committed. |
| `~/projects/minimaleffort/pokefirered-autodev` | Autodev's own clone, kept apart from the checkout you work in. It pushes as the bot over HTTPS, using `gh` as git's credential helper. |
| `autodev.timer` | A systemd user timer that starts `autodev-dispatcher.service` every 2 minutes. |
| `autodev-dispatcher.service` | One dispatcher tick: `git pull` the clone, then `python3 -m autodev tick` (code in `automation/autodev/`). |
| `autodev-recordings.service` | `test/serve_recordings.py` on port 8080, so the recording links in PRs always work. |
| Linger | Keeps systemd user units running when no terminal is open. |
| Windows logon task | Starts a hidden `wsl.exe … sleep infinity` at Windows logon, because WSL shuts down when no Windows program is using it. |
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
journalctl --user -u autodev-dispatcher -f        # each tick's board snapshot
systemctl --user stop autodev.timer               # stop everything
systemctl --user start autodev.timer              # start again
```

A tick logs one line per issue on the board. The line gives the issue number, its author, its Status (a card with no Status shows as `Todo`), and whether the body has a `## Parent` section (that makes it a ticket; anything else is a parent to be grilled):

```
board #7 "pokefirered autodev": 2 items
#63 MinimalEffort07 Blocked has_parent=yes
#61 MinimalEffort07 Todo has_parent=no
```

## Tests

```bash
cd automation && python3 -m unittest
```

The tests replay recorded GitHub responses (`tests/fixtures/`) through a fake `gh`. They check what the tick logs, and that it sends GitHub nothing that would change anything.
