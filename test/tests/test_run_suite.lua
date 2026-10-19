-------------------------------------------------------------------------------
-- test_run_suite.lua: the whole-suite runner, test/run_suite.sh (issue #54)
-------------------------------------------------------------------------------
-- Runs test/run_suite.sh on the small fixture tests in
-- test/tests/suite_fixtures/, exactly as a person would, and checks what it
-- does from the outside: its exit code, its summary, and which folders it
-- leaves behind. A4 and A5 are the names of issue #54's acceptance criteria.
--
--   A4         A test listed in the exclusion file is not run, and the
--              summary lists it as SKIPPED with its reason. The excluded
--              fixture fails if it runs, so the suite exiting 0 proves it
--              was left out.
--   A5         With one test failing next to two passing ones, the suite
--              exits 1, names the failing test, keeps the failing run's
--              folder (with its full output in run_test.log) and deletes the
--              passing runs' folders.
--   TIMEOUT    A test stopped by its time limit is reported as TIMEOUT, and
--              its run folder is kept.
--   NO-REASON  An exclusion with no reason stops the suite (exit 2) before
--              any test runs: every exclusion needs a reason.
--   STALE      So does an exclusion naming a test that doesn't exist (one
--              renamed or deleted since it was listed).
--
-- The fixtures (test/tests/suite_fixtures/): passes_a.lua and passes_b.lua
-- pass, fails.lua fails, hangs.lua never finishes, and the exclude_*.txt
-- files are exclusion lists in test/suite_exclude.txt's format.
--
-- WHICH FOLDERS ARE LEFT: every run_suite.sh started here gets TMPDIR set to
-- a new, empty folder inside this run's own output folder. run_test.sh makes
-- each run's folder in TMPDIR (default /tmp), and run_suite.sh makes its
-- scratch folder there too, so listing that one folder afterwards shows
-- exactly what the suite left behind, whatever else is running on this
-- machine at the same time.
--
-- HOW A TEST CAN RUN THE SUITE: mGBA's Lua has the standard io library, so
-- io.popen starts `bash test/run_suite.sh ...` as a separate process and
-- reads everything it prints. That suite starts its own mgba-headless for
-- each fixture; this emulator just waits (no frames pass) until it is done.
-- The whole test takes about 5 seconds.
--
-- Run:
--   bash test/run_test.sh test/tests/test_run_suite.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local RUN_SUITE = project_dir .. "test/run_suite.sh"
local FIXTURES = project_dir .. "test/tests/suite_fixtures/"

