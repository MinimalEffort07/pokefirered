-------------------------------------------------------------------------------
-- test_fast_forward_gui.lua: moments really play at normal speed in the
-- mGBA window (issue #119, #124)
-------------------------------------------------------------------------------
-- In the mGBA window a test fast-forwards, except at its moments, which play
-- at normal speed (MOMENTS in test/lib/framework.lua). The headless test
-- test/tests/test_recording_moments.lua checks what fw.speed() says frame by
-- frame; only the window has a real speed to measure, so this test times
-- frames there, by the clock:
--
--   G0  The 1,500 frames before any moment take under 5 s: the framework
--       turned fast-forward on at the start. (At normal speed they'd take
--       25 s; fast-forward does them in about 1 s.)
--   G1  A 60-frame fw.show lasts 0.8 to 2.5 s (measured: 1.2 s, a second at
--       normal speed plus the emulator settling after fast-forward).
--   G2  The 1,500 frames after it take under 5 s: fast-forward came back by
--       itself.
--   G3  A fw.show made right after the test started a process (io.popen) in
--       the same frame also lasts 0.8 to 2.5 s. mGBA ignores a speed change
--       the test makes itself after an io.popen in the same frame; the
--       framework makes it from its frame callback instead, and G3 checks
--       that this still holds.
--   G4  fw.show_next(label, 60): those 60 frames take 0.8 to 2.5 s while the
--       test waits frame by frame, and the 1,500 frames after them take
--       under 5 s.
--
-- THE CLOCK: `date +%s.%N` (seconds since 1970, to the nanosecond), run
-- through io.popen. Not os.clock(): that counts the CPU time this process
-- has used, not the time that passes, so a frame spent waiting at normal
-- speed barely moves it. (Timing by os.clock() is why this test failed in
-- the window before #124.) Each reading starts a process, which is exactly
-- what G3 needs anyway.
--
-- HOW TO RUN: only in the window, which test/record_test.sh is the way to
-- reach:
--   bash test/record_test.sh test/tests/test_fast_forward_gui.lua
-- Headless (bash test/run_test.sh ...) it has no window to time, so it
-- skips itself and passes with no assertions; test/suite_exclude.txt
-- leaves it out of the suite.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local is_gui = (canvas ~= nil)

-- The pass marks (see G0 to G4 above). The margins allow for a busy machine.
local FAST_FRAMES = 1500     -- 25 s at normal speed
local FAST_MAX_SECONDS = 5
local MOMENT_MIN_SECONDS = 0.8
local MOMENT_MAX_SECONDS = 2.5

-- The time on the clock, in seconds.
local function now()
    local pipe = io.popen("date +%s.%N")
    local t = tonumber(pipe:read("l"))
    pipe:close()
    return t
end

-- Seconds fn() takes on the clock.
local function timed(fn)
    local start = now()
    fn()
    return now() - start
end

-- Check that `seconds` is a moment's length (G1, G3, G4).
local function check_moment(name, seconds)
    fw.assert_true(seconds >= MOMENT_MIN_SECONDS and seconds <= MOMENT_MAX_SECONDS,
        string.format("%s lasts %.1f to %.1f s (took %.2f s)",
            name, MOMENT_MIN_SECONDS, MOMENT_MAX_SECONDS, seconds))
end

-- Check that `seconds` is FAST_FRAMES frames at fast-forward (G0, G2, G4).
local function check_fast(name, seconds)
    fw.assert_true(seconds < FAST_MAX_SECONDS,
        string.format("%s: %d frames take under %d s (took %.2f s; 25 s at normal speed)",
            name, FAST_FRAMES, FAST_MAX_SECONDS, seconds))
end

fw.run(function()
    fw.log("=== Moments in the mGBA window (#124) ===")

    if not is_gui then
        -- Headless there is no window and no real speed to time. Skip
        -- cleanly instead of reporting a spurious failure: with no
        -- assertions, which only this skip declares allowed (in the window
        -- the test must still assert).
        fw.log("Skipped: this test only makes sense in GUI mode (mgba-qt)")
        fw.allow_no_assertions("GUI-only test: skipped under mgba-headless")
        -- Brief wait so stdout flushes before clean_shutdown SIGTERMs us;
        -- without this the skip completes in microseconds and the kill
        -- races ahead of the log/PASS lines reaching the terminal.
        fw.wait_frames(30)
        fw.finish()
        return
    end

    -- Get past the title animation so the emulator is fully running.
    fw.wait_frames(180)

    check_fast("G0 before any moment",
        timed(function() fw.wait_frames(FAST_FRAMES) end))

    -- G1: one frame first, so no process ran in the frame the moment is
    -- marked in (that is G3).
    local start = now()
    fw.yield()
    fw.show("G1 a 60-frame moment (fw.show)")
    check_moment("G1 a 60-frame fw.show", now() - start)

    check_fast("G2 after the moment",
        timed(function() fw.wait_frames(FAST_FRAMES) end))

    -- G3: a process started in the very frame the moment is marked in.
    start = now()
    local pipe = io.popen("true")
    pipe:close()
    fw.show("G3 a 60-frame moment right after io.popen")
    check_moment("G3 a 60-frame fw.show right after io.popen", now() - start)
    fw.wait_frames(120)  -- back to fast-forward before G4

    -- G4: the moment plays while the test waits frame by frame.
    check_moment("G4 the 60 frames of fw.show_next", timed(function()
        fw.show_next("G4 the next 60 frames (fw.show_next)", 60)
        for _ = 1, 60 do
            fw.yield()
        end
    end))
    check_fast("G4 after the moment",
        timed(function() fw.wait_frames(FAST_FRAMES) end))

    fw.finish()
end)
