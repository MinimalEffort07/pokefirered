-------------------------------------------------------------------------------
-- zero_assertions.lua: a test that checks nothing (fixture for
-- test/tests/test_framework_rules.lua, rule R1)
-------------------------------------------------------------------------------
-- It plays a few frames and finishes without a single assertion. A test like
-- this proves nothing, so test/run_test.sh must report it as a failure and
-- say "0 assertions".
--
-- Run by test_framework_rules.lua; it sits in a subfolder so that running
-- every top-level test/tests/*.lua file doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.log("played 10 frames and checked nothing")
    fw.finish()
end)
