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
--
-- Exempt until the fossil-chamber ticket (#27) builds the chamber around
-- them: Super Nerd Miguel and the two fossils on B2F. They are still
-- counted, and still may not share a tile with anything.
-------------------------------------------------------------------------------

-- Local ids of the exempt objects per floor, from the generated header
-- include/constants/map_event_ids.h (filled in on the first run).
local exempt_ids = nil

local function exempt_on(h, floor_name)
    if exempt_ids == nil then
        local ids = h.read_defines("include/constants/map_event_ids.h")
        exempt_ids = {
            B2F = {
                [ids.LOCALID_DOME_FOSSIL] = true,
                [ids.LOCALID_HELIX_FOSSIL] = true,
                [ids.LOCALID_MIGUEL] = true,
            },
        }
    end
    return exempt_ids[floor_name] or {}
end

return {
    id = "P4",
    title = "content complete",
    run = function(h, floor, rng, snap)
        local rom = h.rom_events(floor)
        local grid = snap.grid
        local warp_at = h.warp_tile_set(h.ladder_warps(floor, snap))
        local exempt = exempt_on(h, floor.name)
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
            if not exempt[t.local_id] then
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
        return true
    end,
}
