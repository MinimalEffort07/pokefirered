-------------------------------------------------------------------------------
-- test_record_game_changes.lua: autodev records only changes you can see in
-- the game (issue #121, part of #120)
-------------------------------------------------------------------------------
-- A recording is made only for a change that makes something on the GBA
-- screen look or behave differently (CLAUDE.md, step 8). As a backstop, in a
-- session autodev started, test/record_test.sh (--check included) refuses to
-- record when the branch changes no file the ROM is built from:
--
--   - what the branch changes is every file that differs from where it left
--     master (git merge-base HEAD origin/master): commits, staged and
--     unstaged edits, untracked new files, deletions and renames;
--   - the game is anything under src/, include/, asm/, data/, graphics/,
--     sound/, constants/ or tools/, plus the top-level Makefile, config.mk,
--     *.mk, ld_script*.ld, sym_*.txt and charmap.txt.
--
-- Each case makes its own fixture repo (test/lib/record_fixture.lua): copies
-- of this checkout's record_test.sh and notes helper, on a branch cut from
-- the repo's first commit, which origin/master points at. The case changes
-- that branch, then runs the copy's --check with complete notes for ticket
-- 90, in a ticket session for #90:
--
--   G1  Only docs/x.md changes, committed: exit 1. The message names the
--       commit it compared with, every path that counts as the game, and
--       "No recording: no in-game behaviour changes."
--   G2  A committed change to src/x.c: exit 0.
--   G3  An unstaged edit to data/x.txt, a tracked file: exit 0.
--   G4  An untracked new file, graphics/new.png: exit 0. Also src/a"b.c, a
--       name git prints in quotes.
--   G5  A committed change to the top-level Makefile: exit 0. A new
--       docs/x.mk alone: exit 1 (a top-level pattern matches only at the
--       top level).
--   G6  include/x.h deleted, not committed: exit 0. Moved to docs/x.h and
--       committed: exit 0 too (the rename deletes include/x.h).
--   G7  G1's branch with no session variables: exit 0. A person may record
--       anything.
--   G8  G2's branch with no origin/master: exit 1, saying it can't tell what
--       the branch changes. The same with origin/master on a history that
--       shares no commit with the branch (no merge base), and in a folder
--       that isn't a git checkout.
--
-- WHERE IT WORKS: every file this test makes is in this run's own folder
-- (fw.out_path), so runs at the same time never meet.
--
-- NEVER A REAL RECORDING: every record_test.sh command here is --check,
-- which exits before the recording lock. A real recording would close every
-- mGBA window on the machine, other sessions' included.
--
-- THE SESSION VARIABLES: autodev's dispatcher starts every session with
-- AUTODEV_ISSUE, AUTODEV_ROLE and, for some roles, AUTODEV_PR and
-- AUTODEV_HANDLED_THROUGH. Every command here first clears all four (env -u),
-- then sets the ones its case needs, so this test checks the same whoever
-- runs it, an autodev session included (as test_recording_notes.lua does).
--
-- HOW A TEST CAN RUN SHELL SCRIPTS: mGBA's Lua has the standard io library,
-- so io.popen starts each command as a separate process and reads what it
-- prints; the emulator just waits (no frames pass) until it is done. The
-- whole test takes a few seconds.
--
-- Run:
--   bash test/run_test.sh test/tests/test_record_game_changes.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local record_fixture = dofile(project_dir .. "test/lib/record_fixture.lua")

local q = record_fixture.quote

-- Every command runs without the session variables, unless its case sets
-- some (see THE SESSION VARIABLES above).
local CLEAN_ENV = "env -u AUTODEV_ISSUE -u AUTODEV_ROLE -u AUTODEV_PR -u AUTODEV_HANDLED_THROUGH "

-- A ticket session for #90, and no session at all.
local TICKET_SESSION = "AUTODEV_ROLE=ticket AUTODEV_ISSUE=90 "
local NO_SESSION = ""

-- Run shell command `cmd` with the session variables `session` sets.
-- Returns its exit code and everything it printed.
local function run(session, cmd)
    local pipe = io.popen(CLEAN_ENV .. session .. cmd .. " 2>&1", "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return nil, ""
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    return code, output
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

local WORK = fw.out_path("game-changes")
os.execute("mkdir -p " .. q(WORK))

-- Complete notes for ticket 90, as a ticket session writes them.
local NOTES = WORK .. "/notes.txt"
do
    local f = io.open(NOTES, "w")
    if f then
        f:write("ticket: 90 Shaking patches survive a field reload\n"
            .. "parent: 87 The Poke Radar\n"
            .. "\n"
            .. "## What's tested\nThe radar's four patches after closing the BAG.\n"
            .. "## The problem\nThe patches vanished.\n"
            .. "## What you should see\nFour patches shake.\n")
        f:close()
    else
        fw.log_error("can't write " .. NOTES)
    end
end

-- A fresh fixture repo named `name`, its branch changed by the shell
-- command `change(repo)` returns, run in the repo's folder (git commands
-- there use repo.git). Nil when that fails, which fails the test.
local function fixture(name, change)
    local repo, err = record_fixture.make(WORK .. "/" .. name, project_dir)
    if not fw.assert_true(repo ~= nil, name .. ": the fixture repo is made") then
        fw.log_error(err)
        return nil
    end
    local code, output = run(NO_SESSION, "sh -c " .. q("cd " .. q(repo.dir) .. " && " .. change(repo)))
    if not fw.assert_eq(code, 0, name .. ": the fixture branch is changed") then
        dump(name .. ": changing the branch", output)
        return nil
    end
    return repo
end

-- The copy's record_test.sh --check, with NOTES, on the stand-in test, in
-- `session`.
local function check(repo, session)
    return run(session, "bash " .. q(repo.record) .. " --check --notes " .. q(NOTES) .. " " .. q(repo.test))
end

-- How G1 and G2 change the branch: G7 and G8 reuse them.
local function docs_change(r) return "echo more >> docs/x.md && " .. r.git .. "commit -qam docs" end
local function src_change(r) return "echo 'int y;' >> src/x.c && " .. r.git .. "commit -qam src" end

-- Each case: its id, what it is, how it changes the branch (a function of
-- the repo, giving a shell command), and the exit code --check must give
-- in a ticket session.
local CASES = {
    { id = "G1", what = "only docs/x.md changes, committed", change = docs_change, want = 1 },
    { id = "G2", what = "a committed change to src/x.c", change = src_change, want = 0 },
    { id = "G3", what = "an unstaged edit to data/x.txt",
      change = function(r) return "echo more >> data/x.txt" end, want = 0 },
    { id = "G4", what = "an untracked new file under graphics/",
      change = function(r) return "mkdir graphics && echo png > graphics/new.png" end, want = 0 },
    { id = "G4", what = "an untracked src/a\"b.c, a name git prints in quotes",
      change = function(r) return "echo x > 'src/a\"b.c'" end, want = 0 },
    { id = "G5", what = "a committed change to the top-level Makefile",
      change = function(r) return "echo 'clean:' >> Makefile && " .. r.git .. "commit -qam make" end, want = 0 },
    { id = "G5", what = "a new docs/x.mk alone, committed",
      change = function(r) return "echo 'x:' > docs/x.mk && " .. r.git .. "add -A && " .. r.git .. "commit -qm mk" end,
      want = 1 },
    { id = "G6", what = "include/x.h deleted, not committed",
      change = function(r) return "rm include/x.h" end, want = 0 },
    { id = "G6", what = "include/x.h moved to docs/x.h, committed",
      change = function(r) return r.git .. "mv include/x.h docs/x.h && " .. r.git .. "commit -qm move" end, want = 0 },
}

local function check_cases()
    for i, case in ipairs(CASES) do
        local repo = fixture("case" .. i, case.change)
        if repo ~= nil then
            local code, output = check(repo, TICKET_SESSION)
            if not fw.assert_eq(code, case.want, case.id .. ": a ticket session's --check, " .. case.what
                    .. ": exit " .. case.want) then
                dump(case.id .. ": " .. case.what, output)
            end
        end
    end
end

-- G1's refusal says what was compared, what counts as the game, and what
-- to do instead; G7: with no session the same branch is accepted.
local function check_refusal()
    local repo = fixture("g1", docs_change)
    if repo == nil then return end
    local code, output = check(repo, TICKET_SESSION)
    local ok = fw.assert_eq(code, 1, "G1: the docs-only branch is refused (exit 1)")
    ok = fw.assert_true(repo.base ~= nil and has(output, repo.base:sub(1, 7)),
        "G1: ... naming the commit it compared with, " .. tostring(repo.base)) and ok
    for _, path in ipairs({ "src/", "include/", "asm/", "data/", "graphics/", "sound/", "constants/",
                            "tools/", "Makefile", "config.mk", "*.mk", "ld_script*.ld", "sym_*.txt",
                            "charmap.txt" }) do
        ok = fw.assert_true(has(output, path), "G1: ... naming " .. path .. " as the game") and ok
    end
    ok = fw.assert_true(has(output, "No recording: no in-game behaviour changes."),
        "G1: ... and saying to write \"No recording: no in-game behaviour changes.\" in the PR") and ok
    if not ok then dump("G1", output) end

    code, output = check(repo, NO_SESSION)
    if not fw.assert_eq(code, 0, "G7: with no session variables, the docs-only branch is accepted (exit 0)") then
        dump("G7", output)
    end
end

-- G8: when it can't tell what the branch changes, it refuses. Each case
-- makes G2's src/ change first, so only the missing comparison can refuse.
local CANT_TELL = {
    { what = "with no origin/master",
      change = function(r) return r.git .. "update-ref -d refs/remotes/origin/master" end },
    -- origin/master on a commit of its own, with no parent: the branch and
    -- it share no commit, so there is no merge base.
    { what = "with no merge base",
      change = function(r)
          return "tree=$(" .. r.git .. "write-tree)"
              .. " && orphan=$(" .. r.git .. "commit-tree \"$tree\" -m orphan)"
              .. " && " .. r.git .. "update-ref refs/remotes/origin/master \"$orphan\""
      end },
    -- The fixture's .git moved aside: a folder of files, as an export is.
    { what = "in a folder that isn't a git checkout",
      change = function(r) return "mv .git ../" .. r.dir:match("[^/]+$") .. ".git-moved" end },
}

local function check_cant_tell()
    for i, case in ipairs(CANT_TELL) do
        local repo = fixture("g8-" .. i, function(r) return src_change(r) .. " && " .. case.change(r) end)
        if repo ~= nil then
            local code, output = check(repo, TICKET_SESSION)
            local ok = fw.assert_eq(code, 1, "G8: " .. case.what .. ", a src/ change is refused (exit 1)")
            ok = fw.assert_true(has(output, "can't tell what the branch changes"),
                "G8: ... saying it can't tell what the branch changes") and ok
            if not ok then dump("G8: " .. case.what, output) end
        end
    end
end

-------------------------------------------------------------------------------
-- Test
-------------------------------------------------------------------------------

fw.run(function()
    fw.log("=== Autodev records only game changes (record_test.sh --check) ===")
    check_cases()
    check_refusal()
    check_cant_tell()
    fw.finish()
end)
