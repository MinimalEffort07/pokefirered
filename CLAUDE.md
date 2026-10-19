# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Pokemon FireRed decompilation (forked from pret/pokefirered) with custom features: real-time 4-player multiplayer via GBA Serial I/O and a procedurally generated Mt. Moon (three seeded cave floors, each drawn in its vanilla style).

## Build Commands

Targets live in `Makefile`; variable defaults live in `config.mk`. Add `-j$(nproc)` to any build.

```bash
make -j$(nproc) firered   # Standard build with agbcc → pokefirered.gba (plain `make` does the same)
make debug                # FireRed with gameplay debug features (POKEMON_DEBUG=1)
make release              # Normal build again after `make debug` (cleans first, see below)
make firered MODERN=1     # Modern GCC instead of agbcc → pokefirered_modern.gba
make syms                 # Symbol table → pokefirered.sym
make tidy                 # Delete ROMs and build/ only; keeps tools and converted assets (fast rebuild)
make clean                # tidy + clean-tools + clean-generated + clean-assets (full rebuild)
make tools                # Build host tools in tools/ (also runs automatically before every build)
```

Finer-grained cleans: `clean-tools`, `clean-generated` (headers generated from JSON data), `clean-assets` (converted graphics, audio, and map `.inc` files).

**Variant targets.** Each writes `poke<name>.gba` in the repo root, with objects in `build/<name>/`:

| Target | Builds |
|---|---|
| `firered`, `leafgreen` | v1.0 |
| `firered_rev1`, `leafgreen_rev1` | v1.1 (`GAME_REVISION=1`) |
| `firered_switch`, `leafgreen_switch` | Switch re-release (`GAME_REVISION=10`, uses `ld_script_rev10.ld`) |
| `<variant>_modern` | Modern GCC; exists for v1.0 and rev1 only |
| `compare_<variant>` | Build, then SHA1-check against `<variant>.sha1` |

`make modern` and `make compare` are shorthands for FireRed v1.0 that only take effect when they are the sole goal.

**Variables.** Pass them on the command line, e.g. `make firered O_LEVEL=0 DINFO=1`:

| Variable | Default | Effect |
|---|---|---|
| `GAME_VERSION` | `FIRERED` | `FIRERED` or `LEAFGREEN` |
| `GAME_REVISION` | `0` | `0`, `1`, or `10` (Switch) |
| `MODERN` | `0` | `1` = compile with `arm-none-eabi-gcc` instead of agbcc |
| `COMPARE` | `0` | `1` = SHA1-check the ROM after building |
| `POKEMON_DEBUG` | `0` | `1` = define `DEBUG_GAMEPLAY` in C and asm; use `make debug` instead of setting it directly |
| `O_LEVEL` | `2` | Compiler `-O` level |
| `DINFO` | unset | `1` = add `-g` debug info to the ELF |
| `KEEP_TEMPS` | `0` | `1` = keep each C file's preprocessed `.i` and compiler `.s` output in `build/<name>/src/` |
| `NODEP` | `0` | `1` = skip header dependency scanning (faster, but header edits no longer trigger rebuilds) |
| `TOOLCHAIN` | `$DEVKITARM` | Directory whose `bin/` holds `arm-none-eabi-*`; prepended to `PATH` |

**Gotchas:**
- **Flag changes don't trigger rebuilds.** Objects depend only on their sources, so after changing `POKEMON_DEBUG`, `O_LEVEL`, or `DINFO`, run `make tidy` before rebuilding. `make debug` and `make release` handle this themselves: a `.pokemon_debug` marker file makes them run `make clean` when switching modes. Running `make firered` right after `make debug` reuses the debug objects, so switch back with `make release`. Changing `MODERN`, `GAME_VERSION`, or `GAME_REVISION` is safe because each combination gets its own `build/<name>/` directory.
- **`COMPARE=1` fails on this fork.** The custom code means the ROM no longer matches retail's SHA1. A compare check only means something on an unmodified upstream tree.
- **Tests only see `pokefirered.gba`.** `test/run_test.sh`, `test/run_suite.sh` and `test/record_test.sh` load whatever `make firered` or `make debug` built last. Modern, rev1, and Switch builds write to other files, so the tests never run against them.

## Tests

