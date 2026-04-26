-------------------------------------------------------------------------------
-- test_slaveless_hm_obstacle.lua
-- Issue #5: slaveless HM system should prompt at obstacles.
--
-- Sub-tests:
--   A — slaveless Cut (HM01 in bag + badge, no party mon with Cut) → tree cut
--   B — no HM01 in bag (badge set, no party mon) → sign only, tree intact
--   C — party mon with Cut (original path) → tree cut (regression)
--
-- Run:    bash test/run_test.sh test/tests/test_slaveless_hm_obstacle.lua
-- Record: bash test/record_test.sh test/tests/test_slaveless_hm_obstacle.lua
-------------------------------------------------------------------------------
local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.log("=== Test: Slaveless HM Obstacle (issue #5) ===")
    fw.assert_true(false, "stub — not yet implemented")
    fw.finish()
end)
