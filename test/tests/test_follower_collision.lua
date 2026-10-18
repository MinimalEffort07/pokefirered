-------------------------------------------------------------------------------
-- test_follower_collision.lua
--
-- Issue #46 (part of #3): the walking Pokemon (the "follower") only ever
-- stands on tiles a character could stand on, and still keeps up with the
-- player.
--
-- EVERY-FRAME RULE, in every scenario (test/lib/follower.lua, new_watch):
-- fail on the first frame the follower's tile is solid (collision bits not
-- 0, which covers ledges), is a warp, is water, or is the current tile of
-- another live object. During a 2-tile ledge jump the follower's tile is the
-- landing tile. A teleport is a frame on which the follower's tile changes
-- by more than one tile.
--
-- SCENARIOS (map coordinates, as in data/maps/<Map>/map.json):
--   S1 side ledge. Viridian City, ledge row y=30: follower (22,30), first
--      ledge (23,30). Run once with the player at (26,29) (distance 5) and
--      once at (27,29) (distance 6, catch-up). Passes when the follower never
--      enters x >= 23 on row 30 during 300 frames.
--   S2 ledge jump. Viridian City, ledge (28,30): follower (28,29), player
--      (28,33). Passes when the follower is at y >= 31 within 120 frames and
--      never on (28,30).
--   S3 wall, then teleport. Route 1, tree pillar in column 9 (y=4..11):
--      follower (9,3), player (9,12) facing north, distance 9. Passes when
--      there is no step and no teleport during frames 1-119, and the
--      follower has teleported by frame 130, onto (9,13), behind the player.
--   S4 walk-on warp. Route 2, Diglett's Cave mouth (17,11) (collision 0, a
--      warp): follower (17,12), player (17,5) facing south, distance 7.
--      Passes when the follower is never on (17,11), and teleports as in S3:
--      nothing during frames 1-119, then by frame 130 onto the tile behind
--      the player, (17,4).
--   S5 NPC. Pewter City, stationary man at (21,28): follower (19,28), player
--      (23,28), distance 4. Passes when the follower is never on (21,28) and
--      does not teleport during 300 frames.
--   S6 surf. Viridian pond (x 11..16, y 26..30): the player surfs west from
--      (17,28) and lands at (10,28) (distance 7, not reachable on foot: the
--      pocket west of the pond is closed off by a cut tree and ledges).
--      Passes when the follower is never on water, does not teleport while
--      the player is on water, and is within 2 tiles of the player within
--      180 frames of landing (the first frame the player's tile is (10,28)).
--
-- SETUP. One fixture per run: a new game with Pikachu in the party and WALK
-- chosen (follower.ensure_fixture). Every scenario warps the player to its
-- spot (the follower respawns next to them), waits until both stand still,
-- then puts the follower on its starting tile (follower.place) and starts
-- watching on the next frame. Pikachu is made level 100 and a Repel is
-- started, so no wild battle starts while surfing; on Route 1 and Route 2
-- the roaming Pokemon are moved away (follower.clear_roamers).
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_follower_collision.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw       = dofile(project_dir .. "test/lib/framework.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
follower.bind(fw)   -- the library's assertions count in this test's results

local STATE_PATH = fw.out_path("pokefirered-follower-collision.ss")

-- The game's random state at the start of every scenario (see setup), so a
-- run plays out the same way each time. The rules hold for any value; with
-- 1, the code before issue #46 walks straight onto the man in S5 (with some
-- other values its random side-step happens to take it around him).
-- FOLLOWER_COLLISION_SEED overrides it, e.g. to try other random paths.
local SEED = tonumber(os.getenv("FOLLOWER_COLLISION_SEED") or "") or 1

-- Map ids (group, number), include/constants/map_groups.h.
local VIRIDIAN = { 3, 1 }
local PEWTER   = { 3, 2 }
local ROUTE1   = { 3, 19 }
local ROUTE2   = { 3, 20 }

-- Warp to the scenario's spot, face `facing` if given (only where that way
-- is solid), wait for a still picture and put the follower on (fx, fy).
-- Returns a fresh watch, or nil if the setup failed (already reported).
local function setup(name, map, px, py, fx, fy, facing, roamers)
    fw.log(string.format("--- %s: player (%d,%d), follower (%d,%d) ---", name, px, py, fx, fy))
    if not fw.assert_true(follower.warp(map[1], map[2], px, py),
            name .. ": warped the player to its spot") then
        return nil
    end
    if roamers and not fw.assert_true(follower.clear_roamers(),
            name .. ": no roaming Pokemon left on the map") then
        return nil
    end
    if facing then follower.face(facing) end
    if not follower.wait_still(300) then
        fw.assert_true(false, name .. ": player and follower standing still before the start")
        return nil
    end
    local snap = follower.map_snapshot()
    -- The starting tile itself must be walkable, or the scenario means
    -- nothing (catches a wrong coordinate in this file).
    fw.assert_true(not snap.solid(fx, fy) and not snap.warp(fx, fy) and not snap.water(fx, fy),
        string.format("%s: start tile (%d,%d) is open ground", name, fx, fy))
    follower.place(fx, fy)
    follower.set_rng(SEED)
    return follower.new_watch(name, snap)
end

-------------------------------------------------------------------------------
-- S1: never onto a ledge from the side
-------------------------------------------------------------------------------
local function s1(px)
    local name = string.format("S1 (player x=%d)", px)
    local watch = setup(name, VIRIDIAN, px, 29, 22, 30)
    if not watch then return end
    local entered = nil
    watch.run(300, function(n, x, y)
        if entered == nil and y == 30 and x ~= nil and x >= 23 then entered = n end
    end)
    watch.finish()
    fw.assert_true(entered == nil, string.format(
        "%s: follower never enters x >= 23 on row 30 in 300 frames%s",
        name, entered and (" (did on frame " .. entered .. ")") or ""))
