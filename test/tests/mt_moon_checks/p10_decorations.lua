-------------------------------------------------------------------------------
-- P10 decorations: each floor has about as much rubble, craters and puddles
-- as its vanilla map
-------------------------------------------------------------------------------
--
-- Vanilla Mt. Moon's caves are dotted with small decorations, each a fixed
-- arrangement of vanilla tiles (metatile ids):
--
--   RUBBLE  one solid tile of loose rocks, 0x283. Counted per tile.
--   CRATER  a 2 x 2 dent in the floor, walkable:   0x2d5 0x2d6
--                                                  0x2dd 0x2de
--   PUDDLE  water dripping from a wall: the drip    0x2ee 0x2ef   (solid,
--           tiles in the wall's bottom row and the  0x2f6 0x2f7    in the wall)
--           2-wide puddle below them, walkable.                   (walkable)
--
-- Every 0x283 on the floor counts as rubble, and every complete crater and
-- puddle counts, wherever it came from: what the player sees is what the
-- design's counts are about. That includes B1F's junction rubble (the B1F
-- style's rubble where a lower room's wall ends beside a corridor) and the
-- rubble and puddle that are part of B2F's fossil chamber prefab (vanilla
-- B2F's counts include them too).
--
-- THE BANDS (design comment on #22, section 4): each count lies between
-- half and one and a half times vanilla's count for that floor, "rounded",
-- with at least 1 wherever vanilla has any. Rounded here means rounded
-- INWARD to whole numbers: the band is every whole number c with
--     vanilla / 2 <= c <= 1.5 * vanilla,
-- that is ceil(vanilla / 2) .. floor(1.5 * vanilla). (Every vanilla count is
-- at least 1, so ceil(vanilla / 2) is too: the "at least 1" rule holds.)
--
--   floor   vanilla rubble / craters / puddles    bands
--   1F      19 / 6 / 2                            10-28 / 3-9 / 1-3
--   B1F     14 / 1 / 2                             7-21 / 1-1 / 1-3
--   B2F      7 / 4 / 2                             4-10 / 2-6 / 1-3
--
-- B2F's stairs (0x284) are not decorations here: their count is however
-- many doorways B2F's rooms need.
--
-- PASS when, on the generated floor (read from RAM after the warp):
--   1. every crater tile and every puddle tile belongs to a complete 2 x 2
--      crater or puddle, as in vanilla (no half decorations);
--   2. the rubble tiles, craters and puddles are each inside their band.
-- The vanilla counts above are also recounted from each floor's vanilla
-- map.bin with the same counting code, so a counting mistake here can't
-- go unnoticed. The summary prints min/mean/max of each count per floor.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

local RUBBLE = 0x283
local CRATER = { nw = 0x2d5, ne = 0x2d6, sw = 0x2dd, se = 0x2de }
local PUDDLE = { nw = 0x2ee, ne = 0x2ef, sw = 0x2f6, se = 0x2f7 }

-- The design's vanilla counts (#22, section 4) and the bands that follow
-- from them (see THE BANDS above).
local VANILLA = {
    ["1F"]  = { rubble = 19, craters = 6, puddles = 2 },
    ["B1F"] = { rubble = 14, craters = 1, puddles = 2 },
    ["B2F"] = { rubble = 7,  craters = 4, puddles = 2 },
}
local BANDS = {
    ["1F"]  = { rubble = { 10, 28 }, craters = { 3, 9 }, puddles = { 1, 3 } },
    ["B1F"] = { rubble = { 7, 21 },  craters = { 1, 1 }, puddles = { 1, 3 } },
    ["B2F"] = { rubble = { 4, 10 },  craters = { 2, 6 }, puddles = { 1, 3 } },
}
local KINDS = { "rubble", "craters", "puddles" }

-- Counts of a map given as metatile_at(x, y) over w x h tiles:
-- { rubble, craters, puddles }, or nil and a message if some crater or
-- puddle tile isn't part of a complete 2 x 2.
local function count_decorations(w, h, metatile_at)
    local counts = { rubble = 0, craters = 0, puddles = 0 }
    local covered = {}  -- tiles of complete craters and puddles
    local function at(x, y)
        if x < 0 or y < 0 or x >= w or y >= h then return nil end
        return metatile_at(x, y)
    end
    local function find(shape, kind)
        for y = 0, h - 2 do
            for x = 0, w - 2 do
                if at(x, y) == shape.nw and at(x + 1, y) == shape.ne
                    and at(x, y + 1) == shape.sw and at(x + 1, y + 1) == shape.se then
                    counts[kind] = counts[kind] + 1
                    for dy = 0, 1 do
                        for dx = 0, 1 do covered[(y + dy) * w + x + dx] = true end
                    end
                end
            end
        end
    end
    find(CRATER, "craters")
    find(PUDDLE, "puddles")
    local parts = {}
    for _, shape in ipairs({ CRATER, PUDDLE }) do
        for _, t in pairs(shape) do parts[t] = true end
    end
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            local m = at(x, y)
            if m == RUBBLE then counts.rubble = counts.rubble + 1 end
            if parts[m] and not covered[y * w + x] then
                return nil, string.format("tile 0x%03x at (%d,%d) is not part of a complete crater or puddle", m, x, y)
            end
        end
    end
    return counts
end

-- The vanilla counts, recounted from map.bin, checked once per floor.
local recounted = {}

local function check_vanilla_counts(floor)
    if recounted[floor.name] ~= nil then return recounted[floor.name] end
    local layout = tiles.vanilla_layout(floor)
    local counts, why = count_decorations(layout.w, layout.h, function(x, y)
        return layout.cells[y * layout.w + x + 1] & 0x3ff
    end)
    local problem = false
    if counts == nil then
        problem = "vanilla " .. floor.name .. ": " .. why
    else
        for _, kind in ipairs(KINDS) do
            if counts[kind] ~= VANILLA[floor.name][kind] then
                problem = string.format("vanilla %s's map.bin has %d %s, the design says %d", floor.name,
                    counts[kind], kind, VANILLA[floor.name][kind])
                break
            end
        end
    end
    recounted[floor.name] = problem
    return problem
end

local stats = {}

return {
    id = "P10",
    title = "decoration counts (rubble, craters, puddles)",
    run = function(h, floor, rng, snap)
        local problem = check_vanilla_counts(floor)
        if problem then return false, problem end

        local grid = snap.grid
        local counts, why = count_decorations(grid.w, grid.h, function(x, y)
            return h.metatile(grid.cells[h.key(x, y)])
        end)
        if counts == nil then return false, why end

        local s = stats[floor.name]
        if s == nil then
            s = { rubble = {}, craters = {}, puddles = {} }
            stats[floor.name] = s
        end
        local bad = {}
        for _, kind in ipairs(KINDS) do
            local band = BANDS[floor.name][kind]
            s[kind][#s[kind] + 1] = counts[kind]
            if counts[kind] < band[1] or counts[kind] > band[2] then
                bad[#bad + 1] = string.format("%d %s, expected %d-%d", counts[kind], kind, band[1], band[2])
            end
        end
        if #bad > 0 then return false, table.concat(bad, "; ") end
        return true
    end,
    summary = function(floor)
        local s = stats[floor.name]
        if s == nil then return nil end
        local parts = {}
        for _, kind in ipairs(KINDS) do
            local list = s[kind]
            local lo, hi, sum = list[1], list[1], 0
            for _, v in ipairs(list) do
                if v < lo then lo = v end
                if v > hi then hi = v end
                sum = sum + v
            end
            local band = BANDS[floor.name][kind]
            parts[#parts + 1] = string.format("%s min %d, mean %.1f, max %d (band %d-%d)", kind, lo,
                sum / #list, hi, band[1], band[2])
        end
        return string.format("%s over %d seeds | %s", floor.name, #s.rubble, table.concat(parts, " | "))
    end,
}
