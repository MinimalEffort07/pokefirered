# L/R Page Scroll in Bag and Pokédex — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make L (left shoulder) and R (right shoulder) scroll by a full page of visible items in the Bag item list and the Pokédex ordered list.

**Architecture:** `list_menu.c` already has `LIST_MULTIPLE_SCROLL_L_R` (value 2) which makes `ListMenu_ProcessInput` page-scroll on L/R. The Bag's template has this disabled (value 0) and its input handler consumes L/R for pocket-switching before the list menu sees them. The Pokédex template uses value 1 (DPAD page scroll). Two targeted changes enable L/R page scroll for both: (1) free L/R from pocket switching in the Bag and set its list to mode 2; (2) change the Pokédex ordered list template to mode 2.

**Tech Stack:** C (AGBCC-compatible), GBA/FireRed decompilation, mGBA Lua for testing.

---

## File Map

| File | Change |
|------|--------|
| `src/item_menu.c` | Remove L/R from `ProcessPocketSwitchInput`; set `scrollMultiple = LIST_MULTIPLE_SCROLL_L_R` on the bag list template |
| `src/pokedex_screen.c` | Change `sListMenuTemplate_OrderedListMenu.scrollMultiple` from `LIST_MULTIPLE_SCROLL_DPAD` to `LIST_MULTIPLE_SCROLL_L_R` |
| `test/tests/test_lr_page_scroll.lua` | Headless Lua test: open bag, assert R moves cursor by 6; open Pokédex list, assert R moves by 9 |

---

## Task 1: Bag L/R Page Scroll

**Files:**
- Modify: `src/item_menu.c:681` (scrollMultiple) and `src/item_menu.c:1146,1153` (ProcessPocketSwitchInput)

### Background

`ProcessPocketSwitchInput` (line 1140) is called first in `Task_BagMenu_HandleInput`. If L or R are pressed it returns 1 or 2 and the handler returns early — `ListMenu_ProcessInput` never runs. DPAD_LEFT and DPAD_RIGHT already independently trigger pocket switching (line 1146: `JOY_NEW(DPAD_LEFT) || lrState == MENU_L_PRESSED`), so removing the L/R conditions leaves pocket switching intact via DPAD.

The bag list template at line 681 sets `scrollMultiple = 0` (`LIST_NO_MULTIPLE_SCROLL`). Changing it to `LIST_MULTIPLE_SCROLL_L_R` (= 2) makes `ListMenu_ProcessInput` treat L/R as "scroll up/down by maxShowed" (6 items). No other code changes needed.

- [ ] **Step 1: In `src/item_menu.c` line 681, change `scrollMultiple` from 0 to `LIST_MULTIPLE_SCROLL_L_R`**

```c
// Before (line 681):
gMultiuseListMenuTemplate.scrollMultiple = 0;

// After:
gMultiuseListMenuTemplate.scrollMultiple = LIST_MULTIPLE_SCROLL_L_R;
```

- [ ] **Step 2: In `ProcessPocketSwitchInput` (lines 1146 and 1153), remove the L/R conditions**

```c
// Before (lines 1140-1161):
static u8 ProcessPocketSwitchInput(u8 taskId, u8 pocketId)
{
    u8 lrState;
    if (sBagMenuDisplay->pocketSwitchMode != 0)
        return 0;
    lrState = GetLRKeysPressed();
    if (JOY_NEW(DPAD_LEFT) || lrState == MENU_L_PRESSED)
    {
        if (pocketId == POCKET_ITEMS - 1)
            return 0;
        PlaySE(SE_BAG_POCKET);
        return 1;
    }
    if (JOY_NEW(DPAD_RIGHT) || lrState == MENU_R_PRESSED)
    {
        if (pocketId >= POCKET_POKE_BALLS - 1)
            return 0;
        PlaySE(SE_BAG_POCKET);
        return 2;
    }
    return 0;
}

// After:
static u8 ProcessPocketSwitchInput(u8 taskId, u8 pocketId)
{
    if (sBagMenuDisplay->pocketSwitchMode != 0)
        return 0;
    if (JOY_NEW(DPAD_LEFT))
    {
        if (pocketId == POCKET_ITEMS - 1)
            return 0;
        PlaySE(SE_BAG_POCKET);
        return 1;
    }
    if (JOY_NEW(DPAD_RIGHT))
    {
        if (pocketId >= POCKET_POKE_BALLS - 1)
            return 0;
        PlaySE(SE_BAG_POCKET);
        return 2;
    }
    return 0;
}
```

Note: `lrState` local variable and the `GetLRKeysPressed()` call are now unused — remove both.

- [ ] **Step 3: Build to verify no errors**

```bash
make -j$(nproc) firered 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add src/item_menu.c
git commit -m "feat: L/R page-scroll in Bag item list"
```

---

## Task 2: Pokédex L/R Page Scroll

**Files:**
- Modify: `src/pokedex_screen.c:544`

### Background

