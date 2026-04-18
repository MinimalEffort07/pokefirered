-------------------------------------------------------------------------------
-- mGBA Lua Test Framework for pokefirered
-------------------------------------------------------------------------------
--
-- HOW THIS WORKS (for people unfamiliar with mGBA scripting):
--
-- mGBA is a Game Boy Advance emulator. It has a built-in Lua scripting engine
-- that lets you control the emulator programmatically. When a Lua script is
-- loaded, mGBA provides several global objects:
--
--   emu      - The emulator core. Used to read/write memory, inject button
--              presses, take screenshots, reset the game, etc.
--   console  - Logging output. console:log("msg") prints to the scripting
--              console (visible in GUI) or stdout (in headless mode).
--   callbacks - Event system. callbacks:add("frame", fn) registers a function
--              that mGBA calls once per emulated frame (60 times per second on
--              GBA). This is the main hook for automation.
--   canvas   - Drawing overlay (only available in GUI mode, nil in headless).
--
-- THE COROUTINE PATTERN:
--
-- The GBA runs at 60 frames per second. mGBA calls our "frame" callback once
-- per frame. To write sequential test logic ("press A, wait 60 frames, check
-- memory"), we use Lua coroutines:
--
--   1. The test function is wrapped in a coroutine (a pausable function)
--   2. Each frame, the "frame" callback resumes the coroutine for one step
--   3. When the test calls fw.wait_frames(60), the coroutine yields 60 times,
--      meaning 60 real game frames pass before the test continues
--   4. This lets us write linear test code that plays out over time
--
-- MEMORY LAYOUT:
--
-- The GBA has memory-mapped hardware. Game data lives at specific addresses:
--   0x02000000-0x0203FFFF  EWRAM (256KB) - main game data, sprites, save blocks
--   0x03000000-0x03007FFF  IWRAM (32KB)  - fast memory for critical data
--   0x08000000+            ROM           - game code and static data
--
-- We read these addresses to check game state (e.g., what sprite the player
-- is using, whether we're in the overworld, etc.)
--
-- RUNNING TESTS:
--
-- Headless (automated, no GUI):
--   mgba-headless pokefirered.gba --script test/tests/test_player_sprite.lua
--
-- GUI (visual, interactive):
--   mgba-qt pokefirered.gba
--   Then: Tools > Scripting > File > Load script > select a test
--
-- The framework auto-detects which mode it's in. In headless mode, it kills
-- the process when done. In GUI mode, it restores normal speed and stops.
--
-- USAGE FROM A TEST SCRIPT:
--
--   local fw = dofile("test/lib/framework.lua")
--   fw.run(function()
--       fw.wait_frames(60)           -- wait 1 second (60 frames)
--       fw.press("A")                -- press the A button
--       fw.assert_eq(value, 42, "should be 42")
--       fw.finish()                  -- report results and exit
--   end)
--
-------------------------------------------------------------------------------

local fw = {}

-------------------------------------------------------------------------------
-- GBA BUTTON MAPPING
-------------------------------------------------------------------------------
-- The GBA has 10 buttons. mGBA identifies them by index (0-9).
-- These match the bit positions in the GBA's KEYINPUT register (0x04000130).
-- When you call emu:addKey(0), it's like pressing the A button on the GBA.

fw.KEY = {
    A      = 0,   -- The A button (confirm/interact)
    B      = 1,   -- The B button (cancel/back)
    SELECT = 2,   -- The Select button
    START  = 3,   -- The Start button (pause/menu)
    RIGHT  = 4,   -- D-pad right
    LEFT   = 5,   -- D-pad left
    UP     = 6,   -- D-pad up
    DOWN   = 7,   -- D-pad down
    R      = 8,   -- Right shoulder button
    L      = 9,   -- Left shoulder button
}

-------------------------------------------------------------------------------
-- TEST RESULTS TRACKING
-------------------------------------------------------------------------------
-- Accumulates pass/fail counts and error messages during the test.
-- Written to a JSON file at the end so the shell runner can parse results.

local results = {
    pass = 0,       -- number of assertions that passed
    fail = 0,       -- number of assertions that failed
    errors = {},    -- list of error message strings
    log_lines = {}, -- all log messages (for debugging)
}

-- The coroutine that runs the test logic. Set by fw.run().
local test_coroutine = nil

-- Tracks whether fw.run() has been called (unused but reserved).
local test_started = false

-------------------------------------------------------------------------------
-- LOGGING
-------------------------------------------------------------------------------
-- All log messages go through these functions. In mGBA, console:log() prints
-- to the scripting console (GUI) or stdout (headless). The [TEST] prefix makes
-- it easy to filter test output from mGBA's own debug messages.

-- Log an informational message.
function fw.log(msg)
    local line = "[TEST] " .. msg
    table.insert(results.log_lines, line)
    -- console:log is provided by mGBA's scripting environment
    console:log(line)
end

-- Log an error message.
function fw.log_error(msg)
    local line = "[FAIL] " .. msg
    table.insert(results.log_lines, line)
    -- console:error prints in red in the GUI scripting console
    console:error(line)
end

-------------------------------------------------------------------------------
-- FRAME CONTROL
-------------------------------------------------------------------------------
-- These functions control the passage of time in the emulated game.
-- They MUST be called from within the test coroutine (inside fw.run's function).
--
-- How it works: coroutine.yield() pauses the test function. The frame callback
-- (registered in fw.run) resumes it on the next frame. So each yield = 1 game
-- frame = 1/60th of a second of game time.

-- Advance exactly one game frame.
function fw.yield()
    coroutine.yield()
end

-- Advance the given number of game frames.
-- Example: fw.wait_frames(60) waits exactly 1 second of game time.
function fw.wait_frames(n)
    for i = 1, n do
        coroutine.yield()
    end
end

-- Wait until a condition is true, checking every frame.
-- Returns true if the condition was met, false if it timed out.
-- Default timeout is 600 frames (10 seconds at 60fps).
--
-- Example:
--   fw.wait_until(function() return emu:read8(addr) == 1 end, 300, "flag set")
function fw.wait_until(predicate, timeout_frames, description)
    timeout_frames = timeout_frames or 600
    description = description or "condition"
    for i = 1, timeout_frames do
        if predicate() then
            fw.log(description .. " met after " .. i .. " frames")
            return true
        end
        coroutine.yield()
    end
    fw.log_error(description .. " NOT met after " .. timeout_frames .. " frames (timeout)")
    return false
end

-------------------------------------------------------------------------------
-- INPUT HELPERS
-------------------------------------------------------------------------------
-- These simulate GBA button presses. The GBA reads button state once per frame,
-- so holding a button for 1 frame is a single press. The game processes input
-- during each frame's VBlank (vertical blank) interrupt.
--
-- emu:addKey(idx) marks a button as held down.
-- emu:clearKey(idx) marks a button as released.
-- The button state persists between frames until explicitly cleared.

-- Press and release a button. Holds for 1 frame, then releases for 1 frame.
-- This simulates a quick tap, like pressing A to advance a text box.
-- key: a string name like "A", "START", "DOWN", etc.
function fw.press(key)
    local idx = fw.KEY[key]
    if idx == nil then
        error("Unknown key: " .. tostring(key))
    end
    emu:addKey(idx)      -- tell mGBA this button is now held down
    coroutine.yield()    -- wait 1 frame (the game reads it as pressed)
    emu:clearKey(idx)    -- release the button
    coroutine.yield()    -- wait 1 frame (the game reads it as released)
end

-- Hold a button for multiple frames, then release.
-- Useful for walking (hold a direction) or scrolling menus.
-- Example: fw.hold("RIGHT", 30) walks right for 30 frames (half a second).
function fw.hold(key, frames)
    local idx = fw.KEY[key]
    if idx == nil then
        error("Unknown key: " .. tostring(key))
    end
    emu:addKey(idx)          -- press and hold
    fw.wait_frames(frames)   -- keep holding for N frames
    emu:clearKey(idx)        -- release
end

-- Hold a direction continuously while a predicate is being polled. Stops
-- when the predicate returns true OR the key has been held for max_frames.
-- Returns true if the predicate succeeded, false on timeout.
--
-- Useful for navigation: hold RIGHT until the map changes, hold UP until
-- the player passes the screen-edge connection. Releases the key on exit
-- in all cases. Polls the predicate every poll_interval frames (default 8).
function fw.hold_until(key, predicate, max_frames, poll_interval)
    local idx = fw.KEY[key]
    if idx == nil then
        error("Unknown key: " .. tostring(key))
    end
    poll_interval = poll_interval or 8
    max_frames = max_frames or 600
    emu:addKey(idx)
    local elapsed = 0
    while elapsed < max_frames do
        fw.wait_frames(poll_interval)
        elapsed = elapsed + poll_interval
        if predicate() then
            emu:clearKey(idx)
            return true
        end
    end
    emu:clearKey(idx)
    return false
end

-- Press a button and then wait extra frames for the game to react.
-- Useful when pressing A triggers an animation or screen transition.
function fw.press_and_wait(key, wait_after)
    fw.press(key)
    fw.wait_frames(wait_after or 30)
end

-- Release all buttons. A safety measure to ensure no keys are stuck.
-- 0x3FF is a bitmask with all 10 GBA buttons set (bits 0-9).
function fw.clear_all_keys()
    emu:clearKeys(0x3FF)
end

-------------------------------------------------------------------------------
-- MEMORY READING HELPERS
-------------------------------------------------------------------------------
-- The GBA's memory is directly accessible through mGBA's scripting API.
-- emu:read8(addr) reads 1 byte, read16 reads 2 bytes (16-bit), read32 reads
-- 4 bytes (32-bit). All addresses are physical GBA memory addresses.
--
-- Common memory regions:
--   0x02000000+ = EWRAM - where most game variables live
--   0x03000000+ = IWRAM - fast RAM for performance-critical data
--   0x08000000+ = ROM   - read-only game code and data
--
-- These wrappers exist so test scripts can use fw.read8() instead of
-- emu:read8() directly, keeping the API consistent.

function fw.read8(addr)
    return emu:read8(addr)
end

function fw.read16(addr)
    return emu:read16(addr)
end

function fw.read32(addr)
    return emu:read32(addr)
end

-------------------------------------------------------------------------------
-- ASSERTIONS
-------------------------------------------------------------------------------
-- Standard test assertions. Each one increments either the pass or fail count.
-- On failure, the error message is stored for the final results report.

-- Assert that actual == expected.
function fw.assert_eq(actual, expected, msg)
    msg = msg or "values equal"
    if actual == expected then
        results.pass = results.pass + 1
        fw.log("PASS: " .. msg)
        return true
    else
        results.fail = results.fail + 1
        local err = string.format("FAIL: %s (expected %s, got %s)",
            msg, tostring(expected), tostring(actual))
        fw.log_error(err)
        table.insert(results.errors, err)
        return false
    end
end

-- Assert that actual ~= unexpected.
function fw.assert_neq(actual, unexpected, msg)
    msg = msg or "values not equal"
    if actual ~= unexpected then
        results.pass = results.pass + 1
        fw.log("PASS: " .. msg)
        return true
    else
        results.fail = results.fail + 1
        local err = string.format("FAIL: %s (got unexpected %s)",
            msg, tostring(actual))
        fw.log_error(err)
        table.insert(results.errors, err)
        return false
    end
end

-- Assert that a condition is truthy.
function fw.assert_true(cond, msg)
    msg = msg or "condition true"
    if cond then
        results.pass = results.pass + 1
        fw.log("PASS: " .. msg)
        return true
    else
        results.fail = results.fail + 1
        local err = "FAIL: " .. msg
        fw.log_error(err)
        table.insert(results.errors, err)
        return false
    end
end

-------------------------------------------------------------------------------
-- SCREENSHOT
-------------------------------------------------------------------------------
-- Takes a screenshot of the current emulated GBA screen and saves it as a PNG.
-- In headless mode, this works because we patched mGBA to allocate a video
-- buffer (without the patch, headless mode has no video buffer and crashes).

function fw.screenshot(path)
    path = path or "/tmp/mgba-test-screenshot.png"
    emu:screenshot(path)
    fw.log("Screenshot saved to " .. path)
end

-------------------------------------------------------------------------------
-- SAVE STATE CACHING
-------------------------------------------------------------------------------
-- mGBA save states capture the full emulator state to a file (~600KB
-- compressed). Tests that require a long boot/navigation sequence to
-- reach a specific game state (e.g., "player standing on Route 1") can
-- save a state once and reload it on subsequent runs to skip minutes
-- of fast-forwarded setup.
--
-- SAVESTATE_ALL (31) covers screenshot + save data + cheats + RTC +
-- metadata. We always save with the full set so reloads survive
-- unrelated emulator config changes.
--
-- USAGE PATTERN:
--   if not fw.try_load_state("/tmp/my-fixture.ss") then
--       -- slow setup path
--       fw.save_state("/tmp/my-fixture.ss")
--   end
--   -- assertions on the now-loaded state
--
-- The state files live in /tmp by default so they're cleaned up by the
-- OS between reboots and don't accumulate. For permanent fixtures,
-- write under test/fixtures/ instead.

local SAVESTATE_ALL = 31

-- Write a save state to disk. Returns true on success.
function fw.save_state(path)
    local ok = emu:saveStateFile(path, SAVESTATE_ALL)
    if ok then
        fw.log("[STATE] saved -> " .. path)
    else
        fw.log_error("[STATE] save failed -> " .. path)
    end
    return ok
end

-- Try to load a save state. Returns true if the file existed and loaded
-- cleanly; false (with a log line) if not -- callers should treat false
-- as "fall through to slow setup". We probe with io.open first because
-- emu:loadStateFile will spam mGBA error logs when the file is missing.
function fw.try_load_state(path)
    local probe = io.open(path, "rb")
    if probe == nil then
        fw.log("[STATE] no cached state at " .. path .. " -- running fresh setup")
        return false
    end
    probe:close()
    local ok = emu:loadStateFile(path, SAVESTATE_ALL)
    if ok then
        fw.log("[STATE] loaded <- " .. path)
    else
        fw.log_error("[STATE] load failed (file exists but rejected)")
    end
    return ok
end

-------------------------------------------------------------------------------
-- VISUAL INSPECTION CONTROL (GUI mode only)
-------------------------------------------------------------------------------
-- Tests run with fast-forward enabled by default so the long boot /
-- navigation sequences don't waste a tester's time. But the part of a
-- test that actually demonstrates the feature (e.g., a battle starting,
-- a sprite appearing, a warp transition) flies by too fast to see at
-- 1000+ fps. These helpers let the test author drop back to real-time
-- 60fps right before the interesting moment so a human watching the
-- mGBA window can actually observe it, then resume fast-forward
-- afterward to skip the cleanup phase.
--
-- In headless mode (no canvas), both functions are no-ops -- there is
-- no display to slow down for, and slowing the headless run would just
-- waste CI time. Detection is the same `canvas == nil` check used
-- throughout the framework.

-- Drop fast-forward to normal 60fps speed for visual inspection.
-- Pass a short reason string so the test log explains why we slowed.
function fw.slow_down(reason)
    if emu.setFastForward and canvas ~= nil then
        emu:setFastForward(1)  -- 1.0 = real-time 60fps
        fw.log("[SLOW] " .. (reason or "visual inspection"))
    end
end

-- Resume unbounded fast-forward (GUI only). Pair with a prior
-- slow_down() to skip back into accelerated playback once the human
-- has seen the moment.
function fw.speed_up()
    if emu.setFastForward and canvas ~= nil then
        emu:setFastForward(0)  -- 0 = unbounded
        fw.log("[FAST] resumed fast-forward")
    end
end

-------------------------------------------------------------------------------
-- RESULTS AND EXIT
-------------------------------------------------------------------------------

-- Write test results to a JSON file so the shell runner can parse them.
function fw.write_results(path)
    path = path or "/tmp/mgba-test-results.json"
    -- io.open is available because mGBA's Lua engine calls luaL_openlibs(),
    -- which loads all standard Lua libraries including io, os, string, etc.
    local f = io.open(path, "w")
    if not f then
        console:error("[TEST] Failed to open results file: " .. path)
        return
    end

    f:write('{\n')
    f:write(string.format('  "pass": %d,\n', results.pass))
    f:write(string.format('  "fail": %d,\n', results.fail))
    f:write('  "errors": [')
    for i, e in ipairs(results.errors) do
        if i > 1 then f:write(', ') end
        local escaped = e:gsub('\\', '\\\\'):gsub('"', '\\"')
        f:write('"' .. escaped .. '"')
    end
    f:write(']\n')
    f:write('}\n')
    f:close()
    fw.log("Results written to " .. path)
end

-- Flag checked by the frame callback to trigger shutdown.
-- We can't call os.exit() or os.execute("kill") from inside a Lua coroutine
-- in mGBA (it crashes or doesn't work). Instead, the test sets this flag,
-- and the frame callback (which runs OUTSIDE the coroutine) handles shutdown.
local shutdown_requested = false

-- Call this at the end of every test to report results and exit.
-- In GUI mode, it restores normal emulation speed so you can interact.
-- In headless mode, it triggers process termination.
function fw.finish()
    fw.write_results()
    local total = results.pass + results.fail
    local summary = string.format("RESULTS: %d/%d passed, %d failed",
        results.pass, total, results.fail)
    if results.fail > 0 then
        fw.log_error(summary)
    else
        fw.log(summary)
    end

    -- Write exit code to a file for the shell runner script to read.
    -- (The actual process exit code can't be controlled from Lua.)
    local ef = io.open("/tmp/mgba-test-exitcode", "w")
    if ef then
        ef:write(tostring(results.fail > 0 and 1 or 0))
        ef:close()
    end

    -- Restore normal emulation speed in GUI mode so the user can
    -- interact with the game after the test finishes.
    -- emu.setFastForward is a custom binding we added to mGBA.
    if emu.setFastForward then
        emu:setFastForward(1)  -- 1.0 = normal 60fps speed
    end

    shutdown_requested = true
end

-------------------------------------------------------------------------------
-- HEADLESS VS GUI MODE DETECTION
-------------------------------------------------------------------------------
-- mGBA provides a "canvas" global for drawing overlays in the GUI.
-- In headless mode (mgba-headless), canvas is nil because there's no display.
-- We use this to detect which mode we're running in.
local is_headless = (canvas == nil)

-- Terminate the mGBA process cleanly (headless mode only).
-- In GUI mode, we just stop the test — we don't want to close the window.
--
-- We send SIGTERM to our own process by reading our PID from /proc/self/stat
-- (a Linux-specific file). mGBA's headless mode has a signal handler that
-- cleanly shuts down when it receives SIGTERM.
--
-- IMPORTANT: This must be called from the frame callback (outside the
-- coroutine), not from inside the test function. os.execute() from inside
-- a Lua coroutine doesn't reliably deliver the signal in mGBA.
local shutdown_done = false  -- latch so GUI mode doesn't log every frame
local function clean_shutdown()
    if shutdown_done then
        return
    end
    shutdown_done = true
    if not is_headless then
        fw.log("GUI mode — test complete, not killing process")
        return
    end
    local f = io.open("/proc/self/stat", "r")
    if f then
        local pid = f:read("*a"):match("^(%d+)")
        f:close()
        if pid then
            os.execute("kill " .. pid)
        end
    end
end

-------------------------------------------------------------------------------
-- TEST RUNNER ENGINE
-------------------------------------------------------------------------------
-- This is the core of the framework. It sets up the coroutine and frame
-- callback that drive the test.

-- Run a test function. Call this once at the end of your test script.
--
-- How it works:
--   1. Wraps your test function in a Lua coroutine (a pausable function)
--   2. Registers a "frame" callback with mGBA that fires every game frame
--   3. Each frame, the callback resumes the coroutine for one step
--   4. When the test calls fw.wait_frames(), fw.press(), etc., the coroutine
--      yields (pauses), and the game advances a frame before resuming
--   5. When the test calls fw.finish(), a shutdown flag is set
--   6. On the next frame, the callback sees the flag and terminates
function fw.run(test_fn)
    -- Create the coroutine that will run the test function.
    -- xpcall wraps it in error handling so crashes produce useful messages.
    test_coroutine = coroutine.create(function()
        -- Wait 2 frames for mGBA to fully initialize before doing anything.
        fw.wait_frames(2)

        -- Enable fast-forward so tests run as fast as the CPU allows.
        -- In GUI mode: removes the 60fps frame limiter (the game runs at
        -- hundreds or thousands of fps). The screen still updates each frame.
        -- In headless mode: already runs at max speed, so this is a no-op.
        -- emu.setFastForward is a custom API we added to mGBA's scripting
        -- bindings. It sets sync.fpsTarget to 0 (unbounded).
        if emu.setFastForward then
            emu:setFastForward(0)  -- 0 = run as fast as possible
            fw.log("Fast-forward enabled")
        end

        -- Run the test function with error protection.
        -- debug.traceback gives us a full stack trace if it crashes.
        local ok, err = xpcall(test_fn, debug.traceback)
        if not ok then
            fw.log_error("Test crashed: " .. tostring(err))
            results.fail = results.fail + 1
            table.insert(results.errors, "CRASH: " .. tostring(err))
            fw.finish()
        end
    end)

    -- Register the frame callback with mGBA.
    -- callbacks:add("frame", fn) tells mGBA to call fn() after every
    -- emulated frame (i.e., 60 times per second of game time).
    -- This is the "heartbeat" that drives the test forward.
    callbacks:add("frame", function()
        -- If the test called fw.finish(), shut down on this frame.
        -- clean_shutdown() runs outside the coroutine (important!).
        if shutdown_requested then
            clean_shutdown()
            return
        end

        -- If the test coroutine is still running, resume it for one step.
        -- coroutine.resume() continues execution from where the coroutine
        -- last called coroutine.yield() (e.g., inside fw.wait_frames).
        if test_coroutine and coroutine.status(test_coroutine) ~= "dead" then
            local ok, err = coroutine.resume(test_coroutine)
            if not ok then
                console:error("[TEST] Coroutine error: " .. tostring(err))
                results.fail = results.fail + 1
                table.insert(results.errors, "COROUTINE: " .. tostring(err))
                fw.finish()
            end
        end
    end)

    fw.log("Test framework initialized, waiting for frames...")
end

return fw
