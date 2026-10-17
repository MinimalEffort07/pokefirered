-------------------------------------------------------------------------------
-- P1 ladders: every floor is one link of the chain, with vanilla-style ladders
-------------------------------------------------------------------------------
--
-- Mt. Moon is a chain of three generated floors:
--
--   Route 4 west door <-> 1F <-> B1F <-> B2F <-> Route 4 east door
--
-- so each floor has exactly two warps the player can use: the entrance
-- (toward Route 4 west) and the exit (toward Route 4 east). mt_moon.lua's
-- FLOORS table says, for each floor, which vanilla warp index each one
-- reuses, where it must lead and what kind of tile it must be.
--
-- PASS when, on the generated floor (read from RAM after the warp):
--   1. Exactly 2 warps are active (not parked at (0,0)), and they are the
--      entrance and exit warps from FLOORS. Every parked warp sits on (0,0),
--      which is solid, so it can never fire.
--   2. Each active warp leads where the chain says (destination map + warp).
--   3. Each warp tile holds its kind's tiles (see FLOORS in mt_moon.lua):
--        door  0x287, walkable, with the doorframe 0x2c8 0x2c9 0x2ca (solid)
--              below it and walkable floor above it. On 1F it is at (18,37),
--              the south-edge doorframe where vanilla has it.
--        down  0x285 or 0x2af, walkable, with all 8 neighbours walkable
--              (a hole in open floor).
--        up    0x296, walkable, with walkable floor south of it; the tile
--              north of it is the ladder top 0x28f or 0x28e (solid), set
--              into a north wall: the tiles west and east of the top and
--              the tile above it are solid too.
--   4. The two warps are at least 24 tiles apart (Manhattan distance:
--      steps east-west plus steps north-south).
--   5. The exit can be reached on foot from the entrance (BFS over walkable
--      tiles, not stepping on any other warp tile on the way).
-------------------------------------------------------------------------------

local MT_DOOR = 0x287
local MT_DOORFRAME = { 0x2c8, 0x2c9, 0x2ca }  -- west, middle, east
local MT_HOLES = { [0x285] = true, [0x2af] = true }
local MT_LADDER_FOOT = 0x296
local MT_LADDER_TOPS = { [0x28f] = true, [0x28e] = true }

-- 1F's door stays where vanilla has it.
local DOOR_1F_X, DOOR_1F_Y = 18, 37

local function cell(h, grid, x, y)
    if x < 0 or y < 0 or x >= grid.w or y >= grid.h then return nil end
    return grid.cells[h.key(x, y)]
end

local function walkable(h, grid, x, y)
    return h.passable(cell(h, grid, x, y))
end

local function solid(h, grid, x, y)
    local c = cell(h, grid, x, y)
    return c == nil or not h.passable(c)
end

local function metatile_at(h, grid, x, y)
    local c = cell(h, grid, x, y)
    return c and h.metatile(c) or nil
end

