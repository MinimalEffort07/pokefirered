-------------------------------------------------------------------------------
-- test_mt_moon_safety_net.lua: S9, Continue never leaves the player in a wall
-------------------------------------------------------------------------------
--
-- When Continue rebuilds a Mt. Moon floor, the player's saved tile might not
-- be floor in the rebuilt cave: saves made before the cave seed existed (the
-- seed var reads 0 there), or a future generator change that alters what a
-- seed produces. The generator's safety net then moves the player to the
-- nearest reachable floor tile.
--
-- FOR EACH GENERATED FLOOR (1F, then B1F):
--
-- A. Player inside a wall (the S9 scenario from the design):
--   1. Fresh entry with a fixed gRngValue.
--   2. Poke the player's saved position onto a solid wall tile: the wall tile
--      with walls on all four sides that is closest to the middle of the map
--      (so no real walk could ever end there).
--   3. Save, soft reset, CONTINUE.
--   4. PASS when the player is ON REACHABLE FLOOR (below), and the seed var
--      and the map are unchanged.
--
-- B. A save from before cave seeds existed:
--   1. Build a reference: a fresh entry whose gRngValue is chosen to roll
--      seed 0 (see mt_moon.rng_for_seed). Remember its map and templates.
--   2. Fresh entry with a different seed, so the player is in another cave.
--      Poke the player onto a tile that is floor in this cave but wall in the
--      seed-0 cave, and poke the seed var to 0: that is exactly what an old
--      save looks like (seed var 0, but saved in an unrelated cave).
--   3. Save, soft reset, CONTINUE.
--   4. PASS when the map and object templates equal the seed-0 reference
--      (no leftover patch of the old cave around the player) and the player
--      is ON REACHABLE FLOOR.
--
-- ON REACHABLE FLOOR means:
--   - the player is no longer on the tile they were saved on,
--   - the player's tile is walkable floor reachable on foot from the
--     floor's arrival warp, and not under an object or on a warp,
--   - no such tile is closer to the saved tile (Manhattan distance), i.e.
--     the player was moved to a NEAREST reachable floor tile,
--   - SaveBlock1.pos agrees with the player object.
--
-- Run:
--   bash test/run_test.sh test/tests/test_mt_moon_safety_net.lua
-- Record frames for a video (see test/lib/frame_capture.lua):
--   MGBA_FRAME_DIR=/tmp/s9_frames bash test/run_test.sh \
--       test/tests/test_mt_moon_safety_net.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

local RNG = { ["1F"] = 0x5AFE0001, ["B1F"] = 0x5AFE0002 }
local OLD_SAVE_RNG = { ["1F"] = 0x01D5A7E1, ["B1F"] = 0x01D5A7E2 }

local function solid(grid, x, y)
    if x < 0 or y < 0 or x >= grid.w or y >= grid.h then return true end
    return not mm.passable(grid.cells[mm.key(x, y)])
end

-- A wall tile with walls on all four sides, closest to (ax, ay).
local function buried_wall_tile(grid, ax, ay)
    local best_x, best_y, best_d = nil, nil, nil
    for y = 0, grid.h - 1 do
        for x = 0, grid.w - 1 do
            if solid(grid, x, y) and solid(grid, x - 1, y) and solid(grid, x + 1, y)
                and solid(grid, x, y - 1) and solid(grid, x, y + 1) then
                local d = math.abs(x - ax) + math.abs(y - ay)
                if best_d == nil or d < best_d then
                    best_x, best_y, best_d = x, y, d
                end
            end
        end
    end
    return best_x, best_y
end

-- Poke the player to (x, y), save and Continue. Returns (saved, loaded)
-- snapshots, or nil if a step failed.
local function save_and_continue(label, x, y)
    mm.poke_player(x, y)
    fw.wait_frames(10)
    capture.caption(label .. ": save, reset, then CONTINUE")
    local saved, loaded = mm.save_and_continue()
    fw.assert_true(loaded ~= nil, label .. ": saved and Continued back to the overworld")
    if loaded == nil then return nil end
    fw.assert_true(saved.player_x == x and saved.player_y == y, string.format(
        "%s: the save holds the player at (%d,%d)", label, x, y))
    return saved, loaded
end

-- The ON REACHABLE FLOOR checks (see the header), for a player saved at
-- (sx, sy).
local function check_on_reachable_floor(label, floor, loaded, sx, sy)
    local px, py = loaded.player_x, loaded.player_y
    fw.log(string.format("%s: saved at (%d,%d), loaded at (%d,%d)", label, sx, sy, px, py))
    fw.assert_true(not (px == sx and py == sy), label .. ": player is no longer on the saved tile")

    local reach = mm.reachable_from_arrival(floor, loaded)
    fw.assert_true(reach[mm.key(px, py)] ~= nil,
        label .. ": player is on floor reachable from the arrival warp")

    -- Tiles the safety net must not use: warps and objects.
    local taken = {}
    for _, w in ipairs(loaded.warps) do taken[mm.key(w.x, w.y)] = true end
    for _, t in ipairs(loaded.templates) do taken[mm.key(t.x, t.y)] = true end
    for _, o in ipairs(loaded.live) do
        if not o.is_player then taken[mm.key(o.x, o.y)] = true end
    end
    fw.assert_true(not taken[mm.key(px, py)], label .. ": player is not on a warp or object")

    local landed = math.abs(px - sx) + math.abs(py - sy)
    local nearest = nil
    for k, _ in pairs(reach) do
        local x, y = k % 1000, k // 1000
        if not taken[k] then
            local d = math.abs(x - sx) + math.abs(y - sy)
            if nearest == nil or d < nearest then nearest = d end
        end
    end
    fw.assert_eq(landed, nearest, label .. ": moved to a nearest reachable floor tile (distance)")

    fw.assert_true(loaded.pos_x == px and loaded.pos_y == py,
        label .. ": SaveBlock1.pos agrees with the player object")
