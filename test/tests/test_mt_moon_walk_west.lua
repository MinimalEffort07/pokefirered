-------------------------------------------------------------------------------
-- test_mt_moon_walk_west.lua: S2, walk Mt. Moon from Route 4 east to west
-------------------------------------------------------------------------------
--
-- The reverse of S1 (test_mt_moon_walk_east.lua), again a real walk:
--
--   Route 4 (37,6) -> east door (32,5) -> B2F exit up-ladder
--   B2F up-ladder -> B1F down-ladder
--   B1F up-ladder -> 1F down-ladder
--   1F door (18,37), pressing DOWN -> Route 4 west door, warp 0 (19,5)
--
-- Same setup as S1 (test/lib/mt_moon_chain.lua).
--
-- PASS: after each of the 4 warps the map is the next one in the chain and
-- the player stands on the expected arrival warp; the run ends on Route 4
-- at warp 0 (19,5).
--
-- Run:
--   MGBA_TEST_TIMEOUT=300 bash test/run_test.sh test/tests/test_mt_moon_walk_west.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

fw.run(function()
    fw.log("=== Mt. Moon: walk from Route 4 east to Route 4 west (S2) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    capture.start()
    chain.walk_chain("west", fw, capture)
    fw.wait_frames(60)
    capture.stop()
    fw.finish()
end)
