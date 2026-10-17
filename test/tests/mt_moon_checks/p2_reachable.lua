-------------------------------------------------------------------------------
-- P2 reachable: the player can get to everything on the floor
-------------------------------------------------------------------------------
--
-- Defeated trainers don't walk away: they keep standing where they are for
-- the rest of the game, as solid as a wall. So every object counts as solid
-- here, and a moving object (a wanderer) counts as solid on every tile of
-- its roaming box (mt_moon.roaming_box), since it could be standing on any
-- of them.
--
-- The player arrives through either ladder, depending on which way they are
-- going, so this is checked from BOTH of the floor's warps. From the arrival
-- warp, walking N/S/E/W over walkable tiles (the other warp can be stepped
-- onto but not walked through, because stepping on it leaves the floor):
--
--   1. the other warp can be reached;
--   2. for every object (trainer, NPC, item ball), a tile right next to it
--      (north, south, west or east of its box, so the player can face it)
--      can be reached and is not a warp;
--   3. for every hidden item and sign, the same: a reachable non-warp tile
--      next to it;
--   4. every coordinate event's tile (a trigger tile, such as the one next
--      to Super Nerd Miguel in B2F's fossil chamber) can itself be reached:
--      a trigger only fires when the player steps onto it;
--   5. every hidden item sits on a boulder, metatile 0x282 (as every
--      vanilla Mt. Moon hidden item does).
--
-- Miguel and the two fossils are objects like any other, so rule 2 covers
-- them too.
-------------------------------------------------------------------------------

-- Some tile in `tiles` is reachable (in dist) and not a warp.
local function any_reachable(h, dist, tiles, warp_tile)
    for _, p in ipairs(tiles) do
        local k = h.key(p[1], p[2])
        if dist[k] and not warp_tile[k] then return true end
    end
    return false
end

return {
    id = "P2",
    title = "reachable",
    run = function(h, floor, rng, snap)
        local warp_tile = h.warp_tile_set(h.ladder_warps(floor, snap))

        for _, bg in ipairs(snap.bg_events) do
            if bg.kind == h.BG_EVENT.BG_EVENT_HIDDEN_ITEM then
                local cell = snap.grid.cells[h.key(bg.x, bg.y)]
                if cell == nil or h.metatile(cell) ~= h.BOULDER then
                    return false, string.format("hidden item at (%d,%d) sits on 0x%03x, not the boulder 0x%03x",
                        bg.x, bg.y, cell and h.metatile(cell) or 0, h.BOULDER)
                end
            end
        end

        return h.check_each_arrival(floor, snap, function(label, dist, objects, from, to)
            if dist[h.key(to.x, to.y)] == nil then
                return false, string.format("%s: the other warp (%d,%d) can't be reached", label, to.x, to.y)
            end
            for _, t in ipairs(snap.templates) do
                if not any_reachable(h, dist, h.facing_tiles(h.roaming_box(t)), warp_tile) then
                    return false, string.format("%s: no reachable tile next to object %d at (%d,%d)",
                        label, t.local_id, t.x, t.y)
                end
            end
            for _, bg in ipairs(snap.bg_events) do
                if not any_reachable(h, dist, h.facing_tiles(bg.x, bg.y, bg.x, bg.y), warp_tile) then
                    return false, string.format("%s: no reachable tile next to the %s at (%d,%d)", label,
                        bg.kind == h.BG_EVENT.BG_EVENT_HIDDEN_ITEM and "hidden item" or "sign", bg.x, bg.y)
                end
            end
            for _, c in ipairs(snap.coord_events) do
                if not any_reachable(h, dist, { { c.x, c.y } }, warp_tile) then
                    return false, string.format("%s: the trigger tile (%d,%d) can't be reached", label, c.x, c.y)
                end
            end
            return true
        end)
    end,
}
