# Walking Pokemon Return Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the party-menu RETURN option to identify the walking Pokemon by species+PID rather than by party slot index, so rearranging the party no longer orphans the RETURN option.

**Architecture:** Add `u32 pid` to the `sFollower` EWRAM struct. Add `IsFollowerPokemon(struct Pokemon *mon)` that compares species and personality value against the cached values. Replace the slot-index comparison in `SetPartyMonFieldSelectionActions` with this identity check.

**Tech Stack:** C (GBA bare-metal, no OS), GBA EWRAM, mGBA Lua scripting tests.

---

## Root Cause

`sFollower.partySlot` stores a slot index. After a party SWITCH, the index is stale:

- Slot that now holds the follower Pokemon: no RETURN option (orphaned)
- Slot that *used to* hold it: incorrectly shows RETURN

`IsFollowerPokemon` fixes this by comparing identity (species + 32-bit personality value) rather than position. Two Pokemon in the same party sharing identical species AND PID is astronomically unlikely (~1 in 700M), so this is safe in practice.

---

## File Map

| File | Change |
|------|--------|
| `src/pokemon_follower.c` | Add `u32 pid` to `sFollower` struct; store it in `ActivateFollower`; clear it in `DeactivateFollower`; add `IsFollowerPokemon` function |
| `include/pokemon_follower.h` | Declare `IsFollowerPokemon(struct Pokemon *mon)` |
| `src/party_menu.c` | Replace slot-index check with `IsFollowerPokemon(&mons[slotId])` |
| `test/tests/test_walking_pokemon_return.lua` | New integration test (full menu nav) |
| `test/lib/addresses.lua` | Add `sFollower` address; fix stale roaming addresses (+4 from new struct field) |

---

### Task 1: Create feature branch

- [ ] Create branch:
```bash
git checkout -b issue-4-walking-pokemon-return-by-identity
```

- [ ] Verify:
```bash
git branch --show-current
```
Expected: `issue-4-walking-pokemon-return-by-identity`

---

### Task 2: Comment issue with agreed plan

- [ ] Post the agreed design on the issue:
```bash
gh issue comment 4 --body "## Agreed Fix

Track the walking Pokemon by **species+PID** instead of party slot index.

### Root Cause
\`sFollower.partySlot\` stores a slot index. After a party SWITCH, the index becomes stale — RETURN appears on whichever Pokemon now occupies that slot, not the actual follower. The actual follower loses its RETURN option entirely.

### Solution
Add \`u32 pid\` (personality value) to the \`sFollower\` EWRAM struct. Store it at activation time. Add \`IsFollowerPokemon(struct Pokemon *mon)\` that checks both species and PID. Replace the slot-index comparison in \`SetPartyMonFieldSelectionActions\` with this identity check.

### Test
Full party-menu integration test:
1. Boot to overworld with Pikachu (slot 0) + Meowth (slot 1)
2. WALK Pikachu from slot 0
3. SWITCH Pikachu to slot 1
4. Verify RETURN works on slot 1 (Pikachu's new position) — follower deactivates
5. WALK Meowth from slot 0 — verify follower activates with Meowth's species"
```

---

### Task 3: Write the failing test

**Files:**
- Create: `test/tests/test_walking_pokemon_return.lua`

- [ ] Create the test file with this content:

