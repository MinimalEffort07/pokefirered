-------------------------------------------------------------------------------
-- test_recording_notes.lua: recordings carry notes, and the recordings page
-- shows them (issue #110, part of #103)
-------------------------------------------------------------------------------
-- A recording's notes say which ticket, feature (the parent issue) and PR it
-- belongs to, what it tests, what the problem was and what you should see.
-- They are written when it is recorded:
--
--   bash test/record_test.sh --notes FILE TEST     (FILE may be - for stdin)
--
-- and record_test.sh keeps them, with what it knows itself (when, the test,
-- the branch and commit, the run's result), in <name>.json beside the video,
-- through test/tools/recording_notes.py. test/serve_recordings.py shows them
-- on the recording's card. C1 to C7 are the names of issue #110's acceptance
-- criteria:
--
--   C1  Accepted: record_test.sh --check exits 0 with no notes, and with
--       complete notes, from a file or from stdin.
--   C2  Refused: --check exits 1, with a message naming the problem, for a
--       missing, empty or repeated section, an unknown ## heading, an
--       unknown header key, a ticket: without a title, a pr: that isn't a
--       number, and a notes file that doesn't exist.
--   C3  The notes file: given complete notes and a results file with 215
--       passes and no fails, the helper writes every field with the right
--       values; with no results file "result" is null, and a demo's 0 and 0
--       stay {"pass": 0, "fail": 0}. Without notes, the ticket, parent, PR
--       and notes are null. The branch, commit and uncommitted changes come
--       from a small git repo the test makes, and the test's path is from
--       that repo's root, or a full path for a test outside it.
--   C4  The name: --check says the video would be named
--       issue-90_<test>_<date>_<time>.mp4 for notes naming ticket 90, and
--       <test>_<date>_<time>.mp4, as before, without a ticket.
--
-- NEVER A REAL RECORDING: every record_test.sh command here starts with
-- --check, which checks the arguments and the notes and exits before the
-- recording lock. A real recording would close every mGBA window on the
-- machine, other sessions' included. --check comes first on purpose: a
-- record_test.sh that doesn't know it yet takes it for the test's file name,
-- finds no such file, and stops right there.
--
-- THE SESSION VARIABLES: autodev's dispatcher starts every session with
-- AUTODEV_ISSUE, AUTODEV_ROLE and, for some roles, AUTODEV_PR and
-- AUTODEV_HANDLED_THROUGH (launch_command in automation/autodev/sessions.py).
-- A later ticket of #103 gives them a meaning to record_test.sh, so every
-- command here clears them (env -u), and this test checks the same whoever
-- runs it.
--
-- WHERE IT WORKS: every file this test makes is in this run's own folder
-- (fw.out_path), so runs at the same time never meet.
--
-- HOW A TEST CAN RUN SHELL SCRIPTS: mGBA's Lua has the standard io library,
-- so io.popen starts each command as a separate process and reads what it
-- prints; the emulator just waits (no frames pass) until it is done. The
-- whole test takes a few seconds.
--
-- Run:
--   bash test/run_test.sh test/tests/test_recording_notes.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local RECORD = project_dir .. "test/record_test.sh"
local NOTES_TOOL = project_dir .. "test/tools/recording_notes.py"
-- A real test for --check to accept. It is never run.
local SOME_TEST = project_dir .. "test/tests/test_player_sprite.lua"

-- Every command runs without the session variables (see THE SESSION
-- VARIABLES above).
local CLEAN_ENV = "env -u AUTODEV_ISSUE -u AUTODEV_ROLE -u AUTODEV_PR -u AUTODEV_HANDLED_THROUGH "

-- `s` in single quotes, safe to paste into a shell command.
local function sh_quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- Run shell command `cmd` without the session variables. Returns its exit
-- code and everything it printed (stdout and stderr together).
local function run(cmd)
    local pipe = io.popen(CLEAN_ENV .. cmd .. " 2>&1", "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return nil, ""
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    return code, output
end

-- Write `text` to file `path`.
local function write_file(path, text)
    local f = io.open(path, "w")
    if not f then
        fw.log_error("can't write " .. path)
        return
    end
    f:write(text)
    f:close()
end

-- Log what a command printed, so a failed check can be diagnosed from this
-- test's output.
local function dump(label, output)
    fw.log("---- " .. label .. " ----")
    for line in output:gmatch("[^\n]+") do fw.log("| " .. line) end
end

local WORK = fw.out_path("notes-work")
os.execute("mkdir -p " .. sh_quote(WORK))

-- `record_test.sh --check` with `args` (already shell-quoted), on SOME_TEST.
local function check(args)
    return run("bash " .. sh_quote(RECORD) .. " --check " .. args .. " " .. sh_quote(SOME_TEST))
end

-- Write `text` as notes file `name` in WORK and return its path.
local function notes_file(name, text)
    local path = WORK .. "/" .. name
    write_file(path, text)
    return path
end

-------------------------------------------------------------------------------
-- Notes
-------------------------------------------------------------------------------

-- Complete notes, as the ticket's example. The text has a < and a & (C5
-- checks they are escaped on the page) and a line break kept as written.
local TESTED = "The radar's four patches after closing the BAG or POKéMON,\n"
    .. "after Continue, and after a warp: 4 < 5 & so on."
local PROBLEM = "Rebuilding the field destroyed the patch sprites, but the radar\n"
    .. "still counted them as live."
local EXPECT = "Four patches shake. The BAG opens and closes, and the same four still shake."

local COMPLETE = "ticket: 90 Shaking patches survive a field reload\n"
    .. "parent: #87 The Poke Radar\n"
    .. "pr: 97\n"
    .. "\n"
    .. "## What's tested\n" .. TESTED .. "\n"
    .. "\n"
    .. "## The problem\n" .. PROBLEM .. "\n"
    .. "\n"
    .. "## What you should see\n" .. EXPECT .. "\n"

-- The three sections, in any order, with no headers: also complete.
local SECTIONS_ONLY = "## What you should see\n" .. EXPECT .. "\n"
    .. "## The problem\n" .. PROBLEM .. "\n"
    .. "## What's tested\n" .. TESTED .. "\n"

-------------------------------------------------------------------------------
-- C1: accepted
-------------------------------------------------------------------------------
local function check_accepted()
    local code, output = check("")
    if not fw.assert_eq(code, 0, "C1: --check with no notes exits 0") then dump("no notes", output) end

    for _, case in ipairs({ { "complete notes", COMPLETE },
                            { "the three sections only, in another order", SECTIONS_ONLY } }) do
        local path = notes_file("accepted.txt", case[2])
        code, output = check("--notes " .. sh_quote(path))
        if not fw.assert_eq(code, 0, "C1: --check with " .. case[1] .. " exits 0") then
            dump(case[1], output)
        end
    end

    -- --notes - reads the notes from stdin.
    local path = notes_file("stdin.txt", COMPLETE)
    code, output = run("bash " .. sh_quote(RECORD) .. " --check --notes - "
        .. sh_quote(SOME_TEST) .. " < " .. sh_quote(path))
    if not fw.assert_eq(code, 0, "C1: --check --notes - with complete notes on stdin exits 0") then
        dump("stdin", output)
    end
    code, output = run("bash " .. sh_quote(RECORD) .. " --check --notes - "
        .. sh_quote(SOME_TEST) .. " < /dev/null")
    if not fw.assert_eq(code, 1, "C1: --check --notes - with nothing on stdin exits 1") then
        dump("empty stdin", output)
    end
end

-------------------------------------------------------------------------------
-- C2: refused
-------------------------------------------------------------------------------

-- `text` with the line `line` (and its line break) taken out.
local function without(text, line)
    local s, e = text:find(line .. "\n", 1, true)
    if not s then
        fw.log_error("fixture: no line " .. line)
        return text
    end
    return text:sub(1, s - 1) .. text:sub(e + 1)
end

-- Each refused case: what it is, its notes, and the words its message must
-- hold (all of them, matched as plain text).
local REFUSED = {
    { what = "a missing section",
      notes = without(without(COMPLETE, "## The problem"), PROBLEM),
      says = { "The problem", "missing" } },
    { what = "an empty section",
      notes = COMPLETE:gsub("## The problem\n.-\n\n", "## The problem\n   \n\n"),
      says = { "The problem", "empty" } },
    { what = "a section given twice",
      notes = COMPLETE .. "\n## What's tested\nAgain.\n",
      says = { "What's tested", "twice" } },
    { what = "an unknown ## heading",
      notes = COMPLETE .. "\n## Extra thoughts\nMore.\n",
      says = { "Extra thoughts", "heading" } },
    { what = "an unknown header key",
      notes = COMPLETE:gsub("^ticket:", "tickt:"),
      says = { "tickt", "header" } },
    { what = "a ticket: without a title",
      notes = COMPLETE:gsub("^ticket: [^\n]*", "ticket: 90"),
      says = { "ticket:", "title" } },
    { what = "a pr: that isn't a number",
      notes = COMPLETE:gsub("pr: 97", "pr: ninety-seven"),
      says = { "pr:", "number", "ninety-seven" } },
}

local function check_refused()
    for _, case in ipairs(REFUSED) do
        local path = notes_file("refused.txt", case.notes)
        local code, output = check("--notes " .. sh_quote(path))
        local ok = fw.assert_eq(code, 1, "C2: --check refuses " .. case.what .. " (exit 1)")
        for _, word in ipairs(case.says) do
            ok = fw.assert_true(output:find(word, 1, true) ~= nil,
                "C2: the message for " .. case.what .. " says \"" .. word .. "\"") and ok
        end
        if not ok then
            dump(case.what .. ": notes", case.notes)
            dump(case.what .. ": output", output)
        end
    end

    -- A notes file that doesn't exist.
    local missing = WORK .. "/no_such_notes.txt"
    local code, output = check("--notes " .. sh_quote(missing))
    local ok = fw.assert_eq(code, 1, "C2: --check refuses a notes file that doesn't exist (exit 1)")
    ok = fw.assert_true(output:find(missing, 1, true) ~= nil and output:find("not found", 1, true) ~= nil,
        "C2: the message names the missing file and says it is not found") and ok
    if not ok then dump("missing notes file", output) end
end

-------------------------------------------------------------------------------
-- C4: the name
-------------------------------------------------------------------------------
local function check_name()
    local path = notes_file("named.txt", COMPLETE)
    local code, output = check("--notes " .. sh_quote(path))
    local name = output:match("/([^/%s]+%.mp4)")
    local ok = fw.assert_eq(code, 0, "C4: --check with ticket 90's notes exits 0")
    ok = fw.assert_true(name ~= nil and name:match("^issue%-90_test_player_sprite_%d%d%d%d%d%d%d%d_%d%d%d%d%d%d%.mp4$") ~= nil,
        "C4: with ticket 90 the video is named issue-90_test_player_sprite_<date>_<time>.mp4: "
        .. tostring(name)) and ok
    if not ok then dump("named", output) end

    for _, case in ipairs({ { "no notes", "" },
                            { "notes without a ticket", "--notes " .. sh_quote(notes_file("unnamed.txt", SECTIONS_ONLY)) } }) do
        code, output = check(case[2])
        name = output:match("/([^/%s]+%.mp4)")
        ok = fw.assert_true(name ~= nil and name:match("^test_player_sprite_%d%d%d%d%d%d%d%d_%d%d%d%d%d%d%.mp4$") ~= nil,
            "C4: with " .. case[1] .. " the video is named test_player_sprite_<date>_<time>.mp4: "
            .. tostring(name))
        if not ok then dump(case[1], output) end
    end
end

-------------------------------------------------------------------------------
-- C3: the notes file
-------------------------------------------------------------------------------

-- `s` as JSON writes a string (the helper writes UTF-8 as it is).
local function json_string(s)
    return '"' .. s:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub("\n", "\\n") .. '"'
end

-- The fields of notes file `path`, read by Python's own json module: a
-- table from each top-level key to its value written back as JSON, keys
-- sorted, plus `keys`, every key in file order, and `error` when the file
-- can't be read as JSON.
local function read_json(path)
    local reader = "import json, sys\n"
        .. "d = json.load(open(sys.argv[1], encoding='utf-8'))\n"
        .. "print('keys=' + ' '.join(d))\n"
        .. "for k, v in d.items():\n"
        .. "    print(k + '=' + json.dumps(v, sort_keys=True, ensure_ascii=False))\n"
    local code, output = run("python3 -c " .. sh_quote(reader) .. " " .. sh_quote(path))
    local fields = {}
    if code ~= 0 then
        fields.error = output
        return fields
    end
    for key, value in output:gmatch("([%w_]+)=([^\n]*)") do
        fields[key] = value
    end
    return fields
end

-- The first line `cmd` prints, without its line break ("" if none).
local function first_line(cmd)
    local _, output = run(cmd)
    return output:match("^([^\n]*)") or ""
end

local REPO = WORK .. "/repo"
local BRANCH = "issue-90-shaking-patches"
local REPO_TEST = REPO .. "/test/tests/test_x.lua"
local OUTSIDE_TEST = WORK .. "/elsewhere/test_y.lua"
local RECORDED_AT = "2026-10-20T13:45:54+11:00"

-- A git repo of its own, on branch BRANCH with one commit, holding a test
-- file and a README. Its commit is made with a fixed name and email and
-- unsigned, so the machine's git settings don't matter.
local function make_repo()
    local q = sh_quote(REPO)
    local git = "git -C " .. q .. " -c user.name=test -c user.email=test@example.com "
        .. "-c commit.gpgsign=false "
    local cmd = "mkdir -p " .. sh_quote(REPO .. "/test/tests") .. " " .. sh_quote(WORK .. "/elsewhere")
        .. " && git init -q -b " .. BRANCH .. " " .. q
        .. " && echo 'fw = nil' > " .. sh_quote(REPO_TEST)
        .. " && echo 'readme' > " .. sh_quote(REPO .. "/README")
        .. " && echo 'fw = nil' > " .. sh_quote(OUTSIDE_TEST)
        .. " && " .. git .. "add -A && " .. git .. "commit -q -m fixture"
    local code, output = run(cmd)
    if code ~= 0 then fw.log_error("fixture repo failed: " .. output) end
end

-- Write a results file like test/lib/framework.lua's and return its path.
local function results_file(name, pass, fail)
    local path = WORK .. "/" .. name
    write_file(path, string.format('{\n  "pass": %d,\n  "fail": %d,\n  "errors": []\n}\n', pass, fail))
    return path
end

-- Run the helper's write on video `video` (made here, a few zero bytes)
-- with `extra` arguments (shell-quoted). Returns the exit code, what it
-- printed and the fields of the notes file it wrote.
local function write_notes(video, test_path, results, extra)
    write_file(video, "\0\0\0\0")
    local code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " write " .. sh_quote(video)
        .. " --test " .. sh_quote(test_path) .. " --recorded-at " .. RECORDED_AT
        .. " --results " .. sh_quote(results) .. " --repo " .. sh_quote(REPO) .. " " .. (extra or ""))
    return code, output, read_json(video:gsub("%.mp4$", ".json"))
end

local function check_notes_file()
    make_repo()
    local full_commit = first_line("git -C " .. sh_quote(REPO) .. " rev-parse HEAD")
    local notes = notes_file("complete.txt", COMPLETE)
    local passed = results_file("passed.json", 215, 0)

    -- Complete notes, 215 passes.
    local video = WORK .. "/issue-90_test_x_20261020_134554.mp4"
    local code, output, f = write_notes(video, REPO_TEST, passed, "--notes " .. sh_quote(notes))
    local ok = fw.assert_eq(code, 0, "C3: the helper writes the notes file (exit 0)")
    ok = fw.assert_eq(f.error, nil, "C3: the notes file is JSON") and ok
    ok = fw.assert_eq(f.keys, "video recorded_at test branch commit uncommitted_changes result "
        .. "ticket parent pr notes", "C3: the notes file has every field, in order") and ok
    local want = {
        video = '"issue-90_test_x_20261020_134554.mp4"',
        recorded_at = '"' .. RECORDED_AT .. '"',
        test = '"test/tests/test_x.lua"',
        branch = '"' .. BRANCH .. '"',
        uncommitted_changes = "false",
        result = '{"fail": 0, "pass": 215}',
        ticket = '{"number": 90, "title": "Shaking patches survive a field reload"}',
        parent = '{"number": 87, "title": "The Poke Radar"}',
        pr = "97",
        notes = '{"expect": ' .. json_string(EXPECT) .. ', "problem": ' .. json_string(PROBLEM)
            .. ', "tested": ' .. json_string(TESTED) .. '}',
    }
    for _, key in ipairs({ "video", "recorded_at", "test", "branch", "uncommitted_changes",
                           "result", "ticket", "parent", "pr", "notes" }) do
        ok = fw.assert_eq(f[key], want[key], "C3: complete notes: " .. key) and ok
    end
    local commit = (f.commit or ""):match('^"(%x+)"$')
    ok = fw.assert_true(commit ~= nil and #commit >= 7 and full_commit:sub(1, #commit) == commit,
        "C3: commit is the repo's commit, shortened: " .. tostring(f.commit) .. " of " .. full_commit) and ok
    if not ok then dump("write, complete notes", output) end

    -- No results file: the test didn't finish.
    video = WORK .. "/no_result.mp4"
    code, output, f = write_notes(video, REPO_TEST, WORK .. "/no_such_results.json", "--notes " .. sh_quote(notes))
    ok = fw.assert_eq(code, 0, "C3: no results file: the helper still writes (exit 0)")
    ok = fw.assert_eq(f.result, "null", "C3: no results file: result is null") and ok
    if not ok then dump("write, no results", output) end

    -- A demo's 0 and 0, and no notes at all: the notes' fields are null.
    video = WORK .. "/demo_0_0.mp4"
    code, output, f = write_notes(video, REPO_TEST, results_file("demo.json", 0, 0))
    ok = fw.assert_eq(code, 0, "C3: no notes: the helper still writes (exit 0)")
    ok = fw.assert_eq(f.result, '{"fail": 0, "pass": 0}', "C3: a demo's 0 and 0 stay 0 and 0") and ok
    for _, key in ipairs({ "ticket", "parent", "pr", "notes" }) do
        ok = fw.assert_eq(f[key], "null", "C3: no notes: " .. key .. " is null") and ok
    end
    if not ok then dump("write, no notes", output) end

    -- 3 failed of 215. The framework writes an error message's line breaks
    -- as they are, which strict JSON forbids; the counts are still read.
    video = WORK .. "/failed.mp4"
    local failed = WORK .. "/failed.json"
    write_file(failed, '{\n  "pass": 212,\n  "fail": 3,\n  "errors": ["a\nb", "c", "d"]\n}\n')
    code, output, f = write_notes(video, REPO_TEST, failed)
    ok = fw.assert_eq(f.result, '{"fail": 3, "pass": 212}', "C3: 3 failed: result is 212 and 3")
    if not ok then dump("write, failed", output) end

    -- A test outside the repo: its full path.
    video = WORK .. "/outside.mp4"
    code, output, f = write_notes(video, OUTSIDE_TEST, passed)
    local real = first_line("realpath " .. sh_quote(OUTSIDE_TEST))
    ok = fw.assert_eq(f.test, '"' .. real .. '"', "C3: a test outside the repo: its full path")
    if not ok then dump("write, outside", output) end

    -- An untracked file isn't a change to the commit; a changed tracked
    -- file is.
    write_file(REPO .. "/untracked.txt", "new\n")
    video = WORK .. "/untracked.mp4"
    code, output, f = write_notes(video, REPO_TEST, passed)
    ok = fw.assert_eq(f.uncommitted_changes, "false", "C3: only an untracked file: uncommitted_changes is false")
    write_file(REPO .. "/README", "changed\n")
    video = WORK .. "/changed.mp4"
    code, output, f = write_notes(video, REPO_TEST, passed)
    ok = fw.assert_eq(f.uncommitted_changes, "true", "C3: a tracked file changed: uncommitted_changes is true") and ok
    if not ok then dump("write, changes", output) end
end

-------------------------------------------------------------------------------
-- Test
-------------------------------------------------------------------------------

fw.run(function()
    fw.log("=== Recording notes (record_test.sh --check) ===")
    check_accepted()
    check_refused()
    check_name()
    check_notes_file()
    fw.finish()
end)
