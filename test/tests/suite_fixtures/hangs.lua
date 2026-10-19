-------------------------------------------------------------------------------
-- hangs.lua: a test that never finishes (fixture for
-- test/tests/test_run_suite.lua)
-------------------------------------------------------------------------------
-- It plays frames forever and never calls fw.finish(), so it only ends when
-- test/run_test.sh's time limit stops the emulator. test_run_suite.lua runs
-- it through test/run_suite.sh with a 5-second limit and checks that the
-- suite reports it as TIMEOUT, not as an ordinary failure.
--
-- It sits in a subfolder so that the real suite, which runs every top-level
-- test/tests/*.lua file, doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    while true do
        fw.wait_frames(60)
    end
end)
