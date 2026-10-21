# Autodev

Autodev runs this repo's GitHub Issue Workflow (CLAUDE.md steps 1–13) on its own. You add an issue to the **pokefirered autodev** board. A headless Claude Code session grills it, taking every recommended answer, and breaks it into tickets. Other sessions implement the tickets and open PRs. Nothing reaches master until you comment `/merge` on the PR, or `/merge breaking` on one that is a breaking change. The full design is on issue #48. For the one-page picture of how autodev, the tests and the recordings fit together, see [docs/SYSTEM_MAP.md](../docs/SYSTEM_MAP.md).

> **Status:** grilling (#55), implementation (#56), merging (#57), pausing and failure handling (#58) and fix-ups (#59) work. An issue you add to the board gets grilled into a design and tickets. Each ticket is implemented in its own worktree as soon as nothing blocks it, ending in a PR that waits for you. Feedback on that PR gets a fix-up; your `/merge` gets it merged, and the ticket and, in the end, the parent are closed. `Paused` stops work; usage limits and crashes are ridden out. The main README's ["Autodev" section](../README.md#autodev) is the short guide, and its demo is the live canary (#83, from #60).

## What happens to an issue you add

1. **On the next tick** (at most 2 minutes), the dispatcher sees your issue on the board with no Status (or `Todo`). An issue with no `## Parent` heading is a **parent**:
   - It starts a grill session for it and moves the card to `Grilling`.
   - At most 2 sessions run at once, of any kind; the rest wait their turn. Merges go first, then fix-ups, then tickets, then parents: finishing work beats starting more.
   - Within merges, within fix-ups and within tickets, the parent grilled first goes first, then the lowest ticket number, so one feature's tickets finish before a later feature's start. Which parent was grilled first is judged by its first ticket's number (open or closed, on the board or among its sub-issues), not its own: issue numbers only go up, so an old issue you add to the board late lines up behind the features already in flight. Only tickets written by you or the bot count.
   - That is a priority, not a limit: a later feature's tickets start whenever the earlier ones have none ready (blocked, running, or in review waiting for you). Running sessions are never stopped. Among parents, the oldest issue goes first, and a parent is grilled whenever a slot is free and nothing above wants it.
   - An issue written by anyone but you is never handed to a session. It goes to `Needs human`, and the bot comments: "Autodev only runs issues written by @MinimalEffort07; restate it as your own issue to run it."
2. **The grill session** follows CLAUDE.md's "Autonomous mode" for the grill role:
   - It grills the issue by itself, taking every recommended answer, and posts the design comment. That comment has a "Least-confident defaults" list and every round in a collapsed `<details>` block.
   - The design says whether it is a breaking change (CLAUDE.md, "Breaking changes"). A breaking one has a `## Breaking change` section (what stops working, what happens to a save made before, why there's no compatible way) and the hidden marker `<!-- autodev:breaking -->` beside `<!-- autodev:design -->`. Any other says "Breaking change: none." Each ticket's acceptance criteria say "Breaking change: …" or "Breaking change: none." too.
   - It rewrites the issue body into a scope summary, then creates the tickets and links each one as a sub-issue.
   - It is read-only on the code and has 60 minutes.
3. **On the first tick after it ends**, the dispatcher checks GitHub: is the design comment there, and is there at least one ticket?
   - **Yes:** each ticket goes on the board, as `Blocked` if an issue under its `## Blocked by` is still open, otherwise `Todo`. The parent moves to `In progress`.
   - **Yes, and the design is a breaking change** (the bot's design comment carries `<!-- autodev:breaking -->` outside code; the marker anywhere else doesn't count): the tickets go on the board the same way, but the parent moves to `Paused` instead, so none of its tickets starts. The bot comments, @mentioning you: "the design for #N is a breaking change (see its Breaking change section), so its tickets wait. Move the card to `Todo` to approve it and start them. To change the plan, edit or close its tickets first." Moving it to `Todo` is the usual resume (see "Pausing, usage limits and crashes"): it goes to `In progress` without a second grill, and its unblocked tickets start. A parent you paused during the grill stays `Paused`, and the comment still posts.
   - **No**, or the session reported a failure, or it hit its time limit: the parent goes to `Needs human`. The bot comments with the reason, @mentions you, and gives the command that resumes the session. A crash or the usage limit re-queues it instead (see "Pausing, usage limits and crashes").

## What happens to a ticket

An issue whose body has a `## Parent` heading is a **ticket**. Grill sessions write them, as the bot; you can write one too. A ticket written by anyone else goes to `Needs human`, like a parent.

1. **It waits for its blockers.** On every tick, a ticket in `Todo` with an open issue under its `## Blocked by` heading moves to `Blocked`. Once all of them are closed, it moves back to `Todo`. On that tick it also holds a free slot, so a later feature's ticket or a grill can't take the slot first, and it starts on the next tick; a ticket whose parent is `Paused`, still being grilled, or waiting to be grilled again holds nothing. A closed issue counts as done whether a PR closed it or you did, so closing a ticket is how you skip it.
2. **A ticket in `Todo` with nothing open blocking it** gets its own worktree, `~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-<T>`, beside the clone. The worktree is on a new branch, `issue-<T>-<slug>`, cut from master, with `tools/agbcc` linked. The slug is the title's first five words, or fewer if five would go over 40 characters. If the folder is gone but the branch is still there, the worktree is made again on that branch. A ticket session starts in it, and the card moves to `In progress`.
3. **The ticket session** follows CLAUDE.md's "Autonomous mode" for the ticket role, steps 7–11:
   - TDD, a clean build and `bash test/run_suite.sh`
   - a recording only if the change makes something on the GBA screen look or behave differently (CLAUDE.md, step 8). A change to tests, tooling, autodev, docs, the recordings page or the recorder, or a new test of behaviour the game already had, gets none, and its PR says "No recording: no in-game behaviour changes." As a backstop, `test/record_test.sh` refuses to record in a session whose branch changes no file the ROM is built from (`src/`, `include/`, `asm/`, `data/`, `graphics/`, `sound/`, `constants/`, `tools/` and the top-level build files), compared with where the branch left master. When a ticket written before this rule asks for a recording it refuses, the session leaves the recording out and says so in the PR, without failing. Recordings take turns (see "How it runs").
   - a recording always has notes: which ticket and feature it's for, what it tests, what the problem was and what you should see. `test/record_test.sh` refuses to record in a session without them, or with notes for another ticket or without the parent.
   - the README and `docs/SYSTEM_MAP.md`, then a code review against master
   - finally `issue.py open-pr`, which only opens a PR from the ticket's own `issue-<T>-*` branch, and only when the body has Fixes #<T>, an @mention of you and a "What changes for you" section, all written outside code (GitHub ignores the first two inside code). A branch that changes `test/save_format.txt` (compared with master on GitHub) also needs a "Breaking change" section with text under it, for you to read before your `/merge breaking` (see "When you comment `/merge`"). The body links each recording's own page (`Recording: http://100.76.129.86:8080/r/<name>`). Once the PR is open, `python3 test/tools/recording_notes.py set-pr <name> <PR>` puts its number into each recording's notes, so the recording's card links the PR.

   It has 3 hours.
4. **On the first tick after it ends**, the dispatcher looks on GitHub for the ticket's open PR: one from an `issue-<T>-*` branch of this repo, opened by the bot or by you (you may finish a parked ticket yourself). A PR from a fork, or opened by anyone else, doesn't count.
   - **Found:** the card moves to `In review`, and the PR waits for you.
   - **Not found**, or the session reported a failure, or it hit its time limit: the card goes to `Needs human`, with the reason and the resume command ("hit its 3-hour time limit" when systemd stopped it). A crash or the usage limit re-queues it instead, unless its PR is already open, which counts as done.

## When you comment `/merge`

1. **On the next tick**, the dispatcher reads everything said on each `In review` ticket's PR: conversation comments, reviews and inline review comments.
   - **What counts as unhandled:** each of autodev's replies ends with a hidden `<!-- autodev handled-through: <id> -->` marker naming the newest of your comments it dealt with. Your comments after that one are unhandled; nobody else's count.
   - **When it merges:** if your newest unhandled comment is exactly `/merge` or exactly `/merge breaking`, a merge session starts and the card moves to `In progress`. Anything else is feedback (see "When you leave feedback"), so feedback you post after `/merge` cancels it.
   - **A breaking change merges only on `/merge breaking`** (CLAUDE.md, "Breaking changes"). A PR counts as one when, compared with master on GitHub, it changes `test/save_format.txt` (added, changed, removed or renamed), or GitHub can't list all of its files (or can't compare it with master at all), or its body or one of the bot's replies or reviews on it has a `## Breaking change` heading outside code. Your own comments don't count.
     - **A plain `/merge` on one** starts no session. Instead the bot replies on the PR, @mentioning you: why the PR is a breaking change, that a plain `/merge` doesn't merge it, and to read its Breaking change section, then comment exactly `/merge breaking` to approve it and merge, or leave feedback. The reply handles that `/merge`, so it gets one reply, and the card stays in `In review`.
     - **`/merge breaking`** starts a merge session exactly as `/merge` does, and it also merges a PR that breaks nothing. Only yours counts, and anything you say after it cancels it, as for `/merge`.
   - **While a session runs:** comments that arrive wait until it ends. Feedback posted while a merge session runs still cancels the `/merge`, and so does deleting the `/merge`: `issue.py merge-pr` refuses to merge, and the card goes back to `In review`, where a fix-up starts for any feedback.
2. **The merge session** runs in the ticket's worktree and follows CLAUDE.md's "Autonomous mode" for the merge role. It has 45 minutes.
   - It brings the branch up to date, merges master in, and builds and runs the unit tests and `run_suite.sh` on that exact tree.
   - **If all of that passes,** it pushes, then merges with `issue.py merge-pr`. That always makes a merge commit (never a squash or rebase) of the commit it tested. It refuses if master has moved on since, so the merged tree is always the tested tree. It also checks again, on that exact commit, whether the PR is a breaking change, and refuses one unless the comment the session answers is your `/merge breaking`: a backstop to the dispatcher's own check.
   - **On a conflict,** it resolves it on the branch, re-tests and pushes. A conflict in `test/save_format.txt` is resolved by running `python3 test/tools/save_format.py --write` on the merged tree. Then it explains on the PR exactly how it resolved each conflict, @mentioning you. It doesn't merge: your fresh `/merge` approves the resolution and starts a new merge session. On a breaking PR the reply says so, and it takes a fresh `/merge breaking`.
3. **On the first tick after it ends:**
   - **Merged** (even if the session failed or timed out afterwards): the ticket is closed (the PR's closing line usually did that already), its worktree and local branch are removed, and the card moves to `Done`.
   - **Conflict reply posted:** the card goes back to `In review` to wait for your `/merge`.
   - **You commented after the `/merge`, or deleted it:** back to `In review`, whatever the session reported, so a fix-up picks your comments up.
   - **`merge-pr` refused a plain `/merge` on a breaking change:** back to `In review`, whatever the session reported. That `/merge` is still unhandled, so the next tick's reply explains.
   - **Neither,** or the session reported a failure, or it hit its time limit: `Needs human`, with the reason and the resume command. A crash or the usage limit sends it back to `In review` instead, unless its conflict reply is already posted, which counts as done.
4. **Closed tickets are tidied up whoever closed them.** A ticket you close yourself, or whose PR you merge yourself, also loses its worktree, and its card moves to `Done`. The tickets it blocked move on to `Todo`.
   - **Closed cards leave the board.** The board's "Auto-archive items" workflow archives a card within seconds of its issue closing, usually before the next tick, and an archived card isn't read. So autodev doesn't rely on seeing one: it removes the worktree of any ticket that's off the board and closed on GitHub, and it leaves a gone card's Status alone (GitHub won't change an archived card, even one archived a moment ago, as the tick closed its issue).
5. **Once every ticket of one of your parents is closed,** the bot closes the parent, comments on it @mentioning you, and moves it to `Done`. A parent's tickets are the issues whose `## Parent` names it, found among its sub-issues on GitHub (the grill links each ticket as one) and on the board. The board alone can't say, since closed tickets' cards are archived.

## When you leave feedback

1. **On the next tick,** if your newest unhandled comment, review or inline comment on an `In review` ticket's PR is anything but `/merge`, a fix-up session starts in the ticket's worktree, and the card moves to `In progress`. It handles every comment of yours queued up to that one, all together. It has 90 minutes. Inline comments you make in a review count from when you submit the review, since nobody sees them before.
2. **The fix-up session** follows CLAUDE.md's "Autonomous mode" for the fix-up role:
   - `issue.py read <PR>` shows it the PR's comments, reviews and inline comments (each inline one with its file and line), and marks your unhandled ones.
   - It brings the branch up to date (you may have pushed to it), then makes the changes with TDD, builds, runs `run_suite.sh`, and records again only if the change shows in the game (as for a ticket, `test/record_test.sh` refuses a branch that changes nothing in the game). A fresh recording has notes too, and gets the PR's number by itself.
   - It pushes, then replies on the PR with `issue.py pr-comment`, @mentioning you and saying what it changed for each comment. The reply's handled-through marker names the comment it was started for. A question needs no change: the reply answers it.
   - A fix-up that makes the PR a breaking change gives its reply a `## Breaking change` section, since it can't edit the PR's body. From then on the PR merges only on your `/merge breaking`.
3. **On the first tick after it ends:**
   - **Reply posted:** the card goes back to `In review`. Commits aren't required, since a question only needs an answer; the reply is what you read before your next `/merge`.
   - **No reply,** or the session reported a failure, or it hit its time limit: `Needs human`, with the reason and the resume command. A crash or the usage limit sends it back to `In review` instead, where your comment is still unhandled, unless its reply is already posted, which counts as done.
4. **A comment you post while the fix-up runs** isn't covered by its reply, so a follow-up fix-up starts for it once the card is back in `In review`.

## When a ticket is parked

A ticket in `Needs human` is left alone until you've dealt with it. Then it comes back by itself, on the next tick:

- **Its PR is opened** after it was parked, by the resumed session or by you: the card moves to `In review`.
- **You comment on its PR** after it was parked: your newest unhandled comment there is answered as in review, so `/merge` starts a merge session and anything else a fix-up. A plain `/merge` on a breaking change gets the bot's reply instead, and the card moves to `In review`.
- **"After it was parked"** means after the bot's newest "autodev parked this issue" comment on the ticket. What came before is what it was parked over (the feedback a failed fix-up was working on, say), so it never starts the same work again.
- **Not these:** a card you moved to `Needs human` yourself (there's no parked comment), and someone else's ticket, which stays parked for its author.
- **Moving the card back to `Todo`** still works too, as from `Paused`.

## Pausing, usage limits and crashes

- **`Paused` is your stop switch.**
  - **What it stops:** on a parent, none of its tickets start, neither implementation, fix-up nor merge. On a ticket, that ticket doesn't start.
  - **Sessions already running** finish their run. When one ends, its comments still post, but a paused card stays `Paused`.
  - **Resuming:** move the card back to `Todo`, and it is picked up on the next tick.
    - A parent that already has tickets goes back to `In progress`; it isn't grilled again. That's also how you approve a breaking design, which autodev parks in `Paused` itself (see "What happens to an issue you add"). Its tickets count even if their cards have left the board. If none is on the board and GitHub can't list its sub-issues that tick, it waits for the next tick rather than risk a second grill.
    - A ticket whose PR is already open goes back to `In review`; it isn't implemented again.
  - **Stopping a session for good:** pause its card (or its parent), then `systemctl --user stop` its unit. A stop on its own counts as a crash, so the work would run once more.
- **The usage limit:** a session that stops on the Claude usage-limit message ("You've hit your … limit · resets 3pm" and similar) pauses everything.
  - No session of any role starts until the reset time plus 5 minutes, worked out from when the session stopped. The board is still kept up to date, and each tick logs `usage limit: no session starts until …`.
  - The bot comments "paused until <time>" on the issue, @mentioning you. Only an error result or stderr is read for the message, never the session's own words.
  - The interrupted work goes back to `Todo`, or a merge or fix-up back to `In review` with your comment still unhandled. It starts again once the pause is over, as a fresh session that carries on from its branch.
  - **Unreadable time:** if the message's time can't be read, the pause lasts an hour.
  - **Where it's kept:** the pause is in `~/.local/state/autodev/sessions/paused-until`, so it survives reboots. Delete that file to lift it early.
- **Crashes:** a session that ends with no report, no usage-limit message and not at its time limit has crashed. That includes one killed or stopped, and one whose unit vanished in a reboot.
  - **The first time:** the work is re-queued (`Todo`, or `In review` for a merge or fix-up), and the bot notes it on the issue. A re-queued grill is grilled again, even though the tickets it had already created are on the board (the board adds sub-issues as they are made). Those tickets wait while their parent is `Grilling`.
  - **Work already done counts:** a ticket session whose PR is open, or a merge or fix-up session whose reply is posted, is done however it ended.
  - **The second time in a row** for the same role and issue: `Needs human`, with the reason and the resume command. A run that finishes in between starts the count again, since a PR gets a fresh fix-up for each piece of feedback.
  - **Time limit:** a session that hit its time limit goes to `Needs human` straight away.

## How it runs

| Piece | What it is |
|---|---|
| Bot account | A separate GitHub account. Autodev reads and writes as the bot, never as you, so its PRs and comments notify you, and its token can only reach this repo. |
| `~/.config/autodev/env` | The bot's token (`GH_TOKEN`) and login (`BOT_LOGIN`). Mode 600, never committed. |
| `~/projects/minimaleffort/pokefirered-autodev` | Autodev's own clone, kept apart from the checkout you work in. It pushes as the bot over HTTPS, using `gh` as git's credential helper. |
| `autodev.timer` | A systemd user timer that starts `autodev-dispatcher.service` every 2 minutes. |
| `autodev-dispatcher.service` | One dispatcher tick: `git pull` the clone, then `python3 -m autodev tick` (code in `automation/autodev/`), which restarts the recordings page if a merge changed it, then works the board. |
| `autodev-<role>-<issue>.service` | One Claude session, e.g. `autodev-grill-67` or `autodev-ticket-68`. It's a transient unit that the dispatcher starts with `systemd-run`, and systemd forgets it once it ends. It runs `claude -p` as the bot (the env file), with your Claude login and plugins (`CLAUDE_CONFIG_DIR=~/.claude-personal`), the role's permissions and the role's time limit (grill 60 minutes, ticket 3 hours, merge 45 minutes, fix-up 90 minutes). The model is `config.json`'s `model` (`opus`, the latest Opus). The effort level is set per role in `sessions.py`, so your own default never applies: grill `max`; ticket, merge and fix-up `xhigh`. A grill session runs in the clone; ticket, merge and fix-up sessions run in the ticket's worktree. |
| `~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-<T>` | Each ticket's worktree (a second checkout of the clone's repository), on its branch `issue-<T>-<slug>`. The ticket's merge and fix-up sessions reuse it, and it is removed once the ticket closes. The worktrees sit beside the clone, not inside it: Claude reads every CLAUDE.md in the folders above where it starts, so a session in a worktree inside the clone would also load the clone's CLAUDE.md. |
| `automation/settings/<role>.json` | Each role's permissions. A session always uses the clone's copy, never its worktree's, because it could edit the one in its worktree. The grill role is read-only. The ticket and fix-up roles may edit, build, test, commit and push, but they can't use `gh`, push to master, force-push, delete branches, run their worktree's own `issue.py`, or touch `pret/pokefirered` or the `upstream` remote. The fix-up role may also `git pull --ff-only` but can't rebase or merge master in. The merge role has the same limits, may merge master in, and merges only through `issue.py merge-pr`. In auto mode an allow rule only skips the safety classifier, so every limit is a deny rule. The master rulesets are the backstop. |
| `automation/bin/issue.py` | A session's only way to reach GitHub; its settings deny plain `gh`. Sessions run the clone's copy (ticket, merge and fix-up sessions by its full path), never their worktree's. Every role's settings deny commands that mention `AUTODEV_`, so a session can't point the tool at another issue or PR for one command, and the tool checks a merge or fix-up session's PR is that ticket's PR (the bot's or yours, from its branch) anyway. It shows only text written by you or the bot, and it changes only the session's own issue (`AUTODEV_ISSUE`, set by the dispatcher). Reading a PR shows its reviews and inline comments too, and marks your unhandled comments. Each role gets only its own commands (`AUTODEV_ROLE`): a grill session may comment, rewrite its parent's body and create tickets; a ticket session may comment and open its PR; a merge or fix-up session may reply on its PR (`AUTODEV_PR`), @mentioning you and naming the newest comment it answers (`AUTODEV_HANDLED_THROUGH`); a merge session may also merge it, with a merge commit of the commit it tested, unless you have commented since the `/merge` or deleted it, and a breaking change only on your `/merge breaking`. A ticket session's PR whose branch changes `test/save_format.txt` needs a `## Breaking change` section. No labels, no closing, no board. |
| `~/autodev/logs/` | Each session's full stream-json log (`<role>-<issue>-<time>.jsonl`, with stderr in `.err`). The newest `result` event that carries a report holds what the session reported. A session can log several: one per turn, including an empty one when a background job wakes it after it reported. When the unit ends, systemd writes how it ended into `.exit` (`success`, `exit-code`, or `timeout` at the time limit). The tick deletes logs older than 30 days. |
| `/tmp/autodev-record.lock` | The machine-wide lock `test/record_test.sh` takes, because a recording films the whole virtual display. Sessions recording at the same time take turns. |
| `~/.local/state/autodev/sessions/` | One small record per session the dispatcher started: its session id and log path. Once a session's outcome is handled, its record moves to `done/`, marked `"crashed": true` if it crashed (a second crash in a row parks the work; those records are kept past the 30-day clean-up while they still count). `paused-until` holds a usage-limit pause. |
| `autodev-recordings.service` | `test/serve_recordings.py` on port 8080, so the recording links in PRs always work. It runs from the clone and keeps running, so on its own it would keep the code it started with. Each tick, before reading the board, checks it: if it's running and the clone's `test/serve_recordings.py` was modified after it started (`systemctl --user show` gives the start time), the tick runs `systemctl --user try-restart autodev-recordings.service` and logs `restarted the recordings server: …`. A merged change to the page then shows within about 2 minutes. A service you stopped stays stopped (`try-restart` leaves a stopped unit alone), and runs the new code when it next starts. If `systemctl` fails, the tick logs one error line and exits 1, but still does the board's work. The server must stay one file that imports only Python's standard library, because that file is all the check watches (code in `automation/autodev/recordings.py`). |
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

