-------------------------------------------------------------------------------
-- zero_assertions.lua: a test that checks nothing (fixture for
-- test/tests/test_framework_rules.lua, rule R1)
-------------------------------------------------------------------------------
-- It plays a few frames and finishes without a single assertion. A test like
-- this proves nothing, so test/run_test.sh must report it as a failure and
-- say "0 assertions".
--
-- Like every file in this folder, it is run by test_framework_rules.lua, not
-- by whole-suite runs (which only run the top-level test/tests/*.lua files).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "../../lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.log("played 10 frames and checked nothing")
    fw.finish()
end)
