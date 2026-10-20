# System map

How this repo's machinery fits together: autodev, the tests, the recordings, and one line on every script. Read this first when you've lost track; the details live in the files it links to.

**In one paragraph.** You write an issue. A timer on your laptop wakes a dispatcher every 2 minutes, and the dispatcher hands the issue to headless Claude sessions that act as a bot account. They design it, split it into tickets, build each ticket in its own folder, prove it in an emulator, film it, and open a PR. Nothing reaches master until you comment `/merge`.

**Keeping it true.** Every PR that adds, removes or renames a script, or changes what a script, test runner, workflow step or autodev does, updates this map in the same PR (CLAUDE.md, "System map"). `test/tests/test_system_map.lua` fails when a script is missing from the map or the map names a file that's gone. In the diagrams, red marks a step only you take.

## The four parts

| Part | Where | What it is |
|---|---|---|
| The game | `src/`, `data/`, `include/` | The FireRed decompilation plus the fork's features (the generated Mt. Moon, the walking Pokémon, multiplayer and the rest; see `README.md`). `make firered` builds `pokefirered.gba`. |
| The tests | `test/` | Lua scripts drive a patched mGBA emulator: they press buttons, read game memory and assert. `test/run_suite.sh` must pass before anything merges. |
| The recordings | `test/record_test.sh`, `gifs/` | Films a test to video so you can watch what a PR changed, and turns demo frames into the README's GIFs. |
| Autodev | `automation/` | A small Python dispatcher plus headless Claude sessions. Together they run CLAUDE.md's GitHub Issue Workflow from issue to merged PR. |

## The loop

```mermaid
flowchart TB
  subgraph you["You"]
    direction LR
    Y1["Add an issue<br/>to the board"]
    Y2["Skim the design<br/>least-confident defaults"]
    Y3["Review the PR<br/>diff, video, README"]
    Y4["/merge or feedback<br/>nothing merges without it"]
  end
  subgraph gh["GitHub"]
    direction LR
    G1["Project board<br/>a card per issue"]
    G2["Issue + tickets<br/>design comment"]
    G3["Pull request<br/>closes the ticket, mentions you"]
  end
  subgraph laptop["Your laptop (WSL), acting as the bot"]
    direction LR
    A["autodev.timer<br/>every 2 minutes"]
    B["Dispatcher tick<br/>read, decide, act"]
    C["Claude session<br/>claude -p, one role, at most 2 at once"]
    D["Ticket worktree<br/>build, test, record"]
    E["Recordings server<br/>port 8080"]
  end
  Y1 -->|adds card| G1
  A -->|fires| B
  B <-->|"reads board + PR comments, sets Status"| G1
  B -->|starts| C
  C -->|"issue.py, its only door"| G2
  C -->|runs in| D
  D -->|"git push, open-pr"| G3
  D -->|mp4| E
  E -.->|video link| G3
  G2 -->|you read it| Y2
  G3 -->|notifies you| Y3
  Y3 --> Y4
  Y4 -->|comments| G3
  classDef owner stroke:#c22a35,stroke-width:2px
  class Y1,Y2,Y3,Y4 owner
```

The dispatcher never does the work itself. Each tick it reads the board and the PR comments, decides what should happen next, and starts a Claude session for it. The session does the work and reaches GitHub only through `automation/bin/issue.py`. Your comment on a PR is read on the next tick, at most 2 minutes later.

**Where you come in:**

1. Write an issue and add it to the **pokefirered autodev** board.
2. Skim the design comment the grill session posts, starting with its **Least-confident defaults** list. Set the card to `Paused` if you want to stop work and rethink.
3. Review each PR: the diff, its **What changes for you** section, the recording link and the branch README.
4. Comment exactly `/merge` to approve it. Any other comment is feedback, and a fix-up session handles it.
5. Rescue anything in `Needs human`. The bot's comment says what went wrong and ends with a command that reopens that session where it stopped.

**Three facts that explain most surprises:**

