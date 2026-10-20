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
--     below), plus a record of every tile change, teleport and swap;
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
-- Two exceptions:
--   * The spawn fallback: when no tile next to the player is free, the
--     follower spawns on the player's own tile, and may stay there until its
--     first step, even when that tile is a warp (the player arrived on a
--     cave mouth, say).
--   * A swap (issue #99, see below) may put the follower on a warp: the
--     tile the player stepped off, such as a doormat. It may stay there
--     until its next step.
--
-- A SWAP is a frame on which the player and the follower trade tiles: the
-- player's tile becomes the follower's tile of the frame before, and the
-- follower's tile becomes the player's. The game makes one when the player
-- walks, runs or rides into the standing follower (src/pokemon_follower.c,
-- CanPlayerSwapWithFollower). Each swap is recorded with the frame each of
-- the two steps ends on: the first frame its previous tile has caught up
-- with its current one, as at the end of every step.
--
-- During a ledge jump the follower's tile is the landing tile. The engine's
-- 2-tile jump (MOVEMENT_ACTION_JUMP_2_*) moves the ObjectEvent's tile one
-- step when the jump starts, onto the ledge, and the second step halfway
-- through the jump; the sprite is in the air the whole time. So while a
-- JUMP_2 held movement is running and the follower's tile is still the
-- (solid) ledge, the landing tile one further on is the one checked.
--
-- A TELEPORT is a frame on which the follower's tile changes by more than
-- one tile, other than the start of a ledge jump (whose tile, the landing
-- tile, is two tiles on in the jump's direction). Walking moves it one tile
-- at a time.
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

-- ASSERTIONS. This library asserts through T, the calling test's
-- framework, which the test hands over with follower.bind(fw) right after
-- loading this file. Every copy of framework.lua reports into one shared
-- results table (see test/lib/framework.lua), so assertions made through T
-- and through this file's own copy are counted alike.
local T = fw
function follower.bind(test_fw)
    T = test_fw
end

local MAP_OFFSET = mm.MAP_OFFSET

-------------------------------------------------------------------------------
-- Layout facts (see include/global.fieldmap.h)
-------------------------------------------------------------------------------

-- The struct ObjectEvent fields read here that mt_moon.lua's object readers
-- (mm.live_objects) don't report. 36 bytes per object:
--   0x00 u32 bit flags: bit 0 active, bit 6 heldMovementActive,
--        bit 7 heldMovementFinished, bit 16 isPlayer
--   0x04 u8 spriteId (its sprite's index in gSprites)
--   0x10 s16 currentCoords.x, 0x12 currentCoords.y
--   0x14 s16 previousCoords.x, 0x16 previousCoords.y (the tile a step
--        started from; equal to currentCoords once the step has ended)
--   0x1C u8 movementActionId (the held movement being played)
local OE_SIZE       = 0x24
local OE_FLAGS      = 0x00
local OE_SPRITE_ID  = 0x04
local OE_CURRENT    = 0x10
local OE_PREVIOUS   = 0x14
local OE_ACTION     = 0x1C

local FLAG_ACTIVE          = 0x001
local FLAG_HELD_ACTIVE     = 0x040
local FLAG_HELD_FINISHED   = 0x080
local FLAG_IS_PLAYER       = 0x10000

-- struct Sprite (include/sprite.h), 0x44 bytes each in gSprites: s16 x at
-- 0x20 and s16 y at 0x22, the sprite's position in pixels.
local SPRITE_SIZE = 0x44
local SPRITE_X    = 0x20
local SPRITE_Y    = 0x22

-- sFollower (src/pokemon_follower.c), see test/lib/addresses.lua for the
-- whole layout: bool8 active at offset 0, u8 objEventId at offset 7.
local FOLLOWER_ACTIVE_OFF = 0
local FOLLOWER_OBJ_EVENT_ID_OFF = 7
local OBJECT_EVENTS_COUNT = 16

-- gPlayerAvatar.flags (u8 at offset 0 of struct PlayerAvatar,
-- include/global.fieldmap.h), one bit per avatar state: bit 1 riding the
-- bicycle (PLAYER_AVATAR_FLAG_MACH_BIKE, FireRed's only bicycle), bit 7
-- running (PLAYER_AVATAR_FLAG_DASH, set while a step with B held plays).
local PLAYER_AVATAR_FLAG_MACH_BIKE = 0x02
local PLAYER_AVATAR_FLAG_DASH      = 0x80

-- Directions as the game numbers them (include/constants/global.h).
follower.DIR_SOUTH, follower.DIR_NORTH, follower.DIR_WEST, follower.DIR_EAST = 1, 2, 3, 4
local DIR_DELTA = {
    [1] = { 0, 1 }, [2] = { 0, -1 }, [3] = { -1, 0 }, [4] = { 1, 0 },
}

-- MOVEMENT_ACTION_JUMP_2_DOWN/UP/LEFT/RIGHT (include/constants/
-- event_object_movement.h) and the direction each one jumps.
local JUMP_2_DIR = { [0x14] = 1, [0x15] = 2, [0x16] = 3, [0x17] = 4 }

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

-- The same tile key as mt_moon.lua's (mm.key), for sets of tiles.
local key = mm.key
follower.key = key

-- Tiles between (ax, ay) and (bx, by), counting east-west plus north-south
-- steps (the "Manhattan distance" the follower code uses).
function follower.distance(ax, ay, bx, by)
    return math.abs(ax - bx) + math.abs(ay - by)
end

-------------------------------------------------------------------------------
-- The follower's ObjectEvent
-------------------------------------------------------------------------------

-- Index of the follower's ObjectEvent, or nil when no follower sprite exists.
-- Before the first WALK, sFollower is all zeros: objEventId 0 is then the
-- player's object, so a follower also needs sFollower.active and an object
-- that isn't the player's.
function follower.object_index()
    if emu:read8(sym.addr("sFollower") + FOLLOWER_ACTIVE_OFF) == 0 then return nil end
    local i = emu:read8(sym.addr("sFollower") + FOLLOWER_OBJ_EVENT_ID_OFF)
    if i >= OBJECT_EVENTS_COUNT then return nil end
    local flags = emu:read32(object_addr(i) + OE_FLAGS)
    if (flags & FLAG_ACTIVE) == 0 or (flags & FLAG_IS_PLAYER) ~= 0 then return nil end
    return i
end

-- The follower's state this frame, or nil:
--   { index, x, y, prev_x, prev_y, held (a held movement is playing),
--     action, jump_dir }
-- (prev_x, prev_y) is the tile its step started from: (x, y) itself when it
-- stands still. jump_dir is the direction of a 2-tile ledge jump in
-- progress, else nil.
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
        prev_x = s16(emu:read16(a + OE_PREVIOUS)) - MAP_OFFSET,
        prev_y = s16(emu:read16(a + OE_PREVIOUS + 2)) - MAP_OFFSET,
        held = held,
        action = action,
        jump_dir = held and JUMP_2_DIR[action] or nil,
    }
end

-- The player's object as mm.live_objects lists it ({ x, y, prev_x, prev_y,
-- facing, ... }), or nil.
function follower.player_object()
    for _, o in ipairs(mm.live_objects()) do
        if o.is_player then return o end
    end
    return nil
end

-- The player's tile and facing direction (DIR_* above).
function follower.player()
    local o = follower.player_object()
    if o == nil then return nil end
    return o.x, o.y, o.facing
end

-- The held movement the player's object is playing this frame (its
-- movementActionId, MOVEMENT_ACTION_* in include/constants/
-- event_object_movement.h), or nil when none is.
function follower.player_action()
    local o = follower.player_object()
    if o == nil then return nil end
    local a = object_addr(o.index)
    local flags = emu:read32(a + OE_FLAGS)
    if (flags & FLAG_HELD_ACTIVE) == 0 or (flags & FLAG_HELD_FINISHED) ~= 0 then return nil end
    return emu:read8(a + OE_ACTION)