Before the snapshot, a tick that restarts the recordings page logs `restarted the recordings server: test/serve_recordings.py changed since it started`. After the snapshot, the tick logs each action it takes, e.g. `→ Launch(role='grill', issue=61)` and then `→ SetStatus(issue=61, status='Grilling')`. For a ticket, the worktree comes first: `→ MakeWorktree(issue=68, branch='issue-68-note-the-grill-paths-live')`, then the launch and the move to `In progress`.

### Resuming a parked session

A `Needs human` comment ends with a command like this one, which reopens the session interactively exactly where it stopped. `claude --resume` only finds a session from the folder it ran in, so the command starts in that folder: the clone for a grill session, the ticket's worktree for the others.

```bash
cd ~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-68 && (set -a && . ~/.config/autodev/env && set +a && AUTODEV_ROLE=ticket AUTODEV_ISSUE=68 CLAUDE_CONFIG_DIR=~/.claude-personal claude --resume <session-id>)
```

The resumed session needs what the dispatcher gave it, or `issue.py` treats it as nobody's and only lets it read: the bot's token (`GH_TOKEN`, from the env file, so `gh` and `git` act as the bot) and its `AUTODEV_ROLE` and `AUTODEV_ISSUE`, plus a merge or fix-up's `AUTODEV_PR` and `AUTODEV_HANDLED_THROUGH`. The command sets them inside the brackets, a subshell, so none of them stays in your own shell. `config.json`'s `resume_command` is the template: `{cwd}`, `{env}` (the `AUTODEV_*` assignments) and `{session_id}` are filled in.

