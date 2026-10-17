-------------------------------------------------------------------------------
-- test_mt_moon_save_continue.lua: S6, a generated cave survives save/Continue
-------------------------------------------------------------------------------
--
-- Mt. Moon is generated from a per-visit seed stored in the save
-- (VAR_MT_MOON_MAP_SEED). Walking in rolls a new seed; Continue must rebuild
-- the cave from the stored one, so the player loads into the same cave they
-- saved in.
--
-- FOR EACH GENERATED FLOOR (1F, then B1F):
--   1. Fresh entry through the floor's arrival warp, with a fixed gRngValue.
--      PASS: VAR_MT_MOON_MAP_SEED changed (a fresh entry rolls a new seed),
--      and the player stands on the arrival warp's generated tile.
--      On floors with NPCs, re-enter (fresh entries with the next fixed
--      gRngValues) until at least one NPC is on screen at the arrival, so the
--      save in step 2 contains live NPCs to compare.
--   2. Save at the arrival tile through START > SAVE, soft reset, CONTINUE.
--      PASS when, compared with what was saved:
--        - the map is the same floor,
--        - VAR_MT_MOON_MAP_SEED is unchanged (Continue doesn't roll),
--        - the map grid (every metatile, collision and elevation) is identical,
--        - every object template position is identical,
--        - every floor object that was on screen when saving is back on the
--          same tile (and on floors with NPCs, at least one was compared),
--        - the player is on the same tile, SaveBlock1.pos agrees, and that
--          tile is walkable floor reachable from the arrival warp.
--   3. Move the player to the reachable tile farthest from the arrival warp,
--      then save and Continue again with the same checks. On B1F this proves
--      the old "teleport to the entrance on every load" workaround is gone.
--
-- Why the player is moved by poking RAM rather than by walking: the test
-- game has no Pokemon, and a wild encounter on the way would start a battle
-- with an empty party. A poke moves the player but not the camera, so after
-- step 3 the NPCs near the new tile only appear once the camera moves; step 2
-- is the one that compares live NPCs.
--
-- Objects that aren't Mt. Moon content (the roaming-Pokemon sprites, local
-- ids 200+) are ignored: only objects whose local id matches one of the
-- floor's templates count.
--
-- Pass/fail values are read from RAM: the seed var from SaveBlock1.vars, the
-- map from the VMap buffer, templates from SaveBlock1.objectEventTemplates,
-- live objects from gObjectEvents (and their saved copy in SaveBlock1).
--
-- Run:
--   bash test/run_test.sh test/tests/test_mt_moon_save_continue.lua
-- Record frames for a video (see test/lib/frame_capture.lua):
--   MGBA_FRAME_DIR=/tmp/s6_frames bash test/run_test.sh \
--       test/tests/test_mt_moon_save_continue.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

-- Fixed random states, so every run builds the same caves.
local RNG = { ["1F"] = 0x00C0FFEE, ["B1F"] = 0x0B1F0B1F }

-- Tiles the player shouldn't be moved onto: warp tiles and object tiles.
local function occupied_set(snap)
    local set = {}
    for _, w in ipairs(snap.warps) do set[mm.key(w.x, w.y)] = true end
    for _, t in ipairs(snap.templates) do set[mm.key(t.x, t.y)] = true end
    for _, o in ipairs(snap.live) do
        if not o.is_player then set[mm.key(o.x, o.y)] = true end
    end
    return set
end

-- The reachable, free tile farthest (in steps) from the arrival warp.
-- Ties go to the tile scanned first (top to bottom, left to right).
local function farthest_reachable(floor, snap)
    local dist = mm.reachable_from_arrival(floor, snap)
    local occupied = occupied_set(snap)
    local best_x, best_y, best_d = nil, nil, -1
    for y = 0, snap.grid.h - 1 do
        for x = 0, snap.grid.w - 1 do
            local d = dist[mm.key(x, y)]
            if d and d > best_d and not occupied[mm.key(x, y)] then
                best_x, best_y, best_d = x, y, d
            end
        end
    end
    return best_x, best_y
end

-- The floor's own objects (local id matches a template), by local id.
local function floor_objects(floor, snap)
    local is_template = {}
    for _, t in ipairs(snap.templates) do is_template[t.local_id] = t end
    local objects = {}
    for _, o in ipairs(snap.live) do
        if not o.is_player and is_template[o.local_id]
            and o.map_group == floor.group and o.map_num == floor.num then
            objects[o.local_id] = o
        end
    end
    return objects, is_template
end