-- Returns nil when the tiles at warp `w` fit `kind`, else what's wrong.
local function check_tiles(h, floor, grid, w, kind)
    local x, y = w.x, w.y
    local mt = metatile_at(h, grid, x, y)
    local function fmt(what)
        return string.format("%s warp %d at (%d,%d): %s", kind, w.id, x, y, what)
    end
    if not walkable(h, grid, x, y) then
        return fmt(string.format("tile 0x%03x is not walkable", mt or 0))
    end
    if kind == "door" then
        if mt ~= MT_DOOR then return fmt(string.format("tile 0x%03x, expected 0x%03x", mt, MT_DOOR)) end
        for dx = -1, 1 do
            local frame = metatile_at(h, grid, x + dx, y + 1)
            if frame ~= MT_DOORFRAME[dx + 2] or not solid(h, grid, x + dx, y + 1) then
                return fmt(string.format("doorframe tile at (%d,%d) is 0x%03x, expected solid 0x%03x",
                    x + dx, y + 1, frame or 0, MT_DOORFRAME[dx + 2]))
            end
        end
        if not walkable(h, grid, x, y - 1) then return fmt("no walkable floor north of the door") end
        if floor.name == "1F" and (x ~= DOOR_1F_X or y ~= DOOR_1F_Y) then
            return fmt(string.format("expected 1F's door at (%d,%d)", DOOR_1F_X, DOOR_1F_Y))
        end
    elseif kind == "down" then
        if not MT_HOLES[mt] then return fmt(string.format("tile 0x%03x is not a hole (0x285/0x2af)", mt)) end
        for dy = -1, 1 do
            for dx = -1, 1 do
                if not walkable(h, grid, x + dx, y + dy) then
                    return fmt(string.format("not in open floor: (%d,%d) is solid", x + dx, y + dy))
                end
            end
        end
    elseif kind == "up" then
        if mt ~= MT_LADDER_FOOT then
            return fmt(string.format("tile 0x%03x, expected the ladder foot 0x%03x", mt, MT_LADDER_FOOT))
        end
        local top = metatile_at(h, grid, x, y - 1)
        if not MT_LADDER_TOPS[top] or not solid(h, grid, x, y - 1) then
            return fmt(string.format("tile above is 0x%03x, expected a solid ladder top (0x28f/0x28e)", top or 0))
        end
        for _, p in ipairs({ { x - 1, y - 1 }, { x + 1, y - 1 }, { x, y - 2 } }) do
            if not solid(h, grid, p[1], p[2]) then
                return fmt(string.format("not at the foot of a north wall: (%d,%d) is walkable", p[1], p[2]))
            end
        end
        if not walkable(h, grid, x, y + 1) then return fmt("no walkable floor south of the ladder") end
    else
        return fmt("unknown kind")
    end
    return nil
end

return {
    id = "P1",
    title = "ladders",
    run = function(h, floor, rng, snap)
        local grid = snap.grid
        local ends = { entrance = floor.entrance, exit = floor.exit }

        -- 1. Exactly the two expected warps are active.
        local active = {}
        for _, w in ipairs(snap.warps) do
            if w.x == 0 and w.y == 0 then
                if not solid(h, grid, 0, 0) then
                    return false, string.format("warp %d is parked on (0,0) but (0,0) is walkable", w.id)
                end
            else
                active[#active + 1] = w
            end
        end
        if #active ~= 2 then
            local list = {}
            for _, w in ipairs(active) do list[#list + 1] = string.format("%d@(%d,%d)", w.id, w.x, w.y) end
            return false, string.format("%d active warps, expected 2: %s", #active, table.concat(list, " "))
        end
        local warp_of = {}
        for name, e in pairs(ends) do
            local w = snap.warps[e.warp + 1]
            if w == nil or (w.x == 0 and w.y == 0) then
                return false, string.format("%s warp %d is not active", name, e.warp)
            end
            warp_of[name] = w
        end

        for name, e in pairs(ends) do
            local w = warp_of[name]
            -- 2. It leads where the chain says.
            if w.dest_group ~= e.to.group or w.dest_num ~= e.to.num or w.dest_warp ~= e.to.warp then
                return false, string.format("%s warp %d leads to map %d.%d warp %d, expected %d.%d warp %d",
                    name, e.warp, w.dest_group, w.dest_num, w.dest_warp, e.to.group, e.to.num, e.to.warp)
            end
            -- 3. Its tiles are the right kind.
            local why = check_tiles(h, floor, grid, w, e.kind)
            if why then return false, name .. " " .. why end
        end

        -- 4. Far enough apart.
        local a, b = warp_of.entrance, warp_of.exit
        local dist = math.abs(a.x - b.x) + math.abs(a.y - b.y)
        if dist < h.MIN_LADDER_DISTANCE then
            return false, string.format("entrance (%d,%d) and exit (%d,%d) are %d apart, need %d",
                a.x, a.y, b.x, b.y, dist, h.MIN_LADDER_DISTANCE)
        end

        -- 5. Connected on foot, without stepping on another warp.
        local warp_tile = {}
        for _, w in ipairs(snap.warps) do warp_tile[h.key(w.x, w.y)] = true end
        local reach = h.bfs(grid, a.x, a.y, function(x, y)
            return warp_tile[h.key(x, y)] and not (x == b.x and y == b.y)
        end)
        if reach[h.key(b.x, b.y)] == nil then
            return false, string.format("exit (%d,%d) can't be reached on foot from the entrance (%d,%d)",
                b.x, b.y, a.x, a.y)
        end
        return true
    end,
}
