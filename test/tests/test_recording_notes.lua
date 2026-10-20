-------------------------------------------------------------------------------
-- test_recording_notes.lua: recordings carry notes, the recordings page
-- shows them, autodev's sessions must write them (issues #110 and #111,
-- part of #103), and recordings kept before notes existed can get notes
-- without moving on the page (issue #122, part of #120)
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
-- D1 to D7 are issue #111's: in an autodev session, record_test.sh and the
-- helper hold the notes to more rules (see THE SESSION VARIABLES).
--
--   D1  A ticket session for #90, with no notes: --check exits 1, saying
--       autodev sessions must pass --notes.
--   D2  The same session, with notes saying ticket: 91, or with no ticket:,
--       is refused, naming #90.
--   D3  The same session, with no parent:, is refused.
--   D4  The same session, with complete notes for #90: accepted.
--   D5  A fix-up session for #90 with PR 97, notes without pr:: --check says
--       PR #97, and the notes file the helper writes has "pr": 97. With
--       pr: 97 in the notes, it's kept, and so is a pr: 98.
--   D4 and D5 run --check on a copy of record_test.sh in a fixture repo
--   whose branch changes src/ (test/lib/record_fixture.lua): in a session,
--   record_test.sh refuses a branch that changes nothing in the game (#121,
--   test_record_game_changes.lua), and these checks must pass whichever
--   branch the suite runs on.
--   D6  No session variables, no notes: accepted, as in C1.
--   D7  set-pr sets "pr" in an existing notes file in either folder (kept
--       first, as on the page), leaving the rest as it was. It refuses a
--       name with no notes file, a PR that isn't a number, and a path.
--
-- N1 to N7 are issue #122's: `recording_notes.py add-notes NAME NOTES
-- [--test TEST] [--kept DIR]` gives notes to a recording kept before notes
-- existed. Its commands run in Sydney's time (SYDNEY below), so a UTC
-- offset shows, and changes with summer time.
--
--   N1  On test_x_20261017_120000.mp4, recorded_at is 2026-10-17T12:00:00
--       with that date's offset (+11:00), whatever the video's modification
--       time; a July stamp gets +10:00. The test is --test's path from the
--       repo root (null without --test); the ticket, parent, PR and notes
--       are the notes'; branch, commit, uncommitted_changes and result are
--       null; notes_added_at, last, is now with this machine's offset.
--   N2  On issue-24-safety-net.mp4, which has no stamp, recorded_at is the
--       video's modification time, and so it is for an impossible stamp
--       (month 13). A test outside the repo is its full path.
--   N3  A folder modelled on ~/recordings: stamped and unstamped names, a
--       stamped video modified hours after its stamp, and a newer video
--       whose notes file the helper's write made. Every video without a
--       notes file gets notes, the page's cards keep their order, and the
--       written notes file stays as it was.
--   N4  Such a card has its ticket as title, "Part of #22 ..." and "PR
--       #33", the three notes, its test and "notes added <date>" (to the
--       minute, as "Recorded"), and no result, branch or commit, and no
--       "No notes for this recording.". A notes_added_at the page can't
--       read makes the notes file unreadable (a C7 fixture).
--   N5  Refused, with a message, every file in the folder staying byte for
--       byte as it was: a name with no video, a video whose notes file
--       record_test.sh wrote, a video with an unreadable notes file (also
--       one add-notes wrote whose notes_added_at was broken by hand), notes
--       missing a section (every problem listed), and a path.
--   N6  add-notes again on a video it gave notes rewrites them with the new
--       text, keeping recorded_at though the video was touched since.
--   N7  In a ticket session for #90, add-notes accepts notes naming
--       ticket: 24 with no parent:; in a fix-up session they don't get its
--       PR. record_test.sh --check (and the helper's check) still refuses
--       the same notes for a recording in that session, naming #90 and
--       parent:.
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
-- With AUTODEV_ROLE set, record_test.sh requires notes and the helper
-- checks them against the session (D1 to D5). So every command here first
-- clears all four (env -u), then sets the ones its case needs (run_as), and
-- this test checks the same whoever runs it, an autodev session included.
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
local record_fixture = dofile(project_dir .. "test/lib/record_fixture.lua")

local RECORD = project_dir .. "test/record_test.sh"
local NOTES_TOOL = project_dir .. "test/tools/recording_notes.py"
local SERVER = project_dir .. "test/serve_recordings.py"
-- A real test for --check to accept. It is never run.
local SOME_TEST = project_dir .. "test/tests/test_player_sprite.lua"

-- Every command runs without the session variables, unless its case sets
-- some (see THE SESSION VARIABLES above).
local CLEAN_ENV = "env -u AUTODEV_ISSUE -u AUTODEV_ROLE -u AUTODEV_PR -u AUTODEV_HANDLED_THROUGH "

-- `s` in single quotes, safe to paste into a shell command.
local function sh_quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- Run shell command `cmd` as an autodev session would: without the session
-- variables, then with those in `session` set ({ ROLE = "ticket", ISSUE =
-- 90 } sets AUTODEV_ROLE and AUTODEV_ISSUE). Returns its exit code and
-- everything it printed (stdout and stderr together).
local function run_as(session, cmd)
    local set = ""
    for _, key in ipairs({ "ROLE", "ISSUE", "PR", "HANDLED_THROUGH" }) do
        if session[key] ~= nil then
            set = set .. "AUTODEV_" .. key .. "=" .. tostring(session[key]) .. " "
        end
    end
    local pipe = io.popen(CLEAN_ENV .. set .. cmd .. " 2>&1", "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return nil, ""
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    return code, output
end

-- Run shell command `cmd` without the session variables, as a person does.
local function run(cmd)
    return run_as({}, cmd)
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

-- Whether `text` holds `part`, as plain text.
local function has(text, part)
    return text:find(part, 1, true) ~= nil
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

    -- --check checks only the arguments and the notes, so it works in a
    -- checkout that isn't built yet: here a copy of the two scripts in a
    -- folder with no ROM.
    local bare = WORK .. "/unbuilt"
    run("mkdir -p " .. sh_quote(bare .. "/test/tools")
        .. " && cp " .. sh_quote(RECORD) .. " " .. sh_quote(bare .. "/test/record_test.sh")
        .. " && cp " .. sh_quote(NOTES_TOOL) .. " " .. sh_quote(bare .. "/test/tools/recording_notes.py"))
    code, output = run("bash " .. sh_quote(bare .. "/test/record_test.sh") .. " --check --notes "
        .. sh_quote(path) .. " " .. sh_quote(SOME_TEST))
    if not fw.assert_eq(code, 0, "C1: --check works in a checkout with no ROM built") then
        dump("no ROM", output)
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

    -- --notes followed by another option, rather than a file.
    code, output = run("bash " .. sh_quote(RECORD) .. " --check --notes --check " .. sh_quote(SOME_TEST))
    ok = fw.assert_eq(code, 1, "C2: --check refuses --notes followed by an option (exit 1)")
    ok = fw.assert_true(output:find("--notes needs a notes file", 1, true) ~= nil,
        "C2: the message says --notes needs a notes file") and ok
    if not ok then dump("--notes --check", output) end

    -- The helper's own failures are messages, not Python tracebacks.
    code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " check --save "
        .. sh_quote(WORK .. "/no_such_folder/copy.txt") .. " " .. sh_quote(notes_file("save.txt", COMPLETE)))
    ok = fw.assert_eq(code, 1, "C2: check --save into a folder that doesn't exist exits 1")
    ok = fw.assert_true(output:find("Traceback", 1, true) == nil and output:find("can't save", 1, true) ~= nil,
        "C2: ... saying it can't save the copy, without a traceback") and ok
    if not ok then dump("check --save", output) end
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

    -- recorded_at must be a time the page can read.
    write_file(WORK .. "/bad_time.mp4", "")
    code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " write " .. sh_quote(WORK .. "/bad_time.mp4")
        .. " --test " .. sh_quote(REPO_TEST) .. " --recorded-at now --repo " .. sh_quote(REPO))
    ok = fw.assert_eq(code, 1, "C3: write refuses --recorded-at now (exit 1)")
    ok = fw.assert_true(output:find("recorded-at", 1, true) ~= nil, "C3: ... naming --recorded-at") and ok
    ok = fw.assert_eq(read_json(WORK .. "/bad_time.json").keys, nil, "C3: ... and writes no notes file") and ok
    if not ok then dump("write, bad time", output) end

    -- record_test.sh writes the notes file when the recording starts (the
    -- branch and commit are the ones recorded, whatever happens to the
    -- checkout meanwhile) and adds the result once the test has finished,
    -- with set-result. The rest of the file stays as it was.
    local before = read_json(video:gsub("%.mp4$", ".json"))
    code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " set-result "
        .. sh_quote(video:gsub("%.mp4$", ".json")) .. " --results " .. sh_quote(passed))
    f = read_json(video:gsub("%.mp4$", ".json"))
    ok = fw.assert_eq(code, 0, "C3: set-result exits 0")
    ok = fw.assert_eq(f.result, '{"fail": 0, "pass": 215}', "C3: set-result adds the result: 215 and 0") and ok
    for _, key in ipairs({ "video", "recorded_at", "test", "branch", "commit", "uncommitted_changes",
                           "ticket", "parent", "pr", "notes", "keys" }) do
        ok = fw.assert_eq(f[key], before[key], "C3: set-result leaves " .. key .. " as it was") and ok
    end
    code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " set-result "
        .. sh_quote(video:gsub("%.mp4$", ".json")) .. " --results " .. sh_quote(WORK .. "/no_such_results.json"))
    f = read_json(video:gsub("%.mp4$", ".json"))
    ok = fw.assert_eq(f.result, "null", "C3: set-result with no results file: result is null") and ok
    if not ok then dump("set-result", output) end

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
-- D1 to D6: autodev sessions must write notes
-------------------------------------------------------------------------------

-- A ticket session for #90, and a fix-up session for #90's PR 97.
local TICKET_SESSION = { ROLE = "ticket", ISSUE = 90 }
local FIXUP_SESSION = { ROLE = "fixup", ISSUE = 90, PR = 97, HANDLED_THROUGH = 1234 }

-- `record_test.sh --check` with `args` (already shell-quoted), on SOME_TEST,
-- in `session`. `record` is the record_test.sh to run, RECORD if not given.
local function check_as(session, args, record)
    return run_as(session, "bash " .. sh_quote(record or RECORD) .. " --check " .. args .. " " .. sh_quote(SOME_TEST))
end

-- A copy of record_test.sh on a fixture branch that changes the game, for
-- D4 and D5 (see the header). When the fixture can't be made, that fails
-- the test, and the path returned doesn't exist, so D4 and D5 fail too.
local function game_change_record()
    local repo, err = record_fixture.make(WORK .. "/game-change", project_dir)
    if not fw.assert_true(repo ~= nil, "D4, D5: the fixture repo is made") then
        fw.log_error(err)
        return WORK .. "/no_fixture/record_test.sh"
    end
    local code, output = run("echo 'int y;' >> " .. sh_quote(repo.dir .. "/src/x.c")
        .. " && " .. repo.git .. "commit -qam src")
    if not fw.assert_eq(code, 0, "D4, D5: the fixture branch commits a src/ change") then
        dump("fixture", output)
    end
    return repo.record
end

local function check_sessions()
    -- D1: no notes.
    local code, output = check_as(TICKET_SESSION, "")
    local ok = fw.assert_eq(code, 1, "D1: a ticket session's --check with no notes exits 1")
    ok = fw.assert_true(has(output, "autodev sessions must pass --notes"),
        "D1: ... saying autodev sessions must pass --notes") and ok
    if not ok then dump("D1", output) end

    -- D2: notes for another ticket, or for none.
    for _, case in ipairs({
        { "notes saying ticket: 91", COMPLETE:gsub("^ticket: 90", "ticket: 91") },
        { "notes with no ticket:", without(COMPLETE, "ticket: 90 Shaking patches survive a field reload") },
    }) do
        code, output = check_as(TICKET_SESSION, "--notes " .. sh_quote(notes_file("d2.txt", case[2])))
        ok = fw.assert_eq(code, 1, "D2: a ticket session for #90 refuses " .. case[1] .. " (exit 1)")
        ok = fw.assert_true(has(output, "#90") and has(output, "ticket:"),
            "D2: ... naming #90 and ticket:") and ok
        if not ok then dump("D2: " .. case[1], output) end
    end

    -- D3: notes with no parent:.
    code, output = check_as(TICKET_SESSION, "--notes "
        .. sh_quote(notes_file("d3.txt", without(COMPLETE, "parent: #87 The Poke Radar"))))
    ok = fw.assert_eq(code, 1, "D3: a ticket session refuses notes with no parent: (exit 1)")
    ok = fw.assert_true(has(output, "parent:") and has(output, "missing"),
        "D3: ... saying parent: is missing") and ok
    if not ok then dump("D3", output) end

    -- D4: complete notes for #90, as a ticket session writes them before
    -- its PR exists, on a branch that changes the game.
    local game_record = game_change_record()
    local no_pr = without(COMPLETE, "pr: 97")
    code, output = check_as(TICKET_SESSION, "--notes " .. sh_quote(notes_file("d4.txt", no_pr)), game_record)
    ok = fw.assert_eq(code, 0, "D4: a ticket session for #90 accepts complete notes for #90 (exit 0)")
    ok = fw.assert_true(has(output, "issue-90_test_player_sprite_"), "D4: ... named for ticket 90") and ok
    if not ok then dump("D4", output) end

    -- D5: a fix-up's notes get its PR when they name none, in --check's
    -- verdict and in the notes file record_test.sh writes with the helper.
    code, output = check_as(FIXUP_SESSION, "--notes " .. sh_quote(notes_file("d5.txt", no_pr)), game_record)
    ok = fw.assert_eq(code, 0, "D5: a fix-up session for #90 accepts notes without pr: (exit 0)")
    ok = fw.assert_true(has(output, "PR #97"), "D5: ... and --check says PR #97") and ok
    if not ok then dump("D5 --check", output) end
    -- (pr: 98 shows the notes' own PR is kept, not overwritten by 97.)
    for _, case in ipairs({ { "without pr:", no_pr, "97" }, { "with pr: 97", COMPLETE, "97" },
                            { "with pr: 98", (COMPLETE:gsub("pr: 97", "pr: 98")), "98" } }) do
        local video = WORK .. "/d5_" .. case[1]:gsub("%W", "_") .. ".mp4"
        write_file(video, "\0\0\0\0")
        code, output = run_as(FIXUP_SESSION, "python3 " .. sh_quote(NOTES_TOOL) .. " write " .. sh_quote(video)
            .. " --test " .. sh_quote(REPO_TEST) .. " --recorded-at " .. RECORDED_AT
            .. " --repo " .. sh_quote(REPO) .. " --notes " .. sh_quote(notes_file("d5.txt", case[2])))
        local f = read_json(video:gsub("%.mp4$", ".json"))
        ok = fw.assert_eq(code, 0, "D5: a fix-up's notes " .. case[1] .. ": the helper writes the notes file")
        ok = fw.assert_eq(f.pr, case[3], "D5: a fix-up's notes " .. case[1] .. ": the notes file has \"pr\": "
            .. case[3]) and ok
        if not ok then dump("D5 write " .. case[1], output) end
    end

    -- D6: no session, no notes: accepted, as for a person.
    code, output = check_as({}, "")
    ok = fw.assert_eq(code, 0, "D6: with no session variables, --check with no notes exits 0")
    if not ok then dump("D6", output) end
end

-------------------------------------------------------------------------------
-- D7: set-pr
-------------------------------------------------------------------------------

-- set-pr's folders here, in place of ~/recordings and /tmp/mgba-recordings.
local PR_KEPT = WORK .. "/set-pr-kept"
local PR_SCRATCH = WORK .. "/set-pr-scratch"

-- `recording_notes.py set-pr NAME PR` on PR_KEPT and PR_SCRATCH.
local function set_pr(name, pr)
    return run("python3 " .. sh_quote(NOTES_TOOL) .. " set-pr --kept " .. sh_quote(PR_KEPT)
        .. " --scratch " .. sh_quote(PR_SCRATCH) .. " " .. sh_quote(name) .. " " .. sh_quote(pr))
end

local function check_set_pr()
    run("mkdir -p " .. sh_quote(PR_KEPT) .. " " .. sh_quote(PR_SCRATCH))
    local notes = notes_file("set_pr.txt", without(COMPLETE, "pr: 97"))
    -- Recordings without a PR yet: one kept, one in scratch, and one in both.
    local made = {}
    for _, where in ipairs({ { PR_KEPT, "kept_only" }, { PR_SCRATCH, "scratch_only" },
                             { PR_KEPT, "both" }, { PR_SCRATCH, "both" } }) do
        local video = where[1] .. "/issue-90_test_" .. where[2] .. "_20261020_134554.mp4"
        local code, output, f = write_notes(video, REPO_TEST, WORK .. "/no_such_results.json",
            "--notes " .. sh_quote(notes))
        if code ~= 0 or f.pr ~= "null" then fw.log_error("set-pr fixture failed: " .. output) end
        made[#made + 1] = video:gsub("%.mp4$", ".json")
    end
    local kept_only, scratch_only, both_kept, both_scratch = made[1], made[2], made[3], made[4]
    local before = read_json(kept_only)

    -- In either folder; the name may end in .mp4.
    local code, output = set_pr("issue-90_test_kept_only_20261020_134554", "120")
    local f = read_json(kept_only)
    local ok = fw.assert_eq(code, 0, "D7: set-pr on a kept recording exits 0")
    ok = fw.assert_eq(f.pr, "120", "D7: ... and its notes file has \"pr\": 120") and ok
    for _, key in ipairs({ "keys", "video", "recorded_at", "test", "branch", "commit", "result",
                           "ticket", "parent", "notes" }) do
        ok = fw.assert_eq(f[key], before[key], "D7: ... leaving " .. key .. " as it was") and ok
    end
    if not ok then dump("set-pr kept", output) end
    code, output = set_pr("issue-90_test_scratch_only_20261020_134554.mp4", "#121")
    ok = fw.assert_eq(code, 0, "D7: set-pr on a scratch recording, named with .mp4 and the PR with #, exits 0")
    ok = fw.assert_eq(read_json(scratch_only).pr, "121", "D7: ... and its notes file has \"pr\": 121") and ok
    if not ok then dump("set-pr scratch", output) end

    -- Kept comes first, as on the recordings page.
    code, output = set_pr("issue-90_test_both_20261020_134554", "122")
    ok = fw.assert_eq(read_json(both_kept).pr, "122", "D7: a name in both folders: the kept notes file gets the PR")
    ok = fw.assert_eq(read_json(both_scratch).pr, "null", "D7: ... and the scratch one doesn't") and ok
    if not ok then dump("set-pr both", output) end

    -- Refused: a name with no notes file, a PR that isn't a number, a path.
    for _, case in ipairs({
        { "a name with no notes file", "issue-90_test_nothing_20261020_134554", "123",
          { "issue-90_test_nothing_20261020_134554", "no notes file" } },
        { "a PR that isn't a number", "issue-90_test_kept_only_20261020_134554", "abc",
          { "abc", "number" } },
        { "a PR of 0", "issue-90_test_kept_only_20261020_134554", "0", { "number" } },
        { "a path instead of a name", "../set-pr-kept/issue-90_test_kept_only_20261020_134554", "123",
          { "name" } },
    }) do
        code, output = set_pr(case[2], case[3])
        ok = fw.assert_eq(code, 1, "D7: set-pr refuses " .. case[1] .. " (exit 1)")
        for _, word in ipairs(case[4]) do
            ok = fw.assert_true(has(output, word), "D7: ... saying \"" .. word .. "\"") and ok
        end
        ok = fw.assert_true(not has(output, "Traceback"), "D7: ... without a traceback") and ok
        if not ok then dump("set-pr " .. case[1], output) end
    end
    fw.assert_eq(read_json(kept_only).pr, "120", "D7: a refused set-pr leaves the notes file as it was")
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
    { name = "test_bad_added_20261012_100000.mp4",
      broken = '{"recorded_at": "2026-10-12T10:00:00+00:00", "notes_added_at": "last week", '
          .. '"notes": {"tested": "t", "problem": "p", "expect": "e"}}',
      mtime = "2026-10-12 10:00", shows = "2026-10-12 10:00" },
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

-- What a GET of `path` would return, from serve_recordings.py --print: on
-- the page fixtures' folders in UTC, or on `where` ({ kept = K, scratch =
-- S, tz = TZ }) when given. Returns a table: status (a number, or nil if
-- the first line isn't one), body (everything after the first line), code
-- (the exit code) and output (everything printed, for the log).
local function get(path, where)
    where = where or { kept = KEPT, scratch = SCRATCH, tz = "UTC" }
    local code, output = run("TZ=" .. sh_quote(where.tz) .. " python3 " .. sh_quote(SERVER)
        .. " --kept " .. sh_quote(where.kept) .. " --scratch " .. sh_quote(where.scratch)
        .. " --print " .. sh_quote(path))
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

-- The cards of the page's kept section, as cards() gives them.
local function kept_cards_of(page)
    local scratch_at = page.body:find('id="scratch"', 1, true) or (#page.body + 1)
    return cards(page.body:sub(1, scratch_at - 1))
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
-- N1 to N7: notes for recordings kept before notes existed
-------------------------------------------------------------------------------

-- The machine's time zone for these checks: Sydney's, with its rules
-- written out so no time zone files are needed (UTC+10, and UTC+11 from
-- the first Sunday of October to the first Sunday of April).
local SYDNEY = "AEST-10AEDT,M10.1.0,M4.1.0/3"

-- `cmd` run in SYDNEY's time.
local function in_sydney(cmd)
    return "TZ=" .. sh_quote(SYDNEY) .. " " .. cmd
end

-- Set file `path`'s modification time to `mtime`, Sydney time. `date`
-- turns it into seconds since 1970 first: uutils' touch (this machine's)
-- reads `touch -d` times as UTC whatever TZ says.
local function set_mtime(path, mtime)
    local seconds = first_line(in_sydney("date -d " .. sh_quote(mtime) .. " +%s"))
    local code, output = run("touch -d @" .. seconds .. " " .. sh_quote(path))
    if code ~= 0 then fw.log_error("can't set the time of " .. path .. ": " .. output) end
end

-- Make video `name` (a few zero bytes) in folder `dir`, last modified at
-- `mtime`, Sydney time. Returns its path.
local function make_video(dir, name, mtime)
    local video = dir .. "/" .. name
    write_file(video, "\0\0\0\0")
    set_mtime(video, mtime)
    return video
end

-- `recording_notes.py add-notes` in Sydney's time on recording `name` in
-- folder `kept`, with notes file `notes` and `extra` arguments (already
-- shell-quoted), in `session` (none when nil). Returns its exit code and
-- what it printed.
local function add_notes(kept, name, notes, extra, session)
    return run_as(session or {}, in_sydney("python3 " .. sh_quote(NOTES_TOOL) .. " add-notes --kept "
        .. sh_quote(kept) .. " " .. (extra or "") .. " " .. sh_quote(name) .. " " .. sh_quote(notes)))
end

-- Seconds since 1970 of ISO time `iso`, or nil when `date` can't read it.
local function epoch_of(iso)
    return tonumber(first_line("date -d " .. sh_quote(iso) .. " +%s"))
end

-- An old recording's notes, as the owner would write them for #24's video.
local OLD_TESTED = "Save in a generated Mt. Moon floor, then Continue."
local OLD_PROBLEM = "Continue could put you inside a rock."
local OLD_EXPECT = "About 20 s in, the game loads and you stand on open floor."
local OLD_NOTES = "ticket: 24 Continue puts you back on the cave floor\n"
    .. "parent: 22 Procedurally generated Mt. Moon\n"
    .. "pr: 33\n"
    .. "\n"
    .. "## What's tested\n" .. OLD_TESTED .. "\n"
    .. "## The problem\n" .. OLD_PROBLEM .. "\n"
    .. "## What you should see\n" .. OLD_EXPECT .. "\n"
local OLD_KEYS = "video recorded_at test branch commit uncommitted_changes result "
    .. "ticket parent pr notes notes_added_at"

-- Check the facts add-notes writes that nobody knows any more, and that
-- notes_added_at is now, with this machine's UTC offset; `label` starts
-- each check's name. `before` is the time (date +%s) just before
-- add-notes ran.
local function check_added_facts(label, f, before)
    local ok = fw.assert_eq(f.keys, OLD_KEYS, label .. ": the notes file has every field, notes_added_at last")
    for _, key in ipairs({ "branch", "commit", "uncommitted_changes", "result" }) do
        ok = fw.assert_eq(f[key], "null", label .. ": " .. key .. " is null") and ok
    end
    local added = (f.notes_added_at or ""):match('^"(.*)"$') or ""
    local offset = first_line(in_sydney("date +%:z"))
    ok = fw.assert_true(added:match("^%d%d%d%d%-%d%d%-%d%dT%d%d:%d%d:%d%d[+-]%d%d:%d%d$") ~= nil
        and added:sub(-6) == offset,
        label .. ": notes_added_at is an ISO time with this machine's offset (" .. offset .. "): " .. added) and ok
    local at, now = epoch_of(added), tonumber(first_line("date +%s"))
    ok = fw.assert_true(at ~= nil and before ~= nil and now ~= nil and at >= before - 1 and at <= now,
        label .. ": notes_added_at is when the notes were added") and ok
    return ok
end

-- N1 and N2: when an old recording was recorded.
local ADD_ONE = WORK .. "/add-notes-one"

local function check_add_notes_time()
    run("mkdir -p " .. sh_quote(ADD_ONE))
    local notes = notes_file("old_notes.txt", OLD_NOTES)

    -- N1: the name's stamp, whatever the modification time says. The test
    -- is given from inside the repo, so it is the path from the repo root.
    make_video(ADD_ONE, "test_x_20261017_120000.mp4", "2025-03-01 08:00")
    local before = tonumber(first_line("date +%s"))
    local code, output = add_notes(ADD_ONE, "test_x_20261017_120000", notes, "--test " .. sh_quote(SOME_TEST))
    local f = read_json(ADD_ONE .. "/test_x_20261017_120000.json")
    local ok = fw.assert_eq(code, 0, "N1: add-notes on test_x_20261017_120000.mp4 exits 0")
    ok = fw.assert_eq(f.recorded_at, '"2026-10-17T12:00:00+11:00"',
        "N1: recorded_at is the name's stamp, with the offset of that date") and ok
    ok = fw.assert_eq(f.video, '"test_x_20261017_120000.mp4"', "N1: video is the video's file name") and ok
    ok = fw.assert_eq(f.test, '"test/tests/test_player_sprite.lua"', "N1: test is --test's path from the repo root") and ok
    ok = fw.assert_eq(f.ticket, '{"number": 24, "title": "Continue puts you back on the cave floor"}',
        "N1: ticket is the notes'") and ok
    ok = fw.assert_eq(f.parent, '{"number": 22, "title": "Procedurally generated Mt. Moon"}',
        "N1: parent is the notes'") and ok
    ok = fw.assert_eq(f.pr, "33", "N1: pr is the notes'") and ok
    ok = fw.assert_eq(f.notes, '{"expect": ' .. json_string(OLD_EXPECT) .. ', "problem": '
        .. json_string(OLD_PROBLEM) .. ', "tested": ' .. json_string(OLD_TESTED) .. '}',
        "N1: notes are the notes' three sections") and ok
    ok = check_added_facts("N1", f, before) and ok
    if not ok then dump("N1 add-notes", output) end

    -- A stamp in winter gets winter's offset, though the notes are added
    -- in summer; the name may end in .mp4, and without --test the test is
    -- null.
    make_video(ADD_ONE, "test_x_20260715_120000.mp4", "2026-10-21 09:00")
    code, output = add_notes(ADD_ONE, "test_x_20260715_120000.mp4", notes)
    f = read_json(ADD_ONE .. "/test_x_20260715_120000.json")
    ok = fw.assert_eq(code, 0, "N1: add-notes on a name ending in .mp4 exits 0")
    ok = fw.assert_eq(f.recorded_at, '"2026-07-15T12:00:00+10:00"',
        "N1: a July stamp gets July's offset, +10:00") and ok
    ok = fw.assert_eq(f.test, "null", "N1: without --test, test is null") and ok
    if not ok then dump("N1 add-notes, July", output) end

    -- N2: no stamp, so the video's modification time (when its recording
    -- ended; mv and the shrink tool keep it). A test outside the repo is
    -- its full path.
    make_video(ADD_ONE, "issue-24-safety-net.mp4", "2026-10-05 20:30:15")
    before = tonumber(first_line("date +%s"))
    code, output = add_notes(ADD_ONE, "issue-24-safety-net", notes, "--test " .. sh_quote(OUTSIDE_TEST))
    f = read_json(ADD_ONE .. "/issue-24-safety-net.json")
    ok = fw.assert_eq(code, 0, "N2: add-notes on issue-24-safety-net.mp4 exits 0")
    ok = fw.assert_eq(f.recorded_at, '"2026-10-05T20:30:15+11:00"',
        "N2: recorded_at is the video's modification time") and ok
    ok = fw.assert_eq(f.test, '"' .. first_line("realpath " .. sh_quote(OUTSIDE_TEST)) .. '"',
        "N2: a test outside the repo is its full path") and ok
    ok = check_added_facts("N2", f, before) and ok
    if not ok then dump("N2 add-notes", output) end

    -- A stamp that isn't a real date and time is no stamp.
    make_video(ADD_ONE, "test_x_20261399_250000.mp4", "2026-09-01 07:00:00")
    code, output = add_notes(ADD_ONE, "test_x_20261399_250000", notes)
    f = read_json(ADD_ONE .. "/test_x_20261399_250000.json")
    ok = fw.assert_eq(code, 0, "N2: add-notes on test_x_20261399_250000.mp4 exits 0")
    ok = fw.assert_eq(f.recorded_at, '"2026-09-01T07:00:00+10:00"',
        "N2: an impossible stamp (month 13, hour 25) gives the modification time") and ok
    if not ok then dump("N2 add-notes, bad stamp", output) end
end

-- Every file and folder in `dir`: each one's name, kind, size and
-- modification time, then each file's SHA-256. Equal snapshots mean
-- nothing in it changed, not even a hidden file left behind.
local function snapshot(dir)
    local q = sh_quote(dir)
    local _, output = run("find " .. q .. " -printf '%P %y %s %T@\\n' | LC_ALL=C sort"
        .. " && find " .. q .. " -type f -exec sha256sum {} + | LC_ALL=C sort -k2")
    return output
end

-- N5: refusals change nothing.
local REFUSE_KEPT = WORK .. "/add-notes-refused"

local function check_add_notes_refused()
    run("mkdir -p " .. sh_quote(REFUSE_KEPT))
    local notes = notes_file("old_notes.txt", OLD_NOTES)
    -- A fresh recording, whose notes file record_test.sh wrote with the
    -- helper's write; a video whose notes file isn't JSON; and a video
    -- with no notes file.
    write_notes(REFUSE_KEPT .. "/recorded_20261020_120000.mp4", REPO_TEST,
        WORK .. "/no_such_results.json", "--notes " .. sh_quote(notes_file("refused_fresh.txt", COMPLETE)))
    make_video(REFUSE_KEPT, "unreadable_20261020_110000.mp4", "2026-10-20 11:05")
    write_file(REFUSE_KEPT .. "/unreadable_20261020_110000.json", "{not json")
    -- One add-notes wrote, then broken by hand: the page can't read it.
    make_video(REFUSE_KEPT, "bad_added_20261020_105000.mp4", "2026-10-20 10:55")
    write_file(REFUSE_KEPT .. "/bad_added_20261020_105000.json",
        '{"recorded_at": "2026-10-20T10:50:00+11:00", "notes_added_at": "last week"}')
    make_video(REFUSE_KEPT, "plain_20261020_100000.mp4", "2026-10-20 10:05")
    -- Notes the parser refuses twice over: a missing section, and an
    -- unknown header. Both problems are listed.
    local bad_notes = notes_file("old_bad.txt",
        without(without(OLD_NOTES, "## The problem"), OLD_PROBLEM):gsub("^ticket:", "tickt:"))

    for _, case in ipairs({
        { "a name with no such video", "nothing_20261020_090000", notes,
          { "nothing_20261020_090000.mp4", "no video" } },
        { "a video whose notes file record_test.sh wrote", "recorded_20261020_120000", notes,
          { "record_test.sh" } },
        { "a video whose notes file can't be read", "unreadable_20261020_110000", notes,
          { "unreadable_20261020_110000.json", "can't read" } },
        { "a notes file add-notes wrote whose notes_added_at isn't a time", "bad_added_20261020_105000", notes,
          { "bad_added_20261020_105000.json", "can't read", "notes_added_at" } },
        { "notes missing a section", "plain_20261020_100000", bad_notes,
          { "The problem", "missing", "tickt" } },
        { "a path instead of a name", "../add-notes-one/issue-24-safety-net", notes, { "name" } },
    }) do
        local before = snapshot(REFUSE_KEPT)
        local code, output = add_notes(REFUSE_KEPT, case[2], case[3])
        local ok = fw.assert_eq(code, 1, "N5: add-notes refuses " .. case[1] .. " (exit 1)")
        for _, word in ipairs(case[4]) do
            ok = fw.assert_true(has(output, word), "N5: ... saying \"" .. word .. "\"") and ok
        end
        ok = fw.assert_true(not has(output, "Traceback"), "N5: ... without a traceback") and ok
        local after = snapshot(REFUSE_KEPT)
        ok = fw.assert_true(before ~= "" and after == before, "N5: ... and every file stays as it was") and ok
        if not ok then
            dump("N5 " .. case[1], output)
            dump("before", before)
            dump("after", after)
        end
    end
end

-- N3 and N4: a folder modelled on ~/recordings, newest first as the page
-- must list it. `mtime` is each video's modification time (Sydney time);
-- `fresh` marks the recording whose notes file record_test.sh wrote, with
-- recorded_at `at`. The others have no notes file yet. A stamped video's
-- time is when its recording ended, a few minutes after its stamp, except
-- test_poke_radar_no_freeze's, touched hours later (by a copy, say).
-- demo_follower_collision_realtime_20261018 has a date but no time, so no
-- stamp.
local OLD_KEPT = WORK .. "/add-notes-kept"
local OLD_SCRATCH = WORK .. "/add-notes-scratch"
local OLD_VIDEOS = {
    { name = "issue-111_test_recording_notes_20261020_212036.mp4", fresh = true,
      at = "2026-10-20T21:20:36+11:00", mtime = "2026-10-20 21:21:10" },
    { name = "test_follower_swap_20261020_165719.mp4", mtime = "2026-10-20 17:04:30" },
    { name = "test_poke_radar_no_freeze_20261020_093844.mp4", mtime = "2026-10-20 14:10:00" },
    { name = "demo_pc_anywhere_20261019_122254.mp4", mtime = "2026-10-19 12:23:40" },
    { name = "test_shiny_chaining_20261019_122027.mp4", mtime = "2026-10-19 12:21:15" },
    { name = "demo_follower_collision_realtime_20261018.mp4", mtime = "2026-10-18 13:58:00" },
    { name = "issue-32-demo.mp4", mtime = "2026-10-10 11:00:00" },
    { name = "issue-24-safety-net.mp4", mtime = "2026-10-05 20:30:15", test = SOME_TEST },
    { name = "route4_no_warp.mp4", mtime = "2026-10-01 09:00:00" },
}
local OLD_PAGE = { kept = OLD_KEPT, scratch = OLD_SCRATCH, tz = SYDNEY }

-- The kept cards' file names, in page order, joined with ", ".
local function card_order(page)
    local names = {}
    for i, c in ipairs(kept_cards_of(page)) do names[i] = tostring(c.file) end
    return table.concat(names, ", ")
end

local function check_add_notes_page()
    run("mkdir -p " .. sh_quote(OLD_KEPT) .. " " .. sh_quote(OLD_SCRATCH))
    local want = {}
    local fresh_json
    for i, v in ipairs(OLD_VIDEOS) do
        want[i] = v.name
        local video = OLD_KEPT .. "/" .. v.name
        if v.fresh then
            write_file(video, "\0\0\0\0")
            fresh_json = video:gsub("%.mp4$", ".json")
            local code, output = run("python3 " .. sh_quote(NOTES_TOOL) .. " write " .. sh_quote(video)
                .. " --test " .. sh_quote(REPO_TEST) .. " --recorded-at " .. v.at
                .. " --results " .. sh_quote(results_file("old_page_results.json", 12, 0))
                .. " --repo " .. sh_quote(REPO) .. " --notes " .. sh_quote(notes_file("old_page_fresh.txt", COMPLETE)))
            if code ~= 0 then fw.log_error("fixture notes failed for " .. v.name .. ": " .. output) end
        end
        make_video(OLD_KEPT, v.name, v.mtime)
    end
    want = table.concat(want, ", ")

    local page = get("/", OLD_PAGE)
    local ok = fw.assert_eq(card_order(page), want, "N3: before add-notes, the page lists the folder in this order")
    if not ok then dump("N3 page before", page.output) end
    local fresh_before = snapshot(fresh_json)

    -- Notes for every video without a notes file.
    local notes = notes_file("old_notes.txt", OLD_NOTES)
    for _, v in ipairs(OLD_VIDEOS) do
        if not v.fresh then
            local code, output = add_notes(OLD_KEPT, v.name, notes, v.test and ("--test " .. sh_quote(v.test)))
            local f = read_json(OLD_KEPT .. "/" .. v.name:gsub("%.mp4$", ".json"))
            local good = fw.assert_eq(code, 0, "N3: add-notes gives " .. v.name .. " notes (exit 0)")
            good = fw.assert_true(f.notes_added_at ~= nil and f.notes_added_at ~= "null",
                "N3: ... and its notes file has notes_added_at") and good
            if not good then dump("N3 add-notes " .. v.name, output) end
        end
    end

    page = get("/", OLD_PAGE)
    ok = fw.assert_eq(page.status, 200, "N3: after add-notes, the page returns 200")
    ok = fw.assert_eq(card_order(page), want, "N3: after add-notes, the cards are in the same order") and ok
    ok = fw.assert_true(not has(page.body, "notes file unreadable") and not has(page.body, "No notes"),
        "N3: ... and every card has its notes") and ok
    ok = fw.assert_eq(snapshot(fresh_json), fresh_before,
        "N3: the notes file record_test.sh wrote stays as it was") and ok
    if not ok then dump("N3 page after", page.output) end

    -- N4: the card of a recording given notes this way.
    local c = card_named(kept_cards_of(page), "issue-24-safety-net.mp4")
    ok = fw.assert_true(c ~= nil, "N4: issue-24-safety-net.mp4 has a card")
    if not c then return end
    local title, link = title_of(c)
    ok = fw.assert_eq(title, "#24 Continue puts you back on the cave floor", "N4: its title is its ticket's") and ok
    ok = fw.assert_eq(link, ISSUES .. "24", "N4: ... linked to the ticket") and ok
    local text = text_of(c.text)
    ok = fw.assert_true(has(text, "Part of #22 Procedurally generated Mt. Moon")
        and has(c.text, 'href="' .. ISSUES .. '22"'), "N4: \"Part of #22 ...\" is under it, linked") and ok
    ok = fw.assert_true(has(text, "PR #33") and has(c.text, 'href="' .. PULLS .. '33"'),
        "N4: \"PR #33\" is under it, linked") and ok
    for _, want_text in ipairs({ OLD_TESTED, OLD_PROBLEM, OLD_EXPECT }) do
        ok = fw.assert_true(has(text, want_text), "N4: the card shows the note \"" .. want_text .. "\"") and ok
    end
    -- The facts line: when it was recorded, its test, and when its notes
    -- were added, in this machine's time to the minute, as "Recorded" is.
    local added = (read_json(OLD_KEPT .. "/issue-24-safety-net.json").notes_added_at or ""):match('^"(.*)"$') or ""
    local added_shows = added:sub(1, 10) .. " " .. added:sub(12, 16)
    local facts = facts_of(c)
    for _, part in ipairs({ "Recorded 2026-10-05 20:30", "test/tests/test_player_sprite.lua",
                            "notes added " .. added_shows }) do
        ok = fw.assert_true(has(facts, part), "N4: the facts line has \"" .. part .. "\"") and ok
    end
    ok = fw.assert_true(not has(c.text, "didn't finish") and not has(facts, "result")
        and not has(facts, "checks"), "N4: the card says nothing about the run's result") and ok
    ok = fw.assert_true(not has(facts, "branch") and not has(facts, "commit"),
        "N4: ... nor a branch or commit") and ok
    ok = fw.assert_true(not has(c.text, "No notes for this recording."),
        "N4: the card doesn't say \"No notes for this recording.\"") and ok
    if not ok then dump("N4 card", c.text) end
end

-- N6: add-notes may rewrite the notes it wrote, to fix them.
local function check_add_notes_rewrite()
    -- N2 gave issue-24-safety-net.mp4 its notes. Its video has since been
    -- touched, so working the time out again would give a new one.
    local json = ADD_ONE .. "/issue-24-safety-net.json"
    local first = read_json(json)
    set_mtime(ADD_ONE .. "/issue-24-safety-net.mp4", "2026-10-21 09:00")
    local fixed = "About 25 s in, the game loads and you stand on open floor."
    local code, output = add_notes(ADD_ONE, "issue-24-safety-net",
        notes_file("old_fixed.txt", OLD_NOTES:gsub("About 20 s in[^\n]*", fixed)))
    local f = read_json(json)
    local ok = fw.assert_eq(code, 0, "N6: add-notes again on a recording it gave notes exits 0")
    ok = fw.assert_eq(f.recorded_at, '"2026-10-05T20:30:15+11:00"', "N6: ... keeping recorded_at") and ok
    ok = fw.assert_eq(f.recorded_at, first.recorded_at, "N6: ... which is the first run's") and ok
    ok = fw.assert_true(has(f.notes or "", json_string(fixed)) and not has(f.notes or "", "About 20 s in"),
        "N6: ... with the new text") and ok
    local was, now = epoch_of((first.notes_added_at or ""):match('^"(.*)"$') or ""),
        epoch_of((f.notes_added_at or ""):match('^"(.*)"$') or "")
    ok = fw.assert_true(was ~= nil and now ~= nil and now >= was, "N6: ... and notes_added_at is the rewrite's") and ok
    if not ok then dump("N6 add-notes again", output) end
end

-- N7: an old video belongs to an old ticket, so a session's rules for its
-- own recordings don't apply to add-notes. They still apply to recording.
local function check_add_notes_in_session()
    local old = notes_file("old_no_parent.txt", without(without(OLD_NOTES,
        "parent: 22 Procedurally generated Mt. Moon"), "pr: 33"))
    -- (N5 left plain_20261020_100000.mp4 without notes.)
    local code, output = add_notes(REFUSE_KEPT, "plain_20261020_100000", old, "", TICKET_SESSION)
    local f = read_json(REFUSE_KEPT .. "/plain_20261020_100000.json")
    local ok = fw.assert_eq(code, 0,
        "N7: in a ticket session for #90, add-notes accepts notes naming ticket: 24 with no parent: (exit 0)")
    ok = fw.assert_eq(f.ticket, '{"number": 24, "title": "Continue puts you back on the cave floor"}',
        "N7: ... and writes ticket 24") and ok
    ok = fw.assert_eq(f.parent, "null", "N7: ... with no parent") and ok
    if not ok then dump("N7 add-notes", output) end

    -- A fix-up's PR isn't an old video's PR either.
    make_video(REFUSE_KEPT, "plain_20261020_090000.mp4", "2026-10-20 09:05")
    code, output = add_notes(REFUSE_KEPT, "plain_20261020_090000", old, "", FIXUP_SESSION)
    f = read_json(REFUSE_KEPT .. "/plain_20261020_090000.json")
    ok = fw.assert_eq(code, 0, "N7: in a fix-up session for #90's PR 97, add-notes accepts them too (exit 0)")
    ok = fw.assert_eq(f.pr, "null", "N7: ... and doesn't give them the fix-up's PR") and ok
    if not ok then dump("N7 add-notes, fix-up", output) end

    -- The same notes, for a recording in the same session: refused.
    code, output = check_as(TICKET_SESSION, "--notes " .. sh_quote(old))
    ok = fw.assert_eq(code, 1, "N7: record_test.sh --check still refuses those notes in the session (exit 1)")
    ok = fw.assert_true(has(output, "#90") and has(output, "parent:"),
        "N7: ... naming the session's ticket, #90, and the missing parent:") and ok
    if not ok then dump("N7 record_test.sh --check", output) end
    code, output = run_as(TICKET_SESSION, "python3 " .. sh_quote(NOTES_TOOL) .. " check " .. sh_quote(old))
    ok = fw.assert_eq(code, 1, "N7: the helper's check, which record_test.sh runs, refuses them (exit 1)")
    ok = fw.assert_true(has(output, "#90") and has(output, "parent:"),
        "N7: ... naming the session's ticket, #90, and the missing parent:") and ok
    if not ok then dump("N7 check", output) end
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

    fw.log("=== Autodev sessions must write notes ===")
    check_sessions()
    check_set_pr()

    fw.log("=== The recordings page with notes (serve_recordings.py --print) ===")
    make_page_fixtures()
    local page = get("/")
    fw.assert_eq(page.code, 0, "--print / exits 0")
    local scratch_at = page.body:find('id="scratch"', 1, true) or (#page.body + 1)
    local kept_cards = kept_cards_of(page)
    local scratch_cards = cards(page.body:sub(scratch_at))
    check_card(page, kept_cards)
    check_order(page, kept_cards)
    check_bad_notes(page, kept_cards, scratch_cards)

    fw.log("=== Notes for recordings kept before notes existed (add-notes) ===")
    check_add_notes_time()
    check_add_notes_page()
    check_add_notes_refused()
    check_add_notes_rewrite()
    check_add_notes_in_session()
    fw.finish()
end)
