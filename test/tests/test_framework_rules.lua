-------------------------------------------------------------------------------
-- test_framework_rules.lua: a test can only pass by proving something
-------------------------------------------------------------------------------
-- Checks the rules of the test framework itself (test/lib/framework.lua),
-- issue #49. It runs small fixture scripts from test/tests/framework_fixtures/
-- through test/run_test.sh, exactly as a person would, and checks
-- run_test.sh's exit code (0 = pass, 1 = fail) and what it printed:
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

-- Run one fixture through run_test.sh, with the environment variables in
-- `env` (a table of name = value, may be nil). Returns run_test.sh's exit
-- code and everything it printed.
--
-- MGBA_TEST_TIMEOUT=30 overrides whatever this run was given (a whole-suite
-- run may pass a long one): every fixture finishes in a few seconds, so one
-- that hangs should fail fast.
local function run_fixture(name, env)
    local settings = { "MGBA_TEST_TIMEOUT=30" }
    for var, value in pairs(env or {}) do
        settings[#settings + 1] = string.format("%s='%s'", var, value)
    end
    local cmd = string.format("%s bash '%s' '%s' 2>&1",
        table.concat(settings, " "), RUN_TEST, FIXTURES .. name)
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

    fw.log(string.format("fixture %s (%s) -> exit %s", name,
        table.concat(settings, " "), tostring(code)))
    return code, output
end

-- Run one fixture and check its verdict. `c` holds:
--   label    the rule being checked, for the assertion messages
--   fixture  the file in test/tests/framework_fixtures/
--   env      optional environment variables (name = value)
--   exit     the exit code run_test.sh must give
--   says     a piece of text run_test.sh's output must contain
local function expect(c)
    local code, output = run_fixture(c.fixture, c.env)
    local ok = fw.assert_eq(code, c.exit, c.label .. ": run_test.sh exit code")
    ok = fw.assert_true(output:find(c.says, 1, true) ~= nil,
        string.format("%s: output says \"%s\"", c.label, c.says)) and ok
    if not ok then
        fw.log("---- output of " .. c.fixture .. " ----")
        for line in output:gmatch("[^\n]+") do fw.log("| " .. line) end
    end
end

local FAKE_BOOT_FAILURE = { MGBA_TEST_FAKE_BOOT_FAILURE = "1" }

fw.run(function()
    fw.log("=== Test framework rules (issue #49) ===")

    expect{ label = "R0 one passing assertion", fixture = "one_assertion.lua",
            exit = 0, says = "1/1 passed" }
    expect{ label = "R1 zero assertions", fixture = "zero_assertions.lua",
            exit = 1, says = "0 assertions" }
    expect{ label = "R2 zero assertions, declared", fixture = "no_assertions_allowed.lua",
            exit = 0, says = "stands in for a demo that only makes frames" }
    expect{ label = "R3 boot fails", fixture = "boot_fails.lua",
            env = FAKE_BOOT_FAILURE,
            exit = 1, says = "boot to character select failed" }
    expect{ label = "R4 boot fails in a demo", fixture = "boot_fails_in_demo.lua",
            env = FAKE_BOOT_FAILURE,
            exit = 1, says = "boot to character select failed" }

    fw.finish()
end)