```lua
-------------------------------------------------------------------------------
-- test_walking_pokemon_return.lua
--
-- Regression test for issue #4: walking-pokemon RETURN must follow the
-- Pokemon by identity (species+PID), not party slot index.
--
-- Bug: after a party SWITCH that moves the walking Pokemon to a new slot,
-- the stale sFollower.partySlot caused RETURN to appear on the wrong slot
-- and the actual follower to lose its RETURN option entirely.
--
-- Full flow:
--   1. Boot to overworld. Inject Pikachu (slot 0) + Meowth (slot 1).
--   2. Open party menu -> WALK Pikachu.
--      Check: sFollower.active==1, sFollower.species==25.
--   3. Open party menu -> SWITCH Pikachu (slot 0) with Meowth (slot 1).
--      Check: sFollower.active still 1.
--   4. Open party menu -> slot 1 (Pikachu's new position) -> option [1].
--      After fix: option [1] = RETURN -> follower deactivates.
--      Before fix: option [1] = SWITCH -> sub-menu opens; B backs out; follower stays.
--      Check: sFollower.active==0.
--   5. Open party menu -> WALK Meowth (slot 0).
--      Check: sFollower.active==1, sFollower.species==52.
--
-- Run:
--   bash test/run_test.sh test/tests/test_walking_pokemon_return.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH   = "/tmp/pokefirered-walking-pokemon.ss"
local PARTY_BASE   = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE = 100         -- sizeof(struct Pokemon)

-- sFollower EWRAM struct (src/pokemon_follower.c).
-- Base from map: grep "src/pokemon_follower.o" pokefirered.map (ewram_data line).
-- Field layout after adding u32 pid (offsets in bytes):
--   +0  bool8 active
--   +1  bool8 spriteSpawned
--   +2  u8    partySlot
--   +3  (padding byte for u16 alignment)
--   +4  u16   species
--   +6  u8    graphicsId
--   +7  u8    objEventId
--   +8  u32   pid
local FOLLOWER_BASE    = 0x0203f7a8
local FOLLOWER_ACTIVE  = FOLLOWER_BASE + 0
local FOLLOWER_SPECIES = FOLLOWER_BASE + 4  -- u16

local SPECIES_PIKACHU = 25
local SPECIES_MEOWTH  = 52

-- Write a minimal valid Pokemon into party memory at `base`.
-- Uses PID=0, OTID=0 so XOR encryption key = PID XOR OTID = 0 (no encryption).
-- Substruct order = PID % 24 = 0 = [Growth, Attacks, EVs, Misc].
-- Growth.species is the first u16 of the secure block (BoxPokemon+0x20).
local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)                       -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)                       -- no mail
    emu:write8(base + 0x20, species % 256)              -- Growth.species low
    emu:write8(base + 0x21, math.floor(species / 256))  -- Growth.species high
    emu:write8(base + 0x1C, species % 256)              -- checksum low
    emu:write8(base + 0x1D, math.floor(species / 256))  -- checksum high
    emu:write8(base + 0x54, level)                      -- level
    emu:write8(base + 0x56, 20)                         -- currentHP
    emu:write8(base + 0x58, 20)                         -- maxHP
end

local results = {}
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.log((cond and "[PASS] " or "[FAIL] ") .. name)
end

fw.run(function()
    fw.log("=== Walking Pokemon Return Fix Test ===")

    -- ------------------------------------------------------------------ --
    -- Fixture: boot to overworld with Pikachu+Meowth, save state
    -- ------------------------------------------------------------------ --
    if not fw.try_load_state(STATE_PATH) then
        fw.log("No cached state — booting through Oak's speech...")
        local sel = cs.find_selection_press()
        if not sel then
            fw.log("ERROR: character select discovery failed — aborting")
            fw.finish()
            return
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()
        fw.log("Reached overworld. Injecting Pikachu + Meowth...")

        inject_pokemon(PARTY_BASE,                SPECIES_PIKACHU, 5)
        inject_pokemon(PARTY_BASE + POKEMON_SIZE, SPECIES_MEOWTH,  5)
        emu:write8(ADDR.gPlayerPartyCount, 2)

        -- Set FLAG_SYS_POKEMON_GET (0x828) so Start menu shows POKEMON.
        -- byte index = 0x828/8 = 0x105, bit = 0
        local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
        local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
        emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
        fw.wait_frames(30)
        fw.save_state(STATE_PATH)
    end
    fw.wait_frames(60)

    -- ------------------------------------------------------------------ --
    -- Phase 1: WALK Pikachu from slot 0
    -- Action menu for slot 0 (Pikachu, eligible, no follower):
    --   [0] SUMMARY  [1] WALK  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 1: WALK Pikachu (slot 0) ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("A");     fw.wait_frames(80)   -- open slot 0 action menu
    fw.press("DOWN");  fw.wait_frames(20)   -- move to [1] WALK
    fw.press("A");     fw.wait_frames(120)  -- select WALK; returns to overworld

    check("Phase 1: follower active after WALK",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("Phase 1: follower species is Pikachu",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_PIKACHU)

    -- ------------------------------------------------------------------ --
    -- Phase 2: SWITCH Pikachu (slot 0) with Meowth (slot 1)
    -- Action menu for slot 0 (Pikachu, IS follower, active):
    --   [0] SUMMARY  [1] RETURN  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- After SWITCH: Meowth is at slot 0, Pikachu is at slot 1.
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 2: SWITCH Pikachu to slot 1 ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("A");     fw.wait_frames(80)   -- open slot 0 action menu (Pikachu)
    fw.press("DOWN");  fw.wait_frames(20)   -- to [1] RETURN
    fw.press("DOWN");  fw.wait_frames(20)   -- to [2] SWITCH
    fw.press("A");     fw.wait_frames(60)   -- enter SWITCH mode
    fw.press("DOWN");  fw.wait_frames(20)   -- move cursor to slot 1
    fw.press("A");     fw.wait_frames(80)   -- confirm swap; party now swapped
    fw.press("B");     fw.wait_frames(80)   -- close party menu
    fw.press("B");     fw.wait_frames(30)   -- close start menu if still open

    check("Phase 2: follower still active after SWITCH",
          fw.read8(FOLLOWER_ACTIVE) == 1)

    -- ------------------------------------------------------------------ --
    -- Phase 3: RETURN Pikachu from slot 1 (its new position)
    -- After fix — slot 1 action menu (Pikachu identified by species+PID):
    --   [0] SUMMARY  [1] RETURN  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- Before fix — slot 1 action menu (orphaned: follower is "not here" by slot):
    --   [0] SUMMARY  [1] SWITCH  [2] ITEM  [3] MOVE TO PC  [4] CANCEL
    --   Pressing [1] opens SWITCH sub-menu; pressing B backs out; active stays 1.
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 3: RETURN Pikachu from slot 1 ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("DOWN");  fw.wait_frames(20)   -- cursor to slot 1
    fw.press("A");     fw.wait_frames(80)   -- open slot 1 action menu
    fw.press("DOWN");  fw.wait_frames(20)   -- to option [1] (RETURN after fix)
    fw.press("A");     fw.wait_frames(120)  -- select it; returns to overworld if RETURN
    -- If SWITCH fired instead (pre-fix bug), back out of any sub-menus
    fw.press("B");     fw.wait_frames(30)
    fw.press("B");     fw.wait_frames(30)
    fw.press("B");     fw.wait_frames(30)

    check("Phase 3: follower deactivated by RETURN",
          fw.read8(FOLLOWER_ACTIVE) == 0)

    -- ------------------------------------------------------------------ --
    -- Phase 4: WALK Meowth from slot 0 (its new position after swap)
    -- Action menu for slot 0 (Meowth, eligible, no follower active):
    --   [0] SUMMARY  [1] WALK  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 4: WALK Meowth (slot 0) ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("A");     fw.wait_frames(80)   -- open slot 0 action menu (Meowth)
    fw.press("DOWN");  fw.wait_frames(20)   -- to [1] WALK
    fw.press("A");     fw.wait_frames(120)  -- select WALK; returns to overworld

    check("Phase 4: follower active after WALK Meowth",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("Phase 4: follower species is Meowth",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_MEOWTH)

    -- ------------------------------------------------------------------ --
    -- Report
    -- ------------------------------------------------------------------ --
    local passed = 0
    for _, r in ipairs(results) do if r.pass then passed = passed + 1 end end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
```

