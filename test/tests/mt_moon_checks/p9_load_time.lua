-------------------------------------------------------------------------------
-- P9 load time: generating a floor doesn't visibly stall the warp
-------------------------------------------------------------------------------
--
-- A generated floor is built while the map loads, between the fade to black
-- and the fade back in. If generation is slow the screen stays black longer
-- than for any other warp. This check measures it against an untouched
-- vanilla cave of the same size, Rock Tunnel 1F (48 x 40 tiles), in the same
-- run and with the same method:
--
--   frames = number of frames from handing the game the warp (what a door or
--            ladder does when its fade to black ends) until the player can
--            move again on the new map (mt_moon.timed_entry).
--
-- The fade to black before the load is the same for every warp, so leaving
-- it out doesn't change the difference.
--
-- For each seed: one timed entry into Rock Tunnel 1F, then one timed entry
-- into the floor through its entrance with the seed's gRngValue. Then one
-- plain entry into the floor (mt_moon.enter_floor), so the next check or
-- seed leaves the floor in the same state every other entry leaves it in:
-- just loaded. (A timed entry waits until the player can move, by which
-- time roaming Pokemon may have appeared; leaving a map in that state can
-- change how many Random() calls happen before the next floor rolls its
-- seed, which made P8 see a different seed for the same gRngValue.)
--
-- PASS when floor frames - Rock Tunnel frames <= 30.
-- The summary line printed after each floor gives min/mean/max of both.
-------------------------------------------------------------------------------

local MAX_EXTRA_FRAMES = 30

-- Measurements per floor name, for the summary.
local stats = {}

local function add(list, value)
    list[#list + 1] = value
end

local function describe(list)
    if #list == 0 then return "none" end
    local lo, hi, sum = list[1], list[1], 0
    for _, v in ipairs(list) do
        if v < lo then lo = v end
        if v > hi then hi = v end
        sum = sum + v
    end
    return string.format("min %d, mean %.1f, max %d", lo, sum / #list, hi)
end

return {
    id = "P9",
    title = "load time",
    run = function(h, floor, rng, snap)
        local s = stats[floor.name]
        if s == nil then
            s = { floor = {}, vanilla = {}, extra = {} }
            stats[floor.name] = s
        end
        local cave = h.VANILLA_CAVE
        local vanilla = h.timed_entry(cave, cave.warp, rng)
        local generated = h.timed_entry(floor, floor.entrance.warp, rng)
        -- The timed entry leaves Rock Tunnel, not this floor, so it can roll
        -- a different seed than `snap` has; report the one it measured.
        local timed_seed = h.seed()
        h.enter_floor(floor, rng)
        if vanilla == nil then
            return false, cave.name .. ": the player never regained control"
        end
        if generated == nil then
            return false, floor.name .. ": the player never regained control"
        end
        add(s.vanilla, vanilla)
        add(s.floor, generated)
        add(s.extra, generated - vanilla)
        if generated - vanilla > MAX_EXTRA_FRAMES then
            return false, string.format("cave seed 0x%04x: %d frames, %s took %d: %d extra (limit %d)", timed_seed,
                generated, cave.name, vanilla, generated - vanilla, MAX_EXTRA_FRAMES)
        end
        return true
    end,
    -- One line for the run log, after the floor's seeds are done.
    summary = function(floor)
        local s = stats[floor.name]
        if s == nil then return nil end
        return string.format("%s frames %s | %s frames %s | extra %s",
            floor.name, describe(s.floor), "Rock Tunnel 1F", describe(s.vanilla), describe(s.extra))
    end,
}
