-------------------------------------------------------------------------------
-- boot_fails.lua: a test whose boot to character select fails (fixture for
-- test/tests/test_framework_rules.lua, rule R3)
-------------------------------------------------------------------------------
-- Shaped like the many tests that start with
--
--   local sel = cs.find_selection_press()
--   if not sel then fw.finish() return end
--
-- test_framework_rules.lua runs it with MGBA_TEST_FAKE_BOOT_FAILURE=1, which
-- makes cs.find_selection_press() fail at once instead of booting (see
-- test/lib/character_select.lua). The early return then reaches fw.finish()
-- before any assertion of the test's own, and test/run_test.sh must still
-- report a failure that says "boot to character select failed".
--
-- Like every file in this folder, it is run by test_framework_rules.lua, not
-- by whole-suite runs (which only run the top-level test/tests/*.lua files).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "../../lib/framework.lua")
local cs = dofile(script_dir .. "../../lib/character_select.lua")

fw.run(function()
    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    fw.assert_true(true, "booted to character select")
    fw.finish()
end)
