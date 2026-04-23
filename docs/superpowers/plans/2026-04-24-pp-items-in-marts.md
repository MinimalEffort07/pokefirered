# PP Items in Marts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Ether/Max Ether to Super Potion shops, Elixir to Hyper Potion shops, and Max Elixir to Max Potion shops — 12 `.inc` files, 15 inserted lines, plus a Lua test and README update.

**Architecture:** Pure data change. Each mart's item list is a `.2byte` array in its `scripts.inc` file, terminated by `ITEM_NONE`. We insert the PP item immediately after its anchor HP item. A Lua test reads the item list directly from ROM (via the globally-linked `*_Items` labels) to assert correct placement, and also navigates to four representative marts for the recording/GIF.

**Tech Stack:** GBA assembly script data (`.inc`), mGBA Lua test framework (`test/lib/framework.lua`, `test/lib/warp.lua`, `test/lib/addresses.lua`), `make`, `gh`.

---

### Task 1: Create feature branch

**Files:** none

- [ ] **Step 1: Create and switch to branch**

```bash
git checkout -b issue-9-pp-items-in-marts
```

- [ ] **Step 2: Confirm clean working tree**

```bash
git status
```
Expected: `nothing to commit, working tree clean`

---

### Task 2: Add Ether + Max Ether to Super Potion shops

**Files:**
- Modify: `data/maps/CeruleanCity_Mart/scripts.inc`
- Modify: `data/maps/LavenderTown_Mart/scripts.inc`
- Modify: `data/maps/VermilionCity_Mart/scripts.inc`
- Modify: `data/maps/FuchsiaCity_Mart/scripts.inc`
- Modify: `data/maps/CeladonCity_DepartmentStore_2F/scripts.inc`

In each file, find the line `.2byte ITEM_SUPER_POTION` and insert two lines immediately after it:

```asm
	.2byte ITEM_ETHER
	.2byte ITEM_MAX_ETHER
```

- [ ] **Step 1: Edit CeruleanCity_Mart/scripts.inc**

Find:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_POTION
```
Replace with:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_ETHER
	.2byte ITEM_MAX_ETHER
	.2byte ITEM_POTION
```

- [ ] **Step 2: Edit LavenderTown_Mart/scripts.inc**

Find:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_REVIVE
```
Replace with:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_ETHER
	.2byte ITEM_MAX_ETHER
	.2byte ITEM_REVIVE
```

- [ ] **Step 3: Edit VermilionCity_Mart/scripts.inc**

Find:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_ANTIDOTE
```
Replace with:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_ETHER
	.2byte ITEM_MAX_ETHER
	.2byte ITEM_ANTIDOTE
```

- [ ] **Step 4: Edit FuchsiaCity_Mart/scripts.inc**

Find:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_REVIVE
```
Replace with:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_ETHER
	.2byte ITEM_MAX_ETHER
	.2byte ITEM_REVIVE
```

- [ ] **Step 5: Edit CeladonCity_DepartmentStore_2F/scripts.inc**

Find:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_REVIVE
```
Replace with:
```
	.2byte ITEM_SUPER_POTION
	.2byte ITEM_ETHER
	.2byte ITEM_MAX_ETHER
	.2byte ITEM_REVIVE
```

- [ ] **Step 6: Commit**

```bash
git add data/maps/CeruleanCity_Mart/scripts.inc \
        data/maps/LavenderTown_Mart/scripts.inc \
        data/maps/VermilionCity_Mart/scripts.inc \
        data/maps/FuchsiaCity_Mart/scripts.inc \
        data/maps/CeladonCity_DepartmentStore_2F/scripts.inc