- **CLAUDE.md is the program.** Each session's prompt in `automation/prompts/` is a few lines that say "follow CLAUDE.md, Autonomous mode". What a session actually does comes from CLAUDE.md's GitHub Issue Workflow, so editing `CLAUDE.md` changes how the AI behaves.
- **Autodev updates itself.** Every tick starts with `git pull` on autodev's own clone, so a change merged into `automation/` or `CLAUDE.md` is live within about 2 minutes, with no restart.
- **Only your words get in.** Sessions see text written by you or the bot and nothing else, because the repo is public and a stranger's comment could carry instructions. Only your exact `/merge` merges.

## Board columns

```mermaid
flowchart LR
  subgraph parent["Parent issue: yours"]
    direction LR
    PT["Todo"] -->|next tick| PG["Grilling<br/>grill, 60 min"] -->|tickets made| PI["In progress<br/>tickets in flight"] -->|every ticket closed| PD["Done"]
  end
  subgraph ticket["Ticket: written by the grill"]
    direction LR
    TB["Blocked"] -->|unblocked| TT["Todo"] -->|worktree made| TI["In progress<br/>ticket, 3 h"] -->|PR opened| TR["In review<br/>waits for you"]
    TR -->|"/merge"| TM["In progress<br/>merge, 45 min"] -->|merged| TD["Done"]
    TM -->|"conflict: waits for a new /merge"| TR
    TR -->|feedback| TF["In progress<br/>fix-up, 90 min"] -->|reply posted| TR
    TI -.->|fails or times out| NH["Needs human"]
    NH -.->|back once its PR is open| TR
  end
  PG -.->|places tickets| TT
  classDef owner stroke:#c22a35,stroke-width:2px
  class TR owner
```

A parent you write is grilled into tickets. Each ticket waits for its blockers, gets its own session, and ends in a PR that waits for you in `In review`. Feedback starts a fix-up; `/merge` starts a merge. A merge or fix-up session that fails parks its ticket the same way a ticket session does. The rules are in `automation/autodev/decide.py`.

- **`Paused` is your stop switch.** Nothing new starts for that card, or for any ticket of a paused parent. A session already running finishes its run. Move the card back to `Todo` to carry on: a parent with tickets isn't grilled again, and a ticket with an open PR goes straight to review.
- **Usage limit.** When a session hits the Claude usage limit, no session of any kind starts until 5 minutes after the reset, and the interrupted work is queued again.
- **Crashes.** The first crash re-queues the work. A second crash in a row for the same role and issue parks it in `Needs human`.
- **Closing a ticket yourself** counts as done: its worktree is removed and the tickets it blocked move on.

## Session roles

| Role | Starts when | What it does | Runs in | Limit, effort |
|---|---|---|---|---|
| grill | Your new issue (no `## Parent` heading) is in `Todo` | CLAUDE.md steps 1–4. It grills the idea on its own, taking its own recommended answer each round, posts the design with a Least-confident defaults list, rewrites the issue body as a scope summary and creates the tickets. It can't edit code. | autodev's clone | 60 min, max |
| ticket | A ticket is in `Todo` and nothing it's blocked by is open | Steps 7–11: test first, clean build, full suite, recording, README and this map, code review, then a PR that @mentions you. | its own worktree | 3 h, xhigh |
| fix-up | Your newest unhandled PR comment is anything but `/merge` | Handles all your unhandled comments at once, runs the tests again, pushes, and replies on the PR saying what it did about each one. | the ticket's worktree | 90 min, xhigh |
| merge | Your newest unhandled PR comment is exactly `/merge` | Merges master in, builds, runs the unit tests and the suite on that tree, then merges with a merge commit. On a conflict it resolves it, explains on the PR, and waits for a fresh `/merge`. | the ticket's worktree | 45 min, xhigh |

At most 2 sessions run at once, all on Opus (`automation/config.json`). When more are waiting, merges go first, then fix-ups, then tickets, then new issues, so finishing work beats starting more. Time limits and effort levels are set in `automation/autodev/sessions.py`.

