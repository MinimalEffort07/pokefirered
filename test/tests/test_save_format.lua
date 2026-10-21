-------------------------------------------------------------------------------
-- test_save_format.lua: the save format matches its record,
-- test/save_format.txt, and test/tools/save_format.py catches every kind of
-- change to it (issue #140, part of #139)
-------------------------------------------------------------------------------
--
-- WHY THIS EXISTS:
--   A save is the bytes of a few C structs (SaveBlock1, SaveBlock2, the PC
--   boxes and more) copied to the cartridge's flash chip. A save made by one
--   build is read back by the structs of the next. If a member moved, the
--   newer build reads every value after it from the wrong place, and nothing
--   says so: an old save still passes its checksums. That happened once
--   already, when the quick-select change (48ef17586) moved everything in
--   SaveBlock1 from pcItems on (see the comment above struct SaveBlock1 in
--   include/global.h). So the save format is written
--   down, exactly as the compiler lays it out, in test/save_format.txt, and
--   this test fails the suite when a branch's save format no longer matches
--   it. A change made on purpose updates the record with
--   `python3 test/tools/save_format.py --write` and says so in its PR.
--
-- WHAT IT CHECKS (S1 to S10 are the ticket's scenarios):
--   S1   The repo as committed matches the record: exit 0, naming
--        test/save_format.txt.
--   S2   A u8 inserted before SaveBlock2's playerGender: exit 1, the diff
--        adds a line naming it and changes at least one other SaveBlock2
--        line (an offset moved).
--   S3   struct ItemSlot's quantity made a u8: exit 1, quantity's line
--        changes and no line of SaveBlock1 does (ItemSlot stays 4 bytes),
--        so the tool reads nested types, and changes that keep the size.
--   S4   SaveBlock1's reserved bytes given a use (u8 newThing; then 399
--        unused bytes): exit 1, the diff adding a line naming newThing.
--   S5   A u8 added at the end of struct PokeRadar, which is laid over
--        those reserved bytes: exit 1, shown under struct PokeRadar.
--   S6   SECTOR_SIGNATURE changed in include/save.h: exit 1.
--   S7   A member added to struct UCoords16, which no save holds: exit 0.
--   S8   --write on the S2 tree to a scratch record, then a check against
--        it: exit 0. --write on the repo to a scratch record: byte for
--        byte test/save_format.txt.
--   S9   A tree without tools/agbcc: exit 2, naming agbcc.
--   S10  S2's output names `python3 test/tools/save_format.py --write` and
--        CLAUDE.md's "Breaking changes".
--
-- SCRATCH TREES: S2 to S9 run the tool on a copy of what it compiles, with
--   one edit made to the copy (test/lib/save_format_tool.lua). Each edit is
--   checked to have happened, so a scenario can't pass on an unchanged copy.
--   The repo's own files are never changed: --write goes to a scratch
--   record. Every run names its save format record with --record, so the
--   tool checks or writes only that one, never the saved-ID record
--   (test_save_ids.lua's).
--
-- EXIT CODES ARE CHECKED WITH WHAT WAS PRINTED: the tool exits 1 only for
--   "the save format differs" (a crash is reported as 2, can't run), and
--   each exit-1 scenario also checks the diff shows its change.
--
-- HOW A TEST CAN RUN A PROGRAM: through io.popen (save_format_tool.lua).
--   This test never looks at the game; the ROM boots only because
--   run_test.sh always starts the emulator. Each run of the tool takes about
--   a second.
--
-- Run:
--   bash test/run_test.sh test/tests/test_save_format.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local tool = dofile(project_dir .. "test/lib/save_format_tool.lua")

local RECORD = tool.FORMAT_RECORD
local run_tool, expect_exit, has = tool.run_tool, tool.expect_exit, tool.has
local read_file, write_file = tool.read_file, tool.write_file
local scratch_tree, replace_once = tool.scratch_tree, tool.replace_once

-- The tool's arguments to check scratch tree `tree` (nil: the repo) against
-- the repo's save format record, or the scratch one at `record`: only that
-- record, so the saved IDs (test_save_ids.lua's) never decide the outcome.
local function on_tree(tree, record)
    return tool.on_tree(tree, { format = record or RECORD })
end

-------------------------------------------------------------------------------
-- Editing scratch trees, reading records
-------------------------------------------------------------------------------

-- In `tree`'s copy of `header` (a path under include/), change the body of
-- `struct name` (from its "{" to its "};") with `change`, a function from
-- the old body to the new one. Checks that the struct was found and the body
-- changed, so a scenario can't pass on an unchanged copy.
local function edit_struct(tree, header, name, change, what)
    local path = tree .. "/include/" .. header
    local text = read_file(path) or ""
    local head = "struct " .. name .. "\n{"
    local first = text:find(head, 1, true)
    fw.assert_true(first ~= nil, what .. ": found struct " .. name .. " in " .. header)
    if not first then return end
    local body_start = first + #head
    local body_end = text:find("\n};", body_start, true)
    local body = text:sub(body_start, body_end)
    local new_body = change(body)
    fw.assert_true(new_body ~= nil and new_body ~= body, what .. ": the edit changed struct " .. name)
    write_file(path, text:sub(1, body_start - 1) .. (new_body or body) .. text:sub(body_end + 1))
end

-- The lines of a unified diff in `output` that start with `sign` ("+" or
-- "-"), leaving out the "+++" and "---" file lines.
local function diff_lines(output, sign)
    local lines = {}
    for line in output:gmatch("[^\n]+") do
        if line:sub(1, 1) == sign and line:sub(1, 3) ~= sign:rep(3) then
            table.insert(lines, line)
        end
    end
    return lines
end

-- A record's sections, by name: each is its lines, from its first line
-- (such as "struct SaveBlock1, 15760 bytes (0x3D90)", whose name is
-- "struct SaveBlock1") to the blank line after it. Comments are left out.
local function sections_of(text)
    local sections, current = {}, nil
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        if line == "" or line:sub(1, 1) == "#" then
            current = nil
        elseif line:sub(1, 1) ~= " " then
            current = line:match("^([^,]+)")
            sections[current] = { line }
        elseif current then
            table.insert(sections[current], line)
        end
    end
    return sections
end

-- Section `name` of a record's sections as one text ("" when missing).
local function section_text(sections, name)
    return table.concat(sections[name] or {}, "\n")
end

-- Write scratch tree `tree`'s save format into a scratch record and return
-- that record's sections.
local function tree_sections(tree, what)
    local record = tool.scratch_record("save_format.txt")
    local code, output = run_tool("--write " .. on_tree(tree, record))
    expect_exit(code, output, 0, what .. ": --write")
    return sections_of(read_file(record) or "")
end

-- Does any line in `lines` contain every string in `parts`?
local function any_line_has(lines, parts)
    for _, line in ipairs(lines) do
        local all = true
        for _, part in ipairs(parts) do
            if not has(line, part) then all = false end
        end
        if all then return true end
    end
    return false
end

fw.run(function()
    fw.log("=== S1: the repo as committed matches test/save_format.txt ===")
    local code, output = run_tool(on_tree(nil))
    expect_exit(code, output, 0, "S1 the repo")
    fw.assert_true(has(output, "test/save_format.txt"), "S1 the output names test/save_format.txt")
    fw.assert_true(not has(output, "saved IDs"), "S1 given only --record, the tool checks only the save format")

    fw.log("=== S2: a u8 inserted before SaveBlock2's playerGender ===")
    local s2_tree = scratch_tree()
    edit_struct(s2_tree, "global.h", "SaveBlock2", function(body)
        return replace_once(body, "u8 playerGender;", "u8 newField; u8 playerGender;")
    end, "S2")
    local s2_code, s2_output = run_tool(on_tree(s2_tree))
    expect_exit(s2_code, s2_output, 1, "S2 inserted field")
    fw.assert_true(any_line_has(diff_lines(s2_output, "+"), { "newField" }),
        "S2 the diff adds a line naming newField")
    -- playerGender is one byte further on now, so its old line goes and a
    -- new one comes.
    fw.assert_true(any_line_has(diff_lines(s2_output, "-"), { "0x0008", "playerGender" }),
        "S2 the diff removes playerGender's old line, at 0x0008")
    fw.assert_true(any_line_has(diff_lines(s2_output, "+"), { "0x0009", "playerGender" }),
        "S2 the diff adds playerGender's new line, at 0x0009")

    fw.log("=== S10: S2's output shows the way out ===")
    fw.assert_true(has(s2_output, "python3 test/tools/save_format.py --write"),
        "S10 the output names python3 test/tools/save_format.py --write")
    fw.assert_true(has(s2_output, "CLAUDE.md") and has(s2_output, "Breaking changes"),
        "S10 the output names CLAUDE.md's \"Breaking changes\"")

    local repo = sections_of(read_file(RECORD) or "")
    fw.assert_true(repo["struct SaveBlock1"] ~= nil and repo["struct ItemSlot"] ~= nil
        and repo["struct PokeRadar"] ~= nil,
        "the record has sections for SaveBlock1, ItemSlot and PokeRadar")

    fw.log("=== S3: struct ItemSlot's quantity made a u8 (same size) ===")
    local s3_tree = scratch_tree()
    edit_struct(s3_tree, "global.h", "ItemSlot", function(body)
        return replace_once(body, "u16 quantity;", "u8 quantity;")
    end, "S3")
    code, output = run_tool(on_tree(s3_tree))
    expect_exit(code, output, 1, "S3 nested type")
    fw.assert_true(any_line_has(diff_lines(output, "-"), { "quantity", "u16" }),
        "S3 the diff removes quantity's u16 line")
    fw.assert_true(any_line_has(diff_lines(output, "+"), { "quantity", "u8" }),
        "S3 the diff adds quantity's u8 line")
    local s3 = tree_sections(s3_tree, "S3")
    fw.assert_true(section_text(s3, "struct ItemSlot") ~= section_text(repo, "struct ItemSlot"),
        "S3 struct ItemSlot's section changes")
    fw.assert_true(has(section_text(s3, "struct ItemSlot"), "4 bytes"),
        "S3 struct ItemSlot is still 4 bytes")
    fw.assert_eq(section_text(s3, "struct SaveBlock1"), section_text(repo, "struct SaveBlock1"),
        "S3 no line of struct SaveBlock1 changes")

    fw.log("=== S4: SaveBlock1's reserved bytes given a use ===")
    local s4_tree = scratch_tree()
    edit_struct(s4_tree, "global.h", "SaveBlock1", function(body)
        return replace_once(body, "u8 unused_348C[400];", "u8 newThing; u8 unused_348C[399];")
    end, "S4")
    code, output = run_tool(on_tree(s4_tree))
    expect_exit(code, output, 1, "S4 reserved bytes")
    fw.assert_true(any_line_has(diff_lines(output, "+"), { "newThing" }),
        "S4 the diff adds a line naming newThing")

    fw.log("=== S5: a u8 added at the end of struct PokeRadar ===")
    local s5_tree = scratch_tree()
    edit_struct(s5_tree, "poke_radar.h", "PokeRadar", function(body)
        return body .. "    u8 newRadarField;\n"
    end, "S5")
    code, output = run_tool(on_tree(s5_tree))
    expect_exit(code, output, 1, "S5 overlay")
    fw.assert_true(any_line_has(diff_lines(output, "+"), { "newRadarField" }),
        "S5 the diff adds a line naming newRadarField")
    local s5 = tree_sections(s5_tree, "S5")
    fw.assert_true(has(section_text(s5, "struct PokeRadar"), "newRadarField"),
        "S5 the new member shows under struct PokeRadar")

    fw.log("=== S6: SECTOR_SIGNATURE changed in include/save.h ===")
    local s6_tree = scratch_tree()
    local save_h = s6_tree .. "/include/save.h"
    local text, n = (read_file(save_h) or ""):gsub("(#define SECTOR_SIGNATURE )([^\n]+)", "%1((%2) + 1)")
    fw.assert_eq(n, 1, "S6 SECTOR_SIGNATURE's #define changed")
    write_file(save_h, text)
    code, output = run_tool(on_tree(s6_tree))
    expect_exit(code, output, 1, "S6 flash format")
    fw.assert_true(any_line_has(diff_lines(output, "+"), { "SECTOR_SIGNATURE" }),
        "S6 the diff shows SECTOR_SIGNATURE's new value")

    fw.log("=== S7: a member added to struct UCoords16, which no save holds ===")
    local s7_tree = scratch_tree()
    edit_struct(s7_tree, "global.h", "UCoords16", function(body)
        return body .. "    u16 z;\n"
    end, "S7")
    code, output = run_tool(on_tree(s7_tree))
    expect_exit(code, output, 0, "S7 not saved")

    fw.log("=== S8: --write round-trips ===")
    local s8_record = tool.scratch_record("save_format.txt")
    code, output = run_tool("--write " .. on_tree(s2_tree, s8_record))
    expect_exit(code, output, 0, "S8 --write on the S2 tree")
    fw.assert_true(not has(output, "saved IDs"), "S8 given only --record, --write writes only that record")
    code, output = run_tool(on_tree(s2_tree, s8_record))
    expect_exit(code, output, 0, "S8 the S2 tree against the record --write gave it")
    local repo_record = tool.scratch_record("save_format.txt")
    code, output = run_tool("--write " .. on_tree(nil, repo_record))
    expect_exit(code, output, 0, "S8 --write on the repo")
    local written, committed = read_file(repo_record), read_file(RECORD)
    fw.assert_true(written ~= nil and written == committed,
        "S8 --write on the repo gives test/save_format.txt byte for byte")

    fw.log("=== S9: a tree without tools/agbcc ===")
    local s9_tree = scratch_tree(false)
    code, output = run_tool(on_tree(s9_tree))
    expect_exit(code, output, 2, "S9 no agbcc")
    fw.assert_true(has(output, "agbcc"), "S9 the output names agbcc")

    tool.remove_scratch()

    fw.finish()
end)
