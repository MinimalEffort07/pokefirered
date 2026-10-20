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
--   C5  The card: a recording whose notes file has a ticket, a parent, a PR
--       and notes is titled "#90 <title>", linked to the ticket, with "Part
--       of #87 <title>" and "PR #97" under it, each linked, then the three
--       sections with their text HTML-escaped, line breaks kept. Its facts
--       line has when it was recorded, the test, the branch and commit
--       (with uncommitted changes, when so), the result, the size and the
--       file name. Across the fixtures, each of the four result texts.
--   C6  Order: cards with a notes file sort by its recorded_at, newest
--       first, mixed correctly with videos that have none and sort by their
--       modification time.
--   C7  Bad and missing notes: a notes file that can't be read gives the
--       card made from the file name, plus "notes file unreadable", and the
--       page still returns 200. A notes file whose notes are null, and a
--       video without a notes file, say "No notes for this recording." A
--       notes file without its video isn't listed.
--
-- THE PAGE FIXTURES are tiny videos of zeros, each with a notes file
-- written by the helper (so the page is checked against the format the
-- helper really writes) or by hand (the broken ones). Each video's
-- modification time is set so that sorting by it gives a different order
-- than sorting by recorded_at. The page shows times in the machine's local
-- time, so its commands run with TZ=UTC, and the times below are UTC.
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
local SERVER = project_dir .. "test/serve_recordings.py"
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
-- The page fixtures
-------------------------------------------------------------------------------

local KEPT = WORK .. "/kept"
local SCRATCH = WORK .. "/scratch"
local ISSUES = "https://github.com/MinimalEffort07/pokefirered/issues/"
local PULLS = "https://github.com/MinimalEffort07/pokefirered/pull/"