- **One test:** `bash test/run_test.sh test/tests/<test>.lua` exits 0 = pass, 1 = fail, 124 = timed out (`MGBA_TEST_TIMEOUT`, default 60 s). Each run writes to its own `mktemp` folder, printed as `Output:`, so runs can go in parallel.
- **Whole suite:** `bash test/run_suite.sh` is the merge gate. It runs every top-level `test/tests/*.lua` 4 at a time with `MGBA_TEST_TIMEOUT=1500` (about 10 minutes) and exits 0 only if every test it ran passed. Its summary gives each test's result and seconds; failing and timed-out runs keep their folder (with `run_test.log`), passing runs' folders are deleted. Name test files to run a subset; `--help` lists the options.
- **Exclusions:** `test/suite_exclude.txt` lists the tests the suite skips, one per line: the test name, then a one-line reason. Every exclusion needs a reason; the suite refuses to start on a line without one. Exclude only a test that needs a fixture made by hand or makes no assertions (a demo or probe declaring `fw.allow_no_assertions`); a failing test gets fixed.
- The README's "Tests" section has the framework rules (a test with zero assertions fails). `test/tests/test_run_suite.lua` and `test_framework_rules.lua` test the runners themselves.

## Toolchain Requirements

- `arm-none-eabi-binutils` (`as`, `ld`, `objcopy`): needed by every build, including agbcc builds
- `agbcc` in `tools/agbcc/` (built from pret/agbcc; gitignored): compiler for `MODERN=0`
- `arm-none-eabi-gcc`: compiler for `MODERN=1`
- Host `gcc`, `make`, `perl`, `libpng-dev`: `make` compiles the host tools in `tools/` before every build, and agbcc builds run the C preprocessor through the host `cc -E`
- If the ARM toolchain is not on `PATH`, pass `TOOLCHAIN=/path/to/toolchain` or set `DEVKITARM`
- Build tested on Linux / WSL. See `INSTALL.md` for full setup.

## Architecture

### GBA Hardware Context

This is bare-metal GBA code — no OS, no standard library (only `<string.h>` available). All code runs on ARM7TDMI at 16.78 MHz with 32KB IWRAM + 256KB EWRAM. Graphics use tile-based backgrounds and hardware sprites (OAM). I/O is memory-mapped registers.

### Header Hierarchy

- **`include/gba/gba.h`** — GBA hardware: memory map, I/O registers, types (u8/u16/u32), BIOS syscalls, DMA macros
- **`include/global.h`** — Master header included by all .c files. Pulls in gba.h, config.h, game constants, save data structures, core macros (ARRAY_COUNT, MIN, MAX)
- **`include/gflib.h`** — Game Freak library: bg, palette, gpu_regs, dma3, malloc, sound, text, sprite, window
- **`constants/`** — Game data constants (species, flags, vars, maps, items, etc.)

### Source Organization

- **`src/`** — C source (~284 files). Core systems: `main.c`, `overworld.c`, `save.c`, `battle_*.c`, `pokemon.c`, `sprite.c`, `text.c`, `sound.c`
- **`asm/`** — Hand-written ARM assembly (few files remaining)
- **`data/`** — Game data: map scripts/events, layouts, Pokemon data
- **`graphics/`** — Image assets (tilesets, sprites, UI) processed by `gbagfx`
- **`sound/`** — MIDI songs and WAV samples processed by `mid2agb`/`wav2agb`
- **`tools/`** — Build tools source (gbagfx, gbafix, mid2agb, scaninc, preproc, mapjson, etc.)

### Custom Features

**Multiplayer** (`src/multiplayer.c`, `include/multiplayer.h`):
- 4-player real-time position sync via GBA SIO Multi-Player mode
- 3-frame transmission cycle at 60fps: frame 0 = map ID, frame 1 = X/Y coords, frame 2 = graphics/direction/flags
- Each player's data is 16 bits packed with 2-bit header
- Interrupt-driven via `MP_SerialCallback()`; master (ID 0) initiates transfers
- Remote player sprites are spawned/despawned based on map presence
- Movement interpolation handles ~50ms transmission delay

