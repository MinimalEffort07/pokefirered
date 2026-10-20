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
-- A test must make at least one assertion: one that makes none fails with
-- "0 assertions". Demos and probes, which only make frames, opt out with
-- fw.allow_no_assertions(reason); see THE ZERO-ASSERTION RULE above
-- fw.finish().
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
--
-- ONE RESULTS TABLE PER RUN. Test libraries load this file with dofile(),
-- which runs it again and makes a separate copy of every function and local
-- for each file that loads it. If each copy kept its own counts, an
-- assertion made inside a library (test/lib/character_select.lua failing a
-- boot, for example) would land in the library's copy, which nobody reports,
-- and the test would never see it. So every copy shares one table, kept in a
-- global variable (globals are shared by every file in one Lua run), and
-- fw.run() empties it when the test starts.

local results = _G.__pokefirered_test_results
if results == nil then
    results = {
        pass = 0,       -- number of assertions that passed
        fail = 0,       -- number of assertions that failed
        errors = {},    -- list of error message strings
        log_lines = {}, -- all log messages (for debugging)
    }
    _G.__pokefirered_test_results = results
end

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

-- The assertions so far in this run: passed, failed. A test with several
-- scenarios can take the difference before and after each one to log its
-- own counts.
function fw.counts()
    return results.pass, results.fail
end

-------------------------------------------------------------------------------
-- WHERE OUTPUT FILES GO
-------------------------------------------------------------------------------
-- test/run_test.sh makes a private directory for every run and passes it in
-- the MGBA_TEST_DIR environment variable, so two tests running at the same
-- time (in one checkout or in several worktrees) never read or overwrite
-- each other's results and screenshots. A test started some other way (for
-- example loaded by hand in the mGBA GUI) has no MGBA_TEST_DIR and writes to
-- /tmp, as every test did before.
--
-- Every file a test writes should go through fw.out_path:
--   fw.screenshot(fw.out_path("mgba-roaming-init.png"))
--
-- No script under test/ may use a fixed /tmp path, meaning a file or folder
-- that every run uses under the same name (test/tests/test_no_fixed_tmp_paths.lua
-- fails if one does). The only fixed paths allowed are two savestate
-- fixtures that a person makes by hand and the tests only read:
-- /tmp/pokefirered-route1.ss and /tmp/pokefirered-route1-grass.ss.
-- (record_test.sh's /tmp/mgba-recordings folder is shared, but each video in
-- it gets its own timestamped name, so no two runs use the same file.)

local out_dir = os.getenv("MGBA_TEST_DIR")
if out_dir == nil or out_dir == "" then
    out_dir = "/tmp"
end

-- The path for an output file called `name` in this run's directory.
function fw.out_path(name)
    return out_dir .. "/" .. name
end

-------------------------------------------------------------------------------
-- SCREENSHOT
-------------------------------------------------------------------------------
-- Takes a screenshot of the current emulated GBA screen and saves it as a PNG.
-- In headless mode, this works because we patched mGBA to allocate a video
-- buffer (without the patch, headless mode has no video buffer and crashes).