## Guardrails

| Guard | What it does |
|---|---|
| Bot account | Everything autodev does on GitHub is done as **MinimalEffort07-Autodev**. Its token lives in `~/.config/autodev/env` and reaches only this repo. Its PRs and comments notify you like anyone else's. |
| `automation/bin/issue.py` | The only way a session reaches GitHub; plain `gh` is denied. It hides text from anyone but you and the bot, acts only on the session's own issue, and gives each role only the commands it needs. |
| Deny rules per role | `automation/settings/<role>.json`: no pushing to master, no force-push, no deleting branches, nothing on `pret/pokefirered`. Sessions load the clone's copy, so a session can't loosen its own rules by editing its worktree. |
| Master rulesets | GitHub itself blocks force-pushes and deleting master, and the bot can change master only by merging a PR. |
| Your `/merge` | It counts only while it's your last word on the PR. The merge is always of the exact commit that passed the suite after master was merged in; if master moves on meanwhile, the merge is refused. |

## How a change is proven

```mermaid
flowchart LR
  ROM["pokefirered.gba<br/>make firered"]
  ROM --> RT["run_test.sh<br/>one test"] --> HL["mgba-headless<br/>patched mGBA runs the .lua"] --> OUT["/tmp/mgba-test.XXXXXX<br/>results JSON, screenshots"] --> EX["exit 0 / 1 / 124<br/>pass / fail / timed out"]
  ROM --> RS["run_suite.sh<br/>the gate before every merge"] --> X4["run_test.sh, 4 at a time<br/>1500 s limit each"] --> SUM["summary<br/>exit 0 only if every test passed"]
  ROM --> RC["record_test.sh<br/>film one test"] --> QT["mgba-qt on Xvfb :1<br/>ffmpeg records the screen"] --> MP4["/tmp/mgba-recordings<br/>then ~/recordings"] --> SRV["port 8080<br/>linked from the PR"]
  ROM --> DM["a demo test<br/>run with MGBA_FRAME_DIR"] --> FC["frame_capture.lua<br/>PNG frames + captions"] --> CG["captioned_gif.py<br/>2x scale, caption bar"] --> GIF["gifs/<br/>shown in the README"]
```

All four paths start from the same ROM. The top two prove the change and need no screen, so they run alongside anything. The bottom two make pictures for a person: a video for each PR, and GIFs for the README. `README.md`'s "Tests" section has the rules in full.

- **A test passes only by proving something.** Zero assertions counts as a fail. Demos and probes say so with `fw.allow_no_assertions`, and the suite skips them: `test/suite_exclude.txt` lists each skipped test with its reason.
- **Each run gets its own folder** (`/tmp/mgba-test.XXXXXX`), so several worktrees can test at the same time. The suite deletes the folders of passing runs and keeps failures with their `run_test.log`.
- **Recordings take turns.** `test/record_test.sh` films the whole virtual display, so it holds a machine-wide lock (`/tmp/autodev-record.lock`) and a second recording waits.
- **The tests need the patched mGBA** in `~/projects/minimaleffort/mgba/build` (a screenshot fix and a fast-forward scripting API). Stock mGBA won't do.
- **Autodev has its own unit tests** in `automation/tests/`, run with `cd automation && python3 -m unittest`. Most of them pin down the rules in `automation/autodev/decide.py`.

## Every script

Each script, by its path from the repo root. `test/tests/test_system_map.lua` checks that this list is complete.

**Autodev** (`automation/README.md` has every rule):

