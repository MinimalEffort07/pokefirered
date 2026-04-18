-------------------------------------------------------------------------------
-- capture_mtmoon_1f.lua
-------------------------------------------------------------------------------
-- Boots through Oak's speech, programmatically warps to Mt Moon 1F via
-- the south entrance, then walks the player around to several locations
-- inside the cave and screenshots each. Used to verify the wall/corner
-- rendering after the wall-tile fix in src/mt_moon_gen.c.
--
-- Output: /tmp/mgba-mtmoon-spawn.png
--         /tmp/mgba-mtmoon-corner-N.png  (walked north)
--         /tmp/mgba-mtmoon-corner-E.png  (walked east)
--         /tmp/mgba-mtmoon-corner-W.png  (walked west)
--
-- HOW TO COMPARE AGAINST BASELINE:
--   The pre-Claude baseline ROM is at /tmp/pokefirered-baseline.gba (built
--   from commit 7e3f8226). It has the ORIGINAL hand-authored Mt Moon 1F
--   layout. Open that ROM in mgba-qt and walk to Mt Moon 1F to see how
--   the rim-wall set is supposed to look. The procedural cave should now
--   use the same 9-tile rim-wall set (0x0688-0x069a) so corners join
--   cleanly instead of mixing cliff-face + rim-corner styles.
--
-- HOW TO RUN:
--   mgba-qt already open with the CURRENT ROM ->
--     Tools > Scripting > File > Load script > this file
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

-- MAP_MT_MOON_1F = (1 | (1 << 8))  ->  mapGroup=1, mapNum=1.
local MTM_GROUP = 1
local MTM_NUM   = 1

fw.run(function()
    fw.log("=== Mt Moon 1F visual capture ===")

    fw.log("Boot through Oak's speech...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log_error("Could not discover Oak speech press; aborting.")
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    fw.wait_frames(60)

    fw.log("Warping to Mt Moon 1F...")
    local ok = warp.warp_to(MTM_GROUP, MTM_NUM, 0)
    fw.assert_true(ok, "warp into Mt Moon 1F succeeded")
    if not ok then fw.finish() return end

    -- Drop to real-time so the GUI viewer sees the cave layout.
    fw.slow_down("inspecting Mt Moon 1F walls")
    fw.wait_frames(120)
    fw.screenshot("/tmp/mgba-mtmoon-spawn.png")

    -- Walk around to expose different sections of the cave -- corners
    -- and walls in each cardinal direction. Each leg ends with a brief
    -- settle so the camera catches up before screenshotting.
    fw.log("Walking around to expose wall/corner views...")
    fw.hold("UP", 60); fw.wait_frames(30)
    fw.hold("UP", 60); fw.wait_frames(30)
    fw.screenshot("/tmp/mgba-mtmoon-corner-N.png")

    fw.hold("RIGHT", 60); fw.wait_frames(30)
    fw.hold("RIGHT", 60); fw.wait_frames(30)
    fw.screenshot("/tmp/mgba-mtmoon-corner-E.png")

    fw.hold("LEFT", 120); fw.wait_frames(30)
    fw.hold("LEFT", 120); fw.wait_frames(30)
    fw.screenshot("/tmp/mgba-mtmoon-corner-W.png")

    fw.log("Captures complete -- inspect /tmp/mgba-mtmoon-*.png")
    fw.finish()
end)
