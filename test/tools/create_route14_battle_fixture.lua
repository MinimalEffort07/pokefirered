-------------------------------------------------------------------------------
-- create_route14_battle_fixture.lua
-------------------------------------------------------------------------------
-- Saves a state inside the Kiri & Jan double battle on Route 14, with the
-- move-selection screen open on turn 1 and the lead Pikachu knowing Water Gun
-- (move slot 0) and Vine Whip (move slot 1). Handy for poking at the
-- double-battle effectiveness colours (issue #7) by hand in mgba-qt.
--
-- The tests don't need it: test_type_effectiveness_colors.lua and its demo
-- build the same battle themselves every run (test/lib/route14_battle.lua
-- explains why). A saved state only works with the ROM build that saved it,
-- so re-run this after every rebuild.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tools/create_route14_battle_fixture.lua
-- The state is written to pokefirered-route14-double.ss in the run's own
-- output folder, /tmp/mgba-test.XXXXXX (fw.out_path; run_test.sh prints the
-- folder and keeps it after the run, until /tmp is cleared on reboot). A
-- private folder means two runs at once can't overwrite each other's state.
-- Load it in mgba-qt with File > Load State File.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw  = dofile(project_dir .. "test/lib/framework.lua")
local r14 = dofile(project_dir .. "test/lib/route14_battle.lua")

local FIXTURE_PATH = fw.out_path("pokefirered-route14-double.ss")

fw.run(function()
    fw.log("=== create_route14_battle_fixture.lua ===")
    if fw.assert_true(r14.reach_move_selection(),
            "reached move selection in the Kiri & Jan double battle") then
        fw.screenshot(fw.out_path("mgba-fixture-presave.png"))
        fw.assert_true(fw.save_state(FIXTURE_PATH), "fixture saved to " .. FIXTURE_PATH)
    end
    fw.finish()
end)
