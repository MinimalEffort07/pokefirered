-------------------------------------------------------------------------------
-- test_route4_no_warp.lua
-------------------------------------------------------------------------------
-- Checks that talking to the people standing outside Mt. Moon on Route 4 no
-- longer warps the player away. A debug shortcut had appended
--     warp MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB, 6, 12
-- to Route4_EventScript_Woman, so finishing her dialog teleported the player
-- into Oak's Lab. The Boy next to her is checked too, since he is the other
-- person a player meets on the way into the cave.
--
-- FLOW:
--   1. Boot through Oak's speech into the overworld (character_select.lua).
--   2. Warp to Route 4. warp.warp_to_pos() loads the map but leaves the
--      player on the tile they stood on before the warp, which after a
--      fresh boot is (6,6): open ground north-west of the Woman. The walking
--      code below plans from wherever the player actually is, so this is fine.
--   3. Walk up to the Woman (local id 1), face her, press A, and page through
--      her dialog. Assert the script that ran was hers (gSpecialVar_LastTalked).
--   4. Watch the map for 4 seconds after the dialog closes. The old warp fired
--      within ~1 second of the text box closing, so if the map is still
--      Route 4 and the player has not moved, the warp is gone.
--   5. Repeat 3-4 for the Boy (local id 4).
--
-- WALKING TO A WANDERING NPC:
--   Both NPCs wander (MOVEMENT_TYPE_WANDER_AROUND / _LEFT_AND_RIGHT), so their
--   tile changes every few seconds. The test therefore re-plans every step:
--   read the NPC's current tile, pick an empty tile next to it, BFS a path
--   there over the map's collision bits, take ONE step, and repeat. Once the
--   player stands beside the NPC and faces it, press A. Pressing A while the
--   NPC is mid-step just does nothing, so that attempt is retried.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua): the boot, the walks and the dialogs
-- fast-forward. For the Woman and then the Boy, the first second of the
-- watch in step 4 is a moment: the dialog has closed and the screen stays
-- on Route 4, with no fade to Oak's Lab. The watch is a timed stretch, so
-- the moment is a fw.show_next at its start, which adds no frames.
--
-- RECORDING:
--   Set MGBA_FRAME_DIR to a directory and every second emulated frame from the
--   Route 4 warp onwards is saved there as frame_NNNNN.png, with a matching
--   captions.tsv (frame index, phase label, current map) for building a video:
--     MGBA_FRAME_DIR=/tmp/route4_frames bash test/run_test.sh \
--         test/tests/test_route4_no_warp.lua
--
-- Run:
--   bash test/run_test.sh test/tests/test_route4_no_warp.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local ROUTE4_GROUP, ROUTE4_NUM = 3, 22   -- MAP_ROUTE4 = (22 | (3 << 8))
local ROUTE4_PACKED = ROUTE4_GROUP * 256 + ROUTE4_NUM
local OAKS_LAB_PACKED = 4 * 256 + 3      -- MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB

-- Object event local ids are 1-based positions in Route4/map.json's
-- object_events array: the Woman is entry 1, the Boy is entry 4.
local LOCALID_WOMAN = 1
local LOCALID_BOY   = 4

-- Tile requested from warp.warp_to_pos(). The helper does not currently
-- honour it (see step 2 above); the player starts on (6,6) instead.
local START_X, START_Y = 12, 10

-- ObjectEvent coords include a 7-tile border offset (MAP_OFFSET in the C
-- code), so map tile (x, y) is stored as (x + 7, y + 7).
local MAP_OFFSET = 7

-- script.c keeps its state in static (file-local) variables, which have no
-- entry in pokefirered.map. Their addresses are the .bss base of script.o
-- (0x03000ea8, from pokefirered.map) plus each variable's offset from
-- `arm-none-eabi-nm build/firered/src/script.o`. Re-derive after any change
-- that moves IWRAM .bss.
local S_GLOBAL_SCRIPT_CONTEXT_STATUS = 0x03000ea8  -- u8, +0x00
local S_LOCK_FIELD_CONTROLS          = 0x03000f9c  -- bool8, +0xf4
local CONTEXT_SHUTDOWN = 2   -- no script running (enum in script.c)

-- Set by the engine to the local id of the object the player last talked to.
local G_SPECIAL_VAR_LAST_TALKED = 0x020370fa