-- Returns how many objects were compared against their saved tile, and a
-- description of every mismatch.
local function compare_objects(floor, saved, loaded)
    local before = floor_objects(floor, saved)
    local after, templates = floor_objects(floor, loaded)
    local compared, problems = 0, {}
    for id, o in pairs(after) do
        local expect = before[id] or templates[id]
        if o.x ~= expect.x or o.y ~= expect.y then
            problems[#problems + 1] = string.format("object %d at (%d,%d), expected (%d,%d)%s",
                id, o.x, o.y, expect.x, expect.y, before[id] and "" or " (template)")
        end
        if before[id] then compared = compared + 1 end
    end
    for id, o in pairs(before) do
        if after[id] == nil then
            problems[#problems + 1] = string.format("object %d (saved at (%d,%d)) is gone", id, o.x, o.y)
        end
    end
    return compared, problems
end

-- Save, Continue and compare. (tx, ty) is where the player must be.
-- `want_npcs`: at least one live NPC must be compared tile-for-tile.
local function save_and_continue(floor, label, tx, ty, want_npcs)
    capture.caption(floor.name .. ": save, reset, then CONTINUE")
    local saved, loaded = mm.save_and_continue()
    fw.assert_true(loaded ~= nil, label .. ": saved and Continued back to the overworld")
    if loaded == nil then return false end
    capture.caption(floor.name .. ": back in the same cave after CONTINUE")
    fw.assert_true(saved.player_x == tx and saved.player_y == ty, string.format(
        "%s: save holds the player at (%d,%d), got (%s,%s)",
        label, tx, ty, tostring(saved.player_x), tostring(saved.player_y)))

    fw.assert_true(loaded.group == floor.group and loaded.num == floor.num,
        label .. ": Continue loaded the same floor")
    fw.assert_eq(loaded.seed, saved.seed, label .. ": Continue kept VAR_MT_MOON_MAP_SEED")
    local dx, dy, what = mm.first_grid_difference(saved.grid, loaded.grid)
    fw.assert_true(dx == nil, string.format("%s: map grid identical after Continue%s",
        label, dx and string.format(" (first difference at (%d,%d): %s)", dx, dy, what) or ""))
    fw.assert_eq(mm.template_positions(loaded), mm.template_positions(saved),
        label .. ": object template positions identical")
    local compared, problems = compare_objects(floor, saved, loaded)
    fw.assert_true(#problems == 0, string.format("%s: every floor object on its tile (%d compared)%s",
        label, compared, #problems > 0 and (": " .. table.concat(problems, "; ")) or ""))
    if want_npcs then
        fw.assert_true(compared > 0, label .. ": at least one live NPC compared tile-for-tile")
    end
    fw.assert_true(loaded.player_x == tx and loaded.player_y == ty, string.format(
        "%s: player still at (%d,%d) after Continue, got (%s,%s)",
        label, tx, ty, tostring(loaded.player_x), tostring(loaded.player_y)))
    -- The old B1F workaround rewrote SaveBlock1.pos to the entrance on every
    -- load while the player object stayed put; the two must agree.
    fw.assert_true(loaded.pos_x == tx and loaded.pos_y == ty, string.format(
        "%s: SaveBlock1.pos still (%d,%d) after Continue, got (%d,%d)",
        label, tx, ty, loaded.pos_x, loaded.pos_y))
    local reach = mm.reachable_from_arrival(floor, loaded)
    fw.assert_true(reach[mm.key(loaded.player_x, loaded.player_y)] ~= nil,
        label .. ": player stands on floor reachable from the arrival warp")
    fw.wait_frames(90)
    return true
end

-- How many fresh entries to try for a cave with an NPC on screen at the
-- arrival (each uses the next gRngValue).
local MAX_ENTRIES = 20

local function check_floor(floor)
    fw.log("--- " .. floor.name .. " ---")

    -- 1. Fresh entry rolls a new seed and lands on the arrival warp.
    capture.caption(floor.name .. ": walking in rolls a new cave seed")
    local seed_before = mm.seed()
    local rng = RNG[floor.name]
    local entered = mm.enter_floor(floor, rng)
    fw.assert_true(mm.on_floor(floor), floor.name .. ": entered the floor")
    fw.assert_neq(entered.seed, seed_before, string.format(
        "%s: fresh entry changed VAR_MT_MOON_MAP_SEED (0x%04x -> 0x%04x)",
        floor.name, seed_before, entered.seed))
    local arrival = mm.arrival_warp(floor, entered)
    fw.assert_true(entered.player_x == arrival.x and entered.player_y == arrival.y, string.format(
        "%s: fresh entry put the player on the generated arrival warp (%d,%d), got (%s,%s)",
        floor.name, arrival.x, arrival.y, tostring(entered.player_x), tostring(entered.player_y)))

    local has_npcs = #entered.templates > 0
    if has_npcs then
        local tries = 1
        while next((floor_objects(floor, entered))) == nil and tries < MAX_ENTRIES do
            rng = rng + 1
            local seed = entered.seed
            entered = mm.enter_floor(floor, rng)
            fw.log(string.format("%s: no NPC on screen at the arrival; re-entered with gRngValue=0x%08x, seed 0x%04x -> 0x%04x",
                floor.name, rng, seed, entered.seed))
            tries = tries + 1
        end
        fw.assert_true(next((floor_objects(floor, entered))) ~= nil, string.format(
            "%s: found a cave with an NPC on screen at the arrival (gRngValue=0x%08x)", floor.name, rng))
    end
    fw.wait_until(mm.player_has_control, 600, floor.name .. ": player has control")
    fw.wait_frames(60)

    -- 2. Save at the arrival tile and Continue.
    if not save_and_continue(floor, floor.name .. " at arrival", arrival.x, arrival.y, has_npcs) then
        return
    end

    -- 3. Move far from the arrival warp, save and Continue again.
    local tx, ty = farthest_reachable(floor, entered)
    fw.assert_true(tx ~= nil, floor.name .. ": found a reachable tile to stand on")
    if tx == nil then return end
    fw.log(string.format("%s: moving player from the arrival (%d,%d) to (%d,%d)",
        floor.name, arrival.x, arrival.y, tx, ty))
    mm.poke_player(tx, ty)
    fw.wait_frames(10)
    save_and_continue(floor, floor.name .. " away from arrival", tx, ty, false)
end

fw.run(function()
    fw.log("=== Mt. Moon: save and Continue rebuild the same cave (S6) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    capture.start()
    for _, floor in ipairs(mm.FLOORS) do
        check_floor(floor)
    end
    capture.stop()
    fw.finish()
end)
