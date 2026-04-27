-------------------------------------------------------------------------------
-- test_follower_save.lua
--
-- Pre-fix baseline: documents the walking-pokemon save/load bug.
--
--   Test 1 (main fix):  follower active at save-time → follower slot should be
--                       cleared from gSaveBlock1->objectEvents (no frozen sprite
--                       on load). FAILS before fix (follower IS written to SB1).
--   Test 2 (regression): no follower at save-time → no localId=254 slot active
--                         in save block. Should PASS both before and after fix.
--
-- Note: Test 3 (warp regression) was intentionally omitted. It tests post-fix
-- behaviour (DespawnFollowerSprite + SpawnFollowerSprite cycle survives a map
-- warp). The fixture starts in Player's House 2F (group=4, num=1); reaching an
-- outdoor map requires traversing 3 consecutive warps that the framework cannot
-- reliably navigate from that starting position. Test 3 can be added in a
-- follow-up when a suitable outdoor fixture is available.
--
-- FIXTURE NOTES:
--   The save state is captured just after entering the overworld as the default
--   character (Player's House 2F, map group=4 num=1). After loading the state,
--   the game has a pending "Mom" dialog — one B press dismisses it and puts
--   the player in the fully-interactive overworld state. Every test scenario
--   must call dismiss_pending_dialog() before doing any other input.
--
-- START MENU ORDER (with FLAG_SYS_POKEMON_GET set, no POKEDEX flag):
--   0: POKEMON, 1: BAG, 2: PLAYER, 3: SAVE, 4: MULTIPLAYER, 5: OPTION, 6: EXIT
--   → SAVE requires DOWN × 3 from the top of the menu
--
-- PARTY ACTION MENU for a single Pikachu (no field moves, party size=1):
--   0: SUMMARY, 1: WALK, 2: ITEM, 3: MOVE TO PC, 4: CANCEL
--   → WALK requires DOWN × 1 from SUMMARY
--
-- objectEvents[] offset in SaveBlock1:
--   Stale comment in global.h says 0x06A0. This fork expanded registeredItem
--   (u16, 2 bytes) to registeredItems[REGISTERED_ITEMS_MAX=20] (40 bytes),
--   adding 38 bytes; plus a 2-byte compiler alignment pad before pcItems
--   gives +0x28 total shift. Verified empirically: player ObjectEvent flags
--   appear at SB1+0x6C8 after a successful save.
--
-- Run:
--   bash test/run_test.sh test/tests/test_follower_save.lua
-- Temp recording:
--   bash test/record_test.sh test/tests/test_follower_save.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH       = "/tmp/pokefirered-follower-save-base.ss"
local PARTY_BASE       = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE     = 100         -- sizeof(struct Pokemon)
local LOCALID_FOLLOWER = 254         -- LOCALID_FOLLOWER from constants/event_objects.h
-- Offset of objectEvents[] within SaveBlock1.
-- Stale global.h comment: 0x06A0.  Actual offset after registeredItems expansion
-- (+38 bytes) plus 2-byte alignment pad = +0x28 total → 0x06C8.
-- Confirmed empirically: player ObjectEvent flags appear at this offset in
-- gSaveBlock1Ptr after a successful SaveObjectEvents() call.
local SB1_OBJECT_EVENTS = 0x06C8

-------------------------------------------------------------------------------
-- Helpers
-------------------------------------------------------------------------------

-- Inject a minimal valid Pikachu (species 25) into party slot 0.
-- Uses PID=0, OTID=0 so XOR key=0 (substruct order 0=GAEM, no encryption).
local function inject_pikachu()
    local base = PARTY_BASE
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)   -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)   -- no mail
    emu:write8(base + 0x20, 25)     -- species=25 (Pikachu) low byte
    emu:write8(base + 0x21, 0)      -- species high byte
    emu:write8(base + 0x1C, 25)     -- checksum = species when only species non-zero
    emu:write8(base + 0x1D, 0)
    emu:write8(base + 0x54, 5)      -- level = 5
    emu:write8(base + 0x56, 20)     -- currentHP low
    emu:write8(base + 0x58, 20)     -- maxHP low
    emu:write8(ADDR.gPlayerPartyCount, 1)
end

-- Set FLAG_SYS_POKEMON_GET (0x828) so the Start menu shows POKEMON.
-- FLAG byte: index = 0x828/8 = 0x105, bit = 0x828 % 8 = 0.
local function set_pokemon_flag()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
    emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
end