git commit -m "feat: add Ether and Max Ether to Super Potion shops (issue #9)"
```

---

### Task 3: Add Elixir to Hyper Potion shops

**Files:**
- Modify: `data/maps/CinnabarIsland_Mart/scripts.inc`
- Modify: `data/maps/SaffronCity_Mart/scripts.inc`
- Modify: `data/maps/ThreeIsland_Mart/scripts.inc`
- Modify: `data/maps/SevenIsland_Mart/scripts.inc`
- Modify: `data/maps/TrainerTower_Lobby/scripts.inc`

In each file, find `.2byte ITEM_HYPER_POTION` and insert one line immediately after:

```asm
	.2byte ITEM_ELIXIR
```

- [ ] **Step 1: Edit CinnabarIsland_Mart/scripts.inc**

Find and replace — after `ITEM_HYPER_POTION` insert `ITEM_ELIXIR`. Verify by reading the file: Cinnabar has `ITEM_HYPER_POTION` followed by `ITEM_REVIVE`. Replace with:
```
	.2byte ITEM_HYPER_POTION
	.2byte ITEM_ELIXIR
	.2byte ITEM_REVIVE
```

- [ ] **Step 2: Edit SaffronCity_Mart/scripts.inc** — same pattern, check what follows `ITEM_HYPER_POTION` in that file first, then insert `ITEM_ELIXIR` between `ITEM_HYPER_POTION` and whatever follows.

- [ ] **Step 3: Edit ThreeIsland_Mart/scripts.inc** — same pattern.

- [ ] **Step 4: Edit SevenIsland_Mart/scripts.inc** — insert `ITEM_ELIXIR` after `ITEM_HYPER_POTION`. (Max Elixir will be added in Task 4.)

- [ ] **Step 5: Edit TrainerTower_Lobby/scripts.inc** — insert `ITEM_ELIXIR` after `ITEM_HYPER_POTION`. (Max Elixir will be added in Task 4.)

- [ ] **Step 6: Commit**

```bash
git add data/maps/CinnabarIsland_Mart/scripts.inc \
        data/maps/SaffronCity_Mart/scripts.inc \
        data/maps/ThreeIsland_Mart/scripts.inc \
        data/maps/SevenIsland_Mart/scripts.inc \
        data/maps/TrainerTower_Lobby/scripts.inc
git commit -m "feat: add Elixir to Hyper Potion shops (issue #9)"
```

---

### Task 4: Add Max Elixir to Max Potion shops

**Files:**
- Modify: `data/maps/FourIsland_Mart/scripts.inc`
- Modify: `data/maps/SixIsland_Mart/scripts.inc`
- Modify: `data/maps/IndigoPlateau_PokemonCenter_1F/scripts.inc`
- Modify: `data/maps/SevenIsland_Mart/scripts.inc`
- Modify: `data/maps/TrainerTower_Lobby/scripts.inc`

In each file, find `.2byte ITEM_MAX_POTION` and insert one line immediately after:

```asm
	.2byte ITEM_MAX_ELIXIR
```

- [ ] **Step 1: Edit FourIsland_Mart/scripts.inc**

```
	.2byte ITEM_MAX_POTION
	.2byte ITEM_MAX_ELIXIR
	.2byte ITEM_REVIVE
```

- [ ] **Step 2: Edit SixIsland_Mart/scripts.inc** — same pattern.

- [ ] **Step 3: Edit IndigoPlateau_PokemonCenter_1F/scripts.inc** — same pattern.

- [ ] **Step 4: Edit SevenIsland_Mart/scripts.inc** — insert `ITEM_MAX_ELIXIR` after `ITEM_MAX_POTION`. At this point the file already has `ITEM_ELIXIR` from Task 3.

- [ ] **Step 5: Edit TrainerTower_Lobby/scripts.inc** — same dual-tier pattern.

- [ ] **Step 6: Commit**

```bash
git add data/maps/FourIsland_Mart/scripts.inc \
        data/maps/SixIsland_Mart/scripts.inc \
        data/maps/IndigoPlateau_PokemonCenter_1F/scripts.inc \
        data/maps/SevenIsland_Mart/scripts.inc \
        data/maps/TrainerTower_Lobby/scripts.inc
