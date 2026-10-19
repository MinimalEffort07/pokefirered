-------------------------------------------------------------------------------
-- boot_fails_in_demo.lua: a demo whose boot to character select fails
-- (fixture for test/tests/test_framework_rules.lua, rule R4)
-------------------------------------------------------------------------------
-- Like boot_fails.lua, but declared a demo with fw.allow_no_assertions. The
-- opt-out only lifts the zero-assertion rule, so with
-- MGBA_TEST_FAKE_BOOT_FAILURE=1 test/run_test.sh must still report a failure
-- that says "boot to character select failed": a demo that never got past
-- the boot recorded nothing worth keeping.
--
-- Run by test_framework_rules.lua; it sits in a subfolder so that running
-- every top-level test/tests/*.lua file doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
fw.allow_no_assertions("fixture: stands in for a demo that only makes frames")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

fw.run(function()
    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    fw.log("booted to character select")
    fw.finish()
end)
