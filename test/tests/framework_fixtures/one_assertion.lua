-------------------------------------------------------------------------------
-- one_assertion.lua: a test that proves one thing (fixture for
-- test/tests/test_framework_rules.lua, the control case R0)
-------------------------------------------------------------------------------
-- One passing assertion, so test/run_test.sh must report a pass. This keeps
-- the other fixtures honest: if this one failed too, their failures would
-- prove nothing about the rules they check.
--
-- Like every file in this folder, it is run by test_framework_rules.lua, not
-- by whole-suite runs (which only run the top-level test/tests/*.lua files).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "../../lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.assert_eq(1 + 1, 2, "one plus one is two")
    fw.finish()
end)