| Script | What it does |
|---|---|
| `automation/setup.sh` | Sets up everything a script can: the board, the bot's access, the master rulesets, autodev's clone, the systemd units. Safe to run again; it ends "setup: no changes" when nothing is left to do. |
| `automation/wizard.sh` | Walks you through what only a person can do: the bot account and token, `gh`'s project scope, the Windows logon task that keeps WSL awake. |
| `automation/config.json` | Names and paths: owner, bot, board title, the 2-session cap, model, clone, log and state folders. |
| `automation/autodev/__main__.py` | The entry point the timer runs: `python3 -m autodev tick`. |
| `automation/autodev/dispatcher.py` | **One tick.** Gathers the board and what finished sessions left on GitHub, asks `automation/autodev/decide.py` what to do, carries it out, logs it to the journal. |
| `automation/autodev/decide.py` | **The rules,** as one pure function: state in, list of actions out. Priority, blocking, `/merge`, crashes and usage limits all live here. |
| `automation/autodev/board.py` | Reads the Projects board through GitHub's GraphQL API. |
| `automation/autodev/github.py` | Carries out actions on GitHub and checks whether a finished session really left its design, tickets, PR or reply. |
| `automation/autodev/gh.py` | The GitHub adapter: runs `gh api` as a subprocess, with the bot's token. |
| `automation/autodev/sessions.py` | Starts each `claude -p` run in its own systemd unit with the role's time limit and effort, then reads back its result and any usage-limit message. |
| `automation/autodev/registry.py` | The dispatcher's memory between ticks: which sessions it started, crash counts, the usage-limit pause. |
| `automation/autodev/worktrees.py` | Makes and removes each ticket's worktree and branch, with `tools/agbcc` linked. |
| `automation/autodev/config.py` | Loads `automation/config.json`. |
| `automation/bin/issue.py` | The sessions' only way to reach GitHub. |
| `automation/autodev/issue_tool.py` | The trust and per-role rules `automation/bin/issue.py` enforces. |
| `automation/prompts/<role>.md` | The few lines each session starts with. They point at CLAUDE.md. |
| `automation/settings/<role>.json` | Each role's permissions, written as deny rules. |
| `automation/tests/` | The unit tests, with fakes for GitHub and systemd. |

**Tests and media** (`README.md`, "Tests"):

| Script | What it does |
|---|---|
| `test/run_test.sh` | Runs one Lua test in headless mGBA. Exit 0 pass, 1 fail, 124 timed out. |
| `test/run_suite.sh` | Runs every top-level test, 4 at a time, in about 10 minutes. Must pass before a merge. |
| `test/suite_exclude.txt` | The tests the suite skips, each with a one-line reason. The suite refuses to start on a line without one. |
| `test/record_test.sh` | Runs a test in the mGBA window on a virtual display and films it to mp4. |
| `test/serve_recordings.py` | A web page of the recordings on port 8080, run by `autodev-recordings.service`. |
| `test/mgba-qt.sh` | Opens the patched mGBA window by hand. |
| `test/lib/framework.lua` | The test framework: press buttons, wait frames, assert, write results. Every test loads it. |
| `test/lib/addresses.lua` | Where game variables sit in memory, for the older tests that hard-code addresses. |
| `test/lib/symbols.lua` | Looks addresses up by symbol name in the built `pokefirered.elf`, so tests survive the addresses moving. |
| `test/lib/walk.lua` | Walks the player somewhere for real: plans a route over the loaded map and presses the D-pad one tile at a time. |
| `test/lib/warp.lua` | Warps the player straight to a map, for tests and tools that don't need the walk. |
| `test/lib/character_select.lua` | Boots a new game through Oak's speech to the character list. |
| `test/lib/frame_capture.lua` | With `MGBA_FRAME_DIR` set, saves every second frame and its caption for a GIF. |
| `test/lib/follower.lua` | The walking Pokémon harness: a save state with it walking, where it stands, and the per-frame walkable-tile rule. |
| `test/lib/poke_radar.lua` | Reads and writes the Poké Radar chain state, and plays the radar. |
| `test/lib/route14_battle.lua` | Starts the Kiri and Jan double battle on Route 14, for the type-colour tests. |
| `test/lib/mt_moon.lua` | The Mt. Moon harness: enter a floor with a chosen random state, read back the generated cave, search it, save and Continue. |
| `test/lib/mt_moon_chain.lua` | Walks the whole Mt. Moon chain on foot, east or west. |
| `test/lib/mt_moon_chamber.lua` | B2F's fossil chamber as the tests see it. |
| `test/lib/mt_moon_fossil.lua` | Takes a fossil in the chamber: the flow both fossil scenarios share. |
| `test/lib/mt_moon_tiles.lua` | Compares a generated floor's tiles with vanilla's, for the look and density checks. |
| `test/tests/` | The tests and demos. `test/tests/mt_moon_checks/` holds the Mt. Moon property checks; `suite_fixtures/` and `framework_fixtures/` test the runners themselves. |
| `test/demos/` | A demo kept outside the suite. |
| `test/tools/captioned_gif.py` | Turns a demo's captured frames into a captioned README GIF. |
| `test/tools/render_cave.py` | Draws a map, vanilla or a dumped generated cave, to PNG. |
| `test/tools/dump_mtmoon_grid.lua` | Dumps generated Mt. Moon caves by seed for `test/tools/render_cave.py`. |
| `test/tools/create_route14_battle_fixture.lua` | Saves a state inside the Route 14 double battle, for checking the colours by hand. |
| `test/tools/warp_to_route1.lua` | Makes the Route 1 savestate fixture that `test_roaming_route1` needs. |
| `test/tools/warp_to_route1_grass.lua` | Makes the Route 1 tall-grass fixture used by the shiny-chaining tests. |
| `test/tools/save_route1_state.lua` | The older, by-hand way to make the Route 1 fixture. |
| `test/tools/explore_route1.lua` | Probe: random-walks Route 1 looking for tall grass. |
| `test/tools/probe_route1_fixture.lua` | Probe: logs the tiles around the player in the grass fixture. |
| `test/tools/test_route1_movement.lua` | Probe: tries walking each way from the Route 1 fixture. |