git commit -m "feat: add Max Elixir to Max Potion shops (issue #9)"
```

---

### Task 5: Build and verify

**Files:** none

- [ ] **Step 1: Build**

```bash
make -j$(nproc) firered
```
Expected: exits 0, no warnings on any of the modified `.inc` files.

- [ ] **Step 2: Spot-check one mart via symbol lookup**

```bash
grep "CeruleanCity_Mart_Items" pokefirered.map
```
Expected: one line like `0x0816xxxx  CeruleanCity_Mart_Items`

Read 10 u16 values from that address with Python to sanity-check the list includes `ITEM_ETHER (34)`:
```bash
python3 - <<'EOF'
import struct, sys
addr = int(input("Enter ROM address (hex, no 0x): "), 16) - 0x08000000
with open("pokefirered.gba", "rb") as f:
    f.seek(addr)
    items = []
    while True:
        val = struct.unpack("<H", f.read(2))[0]
        if val == 0: break
        items.append(val)
print("Item IDs:", items)
print("Contains ITEM_ETHER (34):", 34 in items)
print("Contains ITEM_MAX_ETHER (35):", 35 in items)
EOF
```
Paste the address from the map file (without `0x`).

---

### Task 6: Add mart item list addresses to addresses.lua

**Files:**
- Modify: `test/lib/addresses.lua`

The mart item list labels are globally linked; their addresses appear in `pokefirered.map` after the build. These addresses changed when you added bytes to the scripts, so extract them now from the freshly-built map.

- [ ] **Step 1: Extract all mart item list addresses**

```bash
grep "Mart_Items\|PokemonCenter_1F_Items\|TrainerTower_Lobby_Mart_Items" pokefirered.map \
  | grep "0x0000000008" | sort
```

Note all 13 addresses.

- [ ] **Step 2: Append to test/lib/addresses.lua**

Add this block before the final `return ADDR` line:

```lua
-------------------------------------------------------------------------------
-- MART ITEM LISTS (ROM — 0x08xxxxxx)
-------------------------------------------------------------------------------
-- Addresses of the .2byte item-id arrays in each mart's scripts.inc.
-- Extracted from pokefirered.map after build. Re-extract if ROM is rebuilt.
-- Item IDs: ITEM_NONE=0, ITEM_SUPER_POTION=22, ITEM_HYPER_POTION=21,
--           ITEM_MAX_POTION=20, ITEM_ETHER=34, ITEM_MAX_ETHER=35,
--           ITEM_ELIXIR=36, ITEM_MAX_ELIXIR=37

ADDR.MART_ITEMS_CERULEAN          = 0x0816XXXX  -- replace XX from map
ADDR.MART_ITEMS_LAVENDER          = 0x0816XXXX
ADDR.MART_ITEMS_VERMILION         = 0x0816XXXX
ADDR.MART_ITEMS_FUCHSIA           = 0x0816XXXX
ADDR.MART_ITEMS_CELADON_DEPT_2F   = 0x0816XXXX
ADDR.MART_ITEMS_CINNABAR          = 0x0816XXXX
ADDR.MART_ITEMS_SAFFRON           = 0x0816XXXX
ADDR.MART_ITEMS_THREE_ISLAND      = 0x0817XXXX
ADDR.MART_ITEMS_FOUR_ISLAND       = 0x0817XXXX
ADDR.MART_ITEMS_SIX_ISLAND        = 0x0817XXXX
ADDR.MART_ITEMS_SEVEN_ISLAND      = 0x0817XXXX
ADDR.MART_ITEMS_INDIGO_PLATEAU    = 0x0816XXXX
ADDR.MART_ITEMS_TRAINER_TOWER     = 0x0816XXXX

