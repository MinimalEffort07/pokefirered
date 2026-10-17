-------------------------------------------------------------------------------
-- walk.lua: walk the player somewhere for real, one D-pad press per step
-------------------------------------------------------------------------------
--
-- Scenario tests must not teleport the player: they plan a route with a
-- breadth-first search (BFS) over the map the game has loaded, then press
-- the D-pad exactly as a person would, one tile at a time. Before every
-- step the route is planned again from what is in RAM right now, because
-- NPCs and the overworld roaming Pokemon move around.
--
-- What blocks a tile, for the planner:
--   * the tile is solid (its collision bits are not 0, see mt_moon.lua);
--   * a warp sits on it and it isn't where we are going (stepping on a
--     ladder or door would leave the map);
--   * an object stands on it, or is walking onto or off it (current and
--     previous coordinates);
--   * it is next to a roaming Pokemon (local id 200 and up). Walking into
--     one starts a wild battle, and they move, so the planner keeps a
--     one-tile gap.
--
-- A step: hold the direction until the player's tile changes (if the player
-- faces another way, the game first turns them, which takes a few frames),
-- then let go. The game finishes the 16-frame step on its own. If an NPC
-- steps into the way, the player only bumps; the key is released after a
-- while and the route is planned again.
--
-- Also here: make_walk_safe() gives the player a strong party Pokemon and a
-- long-lasting Repel, so no wild battle can start mid-walk (the new-game
-- save these tests use has an empty party).
--
-- Everything that waits for frames must run inside fw.run().
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw  = dofile(project_dir .. "test/lib/framework.lua")
local sym = dofile(project_dir .. "test/lib/symbols.lua")
local mm  = dofile(project_dir .. "test/lib/mt_moon.lua")

local walk = {}

local DIRS = {
    { key = "UP",    dx = 0,  dy = -1 },
    { key = "DOWN",  dx = 0,  dy = 1 },
    { key = "LEFT",  dx = -1, dy = 0 },
    { key = "RIGHT", dx = 1,  dy = 0 },
}

-- Local ids of the overworld roaming Pokemon (src/roaming_pokemon.c).
local ROAMER_LOCAL_ID_MIN = 200

-------------------------------------------------------------------------------
-- No wild battles
-------------------------------------------------------------------------------

-- struct Pokemon is 100 bytes. A party slot with personality 0 and trainer
-- id 0 is stored unencrypted (the key is personality XOR trainer id) with
-- its four data blocks in the order Growth, Attacks, EVs, Misc. The game
-- checks a checksum: the sum of the 24 u16s of those blocks, which is just
-- the species when nothing else is set.
local POKEMON_SIZE = 100
local VAR_REPEL_STEP_COUNT = mm.read_defines("include/constants/vars.h").VAR_REPEL_STEP_COUNT

-- Put a level-100 Pokemon in party slot 1 and start a Repel with `steps`
-- steps. A Repel stops every wild Pokemon below the level of the first
-- healthy party Pokemon, and nothing wild in Kanto reaches level 100, so no
-- wild encounter can start. (Walking into a roaming Pokemon still would,
-- which the planner avoids.)
function walk.make_walk_safe(steps)
    local party = sym.addr("gPlayerParty")
    local species = 150  -- any valid species; Mewtwo
    for i = 0, POKEMON_SIZE - 1 do emu:write8(party + i, 0) end
    emu:write8(party + 0x13, 0x02)            -- flags: hasSpecies
    emu:write16(party + 0x20, species)        -- Growth block: species
    emu:write16(party + 0x1C, species)        -- checksum
    emu:write8(party + 0x54, 100)             -- level
    emu:write8(party + 0x55, 0xFF)            -- no mail
    emu:write16(party + 0x56, 300)            -- current HP
    emu:write16(party + 0x58, 300)            -- max HP
    emu:write8(sym.addr("gPlayerPartyCount"), 1)
    mm.set_var(VAR_REPEL_STEP_COUNT, steps or 5000)
end

-------------------------------------------------------------------------------
-- Player state
-------------------------------------------------------------------------------