end

-------------------------------------------------------------------------------
-- S2: jump down a ledge in its own direction
-------------------------------------------------------------------------------
local function s2()
    local name = "S2"
    local watch = setup(name, VIRIDIAN, 28, 33, 28, 29)
    if not watch then return end
    local reached = nil
    watch.run(120, function(n, x, y)
        if reached == nil and y ~= nil and y >= 31 then reached = n end
    end)
    watch.finish()
    fw.assert_true(reached ~= nil, string.format(
        "S2: follower at y >= 31 within 120 frames%s",
        reached and (" (frame " .. reached .. ")") or ""))
    fw.assert_true(not watch.visited(28, 30), "S2: follower never on the ledge (28,30)")
end

-------------------------------------------------------------------------------
-- S3 and S4: blocked in catch-up mode, then teleport behind the player
-------------------------------------------------------------------------------

-- Check the "wait 120 frames, then teleport onto (tx, ty)" outcome.
local function check_teleport(name, watch, tx, ty)
    local early = nil
    for _, c in ipairs(watch.changes) do
        if c.frame <= 119 then early = c break end
    end
    fw.assert_true(early == nil, string.format(
        "%s: no step and no teleport during frames 1-119%s", name,
        early and string.format(" (frame %d: (%d,%d) -> (%d,%d))",
            early.frame, early.from_x, early.from_y, early.x, early.y) or ""))
    local tp = watch.teleports[1]
    fw.assert_true(tp ~= nil and tp.frame >= 120 and tp.frame <= 130, string.format(
        "%s: teleported between frames 120 and 130%s", name,
        tp and (" (frame " .. tp.frame .. ")") or " (no teleport)"))
    fw.assert_true(tp ~= nil and tp.x == tx and tp.y == ty, string.format(
        "%s: teleported onto (%d,%d), behind the player%s", name, tx, ty,
        tp and string.format(" (landed on (%d,%d))", tp.x, tp.y) or ""))
end

local function s3()
    local name = "S3"
    local watch = setup(name, ROUTE1, 9, 12, 9, 3, "UP", true)
    if not watch then return end
    local _, _, facing = follower.player()
    fw.assert_true(facing == follower.DIR_NORTH, "S3: player faces north")
    watch.run(130)
    watch.finish()
    check_teleport(name, watch, 9, 13)
end

local function s4()
    local name = "S4"
    local watch = setup(name, ROUTE2, 17, 5, 17, 12, "DOWN", true)
    if not watch then return end
    local _, _, facing = follower.player()
    fw.assert_true(facing == follower.DIR_SOUTH, "S4: player faces south")
    fw.assert_true(watch.snap.warp(17, 11) and not watch.snap.solid(17, 11),
        "S4: (17,11) is a walk-on warp (collision 0)")
    watch.run(130)
    watch.finish()
    fw.assert_true(not watch.visited(17, 11), "S4: follower never on the cave mouth (17,11)")
    check_teleport(name, watch, 17, 4)
end

-------------------------------------------------------------------------------
-- S5: never onto an NPC
-------------------------------------------------------------------------------
local function s5()
    local name = "S5"
    local watch = setup(name, PEWTER, 23, 28, 19, 28)
    if not watch then return end
    local man = false
    for _, o in ipairs(follower.other_objects()) do
        if o.x == 21 and o.y == 28 and not o.is_player then man = true end
    end
    fw.assert_true(man, "S5: the man stands at (21,28)")
    watch.run(300)
    watch.finish()
    fw.assert_true(not watch.visited(21, 28), "S5: follower never on the man's tile (21,28)")
    fw.assert_true(#watch.teleports == 0, string.format(
        "S5: no teleport in 300 frames (%d)", #watch.teleports))
end

-------------------------------------------------------------------------------
-- S6: wait on the shore while the player surfs, rejoin after landing
-------------------------------------------------------------------------------
local function s6()
    local name = "S6"
    follower.give_surf()
    -- The player stands on the east shore (17,28) facing the pond (west);
    -- the follower is behind them at (18,28).
    local watch = setup(name, VIRIDIAN, 17, 28, 18, 28, "LEFT")
    if not watch then return end
    watch.attach()   -- checks every frame while the helpers below press keys

    -- Surf: A on the water, YES, dismiss the message; the player jumps
    -- onto (16,28). Then surf west until the player lands on (10,28).
    local surfing = fw.assert_true(follower.use_surf(16, 28), "S6: player surfs onto the pond at (16,28)")
    local landed = surfing and follower.surf_to("LEFT", 10, 28, watch)
    local rejoined = landed and follower.wait_rejoin(watch, 180)
    watch.detach()
    watch.finish()

    fw.assert_true(landed, "S6: player lands at (10,28)")
    local tp = watch.teleport_while_player_on_water()
    fw.assert_true(tp == nil, string.format(
        "S6: no teleport while the player is on water%s",
        tp and string.format(" (frame %d, player at (%d,%d))", tp.frame, tp.player_x, tp.player_y) or ""))
    fw.assert_true(rejoined, string.format(
        "S6: follower within 2 tiles of the player within 180 frames of landing%s",
        rejoined and (" (" .. (rejoined - landed) .. " frames)") or ""))
end

fw.run(function()
    fw.log("=== Test: follower only walks on walkable tiles (issue #46) ===")
    if not follower.ensure_fixture(STATE_PATH) then
        fw.finish()
        return
    end
    follower.make_calm()

    s1(26)
    s1(27)
    s2()
    s3()
    s4()
    s5()
    s6()

    fw.finish()
end)