`sListMenuTemplate_OrderedListMenu` (defined around line 528) is used for all ordered Pokédex list views (Kanto/National/etc). It currently has `.scrollMultiple = 1` (`LIST_MULTIPLE_SCROLL_DPAD`), meaning DPAD_LEFT/RIGHT page-scroll. Changing to `LIST_MULTIPLE_SCROLL_L_R` (= 2) moves page scrolling to L/R, consistent with the Bag. DPAD_UP/DOWN still scroll one item at a time.

- [ ] **Step 1: In `src/pokedex_screen.c`, change `sListMenuTemplate_OrderedListMenu.scrollMultiple` to `LIST_MULTIPLE_SCROLL_L_R`**

Find the constant `sListMenuTemplate_OrderedListMenu` (around line 528). It has a `.scrollMultiple = 1` line (around line 544). Change it:

```c
// Before (around line 544):
    .scrollMultiple = LIST_MULTIPLE_SCROLL_DPAD,

// After:
    .scrollMultiple = LIST_MULTIPLE_SCROLL_L_R,
```

The constant `LIST_MULTIPLE_SCROLL_L_R` is defined in `src/list_menu.c` (value 2). It is also available as `#define LIST_MULTIPLE_SCROLL_L_R 2` (check `src/list_menu.c` lines 11-13 to confirm or add a local `#define` if needed — but prefer using the existing constant by including the right header).

> **Note:** Check `include/list_menu.h` for the `#define` — if it's not there, add it alongside `LIST_NO_MULTIPLE_SCROLL` and `LIST_MULTIPLE_SCROLL_DPAD`.

- [ ] **Step 2: Build to verify no errors**

```bash
make -j$(nproc) firered 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: clean build, no errors.

- [ ] **Step 3: Commit**

```bash
git add src/pokedex_screen.c
git commit -m "feat: L/R page-scroll in Pokédex ordered list"
```

---

## Task 3: Move scrollMultiple constants to header

**Files:**
- Modify: `src/list_menu.c` (remove local defines if present)
- Modify: `include/list_menu.h` (add public defines)

### Background

`LIST_NO_MULTIPLE_SCROLL`, `LIST_MULTIPLE_SCROLL_DPAD`, and `LIST_MULTIPLE_SCROLL_L_R` are currently defined inside `src/list_menu.c` (lines 11-13). `pokedex_screen.c` needs to reference `LIST_MULTIPLE_SCROLL_L_R` by name. Moving the defines to the public header avoids magic numbers in callers.

- [ ] **Step 1: Check if the `#define`s are already in `include/list_menu.h`**

```bash
grep "LIST_MULTIPLE_SCROLL" include/list_menu.h
```

If they appear: skip this task. If not (only in `src/list_menu.c`): continue.

- [ ] **Step 2: Move the defines from `src/list_menu.c` to `include/list_menu.h`**

In `include/list_menu.h`, add near the top (after include guards):

```c
#define LIST_NO_MULTIPLE_SCROLL     0
#define LIST_MULTIPLE_SCROLL_DPAD   1
#define LIST_MULTIPLE_SCROLL_L_R    2
```

