# Autodev

Autodev runs this repo's GitHub Issue Workflow (CLAUDE.md steps 1–13) on its own. You add an issue to the **pokefirered autodev** board. A headless Claude Code session grills it, taking every recommended answer, and breaks it into tickets. Other sessions implement the tickets and open PRs. Nothing reaches master until you comment `/merge` on the PR. The full design is on issue #48.

> **Status:** grilling (#55), implementation (#56), merging (#57) and pausing and failure handling (#58) work. An issue you add to the board gets grilled into a design and tickets. Each ticket is implemented in its own worktree as soon as nothing blocks it, ending in a PR that waits for you. Your `/merge` comment on that PR gets it merged, and the ticket and, in the end, the parent are closed. `Paused` stops work; usage limits and crashes are ridden out. Fix-ups after your feedback (#59) come next.

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
   - **No**, or the session reported a failure, or it hit its time limit: the parent goes to `Needs human`. The bot comments with the reason, @mentions you, and gives the command that resumes the session. A crash or the usage limit re-queues it instead (see "Pausing, usage limits and crashes").

## What happens to a ticket

An issue whose body has a `## Parent` heading is a **ticket**. Grill sessions write them, as the bot; you can write one too. A ticket written by anyone else goes to `Needs human`, like a parent.

1. **It waits for its blockers.** On every tick, a ticket in `Todo` with an open issue under its `## Blocked by` heading moves to `Blocked`. Once all of them are closed, it moves back to `Todo`. A closed issue counts as done whether a PR closed it or you did, so closing a ticket is how you skip it.
2. **A ticket in `Todo` with nothing open blocking it** gets its own worktree, `~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-<T>`, beside the clone. The worktree is on a new branch, `issue-<T>-<slug>`, cut from master, with `tools/agbcc` linked. The slug is the title's first five words, or fewer if five would go over 40 characters. If the folder is gone but the branch is still there, the worktree is made again on that branch. A ticket session starts in it, and the card moves to `In progress`.
3. **The ticket session** follows CLAUDE.md's "Autonomous mode" for the ticket role, steps 7–11:
   - TDD, a clean build and `bash test/run_suite.sh`
   - a recording if anything changes in the game (recordings take turns: see "How it runs")
   - the README, then a code review against master
   - finally `issue.py open-pr`, which only opens a PR from the ticket's own `issue-<T>-*` branch, and only when the body has Fixes #<T> and an @mention of you written outside code (GitHub ignores both inside code)

   It has 3 hours.
4. **On the first tick after it ends**, the dispatcher looks on GitHub for the bot's open PR from an `issue-<T>-*` branch of this repo. A PR from a fork doesn't count.
   - **Found:** the card moves to `In review`, and the PR waits for you.
   - **Not found**, or the session reported a failure, or it hit its time limit: the card goes to `Needs human`, with the reason and the resume command ("hit its 3-hour time limit" when systemd stopped it). A crash or the usage limit re-queues it instead, unless its PR is already open, which counts as done.

## When you comment `/merge`

1. **On the next tick**, the dispatcher reads everything said on each `In review` ticket's PR: conversation comments, reviews and inline review comments.
   - **What counts as unhandled:** each of autodev's replies ends with a hidden `<!-- autodev handled-through: <id> -->` marker naming the newest of your comments it dealt with. Your comments after that one are unhandled; nobody else's count.
   - **When it merges:** if your newest unhandled comment is exactly `/merge`, a merge session starts and the card moves to `In progress`. Feedback you post after `/merge` cancels it; fix-ups for feedback come with #59.
   - **While a session runs:** comments that arrive wait until it ends.
2. **The merge session** runs in the ticket's worktree and follows CLAUDE.md's "Autonomous mode" for the merge role. It has 45 minutes.
   - It brings the branch up to date, merges master in, and builds and runs the unit tests and `run_suite.sh` on that exact tree.
   - **If all of that passes,** it pushes, then merges with `issue.py merge-pr`. That always makes a merge commit (never a squash or rebase) of the commit it tested. It refuses if master has moved on since, so the merged tree is always the tested tree.
   - **On a conflict,** it resolves it on the branch, re-tests and pushes. Then it explains on the PR exactly how it resolved each conflict, @mentioning you. It doesn't merge: your fresh `/merge` approves the resolution and starts a new merge session.
3. **On the first tick after it ends:**
   - **Merged** (even if the session failed or timed out afterwards): the ticket is closed (the PR's closing line usually did that already), its worktree and local branch are removed, and the card moves to `Done`.
   - **Conflict reply posted:** the card goes back to `In review` to wait for your `/merge`.
   - **Neither,** or the session reported a failure, or it hit its time limit: `Needs human`, with the reason and the resume command. A crash or the usage limit sends it back to `In review` instead, unless its conflict reply is already posted, which counts as done.
4. **Closed tickets are tidied up whoever closed them.** A ticket you close yourself, or whose PR you merge yourself, also loses its worktree, and its card moves to `Done`. The tickets it blocked move on to `Todo`.
5. **Once every ticket of one of your parents is closed,** the bot closes the parent, comments on it @mentioning you, and moves it to `Done`. A parent's tickets are the board issues whose `## Parent` names it.

## Pausing, usage limits and crashes

- **`Paused` is your stop switch.**
  - **What it stops:** on a parent, none of its tickets start, neither implementation nor merge. On a ticket, that ticket doesn't start.
  - **Sessions already running** finish their run. When one ends, its comments still post, but a paused card stays `Paused`.
  - **Resuming:** move the card back to `Todo`, and it is picked up on the next tick.
    - A parent that already has tickets goes back to `In progress`; it isn't grilled again.
    - A ticket whose PR is already open goes back to `In review`; it isn't implemented again.
  - **Stopping a session for good:** pause its card (or its parent), then `systemctl --user stop` its unit. A stop on its own counts as a crash, so the work would run once more.
- **The usage limit:** a session that stops on the Claude usage-limit message ("You've hit your … limit · resets 3pm" and similar) pauses everything.
  - No session of any role starts until the reset time plus 5 minutes, worked out from when the session stopped. The board is still kept up to date, and each tick logs `usage limit: no session starts until …`.
  - The bot comments "paused until <time>" on the issue, @mentioning you. Only an error result or stderr is read for the message, never the session's own words.
  - The interrupted work goes back to `Todo`, or a merge back to `In review` with your `/merge` still unhandled. It starts again once the pause is over, as a fresh session that carries on from its branch.
  - **Unreadable time:** if the message's time can't be read, the pause lasts an hour.
  - **Where it's kept:** the pause is in `~/.local/state/autodev/sessions/paused-until`, so it survives reboots. Delete that file to lift it early.
- **Crashes:** a session that ends with no report, no usage-limit message and not at its time limit has crashed. That includes one killed or stopped, and one whose unit vanished in a reboot.
  - **The first time:** the work is re-queued (`Todo`, or `In review` for a merge), and the bot notes it on the issue. A re-queued grill is grilled again, even though the tickets it had already created are on the board (the board adds sub-issues as they are made). Those tickets wait while their parent is `Grilling`.
  - **Work already done counts:** a ticket session whose PR is open, or a merge session whose conflict reply is posted, is done however it ended.
  - **The second time** for the same role and issue: `Needs human`, with the reason and the resume command.
  - **Time limit:** a session that hit its time limit goes to `Needs human` straight away.

## How it runs

| Piece | What it is |
|---|---|
| Bot account | A separate GitHub account. Autodev reads and writes as the bot, never as you, so its PRs and comments notify you, and its token can only reach this repo. |
| `~/.config/autodev/env` | The bot's token (`GH_TOKEN`) and login (`BOT_LOGIN`). Mode 600, never committed. |
| `~/projects/minimaleffort/pokefirered-autodev` | Autodev's own clone, kept apart from the checkout you work in. It pushes as the bot over HTTPS, using `gh` as git's credential helper. |
| `autodev.timer` | A systemd user timer that starts `autodev-dispatcher.service` every 2 minutes. |
| `autodev-dispatcher.service` | One dispatcher tick: `git pull` the clone, then `python3 -m autodev tick` (code in `automation/autodev/`). |
| `autodev-<role>-<issue>.service` | One Claude session, e.g. `autodev-grill-67` or `autodev-ticket-68`. It's a transient unit that the dispatcher starts with `systemd-run`, and systemd forgets it once it ends. It runs `claude -p` as the bot (the env file), with your Claude login and plugins (`CLAUDE_CONFIG_DIR=~/.claude-personal`), the role's permissions and the role's time limit (grill 60 minutes, ticket 3 hours, merge 45 minutes). A grill session runs in the clone; ticket and merge sessions run in the ticket's worktree. |
| `~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-<T>` | Each ticket's worktree (a second checkout of the clone's repository), on its branch `issue-<T>-<slug>`. The ticket's merge session and its fix-up sessions (#59) reuse it, and it is removed once the ticket closes. The worktrees sit beside the clone, not inside it: Claude reads every CLAUDE.md in the folders above where it starts, so a session in a worktree inside the clone would also load the clone's CLAUDE.md. |
| `automation/settings/<role>.json` | Each role's permissions. A session always uses the clone's copy, never its worktree's, because it could edit the one in its worktree. The grill role is read-only. The ticket and fix-up roles may edit, build, test, commit and push, but they can't use `gh`, push to master, force-push, delete branches, run their worktree's own `issue.py`, or touch `pret/pokefirered` or the `upstream` remote. The merge role has the same limits, can't rebase either, and merges only through `issue.py merge-pr`. In auto mode an allow rule only skips the safety classifier, so every limit is a deny rule. The master rulesets are the backstop. |
| `automation/bin/issue.py` | A session's only way to reach GitHub; its settings deny plain `gh`. Sessions run the clone's copy (ticket and merge sessions by its full path), never their worktree's. Every role's settings deny commands that mention `AUTODEV_`, so a session can't point the tool at another issue or PR for one command, and the tool checks a merge session's PR is the bot's PR from that ticket's branch anyway. It shows only text written by you or the bot, and it changes only the session's own issue (`AUTODEV_ISSUE`, set by the dispatcher). Each role gets only its own commands (`AUTODEV_ROLE`): a grill session may comment, rewrite its parent's body and create tickets; a ticket session may comment and open its PR; a merge session may reply on its PR (`AUTODEV_PR`), @mentioning you and naming the `/merge` it answers (`AUTODEV_HANDLED_THROUGH`), and merge it with a merge commit of the commit it tested. No labels, no closing, no board. |
| `~/autodev/logs/` | Each session's full stream-json log (`<role>-<issue>-<time>.jsonl`, with stderr in `.err`). The newest `result` event that carries a report holds what the session reported. A session can log several: one per turn, including an empty one when a background job wakes it after it reported. When the unit ends, systemd writes how it ended into `.exit` (`success`, `exit-code`, or `timeout` at the time limit). The tick deletes logs older than 30 days. |
| `/tmp/autodev-record.lock` | The machine-wide lock `test/record_test.sh` takes, because a recording films the whole virtual display. Sessions recording at the same time take turns. |
| `~/.local/state/autodev/sessions/` | One small record per session the dispatcher started: its session id and log path. Once a session's outcome is handled, its record moves to `done/`, marked `"crashed": true` if it crashed (a second crash parks the work). `paused-until` holds a usage-limit pause. |
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
systemctl --user stop autodev-ticket-68           # stop one running session (pause its card first, or it runs once more)
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
cd ~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-68 && CLAUDE_CONFIG_DIR=~/.claude-personal claude --resume <session-id>
```

The same command also reopens a session that finished normally, which is handy for reviewing what it did; its session id is the `session_id` field of its record in `~/.local/state/autodev/sessions/done/`.

## Tests

```bash
cd automation && python3 -m unittest
```

What they cover:
- **`test_tick.py`:** replays recorded GitHub responses (`tests/fixtures/`) through a fake `gh`, and checks the board snapshot the tick logs, including each ticket's parent and blockers read from its body.
- **`test_decide.py`:** checks every decision (`decide()` is a pure function) against a hand-built state, one test per scenario from #48:
  - parents: D1, D2, and D15, D16 and D24 for grills
  - tickets: D2 (someone else's ticket), D3 (launch), D4 (`Blocked`), D5 (back to `Todo`), D6 (two slots, by priority), D15 (`In review`), D16 (no PR) and D17 (time limit), plus a closed issue never launching
  - merging:
    - D9 (`/merge` launches a merge), D10 (`/merge` from someone else or the bot), D12 (all handled) and D13 (a session already running)
    - feedback after `/merge`, and a `/merge` posted while the last reply was being written
    - D6 (a merge goes first)
    - a finished merge: merged (D21, even if the session then failed), a conflict reply, or neither
  - closing: D22 (closed without a PR), the board's own `Done`, a worktree still in use, and D23 (a parent whose tickets are all closed)
  - pausing: D7 (a paused parent or ticket starts nothing; running sessions finish), D8 (resuming a parent with tickets, or a ticket with a PR), and a paused card staying `Paused` when its session ends
  - usage limits and crashes: D18 (pause everything, re-queue, start again after), D19 (a first crash is re-queued, a vanished unit too) and D20 (a second crash parks it)
- **`test_sessions.py`:**
  - pins down the session unit's command line for a grill, a ticket and a merge: the bot's identity, your Claude login, the working folder, the time limit, the role's permissions, the session's issue and role, a merge's PR and `/merge` comment, and the record of how the unit ended
  - checks what counts as "still running", and how a session's result, end reason and folder are read back
  - reads the usage-limit message in its known forms and works out when the limit resets, and keeps crashes and the pause on disk
  - checks that each role keeps its deny rules (D25) and has no plain `gh` access
- **`test_worktrees.py`:** runs real git in a temporary folder. A ticket's worktree is cut from `origin/master` whatever the clone has checked out, its branch tracks nothing (so a bare `git push` can't aim at master), and `tools/agbcc` is linked. Asking again leaves an existing worktree alone but puts back a missing agbcc link. A branch left by a deleted worktree is checked out again, and one that only GitHub has is checked out from there. Removing deletes the folder and its local branch and is safe to repeat. It also checks branch names and listing which tickets have a worktree.
- **`test_issue_tool.py`:** checks `bin/issue.py`'s rules:
  - untrusted authors' text is withheld, and comments get the marker
  - a ticket must name its parent, and it is created without labels and linked as a sub-issue
  - a PR comes only from the ticket's own branch, into master, with Fixes #<T> and an @mention of you outside code, and closes no other issue
  - a merge is a merge commit of the tested commit, refused while master has commits the branch lacks; a reply on the PR @mentions you and names the comment it handled; both act only on the ticket's own PR
  - each role may use only its own commands
- **`test_github.py`:** checks what the dispatcher accepts as a finished session's work. For a grill: only the bot's comment with the design marker, and only open issues whose `## Parent` names the parent, with their blockers checked. For a ticket: only the bot's open PR from that ticket's branch in this repo. For a merge: whether the PR merged, and whether the bot replied naming that `/merge`. It also reads a PR's comments, reviews and inline comments in time order, and covers deleting session files older than 30 days.
- **Failures:** if one of a tick's actions fails, the tick logs it and skips the rest of that issue's actions, but carries on with the others.

**Live check:** the grill path (#55) was checked live on 2026-10-19, on issue #67: autodev grilled it and broke it into tickets with nobody watching.

**Live check:** fix-ups (#59) were checked live on #80, on 2026-10-19.
