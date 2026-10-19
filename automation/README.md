# Autodev

Autodev runs this repo's GitHub Issue Workflow (CLAUDE.md steps 1–13) on its own. You add an issue to the **pokefirered autodev** board. A headless Claude Code session grills it, taking every recommended answer, and breaks it into tickets. Other sessions implement the tickets and open PRs. Nothing reaches master until you comment `/merge` on the PR. The full design is on issue #48.

> **Status:** grilling (#55) and implementation (#56) work. An issue you add to the board gets grilled into a design and tickets, and each ticket is implemented in its own worktree as soon as nothing blocks it, ending in a PR that waits for you. Merging on `/merge` (#57), pausing and failure handling (#58) and fix-ups (#59) come next. Until then, merge a PR yourself; its `Fixes #<T>` closes the ticket, and that unblocks the tickets waiting on it.

## What happens to an issue you add

1. **On the next tick** (at most 2 minutes), the dispatcher sees your issue on the board with no Status (or `Todo`). An issue with no `## Parent` heading is a **parent**:
   - It starts a grill session for it and moves the card to `Grilling`.
   - At most 2 sessions run at once, of any kind; the rest wait their turn. Tickets go before parents. Among tickets, the oldest parent's go first, then the lowest number; among parents, the oldest issue first.
   - An issue written by anyone but you is never handed to a session. It goes to `Needs human`, and the bot comments: "Autodev only runs issues written by @MinimalEffort07; restate it as your own issue to run it."
2. **The grill session** follows CLAUDE.md's "Autonomous mode" for the grill role:
   - It grills the issue by itself, taking every recommended answer, and posts the design comment. That comment has a "Least-confident defaults" list and every round in a collapsed `<details>` block.
   - It rewrites the issue body into a scope summary, then creates the tickets and links each one as a sub-issue.
   - It is read-only on the code and has 60 minutes.
3. **On the first tick after it ends**, the dispatcher checks GitHub: is the design comment there, and is there at least one ticket?
   - **Yes:** each ticket goes on the board, as `Blocked` if an issue under its `## Blocked by` is still open, otherwise `Todo`. The parent moves to `In progress`.
   - **No**, or the session reported a failure, or it ended without a result (it crashed or hit its time limit): the parent goes to `Needs human`. The bot comments with the reason, @mentions you, and gives the command that resumes the session.

## What happens to a ticket

An issue whose body has a `## Parent` heading is a **ticket**. Grill sessions write them, as the bot; you can write one too. A ticket written by anyone else goes to `Needs human`, like a parent.

1. **It waits for its blockers.** On every tick, a ticket in `Todo` with an open issue under its `## Blocked by` heading moves to `Blocked`. Once all of them are closed, it moves back to `Todo`. A closed issue counts as done whether a PR closed it or you did, so closing a ticket is how you skip it.
2. **A ticket in `Todo` with nothing open blocking it** gets its own worktree, `.worktrees/issue-<T>` in the clone. The worktree is on a new branch, `issue-<T>-<first five words of the title>`, cut from master, with `tools/agbcc` linked. A ticket session starts in it, and the card moves to `In progress`.
3. **The ticket session** follows CLAUDE.md's "Autonomous mode" for the ticket role, steps 7–11:
   - TDD, a clean build and `bash test/run_suite.sh`
   - a recording if anything changes in the game (recordings take turns: see "How it runs")
   - the README, then a code review against master
   - finally `python3 automation/bin/issue.py open-pr`, which only opens a PR from the ticket's own `issue-<T>-*` branch, and only with `Fixes #<T>` and an @mention of you in the body

   It has 3 hours.
4. **On the first tick after it ends**, the dispatcher looks on GitHub for the bot's open PR from an `issue-<T>-*` branch of this repo. A PR from a fork doesn't count.
   - **Found:** the card moves to `In review`, and the PR waits for you.
   - **Not found**, or the session reported a failure, or it ended without a result: the card goes to `Needs human`, with the reason and the resume command. When systemd stopped the session at its time limit, the reason says so ("hit its 3-hour time limit").

## How it runs

| Piece | What it is |
|---|---|
| Bot account | A separate GitHub account. Autodev reads and writes as the bot, never as you, so its PRs and comments notify you, and its token can only reach this repo. |
| `~/.config/autodev/env` | The bot's token (`GH_TOKEN`) and login (`BOT_LOGIN`). Mode 600, never committed. |
| `~/projects/minimaleffort/pokefirered-autodev` | Autodev's own clone, kept apart from the checkout you work in. It pushes as the bot over HTTPS, using `gh` as git's credential helper. |
| `autodev.timer` | A systemd user timer that starts `autodev-dispatcher.service` every 2 minutes. |
| `autodev-dispatcher.service` | One dispatcher tick: `git pull` the clone, then `python3 -m autodev tick` (code in `automation/autodev/`). |
| `autodev-<role>-<issue>.service` | One Claude session, e.g. `autodev-grill-67` or `autodev-ticket-68`. It's a transient unit that the dispatcher starts with `systemd-run`, and systemd forgets it once it ends. It runs `claude -p` as the bot (the env file), with your Claude login and plugins (`CLAUDE_CONFIG_DIR=~/.claude-personal`), the role's permissions and the role's time limit (grill 60 minutes, ticket 3 hours). A grill session runs in the clone; a ticket session runs in the ticket's worktree. |
| `.worktrees/issue-<T>` | Each ticket's worktree in the clone, on its branch `issue-<T>-<slug>`. The ticket's later fix-up and merge sessions (#57, #59) reuse it. |
| `automation/settings/<role>.json` | Each role's permissions. A session always uses the clone's copy, never its worktree's, because it could edit the one in its worktree. The grill role is read-only. The ticket and fix-up roles may edit, build, test, commit and push, but they can't use `gh`, push to master, force-push, delete branches or touch `pret/pokefirered`. In auto mode an allow rule only skips the safety classifier, so every limit is a deny rule. The master rulesets are the backstop. |
| `automation/bin/issue.py` | A session's only way to reach GitHub; its settings deny plain `gh`. Sessions run the clone's copy (a ticket session by its full path), never their worktree's. It shows only text written by you or the bot, and it changes only the session's own issue (`AUTODEV_ISSUE`, set by the dispatcher). Each role gets only its own commands (`AUTODEV_ROLE`): a grill session may comment, rewrite its parent's body and create tickets; a ticket session may comment and open its PR. No labels, no closing, no merging, no board. |
| `~/autodev/logs/` | Each session's full stream-json log (`<role>-<issue>-<time>.jsonl`, with stderr in `.err`). The last `result` event holds what the session reported. When the unit ends, systemd writes how it ended into `.exit` (`success`, `exit-code`, or `timeout` at the time limit). The tick deletes logs older than 30 days. |
| `/tmp/autodev-record.lock` | The machine-wide lock `test/record_test.sh` takes, because a recording films the whole virtual display. Sessions recording at the same time take turns. |
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
systemctl --user stop autodev-ticket-68           # stop one running session
systemctl --user start autodev.timer              # start again
```

A tick logs one line per issue on the board. The line gives the issue number, its author, its Status (a card with no Status shows as `Todo`), and whether the body has a `## Parent` section (that makes it a ticket; anything else is a parent to be grilled):

```
board #7 "pokefirered autodev": 2 items
#63 MinimalEffort07 Blocked has_parent=yes
#61 MinimalEffort07 Todo has_parent=no
```

After the snapshot, the tick logs each action it takes, e.g. `→ Launch(role='grill', issue=61)` and then `→ SetStatus(issue=61, status='Grilling')`. For a ticket, the worktree comes first: `→ MakeWorktree(issue=68, branch='issue-68-note-the-grill-paths-live')`, then the launch and the move to `In progress`.

### Resuming a parked session

A `Needs human` comment ends with a command like this one, which reopens the session interactively exactly where it stopped. `claude --resume` only finds a session from the folder it ran in, so the command starts in that folder: the clone for a grill session, the ticket's worktree for a ticket session.

```bash
cd ~/projects/minimaleffort/pokefirered-autodev/.worktrees/issue-68 && CLAUDE_CONFIG_DIR=~/.claude-personal claude --resume <session-id>
```

## Tests

```bash
cd automation && python3 -m unittest
```

What they cover:
- **`test_tick.py`:** replays recorded GitHub responses (`tests/fixtures/`) through a fake `gh`, and checks the board snapshot the tick logs, including each ticket's parent and blockers read from its body.
- **`test_decide.py`:** checks every decision (`decide()` is a pure function) against a hand-built state, one test per scenario from #48:
  - parents: D1, D2, and D15, D16 and D24 for grills
  - tickets: D2 (someone else's ticket), D3 (launch), D4 (`Blocked`), D5 (back to `Todo`), D6 (two slots, by priority), D15 (`In review`), D16 (no PR) and D17 (time limit), plus a closed issue never launching
- **`test_sessions.py`:**
  - pins down the session unit's command line for a grill and for a ticket: the bot's identity, your Claude login, the working folder, the time limit, the role's permissions, the session's issue and role, and the record of how the unit ended
  - checks what counts as "still running", and how a session's result, end reason and folder are read back
  - checks that each role keeps its deny rules (D25) and has no plain `gh` access
- **`test_worktrees.py`:** runs real git in a temporary folder. A ticket's worktree is cut from `origin/master` whatever the clone has checked out, its branch tracks nothing (so a bare `git push` can't aim at master), `tools/agbcc` is linked, and asking again leaves an existing worktree alone.
- **`test_issue_tool.py`:** checks `bin/issue.py`'s rules:
  - untrusted authors' text is withheld, and comments get the marker
  - a ticket must name its parent, and it is created without labels and linked as a sub-issue
  - a PR comes only from the ticket's own branch, into master, with `Fixes #<T>` and an @mention of you
  - each role may use only its own commands
- **`test_github.py`:** checks what the dispatcher accepts as a finished session's work. For a grill: only the bot's comment with the design marker, and only open issues whose `## Parent` names the parent, with their blockers checked. For a ticket: only the bot's open PR from that ticket's branch in this repo. It also covers deleting session files older than 30 days.
- **Failures:** if one of a tick's actions fails, the tick logs it and skips the rest of that issue's actions, but carries on with the others.
