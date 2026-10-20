-------------------------------------------------------------------------------
-- two_moments.lua: a test with two moments that overlap (fixture for
-- test/tests/test_recording_moments.lua, M6)
-------------------------------------------------------------------------------
-- Two 60-frame moments, the second marked 30 frames into the first, so
-- together they play 90 frames at normal speed in the mGBA window: 1.5 s
-- with the overlap counted once. test/run_test.sh's output must then hold
-- both labels and "[WATCH] 2 moments, 1.5 s at normal speed". Between the
-- two it also
--   * tries every moment test_recording_moments.lua's M5 refuses, with both
--     calls: none of them may count;
--   * loads the framework again, as a test library does when it loads its
--     own copy mid-test: that must not lose the first moment or the count.
--
-- Run by test_recording_moments.lua; it sits in a subfolder so that running
-- every top-level test/tests/*.lua file doesn't pick it up.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

-- The label and frames of each refused moment (M5's).
local REFUSED = {
    { "refused", 0 }, { "refused", 181 }, { "refused", 1.5 }, { "refused", "60" },
    { nil, 60 }, { "", 60 }, { 42, 60 },
}

fw.run(function()
    fw.show_next("first moment: the next 60 frames")
    fw.wait_frames(30)
    for _, r in ipairs(REFUSED) do
        pcall(fw.show, r[1], r[2])
        pcall(fw.show_next, r[1], r[2])
    end
    dofile(project_dir .. "test/lib/framework.lua")
    fw.show("second moment: 60 frames from halfway through the first")
    fw.wait_frames(1)
    fw.assert_eq(fw.speed(), "fast", "fast again once both moments are over")
    fw.finish()
end)
