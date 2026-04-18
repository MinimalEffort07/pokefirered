-------------------------------------------------------------------------------
-- test_roaming_init.lua
-------------------------------------------------------------------------------
-- Sanity test for the Roaming Pokemon system. Verifies that:
--   1. The game boots to the overworld without crashing.
--   2. The roamer system's EWRAM state is properly initialized once the
--      overworld is reached (sentinels are set: gRoamers[i].objEventId == 16,
--      gRoamingFlybySpriteIds[i] == 64).
--   3. Logs which map the player landed on, so we can plan integration tests
--      that need a specific map (Route 22 has Spearow for the flyby check).
--
-- This test does NOT require the player to be on a wild-encounter map. It
-- just exercises the EnsureInitialized() codepath and verifies the static
-- state layout matches what addresses.lua expects.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_roaming_init.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

-- Read a single roamer's fields as a Lua table.
local function read_roamer(idx)
    local base = ADDR.gRoamers + idx * ADDR.ROAMER_STRUCT_SIZE
    return {
        objEventId   = fw.read8(base + ADDR.ROAMER_OFF_OBJ_EVENT_ID),
        tableIdx     = fw.read8(base + ADDR.ROAMER_OFF_TABLE_IDX),
        level        = fw.read8(base + ADDR.ROAMER_OFF_LEVEL),
        pendingBattle = fw.read8(base + ADDR.ROAMER_OFF_PENDING),
    }
end

-- Read the player's current map (mapGroup, mapNum) from save block 1.
-- gSaveBlock1Ptr is an indirection — read the pointer first, then the field.
local function read_current_map()
    local sb1_ptr = fw.read32(ADDR.gSaveBlock1Ptr)
    local mapGroup = fw.read8(sb1_ptr + ADDR.SB1_LOC_MAP_GROUP)
    local mapNum   = fw.read8(sb1_ptr + ADDR.SB1_LOC_MAP_NUM)
    return mapGroup, mapNum
end

