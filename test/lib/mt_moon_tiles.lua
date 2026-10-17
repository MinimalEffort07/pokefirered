-------------------------------------------------------------------------------
-- mt_moon_tiles.lua: how a generated floor's tiles compare with vanilla's
-------------------------------------------------------------------------------
--
-- Shared measurements for the "looks right" (P5) and "density" (P6)
-- property checks of the Mt. Moon floor styles. Each style ticket adds its
-- own asserting check file for its floor (e.g. p5_1f.lua, p6_b1f.lua) on
-- top of these; mt_moon_checks/p5p6_report.lua only prints them.
--
-- TILE PAIRS. A map is a grid of metatiles (16x16-pixel pictures; see
-- mt_moon.lua). Two metatiles that sit side by side have to be drawn to fit
-- together, and vanilla Mt. Moon's artist only ever placed certain ones
-- next to each other. A "pair" here is two metatile ids that are
-- neighbours in the map, in a fixed orientation:
--
--   "h:AAA:BBB"  metatile AAA with metatile BBB directly EAST of it
--   "v:AAA:BBB"  metatile AAA with metatile BBB directly SOUTH of it
--
-- (A west or north neighbour is the same pair seen from the other tile,
-- so these two cover all four directions N/S/E/W.) A generated floor
-- "looks right" when every pair it uses also appears somewhere in that
-- floor's vanilla map, data/layouts/MtMoon_<floor>/map.bin. Only the
-- metatile id (the picture, bits 0-9 of a map cell) counts here, not its
-- collision or elevation bits.
--
-- WALKABLE FRACTION. Walkable tiles (collision 0) divided by all tiles of
-- the map. Vanilla: 1F 60%, B1F 16%, B2F 46% (design comment on #22).
--
-- USAGE (from a check file; `h` is mt_moon.lua as passed to check.run, the
-- `mm` parameter below):
--   local tiles = dofile(project_dir .. "test/lib/mt_moon_tiles.lua")
--   local vanilla = tiles.vanilla_pairs(floor)            -- cached per floor
--   local report = tiles.non_vanilla_pairs(h, snap.grid, vanilla)
--   report.count     -- neighbouring tile pairs in the map that vanilla lacks
--   report.total     -- all neighbouring tile pairs in the map
--   report.pairs     -- pair key -> how many times it occurs
--   tiles.walkable_fraction(h, snap.grid)   -- 0.0 .. 1.0
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local tiles = {}

local METATILE_MASK = 0x3ff

function tiles.pair_key(orientation, a, b)
    return string.format("%s:%03x:%03x", orientation, a, b)
end

-- Every pair of `cell_at(x, y)` (a function returning a metatile id) over a
-- w x h map, as key -> count.
local function count_pairs(w, h, cell_at)
    local pairs_seen = {}
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            local a = cell_at(x, y)
            if x + 1 < w then
                local k = tiles.pair_key("h", a, cell_at(x + 1, y))
                pairs_seen[k] = (pairs_seen[k] or 0) + 1
            end
            if y + 1 < h then
                local k = tiles.pair_key("v", a, cell_at(x, y + 1))
                pairs_seen[k] = (pairs_seen[k] or 0) + 1
            end
        end
    end
    return pairs_seen
end

-- The floor's vanilla layout: { w, h, cells } with cells[y * w + x + 1] a
-- full 16-bit map cell. Width, height and file come from
-- data/layouts/layouts.json (LAYOUT_MT_MOON_<floor name>).
function tiles.vanilla_layout(floor)
    local f = assert(io.open(project_dir .. "data/layouts/layouts.json", "r"))
    local json = f:read("a")
    f:close()
    local id = "LAYOUT_MT_MOON_" .. floor.name
    local block = json:match('"id": "' .. id .. '"(.-)}')
    assert(block, "no " .. id .. " in layouts.json")
    local w = tonumber(block:match('"width": (%d+)'))
    local h = tonumber(block:match('"height": (%d+)'))
    local path = block:match('"blockdata_filepath": "([^"]+)"')
    local bin = assert(io.open(project_dir .. path, "rb"))
    local data = bin:read("a")
    bin:close()
    assert(#data == w * h * 2, string.format("%s is %d bytes, expected %d", path, #data, w * h * 2))
    local cells = {}
    for i = 1, w * h do
        cells[i] = string.unpack("<I2", data, (i - 1) * 2 + 1)
    end
    return { w = w, h = h, cells = cells }
end

local vanilla_cache = {}

-- The set of pairs in the floor's vanilla map (key -> count). Cached.
function tiles.vanilla_pairs(floor)
    if vanilla_cache[floor.name] == nil then
        local layout = tiles.vanilla_layout(floor)
        vanilla_cache[floor.name] = count_pairs(layout.w, layout.h, function(x, y)
            return layout.cells[y * layout.w + x + 1] & METATILE_MASK
        end)
    end
    return vanilla_cache[floor.name]
end

-- The pairs of a generated map (mt_moon.read_grid() / snapshot.grid) that
-- the vanilla pair set lacks. `allowlist` (optional) is a set of pair keys
-- that don't count. Returns { count, total, pairs = key -> count }.
function tiles.non_vanilla_pairs(mm, grid, vanilla, allowlist)
    local all = count_pairs(grid.w, grid.h, function(x, y)
        return mm.metatile(grid.cells[mm.key(x, y)])
    end)
    local report = { count = 0, total = 0, pairs = {} }
    for k, n in pairs(all) do
        report.total = report.total + n
        if not vanilla[k] and not (allowlist and allowlist[k]) then
            report.count = report.count + n
            report.pairs[k] = n
        end
    end
    return report
end

-- Walkable tiles / all tiles of a generated map.
function tiles.walkable_fraction(mm, grid)
    local walkable = 0
    for y = 0, grid.h - 1 do
        for x = 0, grid.w - 1 do
            if mm.passable(grid.cells[mm.key(x, y)]) then walkable = walkable + 1 end
        end
    end
    return walkable / (grid.w * grid.h)
end

return tiles
