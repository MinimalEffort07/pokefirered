-------------------------------------------------------------------------------
-- save_format_tool.lua: run test/tools/save_format.py on scratch trees, for
-- the tests of its two records (test_save_format.lua, #140, and
-- test_save_ids.lua, #142)
-------------------------------------------------------------------------------
--
-- The tool checks a tree against test/save_format.txt (the save format)
-- and test/save_ids.txt (the saved IDs). Its tests run it on scratch trees:
-- copies of what it compiles (the repo's include/ folder, made by
-- `mktemp -d`, with tools/agbcc linked in), each with one edit made to the
-- copy. The repo's own files are never changed: a run that writes records
-- names scratch ones (scratch_record), and the tool writes only those.
--
--   local tool = dofile(project_dir .. "test/lib/save_format_tool.lua")
--   local tree = tool.scratch_tree()
--   local code, output = tool.run_tool(tool.on_tree(tree, { format = tool.FORMAT_RECORD }))
--   tool.expect_exit(code, output, 1, "S2 inserted field")
--   ...
--   tool.remove_scratch()            -- at the end of the test
--
-- WHERE SCRATCH GOES: run_test.sh points TMPDIR at the run's own folder, so
-- mktemp makes the copies there. remove_scratch removes them at the end; a
-- test that stops early leaves them in its run folder, like its other files.
--
-- HOW A TEST CAN RUN A PROGRAM: mGBA's Lua has the standard io library, so
-- io.popen starts each command as a separate process and reads what it
-- prints; the emulator just waits (no frames pass) until it is done.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(script_dir .. "framework.lua")

local M = {}

M.TOOL = project_dir .. "test/tools/save_format.py"
M.FORMAT_RECORD = project_dir .. "test/save_format.txt"
M.IDS_RECORD = project_dir .. "test/save_ids.txt"

-- `s` in single quotes, safe to paste into a shell command.
function M.quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end
local q = M.quote

-- Run shell command `cmd`. Returns its exit code and everything it printed
-- (stdout and stderr together).
function M.run(cmd)
    local pipe = io.popen(cmd .. " 2>&1", "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return nil, ""
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    return code, output
end

-- The repo's full path: a link in a scratch tree must not be relative.
local REPO_PATH = select(2, M.run("cd " .. q(project_dir) .. " && pwd")):match("^(%S+)")

-- Run the tool with `args` (already quoted). Returns its exit code and output.
function M.run_tool(args)
    return M.run("python3 " .. q(M.TOOL) .. (args and (" " .. args) or ""))
end

-- Check that the tool exited with `want`; if not, show what it printed, one
-- log line per line (run_test.sh shows only lines mGBA starts with
-- "Scripting:", so the rest of a multi-line message would be lost).
function M.expect_exit(code, output, want, what)
    fw.assert_eq(code, want, what .. ": exit code")
    if code ~= want then
        fw.log_error(what .. ": the tool printed:")
        for line in output:gmatch("[^\n]+") do
            fw.log_error("  " .. line)
        end
    end
end

function M.read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local text = f:read("a")
    f:close()
    return text
end

function M.write_file(path, text)
    local f = io.open(path, "w")
    if not f then
        fw.log_error("can't write " .. path)
        return
    end
    f:write(text)
    f:close()
end

-- Does `text` contain `s`, as plain text?
function M.has(text, s)
    return text:find(s, 1, true) ~= nil
end

-- Every folder made, removed by remove_scratch.
local scratch_dirs = {}

-- A new empty folder, from `mktemp -d`.
function M.scratch_dir()
    local _, output = M.run("mktemp -d")
    local dir = output:match("^(%S+)")
    fw.assert_true(dir ~= nil, "mktemp -d made a folder")
    table.insert(scratch_dirs, dir)
    return dir
end

-- A scratch tree: a copy of the repo's include/ folder, with tools/agbcc
-- linked to the repo's own (unless `with_agbcc` is false). Returns its path.
function M.scratch_tree(with_agbcc)
    local dir = M.scratch_dir()
    local code = M.run("cp -R " .. q(project_dir .. "include") .. " " .. q(dir .. "/include"))
    fw.assert_eq(code, 0, "copied include/ into a scratch tree")
    if with_agbcc ~= false then
        code = M.run("mkdir " .. q(dir .. "/tools") .. " && ln -s "
            .. q(REPO_PATH .. "/tools/agbcc") .. " " .. q(dir .. "/tools/agbcc"))
        fw.assert_eq(code, 0, "linked tools/agbcc into a scratch tree")
    end
    return dir
end

-- A path for a scratch record called `name`, in a new folder.
function M.scratch_record(name)
    return M.scratch_dir() .. "/" .. name
end

-- The tool's arguments to check scratch tree `tree` (nil: the repo)
-- against `records`, a table whose `format` and `ids`, when set, are the
-- save format and ID records to use. The tool works only on the records it
-- is given; given none, on both of the repo's.
function M.on_tree(tree, records)
    local args = {}
    if tree then table.insert(args, "--root " .. q(tree)) end
    records = records or {}
    if records.format then table.insert(args, "--record " .. q(records.format)) end
    if records.ids then table.insert(args, "--ids-record " .. q(records.ids)) end
    return table.concat(args, " ")
end

-- `text` with plain text `old` replaced by `new`, or nil unless `old` is in
-- it exactly once.
function M.replace_once(text, old, new)
    local at = text:find(old, 1, true)
    if not at or text:find(old, at + 1, true) then return nil end
    return text:sub(1, at - 1) .. new .. text:sub(at + #old)
end

-- In `tree`'s copy of `header` (a path under include/), replace plain text
-- `old` with `new`. Checks that `old` was there exactly once, so a scenario
-- can't pass on an unchanged copy.
function M.edit_header(tree, header, old, new, what)
    local path = tree .. "/include/" .. header
    local edited = M.replace_once(M.read_file(path) or "", old, new)
    fw.assert_true(edited ~= nil, what .. ": found \"" .. old:gsub("\n", "\\n") .. "\" once in " .. header)
    if edited then M.write_file(path, edited) end
end

-- Remove every scratch folder. Each is checked to be one mktemp made (a
-- full path whose last name is tmp. and ten letters or digits), so a
-- mistake here can never remove anything else.
function M.remove_scratch()
    for _, dir in ipairs(scratch_dirs) do
        local name = dir:match("^/.+/([^/]+)$")
        if name and name:match("^tmp%.%w+$") then
            os.execute("rm -rf " .. q(dir))
        end
    end
end

return M