-- Dismiss the pending "Mom" dialog present at the start of a new game.
-- The game saves state just after confirm_and_enter_overworld(), which leaves
-- a Mom speech dialog pending. One B press dismisses it and makes the player
-- ObjectEvent active in the overworld. We use B (not A) to avoid accidentally
-- navigating any menu that might open.
local function dismiss_pending_dialog()
    fw.press("B")
    fw.wait_frames(60)
end

-- Return the gObjectEvents slot index that has both active=1 and
-- localId=LOCALID_FOLLOWER, or -1 if none exists.
local function find_follower_slot()
    for i = 0, ADDR.OBJECT_EVENTS_COUNT - 1 do
        local base = ADDR.gObjectEvents + i * ADDR.OBJECT_EVENT_SIZE
        local flags = fw.read32(base + ADDR.OE_FLAGS)
        local localId = fw.read8(base + ADDR.OE_LOCAL_ID)
        if (flags & 1) == 1 and localId == LOCALID_FOLLOWER then
            return i
        end
    end
    return -1
end

-- Read the localId for save-block objectEvents slot `i`.
-- This reads directly from gSaveBlock1Ptr->objectEvents[i].
local function saveblock_slot_localid(i)
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local oe_base = sb1 + SB1_OBJECT_EVENTS + i * ADDR.OBJECT_EVENT_SIZE
    return fw.read8(oe_base + ADDR.OE_LOCAL_ID)
end

-- Read the active bit for slot `i` in gSaveBlock1Ptr->objectEvents[].
-- Returns true if that save-block slot is marked active.
local function saveblock_slot_active(i)
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local oe_base = sb1 + SB1_OBJECT_EVENTS + i * ADDR.OBJECT_EVENT_SIZE
    local flags = fw.read32(oe_base + ADDR.OE_FLAGS)
    return (flags & 1) ~= 0
end

-- Navigate the party menu to activate the follower for slot 0.
-- Precondition: player is in the fully-interactive overworld (dismiss_pending_dialog
-- must have been called first).
-- Party action menu for a single Pikachu (has walking sprite, no field moves,
-- no SWITCH since party size = 1):
--   0: SUMMARY, 1: WALK, 2: ITEM, 3: MOVE TO PC, 4: CANCEL
-- START → POKEMON (index 0, no POKEDEX flag) → A (open party)
-- → A (slot 0 action menu) → DOWN (SUMMARY→WALK) → A (confirm)
-- → B (close party) → B (close start)
local function activate_follower_via_menu()
    fw.press("START")
    fw.wait_frames(120)   -- wait for start menu to fully open
    fw.press("A")         -- select POKEMON (first entry)
    fw.wait_frames(180)   -- party menu load (full fade + render)
    fw.press("A")         -- open slot 0 action menu
    fw.wait_frames(120)   -- action menu animation
    fw.press("DOWN")      -- SUMMARY → WALK
    fw.wait_frames(12)
    fw.press("A")         -- confirm WALK → spawns follower
    fw.wait_frames(120)   -- follower spawn + animation settle
    fw.press("B")         -- close party menu
    fw.wait_frames(60)
    fw.press("B")         -- close Start menu
    fw.wait_frames(60)
end

-- Trigger in-game save via Start menu.
-- Precondition: player is in the fully-interactive overworld.
-- Normal field start menu order (with FLAG_SYS_POKEMON_GET set):
--   0: POKEMON, 1: BAG, 2: PLAYER, 3: SAVE, 4: MULTIPLAYER, 5: OPTION, 6: EXIT
-- START → DOWN × 3 (cursor on SAVE) → A (open save dialog) → A (Yes) → wait
--
-- Timing notes (all empirically verified — shorter waits cause the save to not fire):
--   120f after START: list menu must fully render before DOWN is registered
--   60f between DOWNs: cursor animation between list items
--   200f after first A: "Would you like to save?" text must finish printing before second A
--   600f after second A: flash write + "Game saved." message
local function ingame_save()
    fw.press("START")
    fw.wait_frames(120)
    fw.press("DOWN")      -- POKEMON → BAG
    fw.wait_frames(60)
    fw.press("DOWN")      -- BAG → PLAYER
    fw.wait_frames(60)
    fw.press("DOWN")      -- PLAYER → SAVE
    fw.wait_frames(60)
    fw.press("A")         -- open Save menu ("Would you like to save?")
    fw.wait_frames(200)   -- wait for dialog text to finish printing
    fw.press("A")         -- confirm YES
    fw.wait_frames(600)   -- flash write + "Game saved." result screen
    fw.press("B")         -- close Start menu
    fw.wait_frames(60)
end

-------------------------------------------------------------------------------
-- Test runner
-------------------------------------------------------------------------------

