-------------------------------------------------------------------------------
-- probe_route1_fixture.lua
-------------------------------------------------------------------------------
-- Load /tmp/pokefirered-route1-grass.ss and probe each direction around the
-- player one tile at a time, logging metatile behavior. Used to pick the
-- right walk direction for test_shiny_chaining.lua Tests E and F.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local FIXTURE = "/tmp/pokefirered-route1-grass.ss"

fw.run(function()
    fw.wait_frames(30)
    if not fw.try_load_state(FIXTURE) then
        fw.log_error("no fixture at " .. FIXTURE); fw.finish(); return
    end
    fw.wait_frames(120)

    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    local function pos()
        return fw.read16(oa + ADDR.OE_CURRENT_X),
               fw.read16(oa + ADDR.OE_CURRENT_Y),
               fw.read8(oa + 0x1E)
    end

    local sx, sy, smt = pos()
    fw.log(string.format("[probe] start pos=(%d,%d) mtb=0x%02X", sx, sy, smt))

    -- For each direction, hold briefly, log the new tile, then reload the
    -- fixture to reset before probing the next direction. This avoids
    -- "I stepped left then couldn't come back because there's a wall" false
    -- negatives.
    local dirs = { "UP", "DOWN", "LEFT", "RIGHT" }
    for _, d in ipairs(dirs) do
        fw.try_load_state(FIXTURE)
        fw.wait_frames(60)
        fw.hold(d, 24)
        fw.wait_frames(10)
        local nx, ny, nmt = pos()
        local moved = (nx ~= sx or ny ~= sy)
        fw.log(string.format("[probe] %-5s -> (%d,%d) mtb=0x%02X moved=%s grass=%s",
            d, nx, ny, nmt, tostring(moved), tostring(nmt == 0x02)))
    end

    -- Also try LEFT×4 to check if there's a non-grass tile a bit further west.
    fw.try_load_state(FIXTURE)
    fw.wait_frames(60)
    for i = 1, 4 do
        fw.hold("LEFT", 20)
        fw.wait_frames(6)
        local nx, ny, nmt = pos()
        fw.log(string.format("[probe] LEFT#%d -> (%d,%d) mtb=0x%02X", i, nx, ny, nmt))
    end

    -- And RIGHT×4.
    fw.try_load_state(FIXTURE)
    fw.wait_frames(60)
    for i = 1, 4 do
        fw.hold("RIGHT", 20)
        fw.wait_frames(6)
        local nx, ny, nmt = pos()
        fw.log(string.format("[probe] RIGHT#%d -> (%d,%d) mtb=0x%02X", i, nx, ny, nmt))
    end

    fw.finish()
end)
