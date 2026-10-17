-------------------------------------------------------------------------------
-- test_mt_moon_fossil_dome.lua: S4 take the Dome Fossil, save, then S7 re-enter
-------------------------------------------------------------------------------
--
-- SETUP: a new game, every Mt. Moon trainer marked defeated (Super Nerd
-- Miguel too: VAR_MAP_SCENE_MT_MOON_B2F = 1, the value his script sets once
-- he is beaten), and no wild battles (test/lib/mt_moon_chain.lua's
-- prepare()). Every step is a real D-pad press planned by walk.lua.
--
-- S4, take the Dome Fossil (test/lib/mt_moon_fossil.lua has the details):
--   enter B2F, walk to the Dome Fossil, face it, press A and answer YES.
--   PASS: FLAG_GOT_DOME_FOSSIL and FLAG_GOT_FOSSIL_FROM_MT_MOON are set, the
--   bag holds the DOME FOSSIL, the Helix Fossil's object is gone, Miguel
--   stands right below the Helix Fossil's old tile, and the player has
--   control again within 600 frames.
--
-- S6 after S4, save and Continue right after taking the fossil:
--   Miguel's script ends with copyobjectxytoperm, which makes the tile his
--   walk ended on his position for good (it is written into his object
--   template, which the save keeps). Vanilla keeps him there until the
--   player leaves the floor, and so must Continue.
--   PASS: the cave seed is unchanged, and Miguel's template and his object
--   on the map are on the tile right below the Helix Fossil's old tile.
--
-- S7, leave and come back:
--   walk to B2F's exit ladder (it leads up to Route 4), then walk back into
--   Route 4's east cave door, which leads down to B2F again (a fresh entry,
--   so a new cave). Then walk next to Miguel.
--   PASS:
--     - VAR_MT_MOON_MAP_SEED changed and the map differs from S4's cave;
--     - both fossils are gone: no fossil object on the map, and both
--       fossils' hide flags (FLAG_HIDE_DOME_FOSSIL, FLAG_HIDE_HELIX_FOSSIL)
--       are set, which is what keeps them hidden on every later visit;
--     - Miguel is at his chamber start: his template is where vanilla has
--       him relative to the fossils and his trigger tile (FC's relative
--       positions), and his object on the map stands on that template tile
--       (not where his walk in S4 ended);
--     - P2 (test/tests/mt_moon_checks/p2_reachable.lua) holds on the new
--       cave.
--
-- Run:
--   MGBA_TEST_TIMEOUT=300 bash test/run_test.sh test/tests/test_mt_moon_fossil_dome.lua
-- Record a real-time video from frame dumps (see test/lib/frame_capture.lua):
--   MGBA_FRAME_DIR=/tmp/s4_frames MGBA_TEST_TIMEOUT=600 bash test/run_test.sh \
--       test/tests/test_mt_moon_fossil_dome.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local walk    = dofile(project_dir .. "test/lib/walk.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local chamber = dofile(project_dir .. "test/lib/mt_moon_chamber.lua")
local fossil  = dofile(project_dir .. "test/lib/mt_moon_fossil.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")
local p2      = dofile(project_dir .. "test/tests/mt_moon_checks/p2_reachable.lua")

-- Random state for each fresh entry into B2F.
local RNG = { s4 = 0x5EED2704, s7 = 0x5EED2707 }

local B2F = chamber.FLOOR

local function log_map(tx, ty)
    for _, line in ipairs(walk.describe_map(tx, ty)) do fw.log(line) end
end

-- Walk onto warp `warp_id` of the current map, setting gRngValue to `rng`
-- just before the last step, and wait for the next map. Returns the
-- arrival ({ group, num, x, y }) or nil.
local function walk_through_warp(warp_id, rng, label)
    local group, num = mm.current_map()
    local target = mm.read_warps()[warp_id + 1]
    local result = walk.to(target.x, target.y, {
        on_last_step = function() if rng then mm.set_rng(rng) end end,
    })
    if not result.ok then log_map(target.x, target.y) end
    fw.assert_true(result.ok and result.warped, string.format("%s: walked onto warp %d at (%d,%d)%s", label,
        warp_id, target.x, target.y, result.why and (" (" .. result.why .. ")") or ""))
    if not (result.ok and result.warped) then return nil end
    local arrival = walk.wait_for_arrival(group, num, 900)
    fw.assert_true(arrival ~= nil, label .. ": a new map loaded")
    if arrival then fw.wait_until(walk.ready, 600, label .. ": control after arriving") end
    return arrival
end

fw.run(function()
    fw.log("=== Mt. Moon: take the Dome Fossil (S4), save and Continue, leave and re-enter B2F (S7) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    chain.prepare()

    capture.start()

    -- S4.
    local before = fossil.take("dome", RNG.s4, fw)
    if before == nil then capture.stop() fw.finish() return end

    -- S6 after S4.
    capture.caption("B2F: save and Continue")
    local helix = chamber.pieces(before.templates, before.coord_events).helix
    local walked = { x = helix.x, y = helix.y + 1 }
    local saved, loaded = mm.save_and_continue()
    fw.assert_true(loaded ~= nil, "saved and continued")
    if loaded == nil then capture.stop() fw.finish() return end
    fw.assert_eq(loaded.seed, before.seed, "Continue kept the cave seed")
    for label, snap in pairs({ saved = saved, loaded = loaded }) do
        local m = chamber.pieces(snap.templates, snap.coord_events).miguel
        fw.assert_true(m.x == walked.x and m.y == walked.y, string.format(
            "%s: Miguel's template is where his walk ended (%d,%d): (%d,%d)", label, walked.x, walked.y, m.x, m.y))
    end
    local live = chamber.live(chamber.LOCALID_MIGUEL)
    fw.assert_true(live ~= nil and live.x == walked.x and live.y == walked.y, string.format(
        "after Continue Miguel's object stands where his walk ended (%d,%d): (%d,%d)", walked.x, walked.y,
        live and live.x or -1, live and live.y or -1))

    -- S7: out through the exit ladder to Route 4 ...
    capture.caption("B2F: leaving by the ladder to Route 4")
    local arrival = walk_through_warp(B2F.exit.warp, nil, "B2F -> Route 4")
    fw.assert_true(arrival ~= nil and arrival.group == mm.ROUTE4.group and arrival.num == mm.ROUTE4.num,
        "arrived on Route 4")
    if arrival == nil then capture.stop() fw.finish() return end

    -- ... and back in through Route 4's east door, which leads to B2F.
    capture.caption("Route 4: back into Mt. Moon B2F")
    arrival = walk_through_warp(mm.ROUTE4_EAST_DOOR.warp, RNG.s7, "Route 4 -> B2F")
    fw.assert_true(arrival ~= nil and arrival.group == B2F.group and arrival.num == B2F.num, "arrived on B2F again")
    if arrival == nil then capture.stop() fw.finish() return end

    local after = mm.snapshot()
    fw.log(string.format("B2F again: cave seed 0x%04x (was 0x%04x)", after.seed, before.seed))
    fw.assert_true(after.seed ~= before.seed, "re-entering rolled a new cave seed")
    fw.assert_true(after.grid.key ~= before.grid.key, "re-entering built a different cave")

    fw.assert_true(chamber.live(chamber.LOCALID_DOME) == nil and chamber.live(chamber.LOCALID_HELIX) == nil,
        "no fossil object on the map")
    fw.assert_true(mm.get_flag(chamber.FLAG_HIDE_DOME_FOSSIL) and mm.get_flag(chamber.FLAG_HIDE_HELIX_FOSSIL),
        "both fossils' hide flags are set")

    local p = chamber.pieces(after.templates, after.coord_events)
    local van = chamber.vanilla()
    fw.assert_true(p.miguel.x - p.dome.x == van.miguel.x - van.dome.x
        and p.miguel.y - p.dome.y == van.miguel.y - van.dome.y
        and p.trigger.x - p.miguel.x == van.trigger.x - van.miguel.x
        and p.trigger.y - p.miguel.y == van.trigger.y - van.miguel.y,
        string.format("Miguel's template (%d,%d) is his chamber start: Dome Fossil (%d,%d), trigger (%d,%d)",
            p.miguel.x, p.miguel.y, p.dome.x, p.dome.y, p.trigger.x, p.trigger.y))

    capture.caption("B2F: back to Miguel")
    local result = walk.to_face(p.miguel.x, p.miguel.y)
    if not result.ok then log_map(p.miguel.x, p.miguel.y) end
    fw.assert_true(result.ok, "walked next to Miguel" .. (result.why and (" (" .. result.why .. ")") or ""))
    local miguel = chamber.live(chamber.LOCALID_MIGUEL)
    fw.assert_true(miguel ~= nil and miguel.x == p.miguel.x and miguel.y == p.miguel.y, string.format(
        "Miguel's object stands on his chamber start (%d,%d): at (%d,%d)", p.miguel.x, p.miguel.y,
        miguel and miguel.x or -1, miguel and miguel.y or -1))

    local ok, why = p2.run(mm, B2F, RNG.s7, after)
    fw.assert_true(ok, "P2 reachable holds on the new cave" .. (why and (": " .. why) or ""))

    fw.wait_frames(60)
    capture.stop()
    fw.finish()
end)
