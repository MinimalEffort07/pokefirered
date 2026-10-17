-------------------------------------------------------------------------------
-- mt_moon_chamber.lua: B2F's fossil chamber, as the tests see it
-------------------------------------------------------------------------------
--
-- Vanilla Mt. Moon B2F has a small room where Super Nerd Miguel guards the
-- Dome Fossil and the Helix Fossil. The generator stamps that room, tile for
-- tile, onto the generated B2F (src/mt_moon_gen.c, FOSSIL CHAMBER), so the
-- vanilla scripts keep working: after the player takes one fossil, Miguel's
-- script walks him to the other one with HARD-CODED steps (walk_right,
-- walk_up x3 to the Helix Fossil; walk_up x3 to the Dome Fossil). Those
-- steps only end in front of the right fossil if Miguel, both fossils and
-- the room's tiles keep their vanilla positions relative to each other.
--
-- Vanilla B2F (data/maps/MtMoon_B2F/map.json), map tile coordinates:
--
--   Dome Fossil (13,7)   Helix Fossil (14,7)   both in a 2-tile gap in the
--                                              room's north wall
--   Miguel (13,11), facing east, and his battle trigger right next to him
--   at (14,11), a coordinate event that starts the battle while
--   VAR_MAP_SCENE_MT_MOON_B2F is 0 (it becomes 1 once he is beaten)
--
-- The prefab is vanilla's tiles x 8..20, y 7..15 (VANILLA_RECT): the room
-- (x 9..19, y 8..11, with a little alcove at x 18..19 running down to y 14),
-- its walls, the stairs in its south wall at (13,12) and (14,12), and the
-- floor in front of the stairs (x 8..16, y 13..15). Miguel stands right in
-- front of the stairs and his trigger is the other stair's landing, so a
-- player walking in from the stairs always steps on the trigger first.
--
-- Everything here is read from the ROM (the vanilla object and coordinate
-- events) or written down from the vanilla map, never from the generator.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local mm = dofile(project_dir .. "test/lib/mt_moon.lua")

local chamber = {}

chamber.FLOOR = mm.floor_by_name("B2F")

local IDS = mm.read_defines("include/constants/map_event_ids.h")
chamber.LOCALID_DOME = IDS.LOCALID_DOME_FOSSIL
chamber.LOCALID_HELIX = IDS.LOCALID_HELIX_FOSSIL
chamber.LOCALID_MIGUEL = IDS.LOCALID_MIGUEL

-- The prefab's tiles in vanilla B2F (inclusive), as described above.
chamber.VANILLA_RECT = { x1 = 8, y1 = 7, x2 = 20, y2 = 15 }

local FLAGS = mm.read_defines("include/constants/flags.h")
chamber.FLAG_GOT_FOSSIL_FROM_MT_MOON = FLAGS.FLAG_GOT_FOSSIL_FROM_MT_MOON
chamber.FLAG_GOT_DOME_FOSSIL = FLAGS.FLAG_GOT_DOME_FOSSIL
chamber.FLAG_GOT_HELIX_FOSSIL = FLAGS.FLAG_GOT_HELIX_FOSSIL
chamber.FLAG_HIDE_DOME_FOSSIL = FLAGS.FLAG_HIDE_DOME_FOSSIL
chamber.FLAG_HIDE_HELIX_FOSSIL = FLAGS.FLAG_HIDE_HELIX_FOSSIL

local ITEMS = mm.read_defines("include/constants/items.h")
chamber.ITEM_DOME_FOSSIL = ITEMS.ITEM_DOME_FOSSIL
chamber.ITEM_HELIX_FOSSIL = ITEMS.ITEM_HELIX_FOSSIL

chamber.TRAINER_MIGUEL = mm.read_defines("include/constants/opponents.h").TRAINER_SUPER_NERD_MIGUEL
chamber.VAR_MAP_SCENE_MT_MOON_B2F = mm.read_defines("include/constants/vars.h").VAR_MAP_SCENE_MT_MOON_B2F

-- The chamber's pieces in an events listing (`templates` and
-- `coord_events`, from mm.snapshot() or mm.rom_events()):
--   { miguel = template, dome = template, helix = template, trigger = coord event }
-- Any of them is nil if missing. B2F has exactly one coordinate event,
-- Miguel's trigger.
function chamber.pieces(templates, coord_events)
    local p = { trigger = coord_events[1] }
    for _, t in ipairs(templates) do
        if t.local_id == chamber.LOCALID_MIGUEL then p.miguel = t end
        if t.local_id == chamber.LOCALID_DOME then p.dome = t end
        if t.local_id == chamber.LOCALID_HELIX then p.helix = t end
    end
    return p
end

-- The same pieces in vanilla B2F, read from the ROM.
function chamber.vanilla()
    local rom = mm.rom_events(chamber.FLOOR)
    return chamber.pieces(rom.objects, rom.coord_events)
end

-- How far the generated chamber is from vanilla's: (dx, dy) such that
-- vanilla tile (x, y) of the chamber is generated tile (x + dx, y + dy).
-- Measured on Miguel.
function chamber.offset(snap)
    local gen = chamber.pieces(snap.templates, snap.coord_events)
    local van = chamber.vanilla()
    return gen.miguel.x - van.miguel.x, gen.miguel.y - van.miguel.y
end

-- The live object (mm.live_objects) with local id `local_id`, or nil.
function chamber.live(local_id)
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player and o.local_id == local_id then return o end
    end
    return nil
end

return chamber
