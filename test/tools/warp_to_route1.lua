-------------------------------------------------------------------------------
-- warp_to_route1.lua
-------------------------------------------------------------------------------
-- Programmatically boots through character select and warps the player to
-- Route 1, then saves a mGBA state file to /tmp/pokefirered-route1.ss for
-- use by test/tests/test_roaming_route1.lua.
--
-- This replaces manual "walk Oak's speech -> leave house -> walk north"
-- setup. The heavy lifting (writing sWarpDestination + swapping
-- gMain.callback2) lives in test/lib/warp.lua and is shared with the
-- multi-map showcase test.
--
-- HOW TO RUN:
--   mgba-qt already open -> Tools > Scripting > File > Open this script.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local STATE_PATH = "/tmp/pokefirered-route1.ss"

local ROUTE1_MAP_GROUP = 3
local ROUTE1_MAP_NUM   = 19

fw.run(function()
    fw.log("=== Warp to Route 1 (programmatic) ===")
    fw.slow_down("watch the boot + warp happen")

    fw.log("Booting through Oak's speech...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log_error("could not discover Oak-speech selection press; aborting")
        fw.finish()
        return
    end

    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached the overworld after boot")
    fw.wait_frames(60)

    local ok = warp.warp_to(ROUTE1_MAP_GROUP, ROUTE1_MAP_NUM, 0)
    fw.assert_true(ok, "warp to Route 1 completed")

    if ok then
        fw.save_state(STATE_PATH)
        fw.log("Route 1 state saved. You can now run test_roaming_route1.lua.")
    end

    fw.finish()
end)
