-------------------------------------------------------------------------------
-- P5 looks right (B1F): only tile pairs vanilla B1F itself uses
-------------------------------------------------------------------------------
--
-- B1F is drawn in vanilla B1F's style: rooms and corridors carved out of
-- black void, each floor run with a 2-row north wall above it (design
-- comment on #22, section 4). Whether the tiles fit together is measured by
-- neighbouring tile PAIRS (see test/lib/mt_moon_tiles.lua): two metatiles
-- side by side (or one above the other) in the generated map must also sit
-- side by side, in the same order, somewhere in vanilla B1F's own map,
-- data/layouts/MtMoon_B1F/map.bin. A pair vanilla never uses is a visible
-- seam, such as floor running straight into the side of a wall.
--
-- PASS when the generated B1F has 0 neighbouring tile pairs (north, south,
-- east or west) that are absent from vanilla B1F's map.bin, apart from the
-- ALLOWLIST below.
--
-- The summary line after the floor's seeds gives min/mean/max of the
-- non-vanilla pair count and every distinct non-vanilla pair seen.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

-- Pairs that may appear although vanilla B1F never uses them, as
-- tiles.pair_key() strings ("h:AAA:BBB": BBB east of AAA; "v:AAA:BBB": BBB
-- south of AAA), each with the reason it is allowed.
--
-- It is empty: everything the generated B1F draws, including the core's
-- stamped boulders (0x282, always in open floor) and the B1F style's
-- rubble (0x283, where a lower north wall ends against floor), comes in
-- arrangements vanilla B1F already has.
local ALLOWLIST = {}

-- How many failing pairs one failure message lists.
local LISTED_PAIRS = 6

local stats = { counts = {}, pairs = {} }

return {
    id = "P5-B1F",
    title = "looks right (only vanilla B1F tile pairs)",
    floors = { B1F = true },
    run = function(h, floor, rng, snap)
        local report = tiles.non_vanilla_pairs(h, snap.grid, tiles.vanilla_pairs(floor), ALLOWLIST)
        stats.counts[#stats.counts + 1] = report.count
        for k, n in pairs(report.pairs) do stats.pairs[k] = (stats.pairs[k] or 0) + n end
        if report.count == 0 then return true end

        local listed = {}
        for k, n in pairs(report.pairs) do listed[#listed + 1] = { k = k, n = n } end
        table.sort(listed, function(a, b) return a.n > b.n or (a.n == b.n and a.k < b.k) end)
        local names = {}
        for i = 1, math.min(LISTED_PAIRS, #listed) do
            names[#names + 1] = string.format("%s x%d", listed[i].k, listed[i].n)
        end
        return false, string.format("%d non-vanilla pairs (of %d): %s", report.count, report.total,
            table.concat(names, ", "))
    end,
    summary = function(floor)
        if #stats.counts == 0 then return nil end
        local lo, hi, sum = stats.counts[1], stats.counts[1], 0
        for _, v in ipairs(stats.counts) do
            if v < lo then lo = v end
            if v > hi then hi = v end
            sum = sum + v
        end
        local distinct = {}
        for k, n in pairs(stats.pairs) do distinct[#distinct + 1] = string.format("%s x%d", k, n) end
        table.sort(distinct)
        return string.format("%s over %d seeds | non-vanilla pairs min %d, mean %.1f, max %d | distinct: %s",
            floor.name, #stats.counts, lo, sum / #stats.counts, hi,
            #distinct == 0 and "none" or table.concat(distinct, ", "))
    end,
}