function fw.screenshot(path)
    path = path or fw.out_path("mgba-test-screenshot.png")
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
--   if not fw.try_load_state(fw.out_path("my-fixture.ss")) then
--       -- slow setup path
--       fw.save_state(fw.out_path("my-fixture.ss"))
--   end
--   -- assertions on the now-loaded state
--
-- A state is only valid for the ROM build that saved it. Under
-- test/run_test.sh, fw.out_path is private to the run, so a test can never
-- load a state another run (or another worktree's ROM) saved; it only
-- reuses its own within the run. Run by hand, fw.out_path is /tmp and the
-- state is kept between runs. For permanent fixtures, write under
-- test/fixtures/ instead.

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
-- HEADLESS VS GUI MODE DETECTION
-------------------------------------------------------------------------------
-- mGBA provides a "canvas" global for drawing overlays in the GUI.
-- In headless mode (mgba-headless), canvas is nil because there's no display.
-- We use this to detect which mode we're running in.
local is_headless = (canvas == nil)

-------------------------------------------------------------------------------
-- MOMENTS: WHAT A RECORDING PLAYS AT NORMAL SPEED
-------------------------------------------------------------------------------
-- FAST-FORWARD. A real GBA makes 60 frames (pictures) a second, and an
-- emulator normally holds itself back to that pace so a game plays at its
-- real speed. Fast-forward lifts that limit: the emulator makes frames as
-- fast as the computer can, 1,200 to 1,800 a second in the mGBA window on
-- this machine. The game can't tell the difference (it still sees one frame
-- after another), so a test proves the same things either way; only the
-- wall-clock time changes. Headless (mgba-headless, the suite) there is no
-- window and the emulator always runs flat out.
--
-- So in the mGBA window (test/record_test.sh films a test there) a test
-- fast-forwards throughout, and its boot and setup take seconds instead of
-- minutes. But then the outcome it proves, a menu opening or a sprite
-- swapping, flies by in a few milliseconds of video, too quick to see.
--
-- A MOMENT is an outcome a watcher must see to believe the test. The test
-- marks it, and in the window it plays at normal speed (60 frames a second)
-- for about a second; then fast-forward resumes by itself. The scripting log
-- beside the game names it ("[WATCH] P1: four patches shake around the
-- player"), and the drop to normal speed is the watcher's cue to pause the
-- video. There are two calls:
--
--   fw.show(label, frames)       The test waits here `frames` frames
--                                (default 60, one second), and they play at
--                                normal speed: for an outcome on screen now
--                                (the PP in the move menu, the party after
--                                MOVE TO PC).
--   fw.show_next(label, frames)  The next `frames` frames (default 60) play
--                                at normal speed while the test carries on:
--                                for a motion the test is about to cause (a
--                                swap, a warp, a battle starting).
--
-- `label` is a non-empty string saying what to look at, and `frames` a whole
-- number from 1 to 180 (3 seconds); anything else is an error that names the
-- rule (and so fails the test). A moment marked while another plays extends
-- it to whichever ends later, never shortens it. fw.speed() says "fast" or
-- "normal": the speed the window runs at on the frame the test is on, or
-- would run at headless, so a headless test can check moments frame by frame
-- (test/tests/test_recording_moments.lua does). fw.finish() logs the total,
-- e.g. "[WATCH] 3 moments, 3.0 s at normal speed", or that there were none.
-- In the video a 60-frame moment lasts about 1.2 s: a second at normal
-- speed, plus a fifth of a second as the emulator settles after fast-forward.
--
-- SAME RUN HEADLESS AND RECORDED. A moment changes only the window's speed:
-- the game sees the same frames and the same buttons in both, and fw.show
-- waits its frames headless too (they take milliseconds there). So a video
-- shows exactly the run the suite proves.
--
-- WHERE MOMENTS GO:
--   * One per outcome a watcher must see to believe the test: usually one
--     per scenario, at the outcome it asserts, while it's on screen.
--   * For many cases of one check (8 head-on swaps), one representative.
--   * A second (the default). Up to three only for a motion that lasts
--     longer.
--   * fw.show adds frames, so never put it inside a stretch the test times
--     (a frame count it asserts). Use fw.show_next there: it adds none.
--   * None for a test with nothing to show in the game (one that checks
--     files, scripts or ROM data).
--
-- WHY A TEST NEVER SETS THE SPEED ITSELF. mGBA ignores emu:setFastForward
-- called from inside the test (the coroutine, see THE COROUTINE PATTERN)
-- once the test has started a process (io.popen, which test libraries use
-- to run `nm` or `ls`) earlier in the same frame: asked for normal speed,
-- the game stayed at 1,834 frames a second. Why isn't known. Made from the
-- frame callback, outside the coroutine, every change works. So the test
-- only marks moments, and the framework makes every speed change itself, in
-- its frame callback, before it resumes the test: the fast-forward at the
-- start, each moment, fw.finish()'s normal speed (record_test.sh films 3
-- seconds of the final screen), and the older fw.slow_down/fw.speed_up. A
-- moment marked on frame F therefore starts in the window on frame F+1, a
-- sixtieth of a second later.
--
-- ONE SPEED PER RUN. Like the results table above, the speed state lives in
-- a global shared by every copy of this file, so a moment marked by a test
-- library (through its own copy) reaches the frame callback the test's copy
-- registered.

local MOMENT_DEFAULT_FRAMES = 60
local MOMENT_MAX_FRAMES = 180  -- 3 seconds

-- `watch` starts empty here; fw.run() fills it in (reset_watch, below) as the
-- test starts.
local watch = _G.__pokefirered_test_watch
if watch == nil then
    watch = {}
    _G.__pokefirered_test_watch = watch
end

local function reset_watch()
    watch.frame = 0           -- frames so far: the frame callback counts them
    watch.normal_until = 0    -- the last frame a moment covers
    watch.moments = 0         -- moments marked
    watch.moment_frames = 0   -- frames moments cover, overlaps counted once
    watch.slowed = false      -- between fw.slow_down and fw.speed_up
    watch.slowed_frames = 0   -- frames slowed that way, outside any moment
    watch.finishing = false   -- fw.finish() was called: normal speed
    watch.applied = nil       -- the speed last set in the window
end
reset_watch()

-- "normal" or "fast": the speed of the frame the test is on (see above).
function fw.speed()
    if watch.finishing or watch.slowed or watch.frame <= watch.normal_until then
        return "normal"
    end
    return "fast"
