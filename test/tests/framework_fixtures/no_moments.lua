-------------------------------------------------------------------------------
-- no_moments.lua: a test that marks no moments (fixture for
-- test/tests/test_recording_moments.lua, M7)
-------------------------------------------------------------------------------
-- It plays some frames and marks no moment, so in the mGBA window it would
-- fast-forward from start to end, and test/run_test.sh's output must say
-- "[WATCH] no moments: in the mGBA window this test fast-forwards
-- throughout".
--
-- Run by test_recording_moments.lua; it sits in a subfolder so that running
-- every top-level test/tests/*.lua file doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.assert_eq(fw.speed(), "fast", "with no moments the speed is fast")
    fw.finish()
end)
