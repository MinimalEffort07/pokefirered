-------------------------------------------------------------------------------
-- passes_a.lua: a test that passes (fixture for test/tests/test_run_suite.lua)
-------------------------------------------------------------------------------
-- One passing assertion. test_run_suite.lua runs it through test/run_suite.sh
-- alongside the other fixtures here, and checks that the suite reports it as
-- passed and deletes its run folder.
--
-- It sits in a subfolder so that the real suite, which runs every top-level
-- test/tests/*.lua file, doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.assert_eq(2 * 3, 6, "two times three is six")
    fw.finish()
end)