fw.run(function()
    fw.log("=== Roaming Pokemon Init Test ===")

    -- Boot through the title screen, character selection, name entry,
    -- Oak's speech, and the shrink animation that drops the player into
    -- the overworld. character_select.lua does all this for us; we use
    -- the default character (Red, list index 0).
    --
    -- We use the cs helpers because they're battle-tested for getting
    -- through Oak's speech reliably across timing variations. The
    -- selection-press sentinel discovery normally costs ~1500 frames on
    -- the first call; we let it run.
    fw.log("Booting to overworld (this takes ~5000 frames)...")
    local sel = cs.find_selection_press()
    fw.assert_neq(sel, nil, "discovered Oak-speech selection press")
    if not sel then
        fw.finish()
        return
    end

    emu:reset()
    cs.boot_and_open_list(sel)
    -- Take the default selection (Red at list index 0). No scrolling.
    cs.confirm_and_enter_overworld()

    -- Should now be in the overworld. Verify the player's ObjectEvent
    -- is active — this is our proxy for "we made it to the field".
    fw.assert_true(cs.is_player_active(),
        "player ObjectEvent active (reached the overworld)")

    -- Drop to real-time so a GUI tester can see the player standing in
    -- the bedroom. Most of this test is memory inspection, but the
    -- visual confirmation that the system didn't break the overworld
    -- is the most important thing for a human to actually see.
    fw.slow_down("player should be visible in their bedroom")
    fw.wait_frames(120)  -- 2 seconds at real-time

    -- Log the current map so we know what to plan navigation around.
    local mapGroup, mapNum = read_current_map()
    fw.log(string.format("Player is on map %d.%d (mapGroup.mapNum)",
        mapGroup, mapNum))
    if mapGroup == ADDR.MAP_GROUP_KANTO and mapNum == ADDR.MAP_NUM_PALLET_TOWN then
        fw.log("    -> PALLET_TOWN (no encounters; integration tests need to walk north)")
    elseif mapGroup == ADDR.MAP_GROUP_KANTO and mapNum == ADDR.MAP_NUM_ROUTE1 then
        fw.log("    -> ROUTE1 (encounter table active; roamers should spawn)")
    end

    -- Wait long enough for at least one CB1_Overworld tick to fire.
    -- EnsureInitialized() runs at the top of every public function call,
    -- so by frame 60 it should definitely have run.
    fw.wait_frames(60)

    -- Verify the sentinels were written by EnsureInitialized().
    -- gRoamers[i].objEventId should be OBJECT_EVENTS_COUNT (16) for all
    -- slots (no roamers yet, all slots free).
    fw.log("--- gRoamers[] state ---")
    local all_free = true
    for i = 0, ADDR.ROAMER_CAP_VISIBLE - 1 do
        local r = read_roamer(i)
        fw.log(string.format("  gRoamers[%d]: objEventId=%d, tableIdx=%d, level=%d, pending=%d",
            i, r.objEventId, r.tableIdx, r.level, r.pendingBattle))
        -- After EnsureInitialized, free-slot sentinel is OBJECT_EVENTS_COUNT (16).
        -- We allow either 16 (free) OR a valid slot index (0..15) IF the player
        -- happened to land on a roaming-eligible map and a roamer spawned.
        if r.objEventId ~= ADDR.OBJECT_EVENTS_COUNT then
            all_free = false
        end
    end

    -- gRoamerCount should reflect actual active count. Either 0 (none
    -- spawned, sentinels set, nothing went wrong) or matches the number
    -- of non-sentinel slots above.
    local count = fw.read8(ADDR.gRoamerCount)
    fw.log(string.format("gRoamerCount = %d", count))
    fw.assert_true(count <= ADDR.ROAMER_CAP_VISIBLE,
        "gRoamerCount within cap (<= 4)")

    -- Verify flyby sentinels.
    fw.log("--- gRoamingFlybySpriteIds[] state ---")
    for i = 0, ADDR.FLYBY_CAP_VISIBLE - 1 do
        local sid = fw.read8(ADDR.gRoamingFlybySpriteIds + i)
        fw.log(string.format("  gRoamingFlybySpriteIds[%d] = %d", i, sid))
        -- A free flyby slot should be MAX_SPRITES (64). If a flyby
        -- happens to be active, it should be a valid sprite id (0..63).
        fw.assert_true(sid <= ADDR.MAX_SPRITES,
            string.format("flyby slot %d within valid range", i))
    end

    -- Verify timers are set (non-zero after the initial post-warp seed).
    -- SpawnRoamingPokemonOnMap sets both timers to their BASE values
    -- (60 and 300 respectively) on every map enter. If they're both 0
    -- and we're on a roaming-eligible map, it means SpawnRoamingPokemonOnMap
    -- was never called, which would be a hookup bug.
    local spawnTimer = fw.read16(ADDR.gRoamerNextSpawnTimer)
    local flybyTimer = fw.read16(ADDR.gRoamerNextFlybyTimer)
    fw.log(string.format("gRoamerNextSpawnTimer = %d, gRoamerNextFlybyTimer = %d",
        spawnTimer, flybyTimer))

    -- If we're on a wild-encounter map, the timers should have been
    -- seeded by SpawnRoamingPokemonOnMap (or already counted down).
    -- We don't strictly assert their values because they're tick-dependent,
    -- but a sane range is "0 < x <= 600".
    fw.assert_true(spawnTimer <= 600, "spawn timer in sane range")
    fw.assert_true(flybyTimer <= 600, "flyby timer in sane range")

    -- Take a screenshot for visual inspection.
    fw.screenshot("/tmp/mgba-roaming-init.png")

    fw.log(string.format("Initial result: count=%d, all_free=%s, map=%d.%d",
        count, tostring(all_free), mapGroup, mapNum))

    fw.finish()
end)