-- gMapHeader.mapLayout -> struct MapLayout { s32 width; s32 height;
-- u16 *border; u16 *map; ... }. Each map entry is a u16 whose bits 10-11
-- are the collision value (0 = walkable).
local MAP_LAYOUT_WIDTH  = 0x00
local MAP_LAYOUT_HEIGHT = 0x04
local MAP_LAYOUT_MAP    = 0x0c

-- ObjectEvent field offsets not covered by addresses.lua.
local OE_FACING = 0x18       -- low 4 bits: facingDirection (DIR_*)
local OE_FLAG_ACTIVE = 0x01  -- bit 0 of the first word
local OE_FLAG_IS_PLAYER = 0x10000  -- bit 16 of the first word

local DIR_SOUTH, DIR_NORTH, DIR_WEST, DIR_EAST = 1, 2, 3, 4
local DIR_INFO = {
    [DIR_SOUTH] = { key = "DOWN",  dx =  0, dy =  1 },
    [DIR_NORTH] = { key = "UP",    dx =  0, dy = -1 },
    [DIR_WEST]  = { key = "LEFT",  dx = -1, dy =  0 },
    [DIR_EAST]  = { key = "RIGHT", dx =  1, dy =  0 },
}
local OPPOSITE = {
    [DIR_SOUTH] = DIR_NORTH, [DIR_NORTH] = DIR_SOUTH,
    [DIR_WEST]  = DIR_EAST,  [DIR_EAST]  = DIR_WEST,
}

-- How long to keep watching after a dialog closes. The removed warp started
-- its fade as soon as the text box closed and loaded Oak's Lab well inside
-- one second; 240 frames (4 seconds) leaves a wide margin.
local WATCH_FRAMES = 240

-------------------------------------------------------------------------------
-- Frame capture (optional, see RECORDING above)
-------------------------------------------------------------------------------

local FRAME_DIR = os.getenv("MGBA_FRAME_DIR")
local capturing = false
local capture_tick = 0
local capture_idx = 0
local phase_label = ""
local captions = nil

local function map_label(packed)
    if packed == ROUTE4_PACKED then return "ROUTE 4" end
    if packed == OAKS_LAB_PACKED then return "OAK'S LAB (PALLET TOWN)" end
    return string.format("%d.%d", math.floor(packed / 256), packed % 256)
end

local function set_phase(label)
    phase_label = label
    fw.log("[PHASE] " .. label)
end

local function start_capture()
    if FRAME_DIR == nil or FRAME_DIR == "" then return end
    os.execute("mkdir -p '" .. FRAME_DIR .. "' && rm -f '" .. FRAME_DIR
        .. "'/frame_*.png")
    captions = io.open(FRAME_DIR .. "/captions.tsv", "w")
    capturing = true
end

local function stop_capture()
    capturing = false
    if captions then captions:close(); captions = nil end
end

-- Runs every emulated frame, outside the test coroutine, so frames spent
-- inside fw.hold/fw.press are captured too. Saving every second frame gives
-- a 30 fps video of the 60 fps game.
callbacks:add("frame", function()
    if not capturing then return end
    capture_tick = capture_tick + 1
    if capture_tick % 2 ~= 0 then return end
    emu:screenshot(string.format("%s/frame_%05d.png", FRAME_DIR, capture_idx))
    captions:write(string.format("%d\t%s\tMAP: %s\n", capture_idx,
        phase_label, map_label(warp.current_map_packed())))
    capture_idx = capture_idx + 1
end)

-------------------------------------------------------------------------------
-- Object event and map helpers
-------------------------------------------------------------------------------

local function oe_addr(index)
    return ADDR.gObjectEvents + index * ADDR.OBJECT_EVENT_SIZE
end

local function oe_tile(oa)
    return fw.read16(oa + ADDR.OE_CURRENT_X) - MAP_OFFSET,
           fw.read16(oa + ADDR.OE_CURRENT_Y) - MAP_OFFSET
end

local function player_oe()
    return oe_addr(fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID))
end

local function player_tile()
    return oe_tile(player_oe())
end

local function player_facing()
    return fw.read8(player_oe() + OE_FACING) & 0x0F
end

-- Find an active, non-player object event on Route 4 by its local id.
local function find_npc(local_id)
    for i = 0, ADDR.OBJECT_EVENTS_COUNT - 1 do
        local oa = oe_addr(i)
        local flags = fw.read32(oa + ADDR.OE_FLAGS)
        if (flags & OE_FLAG_ACTIVE) ~= 0 and (flags & OE_FLAG_IS_PLAYER) == 0
            and fw.read8(oa + ADDR.OE_LOCAL_ID) == local_id
            and fw.read8(oa + ADDR.OE_MAP_NUM) == ROUTE4_NUM
            and fw.read8(oa + ADDR.OE_MAP_GROUP) == ROUTE4_GROUP then
            return oa
        end
    end
    return nil