-- Map group/num for warping to test marts
ADDR.MAP_GROUP_CERULEAN_MART      = 7
ADDR.MAP_NUM_CERULEAN_MART        = 7
ADDR.MAP_GROUP_CINNABAR_MART      = 7
ADDR.MAP_NUM_CINNABAR_MART        = 12
ADDR.MAP_GROUP_FOUR_ISLAND_MART   = 7
ADDR.MAP_NUM_FOUR_ISLAND_MART     = 35
ADDR.MAP_GROUP_SEVEN_ISLAND_MART  = 2
ADDR.MAP_NUM_SEVEN_ISLAND_MART    = 31
```

Fill in each `0x0816XXXX` / `0x0817XXXX` from the grep output.

- [ ] **Step 3: Commit**

```bash
git add test/lib/addresses.lua
git commit -m "test: add mart item list ROM addresses to addresses.lua"
```

---

### Task 7: Write Lua test

**Files:**
- Create: `test/tests/test_pp_items_in_marts.lua`

- [ ] **Step 1: Create the test file**

```lua
-------------------------------------------------------------------------------
-- test_pp_items_in_marts.lua
-------------------------------------------------------------------------------
-- Verifies that PP-restoring items were added to the correct shops at the
-- correct positions (immediately after their anchor HP item).
--
-- Test A: Cerulean Mart — Ether + Max Ether after Super Potion
-- Test B: Cinnabar Mart — Elixir after Hyper Potion
-- Test C: Four Island Mart — Max Elixir after Max Potion
-- Test D: Seven Island Mart — Elixir after Hyper Potion AND Max Elixir after Max Potion
--
-- Assertions read the item list directly from ROM via the globally-linked
-- *_Items labels (no in-game menu navigation required for assertions).
-- Navigation to each mart is included for the recording/GIF.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")

-- Item ID constants
local ITEM_SUPER_POTION = 22
local ITEM_HYPER_POTION = 21
local ITEM_MAX_POTION   = 20
local ITEM_ETHER        = 34
local ITEM_MAX_ETHER    = 35
local ITEM_ELIXIR       = 36
local ITEM_MAX_ELIXIR   = 37

-- Read the full item list from a ROM address. Returns a Lua array of u16 IDs
-- in order, stopping at ITEM_NONE (0). ROM is mapped at 0x08000000 in GBA space.
local function read_mart_items(rom_addr)
    local items = {}
    local offset = 0
    while true do
        local id = emu:read16(rom_addr + offset)
        if id == 0 then break end
        table.insert(items, id)
        offset = offset + 2
    end
    return items
end

-- Assert that new_id appears at the slot immediately after anchor_id in items.
local function assert_item_after(items, anchor_id, new_id, label)
    for i = 1, #items do
        if items[i] == anchor_id then
            if items[i + 1] == new_id then
                fw.pass(label)
            else
                fw.fail(label .. ": expected item " .. new_id ..
                        " after anchor " .. anchor_id ..
                        " but got " .. tostring(items[i + 1]))
            end
            return
        end
    end
    fw.fail(label .. ": anchor item " .. anchor_id .. " not found in item list")
end

-- Walk from mart entry point (3, 7) up to clerk at (2, 3) and open the shop.
-- All test marts have the same layout: clerk at tile (2, 3), entry at (3, 7).
-- Sequence: walk UP 4 tiles, face LEFT (toward counter), press A twice
-- (once to dismiss "May I help you?", once to select BUY), then wait
-- for the buy menu, then press B to close.
local function open_and_view_shop()
    fw.hold("UP", 80)        -- walk from (3,7) up 4 tiles to (3,3), ~16 frames/tile + settle
    fw.wait_frames(30)
    fw.press("A")            -- face left + trigger clerk dialog ("May I help you?")
    fw.wait_frames(60)
    fw.press("A")            -- dismiss message, opens Buy/Sell/Quit menu
    fw.wait_frames(30)
    fw.press("A")            -- select "Buy", opens item list
    fw.wait_frames(120)      -- pause so recording shows the shop menu clearly
    fw.press("B")            -- close buy menu
    fw.wait_frames(30)
    fw.press("B")            -- close Buy/Sell/Quit menu
    fw.wait_frames(30)
    fw.press("B")            -- close "Please come again" message
    fw.wait_frames(60)