-- Recordings with notes files the helper writes, in the order the page
-- must show them, mixed with videos that have no notes file (`mtime_only`)
-- and with broken notes files (`broken`, the file's text). `at` is
-- recorded_at; `mtime` the video's modification time (UTC), picked to
-- contradict that order; `shows` the time the card must show (UTC).
local PAGE_VIDEOS = {
    { name = "issue-90_test_x_20261020_134554.mp4", at = "2026-10-20T13:45:54+11:00",
      mtime = "2026-01-01 00:00", shows = "2026-10-20 02:45", notes = COMPLETE,
      results = { 215, 0 }, result_text = "215 of 215 checks passed" },
    { name = "test_plain_20261019_120000.mp4", mtime_only = true,
      mtime = "2026-10-19 12:00", shows = "2026-10-19 12:00" },
    { name = "issue-91_test_x_20261019_100000.mp4", at = "2026-10-19T10:00:00+11:00",
      mtime = "2026-12-01 00:00", shows = "2026-10-18 23:00",
      notes = COMPLETE:gsub("^ticket: [^\n]*", "ticket: 91 Fails <b>loudly</b> & badly")
                      :gsub("parent: [^\n]*\n", ""):gsub("pr: [^\n]*\n", ""),
      results = { 212, 3 }, result_text = "3 of 215 checks failed", dirty = true },
    { name = "test_plain_20261018_180000.mp4", mtime_only = true,
      mtime = "2026-10-18 18:00", shows = "2026-10-18 18:00" },
    { name = "demo_y_20261018_120000.mp4", at = "2026-10-18T12:00:00+00:00",
      mtime = "2026-11-01 00:00", shows = "2026-10-18 12:00", test = "demo_y",
      results = { 0, 0 }, result_text = "no checks: a demo or probe" },
    { name = "issue-55_test_broken_20261017_080000.mp4", broken = "{not json",
      mtime = "2026-10-17 08:00", shows = "2026-10-17 08:00" },
    { name = "issue-92_test_x_20261017_090000.mp4", at = "2026-10-17T09:00:00+11:00",
      mtime = "2026-10-30 00:00", shows = "2026-10-16 22:00", notes = "ticket: 92 Unfinished\n" .. SECTIONS_ONLY,
      result_text = "no result: the test didn't finish" },
    { name = "test_array_20261016_100000.mp4", broken = "[1, 2]",
      mtime = "2026-10-16 10:00", shows = "2026-10-16 10:00" },
    { name = "test_bad_time_20261015_100000.mp4",
      broken = '{"video": "test_bad_time_20261015_100000.mp4", "recorded_at": "yesterday"}',
      mtime = "2026-10-15 10:00", shows = "2026-10-15 10:00" },
    { name = "test_year_one_20261014_100000.mp4",
      broken = '{"recorded_at": "0001-01-01T00:00:00+14:00", "notes": null}',
      mtime = "2026-10-14 10:00", shows = "2026-10-14 10:00" },
    { name = "test_wrong_types_20261013_100000.mp4",
      broken = '{"recorded_at": "2026-10-13T10:00:00+00:00", "ticket": "90", "result": {"pass": "x"}}',
      mtime = "2026-10-13 10:00", shows = "2026-10-13 10:00" },
}

-- Make every page fixture. The fixture repo's README is put back first, so
-- only the recording marked `dirty` is made with uncommitted changes.
local function make_page_fixtures()
    run("mkdir -p " .. sh_quote(KEPT) .. " " .. sh_quote(SCRATCH)
        .. " && git -C " .. sh_quote(REPO) .. " checkout -q -- README")
    local tests = {}
    for i, v in ipairs(PAGE_VIDEOS) do
        local video = KEPT .. "/" .. v.name
        write_file(video, "\0\0\0\0")
        if v.broken then
            write_file(video:gsub("%.mp4$", ".json"), v.broken)
        elseif not v.mtime_only then
            if v.dirty then write_file(REPO .. "/README", "changed\n") end
            local results = v.results and results_file("page_results_" .. i .. ".json", v.results[1], v.results[2])
                or (WORK .. "/no_such_results.json")
            local test_file = REPO_TEST
            if v.test then
                test_file = REPO .. "/test/tests/" .. v.test .. ".lua"
                write_file(test_file, "fw = nil\n")
            end
            local notes = v.notes and ("--notes " .. sh_quote(notes_file("page_notes_" .. i .. ".txt", v.notes))) or ""
            local code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " write " .. sh_quote(video)
                .. " --test " .. sh_quote(test_file) .. " --recorded-at " .. v.at
                .. " --results " .. sh_quote(results) .. " --repo " .. sh_quote(REPO) .. " " .. notes)
            if code ~= 0 then fw.log_error("fixture notes failed for " .. v.name .. ": " .. output) end
            if v.dirty then run("git -C " .. sh_quote(REPO) .. " checkout -q -- README") end
        end
        run("TZ=UTC touch -d " .. sh_quote(v.mtime) .. " " .. sh_quote(video))
    end
    -- A notes file whose video doesn't exist, and one that is a folder.
    write_file(KEPT .. "/orphan_20261021_000000.json", '{"video": "orphan_20261021_000000.mp4", '
        .. '"recorded_at": "2026-10-21T00:00:00+00:00", "notes": null}')
    run("mkdir -p " .. sh_quote(SCRATCH .. "/folder_notes_20261012_100000.json")
        .. " && head -c 4 /dev/zero > " .. sh_quote(SCRATCH .. "/folder_notes_20261012_100000.mp4"))
end