- [ ] Verify the file was created:
```bash
ls test/tests/test_walking_pokemon_return.lua
```

---

### Task 4: Run test — verify it fails before fix

- [ ] Build ROM first (required before running tests):
```bash
make -j$(nproc) firered 2>&1 | tail -5
```

- [ ] Run test:
```bash
bash test/run_test.sh test/tests/test_walking_pokemon_return.lua 2>&1 | tail -20
```
Expected: `[FAIL] Phase 3: follower deactivated by RETURN`

Before the fix, pressing option [1] on slot 1 triggers SWITCH (not RETURN), because `sFollower.partySlot=0` still points to slot 0. The B presses back out of the SWITCH sub-menu, and `sFollower.active` remains 1.

---

### Task 5: Implement the fix

**Files:**
- Modify: `src/pokemon_follower.c` (lines ~75-209)
- Modify: `include/pokemon_follower.h`
- Modify: `src/party_menu.c` (line ~3000)

**5a. Add `pid` to `sFollower` and update lifecycle functions**

In `src/pokemon_follower.c`, update the struct at line ~75 to add `u32 pid`:

```c
static EWRAM_DATA struct {
    bool8 active;
    bool8 spriteSpawned;
    u8 partySlot;
    u16 species;
    u8 graphicsId;
    u8 objEventId;
    u32 pid;  /* personality value — identifies this Pokemon across party rearrangements */
} sFollower = {0};
```