end

fw.run(function()
    cs.select_default_character()

    -- -------------------------------------------------------------------------
    -- Assertion phase: read ROM item lists, no navigation needed
    -- -------------------------------------------------------------------------

    -- Test A: Cerulean Mart (Super Potion tier)
    local cerulean = read_mart_items(ADDR.MART_ITEMS_CERULEAN)
    assert_item_after(cerulean, ITEM_SUPER_POTION, ITEM_ETHER,     "A1: Cerulean has Ether after Super Potion")
    assert_item_after(cerulean, ITEM_ETHER,        ITEM_MAX_ETHER,  "A2: Cerulean has Max Ether after Ether")

    -- Test B: Cinnabar Mart (Hyper Potion tier)
    local cinnabar = read_mart_items(ADDR.MART_ITEMS_CINNABAR)
    assert_item_after(cinnabar, ITEM_HYPER_POTION, ITEM_ELIXIR,    "B: Cinnabar has Elixir after Hyper Potion")

    -- Test C: Four Island Mart (Max Potion tier)
    local four_island = read_mart_items(ADDR.MART_ITEMS_FOUR_ISLAND)
    assert_item_after(four_island, ITEM_MAX_POTION, ITEM_MAX_ELIXIR, "C: Four Island has Max Elixir after Max Potion")

    -- Test D: Seven Island Mart (dual tier)
    local seven_island = read_mart_items(ADDR.MART_ITEMS_SEVEN_ISLAND)
    assert_item_after(seven_island, ITEM_HYPER_POTION, ITEM_ELIXIR,     "D1: Seven Island has Elixir after Hyper Potion")
    assert_item_after(seven_island, ITEM_MAX_POTION,   ITEM_MAX_ELIXIR,  "D2: Seven Island has Max Elixir after Max Potion")

    -- -------------------------------------------------------------------------
    -- Visual phase: navigate to each mart for the recording
    -- -------------------------------------------------------------------------

    warp.warp_to(ADDR.MAP_GROUP_CERULEAN_MART, ADDR.MAP_NUM_CERULEAN_MART, 0)
    open_and_view_shop()

    warp.warp_to(ADDR.MAP_GROUP_CINNABAR_MART, ADDR.MAP_NUM_CINNABAR_MART, 0)
    open_and_view_shop()

    warp.warp_to(ADDR.MAP_GROUP_FOUR_ISLAND_MART, ADDR.MAP_NUM_FOUR_ISLAND_MART, 0)
    open_and_view_shop()

    warp.warp_to(ADDR.MAP_GROUP_SEVEN_ISLAND_MART, ADDR.MAP_NUM_SEVEN_ISLAND_MART, 0)
    open_and_view_shop()

    fw.finish()
end)
```

- [ ] **Step 2: Commit**

```bash
git add test/tests/test_pp_items_in_marts.lua
git commit -m "test: add Lua test for PP items in marts"
```

---

### Task 8: Run test headless

**Files:** none

- [ ] **Step 1: Run**

```bash
bash test/run_test.sh test/tests/test_pp_items_in_marts.lua
```

Expected output contains:
```
[PASS] A1: Cerulean has Ether after Super Potion
[PASS] A2: Cerulean has Max Ether after Ether
[PASS] B: Cinnabar has Elixir after Hyper Potion
[PASS] C: Four Island has Max Elixir after Max Potion
[PASS] D1: Seven Island has Elixir after Hyper Potion
[PASS] D2: Seven Island has Max Elixir after Max Potion
```
Exit code: 0.

If `open_and_view_shop()` gets stuck (dialog timing, wrong tile), adjust `fw.hold("UP", N)` frame count and retry. The ROM assertions do not depend on navigation — they will pass/fail independently.

---

### Task 9: Record test and produce GIF

**Files:** none (recording stored externally)

- [ ] **Step 1: Record**

```bash
bash test/record_test.sh test/tests/test_pp_items_in_marts.lua
```

- [ ] **Step 2: Move recording**

```bash
mv ~/recordings/test_pp_items_in_marts* ~/recordings/
```

- [ ] **Step 3: Convert to GIF**

Check what format the recording is in (`ls ~/recordings/`). If it's an `.mp4` or `.mkv`, convert with:
```bash
ffmpeg -i ~/recordings/test_pp_items_in_marts.mp4 \
  -vf "fps=20,scale=480:-1:flags=lanczos" \
  -loop 0 docs/pp_items_in_marts.gif