**Procedurally generated Mt. Moon** (core `src/mt_moon_gen.c`, styles `src/mt_moon_style_{1f,b1f,b2f}.c`, contract `include/mt_moon_gen_style.h`). Read the core's OVERVIEW comment and the contract before changing either; the README's Mt. Moon entry and "Mt. Moon generator" section describe the design and list the tests.
- Chain: Route 4 west door ⇄ 1F ⇄ B1F ⇄ B2F ⇄ Route 4 east door. Where warps lead is plain `map.json` data; the generator only moves each floor's two warps onto its ladders.
- Floors: each style draws its floor's vanilla look (1F rock islands and sand, B1F rooms in black void, B2F ridges and walled rooms with stairs, its two up-ladders standing in open floor). B2F always carries vanilla's 13 × 9 fossil chamber as a prefab, because Miguel's walk scripts are hard-coded steps. The core places every vanilla object, hidden item and sign where it stays reachable (P2, P3; nothing is ever dropped), adds the decorations in one shared step (`DecorateCave`, P10), and on Continue moves a player standing off reachable floor onto it (the safety net, S9).
- Determinism: a floor is a pure function of `VAR_MT_MOON_MAP_SEED`, rolled with `Random()` on a fresh entry (`InitMap`) and reused on Continue (`InitMapFromSavedGame`). Every later random choice uses `MtMoonGen_Random()`/`MtMoonGen_RandomBelow()`; one `Random()` call during generation makes Continue rebuild a different cave (tests P8, S6).
- Memory: `mt_moon_gen.c`'s IWRAM statics stay at exactly 16 bytes (`sCave`, `sRngState`); growing them moves `gMain`, `gSaveBlock1Ptr` and `gRngValue`, whose addresses tests hard-code. EWRAM is nearly full: working memory goes in the heap block the core borrows per generation (`struct GenerationWork`, or a style's `workBytes`); only state that must outlive generation (the rewritten event tables) is `EWRAM_DATA`.
- Looks: every N/S/E/W tile pair on a generated floor must occur in that floor's own vanilla `data/layouts/MtMoon_<floor>/map.bin` (P5, per floor); the one allowlisted exception is `v:2ce:29a` above B2F's fossil chamber (`p5_b2f.lua`).
- Load time: generation runs in the warp's black screen; P9 allows +30 frames over a vanilla cave and the worst seeds sit at +20 to +25. A pass over the whole map costs about a frame on the GBA.
- Tests: the 9 `test/tests/test_mt_moon_*.lua` files (property sweep P1–P10, FC, SAND-1F, B1F-W over 100 seeds × 3 floors, needing `MGBA_TEST_TIMEOUT=1500`; scenarios S1–S9). A new property check is a new file in `test/tests/mt_moon_checks/`. `test/tools/dump_mtmoon_grid.lua` dumps a cave by seed and `test/tools/render_cave.py` draws it (or any vanilla `map.bin`) to PNG.

## Documentation Standards

Code changes must be thoroughly documented for contributors who lack embedded systems or GBA experience. This means:
- Explain *why* hardware registers are set to specific values, not just what the values are
- Clarify GBA-specific concepts inline (DMA, OAM, tile modes, SIO, interrupts, EWRAM/IWRAM)
- Describe bit manipulation and packed data formats in plain language
- Note non-obvious hardware constraints (alignment, timing, memory limits)
- Don't assume familiarity with ARM architecture, memory-mapped I/O, or GBA SDK conventions

## GitHub Issue Workflow

### Upstream Repo Boundary
This project is forked from `pret/pokefirered`. You may **read** from that upstream repo (e.g. `gh issue list -R pret/pokefirered`) for reference, but **never write to it** — no PRs, comments, pushes, or any mutation of `pret/pokefirered`.

### When Given a GitHub Issue
Follow these steps in order. Do not skip or reorder them.

**1. Read the full issue**
Fetch the complete issue with all comments before doing anything else:
```bash
gh issue view <N> | cat
gh issue view <N> --comments | cat
```

**2. Grill the design**
Read every referenced file, related code, and relevant test patterns, then invoke the `mattpocock-skills:grilling` skill with a description of the change the issue asks for (`grill-me` is the user-typed alias for the same skill). The grilling is done when the user has agreed, in writing, on:
- Scope limits and design choices
- Every scenario the automated test must cover (happy path, failure modes, edge cases — e.g. "what if the box is full?", "what if party is at minimum?")
- The exact visual flow the demo GIF must show (which menus, which transitions, what on-screen text confirms success)
- The pass/fail criteria for each check (what value is read, from which address, expected vs. actual)