end

-- A value as an error message shows it: strings in quotes.
local function shown(value)
    if type(value) == "string" then
        return string.format("%q", value)
    end
    return tostring(value)
end

-- Check a moment's label and frames for fw.show or fw.show_next (`caller`),
-- and return the frames (the default if none were given). The error names
-- the rule and points at the test's line (level 3: the caller's caller).
local function check_moment(caller, label, frames)
    if type(label) ~= "string" or label == "" then
        error(string.format("%s: label must be a non-empty string saying what to watch (got %s)",
            caller, shown(label)), 3)
    end
    if frames == nil then
        frames = MOMENT_DEFAULT_FRAMES
    end
    if type(frames) ~= "number" or frames ~= math.floor(frames)
            or frames < 1 or frames > MOMENT_MAX_FRAMES then
        error(string.format("%s: frames must be a whole number from 1 to %d (got %s)",
            caller, MOMENT_MAX_FRAMES, shown(frames)), 3)
    end
    return frames
end

-- Mark the next `frames` frames as a moment, extending any moment still
-- playing, and log its label.
local function mark_moment(label, frames)
    local last = watch.frame + frames
    -- Count only the frames no earlier moment covers.
    local from = math.max(watch.frame + 1, watch.normal_until + 1)
    if last >= from then
        watch.moment_frames = watch.moment_frames + (last - from + 1)
    end
    watch.normal_until = math.max(watch.normal_until, last)
    watch.moments = watch.moments + 1
    fw.log("[WATCH] " .. label)
end

-- Hold on what's on screen now: the test waits `frames` frames (default 60)
-- and the window plays them at normal speed.
function fw.show(label, frames)
    frames = check_moment("fw.show", label, frames)
    mark_moment(label, frames)
    fw.wait_frames(frames)
end

-- Play the next `frames` frames (default 60) at normal speed in the window,
-- while the test carries on at once.
function fw.show_next(label, frames)
    frames = check_moment("fw.show_next", label, frames)
    mark_moment(label, frames)
end

-- The older way to slow down: normal speed from fw.slow_down(reason) until
-- fw.speed_up(). It is kept only until #119's tickets have given every test
-- that calls it moments instead, and then removed. Prefer fw.show and
-- fw.show_next.
function fw.slow_down(reason)
    watch.slowed = true
    fw.log("[SLOW] " .. (reason or "visual inspection"))
end

function fw.speed_up()
    watch.slowed = false
    fw.log("[FAST] resumed fast-forward")
end

-- Set the window to fw.speed(), if it isn't already. Called only from the
-- frame callback, before it resumes the test (see WHY A TEST NEVER SETS THE
-- SPEED ITSELF). emu.setFastForward is a binding added to our patched mGBA:
-- 0 means unbounded (fast-forward), 1 means normal speed. Headless there is
-- no window to change, so only the record of the speed changes.
local function apply_speed()
    local want = fw.speed()
    if want == watch.applied then
        return
    end
    watch.applied = want
    if is_headless or not emu.setFastForward then
        return
    end
    emu:setFastForward(want == "fast" and 0 or 1)
end

-- The [WATCH] total fw.finish() logs: how many moments, and how long the
-- window played at normal speed for them.
local function watch_total()
    local lines = {}
    if watch.moments > 0 then
        lines[1] = string.format("[WATCH] %d moment%s, %.1f s at normal speed",
            watch.moments, watch.moments == 1 and "" or "s", watch.moment_frames / 60)
    elseif watch.slowed_frames == 0 then
        lines[1] = "[WATCH] no moments: in the mGBA window this test fast-forwards throughout"
    else
        lines[1] = "[WATCH] no moments"
    end
    if watch.slowed_frames > 0 then
        lines[#lines + 1] = string.format(
            "[WATCH] and %.1f s at normal speed between fw.slow_down and fw.speed_up",
            watch.slowed_frames / 60)
    end
    return lines
end

-------------------------------------------------------------------------------
-- RESULTS AND EXIT
-------------------------------------------------------------------------------

-- Write test results to a JSON file so the shell runner can parse them.
function fw.write_results(path)
    path = path or fw.out_path("mgba-test-results.json")
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

