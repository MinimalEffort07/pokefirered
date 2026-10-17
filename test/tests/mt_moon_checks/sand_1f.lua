-------------------------------------------------------------------------------
-- Sand (1F): sand comes in soft-edged patches of at least 3x3 tiles
-------------------------------------------------------------------------------
--
-- Vanilla 1F has patches of yellow sand: a middle of full sand tiles
-- (metatile 0x2fc) ringed by edge tiles that are part sand, part cave floor,
-- so the sand fades into the floor. It never has single tiles of sand.
--
-- A SAND TILE is the full sand tile or one of vanilla's sand edge tiles
-- (SAND_TILES below). A PATCH is a group of sand tiles that touch, sides or
-- corners. PASS when:
--   1. soft edges: no full sand tile has plain cave floor (0x281) directly
--      north, south, west or east of it (an edge tile must be in between);
--   2. every patch spans at least 3 x 3 tiles and has at least one full
--      sand tile, so there is no single-tile sand;
--   3. the floor has at least one patch, as vanilla 1F does.
-- The summary prints how many patches and sand tiles the floors had.
-------------------------------------------------------------------------------

local FULL_SAND = 0x2fc
local PLAIN_FLOOR = 0x281
local MIN_SPAN = 3

-- The full sand tile and vanilla 1F's sand edge tiles.
local SAND_TILES = {
    [0x2fc] = true,
    [0x2f3] = true, [0x2f4] = true, [0x2f5] = true, [0x2f8] = true, [0x2f9] = true,
    [0x2fb] = true, [0x2fd] = true, [0x300] = true, [0x301] = true, [0x303] = true,
    [0x304] = true, [0x305] = true,
}

local patch_counts, tile_counts = {}, {}

local function describe(list)
    local lo, hi, sum = list[1], list[1], 0
    for _, v in ipairs(list) do
        if v < lo then lo = v end
        if v > hi then hi = v end
        sum = sum + v
    end
    return string.format("min %d, mean %.1f, max %d", lo, sum / #list, hi)
end

return {
    id = "SAND-1F",
    title = "sand in soft-edged patches of 3x3 or more",
    floors = { ["1F"] = true },
    run = function(h, floor, rng, snap)
        local grid = snap.grid
        local function tile(x, y)
            if x < 0 or y < 0 or x >= grid.w or y >= grid.h then return nil end
            return h.metatile(grid.cells[h.key(x, y)])
        end

        -- 1. soft edges
        for y = 0, grid.h - 1 do
            for x = 0, grid.w - 1 do
                if tile(x, y) == FULL_SAND then
                    for _, d in ipairs({ { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }) do
                        if tile(x + d[1], y + d[2]) == PLAIN_FLOOR then
                            return false, string.format("full sand at (%d,%d) touches plain floor at (%d,%d)",
                                x, y, x + d[1], y + d[2])
                        end
                    end
                end
            end
        end

        -- 2. patches, found with a flood fill over sand tiles (8 directions)
        local seen, patches, sand_tiles = {}, 0, 0
        for y = 0, grid.h - 1 do
            for x = 0, grid.w - 1 do
                if SAND_TILES[tile(x, y)] and not seen[h.key(x, y)] then
                    local x1, y1, x2, y2, full = x, y, x, y, 0
                    local stack = { { x, y } }
                    seen[h.key(x, y)] = true
                    while #stack > 0 do
                        local c = table.remove(stack)
                        local cx, cy = c[1], c[2]
                        sand_tiles = sand_tiles + 1
                        if tile(cx, cy) == FULL_SAND then full = full + 1 end
                        x1, y1 = math.min(x1, cx), math.min(y1, cy)
                        x2, y2 = math.max(x2, cx), math.max(y2, cy)
                        for dy = -1, 1 do
                            for dx = -1, 1 do
                                local nx, ny = cx + dx, cy + dy
                                if SAND_TILES[tile(nx, ny)] and not seen[h.key(nx, ny)] then
                                    seen[h.key(nx, ny)] = true
                                    stack[#stack + 1] = { nx, ny }
                                end
                            end
                        end
                    end
                    patches = patches + 1
                    if x2 - x1 + 1 < MIN_SPAN or y2 - y1 + 1 < MIN_SPAN or full == 0 then
                        return false, string.format("sand patch at (%d,%d)-(%d,%d) is %dx%d tiles with %d full sand tiles",
                            x1, y1, x2, y2, x2 - x1 + 1, y2 - y1 + 1, full)
                    end
                end
            end
        end

        -- 3. sand at all
        patch_counts[#patch_counts + 1] = patches
        tile_counts[#tile_counts + 1] = sand_tiles
        if patches == 0 then
            return false, "no sand on the floor"
        end
        return true
    end,
    summary = function(floor)
        if #patch_counts == 0 then return nil end
        return string.format("%s over %d seeds: sand patches %s; sand tiles %s",
            floor.name, #patch_counts, describe(patch_counts), describe(tile_counts))
    end,
}
