-------------------------------------------------------------------------------
-- P5 looks right (B2F): only tile pairs vanilla B2F itself uses
-------------------------------------------------------------------------------
--
-- B2F is drawn in vanilla B2F's style: rooms divided by pink-topped ridges
-- with tall cliff faces on their south side, small walled rooms entered by
-- 2-wide stairs, and vanilla's fossil chamber as one of the rooms (design
-- comment on #22, section 4). Whether the tiles fit together is measured by
-- neighbouring tile PAIRS (see test/lib/mt_moon_tiles.lua): two metatiles
-- side by side (or one above the other) in the generated map must also sit
-- side by side, in the same order, somewhere in vanilla B2F's own map,
-- data/layouts/MtMoon_B2F/map.bin. A pair vanilla never uses is a visible
-- seam.
--
-- PASS when the generated B2F has 0 neighbouring tile pairs (north, south,
-- east or west) that are absent from vanilla B2F's map.bin, apart from the
-- ALLOWLIST below. Each allowlisted pair is only allowed at one place on the
-- fossil chamber's edge (one tile inside the 13 x 9 prefab, one outside),
-- so the same pair anywhere else still fails.
--
-- The summary line after the floor's seeds gives min/mean/max of the
-- non-vanilla pair count and every distinct non-vanilla pair seen.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")
local chamber = dofile(script_dir .. "../../lib/mt_moon_chamber.lua")

-- Pairs allowed although vanilla B2F never uses them, as tiles.pair_key()
-- strings ("h:AAA:BBB": BBB east of AAA; "v:AAA:BBB": BBB south of AAA),
-- each at one place: (dx, dy) is the pair's north (or west) tile, counted
-- from the chamber's top-left tile.
--
-- v:2ce:29a  The chamber's top-right tile is the east end of its north
--            wall, 0x29a, which vanilla only ever draws under the wall
--            column 0x292. In vanilla that column belongs to the room north
--            of the chamber, whose floor runs right behind the fossils. The
--            generated chamber keeps the 2 tiles behind the fossils solid
--            (so the fossils can only be reached past Miguel), and no
--            vanilla tile arrangement closes that room off above the
--            chamber's north wall, so the chamber hangs from the rock mass
--            (0x2ce) instead, with this one pair at its north-east corner.
local ALLOWLIST = {
    ["v:2ce:29a"] = { dx = 12, dy = -1 },
}

-- How many failing pairs one failure message lists.
local LISTED_PAIRS = 6

local stats = { counts = {}, pairs = {} }

-- Every neighbouring pair of the map that vanilla lacks and the allowlist
-- doesn't excuse: { count, total, pairs = key -> count }.
local function non_vanilla(h, grid, vanilla, cx, cy)
    local report = { count = 0, total = 0, pairs = {} }
    local function check(key, x, y)
        report.total = report.total + 1
        if vanilla[key] then return end
        local allowed = ALLOWLIST[key]
        if allowed and x == cx + allowed.dx and y == cy + allowed.dy then return end
        report.count = report.count + 1
        report.pairs[key] = (report.pairs[key] or 0) + 1
    end
    for y = 0, grid.h - 1 do
        for x = 0, grid.w - 1 do
            local a = h.metatile(grid.cells[h.key(x, y)])
            if x + 1 < grid.w then
                check(tiles.pair_key("h", a, h.metatile(grid.cells[h.key(x + 1, y)])), x, y)
            end
            if y + 1 < grid.h then
                check(tiles.pair_key("v", a, h.metatile(grid.cells[h.key(x, y + 1)])), x, y)
            end
        end
    end
    return report
end

return {
    id = "P5-B2F",
    title = "looks right (only vanilla B2F tile pairs)",
    floors = { B2F = true },
    run = function(h, floor, rng, snap)
        local dx, dy = chamber.offset(snap)
        local cx, cy = chamber.VANILLA_RECT.x1 + dx, chamber.VANILLA_RECT.y1 + dy
        local report = non_vanilla(h, snap.grid, tiles.vanilla_pairs(floor), cx, cy)
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
