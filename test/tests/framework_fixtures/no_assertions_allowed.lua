-------------------------------------------------------------------------------
-- no_assertions_allowed.lua: a demo that declares it asserts nothing
-- (fixture for test/tests/test_framework_rules.lua, rule R2)
-------------------------------------------------------------------------------
-- Demos and probes only produce frames for a recording, so they make no
-- assertions. They say so with fw.allow_no_assertions(reason), and then
-- test/run_test.sh must report a pass even though nothing was checked.
--
-- Like every file in this folder, it is run by test_framework_rules.lua, not
-- by whole-suite runs (which only run the top-level test/tests/*.lua files).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "../../lib/framework.lua")
fw.allow_no_assertions("fixture: stands in for a demo that only makes frames")

fw.run(function()
    fw.wait_frames(10)
    fw.log("played 10 frames and checked nothing, as declared")
    fw.finish()
end)
