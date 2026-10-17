-------------------------------------------------------------------------------
-- P4 content complete: every piece of vanilla content is on its floor
-------------------------------------------------------------------------------
--
-- The generator moves each floor's vanilla content (from data/maps/<floor>/
-- map.json) onto the generated cave. Nothing may be dropped. The vanilla
-- content is read from the ROM's own copy of the floor's events
-- (mt_moon.rom_events), which the generator never changes.
--
-- PASS when:
--   1. Objects (trainers, NPCs, item balls): the floor has exactly the ROM's
--      objects, in the same order with the same local ids. Each one stands
--      on a walkable tile (never parked at (0,0)), no two share a tile, and
--      none stands on one of the floor's two warps.
--   2. Hidden items and signs (background events): exactly the ROM's, in the
--      same order with the same kind, elevation and contents (which item,
--      which flag), none at (0,0), no two on one tile, none on an object's
--      or a warp's tile. Each hidden item sits on the boulder 0x282 and the
--      sign on the signpost 0x2cd, both solid like their vanilla tiles.
--   3. Trigger tiles (coordinate events; B2F has one, Super Nerd Miguel's):
--      exactly the ROM's, in the same order, watching the same variable for
--      the same value and running the same script. Each one is on a
--      walkable tile (the player has to step onto it), never at (0,0), and
--      not under an object, a hidden item, a sign or a warp.
--
-- Super Nerd Miguel and the two fossils on B2F are checked like every
-- other object: they stand in the fossil chamber (see fc_fossil_chamber.lua).
-------------------------------------------------------------------------------

return {
    id = "P4",
    title = "content complete",
    run = function(h, floor, rng, snap)
        local rom = h.rom_events(floor)
        local grid = snap.grid
        local warp_at = h.warp_tile_set(h.ladder_warps(floor, snap))
        local HIDDEN_ITEM = h.BG_EVENT.BG_EVENT_HIDDEN_ITEM

        -- 1. Objects.
        if #snap.templates ~= #rom.objects then
            return false, string.format("%d objects, vanilla has %d", #snap.templates, #rom.objects)
        end
        local taken = {}
        for i, t in ipairs(snap.templates) do
            if t.local_id ~= rom.objects[i].local_id then
                return false, string.format("object %d has local id %d, vanilla %d", i, t.local_id, rom.objects[i].local_id)
            end
            local k = h.key(t.x, t.y)
            if taken[k] then
                return false, string.format("objects %d and %d share (%d,%d)", taken[k], t.local_id, t.x, t.y)
            end
            taken[k] = t.local_id
            if t.x == 0 and t.y == 0 then
                return false, string.format("object %d is parked at (0,0)", t.local_id)
            end
            if not h.passable(grid.cells[k]) then
                return false, string.format("object %d at (%d,%d) is not on walkable floor", t.local_id, t.x, t.y)
            end
            if warp_at[k] then
                return false, string.format("object %d at (%d,%d) stands on a warp", t.local_id, t.x, t.y)
            end
        end

        -- 2. Hidden items and signs.
        if #snap.bg_events ~= #rom.bg_events then
            return false, string.format("%d hidden items and signs, vanilla has %d", #snap.bg_events, #rom.bg_events)
        end
        local bg_at = {}
        for i, bg in ipairs(snap.bg_events) do
            local v = rom.bg_events[i]
            local what = bg.kind == HIDDEN_ITEM and "hidden item" or "sign"
            if bg.kind ~= v.kind or bg.elevation ~= v.elevation or bg.data ~= v.data then
                return false, string.format("%s %d differs from vanilla: kind %d/%d, elevation %d/%d, data 0x%08x/0x%08x",
                    what, i, bg.kind, v.kind, bg.elevation, v.elevation, bg.data, v.data)
            end
            local k = h.key(bg.x, bg.y)
            if bg.x == 0 and bg.y == 0 then
                return false, string.format("%s %d is parked at (0,0)", what, i)
            end
            if bg_at[k] or taken[k] or warp_at[k] then
                return false, string.format("%s %d at (%d,%d) shares its tile with another event, an object or a warp",
                    what, i, bg.x, bg.y)
            end
            bg_at[k] = true
            local cell = grid.cells[k]
            local want = bg.kind == HIDDEN_ITEM and h.BOULDER or h.SIGNPOST
            if cell == nil or h.metatile(cell) ~= want or h.passable(cell) then
                return false, string.format("%s %d at (%d,%d) is on 0x%04x, expected the solid 0x%03x",
                    what, i, bg.x, bg.y, cell or 0, want)
            end
        end

        -- 3. Trigger tiles.
        if #snap.coord_events ~= #rom.coord_events then
            return false, string.format("%d trigger tiles, vanilla has %d", #snap.coord_events, #rom.coord_events)
        end
        for i, c in ipairs(snap.coord_events) do
            local v = rom.coord_events[i]
            if c.var ~= v.var or c.value ~= v.value or c.script ~= v.script or c.elevation ~= v.elevation then
                return false, string.format("trigger %d differs from vanilla: var 0x%x/0x%x, value %d/%d, "
                    .. "script 0x%08x/0x%08x, elevation %d/%d", i, c.var, v.var, c.value, v.value,
                    c.script, v.script, c.elevation, v.elevation)
            end
            local k = h.key(c.x, c.y)
            if c.x == 0 and c.y == 0 then
                return false, string.format("trigger %d is parked at (0,0)", i)
            end
            if not h.passable(grid.cells[k]) then
                return false, string.format("trigger %d at (%d,%d) is on the solid tile 0x%04x", i, c.x, c.y,
                    grid.cells[k] or 0)
            end
            if taken[k] or bg_at[k] or warp_at[k] then
                return false, string.format("trigger %d at (%d,%d) shares its tile with an object, a hidden item, "
                    .. "a sign or a warp", i, c.x, c.y)
            end
        end
        return true
    end,
}
