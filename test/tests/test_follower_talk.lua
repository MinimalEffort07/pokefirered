-------------------------------------------------------------------------------
-- test_follower_talk.lua
--
-- Ticket #98 (part of #45): pressing A while facing the walking Pokemon (the
-- "follower") no longer freezes the game. It acts as if the follower weren't
-- there: on plain ground nothing happens, with no message and no sound.
--
-- WHY IT FROZE. Pressing A on the field looks for an object on the tile in
-- front of the player and runs that object's script. Scripts belong to the
-- object "templates" in the map's data (data/maps/<Map>/map.json, the
-- object_events list), found by the object's local id. The follower is not
-- in any map's data: src/pokemon_follower.c creates it at run time, with
-- local id 254 (LOCALID_FOLLOWER). The lookup found no template, read a
-- "script pointer" from near address 0 instead, and the script engine ran
-- that garbage: its status stayed "running" for good, so the player never
-- got control back. The fix (src/field_control_avatar.c) skips the follower
-- in the A-button lookups, as they already skipped the player.
--
-- SCENARIOS (map coordinates, as in data/maps/<Map>/map.json):
--   K1 the follower. Viridian City: warp the player to (8,14), in the
--      one-tile path north of the city; they face south and Pikachu spawns
--      behind them at (8,13). Tap UP, so the player turns to face Pikachu
--      without stepping, then press A. Passes when, on each of the 30 frames
--      after the press, the script engine is shut down
--      (sGlobalScriptContextStatus is CONTEXT_SHUTDOWN) and the field
--      controls are unlocked (sLockFieldControls is 0), the player is still
--      on (8,14) and Pikachu on (8,13); and holding DOWN then takes the
--      player to (8,15) within 40 frames. Before the fix the controls never
--      come back.
--   K2 an NPC (regression). Pewter City: the player at (22,28), turned west
--      to face the man at (21,28). Pressing A starts his script: within 10
--      frames the script engine leaves CONTEXT_SHUTDOWN and the game's
--      "last talked to" (gSpecialVar_LastTalked) is his local id. Pressing B
--      through his message gives control back within 600 frames.
--
-- Both scenarios also run the per-frame rule of issue #46 (test/lib/
-- follower.lua, new_watch): the follower stays on walkable tiles.
--
-- SETUP. Each scenario starts from the fixture (follower.ensure_fixture: a
-- new game with Pikachu in the party and WALK chosen), so K2 doesn't depend
-- on how K1 left the game. Pikachu is made level 100 and a Repel is started
-- (follower.make_calm); the random state is seeded once both stand still.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_follower_talk.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw       = dofile(project_dir .. "test/lib/framework.lua")
local sym      = dofile(project_dir .. "test/lib/symbols.lua")
local mm       = dofile(project_dir .. "test/lib/mt_moon.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
follower.bind(fw)   -- the library's assertions count in this test's results

local STATE_PATH = fw.out_path("pokefirered-follower-talk.ss")

-- The game's random state once the scenario's picture is still (the
-- follower's idle moves are random). Any value works.
local SEED = 1

-- Map ids (group, number), include/constants/map_groups.h.
local VIRIDIAN = { 3, 1 }
local PEWTER   = { 3, 2 }

-- The script engine's status (enum in src/script.c): 0 running, 1 waiting
-- for a field task, 2 shut down (no script).
local CONTEXT_SHUTDOWN = 2

local function context_status()
    return emu:read8(sym.addr("sGlobalScriptContextStatus"))
end

local function tile_str(x, y)
    if x == nil then return "(none)" end
    return string.format("(%d,%d)", x, y)
end

-- Start a scenario from the fixture: warp the player to (px, py) on `map`,
-- wait for a still picture, seed the random state. Returns false if the
-- setup failed (already reported).
local function setup(name, map, px, py)
    fw.log(string.format("--- %s: player (%d,%d) ---", name, px, py))
    if not follower.ensure_fixture(STATE_PATH) then return false end
    follower.make_calm()
    if not fw.assert_true(follower.warp(map[1], map[2], px, py),
            name .. ": warped the player to its spot") then
        return false
    end
    if not fw.assert_true(follower.wait_still(300),
            name .. ": player and follower standing still before the start") then
        return false
    end
    follower.set_rng(SEED)
    return true
end

-- Tap `key_name` for one frame, so the player turns that way without
-- stepping, and wait until the turn is over. Returns the facing direction.
local function tap_turn(key_name)
    fw.press(key_name)
    follower.wait_still(30)
    local _, _, facing = follower.player()
    return facing
end

-- Press A (held for one frame), then call each(n) on each of the next
-- `frames` frames (n = 1 is the frame the game reads the press); stop early
-- when it returns true.
local function press_a_then(frames, each)
    emu:addKey(fw.KEY.A)
    for n = 1, frames do
        fw.yield()
        if n == 1 then emu:clearKey(fw.KEY.A) end
        if each(n) then break end
    end
    emu:clearKey(fw.KEY.A)
end

-------------------------------------------------------------------------------
-- K1: A at the follower does nothing, and the player keeps control
-------------------------------------------------------------------------------
local function k1()
    local name = "K1"
    if not setup(name, VIRIDIAN, 8, 14) then return end

    local px, py, facing = follower.player()
    fw.assert_true(facing == follower.DIR_SOUTH, string.format(
        "K1: player faces south on arrival (facing %s)", tostring(facing)))
    local f = follower.read()
    if not fw.assert_true(f ~= nil and f.x == 8 and f.y == 13, string.format(
            "K1: Pikachu spawned behind the player on (8,13) (on %s)",
            f and tile_str(f.x, f.y) or "(none)")) then
        return
    end

    local watch = follower.new_watch(name)
    watch.attach()   -- checks every frame while the code below presses keys

    facing = tap_turn("UP")
    px, py = follower.player()
    fw.assert_true(facing == follower.DIR_NORTH and px == 8 and py == 14, string.format(
        "K1: tapping UP turns the player north without a step (facing %s on %s)",
        tostring(facing), tile_str(px, py)))

    -- Press A, then check each of the next 30 frames.
    local first_bad = nil
    press_a_then(30, function(n)
        local x, y = follower.player()
        local fol = follower.read()
        local status = context_status()
        local locked = emu:read8(sym.addr("sLockFieldControls"))
        if first_bad == nil and (status ~= CONTEXT_SHUTDOWN or locked ~= 0
                or x ~= 8 or y ~= 14 or fol == nil or fol.x ~= 8 or fol.y ~= 13) then
            first_bad = string.format(
                "frame %d: script status %d, controls locked %d, player %s, Pikachu %s",
                n, status, locked, tile_str(x, y), fol and tile_str(fol.x, fol.y) or "(none)")
        end
    end)
    fw.assert_true(first_bad == nil, string.format(
        "K1: for 30 frames after A, no script runs, the controls stay unlocked and neither moves%s",
        first_bad and (" (" .. first_bad .. ")") or ""))

    -- Hold DOWN: the player turns south and walks one tile.
    local reached = nil
    emu:addKey(fw.KEY.DOWN)
    for n = 1, 40 do
        fw.yield()
        local x, y = follower.player()
        if x == 8 and y == 15 then reached = n break end
    end
    emu:clearKey(fw.KEY.DOWN)
    px, py = follower.player()
    fw.assert_true(reached ~= nil, string.format(
        "K1: the player then walks DOWN to (8,15) within 40 frames%s",
        reached and (" (frame " .. reached .. ")") or (" (on " .. tile_str(px, py) .. ")")))

    follower.wait_still(120)
    watch.detach()
    watch.finish()
end

-------------------------------------------------------------------------------
-- K2: A at an NPC still starts his script, and control comes back after it
-------------------------------------------------------------------------------
local function k2()
    local name = "K2"
    if not setup(name, PEWTER, 22, 28) then return end

    local man = nil
    for _, o in ipairs(follower.other_objects()) do
        if o.x == 21 and o.y == 28 and not o.is_player then man = o end
    end
    if not fw.assert_true(man ~= nil, "K2: the man stands at (21,28)") then return end

    local watch = follower.new_watch(name)
    watch.attach()

    local facing = tap_turn("LEFT")
    local px, py = follower.player()
    fw.assert_true(facing == follower.DIR_WEST and px == 22 and py == 28, string.format(
        "K2: the player faces the man, west, from (22,28) (facing %s on %s)",
        tostring(facing), tile_str(px, py)))

    -- Press A and wait for his script to start.
    local started = nil
    press_a_then(10, function(n)
        if context_status() ~= CONTEXT_SHUTDOWN then started = n return true end
    end)
    fw.assert_true(started ~= nil, string.format(
        "K2: A starts a script within 10 frames%s",
        started and (" (frame " .. started .. ")") or ""))
    local last_talked = emu:read16(sym.addr("gSpecialVar_LastTalked"))
    fw.assert_eq(last_talked, man.local_id, "K2: the script is the man's (last talked to)")

    -- Press B every 10 frames until his message is gone and control is back.
    local back = nil
    if started then
        for n = 1, 600 do
            if n % 10 == 1 then emu:addKey(fw.KEY.B) end
            fw.yield()
            if n % 10 == 2 then emu:clearKey(fw.KEY.B) end
            if mm.player_has_control() then back = n break end
        end
        emu:clearKey(fw.KEY.B)
    end
    fw.assert_true(back ~= nil, string.format(
        "K2: pressing B through his message gives control back within 600 frames%s",
        back and (" (frame " .. back .. ")") or ""))

    watch.detach()
    watch.finish()
end

fw.run(function()
    fw.log("=== Test: pressing A at the walking Pokemon (ticket #98) ===")
    k1()
    k2()
    fw.finish()
end)
