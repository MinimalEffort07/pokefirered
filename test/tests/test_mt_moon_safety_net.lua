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
--   1. Fresh entry with a fixed gRngValue.
--   2. Poke the player's saved position onto a solid wall tile: the wall tile
--      with walls on all four sides that is closest to the middle of the map
--      (so no real walk could ever end there).
--   3. Save, soft reset, CONTINUE.
--   4. PASS when:
--        - the player is no longer on the wall tile,
--        - the player's tile is walkable floor reachable on foot from the
--          floor's arrival warp, and not under an object or on a warp,
--        - no such tile is closer to the wall tile (Manhattan distance), i.e.
--          the player was moved to a NEAREST reachable floor tile,
--        - SaveBlock1.pos agrees with the player object,
--        - the seed var and the map are unchanged.
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

local function check_floor(floor)
    fw.log("--- " .. floor.name .. " ---")
    capture.caption(floor.name .. ": entering")
    local entered = mm.enter_floor(floor, RNG[floor.name])
    fw.assert_true(mm.on_floor(floor), floor.name .. ": entered the floor")
    fw.wait_until(mm.player_has_control, 600, floor.name .. ": player has control")
    fw.wait_frames(60)

    local wx, wy = buried_wall_tile(entered.grid, entered.grid.w // 2, entered.grid.h // 2)
    fw.assert_true(wx ~= nil, floor.name .. ": found a buried wall tile")
    if wx == nil then return end
    fw.log(string.format("%s: poking the player into the wall at (%d,%d)", floor.name, wx, wy))
    mm.poke_player(wx, wy)
    fw.wait_frames(10)

    capture.caption(floor.name .. ": player poked into a wall, saving")
    local saved_ok = mm.save_game()
    fw.assert_true(saved_ok, floor.name .. ": game saved")
    if not saved_ok then return end
    local saved = mm.snapshot(true)
    fw.assert_true(saved.player_x == wx and saved.player_y == wy,
        floor.name .. ": the save holds the player inside the wall")

    capture.caption(floor.name .. ": reset, then CONTINUE")
    local loaded = mm.continue_game()
    fw.assert_true(loaded ~= nil, floor.name .. ": Continue reached the overworld")
    if loaded == nil then return end
    capture.caption(floor.name .. ": safety net moved the player onto floor")

    local px, py = loaded.player_x, loaded.player_y
    fw.log(string.format("%s: player loaded at (%d,%d)", floor.name, px, py))
    fw.assert_true(not (px == wx and py == wy), floor.name .. ": player is no longer inside the wall")

    local reach = mm.reachable_from_arrival(floor, loaded)
    fw.assert_true(reach[mm.key(px, py)] ~= nil,
        floor.name .. ": player is on floor reachable from the arrival warp")

    -- Tiles the safety net must not use: warps and objects.
    local taken = {}
    for _, w in ipairs(loaded.warps) do taken[mm.key(w.x, w.y)] = true end
    for _, t in ipairs(loaded.templates) do taken[mm.key(t.x, t.y)] = true end
    for _, o in ipairs(loaded.live) do
        if not o.is_player then taken[mm.key(o.x, o.y)] = true end
    end
    fw.assert_true(not taken[mm.key(px, py)], floor.name .. ": player is not on a warp or object")

    local landed = math.abs(px - wx) + math.abs(py - wy)
    local nearest = nil
    for k, _ in pairs(reach) do
        local x, y = k % 1000, k // 1000
        if not taken[k] then
            local d = math.abs(x - wx) + math.abs(y - wy)
            if nearest == nil or d < nearest then nearest = d end
        end
    end
    fw.assert_eq(landed, nearest, floor.name .. ": moved to a nearest reachable floor tile (distance)")

    fw.assert_true(loaded.pos_x == px and loaded.pos_y == py,
        floor.name .. ": SaveBlock1.pos agrees with the player object")
    fw.assert_eq(loaded.seed, saved.seed, floor.name .. ": seed unchanged by Continue")
    local dx = mm.first_grid_difference(saved.grid, loaded.grid)
    fw.assert_true(dx == nil, floor.name .. ": map unchanged by Continue")
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
        check_floor(floor)
    end
    capture.stop()
    fw.finish()
end)
