-------------------------------------------------------------------------------
-- test_follower_swap.lua
--
-- Issue #99 (part of #45): walk, run or ride the bicycle into the walking
-- Pokemon (the "follower") and you swap places with it, as in HeartGold and
-- SoulSilver, so it never blocks a cave mouth, a door or a one-tile path.
-- Only the follower swaps: NPCs stay solid.
--
-- A SWAP (test/lib/follower.lua, new_watch): on one frame the player's tile
-- becomes the follower's old tile and the follower's tile becomes the
-- player's old tile, and both steps end on the same frame (the first frame
-- each one's previous tile has caught up with its current tile). Each step
-- lasts 16 frames walking, 8 running, 6 cycling. Tiles are the current and
-- previous coordinates in gObjectEvents, the map is SaveBlock1's location,
-- riding is gPlayerAvatar.flags.
--
-- EVERY-FRAME RULE, in every scenario (test/lib/follower.lua): the
-- follower's tile is never solid, a warp, water or another live object's
-- tile, the player included, also in the middle of a swap. A warp is
-- allowed as the spawn fallback until the follower's first step, or where
-- a swap put it, until its next step.
--
-- SCENARIOS (map coordinates, as in data/maps/<Map>/map.json):
--   W1 cave mouth. Leave Diglett's Cave north entrance by walking down from
--      (4,5) onto its exit (4,6). On Route 2 the player ends on (17,12)
--      facing south and Pikachu on the cave mouth (17,11), the spawn
--      fallback. Hold UP. Passes when they swap (walking) and the map
--      becomes Diglett's Cave north entrance within 180 frames.
--   W2 door front. Pallet Town: the player at (6,9) facing north, Pikachu
--      at (6,8), right below the player's house door (6,7). Hold UP. Passes
--      when they swap and the map becomes the player's house 1F within 180
--      frames.
--   W3 one-tile path, running. Viridian City (8,14), in the one-tile path
--      at x=8: the player faces south and Pikachu spawns behind them at
--      (8,13). With running shoes, hold B+UP. Passes when they swap at
--      running speed and the player reaches (8,9) within 90 frames.
--   W4 one-tile path, cycling. As W3, arriving on the bicycle; hold UP.
--      Passes when they swap at bicycle speed, the player is still riding,
--      and they reach (8,9) within 90 frames.
--   W5 doormat. Walk into the player's house through its door: the player
--      arrives on the doormat (4,8), a warp, facing north, and Pikachu
--      spawns at (3,8). Hold LEFT: they swap, and 120 frames later the map
--      is still the house and Pikachu is still on (4,8). Hold RIGHT: they
--      swap back. Hold DOWN: the map becomes Pallet Town within 180 frames.
--   W6 head-on. Viridian City's path: the player at (8,14) facing north,
--      Pikachu put at (8,8) (6 tiles away, so it hurries toward the player),
--      UP held from that frame. Run with 8 random seeds. Each passes when
--      the player reaches (8,6) within 240 frames with Pikachu south of
--      them. Meeting it while it moves is a bump, then the swap.
--   W7 NPCs stay solid. Pewter City: the player at (22,28) holds LEFT into
--      the man at (21,28) for 60 frames. Passes when neither moves.
-- W1 to W6 fail on master, where the follower is solid.
--
-- SETUP. One fixture per run: a new game with Pikachu in the party and WALK
-- chosen (follower.ensure_fixture). Pikachu is made level 100 and a Repel
-- is started, so no wild battle starts; on Route 2 the roaming Pokemon are
-- moved away (follower.clear_roamers). Every scenario seeds the game's
-- random state at its start.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_follower_swap.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw       = dofile(project_dir .. "test/lib/framework.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
local mm       = dofile(project_dir .. "test/lib/mt_moon.lua")
local sym      = dofile(project_dir .. "test/lib/symbols.lua")
local walk     = dofile(project_dir .. "test/lib/walk.lua")
follower.bind(fw)   -- the library's assertions count in this test's results

local STATE_PATH = fw.out_path("pokefirered-follower-swap.ss")

-- The random state each scenario starts from (W6 runs with W6_SEEDS).
-- FOLLOWER_SWAP_SEED overrides it, e.g. to try other random paths.
local SEED = tonumber(os.getenv("FOLLOWER_SWAP_SEED") or "") or 1
local W6_SEEDS = { 1, 2, 3, 4, 5, 6, 7, 8 }

-- Map ids (group, number), include/constants/map_groups.h.
local PALLET    = { 3, 0 }
local VIRIDIAN  = { 3, 1 }
local PEWTER    = { 3, 2 }
local ROUTE2    = { 3, 20 }
local DIGLETT_N = { 1, 36 }   -- Diglett's Cave north entrance
local HOUSE_1F  = { 4, 0 }    -- the player's house, 1F

-- How many frames one step takes (MOVE_SPEED_* in event_object_movement.c:
-- 1 pixel a frame walking, 2 running, 2-3 on the bicycle; a tile is 16).
local STEP_FRAMES = { walking = 16, running = 8, cycling = 6 }

-- gPlayerAvatar.flags bit 1 (PLAYER_AVATAR_FLAG_MACH_BIKE): riding the
-- bicycle. FireRed's only bicycle uses the Mach Bike flag.
local PLAYER_AVATAR_FLAG_MACH_BIKE = 0x02
-- FLAG_SYS_B_DASH (SYS_FLAGS 0x800 + 0x2F, include/constants/flags.h): the
-- running shoes. With it, holding B while walking runs.
local FLAG_SYS_B_DASH = 0x82F

local function on_map(map)
    local g, n = mm.current_map()
    return g == map[1] and n == map[2]
end

local function key_down(name) emu:addKey(fw.KEY[name]) end
local function key_up(name) emu:clearKey(fw.KEY[name]) end

-- The follower's tile right now (nil when it has no sprite).
local function follower_tile()
    local f = follower.read()
    if f == nil then return nil end
    return f.x, f.y
end

-- Warp to (x, y) on `map` and wait for a still picture. After a warp the
-- player faces south and the follower appears behind them (north) when that
-- tile is free.
local function arrive(name, map, x, y)
    fw.log(string.format("--- %s: warp to %d.%d (%d,%d) ---", name, map[1], map[2], x, y))
    if not fw.assert_true(follower.warp(map[1], map[2], x, y),
            string.format("%s: warped the player to (%d,%d)", name, x, y)) then
        return false
    end
    return fw.assert_true(follower.wait_still(300),
        name .. ": player and follower standing still before the start")
end

-- Assert where the player and the follower stand before a scenario starts.
local function check_start(name, px, py, facing, fx, fy)
    local x, y, f = follower.player()
    local ax, ay = follower_tile()
    local dirs = { [1] = "south", [2] = "north", [3] = "west", [4] = "east" }
    return fw.assert_true(x == px and y == py and f == facing and ax == fx and ay == fy,
        string.format("%s: player on (%d,%d) facing %s, Pikachu on (%d,%d)"
            .. " (player (%s,%s) facing %s, Pikachu (%s,%s))",
            name, px, py, dirs[facing], fx, fy,
            tostring(x), tostring(y), tostring(dirs[f]), tostring(ax), tostring(ay)))
end

-- The swap in which the player stepped from (px, py) onto (fx, fy) and the
-- follower the other way, from `watch.swaps`, or nil.
local function find_swap(watch, px, py, fx, fy)
    for _, s in ipairs(watch.swaps) do
        if s.player_x == fx and s.player_y == fy and s.x == px and s.y == py then return s end
    end
    return nil
end

-- Assert a swap (see the header): the player from (px, py) onto (fx, fy),
-- the follower from (fx, fy) onto (px, py), both steps ending on the same
-- frame and lasting STEP_FRAMES[how].
local function check_swap(name, watch, how, px, py, fx, fy)
    local s = find_swap(watch, px, py, fx, fy)
    fw.assert_true(s ~= nil, string.format(
        "%s: the player (%d,%d) -> (%d,%d) and Pikachu (%d,%d) -> (%d,%d) swap on one frame%s",
        name, px, py, fx, fy, fx, fy, px, py,
        s and (" (frame " .. s.frame .. ")") or
            string.format(" (no such swap; %d swaps, player now on (%s,%s))",
                #watch.swaps, tostring(watch.player_tiles[watch.frame_no].x),
                tostring(watch.player_tiles[watch.frame_no].y))))
    if not s then return nil end
    local fl, pl = follower.swap_lengths(s)
    fw.assert_true(s.follower_end ~= nil and s.follower_end == s.player_end, string.format(
        "%s: both steps of the swap end on the same frame (Pikachu's on %s, the player's on %s)",
        name, tostring(s.follower_end), tostring(s.player_end)))
    fw.assert_true(fl == STEP_FRAMES[how] and pl == STEP_FRAMES[how], string.format(
        "%s: both steps last %d frames, %s speed (Pikachu %s, player %s)",
        name, STEP_FRAMES[how], how, tostring(fl), tostring(pl)))
    return s
end

-- Hold `key` until the watch has seen its `count`-th swap end (both steps)
-- or `frames` frames pass, checking every frame; then let go. Returns true
-- if the swap ended.
local function hold_until_swapped(watch, key, count, frames)
    key_down(key)
    local done = watch.run(frames, function()
        local s = watch.swaps[count]
        return s ~= nil and s.follower_end ~= nil and s.player_end ~= nil
    end)
    key_up(key)
    return done ~= nil
end

-------------------------------------------------------------------------------
-- W1: the cave mouth
-------------------------------------------------------------------------------
local function w1()
    local name = "W1"
    if not arrive(name, DIGLETT_N, 4, 5) then return end
    follower.set_rng(SEED)

    -- Walk down onto the exit (4,6). DOWN is let go as soon as the warp
    -- starts (SaveBlock1's location changes), so on Route 2 only the game's
    -- own step out of the cave moves the player.
    local left = fw.hold_until("DOWN", function() return on_map(ROUTE2) end, 120, 1)
    if not fw.assert_true(left, "W1: walked onto Diglett's Cave's exit; the map becomes Route 2") then
        return
    end
    -- Route 2's objects appear once the map has loaded: the player on the
    -- cave mouth (17,11) and Pikachu on the same tile, the spawn fallback
    -- (no tile next to the player is free).
    local spawned = fw.wait_until(function()
        local px, py = follower.player()
        local fx, fy = follower_tile()
        return on_map(ROUTE2) and px == 17 and py == 11 and fx == 17 and fy == 11
    end, 300, "W1: the player and Pikachu on the cave mouth (17,11)")
    if not fw.assert_true(spawned, "W1: Pikachu spawns on the player's tile, the cave mouth (17,11)") then
        return
    end
    local watch = follower.new_watch(name)
    watch.attach()
    -- The game's own step out of the cave, south onto (17,12), then
    -- control comes back.
    local out = fw.wait_until(function()
        local px, py = follower.player()
        return px == 17 and py == 12 and walk.ready()
    end, 300, "W1: player out of the cave on (17,12) with control")
    local cleared = follower.clear_roamers()
    watch.detach()
    if not (fw.assert_true(out, "W1: the player steps out of the cave onto (17,12)")
            and fw.assert_true(cleared, "W1: no roaming Pokemon left on Route 2")) then
        watch.finish()
        return
    end
    if not check_start(name, 17, 12, follower.DIR_SOUTH, 17, 11) then
        watch.finish()
        return
    end
    fw.assert_true(watch.snap.warp(17, 11), "W1: the cave mouth (17,11) is a warp")

    -- Walk back in: hold UP into Pikachu.
    key_down("UP")
    local entered = watch.run_until(180, function() return on_map(DIGLETT_N) end)
    key_up("UP")
    watch.finish()
    check_swap(name, watch, "walking", 17, 12, 17, 11)
    fw.assert_true(entered ~= nil, string.format(
        "W1: the map becomes Diglett's Cave north entrance within 180 frames of holding UP%s",
        entered and (" (" .. entered .. " frames)") or ""))
end

-------------------------------------------------------------------------------
-- W2: in front of a door
-------------------------------------------------------------------------------
local function w2()
    local name = "W2"
    if not arrive(name, PALLET, 6, 9) then return end
    follower.turn("UP")   -- face north, toward Pikachu and the door
    if not fw.assert_true(follower.wait_still(300), "W2: standing still after turning north") then
        return
    end
    if not check_start(name, 6, 9, follower.DIR_NORTH, 6, 8) then return end
    follower.set_rng(SEED)
    local watch = follower.new_watch(name)

    key_down("UP")
    local entered = watch.run_until(180, function() return on_map(HOUSE_1F) end)
    key_up("UP")
    watch.finish()
    check_swap(name, watch, "walking", 6, 9, 6, 8)
    fw.assert_true(entered ~= nil, string.format(
        "W2: the map becomes the player's house 1F within 180 frames of holding UP%s",
        entered and (" (" .. entered .. " frames)") or ""))
end

-------------------------------------------------------------------------------
-- W3 and W4: Viridian City's one-tile path, running and cycling
-------------------------------------------------------------------------------

-- Hold `keys` until the player reaches (8,9), at most `frames` frames.
-- Returns the number of frames it took, or nil.
local function up_the_path(watch, keys, frames)
    for _, k in ipairs(keys) do key_down(k) end
    local start = watch.frame_no
    local reached = watch.run(frames, function()
        local t = watch.player_tiles[watch.frame_no]
        return t.x == 8 and t.y == 9
    end)
    for _, k in ipairs(keys) do key_up(k) end
    return reached and (reached - start)
end

local function w3()
    local name = "W3"
    mm.set_flag(FLAG_SYS_B_DASH)
    if not arrive(name, VIRIDIAN, 8, 14) then return end
    if not check_start(name, 8, 14, follower.DIR_SOUTH, 8, 13) then return end
    follower.set_rng(SEED)
    local watch = follower.new_watch(name)

    local took = up_the_path(watch, { "B", "UP" }, 90)
    watch.finish()
    check_swap(name, watch, "running", 8, 14, 8, 13)
    fw.assert_true(took ~= nil, string.format(
        "W3: the player reaches (8,9) within 90 frames of holding B+UP%s",
        took and (" (" .. took .. " frames)") or ""))
end

local function w4()
    local name = "W4"
    -- Arrive on the bicycle: the warp builds the player from the "initial
    -- avatar state" (sInitialPlayerAvatarState in src/overworld.c, its
    -- first byte the avatar flags), which a real warp copies from
    -- gPlayerAvatar.flags as it starts. The test's warp skips that copy, so
    -- the bicycle bit is written there directly.
    emu:write8(sym.addr("sInitialPlayerAvatarState"), PLAYER_AVATAR_FLAG_MACH_BIKE)
    if not arrive(name, VIRIDIAN, 8, 14) then return end
    local riding = (emu:read8(sym.addr("gPlayerAvatar")) & PLAYER_AVATAR_FLAG_MACH_BIKE) ~= 0
    if not fw.assert_true(riding, "W4: the player arrives on the bicycle") then return end
    if not check_start(name, 8, 14, follower.DIR_SOUTH, 8, 13) then return end
    follower.set_rng(SEED)
    local watch = follower.new_watch(name)

    local took = up_the_path(watch, { "UP" }, 90)
    watch.finish()
    check_swap(name, watch, "cycling", 8, 14, 8, 13)
    riding = (emu:read8(sym.addr("gPlayerAvatar")) & PLAYER_AVATAR_FLAG_MACH_BIKE) ~= 0
    fw.assert_true(riding, "W4: the player is still riding the bicycle")
    fw.assert_true(took ~= nil, string.format(
        "W4: the player reaches (8,9) within 90 frames of holding UP%s",
        took and (" (" .. took .. " frames)") or ""))
end

-------------------------------------------------------------------------------
-- W5: the doormat
-------------------------------------------------------------------------------
local function w5()
    local name = "W5"
    -- In front of the house door (6,7); Pikachu appears beside the player
    -- (the tile behind them, north, is the door itself).
    if not arrive(name, PALLET, 6, 8) then return end
    local inside = fw.hold_until("UP", function() return on_map(HOUSE_1F) end, 240, 1)
    if not fw.assert_true(inside, "W5: walked in through the house door") then return end
    local ready = fw.wait_until(function()
        local px, py = follower.player()
        local f = follower.read()
        return on_map(HOUSE_1F) and px == 4 and py == 8 and f ~= nil and not f.held
            and walk.ready()
    end, 300, "W5: in the house with control")
    if not fw.assert_true(ready, "W5: the player stands in the house with control") then return end
    if not check_start(name, 4, 8, follower.DIR_NORTH, 3, 8) then return end
    follower.set_rng(SEED)
    local watch = follower.new_watch(name)
    fw.assert_true(watch.snap.warp(4, 8), "W5: the doormat (4,8) is a warp")

    -- LEFT: the player onto (3,8), Pikachu onto the doormat.
    hold_until_swapped(watch, "LEFT", 1, 90)
    check_swap(name .. " LEFT", watch, "walking", 4, 8, 3, 8)
    watch.run(120)
    local fx, fy = follower_tile()
    fw.assert_true(on_map(HOUSE_1F) and fx == 4 and fy == 8, string.format(
        "W5: 120 frames later the map is still the house and Pikachu is still on the doormat (4,8)"
        .. " (Pikachu on (%s,%s))", tostring(fx), tostring(fy)))

    -- RIGHT: swap back, the player onto the doormat.
    hold_until_swapped(watch, "RIGHT", 2, 90)
    check_swap(name .. " RIGHT", watch, "walking", 3, 8, 4, 8)

    -- DOWN: out through the doormat.
    key_down("DOWN")
    local out = watch.run_until(180, function() return on_map(PALLET) end)
    key_up("DOWN")
    watch.finish()
    fw.assert_true(out ~= nil, string.format(
        "W5: the map becomes Pallet Town within 180 frames of holding DOWN%s",
        out and (" (" .. out .. " frames)") or ""))
end

-------------------------------------------------------------------------------
-- W6: head-on in the one-tile path
-------------------------------------------------------------------------------
local function w6(seed)
    local name = string.format("W6 (seed %d)", seed)
    if not arrive(name, VIRIDIAN, 8, 14) then return end
    follower.turn("UP")   -- face north, up the path
    if not fw.assert_true(follower.wait_still(300), name .. ": standing still after turning north") then
        return
    end
    if not check_start(name, 8, 14, follower.DIR_NORTH, 8, 13) then return end
    -- Pikachu 6 tiles up the path, and UP held from this same frame.
    follower.place(8, 8)
    follower.set_rng(seed)
    local watch = follower.new_watch(name)

    key_down("UP")
    local reached = watch.run(240, function()
        local t = watch.player_tiles[watch.frame_no]
        return t.x == 8 and t.y == 6
    end)
    key_up("UP")
    watch.finish()
    local fx, fy = follower_tile()
    local px, py = follower.player()
    fw.assert_true(reached ~= nil, string.format(
        "%s: the player reaches (8,6) within 240 frames%s", name,
        reached and (" (" .. reached .. " frames)") or ""))
    fw.assert_true(fx == px and fy ~= nil and py ~= nil and fy > py, string.format(
        "%s: Pikachu is south of the player (Pikachu on (%s,%s), the player on (%s,%s))",
        name, tostring(fx), tostring(fy), tostring(px), tostring(py)))
    fw.log(string.format("%s: %d swap(s), first on frame %s", name, #watch.swaps,
        tostring(watch.swaps[1] and watch.swaps[1].frame)))
end

-------------------------------------------------------------------------------
-- W7: NPCs stay solid
-------------------------------------------------------------------------------
local function w7()
    local name = "W7"
    if not arrive(name, PEWTER, 22, 28) then return end
    local man = nil
    for _, o in ipairs(follower.other_objects()) do
        if o.x == 21 and o.y == 28 and not o.is_player then man = o.index end
    end
    if not fw.assert_true(man ~= nil, "W7: the man stands at (21,28)") then return end
    follower.set_rng(SEED)
    local watch = follower.new_watch(name)

    local moved = nil
    key_down("LEFT")
    watch.run(60, function(n)
        local t = watch.player_tiles[n]
        local still = false
        for _, o in ipairs(follower.other_objects()) do
            if o.index == man then still = o.x == 21 and o.y == 28 end
        end
        if moved == nil and not (t.x == 22 and t.y == 28 and still) then moved = n end
    end)
    key_up("LEFT")
    watch.finish()
    fw.assert_true(moved == nil, string.format(
        "W7: neither the player (22,28) nor the man (21,28) moves in 60 frames of LEFT%s",
        moved and (" (moved on frame " .. moved .. ")") or ""))
    fw.assert_true(#watch.swaps == 0, string.format("W7: no swap (%d)", #watch.swaps))
end

fw.run(function()
    fw.log("=== Test: walking into the walking Pokemon swaps places (issue #99) ===")
    if not follower.ensure_fixture(STATE_PATH) then
        fw.finish()
        return
    end
    follower.make_calm()

    w7()
    w2()
    w5()
    w1()
    w3()
    for _, seed in ipairs(W6_SEEDS) do w6(seed) end
    w4()   -- last: the player is left on the bicycle

    fw.finish()
end)
