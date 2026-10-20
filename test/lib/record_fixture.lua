-------------------------------------------------------------------------------
-- record_fixture.lua: a small git repo holding copies of the recorder, for
-- tests of what test/record_test.sh refuses in an autodev session (#121)
-------------------------------------------------------------------------------
--
-- In an autodev session, record_test.sh refuses to record when the branch
-- changes nothing the game is built from. It judges the checkout it sits in,
-- against where that checkout's branch left origin/master. So a test can't
-- check it on the checkout the test runs in: that branch may or may not
-- change the game. It makes one of these instead, changes its branch the way
-- a case needs, and runs the copy's record_test.sh --check:
--
--   local repo, err = record_fixture.make(dir, project_dir)
--
-- makes a git repo in folder `dir`, which must not exist yet. Its first
-- commit, on master, holds:
--   - copies of project_dir's test/record_test.sh and
--     test/tools/recording_notes.py (project_dir ends with a /);
--   - a stand-in test, test/tests/test_stand_in.lua (never run: --check
--     only needs the file to exist);
--   - one small file in each of src/, include/, data/ and docs/, and a
--     top-level Makefile, for the cases to change.
-- refs/remotes/origin/master points at that commit, as if fetched, and the
-- repo is left on a new branch, issue-90-fixture, with nothing changed yet.
--
-- It returns a table, or nil and what went wrong:
--   repo.dir     the folder
--   repo.record  its copy of record_test.sh
--   repo.test    the stand-in test
--   repo.base    the first commit's full hash
--   repo.git     the start of a git command run in the repo, with a fixed
--                name and email and no signing, so the machine's git
--                settings don't matter: repo.git .. "commit -qam message"
--
-- record_fixture.quote(s) is `s` in single quotes, safe in a shell command.
--
-- WHERE IT WORKS: only in `dir`, which the test puts in its own run folder
-- (fw.out_path), so runs at the same time never meet.
-------------------------------------------------------------------------------

local M = {}

function M.quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- Run shell command `cmd`. Returns its exit code and everything it printed.
local function run(cmd)
    local pipe = io.popen(cmd .. " 2>&1", "r")
    if pipe == nil then return nil, "io.popen failed for: " .. cmd end
    local output = pipe:read("a") or ""
    local _, _, code = pipe:close()
    return code, output
end

function M.make(dir, project_dir)
    local q = M.quote
    local git = "git -C " .. q(dir) .. " -c user.name=test -c user.email=test@example.com "
        .. "-c commit.gpgsign=false "
    local files = {
        ["src/x.c"] = "int x;",
        ["include/x.h"] = "extern int x;",
        ["data/x.txt"] = "data",
        ["docs/x.md"] = "# Docs",
        ["Makefile"] = "all:",
        ["test/tests/test_stand_in.lua"] = "-- a stand-in: never run",
    }
    local cmd = "mkdir " .. q(dir)
        .. " && mkdir -p " .. q(dir .. "/test/tools") .. " " .. q(dir .. "/test/tests")
        .. " " .. q(dir .. "/src") .. " " .. q(dir .. "/include") .. " " .. q(dir .. "/data")
        .. " " .. q(dir .. "/docs")
        .. " && cp " .. q(project_dir .. "test/record_test.sh") .. " " .. q(dir .. "/test/record_test.sh")
        .. " && cp " .. q(project_dir .. "test/tools/recording_notes.py")
        .. " " .. q(dir .. "/test/tools/recording_notes.py")
    for path, text in pairs(files) do
        cmd = cmd .. " && echo " .. q(text) .. " > " .. q(dir .. "/" .. path)
    end
    cmd = cmd .. " && git init -q -b master " .. q(dir)
        .. " && " .. git .. "add -A && " .. git .. "commit -q -m base"
        .. " && " .. git .. "update-ref refs/remotes/origin/master HEAD"
        .. " && " .. git .. "checkout -q -b issue-90-fixture"
        .. " && " .. git .. "rev-parse HEAD"
    local code, output = run(cmd)
    if code ~= 0 then return nil, "the fixture repo failed: " .. output end
    return {
        dir = dir,
        record = dir .. "/test/record_test.sh",
        test = dir .. "/test/tests/test_stand_in.lua",
        base = output:match("(%x+)%s*$"),
        git = git,
    }
end

return M