-- gPlayerAvatar.tileTransitionState (offset 3): 0 when the player stands
-- centred on a tile, not mid-step.
local function standing_still()
    return emu:read8(sym.addr("gPlayerAvatar") + 3) == 0
end

-- True when the player could take a step right now.
function walk.ready()
    return mm.in_overworld() and mm.player_has_control() and standing_still()
        and mm.player_tile() ~= nil
end

local function in_battle()
    return sym.is_func(emu:read32(sym.addr("gMain") + 4), "BattleMainCB2")
end

-------------------------------------------------------------------------------
-- Planning
-------------------------------------------------------------------------------

-- Tiles blocked by objects (see the header), as a set of mm.key(x, y).
local function object_tiles()
    local set = {}
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player then
            set[mm.key(o.x, o.y)] = true
            set[mm.key(o.prev_x, o.prev_y)] = true
            if o.local_id >= ROAMER_LOCAL_ID_MIN then
                for _, d in ipairs(DIRS) do set[mm.key(o.x + d.dx, o.y + d.dy)] = true end
            end
        end
    end
    return set
end

-- The direction of the first step of a shortest route from the player to
-- (tx, ty), or nil if there is no route right now, plus the route length.
-- Searches backwards from the target, then takes the player's neighbour
-- closest to it.
function walk.plan(tx, ty)
    local px, py = mm.player_tile()
    if px == nil then return nil end
    local grid = mm.read_grid()
    local objects = object_tiles()
    local warps = {}
    for _, w in ipairs(mm.read_warps()) do warps[mm.key(w.x, w.y)] = true end
    local function blocked(x, y)
        if x == px and y == py then return false end
        return objects[mm.key(x, y)] or warps[mm.key(x, y)]
    end
    local dist = mm.bfs(grid, tx, ty, blocked)
    local best, best_d = nil, nil
    for _, d in ipairs(DIRS) do
        local nd = dist[mm.key(px + d.dx, py + d.dy)]
        if nd and not objects[mm.key(px + d.dx, py + d.dy)] and (best_d == nil or nd < best_d) then
            best, best_d = d, nd
        end
    end
    return best, best_d and best_d + 1
end

