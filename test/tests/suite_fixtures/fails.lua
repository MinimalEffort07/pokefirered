-------------------------------------------------------------------------------
-- fails.lua: a test that fails (fixture for test/tests/test_run_suite.lua)
-------------------------------------------------------------------------------
-- One assertion, and it is false on purpose. test_run_suite.lua runs it
-- through test/run_suite.sh in two ways:
--   - listed in exclude_fails.txt (check A4): the suite must leave it out, so
--     the suite still passes. If it ran, the suite would fail.
--   - next to two passing fixtures (check A5): the suite must exit non-zero,
--     name it, and keep its run folder.
--
-- It sits in a subfolder so that the real suite, which runs every top-level
-- test/tests/*.lua file, doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.assert_eq(1 + 1, 3, "false on purpose: one plus one is three")
    fw.finish()
end)
