-------------------------------------------------------------------------------
-- no_assertions_allowed.lua: a demo that declares it asserts nothing
-- (fixture for test/tests/test_framework_rules.lua, rule R2)
-------------------------------------------------------------------------------
-- Demos and probes only produce frames for a recording, so they make no
-- assertions. They say so with fw.allow_no_assertions(reason), and then
-- test/run_test.sh must report a pass even though nothing was checked.
--
-- Run by test_framework_rules.lua; it sits in a subfolder so that running
-- every top-level test/tests/*.lua file doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
fw.allow_no_assertions("fixture: stands in for a demo that only makes frames")

fw.run(function()
    fw.wait_frames(10)
    fw.log("played 10 frames and checked nothing, as declared")
    fw.finish()
end)