-- THE ZERO-ASSERTION RULE: a test can only pass by proving something.
--
-- A test that reaches fw.finish() without making a single assertion
-- (fw.assert_eq / fw.assert_neq / fw.assert_true) has checked nothing, so it
-- fails, with a "0 assertions" error. Without this rule a test whose checks
-- were all skipped (an early return, a check that only logs) would report
-- "0/0 passed" and exit 0, exactly like a test that works.
--
-- THE OPT-OUT: demos and probes. A demo exists to make frames for a README
-- GIF and a probe to print or capture something for a person to look at;
-- neither proves anything, so neither asserts. Such a file says so once, at
-- the top, right after loading this framework:
--
--   fw.allow_no_assertions("demo: only captures frames for the README GIF")
--
-- (A test that only skips itself in some setting, such as a GUI-only test
-- run headless, calls it on the path that skips instead.) It then passes
-- with zero assertions, and the RESULTS line gives the reason. The opt-out
-- only lifts the zero-assertion rule: an assertion that fails, a crash, or a
-- failed boot (test/lib/character_select.lua) still fails the run. As these
-- files prove nothing, a whole-suite run should leave them out; run them on
-- their own, or record them with test/record_test.sh.
local ZERO_ASSERTIONS_FAILURE = "0 assertions -- this test checked nothing, "
    .. "so it cannot pass. Assert what it is meant to prove (fw.assert_true, "
    .. "fw.assert_eq, fw.assert_neq), or, if it is a demo or probe that only "
    .. "makes frames, declare fw.allow_no_assertions(reason) at the top."

-- The reason given to fw.allow_no_assertions, or nil if the test didn't
-- opt out. It belongs to this copy of the framework (the one the test file
-- loaded and calls fw.finish() on), so a library can't opt a test out.
local no_assertions_reason = nil

-- Declare that this file is a demo or probe that makes no assertions (see
-- THE OPT-OUT above). `reason` is a short note saying what the file is for.
function fw.allow_no_assertions(reason)
    no_assertions_reason = reason or "no reason given"
end

-- Call this at the end of every test to report results and exit.
-- In GUI mode, it restores normal emulation speed so you can interact.
-- In headless mode, it triggers process termination.
function fw.finish()
    -- The zero-assertion rule (above). Its failure is recorded like a failed
    -- assertion, so the results file, the exit code and the RESULTS line all
    -- report it the same way.
    local assertions = results.pass + results.fail
    local summary
    if assertions > 0 then
        summary = string.format("RESULTS: %d/%d passed, %d failed",
            results.pass, assertions, results.fail)
    elseif no_assertions_reason ~= nil then
        summary = "RESULTS: 0 assertions, as declared (" .. no_assertions_reason .. ")"
    else
        fw.assert_true(false, ZERO_ASSERTIONS_FAILURE)
        summary = "RESULTS: 0 assertions, so the test fails"
    end

    for _, line in ipairs(watch_total()) do
        fw.log(line)
    end

    fw.write_results()
    if results.fail > 0 then
        fw.log_error(summary)
    else
        fw.log(summary)
    end

    -- Write exit code to a file for the shell runner script to read.
    -- (The actual process exit code can't be controlled from Lua.)
    local ef = io.open(fw.out_path("mgba-test-exitcode"), "w")
    if ef then
        ef:write(tostring(results.fail > 0 and 1 or 0))
        ef:close()
    end

    -- Back to normal speed in the window, so record_test.sh's last 3
    -- seconds show the final screen and the user can play on. The frame
    -- callback makes the change (see WHY A TEST NEVER SETS THE SPEED ITSELF).
    watch.finishing = true

    shutdown_requested = true
end

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

-- Frames mGBA gets to finish starting up before the test begins.
local STARTUP_FRAMES = 2

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
    -- Start from empty results. Under test/run_test.sh every run is a fresh
    -- emulator, so they are empty anyway; in the mGBA GUI, scripts loaded
    -- one after another share one Lua state (and so the shared results
    -- table), and the second test must not inherit the first one's counts.
    results.pass = 0
    results.fail = 0
    results.errors = {}
    results.log_lines = {}
    -- And from no moments, at frame 0 (the same reason: shared in the GUI).
    reset_watch()

    -- Create the coroutine that will run the test function.
    -- xpcall wraps it in error handling so crashes produce useful messages.
    test_coroutine = coroutine.create(function()
        -- Wait 2 frames for mGBA to fully initialize before doing anything.
        -- The frame callback turns fast-forward on as these end (see
        -- STARTUP_FRAMES below), so the test itself starts fast.
        fw.wait_frames(STARTUP_FRAMES)

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
        -- This run is over (in the GUI the callback is still called every
        -- frame; a script loaded after this one runs its own).
        if shutdown_done then
            return
        end

        -- Count the frame and set the window's speed for it, before the
        -- test runs (see WHY A TEST NEVER SETS THE SPEED ITSELF). The
        -- speed is left alone until mGBA has had its STARTUP_FRAMES: at
        -- the third frame fast-forward goes on, and the test starts.
        watch.frame = watch.frame + 1
        if watch.slowed and watch.frame > watch.normal_until then
            watch.slowed_frames = watch.slowed_frames + 1
        end
        if watch.frame > STARTUP_FRAMES then
            apply_speed()
        end

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
