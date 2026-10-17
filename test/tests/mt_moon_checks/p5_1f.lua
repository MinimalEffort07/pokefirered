-------------------------------------------------------------------------------
-- P5 looks right (1F): every pair of neighbouring tiles is one vanilla uses
-------------------------------------------------------------------------------
--
-- Two metatiles side by side have to be drawn to fit together, and vanilla
-- 1F's artist only ever placed certain ones next to each other. For every
-- pair of tiles that are neighbours north-south or east-west in the
-- generated 1F, that same pair (same two metatiles, same orientation) must
-- appear somewhere in vanilla 1F's own map, data/layouts/MtMoon_1F/map.bin
-- (see test/lib/mt_moon_tiles.lua for how pairs are counted).
--
-- PASS when the map has 0 pairs that vanilla 1F lacks, apart from the
-- ALLOWLIST below. The summary prints how many pairs each seed had.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

-- Pairs that don't count, as tiles.pair_key() writes them, each with the
-- reason it is allowed. Empty: every pair the 1F style draws (rock islands,
-- sand, the border band, the ladders, the core's signpost) is a pair from
-- vanilla 1F itself.
local ALLOWLIST = {}

-- How many bad pairs a failure message names.
local MAX_NAMED = 3

local counts = {}

-- The first map tile (x, y) where `key` occurs, for the failure message.
local function first_location(h, grid, key)
    for y = 0, grid.h - 1 do
        for x = 0, grid.w - 1 do
            local a = h.metatile(grid.cells[h.key(x, y)])
            if x + 1 < grid.w and tiles.pair_key("h", a, h.metatile(grid.cells[h.key(x + 1, y)])) == key then
                return x, y
            end
            if y + 1 < grid.h and tiles.pair_key("v", a, h.metatile(grid.cells[h.key(x, y + 1)])) == key then
                return x, y
            end
        end
    end
end

return {
    id = "P5-1F",
    title = "only vanilla 1F tile pairs",
    floors = { ["1F"] = true },
    run = function(h, floor, rng, snap)
        local report = tiles.non_vanilla_pairs(h, snap.grid, tiles.vanilla_pairs(floor), ALLOWLIST)
        counts[#counts + 1] = report.count
        if report.count == 0 then return true end
        local keys = {}
        for k in pairs(report.pairs) do keys[#keys + 1] = k end
        table.sort(keys)
        local named = {}
        for i = 1, math.min(MAX_NAMED, #keys) do
            local x, y = first_location(h, snap.grid, keys[i])
            named[#named + 1] = string.format("%s x%d (first at %d,%d)", keys[i], report.pairs[keys[i]], x, y)
        end
        return false, string.format("%d non-vanilla pairs, %d distinct: %s", report.count, #keys, table.concat(named, ", "))
    end,
    summary = function(floor)
        if #counts == 0 then return nil end
        local hi, sum = 0, 0
        for _, n in ipairs(counts) do
            if n > hi then hi = n end
            sum = sum + n
        end
        return string.format("%s over %d seeds: non-vanilla pairs mean %.1f, max %d", floor.name, #counts, sum / #counts, hi)
    end,
}
