-------------------------------------------------------------------------------
-- test_framework_rules.lua: a test can only pass by proving something
-------------------------------------------------------------------------------
-- Checks the rules of the test framework itself (test/lib/framework.lua),
-- issue #49. It runs small fixture scripts from test/tests/framework_fixtures/
-- through test/run_test.sh, exactly as a person or the suite runner would, and
-- checks run_test.sh's exit code (0 = pass, 1 = fail) and what it printed:
--
--   R0  (control) A test with one passing assertion passes. Without this,
--       the failures below could just mean every fixture fails.
--   R1  A test that makes zero assertions fails, and the output says
--       "0 assertions".
--   R2  A demo or probe that declares fw.allow_no_assertions(reason) passes
--       with zero assertions, and the output gives the reason.
--   R3  When booting to character select fails, a test that returns early
--       through fw.finish() fails, and the output says "boot to character
--       select failed". The failure is simulated by a stub:
--       MGBA_TEST_FAKE_BOOT_FAILURE=1 makes cs.find_selection_press() fail
--       at once instead of booting (test/lib/character_select.lua).
--   R4  The same in a demo that declared fw.allow_no_assertions: the
--       opt-out doesn't hide a failed boot.
--
-- HOW A TEST CAN RUN OTHER TESTS: mGBA's Lua has the standard io library, so
-- io.popen starts `bash test/run_test.sh <fixture>` as a separate process
-- and reads everything it prints. That process runs its own mgba-headless
-- with its own private output directory; this emulator just waits (no frames
-- pass) until it has finished. Each fixture takes about a second.
--
-- Run:
--   bash test/run_test.sh test/tests/test_framework_rules.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local RUN_TEST = project_dir .. "test/run_test.sh"
local FIXTURES = project_dir .. "test/tests/framework_fixtures/"

-- Run one fixture through run_test.sh. `env` is an optional string of
-- VAR=value words put in front of the command. Returns run_test.sh's exit
-- code and everything it printed.
--
-- MGBA_TEST_TIMEOUT=30 overrides whatever this run was given (a whole-suite
-- run passes a long one): every fixture finishes in a few seconds, so one
-- that hangs should fail fast.
local function run_fixture(name, env)
    local cmd = string.format("%s MGBA_TEST_TIMEOUT=30 bash '%s' '%s' 2>&1",
        env or "", RUN_TEST, FIXTURES .. name)
    local pipe = io.popen(cmd, "r")
    if pipe == nil then
        return nil, "io.popen failed for: " .. cmd
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()

    -- run_test.sh keeps each run's output directory and prints its path
    -- ("Output: /tmp/mgba-test.XXXXXX"). Nobody looks at a fixture's
    -- folder, so remove it rather than leave one behind per fixture.
    local dir = output:match("Output: (/tmp/mgba%-test%.%w+)")
    if dir then
        os.execute("rm -rf '" .. dir .. "'")
    end

    fw.log(string.format("fixture %s%s -> exit %s", env and (env .. " ") or "",
        name, tostring(code)))
    return code, output
end

-- Check one fixture's verdict: run_test.sh's exit code, and (when given) a
-- piece of text its output must contain.
local function expect(label, name, env, want_code, want_text)
    local code, output = run_fixture(name, env)
    local ok = fw.assert_eq(code, want_code, label .. ": run_test.sh exit code")
    if want_text then
        ok = fw.assert_true(output:find(want_text, 1, true) ~= nil,
            string.format("%s: output says \"%s\"", label, want_text)) and ok
    end
    if not ok then
        fw.log("---- output of " .. name .. " ----")
        for line in output:gmatch("[^\n]+") do fw.log("| " .. line) end
    end
end

fw.run(function()
    fw.log("=== Test framework rules (issue #49) ===")

    expect("R0 one passing assertion", "one_assertion.lua", nil, 0, "1/1 passed")
    expect("R1 zero assertions", "zero_assertions.lua", nil, 1, "0 assertions")
    expect("R2 zero assertions, declared", "no_assertions_allowed.lua", nil, 0,
        "stands in for a demo that only makes frames")
    expect("R3 boot fails", "boot_fails.lua", "MGBA_TEST_FAKE_BOOT_FAILURE=1", 1,
        "boot to character select failed")
    expect("R4 boot fails in a demo", "boot_fails_in_demo.lua",
        "MGBA_TEST_FAKE_BOOT_FAILURE=1", 1, "boot to character select failed")

    fw.finish()
end)
