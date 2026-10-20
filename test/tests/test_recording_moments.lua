-------------------------------------------------------------------------------
-- test_recording_moments.lua: a test's marked moments (issue #119, #124)
-------------------------------------------------------------------------------
-- In the mGBA window a test fast-forwards, except at its moments: outcomes a
-- watcher must see, which play at normal speed for about a second (see
-- MOMENTS in test/lib/framework.lua). This test checks the moments headless,
-- through fw.speed(), which says what speed the window runs at (or would run
-- at, headless) on the frame the test is on. The real speed in the window is
-- checked by test/tests/test_fast_forward_gui.lua.
--
--   M1  fw.show(label) waits exactly 60 frames, all at "normal", and the
--       frame after is "fast" again.
--   M2  fw.show(label, 30): the same over 30 frames.
--   M3  fw.show_next(label) returns at once (no frame passes); the next 60
--       frames are "normal" while the test waits frame by frame, the 61st is
--       "fast".
--   M4  Overlaps: fw.show_next(a, 60), then 20 frames later
--       fw.show_next(b, 20): still "normal" until frame 60, not cut short.
--       Then fw.show_next(c, 60) at frame 50: "normal" until frame 110.
--   M5  frames of 0, 181, 1.5 or "60", and a label of nil, "" or 42, are
--       each refused with an error naming the rule, by both calls, and
--       change neither the speed nor the frame. (That they don't count as
--       moments either is M6's: its fixture tries them all, and its total
--       leaves them out.)
--   M6  Run through test/run_test.sh, a fixture with two 60-frame moments
--       overlapping by 30 frames passes, and its output holds both
--       "[WATCH] <label>" lines and "[WATCH] 2 moments, 1.5 s at normal
--       speed".
--   M7  A fixture that marks no moments passes, and its output says
--       "[WATCH] no moments: ...".
--   M9  fw.slow_down makes fw.speed() "normal" until fw.speed_up (both are
--       removed again by #119's last ticket, and M9 with them).
--   M10 A test library loading its own copy of the framework mid-test
--       doesn't end a moment that is playing (the M6 fixture checks that it
--       doesn't lose the count either).
--
-- (M8 belongs to #119's last ticket: a check that nothing under test/ but
-- the framework sets the speed.)
--
-- The fixtures are in test/tests/framework_fixtures/ (two_moments.lua and
-- no_moments.lua). As in test/tests/test_framework_rules.lua, io.popen
-- starts `bash test/run_test.sh <fixture>` as a separate process, with its
-- own mgba-headless, and this emulator waits (no frames pass) until it has
-- finished. Each takes about a second.
--
-- SEEING EACH FRAME INSIDE fw.show: fw.show only returns after its frames,
-- so the test can't look at fw.speed() between them itself. The framework
-- waits a frame by calling coroutine.yield, looked up each time, so for the
-- length of one fw.show the test puts its own function in its place: it
-- yields as before and then notes fw.speed() on the frame it comes back on.
--
-- Run:
--   bash test/run_test.sh test/tests/test_recording_moments.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

-- Run fn() (which waits frames through the framework) and return the speed
-- fw.speed() gave on each frame it waited, in order.
local function speeds_while(fn)
    local real_yield = coroutine.yield
    local seen = {}
    coroutine.yield = function(...)
        local r = real_yield(...)
        seen[#seen + 1] = fw.speed()
        return r
    end
    local ok, err = pcall(fn)
    coroutine.yield = real_yield
    if not ok then error(err, 0) end
    return seen
end

-- How many entries of `speeds` are `want`.
local function count(speeds, want)
    local n = 0
    for _, s in ipairs(speeds) do
        if s == want then n = n + 1 end
    end
    return n
end

-- The speed on each of the next `n` frames, waiting frame by frame.
local function next_speeds(n)
    local seen = {}
    for i = 1, n do
        fw.yield()
        seen[i] = fw.speed()
    end
    return seen
end

local RUN_TEST = project_dir .. "test/run_test.sh"
local FIXTURES = project_dir .. "test/tests/framework_fixtures/"

-- Run one fixture through run_test.sh and return its exit code and output.
-- MGBA_TEST_TIMEOUT=30: each fixture takes a second, so one that hangs
-- should fail fast, whatever limit this run was given.
local function run_fixture(name)
    local cmd = string.format("MGBA_TEST_TIMEOUT=30 bash '%s' '%s' 2>&1",
        RUN_TEST, FIXTURES .. name)
    local pipe = io.popen(cmd, "r")
    if pipe == nil then
        return nil, "io.popen failed for: " .. cmd
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    -- Nobody looks at a fixture's run folder ("Output: <its mktemp
    -- folder>"), so remove it, recognised by its mktemp name.
    local dir = output:match("Output: (%S+)")
    if dir and dir:match("/mgba%-test%.%w+$") then
        os.execute("rm -rf '" .. dir .. "'")
    end
    return code, output
end

-- M6 and M7: run fixture `c.fixture`, which must pass with each of the
-- lines `c.says` in its output.
local function expect(c)
    local code, output = run_fixture(c.fixture)
    local ok = fw.assert_eq(code, 0, c.label .. ": the fixture passes")
    for _, text in ipairs(c.says) do
        ok = fw.assert_true(output:find(text, 1, true) ~= nil,
            string.format("%s: output says \"%s\"", c.label, text)) and ok
    end
    if not ok then
        fw.log("---- output of " .. c.fixture .. " ----")
        for line in output:gmatch("[^\n]+") do fw.log("| " .. line) end
    end
end

-- M1 and M2: fw.show(label, frames) holds for `frames` frames at normal speed.
local function check_show(name, frames)
    local label = name .. " a held moment"
    local start = emu:currentFrame()
    local seen = speeds_while(function() fw.show(label, frames) end)
    local want = frames or 60
    fw.assert_eq(emu:currentFrame() - start, want,
        name .. ": fw.show returns after exactly " .. want .. " frames")
    fw.assert_eq(#seen, want, name .. ": fw.show waited " .. want .. " frames")
    fw.assert_eq(count(seen, "normal"), want,
        name .. ": fw.speed() is normal on each of them")
    fw.yield()
    fw.assert_eq(fw.speed(), "fast", name .. ": the frame after is fast again")
end

fw.run(function()
    fw.log("=== Recording moments (#124) ===")

    -- Before any moment, the window fast-forwards.
    fw.wait_frames(10)
    fw.assert_eq(fw.speed(), "fast", "before any moment the speed is fast")

    check_show("M1", nil)
    check_show("M2", 30)

    -- M3: fw.show_next marks the next 60 frames and returns at once.
    local start = emu:currentFrame()
    fw.show_next("M3 the next 60 frames")
    fw.assert_eq(emu:currentFrame() - start, 0, "M3: fw.show_next returns before any frame passes")
    local seen = next_speeds(61)
    local first60 = { table.unpack(seen, 1, 60) }
    fw.assert_eq(count(first60, "normal"), 60, "M3: the next 60 frames are normal")
    fw.assert_eq(seen[61], "fast", "M3: the 61st frame is fast")

    -- M4: a moment marked while another plays extends it, never cuts it short.
    fw.show_next("M4 a: 60 frames", 60)
    local a = next_speeds(20)                -- frames 1 to 20
    fw.show_next("M4 b: 20 frames, inside a", 20)
    local b = next_speeds(30)                -- frames 21 to 50
    fw.show_next("M4 c: 60 frames from frame 50", 60)
    local c = next_speeds(61)                -- frames 51 to 111
    fw.assert_eq(count(a, "normal") + count(b, "normal"), 50,
        "M4: frames 1 to 50 are normal (b ending at 40 doesn't cut a short)")
    fw.assert_eq(count({ table.unpack(c, 1, 60) }, "normal"), 60,
        "M4: frames 51 to 110 are normal (c carries on past a's 60)")
    fw.assert_eq(c[61], "fast", "M4: frame 111 is fast")

    -- M5: a moment with a bad label or length is refused with an error
    -- naming the rule, and changes nothing.
    local FRAMES_RULE = "frames must be a whole number from 1 to 180"
    local LABEL_RULE = "label must be a non-empty string"
    local refused = {
        { what = "frames 0",    label = "M5", frames = 0,    rule = FRAMES_RULE },
        { what = "frames 181",  label = "M5", frames = 181,  rule = FRAMES_RULE },
        { what = "frames 1.5",  label = "M5", frames = 1.5,  rule = FRAMES_RULE },
        { what = 'frames "60"', label = "M5", frames = "60", rule = FRAMES_RULE },
        { what = "label nil",   label = nil,  frames = 60,   rule = LABEL_RULE },
        { what = 'label ""',    label = "",   frames = 60,   rule = LABEL_RULE },
        { what = "label 42",    label = 42,   frames = 60,   rule = LABEL_RULE },
    }
    for _, r in ipairs(refused) do
        for _, call in ipairs({ "show", "show_next" }) do
            local name = string.format("M5 fw.%s, %s", call, r.what)
            local start = emu:currentFrame()
            local ok, err = pcall(fw[call], r.label, r.frames)
            fw.assert_true(not ok and tostring(err):find(r.rule, 1, true) ~= nil,
                name .. ": refused with an error saying \"" .. r.rule .. "\" (got "
                .. tostring(err) .. ")")
            fw.assert_eq(emu:currentFrame() - start, 0, name .. ": no frame passed")
            local after = next_speeds(3)
            fw.assert_eq(count(after, "fast"), 3, name .. ": the speed stays fast")
        end
    end

    -- M9: the older calls, kept until #119's last ticket removes them.
    fw.slow_down("M9")
    fw.assert_eq(fw.speed(), "normal", "M9: fw.slow_down makes the speed normal")
    fw.assert_eq(count(next_speeds(200), "normal"), 200,
        "M9: it stays normal past a moment's 180 frames")
    fw.speed_up()
    fw.assert_eq(fw.speed(), "fast", "M9: fw.speed_up makes it fast again")
    fw.assert_eq(count(next_speeds(10), "fast"), 10, "M9: and it stays fast")

    -- M10: a test library that loads its own copy of the framework while a
    -- moment plays (test_mt_moon_properties.lua's checks load theirs
    -- mid-test) leaves the moment, and the frame count, alone.
    fw.show_next("M10 a moment while a library loads the framework", 60)
    fw.wait_frames(10)
    dofile(project_dir .. "test/lib/framework.lua")
    local rest = next_speeds(51)
    fw.assert_eq(count({ table.unpack(rest, 1, 50) }, "normal"), 50,
        "M10: the moment's last 50 frames are still normal after the load")
    fw.assert_eq(rest[51], "fast", "M10: and it ends on time")

    expect{ label = "M6 two overlapping moments", fixture = "two_moments.lua",
            says = { "[WATCH] first moment: the next 60 frames",
                     "[WATCH] second moment: 60 frames from halfway through the first",
                     "[WATCH] 2 moments, 1.5 s at normal speed" } }
    expect{ label = "M7 no moments", fixture = "no_moments.lua",
            says = { "[WATCH] no moments: in the mGBA window this test fast-forwards throughout" } }

    fw.finish()
end)
