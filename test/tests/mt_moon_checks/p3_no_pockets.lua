-------------------------------------------------------------------------------
-- P3 no pockets: every walkable tile of the floor can be walked to
-------------------------------------------------------------------------------
--
-- A "pocket" is walkable floor the player can never get to: a sealed cave
-- left over by the shape, or floor cut off behind an object (objects are
-- solid, and defeated trainers never move away).
--
-- From each of the floor's two warps (the player arrives through either,
-- depending on direction), with every tile an object could stand on solid
-- (its tile, or a wanderer's whole roaming box; see mt_moon.object_tiles)
-- and the other warp reachable but not walked through:
--
-- PASS when every walkable tile that no object could stand on is reached.
-------------------------------------------------------------------------------

return {
    id = "P3",
    title = "no pockets",
    run = function(h, floor, rng, snap)
        local grid = snap.grid
        local ladders = h.ladder_warps(floor, snap)
        for _, arrival in ipairs({ "entrance", "exit" }) do
            local from = ladders[arrival]
            local to = ladders[arrival == "entrance" and "exit" or "entrance"]
            local dist, objects = h.reach_from_warp(snap, from, to)
            local missed, first = 0, nil
            for y = 0, grid.h - 1 do
                for x = 0, grid.w - 1 do
                    local k = h.key(x, y)
                    if h.passable(grid.cells[k]) and not objects[k] and dist[k] == nil then
                        missed = missed + 1
                        first = first or string.format("(%d,%d)", x, y)
                    end
                end
            end
            if missed > 0 then
                return false, string.format("arriving at the %s (%d,%d): %d walkable tiles can't be reached, first %s",
                    arrival, from.x, from.y, missed, first)
            end
        end
        return true
    end,
}