In `ActivateFollower` (line ~178), add one line after setting `objEventId`:
```c
    sFollower.objEventId = OBJECT_EVENTS_COUNT;
    sFollower.pid = GetMonData(&gPlayerParty[partySlot], MON_DATA_PERSONALITY);
```

In `DeactivateFollower` (line ~202), add clearing `pid`:
```c
void DeactivateFollower(void)
{
    DespawnFollowerSprite();
    sFollower.active = FALSE;
    sFollower.partySlot = 0;
    sFollower.species = 0;
    sFollower.graphicsId = 0;
    sFollower.pid = 0;
}
```

Add `IsFollowerPokemon` after `GetFollowerPartySlot` (line ~163):
```c
bool8 IsFollowerPokemon(struct Pokemon *mon)
{
    if (!sFollower.active)
        return FALSE;
    return GetMonData(mon, MON_DATA_SPECIES) == sFollower.species
        && GetMonData(mon, MON_DATA_PERSONALITY) == sFollower.pid;
}
```

**5b. Declare `IsFollowerPokemon` in the header**

In `include/pokemon_follower.h`, add after the `GetFollowerPartySlot` line:
```c
bool8 IsFollowerPokemon(struct Pokemon *mon);
```

Also add `#include "pokemon.h"` if `struct Pokemon` is not already reachable through `global.h` — the compiler will tell you if this is needed (error: unknown type `struct Pokemon`).

**5c. Update party menu**

In `src/party_menu.c` line ~3000, change:
```c
if (IsFollowerActive() && slotId == GetFollowerPartySlot())
```
to:
```c
if (IsFollowerPokemon(&mons[slotId]))
```

- [ ] Build to verify no errors:
```bash
make -j$(nproc) firered 2>&1 | tail -10
```
Expected: clean build.

- [ ] Commit:
```bash
git add src/pokemon_follower.c include/pokemon_follower.h src/party_menu.c
git commit -m "$(cat <<'EOF'
fix: track walking pokemon by species+PID, not party slot index

sFollower.partySlot became stale after a party SWITCH — RETURN
appeared on the wrong Pokemon and the actual follower lost its
RETURN option entirely.

Store the follower's personality value (PID) at activation time.
IsFollowerPokemon() compares both species and PID so RETURN follows
the actual Pokemon regardless of which party slot it ends up in.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Update EWRAM addresses after struct growth

Adding `u32 pid` (4 bytes) to `sFollower` shifts all EWRAM variables from `quick_select_menu.o` and `roaming_pokemon.o` onwards by +4 bytes.

**Note:** `gRoamers` and related addresses in `addresses.lua` are ALREADY stale by +8 bytes from the current build (the follower feature was added after those constants were last extracted). This task fixes both the new shift and the pre-existing drift.

- [ ] Re-extract addresses from the map:
```bash
grep "203f7\|203f8\|203f9\|gRoamers\|gRoaming\|gRoamerCount\|gRoamerNext\|pokemon_follower" pokefirered.map
```

- [ ] Update `test/lib/addresses.lua` — add `sFollower` block and fix roaming constants to match map output:

In `addresses.lua`, add a new section (exact addresses must come from the map output above):
```lua
-------------------------------------------------------------------------------
-- WALKING POKEMON FOLLOWER (pokemon_follower.c)
-------------------------------------------------------------------------------
-- sFollower EWRAM struct. Base from map: grep "src/pokemon_follower.o" pokefirered.map.
-- Field offsets (struct layout after fix added u32 pid):
--   +0  bool8 active
--   +1  bool8 spriteSpawned
--   +2  u8    partySlot
--   +3  (alignment padding)
--   +4  u16   species
--   +6  u8    graphicsId
--   +7  u8    objEventId
--   +8  u32   pid
ADDR.sFollower               = 0x0203f7a8   -- re-verify from map after each rebuild
ADDR.FOLLOWER_OFF_ACTIVE     = 0x00
ADDR.FOLLOWER_OFF_SPECIES    = 0x04
ADDR.FOLLOWER_OFF_PID        = 0x08
```

And update the roaming constants to the new addresses shown by the map.

- [ ] Commit:
```bash
git add test/lib/addresses.lua
git commit -m "$(cat <<'EOF'
chore: update EWRAM addresses after sFollower struct growth