end

-- Tiles currently occupied by any other active object event (NPCs block
-- movement just like walls).
local function occupied_tiles()
    local occ = {}
    for i = 0, ADDR.OBJECT_EVENTS_COUNT - 1 do
        local oa = oe_addr(i)
        local flags = fw.read32(oa + ADDR.OE_FLAGS)
        if (flags & OE_FLAG_ACTIVE) ~= 0 and (flags & OE_FLAG_IS_PLAYER) == 0 then
            local x, y = oe_tile(oa)
            occ[y * 1000 + x] = true
        end
    end
    return occ
end

-- Read the loaded map's walkability straight from its layout in ROM.
local function load_collision()
    local layout = fw.read32(ADDR.gMapHeader)
    local w = fw.read32(layout + MAP_LAYOUT_WIDTH)
    local h = fw.read32(layout + MAP_LAYOUT_HEIGHT)
    local map = fw.read32(layout + MAP_LAYOUT_MAP)
    local blocked = {}
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            local block = fw.read16(map + (y * w + x) * 2)
            blocked[y * 1000 + x] = ((block >> 10) & 3) ~= 0
        end
    end
    return { w = w, h = h, blocked = blocked }
end

local function walkable(grid, occ, x, y)
    if x < 0 or y < 0 or x >= grid.w or y >= grid.h then return false end
    local k = y * 1000 + x
    return not grid.blocked[k] and not occ[k]
end

