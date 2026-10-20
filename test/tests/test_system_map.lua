-------------------------------------------------------------------------------
-- test_system_map.lua
-- docs/SYSTEM_MAP.md names every script, and every file it names exists
-------------------------------------------------------------------------------
--
-- WHY THIS EXISTS:
--   docs/SYSTEM_MAP.md is the one-page overview of how this repo's machinery
--   fits together: autodev, the tests, the recordings, and one line on every
--   script. It is only useful while it is true. CLAUDE.md asks every change
--   that adds, removes or changes a script to update the map in the same PR;
--   this test catches the two ways the map goes stale that a program can
--   see. Whether each description still says what its script does is for
--   the code review (CLAUDE.md step 10).
--
-- WHAT IT CHECKS:
--   This test reads files; it never looks at the game. The ROM still boots
--   only because run_test.sh always starts the emulator.
--
--   1. Every script is on the map. A script is a .sh, .py, .pl or .lua file
--      directly in one of the SCRIPT_DIRS below (not in a folder under it):
--      the top level, upstream's CI scripts, autodev, the test runners, and
--      the shared Lua helpers and tools under test/lib/ and test/tools/.
--      Tests (test/tests/, automation/tests/), prompts and settings are
--      described as groups. The map must name each script by its path from
--      the repo root, inside `backticks`.
--   2. Nothing on the map is gone. Every `backticked` span on the map that is
--      shaped like a path from the repo root to a file of a CHECKED_TYPES
--      type must exist. Spans holding a space, "*", "<" or "{", or starting
--      with "/" or "~", are commands, patterns or machine paths, and are not
--      checked.
--
-- Run:
--   bash test/run_test.sh test/tests/test_system_map.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")

local MAP = "docs/SYSTEM_MAP.md"

-- The folders (from the repo root) whose scripts must be on the map, and
-- the file-name patterns that make a file in one of them a script.
local SCRIPT_DIRS = {
    ".", ".github/calcrom", "automation", "automation/bin", "automation/autodev",
    "test", "test/lib", "test/tools",
}
local SCRIPT_PATTERNS = { "*.sh", "*.py", "*.pl", "*.lua" }

-- Scripts too empty to be worth a line.
local NOT_SCRIPTS = { ["automation/autodev/__init__.py"] = true }

-- The file types a path on the map is checked for.
local CHECKED_TYPES = {
    sh = true, py = true, pl = true, lua = true, md = true, json = true, txt = true, yml = true,
}

-------------------------------------------------------------------------------
-- Reading the map
-------------------------------------------------------------------------------

-- Every `backticked` span in the text that is shaped like a path from the
-- repo root to a file of a CHECKED_TYPES type, in order of appearance.
local function named_paths(text)
    local found = {}
    for span in text:gmatch("`([^`\n]+)`") do
        local plain = not span:find("[%s%*<{]") and not span:match("^[/~]")
        local ext = span:match("^[%w_%.%-/]+%.(%w+)$")
        if plain and ext and CHECKED_TYPES[ext] then
            table.insert(found, span)
        end
    end
    return found
end

-- The repo's scripts (see SCRIPT_DIRS), as sorted paths from the repo root.
local function list_scripts()
    local name_tests = {}
    for _, pattern in ipairs(SCRIPT_PATTERNS) do
        table.insert(name_tests, "-name '" .. pattern .. "'")
    end
    local scripts = {}
    for _, dir in ipairs(SCRIPT_DIRS) do
        local pipe = io.popen("cd '" .. project_dir .. "' && find '" .. dir
            .. "' -maxdepth 1 -type f \\( " .. table.concat(name_tests, " -o ") .. " \\)")
        for line in pipe:lines() do
            local path = line:gsub("^%./", "")
            if not NOT_SCRIPTS[path] then
                table.insert(scripts, path)
            end
        end
        pipe:close()
    end
    table.sort(scripts)
    return scripts
end

local function read_file(path)
    local f = io.open(project_dir .. path, "r")
    if not f then return nil end
    local text = f:read("*a")
    f:close()
    return text
end

local function exists(path)
    local f = io.open(project_dir .. path, "r")
    if f then f:close() end
    return f ~= nil
end

-------------------------------------------------------------------------------
-- Test
-------------------------------------------------------------------------------

-- Lines with a known answer, so the span reader itself is checked first: a
-- reader that found nothing would otherwise let the second check pass.
local READER_EXAMPLES = {
    { text = "`test/run_test.sh` runs one test", want = { "test/run_test.sh" } },
    { text = "`automation/autodev/decide.py` and `test/lib/walk.lua`",
      want = { "automation/autodev/decide.py", "test/lib/walk.lua" } },
    { text = "see `CLAUDE.md`", want = { "CLAUDE.md" } },
    { text = "a bare `issue.py` is a file at the repo root", want = { "issue.py" } },
    { text = "`test/tests/*.lua`, `automation/prompts/<role>.md`", want = {} },
    { text = "`cd automation && python3 -m unittest`", want = {} },
    { text = "`~/autodev/logs/notes.txt` and `/srv/notes.txt`", want = {} },
    { text = "`pokefirered.gba` and `make firered`", want = {} },
}

fw.run(function()
    fw.log("=== " .. MAP .. " names every script, and nothing that is gone ===")

    for _, ex in ipairs(READER_EXAMPLES) do
        local got = table.concat(named_paths(ex.text), ", ")
        fw.assert_eq(got, table.concat(ex.want, ", "), "paths named in: " .. ex.text)
    end

    local map = read_file(MAP)
    fw.assert_true(map ~= nil, MAP .. " exists")
    map = map or ""

    local on_map = {}
    for _, path in ipairs(named_paths(map)) do
        on_map[path] = true
    end

    -- 1. Every script is on the map.
    local scripts = list_scripts()
    local missing = {}
    for _, path in ipairs(scripts) do
        if not on_map[path] then
            fw.log_error("not on the map: " .. path)
            table.insert(missing, path)
        end
    end
    -- Guards against a listing that silently found nothing (wrong folder,
    -- find missing): the repo has dozens of scripts.
    fw.assert_true(#scripts >= 40, "found " .. #scripts .. " scripts to look for on the map")
    fw.assert_eq(#missing, 0, "scripts the map doesn't name by their path"
        .. (#missing > 0 and (": " .. table.concat(missing, "; ")) or ""))

    -- 2. Nothing on the map is gone.
    local named = {}
    for path in pairs(on_map) do
        table.insert(named, path)
    end
    table.sort(named)
    local gone = {}
    for _, path in ipairs(named) do
        if not exists(path) then
            fw.log_error("on the map but not in the repo: " .. path)
            table.insert(gone, path)
        end
    end
    fw.assert_eq(#gone, 0, "files the map names that don't exist"
        .. (#gone > 0 and (": " .. table.concat(gone, "; ")) or ""))

    fw.finish()
end)
