-------------------------------------------------------------------------------
-- test_slaveless_hm_obstacle.lua
-- Issue #5: Slaveless HM system must prompt at Cut obstacles when the player
-- has the HM item + badge but no party Pokémon that knows the move.
--
-- FIXTURE STATE (self-healing):
--   Tries to load /tmp/pokefirered-slaveless-hm.ss — player at tile (11, 23)
--   in Viridian City, one tile NORTH of the CUT tree at (11, 24), with an
--   empty party and no HM items or badges set.
--
--   On first run, the test boots through Oak's speech and navigates to
--   Viridian City automatically. If navigation succeeds the fixture is saved
--   for subsequent runs. If it fails, see MANUAL SETUP below.
--
-- MANUAL SETUP (if automatic fixture creation fails):
--   1. Open the ROM in mgba-qt.
--   2. Play through Oak's speech (or load any save that is in Pallet Town).
--   3. Walk to Viridian City and stand at tile (11, 23) — one tile NORTH of
--      the CUT tree at (11, 24), flag=FLAG_TEMP_11.
--   4. In the mGBA scripting console run:
--        emu:saveStateFile("/tmp/pokefirered-slaveless-hm.ss")
--
-- TEST CASES:
--   A. No badge + no HM01 in bag: press A on tree → "can't cut" sign.
--      FLAG_TEMP_11 NOT set (tree stays standing).
--   B. Badge set + HM01 in bag, empty party: slaveless prompt → YES.
--      FLAG_TEMP_11 SET (tree was removed by the cut animation).
--
-- Run:    bash test/run_test.sh test/tests/test_slaveless_hm_obstacle.lua
-- Record: bash test/record_test.sh test/tests/test_slaveless_hm_obstacle.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH = "/tmp/pokefirered-slaveless-hm.ss"
local PARTY_BASE = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE = 100       -- sizeof(struct Pokemon)

-- Map packed IDs: (mapGroup << 8) | mapNum
local MAP_PALLET   = (3 * 256) + 0
local MAP_ROUTE1   = (3 * 256) + 19
local MAP_VIRIDIAN = (3 * 256) + 1

-- Cut tree in Viridian City: at tile (11, 24), player stands at (11, 23) (north).
-- FLAG_TEMP_11 = 17; set by removeobject when tree is cut.
local TREE_FLAG = 17

-- SaveBlock1 offsets (actual, after +0x28 expansion of registeredItems):
--   bagPocket_TMHM : stale 0x0464 + 0x28 = 0x048c
--   vars[]         : stale 0x1000 + 0x28 = 0x1028
-- flags[] offset: SB1_FLAGS = 0x0F08 (verified, stored in ADDR)
local SB1_BAG_TMHM  = 0x048c
local SB1_VARS      = 0x1028
-- VAR_REPEL_STEP_COUNT = 0x4020; index in vars[] = 0x4020 - 0x4000 = 0x20 = 32
local REPEL_VAR_IDX = 32

-- Item IDs
local ITEM_HM01 = 339   -- Cut

-- Flags (numeric values)
-- FLAG_BADGE02_GET = SYS_FLAGS + 0x21 = 0x800 + 0x21 = 0x821 = 2081
local FLAG_BADGE02_GET = 2081
-- FLAG_SYS_POKEMON_GET = SYS_FLAGS + 0x28 = 0x800 + 0x28 = 0x828 = 2088
local FLAG_SYS_POKEMON_GET = 2088

local function map_packed()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local g = fw.read8(sb1 + ADDR.SB1_LOC_MAP_GROUP)
    local n = fw.read8(sb1 + ADDR.SB1_LOC_MAP_NUM)
    return (g * 256) + n
end

local function read_pos()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y)
end

-- Write a minimal unencrypted Pokémon (PID=0, OTID=0, XOR key=0 → substruct
-- order GAEM). Used during navigation to enable the repel check.
local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)            -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)            -- no mail
    emu:write8(base + 0x20, species % 256)   -- Growth.species low byte
    emu:write8(base + 0x21, math.floor(species / 256))
    emu:write8(base + 0x1C, species % 256)   -- checksum = species (only non-zero field)
    emu:write8(base + 0x1D, math.floor(species / 256))
    emu:write8(base + 0x54, level)           -- level
    emu:write8(base + 0x56, 20)              -- currentHP
    emu:write8(base + 0x57, 0)
    emu:write8(base + 0x58, 20)             -- maxHP
    emu:write8(base + 0x59, 0)