```
Adjust filename to match actual output. Place the GIF inside the repo at `docs/pp_items_in_marts.gif`.

---

### Task 10: Update README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add feature entry**

Find the custom features section in `README.md` and add:

```markdown
### PP Items in Marts
Ether and Max Ether are now available in shops that sell Super Potion; Elixir in
shops that sell Hyper Potion; Max Elixir in shops that sell Max Potion. PP
management is no longer limited to overworld pickups.

![PP items in marts demo](docs/pp_items_in_marts.gif)
```

- [ ] **Step 2: Commit**

```bash
git add README.md docs/pp_items_in_marts.gif
git commit -m "docs: add PP items in marts to README with demo GIF"
```

---

### Task 11: Update GitHub issue with plan results

**Files:** none

- [ ] **Step 1: Edit issue description to capture scope**

```bash
gh issue edit 9 --body "$(cat <<'EOF'
## Feature

Add PP-restoring items to Pokémart shops, mirroring the tier of HP restorer sold there.

## Scope

| HP item sold | PP item(s) added | Shops |
|---|---|---|
| Super Potion | Ether + Max Ether | Cerulean, Lavender, Vermilion, Fuchsia, Celadon Dept Store 2F |
| Hyper Potion | Elixir | Cinnabar, Saffron, Three Island, Seven Island, Trainer Tower |
| Max Potion | Max Elixir | Four Island, Six Island, Indigo Plateau, Seven Island, Trainer Tower |

Items appear immediately after their anchor potion in each shop list.

## Design doc
`docs/superpowers/specs/2026-04-24-pp-items-in-marts-design.md`
EOF
)"
```

- [ ] **Step 2: Post plan summary as a comment**

```bash
gh issue comment 9 --body "$(cat <<'EOF'
**Implementation plan agreed.** Changes are pure data (`.inc` files only, no C code):

- 5 files get Ether + Max Ether inserted after Super Potion
- 5 files get Elixir inserted after Hyper Potion (3 unique + 2 shared with Max Potion tier)
- 5 files get Max Elixir inserted after Max Potion (3 unique + 2 shared)

Verification: Lua test reads item lists directly from ROM, asserts correct position for 4 representative shops. Test run doubles as the recording for the README GIF.

Plan: `docs/superpowers/plans/2026-04-24-pp-items-in-marts.md`
EOF
)"
```

---

### Task 12: Create PR

**Files:** none

- [ ] **Step 1: Push branch**

```bash
git push -u origin issue-9-pp-items-in-marts
```

- [ ] **Step 2: Create PR**

```bash
gh pr create \
  --title "Fix #9: Add PP items to mart shops" \
  --body "$(cat <<'EOF'
## Summary

- Ether + Max Ether added to all shops that sell Super Potion (5 shops)
- Elixir added to all shops that sell Hyper Potion (5 shops)
- Max Elixir added to all shops that sell Max Potion (5 shops)
- Seven Island and Trainer Tower get both Elixir and Max Elixir (dual-tier)

Pure data change — no C code modified, no item definitions changed (all four items already had prices set).

## Test plan

- [x] `make -j$(nproc) firered` — clean build
- [x] `bash test/run_test.sh test/tests/test_pp_items_in_marts.lua` — 6 assertions pass
- [x] Recording produced and GIF added to README

Closes #9
EOF
)"
```

- [ ] **Step 3: Wait for approval before merging**

Do not run `gh pr merge` until the user explicitly approves.