local results = {}
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.log((cond and "[PASS] " or "[FAIL] ") .. name)
end

fw.run(function()
    fw.log("=== Follower Save/Load Test ===")

    -- ------------------------------------------------------------------ --
    -- Fixture: boot to overworld as default character (cached after first run)
    -- The state is captured just after entering the overworld, before
    -- dismissing the Mom dialog. Each test scenario calls
    -- dismiss_pending_dialog() after loading this state.
    -- ------------------------------------------------------------------ --
    if not fw.try_load_state(STATE_PATH) then
        fw.log("No cached state — booting to overworld...")
        local sel = cs.find_selection_press()
        if not sel then
            fw.log_error("character select discovery failed")
            fw.finish()
            return
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()
        fw.wait_frames(120)   -- let overworld settle before saving state
        fw.save_state(STATE_PATH)
    end
    fw.wait_frames(60)

    -- ------------------------------------------------------------------ --
    -- Test 1: follower active at save → slot cleared from save block
    -- Expected: FAIL before fix (follower IS written to save block)
    -- Expected: PASS after fix (follower cleared from save block)
    -- ------------------------------------------------------------------ --
    fw.log("--- Test 1: follower slot cleared in save block ---")
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(60)
    dismiss_pending_dialog()
    inject_pikachu()
    set_pokemon_flag()
    fw.wait_frames(5)

    activate_follower_via_menu()
    fw.wait_frames(30)

    local slot = find_follower_slot()
    fw.log(string.format("Follower ObjectEvent slot before save: %d", slot))
    check("Test 1a: follower ObjectEvent active before save", slot ~= -1)

    if slot ~= -1 then
        ingame_save()

        -- After SaveObjectEvents() copies gObjectEvents[] to gSaveBlock1Ptr->objectEvents[],
        -- the follower's slot (localId=254, active=1) should NOT be present if the fix
        -- (ClearFollowerFromSaveBlock) is applied. Without the fix it IS present.
        local sb1_now = fw.read32(ADDR.gSaveBlock1Ptr)
        local slot0_flags = fw.read32(sb1_now + SB1_OBJECT_EVENTS)
        local slot1_flags = fw.read32(sb1_now + SB1_OBJECT_EVENTS + ADDR.OBJECT_EVENT_SIZE)
        local slot1_lid   = fw.read8(sb1_now + SB1_OBJECT_EVENTS + ADDR.OBJECT_EVENT_SIZE + ADDR.OE_LOCAL_ID)
        fw.log(string.format("SB1+0x6C8: slot0_flags=0x%x slot1_flags=0x%x slot1_lid=%d",
            slot0_flags, slot1_flags, slot1_lid))
        local player_slot_ok = (slot0_flags & 1) ~= 0
        local follower_in_sb = ((slot1_flags & 1) ~= 0) and (slot1_lid == LOCALID_FOLLOWER)
        check("Test 1 sanity: player slot active in SB1 (save fired)", player_slot_ok)
        check("Test 1b: follower slot NOT in save block after save", not follower_in_sb)
    end

    -- ------------------------------------------------------------------ --
    -- Test 2: no follower at save → no localId=254 slot active in save block
    -- (regression: the fix must not corrupt unrelated objectEvents slots)
    -- ------------------------------------------------------------------ --
    fw.log("--- Test 2: no follower active, save block clean ---")
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(60)
    dismiss_pending_dialog()
    inject_pikachu()
    set_pokemon_flag()
    fw.wait_frames(5)
    -- deliberately do NOT activate follower
    ingame_save()

    local any_follower_in_sb = false
    local sb1_t2 = fw.read32(ADDR.gSaveBlock1Ptr)
    for i = 0, ADDR.OBJECT_EVENTS_COUNT - 1 do
        local oe_base = sb1_t2 + SB1_OBJECT_EVENTS + i * ADDR.OBJECT_EVENT_SIZE
        local flags = fw.read32(oe_base + ADDR.OE_FLAGS)
        local lid = fw.read8(oe_base + ADDR.OE_LOCAL_ID)
        if (flags & 1) == 1 and lid == LOCALID_FOLLOWER then
            any_follower_in_sb = true
        end
    end
    check("Test 2: no follower localId in save block when follower was inactive",
          not any_follower_in_sb)

    -- ------------------------------------------------------------------ --
    -- Summary
    -- ------------------------------------------------------------------ --
    local passed, total = 0, #results
    for _, r in ipairs(results) do
        if r.pass then passed = passed + 1 end
    end
    fw.log(string.format("Results: %d/%d passed", passed, total))
    fw.finish()
end)
