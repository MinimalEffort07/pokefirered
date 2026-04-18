-------------------------------------------------------------------------------
-- capture_mtmoon_from_exit.lua
-------------------------------------------------------------------------------
-- After the programmatic warp into Mt Moon 1F, the player lands at the
-- bedroom's stale OE coords (13,13) which is INSIDE a wall of the new
-- cave -- explains why hold() walks did nothing in earlier captures. This
-- script reads sExitX/sExitY out of mt_moon_gen.c's .bss (known addresses
-- from pokefirered.map: sExitX=0x030028e8, sExitY=0x030028ec -- 4 bytes
-- each), writes the player's OE coords there so they're standing on the
-- exit ladder (which the procgen guarantees is reachable floor), THEN
-- screenshots and walks.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local MTM_GROUP, MTM_NUM = 1, 1
local MAP_OFFSET = 7

-- mt_moon_gen.c .bss layout (from nm on build/firered/src/mt_moon_gen.o
-- with section base 0x030028e0 per pokefirered.map):
local S_ENTRANCE_X = 0x030028e0
local S_ENTRANCE_Y = 0x030028e4
local S_EXIT_X     = 0x030028e8
local S_EXIT_Y     = 0x030028ec

local function player_pos()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y)
end

local function set_player_pos(gridX, gridY)
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    local ox = gridX + MAP_OFFSET
    local oy = gridY + MAP_OFFSET
    emu:write16(oa + ADDR.OE_CURRENT_X, ox)
    emu:write16(oa + ADDR.OE_CURRENT_Y, oy)
    emu:write16(oa + 0x14, ox)
    emu:write16(oa + 0x16, oy)
end

fw.run(function()
    fw.log("=== Mt Moon 1F capture from exit ladder ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    fw.wait_frames(60)

    -- First warp loads Mt. Moon 1F and runs GenerateMtMoonCave which sets
    -- sEntranceX/sEntranceY. warp.warp_to does NOT call WarpIntoMap, so
    -- SaveBlock1.pos stays at whatever the bedroom spawn left it
    -- (typically (6, 6)). That means DrawWholeMapView at case 10 of the
    -- map-load pipeline reads metatiles from virtual (6, 6) onwards --
    -- the top-left corner of the cave, which is border/wall tiles, not
    -- the entrance. The screenshot ends up showing only walls even
    -- though the player OE was teleported to the entrance.
    local ok = warp.warp_to(MTM_GROUP, MTM_NUM, 0)
    fw.assert_true(ok, "warp into Mt Moon 1F succeeded")
    if not ok then fw.finish() return end
    fw.wait_frames(60)

    local ex = fw.read32(S_ENTRANCE_X)
    local ey = fw.read32(S_ENTRANCE_Y)
    fw.log(string.format("procgen entrance at grid(%d,%d)", ex, ey))

    -- Patch SaveBlock1.pos to the entrance and re-trigger CB2_LoadMap.
    -- The second map-load runs DrawWholeMapView with pos = entrance, so
    -- the BG tilemap is centered on the entrance corridor instead of
    -- the warp-landing area. The procgen runs again (different random
    -- seed) but sEntrance/sExit are hardcoded, so the new cave still
    -- has the entrance at (ex, ey).
    local sb1 = emu:read32(0x03005018)
    emu:write16(sb1 + 0, ex)       -- pos.x
    emu:write16(sb1 + 2, ey)       -- pos.y
    emu:write8(sb1 + 0x06, 3)      -- location.warpId (south entrance)
    emu:write8(warp.S_WARP_DESTINATION + 2, 3)
    emu:write32(warp.G_MAIN_CB2, warp.CB2_LOAD_MAP + 1)
    fw.wait_frames(240)

    -- Re-read entrance: procgen regenerates so sEntranceX/Y could shift
    -- in future if the hardcoded values change; use the fresh ones.
    ex = fw.read32(S_ENTRANCE_X)
    ey = fw.read32(S_ENTRANCE_Y)
    set_player_pos(ex, ey)
    fw.wait_frames(30)

    local nx, ny = player_pos()
    fw.log(string.format("post-teleport player OE=(%d,%d) = grid(%d,%d)",
        nx, ny, nx - MAP_OFFSET, ny - MAP_OFFSET))

    fw.slow_down("looking at cave walls from exit")
    fw.wait_frames(90)
    fw.screenshot("/tmp/mgba-exit-0-start.png")

    -- Walk in each direction from the exit. The exit is near the north
    -- edge of the cave; walls should be directly around the ladder.
    local legs = {
        { key = "DOWN",  frames = 240, name = "south" },
        { key = "LEFT",  frames = 240, name = "west"  },
        { key = "UP",    frames = 240, name = "north" },
        { key = "RIGHT", frames = 240, name = "east"  },
    }
    for i, leg in ipairs(legs) do
        fw.hold(leg.key, leg.frames)
        fw.wait_frames(30)
        local cx, cy = player_pos()
        fw.log(string.format("after %s: OE=(%d,%d)", leg.name, cx, cy))
        fw.screenshot(string.format("/tmp/mgba-exit-%d-%s.png", i, leg.name))
    end

    fw.finish()
end)
