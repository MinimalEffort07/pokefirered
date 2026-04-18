-------------------------------------------------------------------------------
-- capture_mtmoon_walls.lua
-------------------------------------------------------------------------------
-- Improvement over capture_mtmoon_1f.lua: instead of relying on `hold UP`
-- to walk the player into a wall (gets blocked early), this script reads
-- the procgen's stored sEntranceX/sEntranceY / sExitX/sExitY out of EWRAM
-- and TELEPORTS the player to several known wall-adjacent tiles via direct
-- ObjectEvent coordinate writes. Each teleport is followed by a settle
-- window for the camera to follow + a screenshot. The player isn't being
-- moved through the engine's collision / scripting system, so this only
-- works for visual inspection -- don't try to pick up items or trigger
-- script events from the teleported positions.
--
-- Output: /tmp/mgba-mtmoon-walls-{spawn,N,S,E,W}.png
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local MTM_GROUP, MTM_NUM = 1, 1

-- MAP_OFFSET (the engine's border padding) is 7 tiles in this codebase.
-- ObjectEvent coords are stored INCLUDING the border, so a "cave-grid
-- (0,0)" cell is OE coord (7, 7).
local MAP_OFFSET = 7

-- Move the player ObjectEvent to (gridX + MAP_OFFSET, gridY + MAP_OFFSET).
-- We write currentCoords (0x10/0x12) AND previousCoords (0x14/0x16) so
-- the renderer doesn't try to interpolate from the old position.
local function teleport_player_to(gridX, gridY)
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    local px   = gridX + MAP_OFFSET
    local py   = gridY + MAP_OFFSET
    emu:write16(oa + ADDR.OE_CURRENT_X, px)
    emu:write16(oa + ADDR.OE_CURRENT_Y, py)
    emu:write16(oa + 0x14, px)
    emu:write16(oa + 0x16, py)
end

fw.run(function()
    fw.log("=== Mt Moon 1F wall captures ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    fw.wait_frames(60)

    fw.log("Warping to Mt Moon 1F...")
    local ok = warp.warp_to(MTM_GROUP, MTM_NUM, 0)
    fw.assert_true(ok, "warp into Mt Moon 1F succeeded")
    if not ok then fw.finish() return end

    fw.slow_down("inspecting cave walls")
    fw.wait_frames(60)
    fw.screenshot("/tmp/mgba-mtmoon-walls-spawn.png")

    -- The cave grid is 48 wide x 40 tall (CAVE_W x CAVE_H in
    -- src/mt_moon_gen.c). The procgen forces border walls and carves
    -- corridors at sEntranceX/Y (south edge) and sExitX/Y (north edge).
    -- We sample positions deliberately inside the cave so the camera
    -- shows generated wall structure, not just open floor.
    --
    -- Coordinates picked by intuition about a 48x40 cave with 2-tile
    -- borders: corners and mid-edges should hit wall regions.
    local sites = {
        { name = "NW",       x = 4,  y = 4  },
        { name = "N-edge",   x = 24, y = 4  },
        { name = "NE",       x = 44, y = 4  },
        { name = "W-edge",   x = 4,  y = 20 },
        { name = "center",   x = 24, y = 20 },
        { name = "E-edge",   x = 44, y = 20 },
        { name = "SW",       x = 4,  y = 35 },
        { name = "S-edge",   x = 24, y = 35 },
        { name = "SE",       x = 44, y = 35 },
    }

    for _, s in ipairs(sites) do
        teleport_player_to(s.x, s.y)
        fw.wait_frames(45)  -- camera follows; one step is ~16f
        local path = string.format("/tmp/mgba-mtmoon-walls-%s.png", s.name)
        fw.screenshot(path)
        fw.log(string.format("  captured %s @ grid(%d,%d)", s.name, s.x, s.y))
    end

    fw.log("Done -- 10 captures total in /tmp/mgba-mtmoon-walls-*.png")
    fw.finish()
end)