end

-- Every live object except the follower, as mm.live_objects lists them
-- ({ index, x, y, local_id, is_player, ... }).
function follower.other_objects()
    local mine = follower.object_index()
    local list = {}
    for _, o in ipairs(mm.live_objects()) do
        if o.index ~= mine then list[#list + 1] = o end
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
-- coordinates (both, so the engine sees a follower standing still there),
-- and moves its sprite by as many pixels (a tile is 16), so the picture
-- shows it there too. The engine never moves a sprite back onto its tile by
-- itself: each step only adds that step's pixels to where the sprite is.
-- This is a scenario's setup, done between two frames while the follower
-- stands still.
function follower.place(x, y)
    local f = follower.read()
    assert(f ~= nil, "no follower to place")
    mm.poke_object(f.index, x, y)
    local sprite = sym.addr("gSprites") + emu:read8(object_addr(f.index) + OE_SPRITE_ID) * SPRITE_SIZE
    emu:write16(sprite + SPRITE_X, (emu:read16(sprite + SPRITE_X) + (x - f.x) * 16) & 0xFFFF)
    emu:write16(sprite + SPRITE_Y, (emu:read16(sprite + SPRITE_Y) + (y - f.y) * 16) & 0xFFFF)
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
        if mm.is_roamer(o) then
            -- 40 tiles from the player along x: well past the despawn
            -- distance whichever side of the map it lands on.
            mm.poke_object(o.index, px + 40, py)
        end
    end
    fw.wait_frames(2)
    for _, o in ipairs(follower.other_objects()) do
        if mm.is_roamer(o) then return false end
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

-- The watch attached to the frame callback below (watch.attach), if any.
-- mGBA calls every "frame" callback once after each emulated frame, in the
-- order they were added. This one is added when a test loads this file,
-- before the test's fw.run() adds the one that resumes the test, so when a
-- test's fw.yield() returns, the attached watch has already checked that
-- frame.
local attached = nil
callbacks:add("frame", function()
    if attached then attached.frame() end
end)

-- Start watching the follower. Call watch.frame() once after every frame.
-- `name` prefixes the assertion messages. The tile the follower stands on
-- when the watch starts is the starting tile (frame 0).
--
-- The watch records:
--   watch.tiles         the follower's tile on every frame (1-based; [0]
--                       start)
--   watch.player_tiles  the player's tile on every frame, the same way
--   watch.still         whether the follower stood still on every frame (no
--                       step of its own playing, previous tile = current)
--   watch.changes       list of { frame, from_x, from_y, x, y, teleport,
--                       jump, swap, player_x, player_y (the player's tile
--                       that frame) }
--   watch.teleports     the subset of changes that were teleports
--   watch.swaps         list of swaps (see the header): { frame, x, y (the
--                       follower's new tile, the player's old one),
--                       player_x, player_y (the player's new tile, the
--                       follower's old one), follower_end, player_end (the
--                       frame each step ended, nil until then),
--                       follower_was_still (watch.still on the frame
--                       before), running, riding (the player's running and
--                       bicycle flags in gPlayerAvatar.flags on the swap
--                       frame) }
--   watch.failed        true once the per-frame rule has failed (reported
--                       once)
function follower.new_watch(name, snap)
    snap = snap or follower.map_snapshot()
    local f = follower.read()
    assert(f ~= nil, "no follower to watch")
    local watch = {
        name = name, snap = snap, frame_no = 0,
        tiles = {}, player_tiles = {}, changes = {}, teleports = {}, swaps = {},
        failed = false, stepped = false,
        -- The tile the last swap put the follower on, until its next step.
        swap_tile = nil,
    }
    watch.tiles[0] = { x = f.x, y = f.y }
    -- Whether the follower stands still, on every frame.
    local function standing(o) return not o.held and o.prev_x == o.x and o.prev_y == o.y end
    watch.still = { [0] = standing(f) }
    -- The spawn fallback (see the header): the watch starts with the
    -- follower on the player's tile. Only then may it share that tile, and
    -- stand on a warp, and only until its first step.
    local px, py = follower.player()
    watch.player_tiles[0] = { x = px, y = py }
    watch.spawn_fallback = px == f.x and py == f.y

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
        if snap.warp(x, y) then
            local spawned_here = watch.spawn_fallback and not watch.stepped
            local swapped_here = watch.swap_tile ~= nil
                and watch.swap_tile.x == x and watch.swap_tile.y == y
            if not (spawned_here or swapped_here) then return "warp tile" end
        end
        if snap.water(x, y) then
            return string.format("water (behaviour 0x%02x)", snap.behavior(x, y))
        end
        for _, o in ipairs(follower.other_objects()) do
            if o.x == x and o.y == y then
                local fallback = o.is_player and watch.spawn_fallback and not watch.stepped
                if not fallback then
                    return string.format("shared with object %d (local id %d%s)",
                        o.index, o.local_id, o.is_player and ", the player" or "")
                end
            end
        end
        return nil
    end

    function watch.frame()
        watch.frame_no = watch.frame_no + 1
        local n = watch.frame_no
        local x, y, cur = watch.tile()
        if x == nil then
            if not watch.failed then
                watch.failed = true
                T.assert_true(false, string.format("%s: follower sprite exists on frame %d", name, n))
            end
            return
        end
        local prev = watch.tiles[n - 1]
        watch.tiles[n] = { x = x, y = y }
        watch.still[n] = standing(cur)
        local player = follower.player_object()
        local pprev = watch.player_tiles[n - 1]
        watch.player_tiles[n] = { x = player and player.x, y = player and player.y }
        if x ~= prev.x or y ~= prev.y then
            -- A ledge jump takes the follower's tile (the landing tile, see
            -- the header) two tiles on in the jump's direction in one go;
            -- any other change of more than one tile is a teleport.
            local jump = false
            if cur.jump_dir then
                local d = DIR_DELTA[cur.jump_dir]
                jump = x == prev.x + 2 * d[1] and y == prev.y + 2 * d[2]
            end
            local far = not jump and math.abs(x - prev.x) + math.abs(y - prev.y) > 1
            local px, py = player and player.x, player and player.y
            -- A swap: the two traded tiles on this frame (see the header).
            local swap = player ~= nil and px == prev.x and py == prev.y
                and pprev.x == x and pprev.y == y
            local change = { frame = n, from_x = prev.x, from_y = prev.y, x = x, y = y,
                             teleport = far, jump = jump, swap = swap,
                             player_x = px, player_y = py }
            watch.changes[#watch.changes + 1] = change
            if far then watch.teleports[#watch.teleports + 1] = change end
            if swap then
                local avatar = emu:read8(sym.addr("gPlayerAvatar"))
                watch.swaps[#watch.swaps + 1] = {
                    frame = n, x = x, y = y, player_x = px, player_y = py,
                    follower_was_still = watch.still[n - 1],
                    running = (avatar & PLAYER_AVATAR_FLAG_DASH) ~= 0,
                    riding = (avatar & PLAYER_AVATAR_FLAG_MACH_BIKE) ~= 0,
                }
                watch.swap_tile = { x = x, y = y }
            else
                watch.swap_tile = nil
            end
            watch.stepped = true
            fw.log(string.format("  %s frame %d: follower (%d,%d) -> (%d,%d)%s",
                name, n, prev.x, prev.y, x, y,
                far and " TELEPORT" or (jump and " (ledge jump)" or (swap and " SWAP" or ""))))
        end
        -- The two steps of the last swap end on the first frame each one's
        -- previous tile has caught up with its current tile.
        local s = watch.swaps[#watch.swaps]
        if s then
            if s.follower_end == nil and cur.prev_x == cur.x and cur.prev_y == cur.y then
                s.follower_end = n
            end
            if s.player_end == nil and player
                and player.prev_x == player.x and player.prev_y == player.y then
                s.player_end = n
            end
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
    -- called after every frame's check with the frame number and tile; if
    -- it returns true the run stops there. Returns the frame it stopped on,
    -- or nil if it ran all `frames`.
    function watch.run(frames, each)
        for _ = 1, frames do
            fw.yield()
            watch.frame()
            if each then
                local t = watch.tiles[watch.frame_no]
                if each(watch.frame_no, t and t.x, t and t.y) then return watch.frame_no end
            end
        end
        return nil
    end

    -- Like watch.run, but stops on the first frame `stop()` returns true,
    -- before checking that frame: e.g. the frame a warp starts loading
    -- another map, whose loading removes the follower's sprite. Returns how
    -- many frames ran (that one included), or nil if `stop()` never
    -- returned true.
    function watch.run_until(frames, stop)
        for i = 1, frames do
            fw.yield()
            if stop() then return i end
            watch.frame()
        end
        return nil
    end

    -- Check every frame from a frame callback until detach(), for code that
    -- waits through other helpers (walk.to, fw.press, ...) instead of
    -- calling watch.frame() itself.
    function watch.attach()
        attached = watch
    end
    function watch.detach()
        if attached == watch then attached = nil end
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
    -- The first teleport made while the player's tile was water, or nil.
    function watch.teleport_while_player_on_water()
        for _, c in ipairs(watch.teleports) do
            if c.player_x and snap.water(c.player_x, c.player_y) then return c end
        end
        return nil
    end

    function watch.visited(x, y)
        for i = 0, watch.frame_no do
            local t = watch.tiles[i]
            if t and t.x == x and t.y == y then return true end
        end
        return false
    end

    return watch
end

-- The swap in `watch.swaps` in which the player stepped from (px, py) onto
-- (fx, fy) and the follower the other way, or nil.
function follower.find_swap(watch, px, py, fx, fy)
    for _, s in ipairs(watch.swaps) do
        if s.x == px and s.y == py and s.player_x == fx and s.player_y == fy then return s end
    end
    return nil
end

-- How many frames each step of swap `s` (one of watch.swaps) lasted, from
-- the swap frame to the frame it ended, both counted: the follower's step,
-- then the player's. nil for a step that hasn't ended yet. A walking step
-- is 16 frames, a running one 8, a bicycle one 6.
function follower.swap_lengths(s)
    local f = s.follower_end and (s.follower_end - s.frame + 1)
    local p = s.player_end and (s.player_end - s.frame + 1)
    return f, p
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
    return T.assert_true(follower.object_index() ~= nil, "fixture: Pikachu is walking")
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
    local item_hm03 = mm.read_defines("include/constants/items.h").ITEM_HM03
    local sb1 = mm.sb1()
    local enc_key = emu:read32(emu:read32(sym.addr("gSaveBlock2Ptr")) + 0xF20)
    emu:write16(sb1 + 0x048c, item_hm03)
    emu:write16(sb1 + 0x048c + 2, (1 ~ enc_key) & 0xFFFF)
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

-- While surfing, hold `key_name` until the player's tile is (lx, ly), the
-- landing tile on the far shore, or 600 frames pass. `watch` must be
-- attached (watch.attach). Returns the watch frame the player's tile became
-- (lx, ly), or nil if they never got there.
function follower.surf_to(key_name, lx, ly, watch)
    local landed = nil
    local idx = fw.KEY[key_name]
    emu:addKey(idx)
    for _ = 1, 600 do
        fw.yield()
        local px, py = follower.player()
        if px == lx and py == ly then
            landed = watch.frame_no
            break
        end
    end
    emu:clearKey(idx)
    return landed
end

-- Wait up to `frames` frames for the follower to be within 2 tiles of the
-- player. `watch` must be attached. Returns the watch frame it got there,
-- or nil.
function follower.wait_rejoin(watch, frames)
    local start = watch.frame_no
    while true do
        local px, py = follower.player()
        local t = watch.tiles[watch.frame_no]
        if follower.distance(t.x, t.y, px, py) <= 2 then return watch.frame_no end
        if watch.frame_no - start >= frames then return nil end
        fw.yield()
    end
end

-- Turn the player to face `key` ("UP", "DOWN", "LEFT", "RIGHT") without
-- moving: only for spots where the tile that way is solid, so holding the
-- direction just bumps.
function follower.face(key_name)
    fw.hold(key_name, 6)
    fw.wait_frames(30)
end

-- Turn the player to face `key` without moving, wherever they stand: a
-- one-frame tap. A direction other than the one the player faces, pressed
-- while they stand still, only turns them (the game's "turn in place"); a
-- step needs the key held on after the turn.
function follower.turn(key_name)
    fw.press_and_wait(key_name, 30)
end

return follower
