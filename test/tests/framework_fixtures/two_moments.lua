-------------------------------------------------------------------------------
-- two_moments.lua: a test with two moments that overlap (fixture for
-- test/tests/test_recording_moments.lua, M6)
-------------------------------------------------------------------------------
-- Two 60-frame moments, the second marked 30 frames into the first, so
-- together they play 90 frames at normal speed in the mGBA window: 1.5 s
-- with the overlap counted once. Between them it tries two moments that are
-- refused (a 181-frame one and one with an empty label), which must not
-- count. test/run_test.sh's output must then hold both labels and
-- "[WATCH] 2 moments, 1.5 s at normal speed".
--
-- Run by test_recording_moments.lua; it sits in a subfolder so that running
-- every top-level test/tests/*.lua file doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

fw.run(function()
    fw.show_next("first moment: the next 60 frames")
    fw.wait_frames(30)
    pcall(fw.show_next, "refused: too long", 181)
    pcall(fw.show, "", 60)
    fw.show("second moment: 60 frames from halfway through the first")
    fw.wait_frames(1)
    fw.assert_eq(fw.speed(), "fast", "fast again once both moments are over")
    fw.finish()
end)