The same command also reopens a session that finished normally, which is handy for reviewing what it did; its session id is the `session_id` field of its record in `~/.local/state/autodev/sessions/done/`.

## Tests

```bash
cd automation && python3 -m unittest
```

What they cover:
- **`test_tick.py`:** replays recorded GitHub responses (`tests/fixtures/`) through a fake `gh`, and checks the board snapshot the tick logs, including each ticket's parent and blockers read from its body. It also checks what the tick reads for each ticket: its PR (with what was said on it, unless the ticket is in `Todo`), and when a parked one with a PR was parked. And it checks that the tick reads the sub-issues of your open parents in `Todo` or `In progress` (one it can't read is logged and left out), and asks GitHub whether a ticket is closed when the ticket still has a worktree but its card is off the board.
- **`test_decide.py`:** checks every decision (`decide()` is a pure function) against a hand-built state, one test per scenario from #48 (and from #141 and #143, whose tests say so):
  - parents: D1, D2, and D15, D16 and D24 for grills
  - tickets: D2 (someone else's ticket), D3 (launch), D4 (`Blocked`), D5 (back to `Todo`), D6 (two slots, by priority), D15 (`In review`), D16 (no PR) and D17 (time limit), plus a closed issue never launching
  - merging:
    - D9 (`/merge` launches a merge), D10 (`/merge` from someone else or the bot), D12 (all handled) and D13 (a session already running)
    - a `/merge` posted while the last reply was being written
    - D6 (a merge goes first)
    - a finished merge: merged (D21, even if the session then failed), a conflict reply, your feedback since the `/merge` (back to review), or neither
  - breaking changes (#141): B1 (a plain `/merge` on a breaking PR gets one reply, @mentioning you, and no session), B2 (that reply handles it), B3 (`/merge breaking` launches a merge), B4 (on a PR that breaks nothing, either one does), only the exact words approve, B5 (feedback after the reply gets a fix-up), B6 (a parked ticket gets the reply and moves to `In review`), B7 (`/merge breaking` from anyone else or the bot) and B8 (a merge `merge-pr` refused for want of `/merge breaking` goes back to `In review`)
  - fix-ups: D11 (feedback launches one), D13 (a comment posted while one runs waits, then gets a follow-up), D14 (feedback after `/merge`), feedback from anyone else, the priority between merge, fix-up and ticket, and a finished fix-up (D15 with its reply, no reply, a crash before or after replying, the usage limit, the time limit)
  - closing: D22 (closed without a PR), the board's own `Done`, a worktree still in use, a closed ticket whose card has left the board (its worktree goes, and its merge session still finishes), and D23 (a parent whose tickets are all closed, counted from its sub-issues, so it still closes after the board archives every card; it waits if they couldn't be read)
  - pausing: D7 (a paused parent or ticket starts nothing; running sessions finish), D8 (resuming a parent with tickets, including one whose tickets' cards have all left the board, and waiting when that can't be told, or a ticket with a PR), and a paused card staying `Paused` when its session ends
  - breaking designs, #143's scenarios: D2 (a grill whose design is a breaking change places its tickets as usual, moves the parent to `Paused` and comments, @mentioning you, and none of its tickets starts on that tick, even when its card never said `Grilling`), D3 (moving it to `Todo` sends it to `In progress` and starts its unblocked tickets), D4 (a design without the marker goes to `In progress`) and D5 (a parent you paused during the grill stays `Paused`, and the comment posts)
  - usage limits and crashes: D18 (pause everything, re-queue, start again after), D19 (a first crash is re-queued, a vanished unit too) and D20 (a second crash parks it)
  - parked tickets: your `/merge` or feedback on the PR since it was parked, or a PR opened since, brings it back; what came before doesn't, nor does a card with no parked comment or someone else's ticket
  - resume commands: a parked comment's command sets the session's `AUTODEV_*` variables, and `config.json`'s loads the bot's token in a subshell
- **`test_sessions.py`:**
  - pins down the session unit's command line for a grill, a ticket, a merge and a fix-up: the bot's identity, your Claude login, the working folder, the time limit, the effort level, the role's permissions, the session's issue and role, a merge or fix-up's PR and comment, and the record of how the unit ended
  - checks every role has its prompt, its settings and a way to read its outcome
  - checks what counts as "still running", and how a session's result, end reason and folder are read back
  - reads the usage-limit message in its known forms and works out when the limit resets, and keeps crashes and the pause on disk; crashes count in a row, and a finished run starts the count again
  - checks that each role keeps its deny rules (D25) and has no plain `gh` access, and that a fix-up may pull but not rebase
- **`test_worktrees.py`:** runs real git in a temporary folder. A ticket's worktree is cut from `origin/master` whatever the clone has checked out, its branch tracks nothing (so a bare `git push` can't aim at master), and `tools/agbcc` is linked. Asking again leaves an existing worktree alone but puts back a missing agbcc link. A branch left by a deleted worktree is checked out again, and one that only GitHub has is checked out from there. Removing deletes the folder and its local branch and is safe to repeat. It also checks branch names and listing which tickets have a worktree.
- **`test_issue_tool.py`:** checks `bin/issue.py`'s rules:
  - untrusted authors' text is withheld, and comments get the marker
  - reading a PR shows its reviews and inline comments and marks your unhandled ones
  - a ticket must name its parent, and it is created without labels and linked as a sub-issue
  - a PR comes only from the ticket's own branch, into master, with Fixes #<T>, an @mention of you and a "What changes for you" section outside code, and closes no other issue; a branch that changes `test/save_format.txt` (even among the 300 files GitHub lists of a bigger one) also needs a "Breaking change" section with text under it, outside code, and any PR may have one (B10)
  - a merge is a merge commit of the tested commit, refused while master has commits the branch lacks or once you've commented since the `/merge` or deleted it, and, for a breaking change, unless it answers your `/merge breaking` (B11); a reply on the PR @mentions you and names the comment it handled; both act only on the ticket's own PR, the bot's or yours
  - each role may use only its own commands