end

-- Set a single flag bit in SaveBlock1.flags[].
local function set_flag(sb1, flag_idx)
    local byte_off = math.floor(flag_idx / 8)
    local bit_mask = 1 << (flag_idx % 8)
    local addr = sb1 + ADDR.SB1_FLAGS + byte_off
    emu:write8(addr, fw.read8(addr) | bit_mask)
end

-- Clear a single flag bit.
local function clear_flag(sb1, flag_idx)
    local byte_off = math.floor(flag_idx / 8)
    local bit_mask = 1 << (flag_idx % 8)
    local addr = sb1 + ADDR.SB1_FLAGS + byte_off
    emu:write8(addr, fw.read8(addr) & (~bit_mask & 0xFF))
end

-- Read a single flag bit.
local function read_flag(sb1, flag_idx)
    local byte_off = math.floor(flag_idx / 8)
    local bit_mask = 1 << (flag_idx % 8)
    return (fw.read8(sb1 + ADDR.SB1_FLAGS + byte_off) & bit_mask) ~= 0
end

-- Inject ITEM_HM01 (Cut, id=339) into the first slot of bagPocket_TMHM.
-- ItemSlot layout: {u16 itemId, u16 quantity} = 4 bytes.
-- IMPORTANT: bag quantities are XOR-encrypted with gSaveBlock2Ptr->encryptionKey
-- (see GetBagItemQuantity / SetBagItemQuantity in src/item.c). We must store
-- (1 XOR enc_key) & 0xFFFF so that GetBagItemQuantity decodes back to 1.
local SB2_ENCRYPTION_KEY = 0xF20  -- offsetof(SaveBlock2, encryptionKey)
-- gBagPockets[3].itemSlots pointer lives at gBagPockets + 3*8 (BagPocket = 8 bytes).
local GBAGPOCKETS_TMHM_SLOTS_ADDR = 0x02039920 + 3 * 8  -- = 0x02039938

local function inject_hm01(sb1)
    local sb2     = fw.read32(ADDR.gSaveBlock2Ptr)
    local enc_key = fw.read32(sb2 + SB2_ENCRYPTION_KEY)
    local slot0   = sb1 + SB1_BAG_TMHM
    emu:write16(slot0,     ITEM_HM01)                -- itemId = 339
    emu:write16(slot0 + 2, (1 ~ enc_key) & 0xFFFF)  -- encrypted quantity; decodes to 1
end

-- Clear ITEM_HM01 from bag slot 0.
local function remove_hm01(sb1)
    local slot0 = sb1 + SB1_BAG_TMHM
    emu:write16(slot0,     0)  -- ITEM_NONE
    emu:write16(slot0 + 2, 0)
end

-- Set VAR_REPEL_STEP_COUNT = n (activates repel for n steps).
local function set_repel(sb1, n)
    emu:write16(sb1 + SB1_VARS + REPEL_VAR_IDX * 2, n)
end

