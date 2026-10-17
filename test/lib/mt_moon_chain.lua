-------------------------------------------------------------------------------
-- mt_moon_chain.lua: walk the whole Mt. Moon chain on foot (S1 and S2)
-------------------------------------------------------------------------------
--
-- Mt. Moon is a chain of three generated floors (see FLOORS in mt_moon.lua):
--
--   Route 4 west door <-> 1F <-> B1F <-> B2F <-> Route 4 east door
--
-- walk_chain("east") starts on Route 4 west of the cave and walks through
-- every floor to Route 4's east door (S1); walk_chain("west") does the
-- reverse (S2). Every step is a real D-pad press planned by walk.lua; the
-- player is never moved by writing memory once the walk has started.
-- A test that does more on the way (the demo, test/tests/demo_mt_moon.lua,
-- takes a fossil on B2F) walks the legs itself, with chain.legs() and
-- chain.walk_leg().
--
-- SETUP (prepare(), before the walk):
--   * every Mt. Moon trainer is marked defeated, so none starts a battle
--     (defeated trainers still stand where they are, as obstacles);
--   * VAR_MAP_SCENE_MT_MOON_B2F = 1, the value it has once Super Nerd
--     Miguel is beaten. B2F keeps vanilla's trigger tile at (14,11), which
--     starts Miguel's battle while the var is 0; on a generated B2F that
--     tile can be anywhere on the route;
--   * a level-100 party Pokemon and a Repel, so no wild battle starts;
--   * no roaming Pokemon (see keep_roamers_away below).
--
-- FIXED SEEDS: just before the step onto each warp, the game's random
-- state is set to a fixed value per destination floor (RNG below), so each
-- run builds the same caves.
--
-- PASS after every warp: the map is the next one in the chain and the
-- player stands on the expected arrival warp's tile (read from the new
-- map's warp table: the generated ladder, or a fixed door). Route 4's doors
-- must be warp 0 at (19,5) and warp 1 at (32,5), and 1F's door (18,37).
-- The position is taken on the first frame of the new map: arriving through
-- Route 4's cave doors, the game then walks the player one step out.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local sym     = dofile(project_dir .. "test/lib/symbols.lua")
local walk    = dofile(project_dir .. "test/lib/walk.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

local chain = {}

-- Assertions go through the TEST's framework instance (passed to
-- walk_chain or walk_leg, which set T): every dofile() of framework.lua
-- makes its own results table,
-- and only the test's own one is reported. Captions likewise go through the
-- test's frame_capture instance (also passed in): every dofile() of
-- frame_capture.lua is a separate recorder, and only the one the test
-- started writes captions.tsv.
local T = fw
local CAP = capture

local VAR_MAP_SCENE_MT_MOON_B2F = mm.read_defines("include/constants/vars.h").VAR_MAP_SCENE_MT_MOON_B2F

