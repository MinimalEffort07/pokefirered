-------------------------------------------------------------------------------
-- demo_follower_swap.lua: the README's swap demo (gifs/FollowerSwap.gif),
-- issue #99
-------------------------------------------------------------------------------
--
-- Walking into the walking Pokemon swaps places with it, in three beats,
-- each with a caption (map coordinates as in data/maps/<Map>/map.json):
--   1. "Pikachu waits in the cave mouth when you come out": the player walks
--      down out of Diglett's Cave (north entrance) onto Route 2. The game
--      steps them out of the cave mouth (17,11) onto (17,12); Pikachu
--      appears on the cave mouth itself, the only free tile, and stays there.
--   2. "Walk into it and you swap places": the player walks back up into
--      Pikachu; they swap, and the player goes back into the cave.
--   3. "…even at a run": Viridian City's one-tile path (x=8). The player
--      arrives at (8,14) facing south with Pikachu behind them on (8,13),
--      and runs north (B+UP) through it: they swap at running speed and the
--      player runs on up the path, Pikachu following.
-- Between beats 2 and 3 the video cuts (the test's warp to Viridian City
-- isn't filmed).
--
-- Every step is a real D-pad press. Pikachu is level 100 and a Repel is
-- running, so no wild battle starts; on Route 2 the roaming Pokemon are
-- moved away.
--
-- PASS (the demo is a test too, so the GIF's story is checked, not just
-- watched):
--   * the per-frame rule of test/lib/follower.lua holds on every filmed
--     frame with Pikachu on the map: its tile is never solid, water or
--     another object's tile, and a warp only as the spawn fallback (the
--     cave mouth, until its first step);
--   * beat 1: on Route 2 the player ends on (17,12) and Pikachu on the cave
--     mouth (17,11);
--   * beat 2: they swap (the player (17,12) -> (17,11), Pikachu the other
--     way) with both 16-frame steps ending on the same frame, and the map
--     becomes Diglett's Cave north entrance within 180 frames;
--   * beat 3: they swap at running speed (8 frames each) and the player
--     reaches (8,9) within 90 frames, with Pikachu behind them.
--
-- RECORD THE GIF (frame dumps, see test/lib/frame_capture.lua: every 2nd
-- frame of the 60 fps game, so real time at 30 fps), then burn the captions
-- in with test/tools/captioned_gif.py:
--   MGBA_FRAME_DIR=/tmp/swap_demo MGBA_TEST_TIMEOUT=300 \
--       bash test/run_test.sh test/tests/demo_follower_swap.lua
--   python3 -I test/tools/captioned_gif.py /tmp/swap_demo gifs/FollowerSwap.gif \
--       --font-size 15
-- (15, not the default 18: beat 1's caption is too long for the bar at 18.)
-- captions.tsv in the frame directory says which caption each frame has.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw       = dofile(project_dir .. "test/lib/framework.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
local mm       = dofile(project_dir .. "test/lib/mt_moon.lua")
local walk     = dofile(project_dir .. "test/lib/walk.lua")
local capture  = dofile(project_dir .. "test/lib/frame_capture.lua")
follower.bind(fw)   -- the library's assertions count in this test's results

local STATE_PATH = fw.out_path("pokefirered-follower-swap-demo.ss")
local SEED = 1

-- Map ids (group, number), include/constants/map_groups.h.
local VIRIDIAN  = { 3, 1 }
local ROUTE2    = { 3, 20 }
local DIGLETT_N = { 1, 36 }   -- Diglett's Cave north entrance

-- FLAG_SYS_B_DASH (include/constants/flags.h): the running shoes.
local FLAG_SYS_B_DASH = 0x82F

local function on_map(map)
    local g, n = mm.current_map()
    return g == map[1] and n == map[2]
end

-- The swap in which the player stepped from (px, py) onto (fx, fy), or nil.
local function find_swap(watch, px, py, fx, fy)
    for _, s in ipairs(watch.swaps) do
        if s.x == px and s.y == py and s.player_x == fx and s.player_y == fy then return s end
    end
    return nil
end

-- Assert that swap `s` happened and both steps lasted `frames` frames,
-- ending together.
local function check_swap(what, s, frames)
    if not fw.assert_true(s ~= nil, what .. ": the player and Pikachu swap places") then return end
    local fl, pl = follower.swap_lengths(s)
    fw.assert_true(fl == frames and pl == frames and s.follower_end == s.player_end,
        string.format("%s: both steps last %d frames and end together (Pikachu %s, player %s)",
            what, frames, tostring(fl), tostring(pl)))
end

-- End the demo early after a failed beat.
local function stop(watch)
    if watch then watch.finish() end
    capture.stop()
    fw.finish()
end

fw.run(function()
    fw.log("=== Demo: walk into the walking Pokemon to swap places (issue #99) ===")
    if not follower.ensure_fixture(STATE_PATH) then
        fw.finish()
        return
    end
    follower.make_calm()
    mm.set_flag(FLAG_SYS_B_DASH)

    -- Start inside Diglett's Cave, one tile above its exit (4,6).
    if not (fw.assert_true(follower.warp(DIGLETT_N[1], DIGLETT_N[2], 4, 5),
                "warped into Diglett's Cave at (4,5)")
            and fw.assert_true(follower.wait_still(300), "standing still before the demo")) then
        fw.finish()
        return
    end
    follower.set_rng(SEED)
    fw.wait_frames(20)

    fw.slow_down("Follower swap demo")
    capture.start()

    -- 1. Out of the cave: DOWN onto the exit, let go once the warp starts.
    capture.caption("Pikachu waits in the cave mouth when you come out")
    fw.wait_frames(30)
    local left = fw.hold_until("DOWN", function() return on_map(ROUTE2) end, 120, 1)
    if not fw.assert_true(left, "beat 1: walked out of Diglett's Cave onto Route 2") then
        return stop()
    end
    local spawned = fw.wait_until(function()
        local px, py = follower.player()
        local f = follower.read()
        return on_map(ROUTE2) and px == 17 and py == 11 and f ~= nil and f.x == 17 and f.y == 11
    end, 300, "the player and Pikachu on the cave mouth")
    if not fw.assert_true(spawned, "beat 1: Pikachu appears on the cave mouth (17,11)") then
        return stop()
    end
    local watch = follower.new_watch("demo Route 2")
    watch.attach()
    local out = fw.wait_until(function()
        local px, py = follower.player()
        return px == 17 and py == 12 and walk.ready()
    end, 300, "the player out of the cave with control")
    local cleared = follower.clear_roamers()
    fw.wait_frames(60)
    watch.detach()
    local f = follower.read()
    fw.assert_true(out and cleared and f ~= nil and f.x == 17 and f.y == 11, string.format(
        "beat 1: the player on (17,12), Pikachu on the cave mouth (17,11) (Pikachu on (%s,%s))",
        tostring(f and f.x), tostring(f and f.y)))

    -- 2. Back in: UP into Pikachu, swap, through the cave mouth.
    capture.caption("Walk into it and you swap places")
    emu:addKey(fw.KEY.UP)
    local entered = watch.run_until(180, function() return on_map(DIGLETT_N) end)
    emu:clearKey(fw.KEY.UP)
    watch.finish()
    check_swap("beat 2", find_swap(watch, 17, 12, 17, 11), 16)
    if not fw.assert_true(entered ~= nil, "beat 2: back in Diglett's Cave within 180 frames") then
        return stop()
    end
    fw.wait_frames(20)

    -- 3. Viridian City's one-tile path, running. The warp isn't filmed.
    capture.pause()
    if not (fw.assert_true(follower.warp(VIRIDIAN[1], VIRIDIAN[2], 8, 14),
                "warped to Viridian City (8,14)")
            and fw.assert_true(follower.wait_still(300), "standing still in Viridian City")) then
        return stop()
    end
    local f3 = follower.read()
    fw.assert_true(f3 ~= nil and f3.x == 8 and f3.y == 13,
        "beat 3: Pikachu behind the player on (8,13), inside the path")
    follower.set_rng(SEED)
    watch = follower.new_watch("demo Viridian")
    capture.caption("…even at a run")
    capture.resume()
    watch.run(40)
    emu:addKey(fw.KEY.B)
    emu:addKey(fw.KEY.UP)
    local start = watch.frame_no
    local reached = watch.run(90, function(n)
        local t = watch.player_tiles[n]
        return t.x == 8 and t.y == 9
    end)
    -- On up the path, then let go and let Pikachu catch up.
    watch.run(16)
    emu:clearKey(fw.KEY.UP)
    emu:clearKey(fw.KEY.B)
    watch.run(60)
    watch.finish()
    check_swap("beat 3", find_swap(watch, 8, 14, 8, 13), 8)
    fw.assert_true(reached ~= nil, string.format(
        "beat 3: the player reaches (8,9) within 90 frames of B+UP%s",
        reached and (" (" .. (reached - start) .. " frames)") or ""))
    local px, py = follower.player()
    local f4 = follower.read()
    fw.assert_true(f4 ~= nil and f4.x == 8 and py ~= nil and f4.y > py, string.format(
        "beat 3: Pikachu follows behind the player (Pikachu on (%s,%s), the player on (%s,%s))",
        tostring(f4 and f4.x), tostring(f4 and f4.y), tostring(px), tostring(py)))

    capture.stop()
    fw.finish()
end)