-- What a GET of `path` would return, from serve_recordings.py --print, in
-- UTC. Returns a table: status (a number, or nil if the first line isn't
-- one), body (everything after the first line), code (the exit code) and
-- output (everything printed, for the log).
local function get(path)
    local code, output = run("TZ=UTC python3 " .. sh_quote(SERVER) .. " --kept " .. sh_quote(KEPT)
        .. " --scratch " .. sh_quote(SCRATCH) .. " --print " .. sh_quote(path))
    local first, rest = output:match("^([^\n]*)\n?(.*)$")
    fw.log(string.format("GET %s -> status %s, exit %s, %d bytes", path, tostring(first), tostring(code), #output))
    return { status = tonumber(first), body = rest or "", code = code, output = output }
end

-- Undo HTML escaping, for reading text back off the page.
local function unescape(s)
    return (s:gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&quot;", '"')
             :gsub("&#x27;", "'"):gsub("&#39;", "'"):gsub("&amp;", "&"))
end

-- `html` without its tags, unescaped.
local function text_of(html)
    return unescape((html:gsub("<[^>]*>", "")))
end

-- The cards in `html`, in page order, as a list of tables: `text` (the
-- card's HTML, up to the next card) and `file` (its file name, from
-- <span class="file">, unescaped).
local function cards(html)
    local starts = {}
    local from = 1
    while true do
        local s = html:find('<div class="card">', from, true)
        if not s then break end
        starts[#starts + 1] = s
        from = s + 1
    end
    local list = {}
    for i, s in ipairs(starts) do
        local text = html:sub(s, (starts[i + 1] or (#html + 1)) - 1)
        local file = text:match('<span class="file">(.-)</span>')
        list[#list + 1] = { text = text, file = file and unescape(file) }
    end
    return list
end

local function card_named(list, file)
    for _, c in ipairs(list) do
        if c.file == file then return c end
    end
    return nil
end

-- The card's title: its text, and the link it carries (nil for none).
local function title_of(card)
    local title = card.text:match('<div class="title">(.-)</div>') or ""
    return text_of(title), title:match('<a href="([^"]*)"')
end

-- The card's facts line, as plain text.
local function facts_of(card)
    return text_of(card.text:match('<div class="facts">(.-)</div>') or "")
end

local function has(text, part)
    return text:find(part, 1, true) ~= nil
end

-------------------------------------------------------------------------------
-- C5: the card
-------------------------------------------------------------------------------
local function check_card(page, kept_cards)
    local full_commit = first_line("git -C " .. sh_quote(REPO) .. " rev-parse HEAD")
    local c = card_named(kept_cards, "issue-90_test_x_20261020_134554.mp4")
    local ok = fw.assert_true(c ~= nil, "C5: the recording with notes has a card")
    if not c then return end
    local text = c.text

    local title, link = title_of(c)
    ok = fw.assert_eq(title, "#90 Shaking patches survive a field reload", "C5: its title is the ticket's") and ok
    ok = fw.assert_eq(link, ISSUES .. "90", "C5: its title links to the ticket") and ok

    local parent = text:match('<a href="' .. ISSUES:gsub("%p", "%%%0") .. '87">(.-)</a>')
    ok = fw.assert_true(parent ~= nil and has(text_of(text), "Part of #87 The Poke Radar"),
        "C5: \"Part of #87 The Poke Radar\" is under it, linked to issue 87") and ok
    ok = fw.assert_eq(parent and text_of(parent), "#87 The Poke Radar", "C5: the parent's link reads #87 The Poke Radar") and ok
    local pr = text:match('<a href="' .. PULLS:gsub("%p", "%%%0") .. '97">(.-)</a>')
    ok = fw.assert_eq(pr and text_of(pr), "PR #97", "C5: \"PR #97\" is under it, linked to pull 97") and ok

    -- The three sections, in order, each with its text. The text comes back
    -- exactly as written once unescaped, line breaks included.
    local sections = {}
    for heading, body in text:gmatch("<h3>(.-)</h3>%s*<p[^>]*>(.-)</p>") do
        sections[#sections + 1] = { unescape(heading), unescape(body) }
    end
    ok = fw.assert_eq(#sections, 3, "C5: the card has three sections") and ok
    for i, want in ipairs({ { "What's tested", TESTED }, { "The problem", PROBLEM },
                            { "What you should see", EXPECT } }) do
        local got = sections[i] or {}
        ok = fw.assert_eq(got[1], want[1], "C5: section " .. i .. "'s heading") and ok
        ok = fw.assert_eq(got[2], want[2], "C5: \"" .. want[1] .. "\" has its text, line breaks kept") and ok
    end
    ok = fw.assert_true(has(text, "4 &lt; 5 &amp; so on") and not has(text, "4 < 5"),
        "C5: the notes' < and & are HTML-escaped") and ok
    ok = fw.assert_true(has(page.body, "pre-wrap"), "C5: the page keeps the notes' line breaks (white-space: pre-wrap)") and ok
    -- On a phone the sections sit above the video (they come first); on a
    -- wide screen a media query puts them beside it.
    local notes_at, video_at = text:find("<h3>", 1, true), text:find("<video", 1, true)
    ok = fw.assert_true(notes_at ~= nil and video_at ~= nil and notes_at < video_at,
        "C5: the sections come before the video") and ok
    ok = fw.assert_true(has(page.body, "@media (min-width"), "C5: a wide screen lays them out differently") and ok

    -- The facts line.
    local facts = facts_of(c)
    for _, part in ipairs({ "Recorded 2026-10-20 02:45", "test/tests/test_x.lua", BRANCH,
                            full_commit:sub(1, 7), "215 of 215 checks passed", "4 bytes",
                            "issue-90_test_x_20261020_134554.mp4" }) do
        ok = fw.assert_true(has(facts, part), "C5: the facts line has " .. part) and ok
    end
    ok = fw.assert_true(not has(facts, "uncommitted"), "C5: a clean tree isn't said to have uncommitted changes") and ok
    if not ok then dump("card #90", text) end

    -- Each result text, the dirty tree, and escaping in a ticket's title.
    for _, v in ipairs(PAGE_VIDEOS) do
        if v.result_text then
            local vc = card_named(kept_cards, v.name)
            local vf = vc and facts_of(vc) or ""
            local good = fw.assert_true(has(vf, v.result_text), "C5: " .. v.name .. "'s facts say " .. v.result_text)
            if v.dirty then
                good = fw.assert_true(has(vf, "with uncommitted changes"),
                    "C5: " .. v.name .. "'s facts say \"with uncommitted changes\"") and good
                local t = vc and title_of(vc)
                good = fw.assert_eq(t, "#91 Fails <b>loudly</b> & badly", "C5: a ticket's title is shown as written") and good
                good = fw.assert_true(vc ~= nil and has(vc.text, "#91 Fails &lt;b&gt;loudly&lt;/b&gt; &amp; badly"),
                    "C5: a ticket's title is HTML-escaped") and good
                good = fw.assert_true(vc ~= nil and not has(vc.text, "Part of") and not has(vc.text, "PR #"),
                    "C5: no parent or PR line when the notes give neither") and good
            end
            if not good then dump(v.name, vc and vc.text or "(no card)") end
        end
    end
    -- With no ticket in the notes, the title is the test's name.
    local demo = card_named(kept_cards, "demo_y_20261018_120000.mp4")
    local t, l
    if demo then t, l = title_of(demo) end
    fw.assert_eq(t, "demo_y", "C5: without a ticket, the title is the test's name")
    fw.assert_eq(l, nil, "C5: without a ticket, the title links nowhere")

    -- The recording's own page shows the same card, titled the same.
    local r = get("/r/issue-90_test_x_20261020_134554")
    fw.assert_eq(r.status, 200, "C5: /r/issue-90_test_x_20261020_134554 returns 200")
    fw.assert_eq(r.body:match("<title>(.-)</title>"), "#90 Shaking patches survive a field reload",
        "C5: its own page is titled with the ticket")
    -- (Each card is compared up to its video: what follows differs.)
    local function up_to_video(html)
        return ((html:match("^(.-</video>)") or ""):gsub(' · <a href="/r/[^"]*">own page</a>', ""))
    end
    local own = cards(r.body)[1]
    local same = fw.assert_true(own ~= nil and up_to_video(text) ~= "" and up_to_video(own.text) == up_to_video(text),
        "C5: its own page shows the same card, without the link to itself")
    if not same then dump("/r/issue-90_test_x_20261020_134554", r.output) end
end

-------------------------------------------------------------------------------
-- C6: order
-------------------------------------------------------------------------------
local function check_order(page, kept_cards)
    local want, by_mtime = {}, {}
    for i, v in ipairs(PAGE_VIDEOS) do
        want[i] = v.name
        by_mtime[i] = v
    end
    table.sort(by_mtime, function(a, b) return a.mtime > b.mtime end)
    local mtime_names = {}
    for i, v in ipairs(by_mtime) do mtime_names[i] = v.name end
    fw.assert_true(table.concat(mtime_names, ", ") ~= table.concat(want, ", "),
        "C6: the fixture's order by modification time differs from the order the page must show")

    local got = {}
    for i, c in ipairs(kept_cards) do got[i] = tostring(c.file) end
    if not fw.assert_eq(table.concat(got, ", "), table.concat(want, ", "),
        "C6: cards sort by recorded_at, or by modification time without a notes file, newest first") then
        dump("/", page.output)
    end
    for _, v in ipairs(PAGE_VIDEOS) do
        local c = card_named(kept_cards, v.name)
        fw.assert_true(c ~= nil and has(facts_of(c), "Recorded " .. v.shows),
            "C6: " .. v.name .. " shows when it was recorded: " .. v.shows)
    end
end

-------------------------------------------------------------------------------
-- C7: bad and missing notes
-------------------------------------------------------------------------------
local function check_bad_notes(page, kept_cards, scratch_cards)
    fw.assert_eq(page.status, 200, "C7: the page returns 200 despite the broken notes files")
    local broken = {}
    for _, v in ipairs(PAGE_VIDEOS) do
        if v.broken then broken[#broken + 1] = { v.name, KEPT, kept_cards } end
    end
    broken[#broken + 1] = { "folder_notes_20261012_100000.mp4", SCRATCH, scratch_cards }
    for _, b in ipairs(broken) do
        local c = card_named(b[3], b[1])
        local ok = fw.assert_true(c ~= nil and has(c.text, "notes file unreadable"),
            "C7: " .. b[1] .. "'s card says \"notes file unreadable\"")
        if not ok then dump(b[1], c and c.text or "(no card)") end
    end
    -- The card made from the file name, as for a video without notes.
    local c = card_named(kept_cards, "issue-55_test_broken_20261017_080000.mp4")
    local title, link
    if c then title, link = title_of(c) end
    fw.assert_eq(title, "#55 test_broken", "C7: an unreadable notes file's card is titled from the file name")
    fw.assert_eq(link, ISSUES .. "55", "C7: ... and linked to its issue")
    local r = get("/r/issue-55_test_broken_20261017_080000")
    fw.assert_eq(r.status, 200, "C7: its own page returns 200")
    fw.assert_true(has(r.body, "notes file unreadable"), "C7: its own page says \"notes file unreadable\"")

    -- No notes, and no notes file.
    for _, name in ipairs({ "demo_y_20261018_120000.mp4", "test_plain_20261019_120000.mp4" }) do
        local nc = card_named(kept_cards, name)
        fw.assert_true(nc ~= nil and has(nc.text, "No notes for this recording."),
            "C7: " .. name .. " says \"No notes for this recording.\"")
    end
    local with_notes = card_named(kept_cards, "issue-90_test_x_20261020_134554.mp4")
    fw.assert_true(with_notes ~= nil and not has(with_notes.text, "No notes"),
        "C7: a recording with notes doesn't say it has none")

    -- A notes file without its video isn't listed, and has no page.
    fw.assert_true(not has(page.body, "orphan"), "C7: a notes file without its video isn't listed")
    fw.assert_eq(get("/r/orphan_20261021_000000").status, 404, "C7: ... and has no page")
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

    fw.log("=== The recordings page with notes (serve_recordings.py --print) ===")
    make_page_fixtures()
    local page = get("/")
    fw.assert_eq(page.code, 0, "--print / exits 0")
    local scratch_at = page.body:find('id="scratch"', 1, true) or (#page.body + 1)
    local kept_cards = cards(page.body:sub(1, scratch_at - 1))
    local scratch_cards = cards(page.body:sub(scratch_at))
    check_card(page, kept_cards)
    check_order(page, kept_cards)
    check_bad_notes(page, kept_cards, scratch_cards)
    fw.finish()
end)
