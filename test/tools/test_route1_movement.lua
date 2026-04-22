-------------------------------------------------------------------------------
-- test_route1_movement.lua
-------------------------------------------------------------------------------
-- Dumb probe: load the existing /tmp/pokefirered-route1.ss, then try walking
-- in every direction. Diagnoses whether the movement-stuck issue is inherent
-- to the warp state or fixable by the clear we already do.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")

local STATE_PATH = "/tmp/pokefirered-route1.ss"

fw.run(function()
    fw.speed_up()
    fw.wait_frames(30)

    if not fw.try_load_state(STATE_PATH) then
        fw.log_error("no state at " .. STATE_PATH)
        fw.finish(); return
    end
    fw.wait_frames(120)

    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    local function dump(tag)
        local x = fw.read16(oa + ADDR.OE_CURRENT_X)
        local y = fw.read16(oa + ADDR.OE_CURRENT_Y)
        local mtb = fw.read8(oa + 0x1E)
        local paf = fw.read8(ADDR.gPlayerAvatar + 0x00)
        local tts = fw.read8(ADDR.gPlayerAvatar + 0x03)
        local rs  = fw.read8(ADDR.gPlayerAvatar + 0x02)
        local obits = fw.read32(oa + 0x00)
        local slock = fw.read8(0x03000f9c)
        fw.log(string.format("[%s] pos=(%d,%d) mtb=0x%02X pa.f=0x%02X tts=%d rs=%d oe=0x%08X slock=%d",
            tag, x, y, mtb, paf, tts, rs, obits, slock))
    end

    dump("post-load")

    for _, dir in ipairs({"UP","DOWN","LEFT","RIGHT"}) do
        fw.hold(dir, 24)
        fw.wait_frames(16)
        dump("after-" .. dir)
    end

    fw.finish()
end)
