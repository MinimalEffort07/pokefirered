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
--      PASS: VAR_MT_MOON_MAP_SEED changed (a fresh entry rolls a new seed).
--   2. Move the player to the reachable floor tile farthest from the arrival
--      warp. On B1F this proves the old "teleport to the entrance on every
--      load" workaround is gone.
--   3. Save through START > SAVE.
--   4. Soft reset and CONTINUE.
--   5. PASS when, compared with what was saved:
--        - the map is the same floor,
--        - VAR_MT_MOON_MAP_SEED is unchanged (Continue doesn't roll),
--        - the map grid (every metatile, collision and elevation) is identical,
--        - every object template position is identical,
--        - every live object (NPCs, item balls) is on the same tile,
--        - the player is on the same tile, and that tile is walkable floor
--          reachable from the arrival warp.
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

-- The reachable, unoccupied tile farthest (in steps) from the arrival warp.
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
    return best_x, best_y, best_d
end

local function check_floor(floor)
    fw.log("--- " .. floor.name .. " ---")

    -- 1. Fresh entry rolls a new seed.
    capture.caption(floor.name .. ": walking in rolls a new cave seed")
    local seed_before = mm.seed()
    local entered = mm.enter_floor(floor, RNG[floor.name])
    fw.assert_true(mm.on_floor(floor), floor.name .. ": entered the floor")
    fw.assert_neq(entered.seed, seed_before, string.format(
        "%s: fresh entry changed VAR_MT_MOON_MAP_SEED (0x%04x -> 0x%04x)",
        floor.name, seed_before, entered.seed))
    fw.wait_until(mm.player_has_control, 600, floor.name .. ": player has control")
    fw.wait_frames(60)

    -- 2. Move away from the arrival warp.
    local tx, ty, steps = farthest_reachable(floor, entered)
    fw.assert_true(tx ~= nil, floor.name .. ": found a reachable tile to stand on")
    if tx == nil then return end
    local arrival = mm.arrival_warp(floor, entered)
    fw.log(string.format("%s: arrival warp at (%d,%d); moving player to (%d,%d), %d steps away",
        floor.name, arrival.x, arrival.y, tx, ty, steps))
    mm.poke_player(tx, ty)
    fw.wait_frames(10)

    -- 3. Save.
    capture.caption(floor.name .. ": saving the game")
    local saved_ok = mm.save_game()
    fw.assert_true(saved_ok, floor.name .. ": game saved")
    if not saved_ok then return end
    local saved = mm.snapshot(true)  -- player/objects as written to the save
    fw.assert_true(saved.player_x == tx and saved.player_y == ty, string.format(
        "%s: save holds the player at (%d,%d), got (%s,%s)",
        floor.name, tx, ty, tostring(saved.player_x), tostring(saved.player_y)))

    -- 4. Continue.
    capture.caption(floor.name .. ": reset, then CONTINUE")
    local loaded = mm.continue_game()
    fw.assert_true(loaded ~= nil, floor.name .. ": Continue reached the overworld")
    if loaded == nil then return end
    capture.caption(floor.name .. ": back in the same cave after CONTINUE")

    -- 5. Compare.
    fw.assert_true(loaded.group == floor.group and loaded.num == floor.num,
        floor.name .. ": Continue loaded the same floor")
    fw.assert_eq(loaded.seed, saved.seed, floor.name .. ": Continue kept VAR_MT_MOON_MAP_SEED")
    local dx, dy, what = mm.first_grid_difference(saved.grid, loaded.grid)
    fw.assert_true(dx == nil, string.format("%s: map grid identical after Continue%s",
        floor.name, dx and string.format(" (first difference at (%d,%d): %s)", dx, dy, what) or ""))
    fw.assert_eq(mm.template_positions(loaded), mm.template_positions(saved),
        floor.name .. ": object template positions identical")
    fw.assert_eq(mm.live_positions(loaded), mm.live_positions(saved),
        floor.name .. ": every live object on the same tile")
    fw.assert_true(loaded.player_x == tx and loaded.player_y == ty, string.format(
        "%s: player still at (%d,%d) after Continue, got (%s,%s)",
        floor.name, tx, ty, tostring(loaded.player_x), tostring(loaded.player_y)))
    -- The old B1F workaround rewrote SaveBlock1.pos to the entrance on every
    -- load while the player object stayed put; the two must agree.
    fw.assert_true(loaded.pos_x == tx and loaded.pos_y == ty, string.format(
        "%s: SaveBlock1.pos still (%d,%d) after Continue, got (%d,%d)",
        floor.name, tx, ty, loaded.pos_x, loaded.pos_y))
    local reach = mm.reachable_from_arrival(floor, loaded)
    fw.assert_true(reach[mm.key(loaded.player_x, loaded.player_y)] ~= nil,
        floor.name .. ": player stands on floor reachable from the arrival warp")
    fw.wait_frames(90)
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
