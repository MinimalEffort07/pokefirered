-------------------------------------------------------------------------------
-- P5/P6 report: baseline for the floor-style tickets (never fails)
-------------------------------------------------------------------------------
--
-- Measures, for every seed, the two numbers the style tickets (#28 1F,
-- #29 B1F, #30 B2F) will assert on, and prints them per floor:
--
--   P5  non-vanilla pairs: how many neighbouring tile pairs of the
--       generated map never appear in that floor's vanilla map.bin, and
--       what share of all its neighbouring pairs they are (the density);
--   P6  walkable fraction: walkable tiles / all tiles.
--
-- See test/lib/mt_moon_tiles.lua for the definitions. This file only
-- REPORTS (every seed passes). A style ticket adds its own asserting check
-- for its floor, such as p5_1f.lua (0 non-vanilla pairs outside an
-- allowlist) or p6_1f.lua (walkable fraction inside 50-70%), so that
-- parallel tickets never edit the same file.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

-- How many of the most frequent non-vanilla pairs the summary lists.
local TOP_PAIRS = 5

local stats = {}

local function describe(list, fmt)
    local lo, hi, sum = list[1], list[1], 0
    for _, v in ipairs(list) do
        if v < lo then lo = v end
        if v > hi then hi = v end
        sum = sum + v
    end
    return string.format("min " .. fmt .. ", mean " .. fmt .. ", max " .. fmt, lo, sum / #list, hi)
end

return {
    id = "P5P6",
    title = "report (non-vanilla pairs, walkable fraction)",
    run = function(h, floor, rng, snap)
        local s = stats[floor.name]
        if s == nil then
            s = { counts = {}, densities = {}, walkable = {}, pairs = {} }
            stats[floor.name] = s
        end
        local report = tiles.non_vanilla_pairs(h, snap.grid, tiles.vanilla_pairs(floor))
        s.counts[#s.counts + 1] = report.count
        s.densities[#s.densities + 1] = 100 * report.count / report.total
        s.walkable[#s.walkable + 1] = 100 * tiles.walkable_fraction(h, snap.grid)
        for k, n in pairs(report.pairs) do s.pairs[k] = (s.pairs[k] or 0) + n end
        return true
    end,
    summary = function(floor)
        local s = stats[floor.name]
        if s == nil or #s.counts == 0 then return nil end
        local top = {}
        for k, n in pairs(s.pairs) do top[#top + 1] = { k = k, n = n } end
        table.sort(top, function(a, b) return a.n > b.n or (a.n == b.n and a.k < b.k) end)
        local names = {}
        for i = 1, math.min(TOP_PAIRS, #top) do
            names[#names + 1] = string.format("%s x%d", top[i].k, top[i].n)
        end
        return string.format("%s over %d seeds | non-vanilla pairs %s | density %s%% | walkable %s%% | %d distinct non-vanilla pairs, most common: %s",
            floor.name, #s.counts, describe(s.counts, "%.0f"), describe(s.densities, "%.1f"),
            describe(s.walkable, "%.1f"), #top, table.concat(names, ", "))
    end,
}
