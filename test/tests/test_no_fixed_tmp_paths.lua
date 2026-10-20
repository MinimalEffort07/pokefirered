-------------------------------------------------------------------------------
-- test_no_fixed_tmp_paths.lua
-- No script under test/ uses a fixed /tmp path, apart from two hand-made
-- savestate fixtures (issue #50)
-------------------------------------------------------------------------------
--
-- WHY THIS EXISTS:
--   test/run_test.sh gives every run its own new folder, /tmp/mgba-test.XXXXXX
--   (passed to Lua in MGBA_TEST_DIR and reached through fw.out_path in
--   test/lib/framework.lua). That is what lets many tests run at once, from
--   one checkout or from several git worktrees. A script that writes to a
--   path that is the same on every run, such as /tmp/lr_demo_frames, breaks
--   this: two runs at the same time share that path, and each one deletes or
--   overwrites the other's files.
--
-- WHAT IT CHECKS:
--   This test reads source files; it never looks at the game. It reads every
--   .lua, .sh and .py file under test/ and fails if any /tmp path appears in
--   their code, unless the ALLOWED table below lists it. A source file can't
--   show whether a path is read or written, so the check is on every use.
--   The ROM still boots only because run_test.sh always starts the emulator.
--
--   Comments are skipped: commands written there are for a person to type,
--   and the person picks the folder. In Python files, every line holding a
--   triple quote (""" or ''') and every line between two such lines is
--   skipped too, which covers docstrings (and, rarely, code). The scan goes
--   line by line and tracks quotes, so a "--" or "#" inside a string is not
--   taken for the start of a comment. It does not understand Lua's
--   multi-line [[...]] strings or --[[...]] comments, which no file here
--   uses: a "--" inside [[...]] would hide the rest of that line.
--
-- THE ONLY FIXED PATHS ALLOWED:
--   The two hand-made savestate fixtures (see ALLOWED). The tests only read
--   them; the test/tools/ scripts that make them are run by hand, once.
--   And record_test.sh's lock file, which is shared on purpose: it is how
--   recordings from any worktree know about each other and take turns.
--   Three more /tmp names are allowed because no file in them is used under
--   the same name by two runs: mktemp templates, record_test.sh's video
--   folder (each video gets a timestamped name; the recordings page reads
--   it, and recording_notes.py's set-pr finds notes files there), and
--   fw.out_path's fallback
--   for scripts loaded by hand, which run_test.sh never uses.
--
-- Run:
--   bash test/run_test.sh test/tests/test_no_fixed_tmp_paths.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")

-- This file has to name the allowed paths, so it skips its own source.
local SELF = "test/tests/test_no_fixed_tmp_paths.lua"

-- Each entry allows one /tmp path. With `file` set, the path is allowed in
-- that one file only; without it, anywhere under test/.
local ALLOWED = {
    -- The two fixed paths: savestate fixtures that a person makes once, by
    -- hand, with the test/tools/ scripts named below. Tests only read them,
    -- so two runs at the same time never write to them.
    { path = "/tmp/pokefirered-route1.ss",
      why = "Route 1 fixture read by test_roaming_route1; made by hand with "
         .. "test/tools/save_route1_state.lua or warp_to_route1.lua" },
    { path = "/tmp/pokefirered-route1-grass.ss",
      why = "tall-grass fixture read by test_shiny_chaining and "
         .. "test/demos/demo_shiny_chaining; made by hand with "
         .. "test/tools/warp_to_route1_grass.lua or explore_route1.lua" },
    -- Fixed on purpose: one lock for the whole machine.
    { path = "/tmp/autodev-record.lock", file = "test/record_test.sh",
      why = "machine-wide flock so recordings from any worktree take turns; "
         .. "nothing is ever written to it" },

    -- Not fixed for a run (mktemp templates are allowed by a rule in
    -- is_allowed, since their names differ from script to script).
    { path = "/tmp/mgba-recordings", file = "test/record_test.sh",
      why = "video folder: each video gets a timestamped name, and only one "
         .. "recording runs at a time" },
    { path = "/tmp/mgba-recordings", file = "test/serve_recordings.py",
      why = "serves record_test.sh's video folder, read only" },
    { path = "/tmp/mgba-recordings", file = "test/tools/recording_notes.py",
      why = "set-pr finds a recording's notes file there (after ~/recordings) "
         .. "by the name a person gives it, and changes only that file" },
    { path = "/tmp", file = "test/lib/framework.lua",
      why = "fw.out_path's fallback for a script loaded by hand in mGBA; "
         .. "run_test.sh and record_test.sh always set MGBA_TEST_DIR" },
}

-------------------------------------------------------------------------------
-- Scanner
-------------------------------------------------------------------------------

-- The comment marker of each file type the scan reads.
local COMMENT_MARKER = { lua = "--", sh = "#", py = "#" }

-- The code part of one source line: the text before its comment starts.
-- A marker inside a '...' or "..." string does not start a comment. A shell
-- or Python "#" starts one only at the start of the line or after a space,
-- because in shell "$#" and "${#name}" are code.
local function code_part(line, marker)
    local quote = nil
    local i = 1
    while i <= #line do
        local c = line:sub(i, i)
        if quote then
            if c == "\\" then
                i = i + 1  -- the next character is escaped; skip it
            elseif c == quote then
                quote = nil
            end
        elseif c == '"' or c == "'" then
            quote = c
        elseif line:sub(i, i + #marker - 1) == marker then
            if marker == "--" or i == 1 or line:sub(i - 1, i - 1):match("%s") then
                return line:sub(1, i - 1)
            end
        end
        i = i + 1
    end
    return line
end

-- Every /tmp path in a piece of code. "/tmp" only counts at the start of a
-- path: not after a character that could belong to a longer path (so
-- "$HOME/tmp" and "build/tmp" don't count) and not followed by more of a
-- name (so "/tmpfoo" doesn't count). The path runs on over the characters
-- that file names here use.
local function tmp_paths(code)
    local found = {}
    local start = 1
    while true do
        local s, e = code:find("/tmp", start, true)
        if not s then break end
        local before = s > 1 and code:sub(s - 1, s - 1) or ""
        local after = code:sub(e + 1, e + 1)
        if not before:match("[%w_%.%-/$~}]") and not after:match("[%w_%.%-]") then
            table.insert(found, code:match("^/tmp[%w_%.%-/]*", s))
        end
        start = e + 1
    end
    return found
end

local function is_allowed(file, path, code)
    -- A mktemp template such as /tmp/mgba-test.XXXXXX: mktemp swaps the X's
    -- for random characters, so every run gets a new folder.
    if path:match("XXXXXX$") and code:find("mktemp", 1, true) then
        return true
    end
    for _, a in ipairs(ALLOWED) do
        if a.path == path and (a.file == nil or a.file == file) then
            return true
        end
    end
    return false
end

-- All fixed /tmp paths in one file, as { line = n, path = p } entries.
local function scan_file(file, lang)
    local f = io.open(project_dir .. file, "r")
    if not f then return nil end
    local findings = {}
    local in_docstring = false
    local n = 0
    for line in f:lines() do
        n = n + 1
        local skip = false
        if lang == "py" then
            -- A line holding """ or ''' opens or closes a docstring (or is a
            -- one-line docstring); it and every line inside are skipped.
            local quotes = select(2, line:gsub('"""', "")) + select(2, line:gsub("'''", ""))
            skip = in_docstring or quotes > 0
            if quotes % 2 == 1 then in_docstring = not in_docstring end
        end
        if not skip then
            local code = code_part(line, COMMENT_MARKER[lang])
            for _, p in ipairs(tmp_paths(code)) do
                if not is_allowed(file, p, code) then
                    table.insert(findings, { line = n, path = p })
                end
            end
        end
    end
    f:close()
    return findings
end

-------------------------------------------------------------------------------
-- Test
-------------------------------------------------------------------------------

-- Lines with a known answer, so the scanner itself is checked first: a
-- scanner that found nothing would otherwise let the real check pass.
local SCANNER_EXAMPLES = {
    { lang = "lua", line = 'local FRAME_DIR = "/tmp/lr_demo_frames"', want = "/tmp/lr_demo_frames" },
    { lang = "lua", line = 'fw.screenshot("/tmp/a.png")  -- not /tmp/b.png', want = "/tmp/a.png" },
    { lang = "lua", line = "--   ffmpeg -i '/tmp/frames/*.png' out.gif", want = nil },
    { lang = "lua", line = 'os.execute("echo --x /tmp/y")', want = "/tmp/y" },
    { lang = "sh",  line = 'OUT="/tmp/frames"  # frames go here', want = "/tmp/frames" },
    { lang = "sh",  line = "# OUT=/tmp/frames", want = nil },
    { lang = "sh",  line = 'DIR="$HOME/tmp/x"', want = nil },
}

fw.run(function()
    fw.log("=== No fixed /tmp paths under test/ ===")

    for _, ex in ipairs(SCANNER_EXAMPLES) do
        local got = tmp_paths(code_part(ex.line, COMMENT_MARKER[ex.lang]))[1]
        fw.assert_eq(got, ex.want, "scanner finds " .. tostring(ex.want) .. " in: " .. ex.line)
    end

    for _, a in ipairs(ALLOWED) do
        fw.log("allowed: " .. a.path .. (a.file and (" in " .. a.file) or "") .. " (" .. a.why .. ")")
    end

    -- List the files from the project root, so paths read "test/...".
    local pipe = io.popen("cd '" .. project_dir .. "' && find test -type f "
        .. "\\( -name '*.lua' -o -name '*.sh' -o -name '*.py' \\) | sort")
    local scanned = 0
    local bad = {}  -- "file:line path" for every fixed path found
    for file in pipe:lines() do
        if file ~= SELF then
            local findings = scan_file(file, file:match("%.(%w+)$"))
            if findings then
                scanned = scanned + 1
                for _, hit in ipairs(findings) do
                    local where = string.format("%s:%d %s", file, hit.line, hit.path)
                    fw.log_error("fixed /tmp path (use fw.out_path): " .. where)
                    table.insert(bad, where)
                end
            end
        end
    end
    pipe:close()

    -- Guards against a scan that silently read nothing (wrong directory,
    -- find missing): test/ holds dozens of scripts.
    fw.assert_true(scanned >= 40, "scanned " .. scanned .. " scripts under test/")
    -- One assertion for the whole scan; on failure its message lists every
    -- path found, so the results file names them all.
    fw.assert_eq(#bad, 0, "fixed /tmp paths in scripts under test/"
        .. (#bad > 0 and (": " .. table.concat(bad, "; ")) or ""))
    fw.finish()
end)