- **`test_github.py`:** checks which of a parent's sub-issues count as its tickets (only those whose `## Parent` names it first), that setting the Status of a card the board has just archived is skipped rather than failing, and what the dispatcher accepts as a finished session's work. For a grill: only the bot's comment with the design marker, whether that comment also carries the breaking marker outside code (#143's D1: nobody else's comment counts, nor any other of the bot's, and a stray backtick can't hide it), and only open issues whose `## Parent` names the parent, with their blockers checked. For a ticket: only an open PR from that ticket's branch in this repo, opened by the bot or by you, and when it was opened; and when a ticket was last parked (the bot's newest parked comment). For a merge: whether the PR merged, whether the bot replied naming that `/merge`, whether you commented since or deleted it, and whether it is a breaking change answered with a plain `/merge`. Which ticket PRs are breaking changes, and why (B9): one that changes `test/save_format.txt` in any way, one whose files GitHub can't all list or compare at all, and one whose body or a bot reply or review has a `## Breaking change` heading, but not your own comment or a heading in code. For a fix-up: whether the bot replied naming the comment it was started for. It also reads a PR's comments, reviews and inline comments in time order (an inline comment with its file and line, dated by when its review was submitted), and covers deleting session files older than 30 days.
- **Failures:** if one of a tick's actions fails, the tick logs it and skips the rest of that issue's actions, but carries on with the others.

**Live check:** the grill path (#55) was checked live on 2026-10-19, on issue #67: autodev grilled it and broke it into tickets with nobody watching.
