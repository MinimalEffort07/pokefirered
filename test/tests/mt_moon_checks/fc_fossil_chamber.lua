-------------------------------------------------------------------------------
-- FC fossil chamber: B2F carries vanilla's fossil chamber, intact
-------------------------------------------------------------------------------
--
-- See test/lib/mt_moon_chamber.lua for the chamber and why its layout
-- matters: Miguel's walk to the second fossil is a fixed list of steps.
--
-- PASS on B2F when:
--   1. Relative positions: the Dome Fossil, the Helix Fossil and Miguel's
--      trigger tile are exactly where vanilla has them relative to Miguel
--      (read from the ROM's own B2F events).
--   2. Tile for tile: every map cell of the prefab (vanilla B2F x 8..20,
--      y 7..15, moved by the same offset as Miguel) equals the vanilla cell
--      in data/layouts/MtMoon_B2F/map.bin, metatile, collision and
--      elevation.
--   3. Miguel guards the fossils: arriving through either ladder, with
--      every object solid AND the trigger tile solid, the player can reach
--      neither tile in front of a fossil. Every way into the chamber crosses
--      the trigger tile, so the battle can't be skipped (the trigger fires
--      while Miguel is undefeated). P2 checks that the fossils CAN be
--      reached once the trigger is walkable.
--   4. Reachable, not forced: with every tile of the prefab solid as well,
--      the player can still walk from either ladder to the other. The
--      chamber is a side room, never the only way through the floor.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local chamber = dofile(script_dir .. "../../lib/mt_moon_chamber.lua")
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

local vanilla_layout = nil

local function relative_ok(label, gen, van, miguel_gen, miguel_van)
    local dx, dy = gen.x - miguel_gen.x, gen.y - miguel_gen.y
    local vx, vy = van.x - miguel_van.x, van.y - miguel_van.y
    if dx ~= vx or dy ~= vy then
        return false, string.format("%s at (%d,%d) is (%d,%d) from Miguel at (%d,%d); vanilla (%d,%d)",
            label, gen.x, gen.y, dx, dy, miguel_gen.x, miguel_gen.y, vx, vy)
    end
    return true
end

return {
    id = "FC",
    title = "fossil chamber",
    floors = { B2F = true },
    run = function(h, floor, rng, snap)
        local gen = chamber.pieces(snap.templates, snap.coord_events)
        local van = chamber.vanilla()
        for _, name in ipairs({ "miguel", "dome", "helix", "trigger" }) do
            if gen[name] == nil then return false, "no " .. name .. " on the generated floor" end
        end

        -- 1. Relative positions.
        for _, name in ipairs({ "dome", "helix", "trigger" }) do
            local ok, why = relative_ok(name, gen[name], van[name], gen.miguel, van.miguel)
            if not ok then return false, why end
        end

        -- 2. Tile for tile.
        vanilla_layout = vanilla_layout or tiles.vanilla_layout(floor)
        local ox, oy = chamber.offset(snap)
        local r = chamber.VANILLA_RECT
        for vy = r.y1, r.y2 do
            for vx = r.x1, r.x2 do
                local want = vanilla_layout.cells[vy * vanilla_layout.w + vx + 1]
                local got = snap.grid.cells[h.key(vx + ox, vy + oy)]
                if got ~= want then
                    return false, string.format("chamber tile (%d,%d) (vanilla (%d,%d)) is 0x%04x, vanilla 0x%04x",
                        vx + ox, vy + oy, vx, vy, got or 0, want)
                end
            end
        end

        -- 3. Miguel guards the fossils.
        local trigger_key = h.key(gen.trigger.x, gen.trigger.y)
        local ladders = h.ladder_warps(floor, snap)
        local objects = h.object_tiles(snap)
        for _, arrival in ipairs({ "entrance", "exit" }) do
            local from = ladders[arrival]
            local to = ladders[arrival == "entrance" and "exit" or "entrance"]
            local dist = h.bfs(snap.grid, from.x, from.y,
                function(x, y) local k = h.key(x, y) return objects[k] or k == trigger_key end,
                function(x, y) return x == to.x and y == to.y end)
            for _, fossil in ipairs({ gen.dome, gen.helix }) do
                for _, p in ipairs(h.facing_tiles(fossil.x, fossil.y, fossil.x, fossil.y)) do
                    if dist[h.key(p[1], p[2])] then
                        return false, string.format("arriving at the %s, (%d,%d) next to the fossil at (%d,%d) "
                            .. "can be reached without stepping on Miguel's trigger (%d,%d)", arrival,
                            p[1], p[2], fossil.x, fossil.y, gen.trigger.x, gen.trigger.y)
                    end
                end
            end
        end

        -- 4. Reachable, not forced.
        local in_prefab = function(x, y)
            return x >= r.x1 + ox and x <= r.x2 + ox and y >= r.y1 + oy and y <= r.y2 + oy
        end
        local dist = h.bfs(snap.grid, ladders.entrance.x, ladders.entrance.y,
            function(x, y) return objects[h.key(x, y)] or in_prefab(x, y) end)
        if dist[h.key(ladders.exit.x, ladders.exit.y)] == nil then
            return false, string.format("the exit (%d,%d) can only be reached through the chamber",
                ladders.exit.x, ladders.exit.y)
        end
        return true
    end,
}
