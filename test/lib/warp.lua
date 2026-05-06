-------------------------------------------------------------------------------
-- warp.lua
-------------------------------------------------------------------------------
-- Shared helper for programmatically warping the player to an arbitrary
-- map from a mGBA Lua test/tool. Bypasses door/stair walk-through which
-- is fragile to drive via button presses.
--
-- Usage:
--   local warp = dofile(project_dir .. "test/lib/warp.lua")
--   warp.warp_to(3, 19, 0)   -- Route 1 via south entry
--   warp.warp_to(3, 35, 0)   -- Route 17 (Cycling Road, Spearow/Fearow/Doduo)
--
-- HOW IT WORKS (what the normal script `warp` command does internally):
--   1. Write the destination into `sWarpDestination` (static EWRAM struct
--      at 0x02031de4, immediately after `gLastUsedWarp`).
--   2. Also write it into `gSaveBlock1Ptr->location`. The normal flow
--      has ApplyCurrentWarp() do this copy; since we're skipping
--      DoWarp/Task_Teleport2Warp (no fade, no SE, straight to load) we
--      apply it by hand.
--   3. Overwrite `gMain.callback2` (IWRAM 0x03003104) with the address
--      of CB2_LoadMap plus the ARM-thumb bit (|1). The engine's frame
--      driver picks up the new callback next tick and chains through
--      CB2_LoadMap -> CB2_DoChangeMap -> CB2_LoadMap2 -> CB2_Overworld,
--      loading the destination map.
--   4. Wait for gSaveBlock1Ptr->location to match the target AND for a
--      settle window so post-load setup (object events, scripts) runs.
-------------------------------------------------------------------------------

local warp = {}

