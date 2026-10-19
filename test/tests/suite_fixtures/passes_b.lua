-------------------------------------------------------------------------------
-- passes_b.lua: a second test that passes (fixture for
-- test/tests/test_run_suite.lua)
-------------------------------------------------------------------------------
-- Like passes_a.lua. Check A5 runs both next to fails.lua, so the suite has
-- more than one passing run whose folder it must delete.
--
-- It sits in a subfolder so that the real suite, which runs every top-level
-- test/tests/*.lua file, doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.wait_frames(10)
    fw.assert_eq(10 - 4, 6, "ten minus four is six")
    fw.finish()
end)