end

-- A: player saved inside a wall, same seed.
local function check_wall(floor)
    local label = floor.name .. " in wall"
    fw.log("--- " .. label .. " ---")
    capture.caption(floor.name .. ": entering")
    local entered = mm.enter_floor(floor, RNG[floor.name])
    fw.assert_true(mm.on_floor(floor), label .. ": entered the floor")
    fw.wait_until(mm.player_has_control, 600, label .. ": player has control")
    fw.wait_frames(60)

    local wx, wy = buried_wall_tile(entered.grid, entered.grid.w // 2, entered.grid.h // 2)
    fw.assert_true(wx ~= nil, label .. ": found a buried wall tile")
    if wx == nil then return end
    capture.caption(floor.name .. ": player poked into a wall, saving")
    local saved, loaded = save_and_continue(label, wx, wy)
    if saved == nil then return end
    capture.caption(floor.name .. ": safety net moved the player onto floor")

    check_on_reachable_floor(label, floor, loaded, wx, wy)
    fw.assert_eq(loaded.seed, saved.seed, label .. ": seed unchanged by Continue")
    local dx = mm.first_grid_difference(saved.grid, loaded.grid)
    fw.assert_true(dx == nil, label .. ": map unchanged by Continue")
    fw.wait_frames(90)
end

-- B: a save from before seeds existed (seed var 0, saved in another cave).
local function check_old_save(floor)
    local label = floor.name .. " old save"
    fw.log("--- " .. label .. " ---")
    capture.caption(floor.name .. ": imitating a save from before cave seeds")

    -- Reference cave for seed 0. Entries leave this same floor, so the
    -- number of Random() calls before the seed roll (k) is the same each
    -- time; measure it, then pick a gRngValue that rolls 0.
    mm.enter_floor(floor, OLD_SAVE_RNG[floor.name])
    local probe = mm.enter_floor(floor, OLD_SAVE_RNG[floor.name] + 1)
    local k = mm.rng_steps(OLD_SAVE_RNG[floor.name] + 1, probe.seed)
    fw.assert_true(k ~= nil, label .. ": traced the seed roll back to gRngValue")
    if k == nil then return end
    local reference = mm.enter_floor(floor, mm.rng_for_seed(k, 0))
    fw.assert_eq(reference.seed, 0, label .. ": reference entry rolled seed 0")
    if reference.seed ~= 0 then return end

    -- The cave the old save was "made" in.
    local current = mm.enter_floor(floor, OLD_SAVE_RNG[floor.name] + 2)
    fw.assert_neq(current.seed, 0, label .. ": the current cave has a different seed")
    fw.wait_until(mm.player_has_control, 600, label .. ": player has control")
    fw.wait_frames(30)

    -- Stand on a tile that is floor here but wall in the seed-0 cave,
    -- nearest the middle of the map.
    local here = mm.reachable_from_arrival(floor, current)
    local sx, sy, best = nil, nil, nil
    for y = 0, current.grid.h - 1 do
        for x = 0, current.grid.w - 1 do
            if here[mm.key(x, y)] and solid(reference.grid, x, y) then
                local d = math.abs(x - current.grid.w // 2) + math.abs(y - current.grid.h // 2)
                if best == nil or d < best then sx, sy, best = x, y, d end
            end
        end
    end
    fw.assert_true(sx ~= nil, label .. ": found a tile that is floor now but wall at seed 0")
    if sx == nil then return end

    mm.set_seed(0)
    local saved, loaded = save_and_continue(label, sx, sy)
    if saved == nil then return end
    capture.caption(floor.name .. ": old save rebuilt from seed 0, player moved onto floor")

    fw.assert_eq(loaded.seed, 0, label .. ": Continue used the stored seed 0")
    local dx, dy, what = mm.first_grid_difference(reference.grid, loaded.grid)
    fw.assert_true(dx == nil, string.format("%s: map equals the seed-0 cave, no leftover patch%s",
        label, dx and string.format(" (first difference at (%d,%d): %s)", dx, dy, what) or ""))
    fw.assert_eq(mm.template_positions(loaded), mm.template_positions(reference),
        label .. ": object templates equal the seed-0 cave's")
    check_on_reachable_floor(label, floor, loaded, sx, sy)
    fw.wait_frames(90)
end

fw.run(function()
    fw.log("=== Mt. Moon: Continue safety net (S9) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    capture.start()
    for _, floor in ipairs(mm.FLOORS) do
        check_wall(floor)
        check_old_save(floor)
    end
    capture.stop()
    fw.finish()
end)