-- Required framework + address helpers. We assume the caller has already
-- loaded them (they're idempotent) but dofile here guarantees they exist
-- even if this library is required standalone.
local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

-- sWarpDestination is static EWRAM, immediately after gLastUsedWarp
-- (see src/overworld.c:139-140). Layout: 8 bytes of struct WarpData.
warp.S_WARP_DESTINATION = 0x02031de4

-- gMain.callback2 slot in IWRAM + the CB2_LoadMap entry point.
-- The |1 we OR in is the ARM thumb bit (all game code here is thumb).
-- CB2_LOAD_MAP is a ROM code address that SHIFTS with every rebuild; re-extract
-- it from pokefirered.map after any change that adds/removes code.
warp.G_MAIN_CB2   = 0x03003104
warp.CB2_LOAD_MAP = 0x08056ae4

-- Read the currently-loaded map from SaveBlock1->location as a packed
-- (mapGroup*256 + mapNum) value, for easy equality checks.
function warp.current_map_packed()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local mg = fw.read8(sb1 + ADDR.SB1_LOC_MAP_GROUP)
    local mn = fw.read8(sb1 + ADDR.SB1_LOC_MAP_NUM)
    return mg * 256 + mn
end

-- Warp to (mapGroup, mapNum) via the given warpId. warpId indexes into
-- the target map's warp-table — 0 is almost always a boundary entry
-- (route transition / building exit), which is what we want. Use -1
-- (0xFF) for WARP_ID_NONE, in which case the engine won't adjust x/y.
--
-- Returns true on success; false if the map did not load within the
-- settle window (caller should log / abort).
--
-- Must be called from inside fw.run() — uses fw.wait_frames/wait_until.
function warp.warp_to(map_group, map_num, warp_id)
    warp_id = warp_id or 0

    local target_packed = map_group * 256 + map_num
    local before = warp.current_map_packed()
    fw.log(string.format("[WARP] %d.%d -> %d.%d (warpId=%d)",
        math.floor(before / 256), before % 256,
        map_group, map_num, warp_id))

    -- 1. Write sWarpDestination.
    emu:write8(warp.S_WARP_DESTINATION + 0, map_group)
    emu:write8(warp.S_WARP_DESTINATION + 1, map_num)
    emu:write8(warp.S_WARP_DESTINATION + 2, warp_id)
    emu:write16(warp.S_WARP_DESTINATION + 4, 0)  -- x (warpId overrides)
    emu:write16(warp.S_WARP_DESTINATION + 6, 0)  -- y (warpId overrides)

    -- 2. Apply to SaveBlock1->location (ApplyCurrentWarp's job).
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    emu:write8(sb1 + ADDR.SB1_LOC_MAP_GROUP, map_group)
    emu:write8(sb1 + ADDR.SB1_LOC_MAP_NUM,   map_num)
    emu:write8(sb1 + 0x06,                   warp_id)  -- location.warpId
    emu:write16(sb1 + ADDR.SB1_LOC_X, 0)
    emu:write16(sb1 + ADDR.SB1_LOC_Y, 0)

    -- 3. Swap main callback to CB2_LoadMap (+thumb bit).
    emu:write32(warp.G_MAIN_CB2, warp.CB2_LOAD_MAP + 1)

    -- 4. Wait for SaveBlock1->location to match the target. It already
    -- does (we wrote it), so the more meaningful check is "a new map
    -- has actually loaded" — we approximate that by waiting a fixed
    -- post-swap window, long enough for CB2_LoadMap's chain to finish.
    -- 180 frames (~3s) covers normal loads with headroom.
    fw.wait_frames(180)

    local after = warp.current_map_packed()
    if after ~= target_packed then
        fw.log_error(string.format(
            "[WARP] expected %d.%d but ended up on %d.%d",
            map_group, map_num,
            math.floor(after / 256), after % 256))
        return false
    end

    -- Extra settle window so object events spawn + scripts run before
    -- the caller starts reading state.
    fw.wait_frames(60)
    return true
end

-- Warp to a specific map tile position, bypassing the warp table.
-- Uses WARP_ID_NONE (0xFF) so the engine places the player at (x, y) directly
-- rather than looking up a warp entry. x and y are raw map tile coordinates
-- (the engine adds the 7-tile border offset internally).
function warp.warp_to_pos(map_group, map_num, x, y)
    local target_packed = map_group * 256 + map_num
    local before = warp.current_map_packed()
    fw.log(string.format("[WARP] %d.%d -> %d.%d (pos %d,%d)",
        math.floor(before / 256), before % 256,
        map_group, map_num, x, y))

    local WARP_ID_NONE = 0xFF

    -- 1. Write sWarpDestination with explicit position.
    emu:write8(warp.S_WARP_DESTINATION + 0, map_group)
    emu:write8(warp.S_WARP_DESTINATION + 1, map_num)
    emu:write8(warp.S_WARP_DESTINATION + 2, WARP_ID_NONE)
    emu:write16(warp.S_WARP_DESTINATION + 4, x)
    emu:write16(warp.S_WARP_DESTINATION + 6, y)

    -- 2. Apply to SaveBlock1->location.
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    emu:write8(sb1 + ADDR.SB1_LOC_MAP_GROUP, map_group)
    emu:write8(sb1 + ADDR.SB1_LOC_MAP_NUM,   map_num)
    emu:write8(sb1 + 0x06,                   WARP_ID_NONE)
    emu:write16(sb1 + ADDR.SB1_LOC_X, x)
    emu:write16(sb1 + ADDR.SB1_LOC_Y, y)

    -- 3. Trigger map load.
    emu:write32(warp.G_MAIN_CB2, warp.CB2_LOAD_MAP + 1)

    fw.wait_frames(180)

    local after = warp.current_map_packed()
    if after ~= target_packed then
        fw.log_error(string.format(
            "[WARP] expected %d.%d but ended up on %d.%d",
            map_group, map_num,
            math.floor(after / 256), after % 256))
        return false
    end

    fw.wait_frames(60)
    return true
end

return warp
