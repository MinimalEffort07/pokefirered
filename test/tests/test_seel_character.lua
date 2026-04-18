-------------------------------------------------------------------------------
-- test_seel_character.lua
-------------------------------------------------------------------------------
-- Exercises the full character-selection pipeline for a single character
-- (SEEL by default) so individual runs are quick and diagnostic. All the
-- boot → sentinel → scroll → naming → overworld → walk plumbing lives in
-- test/lib/character_select.lua, so this file is just one call.
--
-- HOW TO RUN:
--   Headless:  bash test/run_test.sh test/tests/test_seel_character.lua
--   GUI:       Tools > Scripting > Load script
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

fw.run(function()
    fw.log("=== Test: Select Seel Character ===")
    local r = cs.test_character(94, 126, "SEEL")
    fw.assert_true(r.passed, "SEEL playable end-to-end")
    if not r.passed then
        fw.log_error("SEEL failure reasons: " .. table.concat(r.reasons, "; "))
    end
    fw.finish()
end)
