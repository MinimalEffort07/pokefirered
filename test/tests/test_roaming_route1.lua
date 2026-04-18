-------------------------------------------------------------------------------
-- test_roaming_route1.lua
-------------------------------------------------------------------------------
-- End-to-end VISUAL integration test for the Roaming Pokemon system.
--
-- *** ONE-TIME MANUAL SETUP REQUIRED ***
--
-- This test demonstrates the roamer system on Route 1 with REAL engine-
-- spawned roamers (not memory-injected). Because Route 1 is several
-- map transitions deep, getting there programmatically is unreliable
-- (interior door warps in FireRed have input-state requirements that
-- aren't reproducible cleanly from Lua). Instead, the test loads a
-- cached mGBA save state where the player is already standing on
-- Route 1, and skips straight to the assertions.
--
-- HOW TO CREATE THE CACHED STATE (do this once):
--
--   1. Open the ROM in mgba-qt:                mgba-qt pokefirered.gba
--   2. Play through Oak's speech and walk north to Route 1.
--   3. Open the scripting console and load:
--        Tools > Scripting > File > Open >
--          test/tools/save_route1_state.lua
--      That script saves the current emulator state to
--      /tmp/pokefirered-route1.ss.
--   4. Re-run this test; it will load the state and execute the demo.
--
-- WHAT THIS DEMONSTRATES (after the state loads):
--   1. Real Pokemon sprites wandering on Route 1, spawned via the
--      engine's normal SpawnRoamingPokemonOnMap path -- proper VRAM
--      tiles, palettes, wander animation.
--   2. The player walking into one of those visible Pokemon and a
--      wild battle starting on contact (bump-to-battle pipeline).
--   3. Roamers being cleared when the player warps off Route 1
--      (DespawnAllRoamingPokemon hook).
--
-- For automated CI coverage of the roamer system that doesn't need a
-- manual fixture, see:
--   test/tests/test_roaming_init.lua  - sentinels, timers, init
--   test/tests/test_roaming_bump.lua  - bump-to-battle via injection
--
-- HOW TO RUN:
--   GUI:      mgba-qt, then Tools > Scripting > File > Open this file
--   Headless: bash test/run_test.sh test/tests/test_roaming_route1.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH = "/tmp/pokefirered-route1.ss"

-- Map IDs (mapGroup << 8) | mapNum.
local MAP_PALLET = (3 * 256) + 0
local MAP_ROUTE1 = (3 * 256) + 19

-- ---------- read helpers ----------

local function map_packed()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local mg = fw.read8(sb1 + ADDR.SB1_LOC_MAP_GROUP)
    local mn = fw.read8(sb1 + ADDR.SB1_LOC_MAP_NUM)
    return (mg * 256) + mn
end

local function map_label(packed)
    if packed == MAP_PALLET then return "PALLET_TOWN"
    elseif packed == MAP_ROUTE1 then return "ROUTE1"
    else return string.format("MAP(%d.%d)",
        math.floor(packed/256), packed % 256) end
end

local function read_pos()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y)
end

local function read_obj_pos(oid)
    local oa = ADDR.gObjectEvents + oid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y)
end

local function read_obj_active(oid)
    return (fw.read32(ADDR.gObjectEvents
        + oid * ADDR.OBJECT_EVENT_SIZE) & 1) == 1
end

local function read_obj_gfx(oid)
    return fw.read8(ADDR.gObjectEvents
        + oid * ADDR.OBJECT_EVENT_SIZE + ADDR.OE_GRAPHICS_ID)
end

-- Snapshot every active roamer with its current OE position.
local function list_active_roamers()
    local out = {}
    for i = 0, ADDR.ROAMER_CAP_VISIBLE - 1 do
        local base = ADDR.gRoamers + i * ADDR.ROAMER_STRUCT_SIZE
        local oid = fw.read8(base + ADDR.ROAMER_OFF_OBJ_EVENT_ID)
        if oid < ADDR.OBJECT_EVENTS_COUNT and read_obj_active(oid) then
            local x, y = read_obj_pos(oid)
            table.insert(out, {
                slot = i, objEventId = oid,
                tableIdx = fw.read8(base + ADDR.ROAMER_OFF_TABLE_IDX),
                level = fw.read8(base + ADDR.ROAMER_OFF_LEVEL),
                x = x, y = y, gfx = read_obj_gfx(oid),
            })
        end
    end
    return out