-- Build fixture: boot intro → navigate bedroom → house → Pallet → Route 1 → Viridian.
-- Saves player at (11, 25), empty party, no items/badges, to STATE_PATH.
local function build_fixture()
    fw.log("No cached state — booting through Oak's speech…")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log("ERROR: character select discovery failed")
        return false
    end

    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()

    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)

    -- Set FLAG_SYS_POKEMON_GET to skip the Pallet Town "Oak runs up" cutscene.
    -- Without this flag, Oak's contact-trigger script locks the player.
    set_flag(sb1, FLAG_SYS_POKEMON_GET)

    -- Inject a level-100 Bulbasaur so the repel check has a lead Pokémon.
    -- IsWildLevelAllowedByRepel returns TRUE (allow encounter) if party is empty,
    -- regardless of VAR_REPEL_STEP_COUNT. A lv100 lead repels all Route 1 Pokémon.
    inject_pokemon(PARTY_BASE, 1, 100)
    emu:write8(ADDR.gPlayerPartyCount, 1)
    set_repel(sb1, 9999)

    fw.wait_frames(15)

    fw.log(string.format("  [nav] start: map=0x%04X pos=(%d,%d)",
        map_packed(), read_pos()))

    -- ── Step 1: Exit bedroom (PalletTown_PlayersHouse_2F) ──────────────────
    -- Player spawns at tile (6, 6) = world (13, 13).
    -- Staircase metatile (MB_DOWN_LEFT_STAIR_WARP) at tile (10, 2) = world (17, 9).
    -- TryArrowWarp fires when player is ON world(17,9) moving LEFT (DIR_WEST).
    -- Approach: go RIGHT past staircase column to x>=18 (tile 11), walk UP
    -- to the staircase row y<=9 (tile 2), then step LEFT to trigger the warp.
    local map_2f = map_packed()
    fw.hold_until("RIGHT",
        function() local x, _ = read_pos(); return x >= 18 end, 150, 4)
    fw.wait_frames(16)
    fw.hold_until("UP",
        function() local _, y = read_pos(); return y <= 9 end, 200, 4)
    fw.wait_frames(16)
    fw.log(string.format("  [nav] at staircase approach: pos=(%d,%d)", read_pos()))
    -- Step LEFT from world(18,9)→world(17,9): DOWN_LEFT_STAIR_WARP + DIR_WEST fires.
    fw.hold_until("LEFT",
        function() return map_packed() ~= map_2f end, 60, 4)
    fw.wait_frames(180)  -- wait for stair animation + 1F map load

    fw.log(string.format("  [nav] after bedroom: map=0x%04X pos=(%d,%d)",
        map_packed(), read_pos()))

    -- ── Step 2: Exit house 1F ──────────────────────────────────────────────
    -- Player arrives at tile (10, 2) = world (17, 9) in 1F.
    -- Exit door tile (4, 8) = world (11, 15) has MB_SOUTH_ARROW_WARP (0x65).
    -- TryArrowWarp fires when player steps SOUTH onto world(11,15).
    -- Path: DOWN to row 7 (world y=14), LEFT to col 4 (world x=11), DOWN to (11,15).
    local map_1f = map_packed()
    fw.hold_until("DOWN",
        function() local _, y = read_pos(); return y >= 14 end, 200, 4)
    fw.wait_frames(16)
    fw.hold_until("LEFT",
        function() local x, _ = read_pos(); return x <= 11 end, 200, 4)
    fw.wait_frames(16)
    fw.log(string.format("  [nav] at door approach: pos=(%d,%d)", read_pos()))
    fw.screenshot("/tmp/mgba-slaveless-1f-door.png")
    -- Step SOUTH from world(11,14)→world(11,15): SOUTH_ARROW_WARP fires.
    fw.hold_until("DOWN",
        function() return map_packed() ~= map_1f end, 60, 4)
    fw.wait_frames(180)  -- wait for fade + Pallet Town load

    fw.log(string.format("  [nav] after house 1F: map=0x%04X (expect 0x%04X for Pallet)",
        map_packed(), MAP_PALLET))

    if map_packed() ~= MAP_PALLET then
        fw.log(string.format("ERROR: still not in Pallet Town (map=0x%04X)", map_packed()))
        fw.log("  → See MANUAL SETUP at the top of this test file.")
        return false
    end

    -- ── Step 3: Pallet Town → Route 1 ─────────────────────────────────────
    -- Player exits house to ~world(13,15)=tile(6,8). House door warp at (6,7)
    -- is directly above — holding UP re-enters the house. Walk RIGHT first to
    -- tile x=12 (world x=19) where column is clear all the way to Route 1.
    fw.log("  Clearing house door (walking right)…")
    fw.hold_until("RIGHT",
        function() local x, _ = read_pos(); return x >= 19 end, 200, 4)
    fw.wait_frames(16)
    fw.log("  Walking to Route 1…")
    fw.hold_until("UP",
        function() return map_packed() == MAP_ROUTE1 end,
        800, 8)
    fw.wait_frames(180)

    fw.log(string.format("  [nav] after Pallet: map=0x%04X (expect 0x%04X for Route1)",
        map_packed(), MAP_ROUTE1))

    if map_packed() ~= MAP_ROUTE1 then
        fw.log(string.format("ERROR: could not reach Route 1 (map=0x%04X)", map_packed()))
        fw.log("  → See MANUAL SETUP at the top of this test file.")
        return false
    end

    -- ── Step 4: Route 1 → Viridian City ───────────────────────────────────
    -- Route 1 is 24x40 with scattered obstacle rows. A straight-UP path is
    -- blocked at y=31, 25-26, 20, 16-15, and 5 (tile coords). Navigate using
    -- BFS-derived waypoints (world = tile + 7):
    --   y=39: step left to x=15, pass y=31 obstacle at x=8
    --   y=34: step right to x=19, pass y=25-26 at x=12
    --   y=28: step left to x=17, pass y=20 at x=10
    --   y=24: step right to x=23, pass y=16-15 and y=5 at x=16
    --   y=9:  step left to x=20, then exit to Viridian
    fw.log("  Walking to Viridian City…")
    fw.hold_until("UP",
        function() local _, y = read_pos(); return y <= 39 end, 300, 4)
    fw.wait_frames(8)
    fw.hold_until("LEFT",
        function() local x, _ = read_pos(); return x <= 15 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("UP",
        function() local _, y = read_pos(); return y <= 34 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("RIGHT",
        function() local x, _ = read_pos(); return x >= 19 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("UP",
        function() local _, y = read_pos(); return y <= 28 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("LEFT",
        function() local x, _ = read_pos(); return x <= 17 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("UP",
        function() local _, y = read_pos(); return y <= 24 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("RIGHT",
        function() local x, _ = read_pos(); return x >= 23 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("UP",
        function() local _, y = read_pos(); return y <= 9 end, 400, 4)
    fw.wait_frames(8)
    fw.hold_until("LEFT",
        function() local x, _ = read_pos(); return x <= 20 end, 200, 4)
    fw.wait_frames(8)
    fw.hold_until("UP",
        function() return map_packed() == MAP_VIRIDIAN end, 400, 8)
    fw.wait_frames(180)

    fw.log(string.format("  [nav] after Route 1: map=0x%04X (expect 0x%04X for Viridian)",
        map_packed(), MAP_VIRIDIAN))

    if map_packed() ~= MAP_VIRIDIAN then
        fw.log(string.format("ERROR: could not reach Viridian City (map=0x%04X)", map_packed()))
        fw.log("  → See MANUAL SETUP at the top of this test file.")
        return false
    end

    -- ── Step 5: Navigate within Viridian City to tile(11, 23) ─────────────
    -- Player enters from Route 1 at tile(25,39)=world(32,46). Target is
    -- tile(11,23)=world(18,30): one tile NORTH of the CUT tree at tile(11,24).
    -- We stop here (not south of tree) so the tree's OBJ_EVENT stays in the
    -- active pool — a large memory-injection jump would flush the pool.
    -- BFS waypoints avoid the pond (water tiles at tile y=25-28, world y=32-35):
    --   UP  to world y≤38 (tile y=31, clears south Viridian obstacles)
    --   LEFT to world x≤29 (tile x=22, moves west past eastern cluster)
    --   UP  to world y≤30 (tile y=23, gets above the pond/water zone)
    --   LEFT to world x≤18 (tile x=11, aligns with tree column)
    fw.log("  Walking to CUT tree position (tile 11, 23)…")
    local function log_nav(label)
        local x, y = read_pos()
        fw.log(string.format("  [nav] %s: world(%d,%d)=tile(%d,%d)", label, x, y, x-7, y-7))
    end
    log_nav("Viridian entry")
    fw.hold_until("UP",   function() local _, y = read_pos(); return y <= 38 end, 300, 4)
    fw.wait_frames(16)
    log_nav("after UP-1")
    fw.hold_until("LEFT", function() local x, _ = read_pos(); return x <= 29 end, 300, 4)
    fw.wait_frames(16)
    log_nav("after LEFT-2")
    fw.hold_until("UP",   function() local _, y = read_pos(); return y <= 30 end, 300, 4)
    fw.wait_frames(16)
    log_nav("after UP-3")
    fw.hold_until("LEFT", function() local x, _ = read_pos(); return x <= 18 end, 400, 4)
    fw.wait_frames(60)
    log_nav("after LEFT-4 (final)")

    -- Explicitly sync SaveBlock1.location to the navigation end point.
    sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    emu:write16(sb1 + ADDR.SB1_LOC_X, 11)   -- tile x = 11
    emu:write16(sb1 + ADDR.SB1_LOC_Y, 23)   -- tile y = 23 (north of tree)

    local fx, fy = read_pos()
    fw.log(string.format("  Final position: world(%d,%d)=tile(%d,%d)",
        fx, fy, fx-7, fy-7))

    -- ── Teardown navigation state ──────────────────────────────────────────
    -- Clear party, repel, and FLAG_SYS_POKEMON_GET so fixture starts clean.
    sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(PARTY_BASE + i, 0) end
    emu:write8(ADDR.gPlayerPartyCount, 0)
    set_repel(sb1, 0)
    clear_flag(sb1, FLAG_SYS_POKEMON_GET)
    fw.wait_frames(10)

    fw.save_state(STATE_PATH)
    fw.log("  Fixture saved to " .. STATE_PATH)
    return true
end

-- ── Main test ──────────────────────────────────────────────────────────────

local results = {}
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.log((cond and "[PASS] " or "[FAIL] ") .. name)
end

fw.run(function()
    fw.log("=== Test: Slaveless HM Obstacle (issue #5) ===")

    -- Load or create fixture.
    if not fw.try_load_state(STATE_PATH) then
        local ok = build_fixture()
        if not ok then
            fw.assert_true(false, "fixture creation failed — see log for details")
            fw.finish()
            return
        end
        -- Reload to confirm fixture round-trips correctly.
        if not fw.try_load_state(STATE_PATH) then
            fw.assert_true(false, "fixture reload after creation failed")
            fw.finish()
            return
        end
    end
    fw.wait_frames(60)

    local px, py = read_pos()
    fw.log(string.format("Fixture loaded: player at (%d, %d), map packed=0x%04X",
        px, py, map_packed()))

    if map_packed() ~= MAP_VIRIDIAN then
        fw.assert_true(false,
            string.format("fixture map is 0x%04X, expected MAP_VIRIDIAN 0x%04X",
                map_packed(), MAP_VIRIDIAN))
        fw.log("  → Delete " .. STATE_PATH .. " and re-run to recreate the fixture.")
        fw.finish()
        return
    end

    -- ── Sub-test A: no badge, no HM → sign message, tree intact ───────────
    fw.log("--- Sub-test A: no badge, no HM01 ---")
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(60)

    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)

    -- Confirm initial state is clean (no badge, no HM).
    local badge_before = read_flag(sb1, FLAG_BADGE02_GET)
    local tree_before  = read_flag(sb1, TREE_FLAG)
    fw.log(string.format("  badge02_get=%s, FLAG_TEMP_11=%s",
        tostring(badge_before), tostring(tree_before)))

    -- Interact with the tree: press DOWN to face/bump it (player is NORTH of
    -- tree at tile 11,23; tree is at tile 11,24), then A to confirm.
    fw.hold("DOWN", 8)
    fw.wait_frames(60)
    fw.press("A")     -- dismiss the sign-style "can't cut" message
    fw.wait_frames(60)
    fw.press("A")     -- extra press in case dialog needs two A taps to clear
    fw.wait_frames(60)

    sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local tree_after_a = read_flag(sb1, TREE_FLAG)
    fw.log(string.format("  FLAG_TEMP_11 after A-test: %s", tostring(tree_after_a)))
    check("A: tree intact (no prompt without badge/HM)", not tree_after_a)

    -- ── Sub-test B: badge + HM01, empty party → slaveless → tree cut ──────
    fw.log("--- Sub-test B: badge set, HM01 in bag, empty party ---")
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(60)

    sb1 = fw.read32(ADDR.gSaveBlock1Ptr)

    -- Inject prerequisites for slaveless path.
    set_flag(sb1, FLAG_BADGE02_GET)
    inject_hm01(sb1)
    fw.wait_frames(5)

    -- Diagnostic: verify gBagPockets[3].itemSlots points to our injection site
    -- and that the written values read back correctly.
    do
        local bx, by = read_pos()
        local sb2       = fw.read32(ADDR.gSaveBlock2Ptr)
        local enc_key   = fw.read32(sb2 + SB2_ENCRYPTION_KEY)
        local tmhm_ptr  = fw.read32(GBAGPOCKETS_TMHM_SLOTS_ADDR)
        local expected  = sb1 + SB1_BAG_TMHM
        local stored_id = fw.read16(expected)
        local stored_q  = fw.read16(expected + 2)
        local decoded_q = (stored_q ~ enc_key) & 0xFFFF
        fw.log(string.format("  party count: %d, start pos: world(%d,%d)=tile(%d,%d)",
            fw.read8(ADDR.gPlayerPartyCount), bx, by, bx-7, by-7))
        fw.log(string.format("  encryptionKey=0x%08X", enc_key))
        fw.log(string.format("  gBagPockets[3].itemSlots=0x%08X expected=0x%08X match=%s",
            tmhm_ptr, expected, tostring(tmhm_ptr == expected)))
        fw.log(string.format("  slot0: itemId=%d stored_qty=%d decoded_qty=%d",
            stored_id, stored_q, decoded_q))
    end

    -- OBJ_EVENT sanity check: tree (lid=2, gfx=OBJ_EVENT_GFX_CUT_TREE≈43) must be active.
    for i = 0, 15 do
        local oa = ADDR.gObjectEvents + i * ADDR.OBJECT_EVENT_SIZE
        if (fw.read32(oa + ADDR.OE_FLAGS) & 1) ~= 0 then
            local lid, gfx = fw.read8(oa + ADDR.OE_LOCAL_ID), fw.read8(oa + ADDR.OE_GRAPHICS_ID)
            local ex, ey = fw.read16(oa + ADDR.OE_CURRENT_X), fw.read16(oa + ADDR.OE_CURRENT_Y)
            fw.log(string.format("  OE[%d] lid=%d gfx=%d tile(%d,%d)", i, lid, gfx, ex-7, ey-7))
        end
    end

    -- Interact with tree: press DOWN to face/bump it (player is NORTH of
    -- tree at tile 11,23; tree is at tile 11,24). Two A presses cover both
    -- trigger modes: bump-auto-triggers (A1=YES, A2=no-op) and
    -- A-press-triggers (A1=open YESNO, A2=YES).
    fw.hold("DOWN", 8)
    do
        local ax, ay = read_pos()
        fw.log(string.format("  after DOWN hold: world(%d,%d)=tile(%d,%d)", ax, ay, ax-7, ay-7))
    end
    fw.screenshot("/tmp/mgba-slaveless-b-after-down.png")
    fw.wait_frames(80)    -- wait for dialog box to render
    fw.screenshot("/tmp/mgba-slaveless-b-dialog.png")
    do  -- mid-test: verify sb1 hasn't moved and item is still present
        local cur_sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
        local sb2     = fw.read32(ADDR.gSaveBlock2Ptr)
        local enc_key = fw.read32(sb2 + SB2_ENCRYPTION_KEY)
        local stored_id = fw.read16(cur_sb1 + SB1_BAG_TMHM)
        local stored_q  = fw.read16(cur_sb1 + SB1_BAG_TMHM + 2)
        -- Check badge flag is still set
        local badge_byte_off = math.floor(FLAG_BADGE02_GET / 8)
        local badge_bit = 1 << (FLAG_BADGE02_GET % 8)
        local badge_byte = fw.read8(cur_sb1 + ADDR.SB1_FLAGS + badge_byte_off)
        -- Check gBagPockets[3] capacity
        local bp3_capacity = fw.read8(GBAGPOCKETS_TMHM_SLOTS_ADDR + 4)
        -- Check player facing direction (stored in the player's ObjectEvent)
        local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
        local oa_base = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
        local oe_flags = fw.read32(oa_base + ADDR.OE_FLAGS)
        fw.log(string.format("  PRE-A1: sb1=0x%08X (orig=0x%08X moved=%s) itemId=%d decoded_qty=%d",
            cur_sb1, sb1, tostring(cur_sb1 ~= sb1),
            stored_id, (stored_q ~ enc_key) & 0xFFFF))
        fw.log(string.format("  badge_byte=0x%02X badge_set=%s capacity=%d oe_flags=0x%08X",
            badge_byte, tostring((badge_byte & badge_bit) ~= 0), bp3_capacity, oe_flags))
    end
    fw.press("A")         -- A1: open YESNO dialog (tree interaction triggered by A-press)
    fw.wait_frames(60)    -- wait for dialog text to start rendering
    fw.screenshot("/tmp/mgba-slaveless-b-post-a1.png")
    -- FireRed ignores A while text is scrolling (~4 frames/char × 57 chars ≈ 260 frames).
    -- Wait generously past that, then press A once the YES/NO cursor is live.
    fw.wait_frames(300)   -- wait for text scroll to complete + YES/NO cursor to appear
    fw.press("A")         -- A2: confirm YES
    fw.wait_frames(800)   -- wait for silhouette animation + cut animation + tree removal
    fw.screenshot("/tmp/mgba-slaveless-b-after-anim.png")

    sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local tree_after_b = read_flag(sb1, TREE_FLAG)
    fw.log(string.format("  FLAG_TEMP_11 after B-test: %s", tostring(tree_after_b)))
    check("B: tree cut (FLAG_TEMP_11 set after slaveless YES)", tree_after_b)

    -- Summary
    local passed = 0
    for _, r in ipairs(results) do if r.pass then passed = passed + 1 end end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
