-------------------------------------------------------------------------------
-- test_mt_moon_walk_east.lua: S1, walk Mt. Moon from Route 4 west to east
-------------------------------------------------------------------------------
--
-- A real walk (a path planner picks each step, the test presses the D-pad)
-- through the whole Mt. Moon chain:
--
--   Route 4 (13,8) -> west door (19,5) -> 1F door (18,37)
--   1F down-ladder -> B1F up-ladder
--   B1F down-ladder -> B2F up-ladder
--   B2F exit up-ladder -> Route 4 east door, warp 1 (32,5)
--
-- with every Mt. Moon trainer marked defeated, Miguel's B2F trigger off and
-- no wild battles (see test/lib/mt_moon_chain.lua for the setup and the
-- exact pass/fail rules).
--
-- PASS: after each of the 4 warps the map is the next one in the chain and
-- the player stands on the expected arrival warp; the run ends on Route 4
-- at warp 1 (32,5).
--
-- Run:
--   MGBA_TEST_TIMEOUT=300 bash test/run_test.sh test/tests/test_mt_moon_walk_east.lua
-- Record a real-time video from frame dumps (see test/lib/frame_capture.lua):
--   MGBA_FRAME_DIR=/tmp/s1_frames MGBA_TEST_TIMEOUT=600 bash test/run_test.sh \
--       test/tests/test_mt_moon_walk_east.lua
--   ffmpeg -framerate 30 -i /tmp/s1_frames/frame_%05d.png \
--       -c:v libx264 -pix_fmt yuv420p s1.mp4
-- (bash test/record_test.sh also works, but runs the walk at fast-forward.)
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

fw.run(function()
    fw.log("=== Mt. Moon: walk from Route 4 west to Route 4 east (S1) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    capture.start()
    chain.walk_chain("east", fw, capture)
    fw.wait_frames(60)
    capture.stop()
    fw.finish()
end)
