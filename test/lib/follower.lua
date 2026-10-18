-------------------------------------------------------------------------------
-- follower.lua: test helpers for the walking Pokemon (src/pokemon_follower.c)
-------------------------------------------------------------------------------
--
-- The "follower" is the party Pokemon that walks behind the player after the
-- party menu's WALK option. Its sprite is an ObjectEvent (one of the 16
-- entries of gObjectEvents, the same kind of record every NPC has), and the
-- C code keeps its index in sFollower.objEventId.
--
-- This library gives tests:
--   * ensure_fixture(path): a new game with Pikachu in the party and WALK
--     chosen, saved as an mGBA save state at `path` (made once per run);
--   * read access to the follower's ObjectEvent (tile, held movement);
--   * place(x, y): put the follower on a map tile, as a scenario's setup;
--   * new_watch(): the per-frame "walkable tile" rule from issue #46 (see
--     below), plus a record of every tile change and teleport;
--   * map classification read from RAM and ROM: solid tiles, warps, water.
--
-- COORDINATES. Everything this library takes and returns is in map
-- coordinates, the ones in data/maps/<Map>/map.json. Object coordinates in
-- RAM are map coordinates + 7 (MAP_OFFSET: the engine keeps a 7-tile margin
-- around every map so the camera can scroll past its edge).
--
-- THE PER-FRAME RULE (issue #46). On every frame, the follower's tile must
-- be one a character could stand on. A watch fails (an assertion, so the
-- run fails) on the first frame the follower's tile:
--   * is solid: the map cell's collision bits (bits 10-11) are not 0. That
--     covers walls, trees and ledges (ledges are solid; only a jump crosses
--     them);
--   * holds a warp event (a door, a cave mouth, an arrow warp, a ladder);
--   * is water: its metatile behaviour is one the game lets you SURF on
--     (the game's own sBehaviorSurfable table, read from the ROM);
--   * is the current tile of another live object (an NPC, an item ball, a
--     roaming Pokemon, the player).
-- The one exception is the spawn fallback: when no tile next to the player
-- is free, the follower spawns on the player's own tile, and may stay there
-- until its first step.
--
-- During a ledge jump the follower's tile is the landing tile. The engine's
-- 2-tile jump (MOVEMENT_ACTION_JUMP_2_*) moves the ObjectEvent's tile one
-- step when the jump starts, onto the ledge, and the second step halfway
-- through the jump; the sprite is in the air the whole time. So while a
-- JUMP_2 held movement is running and the follower's tile is still the
-- (solid) ledge, the landing tile one further on is the one checked.
--
-- A TELEPORT is a frame on which the follower's tile changes by more than
-- one tile (walking and the two halves of a jump move it one tile at a
-- time).
--
-- Everything that waits for frames must be called from inside fw.run().
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local sym  = dofile(project_dir .. "test/lib/symbols.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local mm   = dofile(project_dir .. "test/lib/mt_moon.lua")

local follower = {}

-- ASSERTIONS. dofile() gives every file that loads framework.lua its own
-- copy, with its own pass/fail counts, and only the test's copy writes the
-- results test/run_test.sh reads. So this library asserts through T, the
-- calling test's framework, which the test hands over with
-- follower.bind(fw) right after loading this file.
local T = fw
function follower.bind(test_fw)
    T = test_fw
end

local MAP_OFFSET = 7

-------------------------------------------------------------------------------
-- Layout facts (see include/global.fieldmap.h)
-------------------------------------------------------------------------------

-- struct ObjectEvent, 36 bytes:
--   0x00 u32 bit flags: bit 0 active, bit 6 heldMovementActive,
--        bit 7 heldMovementFinished, bit 8 frozen, bit 16 isPlayer
--   0x08 u8 localId
--   0x10 s16 currentCoords.x, 0x12 currentCoords.y
--   0x14 s16 previousCoords.x, 0x16 previousCoords.y
--   0x18 u8 facingDirection (low 4 bits), movementDirection (high 4 bits)
--   0x1C u8 movementActionId (the held movement being played)
local OE_SIZE       = 0x24
local OE_FLAGS      = 0x00
local OE_LOCAL_ID   = 0x08
local OE_CURRENT    = 0x10
local OE_PREVIOUS   = 0x14
local OE_FACING     = 0x18
local OE_ACTION     = 0x1C

local FLAG_ACTIVE          = 0x001
local FLAG_HELD_ACTIVE     = 0x040
local FLAG_HELD_FINISHED   = 0x080
local FLAG_FROZEN          = 0x100
local FLAG_IS_PLAYER       = 0x10000

-- sFollower (src/pokemon_follower.c), see test/lib/addresses.lua for the
-- whole layout: u8 objEventId at offset 7.
local FOLLOWER_OBJ_EVENT_ID_OFF = 7
local OBJECT_EVENTS_COUNT = 16

-- Directions as the game numbers them (include/constants/global.h).
follower.DIR_SOUTH, follower.DIR_NORTH, follower.DIR_WEST, follower.DIR_EAST = 1, 2, 3, 4
local DIR_DELTA = {
    [1] = { 0, 1 }, [2] = { 0, -1 }, [3] = { -1, 0 }, [4] = { 1, 0 },
}

-- MOVEMENT_ACTION_JUMP_2_DOWN/UP/LEFT/RIGHT (include/constants/
-- event_object_movement.h) and the direction each one jumps.
local JUMP_2_DIR = { [0x14] = 1, [0x15] = 2, [0x16] = 3, [0x17] = 4 }

-- Roaming Pokemon (src/roaming_pokemon.c) use local ids 200-205 (the player
-- is 255 and the follower 254, so "200 and up" alone would match them too).
local function is_roamer(o)
    return not o.player and o.local_id >= 200 and o.local_id <= 205
end

-------------------------------------------------------------------------------
-- Small helpers
-------------------------------------------------------------------------------

local function s16(v)
    if v >= 0x8000 then return v - 0x10000 end
    return v
end

local function object_addr(i)
    return sym.addr("gObjectEvents") + i * OE_SIZE
end

local function key(x, y)
    return y * 1000 + x
end
follower.key = key

-------------------------------------------------------------------------------
-- The follower's ObjectEvent
-------------------------------------------------------------------------------

-- Index of the follower's ObjectEvent, or nil when no follower sprite exists.
function follower.object_index()
    local i = emu:read8(sym.addr("sFollower") + FOLLOWER_OBJ_EVENT_ID_OFF)
    if i >= OBJECT_EVENTS_COUNT then return nil end
    if (emu:read32(object_addr(i) + OE_FLAGS) & FLAG_ACTIVE) == 0 then return nil end
    return i
end

-- The follower's state this frame, or nil:
--   { index, x, y, held (a held movement is playing), action, jump_dir }
-- jump_dir is the direction of a 2-tile ledge jump in progress, else nil.
function follower.read()
    local i = follower.object_index()
    if i == nil then return nil end
    local a = object_addr(i)
    local flags = emu:read32(a + OE_FLAGS)
    local held = (flags & FLAG_HELD_ACTIVE) ~= 0 and (flags & FLAG_HELD_FINISHED) == 0
    local action = emu:read8(a + OE_ACTION)
    return {
        index = i,
        x = s16(emu:read16(a + OE_CURRENT)) - MAP_OFFSET,
        y = s16(emu:read16(a + OE_CURRENT + 2)) - MAP_OFFSET,
        held = held,
        action = action,
        jump_dir = held and JUMP_2_DIR[action] or nil,
    }
end

-- The player's tile and facing direction (DIR_* above).
function follower.player()
    for i = 0, OBJECT_EVENTS_COUNT - 1 do
        local a = object_addr(i)
        local flags = emu:read32(a + OE_FLAGS)
        if (flags & FLAG_ACTIVE) ~= 0 and (flags & FLAG_IS_PLAYER) ~= 0 then
            return s16(emu:read16(a + OE_CURRENT)) - MAP_OFFSET,
                   s16(emu:read16(a + OE_CURRENT + 2)) - MAP_OFFSET,
                   emu:read8(a + OE_FACING) & 0xF
        end
    end
    return nil
end

-- Every live object except the follower: { index, x, y, local_id, player }.
function follower.other_objects()
    local mine = follower.object_index()
    local list = {}
    for i = 0, OBJECT_EVENTS_COUNT - 1 do
        local a = object_addr(i)
        local flags = emu:read32(a + OE_FLAGS)
        if i ~= mine and (flags & FLAG_ACTIVE) ~= 0 then
            list[#list + 1] = {
                index = i,
                x = s16(emu:read16(a + OE_CURRENT)) - MAP_OFFSET,
                y = s16(emu:read16(a + OE_CURRENT + 2)) - MAP_OFFSET,
                local_id = emu:read8(a + OE_LOCAL_ID),
                player = (flags & FLAG_IS_PLAYER) ~= 0,
            }
        end
    end
    return list
end

-- Wait until neither the player nor the follower is in the middle of a
-- step, so a scenario starts from a still picture.
function follower.wait_still(timeout)
    return fw.wait_until(function()
        local f = follower.read()
        return f ~= nil and not f.held
            and emu:read8(sym.addr("gPlayerAvatar") + 3) == 0  -- tileTransitionState: not mid-step
    end, timeout or 300, "player and follower standing still")
end

-- Put the follower on map tile (x, y): writes its current and previous
-- coordinates (both, so the engine sees a follower standing still there).
-- Only the record moves; the sprite on screen jumps there on its next step.
-- This is a scenario's setup, done between two frames.
function follower.place(x, y)
    local f = follower.read()
    assert(f ~= nil, "no follower to place")
    local a = object_addr(f.index)
    for _, field in ipairs({ OE_CURRENT, OE_PREVIOUS }) do
        emu:write16(a + field, (x + MAP_OFFSET) & 0xFFFF)
        emu:write16(a + field + 2, (y + MAP_OFFSET) & 0xFFFF)
    end
end

-- Set the game's random number state (gRngValue, the seed Random() steps
-- on every call). The follower's idle pauses and side-steps are random, so
-- a scenario seeds it at its start to play out the same way on every run.
function follower.set_rng(value)
    emu:write32(sym.addr("gRngValue"), value & 0xFFFFFFFF)
end

-------------------------------------------------------------------------------
-- Roaming Pokemon out of the way
-------------------------------------------------------------------------------

-- On maps with wild grass (Route 1, Route 2) roaming Pokemon wander around
-- and could stand on a scenario's tiles. Stop new ones from spawning (the
-- spawn timer gRoamerNextSpawnTimer counts down once per frame; 0xFFFF is
-- about 18 minutes) and move the existing ones far off: the game removes a
-- roamer, sprite and all, when it is more than 14 tiles from the player
-- (ROAMER_DESPAWN_DISTANCE). Returns true once no roamer is left.
function follower.clear_roamers()
    emu:write16(sym.addr("gRoamerNextSpawnTimer"), 0xFFFF)
    local px, py = follower.player()
    for _, o in ipairs(follower.other_objects()) do
        if is_roamer(o) then
            -- 40 tiles from the player along x: well past the despawn
            -- distance whichever side of the map it lands on.
            mm.poke_object(o.index, px + 40, py)
        end
    end
    fw.wait_frames(2)
    for _, o in ipairs(follower.other_objects()) do
        if is_roamer(o) then return false end
    end
    return true
end

-------------------------------------------------------------------------------
-- What a map tile is
-------------------------------------------------------------------------------

-- A snapshot of the loaded map for classifying tiles: the grid (collision
-- bits per cell), the warp tiles, and a metatile-behaviour lookup through
-- the map layout's two tilesets.
--
-- Metatile behaviours: a map cell's bits 0-9 are a metatile id. Ids below
-- 640 belong to the layout's primary tileset, the rest to its secondary
-- tileset; each tileset has a table of u32 attributes per metatile, whose
-- bits 0-8 are the behaviour (MB_* in include/constants/metatile_behaviors.h).
-- struct MapLayout: 0x10 primaryTileset, 0x14 secondaryTileset;
-- struct Tileset: 0x14 metatileAttributes (include/global.fieldmap.h).
function follower.map_snapshot()
    local grid = mm.read_grid()
    local warps = {}
    for _, w in ipairs(mm.read_warps()) do warps[key(w.x, w.y)] = true end
    local layout = emu:read32(sym.addr("gMapHeader"))
    local primary = emu:read32(emu:read32(layout + 0x10) + 0x14)
    local secondary = emu:read32(emu:read32(layout + 0x14) + 0x14)
    local surfable = sym.addr("sBehaviorSurfable")  -- bool8[], indexed by MB_*
    local snap = { grid = grid, warps = warps }

    function snap.cell(x, y)
        return grid.cells[key(x, y)]
    end
    function snap.solid(x, y)
        local c = snap.cell(x, y)
        return c == nil or mm.collision(c) ~= 0   -- off the map counts as solid
    end
    function snap.behavior(x, y)
        local c = snap.cell(x, y)
        if c == nil then return nil end
        local id = c & 0x3FF
        if id < 640 then return emu:read32(primary + id * 4) & 0x1FF end
        return emu:read32(secondary + (id - 640) * 4) & 0x1FF
    end
    function snap.water(x, y)
        local b = snap.behavior(x, y)
        return b ~= nil and (emu:read8(surfable + b) & 1) ~= 0
    end
    function snap.warp(x, y)
        return warps[key(x, y)] == true
    end
    return snap
end

-------------------------------------------------------------------------------
-- The per-frame watch
-------------------------------------------------------------------------------

-- Start watching the follower. Call watch.frame() once after every frame.
-- `name` prefixes the assertion messages. The tile the follower stands on
-- when the watch starts is the starting tile (frame 0).
--
-- The watch records:
--   watch.tiles      the follower's tile on every frame (1-based; [0] start)
--   watch.changes    list of { frame, from_x, from_y, x, y, teleport }
--   watch.teleports  the subset of changes that were teleports
--   watch.failed     true once the per-frame rule has failed (reported once)
function follower.new_watch(name, snap)
    snap = snap or follower.map_snapshot()
    local f = follower.read()
    assert(f ~= nil, "no follower to watch")
    local watch = {
        name = name, snap = snap, frame_no = 0,
        tiles = {}, changes = {}, teleports = {},
        failed = false, stepped = false,
    }
    watch.tiles[0] = { x = f.x, y = f.y }

    -- The follower's tile for the rule: its ObjectEvent's tile, or the
    -- landing tile during the first half of a 2-tile ledge jump.
    function watch.tile()
        local cur = follower.read()
        if cur == nil then return nil end
        local x, y = cur.x, cur.y
        if cur.jump_dir and snap.solid(x, y) then
            local d = DIR_DELTA[cur.jump_dir]
            x, y = x + d[1], y + d[2]
        end
        return x, y, cur
    end

    -- Why (x, y) breaks the rule, or nil.
    function watch.violation(x, y)
        if snap.solid(x, y) then
            return string.format("solid tile (cell 0x%04x)", snap.cell(x, y) or 0)
        end
        if snap.warp(x, y) then return "warp tile" end
        if snap.water(x, y) then
            return string.format("water (behaviour 0x%02x)", snap.behavior(x, y))
        end
        for _, o in ipairs(follower.other_objects()) do
            if o.x == x and o.y == y then
                -- Spawn fallback: on the player's tile until the first step.
                if not (o.player and not watch.stepped) then
                    return string.format("shared with object %d (local id %d%s)",
                        o.index, o.local_id, o.player and ", the player" or "")
                end
            end
        end
        return nil
    end

    function watch.frame()
        watch.frame_no = watch.frame_no + 1
        local n = watch.frame_no
        local x, y = watch.tile()
        if x == nil then
            if not watch.failed then
                watch.failed = true
                T.assert_true(false, string.format("%s: follower sprite exists on frame %d", name, n))
            end
            return
        end
        local prev = watch.tiles[n - 1]
        watch.tiles[n] = { x = x, y = y }
        if x ~= prev.x or y ~= prev.y then
            local far = math.abs(x - prev.x) + math.abs(y - prev.y) > 1
            local change = { frame = n, from_x = prev.x, from_y = prev.y, x = x, y = y, teleport = far }
            watch.changes[#watch.changes + 1] = change
            if far then watch.teleports[#watch.teleports + 1] = change end
            watch.stepped = true
            fw.log(string.format("  %s frame %d: follower (%d,%d) -> (%d,%d)%s",
                name, n, prev.x, prev.y, x, y, far and " TELEPORT" or ""))
        end
        local why = watch.violation(x, y)
        if why and not watch.failed then
            watch.failed = true
            T.assert_true(false, string.format(
                "%s: follower on a walkable tile every frame (frame %d: (%d,%d) is %s)",
                name, n, x, y, why))
        end
    end

    -- Run `frames` frames, checking each one. `each(n, x, y)` (optional) is
    -- called after every frame's check with the frame number and tile.
    function watch.run(frames, each)
        for _ = 1, frames do
            fw.yield()
            watch.frame()
            if each then
                local t = watch.tiles[watch.frame_no]
                each(watch.frame_no, t and t.x, t and t.y)
            end
        end
    end

    -- Report the per-frame rule as one passing assertion if it never failed
    -- (a failure was already reported on its frame).
    function watch.finish()
        if not watch.failed then
            T.assert_true(true, string.format(
                "%s: follower on a walkable tile every frame (%d frames)", name, watch.frame_no))
        end
    end

    -- True if the follower ever stood on (x, y) during the watch.
    function watch.visited(x, y)
        for i = 0, watch.frame_no do
            local t = watch.tiles[i]
            if t and t.x == x and t.y == y then return true end
        end
        return false
    end

    return watch
end

-------------------------------------------------------------------------------
-- Fixture: a new game with Pikachu walking
-------------------------------------------------------------------------------

local POKEMON_SIZE = 100       -- sizeof(struct Pokemon)
local SPECIES_PIKACHU = 25

-- Write a minimal valid Pikachu into party slot memory at `base`.
-- Personality 0 and trainer id 0 make the encryption key 0, so the data is
-- stored in the clear with its blocks in the order Growth, Attacks, EVs,
-- Misc. The checksum is the sum of the 24 u16s of those blocks, which is
-- just the species when nothing else is set.
local function inject_pikachu(base, level)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)                    -- flags: hasSpecies
    emu:write16(base + 0x20, SPECIES_PIKACHU)        -- Growth block: species
    emu:write16(base + 0x1C, SPECIES_PIKACHU)        -- checksum
    emu:write8(base + 0x54, level)                   -- level
    emu:write8(base + 0x55, 0xFF)                    -- no mail
    emu:write16(base + 0x56, 20)                     -- current HP
    emu:write16(base + 0x58, 20)                     -- max HP
end

-- Load the fixture at `path`, or make it: boot a new game, put Pikachu in
-- party slot 0, choose POKEMON > Pikachu > WALK, and save the state.
-- Returns true when a follower is walking afterwards.
function follower.ensure_fixture(path)
    if not fw.try_load_state(path) then
        local sel = cs.find_selection_press()
        if not sel then
            T.assert_true(false, "fixture: character select discovery")
            return false
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()

        inject_pikachu(sym.addr("gPlayerParty"), 5)
        emu:write8(sym.addr("gPlayerPartyCount"), 1)
        -- FLAG_SYS_POKEMON_GET (SYS_FLAGS + 0x28 = 0x828, see
        -- include/constants/flags.h) puts POKEMON first in the START menu.
        mm.set_flag(0x828)
        fw.wait_frames(30)

        fw.press("START")
        fw.wait_frames(50)
        fw.press("A")          -- POKEMON
        fw.wait_frames(80)
        fw.press("A")          -- Pikachu (slot 0) -> action menu
        fw.wait_frames(50)
        fw.press("DOWN")       -- SUMMARY -> WALK
        fw.wait_frames(10)
        fw.press("A")          -- WALK: ActivateFollower, back to the field
        fw.wait_frames(200)

        if follower.object_index() == nil then
            T.assert_true(false, "fixture: follower spawned after WALK")
            return false
        end
        fw.save_state(path)
    end
    fw.wait_frames(30)
    return follower.object_index() ~= nil
end

-- Make the field calm for a scenario: Pikachu (the follower, party slot 0)
-- becomes level 100 and a long Repel starts, so no wild battle can start
-- (a Repel keeps away every wild Pokemon below the lead's level), and the
-- text speed is FAST.
function follower.make_calm()
    emu:write8(sym.addr("gPlayerParty") + 0x54, 100)
    mm.set_var(mm.read_defines("include/constants/vars.h").VAR_REPEL_STEP_COUNT, 5000)
    mm.set_text_speed_fast()
end

-- Warp the player to map tile (x, y) of map (group, num); the follower
-- respawns next to them. Returns true on success.
function follower.warp(group, num, x, y)
    local ok = warp.warp_to_pos(group, num, x, y)
    if not ok then return false end
    return fw.wait_until(function()
        local px, py = follower.player()
        return mm.player_has_control() and px == x and py == y and follower.object_index() ~= nil
    end, 300, string.format("player at (%d,%d) with the follower out", x, y))
end

-------------------------------------------------------------------------------
-- Surf
-------------------------------------------------------------------------------

-- gPlayerAvatar.flags bit set while the player rides a Pokemon on water
-- (PLAYER_AVATAR_FLAG_SURFING = 1 << 3, include/constants/global.h).
local PLAYER_AVATAR_FLAG_SURFING = 0x08

function follower.is_surfing()
    return (emu:read8(sym.addr("gPlayerAvatar")) & PLAYER_AVATAR_FLAG_SURFING) ~= 0
end

-- Let the player SURF without a party Pokemon that knows it (the slaveless
-- HM feature, issue #5): HM03 in the bag and the SOUL BADGE
-- (FLAG_BADGE05_GET = 0x824). The bag's TM/HM pocket is
-- SaveBlock1.bagPocket_TMHM at 0x048c, 4 bytes per slot: u16 item id, u16
-- quantity XOR-ed with SaveBlock2.encryptionKey (at 0xF20), so storing
-- (1 XOR key) reads back as 1.
function follower.give_surf()
    local ITEM_HM03 = 341
    local sb1 = mm.sb1()
    local key = emu:read32(emu:read32(sym.addr("gSaveBlock2Ptr")) + 0xF20)
    emu:write16(sb1 + 0x048c, ITEM_HM03)
    emu:write16(sb1 + 0x048c + 2, (1 ~ key) & 0xFFFF)
    mm.set_flag(0x824)
end

-- Facing water, press A through "Would you like to SURF?" (the cursor
-- starts on YES) and the "...used SURF!" message until the player is
-- surfing on the water tile (wx, wy). Returns true on success.
function follower.use_surf(wx, wy)
    for _ = 1, 60 do
        local px, py = follower.player()
        if follower.is_surfing() and px == wx and py == wy then return true end
        if not follower.is_surfing() then fw.press("A") end
        fw.wait_frames(20)
    end
    local px, py = follower.player()
    return follower.is_surfing() and px == wx and py == wy
end

-- Turn the player to face `key` ("UP", "DOWN", "LEFT", "RIGHT") without
-- moving: only for spots where the tile that way is solid, so holding the
-- direction just bumps.
function follower.face(key_name)
    fw.hold(key_name, 6)
    fw.wait_frames(30)
end

return follower