Under-specified tests are the single biggest source of rework on this project.

**3. Record the design on the issue**
Post the agreed design as a comment on the issue so its history captures what was decided and why:
```bash
gh issue comment <N> --body "..."
```
Also edit the issue description to include a summary of scope if the original description was sparse:
```bash
gh issue edit <N> --body "..."
```

**4. Break the design into ticket issues**
Turn the grilling result into ticket issues with the `to-tickets` skill (the successor to `to-issues`). It is user-invocable only, so read its `SKILL.md` and follow its process yourself, using the highest version this lists:
```bash
ls ~/.claude*/plugins/cache/*/mattpocock-skills/*/skills/engineering/to-tickets/SKILL.md
```
Tracker settings for this repo:
- Publish each ticket as a GitHub issue on `origin` (`MinimalEffort07/pokefirered`) with `gh issue create`.
- Reference `#<N>` under the ticket's `## Parent` heading and list each blocker as `#<M>` under `## Blocked by`. These body sections are the blocking edges.
- Leave labels off; this repo has no triage labels.
- Each ticket's acceptance criteria carry its share of the test scenarios, demo flow, and pass/fail criteria from step 2.

**5. Implement the frontier in parallel**
The **frontier** is every open ticket whose `## Blocked by` issues are all closed. Launch one subagent per frontier ticket with `isolation: "worktree"`, all in a single message so they run concurrently. Give each subagent its ticket number `<T>` and steps 6–11, and have it report back its PR URL.

**6. Create a feature branch**
```bash
git checkout -b issue-<T>-<short-slug>
```
Never implement on `master` directly. A fresh worktree lacks the gitignored `tools/agbcc/`, so symlink it from the main checkout before building:
```bash
ln -s "$(git worktree list | head -1 | cut -d' ' -f1)/tools/agbcc" tools/agbcc
```

**7. Implement with TDD**
- Invoke the `mattpocock-skills:tdd` skill.
- Write a failing test first, then implement until it passes.
- Build must be clean: `make -j$(nproc) firered`

**8. Record a test run**
```bash
bash test/record_test.sh test/tests/<relevant_test>.lua
```
Move the recording to `~/recordings/` as per project convention.

**9. Update the README**
Add or update the feature description and include a demo GIF if the feature has visible in-game behaviour.

**10. Code review**
Invoke the `mattpocock-skills:code-review` skill against `master` and address its findings.

**11. Create a PR**

```bash
gh pr create --title "Fix #<T>: <short description>" --body "..."
```
PR body must reference the ticket and parent issue numbers, summarise what changed, and list what was tested.

Once the frontier's PRs are created, post the following in your response to the user, with one `Branch README` line per branch:
```
Recordings available at: http://100.116.114.81:8080
(run: python3 test/serve_recordings.py)

Branch README: https://github.com/MinimalEffort07/pokefirered/blob/<branch-name>/README.md
```
Replace `<branch-name>` with the actual feature branch name (e.g. `issue-11-pc-anywhere`).

**12. Wait for approval — do not merge until told to**
After creating the PRs, stop. The user will review them. Only run the merge command when the user explicitly says to proceed:
```bash
gh pr merge <PR-number>
```
Do **not** squash commits (`--squash`). Preserve the full commit history from the branch.

**13. After merging — close the ticket, sync master, advance the frontier**
After a PR is merged, close its ticket and sync master:
```bash
gh issue close <T>
git checkout master
git pull
```
Closing tickets unblocks the ones that depend on them: return to step 5 for the new frontier. Once every ticket is closed, close the parent issue `#<N>`.

## Coding Conventions

- No standard library beyond `<string.h>`. Use GBA-specific types: `u8`, `u16`, `u32`, `s8`, `s16`, `s32`, `bool8`
- Memory sections matter: `EWRAM_DATA`, `IWRAM_DATA`, `IWRAM_CODE` attributes control placement
- `AGBCC` compiler has quirks — avoid C99 features when `MODERN=0` (no mixed declarations, no `//` comments in some contexts)
- Game state lives in `gSaveBlock1Ptr`, `gSaveBlock2Ptr`, `gPokemonStoragePtr`
- Map/event scripting uses bytecode in `data/maps/*/scripts.inc`