-- Breadth-first search from the player's tile to (tx, ty). Returns the
-- direction of the first step and the path length, or nil if unreachable.
local function first_step_towards(grid, tx, ty)
    local sx, sy = player_tile()
    if sx == tx and sy == ty then return nil, 0 end
    local occ = occupied_tiles()
    local start = sy * 1000 + sx
    local first = { [start] = 0 }
    local dist = { [start] = 0 }
    local queue = { { sx, sy } }
    local head = 1
    while head <= #queue do
        local cx, cy = queue[head][1], queue[head][2]
        head = head + 1
        local ck = cy * 1000 + cx
        for dir, d in pairs(DIR_INFO) do
            local nx, ny = cx + d.dx, cy + d.dy
            local nk = ny * 1000 + nx
            if first[nk] == nil and walkable(grid, occ, nx, ny) then
                first[nk] = (ck == start) and dir or first[ck]
                dist[nk] = dist[ck] + 1
                if nx == tx and ny == ty then return first[nk], dist[nk] end
                queue[#queue + 1] = { nx, ny }
            end
        end
    end
    return nil, nil
end

-------------------------------------------------------------------------------
-- Player input helpers
-------------------------------------------------------------------------------

-- Take one step in `dir`. Holds the key until the player's tile changes
-- (the engine updates currentCoords the moment a step begins), then lets the
-- 16-frame walk animation finish. If the player was facing elsewhere the
-- first press only turns them; the caller simply calls again.
local function step(dir)
    local idx = fw.KEY[DIR_INFO[dir].key]
    local x0, y0 = player_tile()
    emu:addKey(idx)
    for _ = 1, 12 do
        fw.yield()
        local x, y = player_tile()
        if x ~= x0 or y ~= y0 then break end
    end
    emu:clearKey(idx)
    fw.wait_frames(16)
end

-- Turn in place to face `dir`. A short tap in a new direction turns the
-- player without walking, as long as they are standing still.
local function face(dir)
    for _ = 1, 5 do
        if player_facing() == dir then return true end
        fw.hold(DIR_INFO[dir].key, 2)
        fw.wait_frames(10)
    end
    return player_facing() == dir
end

local function script_idle()
    return fw.read8(S_GLOBAL_SCRIPT_CONTEXT_STATUS) == CONTEXT_SHUTDOWN
        and fw.read8(S_LOCK_FIELD_CONTROLS) == 0
end

-- Press A through every page of the current dialog until the script ends.
-- The script state is checked before each press so a stray A never starts
-- a second conversation after the first one closes.
local function finish_dialog()
    for _ = 1, 30 do
        fw.wait_frames(60)   -- let the page print at normal text speed
        if script_idle() then return true end
        fw.press("A")
    end
    return script_idle()
end

-- Walk up to the NPC with `local_id`, face it, and press A. `sides` lists
-- where to stand relative to the NPC, in order of preference, as the
-- direction the player must face to look at it. Returns true once that
-- NPC's script has started.
local function talk_to(local_id, sides)
    local grid = load_collision()
    for _ = 1, 80 do
        local npc = find_npc(local_id)
        if npc == nil then
            fw.log_error("NPC with local id " .. local_id .. " not on map")
            return false
        end
        local nx, ny = oe_tile(npc)

        -- Choose the reachable standing tile with the shortest path,
        -- keeping the caller's preference order to break ties.
        local best_dir, best_len, best_side, best_tx, best_ty
        for _, facing_dir in ipairs(sides) do
            local back = DIR_INFO[OPPOSITE[facing_dir]]
            local tx, ty = nx + back.dx, ny + back.dy
            local dir, len = first_step_towards(grid, tx, ty)
            if len ~= nil and (best_len == nil or len < best_len) then
                best_dir, best_len, best_side = dir, len, facing_dir
                best_tx, best_ty = tx, ty
            end
        end

        if best_len == nil then
            fw.wait_frames(20)  -- every side blocked right now; let NPC move
        elseif best_len > 0 then
            step(best_dir)
        elseif face(best_side) then
            local cx, cy = oe_tile(npc)
            if cx == nx and cy == ny then
                fw.press("A")
                for _ = 1, 20 do
                    if not script_idle() then break end
                    fw.yield()
                end
                if not script_idle() then
                    if fw.read16(G_SPECIAL_VAR_LAST_TALKED) == local_id then
                        return true
                    end
                    -- Something else (e.g. a sign) answered; close it.
                    finish_dialog()
                end
            end
        end
    end
    fw.log_error("could not start a conversation with local id " .. local_id)
    return false
end

-- Talk to one NPC and verify the player is still on Route 4, on the same
-- tile, for WATCH_FRAMES frames after the dialog closes.
local function check_no_warp(name, local_id, sides)
    set_phase("Walking to the " .. name)
    local started = talk_to(local_id, sides)
    fw.assert_true(started, name .. ": conversation started")
    if not started then return end
    fw.assert_eq(fw.read16(G_SPECIAL_VAR_LAST_TALKED), local_id,
        name .. ": the script that ran is this NPC's")

    set_phase("Talking to the " .. name)
    local px, py = player_tile()
    local closed = finish_dialog()
    fw.assert_true(closed, name .. ": dialog closed and script ended")

    set_phase("Dialog closed: watching for a warp")
    if closed then
        fw.show_next(name .. ": the dialog has closed and the screen stays on Route 4, with no fade to Oak's Lab")
    end
    local left_at = nil
    local left_to = nil
    for i = 1, WATCH_FRAMES do
        local m = warp.current_map_packed()
        if m ~= ROUTE4_PACKED and left_at == nil then
            left_at, left_to = i, m
        end
        fw.yield()
    end
    if left_at ~= nil then
        fw.log_error(string.format("%s: map changed to %s %d frames after the dialog",
            name, map_label(left_to), left_at))
    end
    fw.assert_eq(warp.current_map_packed(), ROUTE4_PACKED,
        name .. ": still on Route 4 " .. WATCH_FRAMES .. " frames after talking")
    fw.assert_true(left_at == nil,
        name .. ": map never changed while watching")
    local qx, qy = player_tile()
    fw.assert_true(qx == px and qy == py, string.format(
        "%s: player stayed on tile (%d,%d), now (%d,%d)", name, px, py, qx, qy))

    set_phase("No warp after talking to the " .. name)
    fw.wait_frames(60)
end

fw.run(function()
    fw.log("=== Route 4: people outside Mt. Moon do not warp ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    local ok = warp.warp_to_pos(ROUTE4_GROUP, ROUTE4_NUM, START_X, START_Y)
    fw.assert_true(ok, "warped to Route 4 outside Mt. Moon")
    if not ok then fw.finish() return end
    fw.log(string.format("player starts on tile (%d,%d)", player_tile()))

    start_capture()
    set_phase("Route 4, outside MT. MOON")
    fw.wait_frames(60)

    check_no_warp("WOMAN", LOCALID_WOMAN, { DIR_NORTH, DIR_EAST, DIR_WEST })
    check_no_warp("BOY", LOCALID_BOY, { DIR_SOUTH, DIR_NORTH })

    set_phase("Done: still on ROUTE 4")
    fw.wait_frames(90)
    stop_capture()
    fw.finish()
end)
