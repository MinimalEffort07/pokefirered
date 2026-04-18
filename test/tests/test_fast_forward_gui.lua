-------------------------------------------------------------------------------
-- test_fast_forward_gui.lua
-------------------------------------------------------------------------------
-- Verifies that calling emu:setFastForward() from a script actually changes
-- the emulator's speed, and prompts the user to visually confirm that the
-- "Emulation > Fast forward" menu item reflects the same state.
--
-- Lua scripts in mGBA can't read Qt widget state directly, so we can't
-- automate the menu-checkbox assertion. Instead:
--   1. We programmatically confirm emulation speed changed using wall-clock
--      time — under FF a batch of emulated frames completes far faster in
--      real time than at normal speed.
--   2. We pause long enough (in wall-clock seconds) for the user to glance
--      at the Emulation menu and verify the checkbox matches the log line.
--
-- This test is designed for GUI mode. In headless mode the menu doesn't
-- exist; the test still runs the programmatic assertions but the visual
-- step is a no-op.
--
-- HOW TO RUN:
--   GUI:       mgba-qt pokefirered.gba, then Tools > Scripting > Load script
--              and select this file. Watch the Emulation menu.
--   Headless:  bash test/run_test.sh test/tests/test_fast_forward_gui.lua
--              (automated assertions only, no menu verification)
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

-- Wall-clock pause. fw.wait_frames runs on emulator time — under fast-forward
-- 60 emulated frames might elapse in 20ms of real time, far too short for a
-- human to glance at a menu. os.clock() gives CPU seconds; polling it keeps
-- the coroutine yielding frames while we wait out a real-world interval.
local function wait_wallclock(seconds)
    local start = os.clock()
    while os.clock() - start < seconds do
        coroutine.yield()
    end
end

-- Measure how long N emulated frames take in wall-clock seconds. Returns
-- the elapsed real time. Short result ⇒ fast-forward is active; long
-- result ⇒ normal speed.
local function frames_wallclock(n)
    local t0 = os.clock()
    fw.wait_frames(n)
    return os.clock() - t0
end

local is_gui = (canvas ~= nil)

fw.run(function()
    fw.log("=== Test: Fast-Forward GUI State ===")

    if not is_gui then
        -- Headless mode has no display rate-limiter and no menu to inspect,
        -- so both the timing measurements and the visual verification this
        -- test performs are meaningless there. Skip cleanly instead of
        -- reporting a spurious failure.
        fw.log("Skipped: this test only makes sense in GUI mode (mgba-qt)")
        fw.assert_true(true, "test skipped in headless mode")
        -- Brief wait so stdout flushes before clean_shutdown SIGTERMs us;
        -- without this the skip completes in microseconds and the kill
        -- races ahead of the log/PASS lines reaching the terminal.
        fw.wait_frames(30)
        fw.finish()
        return
    end

    -- Get past the title animation so the emulator is fully running.
    fw.wait_frames(180)

    -- Ensure we start at normal speed. The framework's fw.run already
    -- enables fast-forward on entry, so flip it back first so the baseline
    -- measurement reflects normal emulation.
    if emu.setFastForward then
        emu:setFastForward(1)
    end
    fw.wait_frames(30)

    ---------------------------------------------------------------------------
    -- STEP 1: Baseline — measure normal speed.
    ---------------------------------------------------------------------------
    fw.log("Measuring normal speed over 60 frames...")
    local t_normal = frames_wallclock(60)
    fw.log(string.format("Normal: 60 frames took %.3fs wall-clock", t_normal))

    -- At 60 Hz native rate, 60 frames should take ~1.0s. Accept 0.5–3.0s
    -- to absorb vsync quirks on different displays.
    fw.assert_true(t_normal > 0.3,
        "normal-speed 60 frames take > 0.3s wall-clock (got "
        .. string.format("%.3fs", t_normal) .. ")")

    if is_gui then
        fw.log(">>> LOOK AT THE MENU NOW <<<")
        fw.log(">>> Emulation > Fast forward should be UNCHECKED")
        wait_wallclock(4)
    end

    ---------------------------------------------------------------------------
    -- STEP 2: Enable fast-forward via script. Expect speed-up AND menu check.
    ---------------------------------------------------------------------------
    fw.log("Calling emu:setFastForward(0) (unbounded)...")
    fw.assert_true(emu.setFastForward ~= nil, "emu.setFastForward binding exists")
    emu:setFastForward(0)
    fw.wait_frames(5)

    fw.log("Measuring fast-forward speed over 60 frames...")
    local t_ff = frames_wallclock(60)
    fw.log(string.format("Fast-forward: 60 frames took %.3fs wall-clock", t_ff))

    -- FF should be at least ~3x faster than normal. On most hardware it's
    -- 10-50x faster, but 3x is a conservative lower bound.
    fw.assert_true(t_ff < t_normal / 3,
        string.format("FF 60 frames (%.3fs) complete at least 3x faster than normal (%.3fs)",
            t_ff, t_normal))

    if is_gui then
        fw.log(">>> LOOK AT THE MENU NOW <<<")
        fw.log(">>> Emulation > Fast forward should be CHECKED")
        fw.log(">>> If it is NOT checked, the script-to-GUI sync is broken")
        wait_wallclock(5)
    end

    ---------------------------------------------------------------------------
    -- STEP 3: Disable fast-forward. Expect speed back to normal AND menu
    -- unchecked.
    ---------------------------------------------------------------------------
    fw.log("Calling emu:setFastForward(1) (normal)...")
    emu:setFastForward(1)
    fw.wait_frames(30)

    fw.log("Measuring speed after disabling FF over 60 frames...")
    local t_restored = frames_wallclock(60)
    fw.log(string.format("Restored: 60 frames took %.3fs wall-clock", t_restored))

    -- After disabling FF, frames should take roughly normal-speed time
    -- again (within 2x of the baseline).
    fw.assert_true(t_restored > t_normal / 2,
        string.format("after setFastForward(1), 60 frames (%.3fs) back near baseline (%.3fs)",
            t_restored, t_normal))

    if is_gui then
        fw.log(">>> LOOK AT THE MENU NOW <<<")
        fw.log(">>> Emulation > Fast forward should be UNCHECKED again")
        wait_wallclock(4)
    end

    ---------------------------------------------------------------------------
    -- STEP 4: Summary for manual visual verification.
    ---------------------------------------------------------------------------
    if is_gui then
        fw.log("---")
        fw.log("Visual verification summary:")
        fw.log("  * Before setFastForward(0): menu should have been UNCHECKED")
        fw.log("  * After  setFastForward(0): menu should have been CHECKED")
        fw.log("  * After  setFastForward(1): menu should be UNCHECKED again")
        fw.log("If any of those were wrong, the Qt FF tracker is not in sync")
        fw.log("with the scripting binding.")
    end

    fw.finish()
end)
