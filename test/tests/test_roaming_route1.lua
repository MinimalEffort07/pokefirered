-------------------------------------------------------------------------------
-- test_roaming_route1.lua
-------------------------------------------------------------------------------
-- End-to-end VISUAL integration test for the Roaming Pokemon system.
--
-- *** NEEDS A ROUTE 1 SAVESTATE FIXTURE ***
--
-- This test demonstrates the roamer system on Route 1 with REAL engine-
-- spawned roamers (not memory-injected). It starts from a cached mGBA
-- save state, /tmp/pokefirered-route1.ss, with the player already on
-- Route 1. The test suite leaves it out (test/suite_exclude.txt) because
-- nothing in the suite makes that state.
--
-- HOW TO MAKE THE FIXTURE (once per ROM build):
--
--   bash test/run_test.sh test/tools/warp_to_route1.lua
--
--   That boots a new game, warps the player onto Route 1 and saves the
--   state. The older way, by hand: open the ROM in mgba-qt, play through
--   Oak's speech, walk north to Route 1, then run
--   test/tools/save_route1_state.lua from Tools > Scripting.
--
-- WHAT THIS DEMONSTRATES (after the state loads):
--   1. Real Pokemon sprites wandering on Route 1, spawned via the
--      engine's normal SpawnRoamingPokemonOnMap path -- proper VRAM
--      tiles, palettes, wander animation.
--   1.5 A flyby (a bird crossing the screen) using the flap animation, if
--      one appears.
--   2. The player walking into one of those visible Pokemon and a
--      wild battle starting on contact (bump-to-battle pipeline).
--   3. Roamers being cleared when the player warps off Route 1
--      (DespawnAllRoamingPokemon hook).
--
-- SETUP AFTER THE STATE LOADS:
--   * The player gets a level-100 PIKACHU and a long Repel
--     (test/lib/poke_radar.lua). The Repel keeps random wild battles in
--     Route 1's tall grass from interrupting the chase (a roamer's battle
--     isn't random, so it still starts), and the PIKACHU is fast enough
--     that RUN always gets away from that battle.
--   * The player moves to open ground in the middle of the route
--     (warp.warp_to_pos, which loads Route 1 again, so its roamers spawn
--     through the engine's on-load path). warp_to_route1.lua leaves the
--     player on the tile they had in the bedroom, in a corner of Route 1
--     closed off by trees and a ledge, where no roamer can be reached.
--   * The chase plans each step with a breadth-first search over the
--     tiles (test/lib/mt_moon.lua), so trees and ledges don't stop it.
--   * After the bump the player RUNs from the battle, then warps to Pallet
--     Town (warp.warp_to), a real map load, as a door or Fly would do.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua): Route 1 loaded; the roamers wandering; a flyby
-- flapping, if one appears; the bump that starts a wild battle (marked as
-- the player comes within two steps of the roamer, so it plays the last
-- step, the bump and the start of the battle transition); and Pallet Town
-- after the warp, with the roamers cleared. Everything else fast-forwards.
--
-- For automated CI coverage of the roamer system that doesn't need a
-- fixture, see:
--   test/tests/test_roaming_init.lua  - sentinels, timers, init
--   test/tests/test_roaming_bump.lua  - bump-to-battle via injection
--
-- HOW TO RUN:
--   Headless: bash test/run_test.sh test/tests/test_roaming_route1.lua
--   Record:   bash test/record_test.sh test/tests/test_roaming_route1.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local mm   = dofile(project_dir .. "test/lib/mt_moon.lua")
local walk = dofile(project_dir .. "test/lib/walk.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local pr   = dofile(project_dir .. "test/lib/poke_radar.lua")

local STATE_PATH = "/tmp/pokefirered-route1.ss"

-- Map IDs (mapGroup << 8) | mapNum.
local MAP_PALLET = (ADDR.MAP_GROUP_KANTO * 256) + ADDR.MAP_NUM_PALLET_TOWN
local MAP_ROUTE1 = (ADDR.MAP_GROUP_KANTO * 256) + ADDR.MAP_NUM_ROUTE1

-- Open ground in the middle of Route 1, in map coordinates (without the
-- 7-tile border offset the object coordinates carry): its row runs from
-- wall to wall and the tall grass the roamers spawn in is around it.
local START_X, START_Y = 14, 12

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

local function wild_battle_started()
    return (fw.read32(ADDR.gBattleTypeFlags) & ADDR.BATTLE_TYPE_WILD_SCRIPTED) ~= 0
end

-- ---------- the chase ----------

-- The four steps a player can take, as tile offsets.
local STEPS = {
    { dx = 0,  dy = -1 }, { dx = 0, dy = 1 },
    { dx = -1, dy = 0 },  { dx = 1, dy = 0 },
}

-- The next step of a shortest walk from the player into roamer `r` (an
-- entry of mm.live_objects, in map coordinates). A breadth-first search
-- (mm.bfs) spreads out from r's tile over the walkable tiles, with every
-- other object (the follower too) solid; the player's neighbour with the
-- lowest distance is the step. A tree or a ledge is solid to the search
-- (a ledge can only be jumped one way), so the walk goes round them.
-- Returns that tile and the walk's length in steps (1: the step into r
-- itself), or nil if nothing reaches r right now. Like walk.plan, but
-- without the gap walk.lua keeps round roaming Pokemon: here walking
-- into one is the point.
local function next_step_toward(r)
    local px, py = mm.player_tile()
    if px == nil then return nil end
    local solid = {}
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player and o.index ~= r.index then
            solid[mm.key(o.x, o.y)] = true
            solid[mm.key(o.prev_x, o.prev_y)] = true
        end
    end
    local dist = mm.bfs(mm.read_grid(), r.x, r.y, function(x, y)
        return solid[mm.key(x, y)] and not (x == px and y == py)
    end)
    local best, best_d = nil, nil
    for _, s in ipairs(STEPS) do
        local nx, ny = px + s.dx, py + s.dy
        local nd = dist[mm.key(nx, ny)]
        if nd and (best_d == nil or nd < best_d) then
            best, best_d = { x = nx, y = ny }, nd
        end
    end
    if best == nil then return nil end
    return best, best_d + 1