sFollower grew by 4 bytes (added u32 pid), shifting quick_select_menu.o
and roaming_pokemon.o sections forward. Also fixes pre-existing 8-byte
drift in gRoamers and related constants (never updated after follower
system was added).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Run test — verify all checks pass

- [ ] Delete stale fixture (EWRAM layout changed; old state maps addresses wrong):
```bash
rm -f /tmp/pokefirered-walking-pokemon.ss
```

- [ ] Also delete other fixtures that might be affected by shifted roaming addresses:
```bash
rm -f /tmp/pokefirered-*.ss
```

- [ ] Run the walking pokemon test:
```bash
bash test/run_test.sh test/tests/test_walking_pokemon_return.lua 2>&1 | tail -20
```

Expected:
```
[PASS] Phase 1: follower active after WALK
[PASS] Phase 1: follower species is Pikachu
[PASS] Phase 2: follower still active after SWITCH
[PASS] Phase 3: follower deactivated by RETURN
[PASS] Phase 4: follower active after WALK Meowth
[PASS] Phase 4: follower species is Meowth
Results: 6/6 passed
```

---

### Task 8: Create PR

- [ ] Create the PR:
```bash
gh pr create --title "Fix #4: track walking pokemon by species+PID, not party slot" --body "$(cat <<'EOF'
## Summary

- `sFollower` now stores `u32 pid` (personality value) at activation time alongside the existing `species` cache
- New `IsFollowerPokemon(struct Pokemon *mon)` checks both species and PID to identify the follower
- `SetPartyMonFieldSelectionActions` uses `IsFollowerPokemon` instead of slot-index comparison
- RETURN option now follows the actual Pokemon through any party rearrangement

## Root Cause

\`sFollower.partySlot\` stored a slot index. After a party SWITCH, the index was stale:
- The new slot holding the follower Pokemon: showed no RETURN (orphaned)
- The old slot: incorrectly showed RETURN on whatever Pokemon moved there

## Test

\`test/tests/test_walking_pokemon_return.lua\` — full party-menu integration:
1. Inject Pikachu (slot 0) + Meowth (slot 1), reach overworld
2. WALK Pikachu → verify active + species
3. SWITCH Pikachu to slot 1 → verify still active
4. Open party menu → slot 1 → RETURN → verify deactivated (**core regression check**)
5. WALK Meowth → verify active + Meowth species

Also fixes pre-existing 8-byte drift in \`gRoamers\` and related addresses in \`addresses.lua\`.

Closes #4

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] After creating the PR, post in session:
```
Branch README: https://github.com/MinimalEffort07/pokefirered/blob/issue-4-walking-pokemon-return-by-identity/README.md
```

---

## Self-Review

**Spec coverage:**
- Fix RETURN to track by identity, not position ✓ (Task 5)
- Test: WALK → SWITCH → RETURN from new slot → WALK new Pokemon ✓ (Task 3)
- No GIF, no recording (bug fix) ✓
- Issue commented with plan ✓ (Task 2)
- Branch created ✓ (Task 1)
- PR created ✓ (Task 8)
- Address drift fixed ✓ (Task 6)

**Placeholder scan:** No TBDs. Task 6 explicitly requires updating addresses from map output rather than hardcoding them, which is correct since they depend on the build.

**Type consistency:** `IsFollowerPokemon(struct Pokemon *mon)` — declared in header, defined in .c with same signature, called as `IsFollowerPokemon(&mons[slotId])` where `mons` is `struct Pokemon *` in party_menu.c. ✓