-- The map as the planner sees it, one text line per row, for failure logs:
--   P player  T target  W other warp  o object  r roaming Pokemon
--   + tile a roaming Pokemon could step onto  # solid  . walkable
function walk.describe_map(tx, ty)
    local grid = mm.read_grid()
    local px, py = mm.player_tile()
    local marks = {}
    for _, w in ipairs(mm.read_warps()) do marks[mm.key(w.x, w.y)] = "W" end
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player then
            if o.local_id >= ROAMER_LOCAL_ID_MIN then
                for _, d in ipairs(DIRS) do
                    local k = mm.key(o.x + d.dx, o.y + d.dy)
                    if marks[k] == nil then marks[k] = "+" end
                end
                marks[mm.key(o.x, o.y)] = "r"
            else
                marks[mm.key(o.x, o.y)] = "o"
            end
        end
    end
    if tx then marks[mm.key(tx, ty)] = "T" end
    if px then marks[mm.key(px, py)] = "P" end
    local lines = {}
    for y = 0, grid.h - 1 do
        local row = {}
        for x = 0, grid.w - 1 do
            local k = mm.key(x, y)
            row[#row + 1] = marks[k] or (mm.passable(grid.cells[k]) and "." or "#")
        end
        lines[#lines + 1] = string.format("%2d %s", y, table.concat(row))
    end
    return lines
end

-------------------------------------------------------------------------------
-- Stepping
-------------------------------------------------------------------------------

-- Hold `dir` until the player starts moving onto the next tile, the map
-- changes (a warp fired), or `timeout` frames pass. Returns true if the
-- player moved.
local function step(dir, timeout)
    local px, py = mm.player_tile()
    local group, num = mm.current_map()
    local idx = fw.KEY[dir.key]
    local moved = false
    emu:addKey(idx)
    for _ = 1, timeout do
        fw.yield()
        local g, n = mm.current_map()
        if g ~= group or n ~= num or not mm.in_overworld() then moved = true break end
        local x, y = mm.player_tile()
        if x ~= px or y ~= py then moved = true break end
    end
    emu:clearKey(idx)
    return moved
end

-- Wait until the player can take a step again on map (group, num).
-- Returns "ready", "warped" (the map changed: a warp fired), "battle" or
-- "timeout".
local function wait_ready(timeout, group, num)
    for _ = 1, timeout do
        local g, n = mm.current_map()
        if g ~= group or n ~= num then return "warped" end
        if walk.ready() then return "ready" end
        if in_battle() then return "battle" end
        fw.yield()
    end
    return "timeout"
end

-- Walk to (tx, ty) on the current map. Returns as soon as the map changes
-- (the target, or a tile on the way, was a warp); call wait_for_arrival()
-- next. Returns a result table:
--   { ok = bool, warped = bool, steps = n, replans = n, why = "..." }
-- `opts.on_last_step()` (optional) runs just before the step onto the
-- target, e.g. to set the random state a warp will use.
function walk.to(tx, ty, opts)
    opts = opts or {}
    local max_steps = opts.max_steps or 400
    local group, num = mm.current_map()
    local result = { ok = false, warped = false, steps = 0, replans = 0 }
    local stuck_frames = 0
    while result.steps < max_steps do
        local state = wait_ready(600, group, num)
        if state == "warped" then
            result.ok = true
            result.warped = true
            return result
        elseif state ~= "ready" then
            result.why = state == "battle" and "a battle started" or "never got control back"
            return result
        end
        local px, py = mm.player_tile()
        if px == tx and py == ty then
            result.ok = true
            return result
        end
        local dir, length = walk.plan(tx, ty)
        if dir == nil then
            -- Nothing reaches the target right now: an NPC or a roaming
            -- Pokemon may be in a corridor. Wait for it to move.
            stuck_frames = stuck_frames + 15
            result.replans = result.replans + 1
            if stuck_frames > (opts.max_wait or 1200) then
                result.why = string.format("no route from (%d,%d) to (%d,%d)", px, py, tx, ty)
                return result
            end
            fw.wait_frames(15)
        else
            if length == 1 and opts.on_last_step then opts.on_last_step() end
            if step(dir, 40) then
                result.steps = result.steps + 1
                stuck_frames = 0
            else
                result.replans = result.replans + 1
                stuck_frames = stuck_frames + 40
                if stuck_frames > (opts.max_wait or 1200) then
                    result.why = string.format("stuck at (%d,%d) going %s", px, py, dir.key)
                    return result
                end
            end
        end
        if in_battle() then
            result.why = "a battle started"
            return result
        end
    end
    result.why = "too many steps"
    return result
end

-------------------------------------------------------------------------------
-- Talking to things
-------------------------------------------------------------------------------

-- struct ObjectEvent (include/global.fieldmap.h): facingDirection is the low
-- 4 bits of the byte at +0x18. Direction values from include/constants/
-- global.h: DIR_SOUTH 1, DIR_NORTH 2, DIR_WEST 3, DIR_EAST 4.
local OE_FACING = 0x18
local DIR_VALUE = { DOWN = 1, UP = 2, LEFT = 3, RIGHT = 4 }

-- struct PlayerAvatar (include/global.fieldmap.h): runningState is the byte
-- at +2; NOT_MOVING (0) once the player has stood still with no direction
-- held for a frame (CheckMovementInputNotOnBike in field_player_avatar.c).
local PLAYER_RUNNING_STATE = 2
local NOT_MOVING = 0

local function player_facing()
    for _, o in ipairs(mm.live_objects()) do
        if o.is_player then
            return emu:read8(sym.addr("gObjectEvents") + o.index * 0x24 + OE_FACING) & 0xF
        end
    end
    return nil
end

-- Walk to the reachable tile next to (tx, ty) (north, south, west or east
-- of it) that is closest to the player, then turn to face (tx, ty), ready
-- to press A. (tx, ty) is usually solid: an object or a boulder. Returns a
-- walk.to() result, with ok = false if no neighbour can be reached or the
-- player doesn't end up facing the target.
function walk.to_face(tx, ty, opts)
    local px, py = mm.player_tile()
    if px == nil then return { ok = false, steps = 0, replans = 0, why = "no player" } end
    local objects = object_tiles()
    local warps = {}
    for _, w in ipairs(mm.read_warps()) do warps[mm.key(w.x, w.y)] = true end
    local dist = mm.bfs(mm.read_grid(), px, py, function(x, y)
        return objects[mm.key(x, y)] or warps[mm.key(x, y)]
    end)
    local best, best_d = nil, nil
    for _, d in ipairs(DIRS) do
        -- The player stands at target - d and faces along d.
        local k = mm.key(tx - d.dx, ty - d.dy)
        if dist[k] and (best_d == nil or dist[k] < best_d) then best, best_d = d, dist[k] end
    end
    if best == nil then
        return { ok = false, steps = 0, replans = 0,
                 why = string.format("no reachable tile next to (%d,%d)", tx, ty) }
    end
    local result = walk.to(tx - best.dx, ty - best.dy, opts)
    if not result.ok then return result end
    -- Turn. The game can still see the last step's direction as held on
    -- the frame the walk ends, and then starts one more step that way (a
    -- 16-frame walk in place if it is blocked), ignoring other input until
    -- it ends. So first release every key and wait until the player has
    -- been standing idle for a few frames (gPlayerAvatar.runningState back
    -- to NOT_MOVING). Then a press turns the player in place within a frame
    -- or two; the key is released as soon as they face the target, so they
    -- never step onto it even when it is free floor.
    fw.clear_all_keys()
    local idle = 0
    for _ = 1, 90 do
        if walk.ready() and emu:read8(sym.addr("gPlayerAvatar") + PLAYER_RUNNING_STATE) == NOT_MOVING then
            idle = idle + 1
            if idle >= 3 then break end
        else
            idle = 0
        end
        fw.yield()
    end
    local idx = fw.KEY[best.key]
    emu:addKey(idx)
    for _ = 1, 30 do
        fw.yield()
        if player_facing() == DIR_VALUE[best.key] then break end
    end
    emu:clearKey(idx)
    fw.wait_until(walk.ready, 60, "standing still after turning")
    local facing = player_facing()
    local px2, py2 = mm.player_tile()
    if facing ~= DIR_VALUE[best.key] or px2 ~= tx - best.dx or py2 ~= ty - best.dy then
        result.ok = false
        result.why = string.format("not facing %s from (%d,%d) after turning: facing %s, player at (%d,%d)",
            best.key, tx - best.dx, ty - best.dy, tostring(facing), px2 or -1, py2 or -1)
    end
    result.facing = best.key
    return result
end

-- Press A (to talk, read, pick up) and keep pressing A to page through the
-- messages that follow, until `done()` is true and the player has control
-- again, or `timeout` frames pass. Returns true on success.
function walk.press_a_until(done, timeout)
    for _ = 1, math.floor((timeout or 900) / 16) do
        fw.press("A")
        fw.wait_frames(14)
        if done() and walk.ready() then return true end
    end
    return false
end

-- Hold `key` (an arrow warp's direction) until the map changes. Returns
-- true if it did within `timeout` frames.
function walk.hold_into_warp(key, timeout)
    local group, num = mm.current_map()
    return fw.hold_until(key, function()
        local g, n = mm.current_map()
        return g ~= group or n ~= num
    end, timeout or 120, 1)
end

-- After a warp has fired, wait for the new map. Returns
--   { group, num, x, y }  where (x, y) is the player's tile on the first
-- frame of the new map, before any exit animation moves them (leaving
-- Route 4's cave doors, the game walks the player one step out), or nil.
function walk.wait_for_arrival(from_group, from_num, timeout)
    for _ = 1, timeout or 600 do
        local g, n = mm.current_map()
        if (g ~= from_group or n ~= from_num) and mm.in_overworld() then
            local x, y = mm.player_tile()
            if x ~= nil then return { group = g, num = n, x = x, y = y } end
        end
        fw.yield()
    end
    return nil
end

return walk
