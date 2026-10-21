-------------------------------------------------------------------------------
-- demo_follower_collision.lua: the README's Walk With Pokemon demo
-- (gifs/WalkWithPokemon.gif), issue #46
-------------------------------------------------------------------------------
--
-- Pikachu follows the player around Viridian City in four beats, each with
-- a caption (map coordinates as in data/maps/ViridianCity/map.json):
--   1. "Pikachu won't step onto a ledge from the side": the player starts at
--      (21,31), facing south, so Pikachu appears behind them in the gap of
--      the ledge row y=30 (open at x 20..22), at (21,30). The player goes up
--      the gap's other side, (22,30), and east along row 29 to (27,29). From
--      the gap the way to the player is east, onto the ledges (the old GIF
--      showed it walking onto them); Pikachu goes up to row 29 instead.
--   2. "…but hops down after you": the player jumps the ledge at (28,30)
--      and walks on to (28,33); Pikachu jumps the ledge row too.
--   3. "It waits on the shore while you SURF": the player walks back up
--      through the gap and along row 28 to the pond's east shore (17,28),
--      waits for Pikachu, uses SURF (slaveless: HM03 and the SOUL BADGE,
--      Pikachu doesn't know it) and surfs west. Pikachu stays on the shore.
--   4. "…and catches up when you land": the player lands at (10,28), in a
--      pocket west of the pond that can't be reached on foot. About 2
--      seconds later Pikachu appears beside them.
--
-- Every step is a real D-pad press (test/lib/walk.lua plans the route).
-- Pikachu is level 100 and a Repel is running, so no wild battle starts on
-- the pond; the text speed is FAST.
--
-- PASS (the demo is a test too, so the GIF's story is checked, not just
-- watched):
--   * the per-frame rule of test/lib/follower.lua holds on every frame from
--     the arrival in Viridian City to the end: the follower's tile is never
--     solid (ledges included), a warp, water or another object's tile;
--   * beat 1: the follower spawns in the gap at (21,30) and ends the beat
--     on row 29, above the ledges;
--   * beat 2: the follower jumps the ledge row (a 2-tile ledge jump) within
--     240 frames of the player's jump;
--   * beat 3: no teleport while the player is on water;
--   * beat 4: the follower is within 2 tiles of the player within 180
--     frames of the landing.
--
-- RECORD THE GIF (frame dumps, see test/lib/frame_capture.lua: every 2nd
-- frame of the 60 fps game, so real time at 30 fps), then burn the captions
-- in with test/tools/captioned_gif.py:
--   MGBA_FRAME_DIR=/tmp/follower_demo MGBA_TEST_TIMEOUT=300 \
--       bash test/run_test.sh test/tests/demo_follower_collision.lua
--   python3 -I test/tools/captioned_gif.py /tmp/follower_demo gifs/WalkWithPokemon.gif
-- captions.tsv in the frame directory says which caption each frame has.
--
-- MOMENTS (issue #119, see test/lib/framework.lua). In the mGBA window
-- (bash test/record_test.sh) the demo fast-forwards except at one moment
-- per beat, which plays at normal speed. Moments change only the window's
-- speed, never the frames, so the GIF's frame dumps are the same either
-- way.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw       = dofile(project_dir .. "test/lib/framework.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
local walk     = dofile(project_dir .. "test/lib/walk.lua")
local capture  = dofile(project_dir .. "test/lib/frame_capture.lua")
follower.bind(fw)   -- the library's assertions count in this test's results

local STATE_PATH = fw.out_path("pokefirered-follower-collision.ss")
local VIRIDIAN = { 3, 1 }

-- (20,32) is right below the TRAINER TIPS sign at (20,31). In FireRed a
-- sign opens when you walk into it, and its message box would hold up the
-- walk, so the routes never use that tile.
local AVOID = { [follower.key(20, 32)] = true }

-- Walk to (x, y), asserting that the walk got there.
local function walk_to(x, y)
    local r = walk.to(x, y, { avoid = AVOID })
    return fw.assert_true(r.ok, string.format("walked to (%d,%d)%s", x, y,
        r.why and (" (" .. r.why .. ")") or ""))
end

-- The follower's tile as the watch last saw it.
local function follower_tile(watch)
    local t = watch.tiles[watch.frame_no]
    return t.x, t.y
end

fw.run(function()
    fw.log("=== Demo: the walking Pokemon only walks on walkable tiles (issue #46) ===")
    if not follower.ensure_fixture(STATE_PATH) then
        fw.finish()
        return
    end
    follower.make_calm()
    follower.give_surf()

    -- After a warp the player faces south, so the follower appears behind
    -- them, north: in the gap of the ledge row.
    if not fw.assert_true(follower.warp(VIRIDIAN[1], VIRIDIAN[2], 21, 31),
            "warped to Viridian City (21,31)") then
        fw.finish()
        return
    end
    fw.assert_true(follower.wait_still(300), "player and follower standing still before the demo")
    fw.wait_frames(20)

    local watch = follower.new_watch("demo")
    watch.attach()
    capture.start()
    local start_frame = watch.frame_no
    local fx0, fy0 = follower_tile(watch)
    fw.assert_true(fx0 == 21 and fy0 == 30, string.format(
        "beat 1: follower starts in the gap at (21,30) (at (%d,%d))", fx0, fy0))

    -- 1. Side ledge: up the gap's other side and east along row 29.
    capture.caption("Pikachu won't step onto a ledge from the side")
    fw.wait_frames(40)
    walk_to(22, 31)
    walk_to(22, 29)
    -- As the player sets off east, Pikachu steps up out of the gap onto row
    -- 29 (about 20 frames from here) and follows along it.
    fw.show_next("Beat 1: Pikachu steps up onto row 29 and follows along it, never onto the ledges", 90)
    walk_to(27, 29)
    fw.wait_frames(40)
    local _, fy1 = follower_tile(watch)
    fw.assert_true(fy1 == 29, string.format(
        "beat 1: follower ends the beat on row 29, above the ledges (row %d)", fy1))

    -- 2. Ledge jump: step onto (28,29), hop down over (28,30), on to (28,33).
    capture.caption("…but hops down after you")
    walk_to(28, 29)
    -- The player's jump, then Pikachu's (it takes off about 70 frames after
    -- DOWN and lands about 30 later).
    fw.show_next("Beat 2: the player jumps the ledge, and Pikachu jumps it after them", 115)
    fw.hold_until("DOWN", function()
        local _, y = follower.player()
        return y == 31
    end, 60, 1)
    local jumped_at = watch.frame_no
    fw.assert_true(fw.wait_until(walk.ready, 120, "player standing after the ledge jump"),
        "beat 2: player jumped the ledge and stands on (28,31)")
    walk_to(28, 33)
    local function follower_jumped()
        for _, c in ipairs(watch.changes) do
            if c.jump and c.frame > jumped_at then return c end
        end
        return nil
    end
    fw.wait_until(function() return follower_jumped() ~= nil end, 240, "follower jumps the ledge")
    local hop = follower_jumped()
    fw.assert_true(hop ~= nil and hop.frame - jumped_at <= 240, string.format(
        "beat 2: follower jumps the ledge row within 240 frames of the player%s",
        hop and string.format(" ((%d,%d) -> (%d,%d), %d frames)",
            hop.from_x, hop.from_y, hop.x, hop.y, hop.frame - jumped_at) or ""))
    fw.wait_frames(40)

    -- 3. Surf: back up through the gap, along row 28 to the pond's east
    -- shore, wait for Pikachu, SURF, and west across the pond.
    capture.caption("It waits on the shore while you SURF")
    walk_to(22, 31)
    walk_to(22, 28)
    walk_to(17, 28)
    fw.assert_true(follower.wait_rejoin(watch, 240),
        "beat 3: follower within 2 tiles of the player on the shore before SURF")
    follower.face("LEFT")   -- the water is west; holding LEFT just turns and bumps
    fw.assert_true(follower.use_surf(16, 28), "beat 3: player surfs onto (16,28)")
    -- Across the pond to the landing (about 60 frames).
    fw.show_next("Beat 3: the player surfs west across the pond; Pikachu stops on the shore", 70)
    local landed = follower.surf_to("LEFT", 10, 28, watch)
    local tp = watch.teleport_while_player_on_water()
    fw.assert_true(tp == nil, "beat 3: no teleport while the player is on water")
    if not fw.assert_true(landed, "beat 4: player lands at (10,28)") then
        watch.detach()
        capture.stop()
        fw.finish()
        return
    end

    -- 4. Landing: the follower appears beside the player.
    capture.caption("…and catches up when you land")
    local rejoined = follower.wait_rejoin(watch, 180)
    if rejoined then
        -- On the frame it appears, so the moment is the first 60 of the 90
        -- frames below.
        fw.show_next("Beat 4: Pikachu appears beside the player, who has landed west of the pond")
    end
    fw.assert_true(rejoined, string.format(
        "beat 4: follower within 2 tiles of the player within 180 frames of landing%s",
        rejoined and (" (" .. (rejoined - landed) .. " frames)") or ""))
    fw.wait_frames(90)

    capture.stop()
    watch.detach()
    watch.finish()
    fw.log(string.format("demo length: %d frames (%.1f s)",
        watch.frame_no - start_frame, (watch.frame_no - start_frame) / 60))
    fw.finish()
end)