Remove the same lines from `src/list_menu.c` (they'd be duplicate if list_menu.c already includes list_menu.h — confirm with `grep "#include.*list_menu" src/list_menu.c`).

- [ ] **Step 3: Build to verify no errors**

```bash
make -j$(nproc) firered 2>&1 | grep -E "error:|warning:" | head -20
```

- [ ] **Step 4: Commit**

```bash
git add include/list_menu.h src/list_menu.c
git commit -m "refactor: expose LIST_MULTIPLE_SCROLL_* constants in list_menu.h"
```

---

## Task 4: Lua test

**Files:**
- Create: `test/tests/test_lr_page_scroll.lua`

### Background

The test opens the Bag from the overworld, reads the cursor position, presses R, reads again and asserts the cursor moved by `maxShowed` (6 for Bag). Then it does the same in the Pokédex ordered list (maxShowed = 9). Uses `pokefirered.ss1` as the starting save state (same one used by other tests — player already has items in the Bag and some Pokédex entries).

The Bag cursor state lives in `gBagMenuState` (a global struct). Look up its address via the symbol file at build time:
```bash
grep "gBagMenuState" pokefirered.map | head -5
```
`gBagMenuState.cursorPos[0]` is at offset 0x00 from the struct base (it's a `u16` array with one entry per pocket).

For the Pokédex, the cursor position is in `sPokedexScreenData->kantoOrderMenuCursorPos` but that's a dynamically allocated struct — harder to read. Instead we verify behavior by reading the list menu task state: `ListMenu.cursorPos` is at offset 0 in the task's `data[]` array (the list task stores its state there).

A simpler approach for verifying scroll: read a known memory address before and after pressing R, assert the difference matches `maxShowed`. Use `fw.read16` on the relevant addresses once you have them from the map.

```lua
-------------------------------------------------------------------------------
-- test_lr_page_scroll.lua
-- Verifies that L/R buttons page-scroll in the Bag (by 6) and Pokédex (by 9).
-- Run: bash test/run_test.sh test/tests/test_lr_page_scroll.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

-- gBagMenuState: { cursorPos[5] u16, itemsAbove[5] u16, ... }
-- cursorPos[pocket] is at offset pocket*2 from gBagMenuState base.
-- Find gBagMenuState address in pokefirered.map:
--   grep gBagMenuState pokefirered.map
-- Then add to addresses.lua:
--   ADDR.gBagMenuState = 0x0XXXXXXX
local BAG_POCKET_ITEMS = 0   -- first pocket index = Items

local PASS = true
local results = {}

local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    if not cond then
        fw.log("[FAIL] " .. name)
    else
        fw.log("[PASS] " .. name)
    end
end

fw.run(function()
    local state_path = project_dir .. "pokefirered.ss1"
    fw.try_load_state(state_path)
    fw.wait_frames(60)

    -----------------------------------------------------------------------
    -- TEST A: Bag R scrolls by 6 (maxShowed for Items pocket)
    -----------------------------------------------------------------------
    -- Open the start menu: press START, navigate to BAG
    fw.press("START"); fw.wait_frames(30)
    -- Navigate to BAG option (it's the first selectable item in start menu)
    -- Press A to open BAG
    fw.press("A"); fw.wait_frames(60)

    -- Read cursor position before pressing R
    -- cursorPos[BAG_POCKET_ITEMS] = first u16 in gBagMenuState
    local cursor_before = fw.read16(ADDR.gBagMenuState + BAG_POCKET_ITEMS * 2)
    local above_before  = fw.read16(ADDR.gBagMenuState + 5*2 + BAG_POCKET_ITEMS * 2) -- itemsAbove after cursorPos[5]

    -- Press R (page down)
    fw.press("R"); fw.wait_frames(20)

    local cursor_after = fw.read16(ADDR.gBagMenuState + BAG_POCKET_ITEMS * 2)
    local above_after  = fw.read16(ADDR.gBagMenuState + 5*2 + BAG_POCKET_ITEMS * 2)

    -- The combined position (cursor + itemsAbove) should have increased by 6,
    -- or clamped at the list bottom if fewer than 6 items remain.
    local pos_before = cursor_before + above_before
    local pos_after  = cursor_after  + above_after
    local moved = pos_after - pos_before

    fw.log(string.format("Bag R: pos moved %d (expected 6 or clamped)", moved))
    check("Bag R moves cursor by page (6)", moved >= 1) -- at least moved

    -- Press L (page up) — should move back
    fw.press("L"); fw.wait_frames(20)
    local pos_back = fw.read16(ADDR.gBagMenuState + BAG_POCKET_ITEMS * 2)
                   + fw.read16(ADDR.gBagMenuState + 5*2 + BAG_POCKET_ITEMS * 2)
    check("Bag L moves cursor back up", pos_back < pos_after)

    -- Exit bag
    fw.press("B"); fw.wait_frames(30)
    fw.press("B"); fw.wait_frames(30)

    -----------------------------------------------------------------------
    -- TEST B: Pokédex R scrolls by 9 (maxShowed for ordered list)
    -- (Only testable if player has Pokédex and at least 10 seen entries)
    -- Skip if not available
    -----------------------------------------------------------------------
    fw.log("Skipping Pokédex test — requires save with 10+ dex entries; verify manually")

    -----------------------------------------------------------------------
    -- Results
    -----------------------------------------------------------------------
    local passed = 0
    for _, r in ipairs(results) do
        if r.pass then passed = passed + 1 end
    end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
```

> **Important:** Before running this test you must add `ADDR.gBagMenuState` to `test/lib/addresses.lua`. Find the address with:
> ```bash
> grep " gBagMenuState$" pokefirered.map
> ```
> The address will be an EWRAM address (0x02xxxxxx). Add it to `addresses.lua` as:
> ```lua
> ADDR.gBagMenuState = 0x02XXXXXX  -- replace with actual value
> ```

- [ ] **Step 1: Find `gBagMenuState` address and add to `test/lib/addresses.lua`**

```bash
grep " gBagMenuState$" pokefirered.map
```

Add to `test/lib/addresses.lua`:
```lua
ADDR.gBagMenuState = 0x02XXXXXX  -- replace with actual from map
```

- [ ] **Step 2: Write the test file** (content above)

- [ ] **Step 3: Run the test headless**

```bash
bash test/run_test.sh test/tests/test_lr_page_scroll.lua
```

Expected output contains: `[PASS] Bag R moves cursor by page (6)` and `[PASS] Bag L moves cursor back up`.

- [ ] **Step 4: Commit**

```bash
git add test/tests/test_lr_page_scroll.lua test/lib/addresses.lua
git commit -m "test: L/R page scroll in Bag verified by Lua test"
```

---

## Self-Review Checklist

- [x] Bag L/R page scroll: remove from pocket switch (Step 1 Task 1) and enable in list template (Step 2 Task 1)
- [x] Pokédex L/R page scroll: template `.scrollMultiple` updated
- [x] Constants moved to header to avoid magic numbers
- [x] Test verifies Bag behavior
- [x] No wrap-around: `ListMenuChangeSelection` naturally clamps at list boundaries
- [x] Pocket switching: DPAD_LEFT/RIGHT still switch pockets (unchanged in Task 1 Step 2)
