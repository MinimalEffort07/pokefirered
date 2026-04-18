-------------------------------------------------------------------------------
-- capture_mtmoon_walk.lua
-------------------------------------------------------------------------------
-- Aggressively walks the player around Mt Moon 1F to get walls in frame,
-- logging the player's grid coords after each leg so we know the walk is
-- actually doing something. Screenshots at multiple points.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local MTM_GROUP, MTM_NUM = 1, 1

local function player_pos()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y)
end

fw.run(function()
    fw.log("=== Mt Moon 1F walk capture ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    fw.wait_frames(60)

    local ok = warp.warp_to(MTM_GROUP, MTM_NUM, 0)
    fw.assert_true(ok, "warp into Mt Moon 1F succeeded")
    if not ok then fw.finish() return end

    fw.slow_down("watching cave exploration")
    fw.wait_frames(60)

    local px, py = player_pos()
    fw.log(string.format("spawn pos: (%d, %d)", px, py))
    fw.screenshot("/tmp/mgba-walk-0-spawn.png")

    -- Walk aggressively in each direction. 300 frames (5s at real-time,
    -- but this runs at fast-forward ~8x so much quicker) should cover
    -- ~18 tiles even with collisions. Log position after each leg; if
    -- the coords are unchanged, the walk is stuck in place.
    local legs = {
        { key = "DOWN",  frames = 300, name = "south" },
        { key = "LEFT",  frames = 300, name = "west"  },
        { key = "UP",    frames = 600, name = "north-from-sw" },
        { key = "RIGHT", frames = 600, name = "east-from-nw"  },
        { key = "DOWN",  frames = 300, name = "south-from-ne" },
    }
    for i, leg in ipairs(legs) do
        fw.hold(leg.key, leg.frames)
        fw.wait_frames(20)
        local nx, ny = player_pos()
        fw.log(string.format("after %s: pos=(%d,%d)", leg.name, nx, ny))
        fw.screenshot(string.format("/tmp/mgba-walk-%d-%s.png", i, leg.name))
    end

    fw.finish()
end)