**Leftovers from upstream pret** you can ignore:

| Script | What it does |
|---|---|
| `build_tools.sh`, `asmdiff.sh`, `compare.sh`, `replace.sh` | Upstream helpers, untouched since 2019–2020. |
| `docs/bugs_and_glitches.md`, `docs/legacy_WSL1_INSTALL.md` | Upstream docs. |
| `docs/superpowers/` | This fork's plans and a spec from April 2026, from before autodev. History only. |

## On this machine

| Path | What it is |
|---|---|
| `~/projects/minimaleffort/pokefirered` | Your own checkout, where you work. Autodev never touches it. |
| `~/projects/minimaleffort/pokefirered-autodev` | Autodev's clone, pulled from master on every tick. Sessions use its `automation/settings/` and `automation/bin/issue.py`. Grill sessions run here. |
| `~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-<T>` | One folder per open ticket, on branch `issue-<T>-<slug>`. Deleted once the ticket closes. |
| `~/projects/minimaleffort/mgba/build` | The patched emulator every test runs on. |
| `~/autodev/logs/` | Each session's full log, `<role>-<issue>-<time>.jsonl`, kept 30 days. |
| `~/.local/state/autodev/sessions/` | The dispatcher's records. Delete `paused-until` there to lift a usage-limit pause early. |
| `~/.config/autodev/env` | The bot's token. Mode 600, never committed. |
| `/tmp/mgba-test.*`, `/tmp/mgba-recordings/` | Per-run test folders and fresh videos. Cleared on reboot. |
| `~/recordings/` | Videos kept for good. |

## Check on it

```bash
systemctl --user list-units 'autodev-*'        # sessions running now
journalctl --user -u autodev-dispatcher -f     # watch each tick live
systemctl --user list-timers autodev.timer     # when the next tick is due
ls -t ~/autodev/logs | head                    # newest session logs
systemctl --user stop autodev.timer            # stop all new work (sessions already running finish)
```

Recordings: http://100.116.114.81:8080 (`python3 test/serve_recordings.py`, run by `autodev-recordings.service`).