-- `s` in single quotes, safe to paste into a shell command.
local function sh_quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- The names of everything in folder `dir`, sorted.
local function list_dir(dir)
    local names = {}
    local pipe = io.popen("ls -A " .. sh_quote(dir) .. " 2>/dev/null")
    if pipe then
        for name in pipe:lines() do names[#names + 1] = name end
        pipe:close()
    end
    table.sort(names)
    return names
end

-- The contents of file `path`, or "" if it can't be read.
local function read_file(path)
    local f = io.open(path, "r")
    if not f then return "" end
    local text = f:read("a") or ""
    f:close()
    return text
end

-- The rows of run_suite.sh's final summary, by test name. Each row is
--   <RESULT> <test name> <rest of the line>
-- where RESULT is PASS, FAIL, TIMEOUT or SKIPPED. Only lines after the
-- "=== Summary" heading count, so the progress lines printed while the
-- tests run are not mistaken for the summary.
local RESULTS = { PASS = true, FAIL = true, TIMEOUT = true, SKIPPED = true }
local function summary_rows(output)
    local rows = {}
    local in_summary = false
    for line in output:gmatch("[^\n]+") do
        if line:find("=== Summary", 1, true) then
            in_summary = true
        elseif in_summary then
            local result, name, rest = line:match("^(%u+)%s+(%S+)%s*(.-)%s*$")
            if result and RESULTS[result] then
                rows[name] = { result = result, rest = rest }
            end
        end
    end
    return rows
end

-- Run test/run_suite.sh on fixtures, with TMPDIR set to a new folder of its
-- own (see WHICH FOLDERS ARE LEFT). `c` holds:
--   scenario  names that folder
--   exclude   the exclusion list, a file in suite_fixtures/
--   tests     the fixture tests to run, files in suite_fixtures/
--   timeout   each test's time limit in seconds (default 30: every fixture
--             but hangs.lua finishes in a second or two, so one that hangs
--             by mistake fails this test fast)
-- Returns a table:
--   code    run_suite.sh's exit code
--   output  everything it printed
--   rows    its summary rows (summary_rows above)
--   tmp     the TMPDIR folder it was given
--   left    the names of everything left in that folder afterwards
local function run_suite(c)
    local tmp = fw.out_path("suite-" .. c.scenario)
    os.execute("mkdir -p " .. sh_quote(tmp))
    local args = { "--timeout", tostring(c.timeout or 30),
                   "--exclude", sh_quote(FIXTURES .. c.exclude) }
    for _, test in ipairs(c.tests) do
        args[#args + 1] = sh_quote(FIXTURES .. test)
    end
    local cmd = "TMPDIR=" .. sh_quote(tmp) .. " bash " .. sh_quote(RUN_SUITE)
        .. " " .. table.concat(args, " ") .. " 2>&1"
    local r = { tmp = tmp, output = "", rows = {}, left = {} }
    local pipe = io.popen(cmd, "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return r
    end
    r.output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    r.code = code
    r.rows = summary_rows(r.output)
    r.left = list_dir(tmp)
    fw.log(string.format("%s: run_suite.sh --exclude %s %s -> exit %s, left: [%s]",
        c.scenario, c.exclude, table.concat(c.tests, " "), tostring(code),
        table.concat(r.left, " ")))
    return r
end

-- The result run_suite.sh's summary gives test `name` (nil if not listed).
local function result_of(r, name)
    return r.rows[name] and r.rows[name].result
end

-- The rest of test `name`'s summary row after its name ("" if not listed):
-- the seconds and kept folder, or a skipped test's reason.
local function rest_of(r, name)
    return r.rows[name] and r.rows[name].rest or ""
end

-- Log everything a suite run printed, so a failed check can be diagnosed
-- from this test's own output.
local function dump(label, r)
    fw.log("---- output of run_suite.sh, " .. label .. " ----")
    for line in r.output:gmatch("[^\n]+") do fw.log("| " .. line) end
end

-------------------------------------------------------------------------------
-- A4: an excluded test isn't run, and the summary gives its reason
-------------------------------------------------------------------------------
local function check_exclusion()
    local r = run_suite{ scenario = "a4", exclude = "exclude_fails.txt",
                         tests = { "passes_a.lua", "fails.lua" } }
    local ok = fw.assert_eq(r.code, 0,
        "A4: suite exits 0, so fails.lua (which fails if run) was left out")
    ok = fw.assert_eq(result_of(r, "passes_a"), "PASS",
        "A4: summary lists passes_a as PASS") and ok
    ok = fw.assert_eq(result_of(r, "fails"), "SKIPPED",
        "A4: summary lists fails as SKIPPED") and ok
    ok = fw.assert_true(rest_of(r, "fails"):find("fixture: would fail the suite if it ran", 1, true) ~= nil,
        "A4: summary gives the reason from the exclusion file, got: " .. rest_of(r, "fails")) and ok
    ok = fw.assert_eq(#r.left, 0,
        "A4: no folder left (fails never ran, passes_a's run folder deleted), left: "
        .. table.concat(r.left, " ")) and ok
    if not ok then dump("A4", r) end
end

-------------------------------------------------------------------------------
-- A5: one failing test fails the suite, and only its run folder is kept
-------------------------------------------------------------------------------
local function check_one_failure()
    local r = run_suite{ scenario = "a5", exclude = "exclude_nothing.txt",
                         tests = { "passes_a.lua", "fails.lua", "passes_b.lua" } }
    local ok = fw.assert_eq(r.code, 1, "A5: suite exits 1 when a test fails")
    ok = fw.assert_eq(result_of(r, "fails"), "FAIL",
        "A5: summary lists fails as FAIL") and ok
    ok = fw.assert_eq(result_of(r, "passes_a"), "PASS",
        "A5: summary lists passes_a as PASS") and ok
    ok = fw.assert_eq(result_of(r, "passes_b"), "PASS",
        "A5: summary lists passes_b as PASS") and ok
    ok = fw.assert_true(r.output:find("SUITE FAILED: fails\n", 1, true) ~= nil,
        "A5: the last line names the failing test: SUITE FAILED: fails") and ok

    -- Exactly one folder is left: the failing run's. Both passing runs'
    -- folders and the suite's scratch folder are gone.
    ok = fw.assert_eq(#r.left, 1,
        "A5: one folder left, left: " .. table.concat(r.left, " ")) and ok
    local kept = r.left[1] or ""
    ok = fw.assert_true(kept:match("^mgba%-test%.%w+$") ~= nil,
        "A5: the folder left is a run folder, got: " .. kept) and ok
    local kept_dir = r.tmp .. "/" .. kept
    ok = fw.assert_true(read_file(kept_dir .. "/mgba-test-results.json"):find('"fail": 1', 1, true) ~= nil,
        "A5: the folder left is the failing run's (its results say 1 failure)") and ok
    ok = fw.assert_true(read_file(kept_dir .. "/run_test.log"):find("false on purpose", 1, true) ~= nil,
        "A5: the folder left holds the run's full output, run_test.log") and ok
    ok = fw.assert_true(rest_of(r, "fails"):find("kept " .. kept_dir, 1, true) ~= nil,
        "A5: the summary row of fails gives the kept folder, got: " .. rest_of(r, "fails")) and ok
    if not ok then dump("A5", r) end
end

-------------------------------------------------------------------------------
-- TIMEOUT: a test stopped by its time limit is reported as a timeout
-------------------------------------------------------------------------------
-- hangs.lua never finishes, so with a 5-second limit run_test.sh's `timeout`
-- stops it after 5 seconds. The suite must call that TIMEOUT (from
-- timeout's exit code, 124), not FAIL, and keep its run folder like any
-- other failure's.
local function check_timeout()
    local r = run_suite{ scenario = "timeout", exclude = "exclude_nothing.txt",
                         tests = { "hangs.lua" }, timeout = 5 }
    local ok = fw.assert_eq(r.code, 1, "TIMEOUT: suite exits 1 when a test times out")
    ok = fw.assert_eq(result_of(r, "hangs"), "TIMEOUT",
        "TIMEOUT: summary lists hangs as TIMEOUT") and ok
    ok = fw.assert_true(r.output:find("1 timed out", 1, true) ~= nil,
        "TIMEOUT: the totals line counts 1 timed out") and ok
    ok = fw.assert_true(r.output:find("SUITE FAILED: hangs\n", 1, true) ~= nil,
        "TIMEOUT: the last line names the test that timed out") and ok
    ok = fw.assert_eq(#r.left, 1,
        "TIMEOUT: its run folder is kept, left: " .. table.concat(r.left, " ")) and ok
    ok = fw.assert_true(rest_of(r, "hangs"):find("kept " .. r.tmp .. "/" .. (r.left[1] or "?"), 1, true) ~= nil,
        "TIMEOUT: the summary row of hangs gives the kept folder, got: " .. rest_of(r, "hangs")) and ok
    if not ok then dump("TIMEOUT", r) end
end

-------------------------------------------------------------------------------
-- NO-REASON and STALE: a bad exclusion line stops the suite before it starts
-------------------------------------------------------------------------------
-- Runs passes_a.lua with the exclusion list `exclude`, which has a bad line.
-- The suite must exit 2 (could not start), say `says`, and run nothing: no
-- summary, and no folder left behind.
local function expect_refused(label, exclude, says)
    local r = run_suite{ scenario = label:lower(), exclude = exclude,
                         tests = { "passes_a.lua" } }
    local ok = fw.assert_eq(r.code, 2, label .. ": suite exits 2 (could not start)")
    ok = fw.assert_true(r.output:find(says, 1, true) ~= nil,
        label .. ": the error says: " .. says) and ok
    ok = fw.assert_true(r.output:find("=== Summary", 1, true) == nil and #r.left == 0,
        label .. ": no test ran (no summary, no folder), left: " .. table.concat(r.left, " ")) and ok
    if not ok then dump(label, r) end
end

fw.run(function()
    fw.log("=== Whole-suite runner, test/run_suite.sh (issue #54) ===")
    check_exclusion()
    check_one_failure()
    check_timeout()
    expect_refused("NO-REASON", "exclude_no_reason.txt", "'fails' has no reason")
    expect_refused("STALE", "exclude_unknown.txt", "'no_such_test' is not a test")
    fw.finish()
end)
