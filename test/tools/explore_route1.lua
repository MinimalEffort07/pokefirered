-------------------------------------------------------------------------------
-- explore_route1.lua
-------------------------------------------------------------------------------
-- Load /tmp/pokefirered-route1.ss and randomly-walk the player around,
-- looking for a tall-grass tile. Logs each step so we can reconstruct the
-- path to grass and hard-code it in the fixture builder.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH = "/tmp/pokefirered-route1.ss"

fw.run(function()
    fw.speed_up()
    fw.wait_frames(30)
    if not fw.try_load_state(STATE_PATH) then
        fw.log_error("no state"); fw.finish(); return
    end
    fw.wait_frames(120)

    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE

    local function pos()
        return fw.read16(oa + ADDR.OE_CURRENT_X),
               fw.read16(oa + ADDR.OE_CURRENT_Y),
               fw.read8(oa + 0x1E)
    end

    -- Try a systematic path. Route 1 goes south->north with tall grass in
    -- the middle. The saved state has player at (13,13). Try moving right
    -- (toward Boy NPC's wander range near the grass) and up.
    -- y=13 is the top walkable row in this save; can only go south.
    -- Try DOWN first, then sweep for grass.
    local path = {
        {"DOWN", 6},  {"RIGHT", 6},
        {"DOWN", 6},  {"LEFT", 10},
        {"DOWN", 6},  {"RIGHT", 10},
        {"DOWN", 6},
    }
    for _, step in ipairs(path) do
        local dir, count = step[1], step[2]
        for i = 1, count do
            fw.hold(dir, 20)
            fw.wait_frames(8)
            local x, y, mtb = pos()
            fw.log(string.format("step %s#%d pos=(%d,%d) mtb=0x%02X",
                dir, i, x, y, mtb))
            if mtb == 0x02 then
                fw.log(">>> REACHED GRASS <<<")
                fw.save_state("/tmp/pokefirered-route1-grass.ss")
                fw.wait_frames(60)
                fw.finish()
                return
            end
        end
    end
    fw.log("path completed without finding grass")
    fw.finish()
end)