end

-- Wait up to `frames` frames until the player can take a step, or a
-- battle has started. Returns true if either happened.
local function wait_ready_or_battle(frames)
    for _ = 1, frames do
        if walk.ready() or wild_battle_started() then return true end
        fw.yield()
    end
    return false
end

-- ---------- the test ----------

fw.run(function()
    fw.log("=== Roaming Pokemon Route 1 (visual integration) ===")

    if not fw.try_load_state(STATE_PATH) then
        fw.log_error("===")
        fw.log_error("No cached Route 1 state at " .. STATE_PATH)
        fw.log_error("This test needs a fixture. See the instructions in the")
        fw.log_error("comment block at the top of this file. TL;DR:")
        fw.log_error("  bash test/run_test.sh test/tools/warp_to_route1.lua")
        fw.log_error("===")
        fw.assert_true(false, "cached Route 1 state exists")
        fw.finish()
        return
    end
    fw.wait_frames(60)  -- let the loaded state settle before reading it

    fw.assert_eq(map_packed(), MAP_ROUTE1,
        "loaded state has the player on Route 1")
    if map_packed() ~= MAP_ROUTE1 then
        fw.log_error("State loaded but player is on " .. map_label(map_packed()) ..
                     ". Re-create the state with the player standing on Route 1.")
        fw.finish()
        return
    end

    -- See SETUP AFTER THE STATE LOADS in the header.
    pr.give_party()
    pr.set_repel(5000)
    local px0, py0 = mm.player_tile()
    fw.log(string.format("Fixture has the player at (%d,%d); moving them to (%d,%d)",
        px0 or -1, py0 or -1, START_X, START_Y))
    fw.assert_true(warp.warp_to_pos(ADDR.MAP_GROUP_KANTO, ADDR.MAP_NUM_ROUTE1, START_X, START_Y),
        "moved the player to open ground on Route 1")
    fw.assert_true(wait_ready_or_battle(300) and walk.ready(),
        "the player has control on Route 1")
    fw.show("Route 1 loaded: the player in the middle of the route")

    -- Walk back and forth so the periodic spawn timer (60 frames) gets
    -- multiple opportunities to populate gRoamers (loading the map spawns some at once, but a
    -- spawn can miss when the tile it picks is taken).
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

    fw.screenshot(fw.out_path("mgba-roaming-route1-spawned.png"))
    -- Two seconds: a roamer takes a step only every second or two.
    fw.show("the roamers wandering on Route 1", 120)

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
    -- so we poll for up to a minute of game time (about one chance in 30
    -- of seeing none; it fast-forwards, so it costs a few seconds). If
    -- none appear in the window, we log a warning but don't fail -- this
    -- is a conditional check.
    fw.log("--- Checking flyby animation numbers ---")
    local flyby_seen = false
    for poll = 1, 120 do
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
        fw.log("  WARNING: no flyby spawned in a minute; anim assertion skipped")
    else
        fw.screenshot(fw.out_path("mgba-roaming-route1-flyby.png"))
        fw.show("a flyby flapping its wings as it crosses the screen")
    end

    -- ---------- ASSERTION 2: bump into a real roamer ----------
    -- Each round: wait until the player can step, plan a walk into the
    -- nearest roamer (by route length; roamers wander, so the route is
    -- planned again before every step), and take its first step. The
    -- step into the roamer itself is the bump.
    fw.log("--- Chasing a roamer to trigger bump-to-battle ---")
    local battle_started = false

    for chase_step = 1, 80 do
        wait_ready_or_battle(120)
        if wild_battle_started() then battle_started = true break end

        local target, step, length = nil, nil, nil
        for _, o in ipairs(mm.live_objects()) do
            if mm.is_roamer(o) then
                local s, len = next_step_toward(o)
                if s and (length == nil or len < length) then
                    target, step, length = o, s, len
                end
            end
        end

        if target == nil then
            fw.log("  chase: no roamer reachable right now; waiting")
            fw.wait_frames(30)
        else
            local px, py = mm.player_tile()
            fw.log(string.format(
                "  chase %d: player=(%d,%d) roamer=(%d,%d) steps=%d next=(%d,%d)",
                chase_step, px, py, target.x, target.y, length, step.x, step.y))
            -- Two steps out: the moment plays the last step, the bump and
            -- the start of the battle transition. If the roamer wanders
            -- off, the next approach is marked again once this one ends.
            if length <= 2 and fw.speed() == "fast" then
                fw.show_next("the player walks into a roamer, and a wild battle starts", 120)
            end
            walk.step_onto(step.x, step.y)
        end

        if wild_battle_started() then battle_started = true break end
    end

    if battle_started then
        fw.log(string.format("  bump succeeded! flags=0x%08X",
            fw.read32(ADDR.gBattleTypeFlags)))
    end
    fw.assert_true(battle_started,
        "wild battle triggered by bumping a real engine-spawned roamer")
    fw.screenshot(fw.out_path("mgba-roaming-route1-battle.png"))

    -- ---------- ASSERTION 3: cleanup on warp ----------
    -- RUN from the battle, then warp off Route 1 to Pallet Town. Loading
    -- the new map calls DespawnAllRoamingPokemon (LoadMapFromWarp in
    -- src/overworld.c) before Route 1's objects go, and Pallet Town has no
    -- wild Pokemon, so no roamer spawns there.
    if battle_started then
        fw.assert_true(pr.wait_for_battle(600), "the roamer's battle began")
        local outcome = pr.play_battle("RUN")
        fw.assert_eq(outcome, pr.B_OUTCOME_RAN, "the player got away from the roamer's battle")
        -- (gBattleTypeFlags keeps the battle's bits after it ends, so
        -- this waits for control alone.)
        fw.assert_true(fw.wait_until(walk.ready, 600, "control back after running"),
            "back on Route 1 with control after running")

        fw.log("--- Warping to Pallet Town to test warp cleanup ---")
        fw.log(string.format("Before the warp, gRoamerCount=%d", fw.read8(ADDR.gRoamerCount)))
        warp.warp_to(ADDR.MAP_GROUP_KANTO, ADDR.MAP_NUM_PALLET_TOWN, 0)
        fw.assert_eq(map_packed(), MAP_PALLET, "warped to Pallet Town")

        local count_after = fw.read8(ADDR.gRoamerCount)
        fw.log(string.format("After warp to Pallet, gRoamerCount=%d", count_after))
        fw.assert_eq(count_after, 0,
            "DespawnAllRoamingPokemon cleared all roamers after warp")
        fw.screenshot(fw.out_path("mgba-roaming-route1-pallet.png"))
        fw.show("Pallet Town after the warp, with the roamers cleared")
    else
        fw.log("  no battle to leave; skipping cleanup assertion")
    end

    fw.finish()
end)