-- Random state written before the step that leads into each floor, per walk
-- direction. Override them to try others, e.g.
--   MT_MOON_WALK_RNG=1F=0x1234,B2F=0x99 bash test/run_test.sh ...
--
-- The 1F values are a regression case. Both roll 1F cave seed 0xf34c (the
-- two walks reach 1F after a different number of Random() calls, so they
-- need different values for the same seed). Before every object's
-- reachability was guaranteed (#26), that cave had a defeated trainer
-- standing in the only gap between the door corridor and the down-ladder
-- room, at (23,25), and both walks got stuck there. (Which seed a value
-- rolls depends on every Random() call before the roll, roaming Pokemon
-- spawns included, so the values changed when keep_roamers_away() came in;
-- on master, east 0x5EED0001 and west 0x0D88ECFA rolled 0xf34c. The west
-- value changed again with B1F's own style (#29): the walk now leaves a
-- different B1F, and 0xBF6E5E43 rolled 0x5b82 instead. And again with
-- B2F's own style (#30): the west walk crosses a different B2F, which
-- changes the Random() calls before 1F's roll, and 0x81F53EA4 rolled 0x16a7;
-- 0xEB0A448A, worked out with mt_moon.rng_for_seed from that walk, rolls
-- 0xf34c. And again with the decorations (#31): the west walk crosses the
-- same B2F and B1F caves, but 1F's roll came 65 Random() calls after the
-- write instead of 50, and 0xEB0A448A rolled 0x6e2b; 0x3EDD72E5, worked out
-- the same way, rolls 0xf34c.)
chain.RNG = {
    east = { ["1F"] = 0x151CC71A, ["B1F"] = 0x5EED0002, ["B2F"] = 0x5EED0003 },
    west = { ["1F"] = 0x3EDD72E5, ["B1F"] = 0x5EED0002, ["B2F"] = 0x5EED0003 },
}
for name, value in (os.getenv("MT_MOON_WALK_RNG") or ""):gmatch("([%w]+)=(0?x?%x+)") do
    chain.RNG.east[name] = tonumber(value)
    chain.RNG.west[name] = tonumber(value)
end

-- Where each walk starts on Route 4: a few tiles from the cave door it
-- walks into, clear of the NPCs that wander near the Pokemon Center. A test
-- may change them before walking.
chain.START = {
    east = { x = 13, y = 8 },  -- west of the west door (19,5)
    west = { x = 37, y = 6 },  -- east of the east door (32,5)
}

-- ROAMING POKEMON. This fork spawns roaming Pokemon (src/roaming_pokemon.c,
-- local ids 200 and up) on every map with wild Pokemon, Mt. Moon included.
-- They wander slowly with no range limit, and walking into one starts a
-- wild battle, so the planner keeps a tile away from them; one that
-- lingers in a one-tile-wide corridor blocks the walk for as long as it
-- stays. They aren't what these walks test, so once keep_roamers_away()
-- has run, every frame:
--   * the roamer spawn timer is held, so no new roamer appears;
--   * any roamer on the map is moved far from the player, so the game's
--     own update removes it (sprite and all) on the next frame: it drops
--     roamers more than ROAMER_DESPAWN_DISTANCE (14) tiles away.
-- A warp still spawns roamers on the new map in the frame it loads; they
-- are gone again before the player can take a step.
local ROAMER_LOCAL_ID_MIN = 200
local roamers_away = false

callbacks:add("frame", function()
    if not roamers_away or not mm.in_overworld() then return end
    emu:write16(sym.addr("gRoamerNextSpawnTimer"), 60)
    local px, py = mm.player_tile()
    if px == nil then return end
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player and o.local_id >= ROAMER_LOCAL_ID_MIN then
            mm.poke_object(o.index, px + 40, py + 40)
        end
    end
end)

function chain.keep_roamers_away()
    roamers_away = true
end

function chain.prepare()
    walk.make_walk_safe()
    chain.keep_roamers_away()
    local trainers = mm.mark_mt_moon_trainers_defeated()
    fw.log(string.format("marked %d Mt. Moon trainers defeated: %s", #trainers, table.concat(trainers, " ")))
    mm.set_var(VAR_MAP_SCENE_MT_MOON_B2F, 1)
end

local function map_name(group, num)
    if group == mm.ROUTE4.group and num == mm.ROUTE4.num then return "Route 4" end
    for _, f in ipairs(mm.FLOORS) do
        if f.group == group and f.num == num then return f.name end
    end
    return string.format("map %d.%d", group, num)
end

-- The legs of the walk toward `direction` ("east" or "west"), in order.
-- Each leg starts on `from` (a map) and walks into warp `warp` there. A leg
-- with `arrow` (1F's door) ends on an arrow warp, entered by holding that
-- direction while standing on it. The player must arrive on map `to` at its
-- warp `to_warp`; `x`/`y`, when given, is where that warp must be.
function chain.legs(direction)
    local f1, b1, b2 = mm.floor_by_name("1F"), mm.floor_by_name("B1F"), mm.floor_by_name("B2F")
    local route4 = { name = "Route 4", group = mm.ROUTE4.group, num = mm.ROUTE4.num }
    if direction == "east" then
        return {
            { from = route4, warp = mm.ROUTE4_WEST_DOOR.warp, to = f1, to_warp = f1.entrance.warp, x = 18, y = 37 },
            { from = f1, warp = f1.exit.warp, to = b1, to_warp = b1.entrance.warp },
            { from = b1, warp = b1.exit.warp, to = b2, to_warp = b2.entrance.warp },
            { from = b2, warp = b2.exit.warp, to = route4, to_warp = mm.ROUTE4_EAST_DOOR.warp,
              x = mm.ROUTE4_EAST_DOOR.x, y = mm.ROUTE4_EAST_DOOR.y },
        }
    end
    return {
        { from = route4, warp = mm.ROUTE4_EAST_DOOR.warp, to = b2, to_warp = b2.exit.warp },
        { from = b2, warp = b2.entrance.warp, to = b1, to_warp = b1.exit.warp },
        { from = b1, warp = b1.entrance.warp, to = f1, to_warp = f1.exit.warp },
        { from = f1, warp = f1.entrance.warp, arrow = "DOWN", to = route4, to_warp = mm.ROUTE4_WEST_DOOR.warp,
          x = mm.ROUTE4_WEST_DOOR.x, y = mm.ROUTE4_WEST_DOOR.y },
    }
end

-- Walk one leg (one of chain.legs(direction)), asserting through `test_fw`
-- and captioning through `test_capture`, the calling test's framework and
-- frame_capture instances (see T and CAP above). Returns false if the walk
-- can't go on.
function chain.walk_leg(leg, direction, test_fw, test_capture)
    T = test_fw or fw
    CAP = test_capture or capture
    local label = leg.from.name .. " -> " .. leg.to.name
    local group, num = mm.current_map()
    if group ~= leg.from.group or num ~= leg.from.num then
        T.assert_true(false, string.format("%s: expected to start on %s, on %s",
            label, leg.from.name, map_name(group, num)))
        return false
    end
    local target = mm.read_warps()[leg.warp + 1]
    local px, py = mm.player_tile()
    fw.log(string.format("%s: walking from (%d,%d) to warp %d at (%d,%d)",
        label, px, py, leg.warp, target.x, target.y))
    CAP.caption(string.format("%s: walking to the %s", leg.from.name,
        leg.arrow and "door" or "warp to " .. leg.to.name))

    local rng = chain.RNG[direction][leg.to.name]
    local result = walk.to(target.x, target.y, {
        on_last_step = function() if rng then mm.set_rng(rng) end end,
    })
    if result.ok and not result.warped and leg.arrow then
        if rng then mm.set_rng(rng) end
        walk.hold_into_warp(leg.arrow, 120)
    end
    fw.log(string.format("%s: %d steps, %d re-plans%s", label, result.steps, result.replans,
        result.why and (", " .. result.why) or ""))
    T.assert_true(result.ok, label .. ": walked to the warp" .. (result.why and (" (" .. result.why .. ")") or ""))
    if not result.ok then
        for _, line in ipairs(walk.describe_map(target.x, target.y)) do fw.log(line) end
        return false
    end

    local arrival = walk.wait_for_arrival(group, num, 900)
    T.assert_true(arrival ~= nil, label .. ": the warp fired and a new map loaded")
    if arrival == nil then return false end
    T.assert_true(arrival.group == leg.to.group and arrival.num == leg.to.num, string.format(
        "%s: arrived on %s", label, map_name(arrival.group, arrival.num)))
    if arrival.group ~= leg.to.group or arrival.num ~= leg.to.num then return false end

    local expect = mm.read_warps()[leg.to_warp + 1]
    T.assert_true(arrival.x == expect.x and arrival.y == expect.y, string.format(
        "%s: arrived on %s warp %d at (%d,%d), player at (%d,%d)",
        label, leg.to.name, leg.to_warp, expect.x, expect.y, arrival.x, arrival.y))
    if leg.x then
        T.assert_true(expect.x == leg.x and expect.y == leg.y, string.format(
            "%s: %s warp %d is at (%d,%d), expected (%d,%d)",
            label, leg.to.name, leg.to_warp, expect.x, expect.y, leg.x, leg.y))
    end
    if mm.floor_by_name(leg.to.name) then
        fw.log(string.format("%s: cave seed 0x%04x", leg.to.name, mm.seed()))
    end
    CAP.caption(string.format("Arrived on %s", leg.to.name))
    local ready = fw.wait_until(walk.ready, 600, label .. ": control after arriving")
    T.assert_true(ready, label .. ": the player can move again after arriving")
    return ready
end

-- Put the player on Route 4 at chain.START[direction] and wait until they
-- have control, asserting through `test_fw`. Returns true on success.
function chain.go_to_start(direction, test_fw)
    local start = chain.START[direction]
    local ok = mm.warp_to_tile(mm.ROUTE4, start.x, start.y)
    local t = test_fw or fw
    t.assert_true(ok, string.format("start on Route 4 at (%d,%d)", start.x, start.y))
    return ok
end

-- Walk the chain toward "east" (S1) or "west" (S2), asserting through
-- `test_fw` and captioning through `test_capture`, the calling test's
-- framework and frame_capture instances. Call after the game has reached
-- the overworld.
function chain.walk_chain(direction, test_fw, test_capture)
    chain.prepare()
    chain.go_to_start(direction, test_fw)
    fw.wait_frames(30)
    for _, leg in ipairs(chain.legs(direction)) do
        if not chain.walk_leg(leg, direction, test_fw, test_capture) then return false end
    end
    return true
end

return chain