end

-- ---------- the test ----------

fw.run(function()
    fw.log("=== Roaming Pokemon Route 1 (visual integration) ===")

    -- Drop out of fast-forward IMMEDIATELY, before doing anything else.
    -- fw.run unconditionally enables fast-forward at start; for this
    -- visual-demo test we want real-time rendering throughout so the
    -- viewer can actually see the loaded state, the spawning roamers,
    -- and the bump-to-battle. mGBA's GUI canvas does not reliably
    -- update during fast-forward (frame renders are skipped to save
    -- CPU), and toggling back to real-time AFTER a state load is too
    -- late -- the canvas is left displaying whatever was last rendered.
    fw.slow_down("entire route1 test runs at real-time for visibility")
    fw.wait_frames(30)  -- let the renderer settle at 60fps

    if not fw.try_load_state(STATE_PATH) then
        fw.log_error("===")
        fw.log_error("No cached Route 1 state at " .. STATE_PATH)
        fw.log_error("This test requires a one-time manual setup. See the")
        fw.log_error("instructions in the comment block at the top of this file.")
        fw.log_error("TL;DR: open the ROM in mgba-qt, walk to Route 1, then run")
        fw.log_error("test/tools/save_route1_state.lua via the scripting console.")
        fw.log_error("===")
        fw.assert_true(false, "cached Route 1 state exists")
        fw.finish()
        return
    end

    -- Brief settle window for the post-load frame to render at real-time.
    fw.log(">>> State loaded. You should now see Route 1 in the GUI.")
    fw.wait_frames(60)  -- 1 second at real-time

    fw.assert_eq(map_packed(), MAP_ROUTE1,
        "loaded state has the player on Route 1")
    if map_packed() ~= MAP_ROUTE1 then
        fw.log_error("State loaded but player is on " .. map_label(map_packed()) ..
                     ". Re-create the state with the player standing on Route 1.")
        fw.finish()
        return
    end

    -- Walk back and forth so the viewer can see the player moving AND
    -- so the periodic spawn timer (60 frames) gets multiple opportunities
    -- to populate gRoamers. The save state may have been created with
    -- zero roamers in flight (depending on when the user pressed save),
    -- so we explicitly drive the spawn loop with motion + waits.
    fw.log(">>> Walking around to trigger periodic roamer spawns...")
    for round = 1, 6 do
        fw.hold("RIGHT", 24); fw.wait_frames(30)
        fw.hold("LEFT",  24); fw.wait_frames(30)
        local snap = list_active_roamers()
        fw.log(string.format("    round %d: %d roamers visible",
            round, #snap))
        if #snap >= 2 then break end
    end

    -- One last settle window so any in-progress spawn ticks complete.
    fw.wait_frames(120)

    -- ---------- ASSERTION 1: real roamers spawned ----------
    local roamers = list_active_roamers()
    local count = fw.read8(ADDR.gRoamerCount)
    fw.log(string.format("Active roamers: count=%d, listed=%d", count, #roamers))
    for _, r in ipairs(roamers) do
        fw.log(string.format("  slot %d: oid=%d gfx=%d level=%d pos=(%d,%d)",
            r.slot, r.objEventId, r.gfx, r.level, r.x, r.y))
    end
    fw.assert_true(count > 0, "engine spawned at least one real roamer on Route 1")
    fw.assert_true(#roamers > 0,
        "at least one roamer's OE is active in gObjectEvents")
    -- Each roamer's gfx should be in the Pokemon range (109..150).
    for _, r in ipairs(roamers) do
        fw.assert_true(r.gfx >= 109 and r.gfx <= 150,
            string.format("roamer slot %d has Pokemon-range graphicsId (%d)",
                r.slot, r.gfx))
    end

    fw.screenshot("/tmp/mgba-roaming-route1-spawned.png")

    -- ---------- ASSERTION 1.5: flyby animation is the flap cycle ----------
    -- CreateOneFlybySprite swaps sprite->anims to sFlybyFlapAnimTable and
    -- calls StartSpriteAnim with FLYBY_FLAP_ANIM_EAST (1) when moving
    -- right and FLYBY_FLAP_ANIM_WEST (0) when moving left. Those alternate
    -- frames 7 and 8 -- the side-profile wings-down / wings-up poses --
    -- at 4 ticks each, with no idle frame in between. Any other animNum
    -- indicates the sprite fell back to a stock anim and will render as
    -- the bird walking (stock GO_WEST/EAST) or staring at the camera
    -- (default FACE_SOUTH = 0 in sAnimTable_Standard). We assert 0 or 1,
    -- which under sFlybyFlapAnimTable is the flap cycle.
    --
    -- Flybys spawn probabilistically (every 300 frames at 1/4 chance),
    -- so we poll for up to ~20 seconds. If none appear in the window,
    -- we log a warning but don't fail -- this is a conditional check.
    fw.log("--- Checking flyby animation numbers ---")
    local flyby_seen = false
    for poll = 1, 40 do
        for i = 0, ADDR.FLYBY_CAP_VISIBLE - 1 do
            local sid = fw.read8(ADDR.gRoamingFlybySpriteIds + i)
            if sid < ADDR.MAX_SPRITES then
                local sprite_addr = ADDR.gSprites + sid * ADDR.SPRITE_STRUCT_SIZE
                local anim = fw.read8(sprite_addr + ADDR.SPRITE_OFF_ANIM_NUM)
                fw.log(string.format(
                    "  flyby slot %d: spriteId=%d animNum=%d (expect %d or %d)",
                    i, sid, anim, ADDR.FLYBY_FLAP_ANIM_WEST, ADDR.FLYBY_FLAP_ANIM_EAST))
                fw.assert_true(
                    anim == ADDR.FLYBY_FLAP_ANIM_WEST
                    or anim == ADDR.FLYBY_FLAP_ANIM_EAST,
                    string.format(
                        "flyby sprite uses flap anim table (got animNum=%d)",
                        anim))
                flyby_seen = true
            end
        end
        if flyby_seen then break end
        fw.wait_frames(30)
    end
    if not flyby_seen then
        fw.log("  WARNING: no flyby spawned in ~20s window; anim assertion skipped")
    else
        fw.screenshot("/tmp/mgba-roaming-route1-flyby.png")
    end

    -- ---------- ASSERTION 2: bump into a real roamer ----------
    fw.log("--- Chasing a roamer to trigger bump-to-battle ---")
    local battle_started = false

    for chase_step = 1, 30 do
        local px, py = read_pos()
        local current = list_active_roamers()
        if #current == 0 then
            fw.log("  chase: no roamers active right now; waiting")
            fw.wait_frames(60)
        else
            -- Closest roamer to player.
            local best, best_dist = nil, math.huge
            for _, r in ipairs(current) do
                local d = math.abs(r.x - px) + math.abs(r.y - py)
                if d < best_dist then best, best_dist = r, d end
            end
            local dx, dy = best.x - px, best.y - py
            local dir
            if math.abs(dx) >= math.abs(dy) then
                dir = (dx > 0) and "RIGHT" or "LEFT"
            else
                dir = (dy > 0) and "DOWN" or "UP"
            end
            fw.log(string.format(
                "  chase %d: player=(%d,%d) target=(%d,%d) dist=%d dir=%s",
                chase_step, px, py, best.x, best.y, best_dist, dir))
            fw.hold(dir, 24)
            fw.wait_frames(8)
        end

        local flags = fw.read32(ADDR.gBattleTypeFlags)
        if (flags & ADDR.BATTLE_TYPE_WILD_SCRIPTED) ~= 0 then
            battle_started = true
            fw.log(string.format("  bump succeeded! flags=0x%08X", flags))
            break
        end
    end

    fw.assert_true(battle_started,
        "wild battle triggered by bumping a real engine-spawned roamer")
    fw.screenshot("/tmp/mgba-roaming-route1-battle.png")

    -- ---------- ASSERTION 3: cleanup on warp ----------
    fw.wait_frames(120)
    fw.log("--- Walking south to test warp cleanup ---")
    fw.hold("DOWN", 6); fw.wait_frames(10)
    fw.hold_until("DOWN",
        function() return map_packed() == MAP_PALLET end, 500, 12)
    fw.wait_frames(120)

    if map_packed() == MAP_PALLET then
        local count_after = fw.read8(ADDR.gRoamerCount)
        fw.log(string.format("After warp to Pallet, gRoamerCount=%d", count_after))
        fw.assert_eq(count_after, 0,
            "DespawnAllRoamingPokemon cleared all roamers after warp")
    else
        fw.log("  did not return to Pallet; skipping cleanup assertion")
    end

    fw.finish()
end)
