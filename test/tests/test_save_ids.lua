-------------------------------------------------------------------------------
-- test_save_ids.lua: every saved ID matches its record, test/save_ids.txt,
-- and test/tools/save_format.py tells a breaking change to one from a new
-- one (issue #142, part of #139)
-------------------------------------------------------------------------------
--
-- WHY THIS EXISTS:
--   A save stores IDs by number: which flags are set, what each var holds,
--   the items in the bag, the species and moves of each Pokemon, the map
--   the player stands on, which trainers they beat. A build reads those
--   numbers back through its own headers. If ITEM_POTION became 14, every
--   Potion in a save made before would turn into whatever 13 now is, with
--   no error. So every saved ID is written down by name and number in
--   test/save_ids.txt, and this test fails the suite when a branch's IDs
--   no longer match it:
--   - a recorded ID whose number changed, or that is gone (removed or
--     renamed), is a breaking change (CLAUDE.md, "Breaking changes");
--   - a new ID isn't, but fails too until `--write` adds it to the record,
--     which only adds lines.
--
-- WHAT IT CHECKS (C1 to C6 are the ticket's scenarios):
--   C1   The repo as committed matches both records: exit 0, naming
--        test/save_format.txt and test/save_ids.txt.
--   R    The record holds an ID of each kind, at the number its header
--        gives it, and leaves out placeholders (FLAG_0x020, VAR_0x402A,
--        ITEM_034), names with UNUSED in them, the ends of lists
--        (MAPSEC_NONE, MAPSEC_COUNT) and function-like macros
--        (ITEM_TO_BERRY).
--   C2   ITEM_POTION's number changed: exit 1, ITEM_POTION listed as
--        changed, under a heading that calls it a breaking change; the
--        output names the --write command and CLAUDE.md's "Breaking
--        changes". Given only --ids-record, it says nothing of the save
--        format.
--   C3   MOVE_KARATE_CHOP's #define deleted (nothing else in include/
--        names it): exit 1, MOVE_KARATE_CHOP listed as gone.
--   C4   #define ITEM_NEW_THING 376 added: exit 1, ITEM_NEW_THING listed as
--        new, "not a breaking change: run --write to add it", nothing
--        changed or gone. --write to a scratch ID record (writing only
--        that), then a check against it: exit 0, and the scratch record is
--        test/save_ids.txt with one line added.
--   C5   VAR_0x402A renamed VAR_NEW_THING (a placeholder's first use):
--        VAR_NEW_THING listed as new, nothing listed as gone or changed.
--   C6   A tree without include/constants/map_groups.h (the build makes
--        it): exit 2, naming that header and `make firered`.
--   N    A map section added at the end of its list (before MAPSEC_NONE,
--        which the build puts after the last one): only MAPSEC_NEW_THING
--        is listed, as new. MAPSEC_NONE moved, but it is the list's end.
--
-- SCRATCH TREES: C2 to C6 run the tool on a copy of what it compiles, with
--   one edit made to the copy (test/lib/save_format_tool.lua). The repo's
--   own files are never changed: --write goes to a scratch record. C2 to N
--   name their ID record with --ids-record, so the tool checks or writes
--   only that one, and the save format (test_save_format.lua's) never
--   decides the outcome.
--
-- HOW THE OUTPUT IS READ: the tool lists each difference on a line of its
--   own, two spaces in, under a heading line ending in ":" that says what
--   kind it is ("Changed number ...", "Gone ...", "New ..."). listed()
--   reads those back.
--
-- Run:
--   bash test/run_test.sh test/tests/test_save_ids.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local tool = dofile(project_dir .. "test/lib/save_format_tool.lua")

local has = tool.has

-- Check scratch trees against the repo's ID record only.
local IDS = { ids = tool.IDS_RECORD }

-- The record's ID lines, in order, and its IDs by name (comments and blank
-- lines left out).
local function record_lines(text)
    local lines, ids = {}, {}
    for line in (text or ""):gmatch("[^\n]+") do
        if line:sub(1, 1) ~= "#" then
            table.insert(lines, line)
            local name, number = line:match("^(%S+) (%d+)$")
            if name then ids[name] = tonumber(number) end
        end
    end
    return lines, ids
end

-- What the tool's output lists: {changed = {...}, gone = {...}, new =
-- {...}}, each the set of names listed under that kind's heading, and
-- `headings`, each kind's heading line as printed.
local function listed(output)
    local found = { changed = {}, gone = {}, new = {}, headings = {} }
    local kind = nil
    for line in output:gmatch("[^\n]+") do
        if line:sub(1, 1) ~= " " then
            kind = nil
            for _, k in ipairs({ { "Changed", "changed" }, { "Gone", "gone" }, { "New", "new" } }) do
                if line:sub(1, #k[1]) == k[1] and line:sub(-1) == ":" then
                    kind = k[2]
                    found.headings[kind] = line
                end
            end
        elseif kind then
            local name = line:match("^  (%S+)")
            if name then found[kind][name] = true end
        end
    end
    return found
end

-- How many names a set from listed() holds.
local function count(set)
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    return n
end

fw.run(function()
    fw.log("=== C1: the repo as committed matches both records ===")
    local code, output = tool.run_tool()
    tool.expect_exit(code, output, 0, "C1 the repo")
    fw.assert_true(has(output, "test/save_format.txt"), "C1 the output names test/save_format.txt")
    fw.assert_true(has(output, "test/save_ids.txt"), "C1 the output names test/save_ids.txt")

    fw.log("=== R: what the record holds ===")
    local record = tool.read_file(tool.IDS_RECORD)
    fw.assert_true(record ~= nil, "R test/save_ids.txt exists")
    fw.assert_true(record ~= nil and record:sub(1, 1) == "#", "R the record starts with its header")
    local _, ids = record_lines(record)
    -- One of each kind, at the number its header gives it.
    for name, number in pairs({
        FLAG_TEMP_1 = 1,                        -- flags.h: TEMP_FLAGS_START + 0x01
        VAR_TEMP_0 = 0x4000,                    -- vars.h: TEMP_VARS_START + 0x0
        ITEM_POTION = 13,                       -- items.h
        SPECIES_BULBASAUR = 1,                  -- species.h
        NATIONAL_DEX_BULBASAUR = 1,             -- pokedex.h: an enum, after NONE
        MOVE_POUND = 1,                         -- moves.h
        TRAINER_AQUA_LEADER = 1,                -- opponents.h
        OBJ_EVENT_GFX_RED_NORMAL = 0,           -- event_objects.h
        GAME_STAT_SAVED_GAME = 0,               -- game_stat.h
        MAP_PALLET_TOWN = 0 | (3 << 8),         -- map_groups.h (built)
        LAYOUT_PALLET_TOWN_PLAYERS_HOUSE_1F = 1, -- layouts.h (built)
        MAPSEC_PALLET_TOWN = 0x58,              -- region_map_sections.h (built)
        LOCALID_CELADON_FAT_MAN = 2,            -- map_event_ids.h (built)
    }) do
        fw.assert_eq(ids[name], number, "R the record gives " .. name)
    end
    for _, name in ipairs({ "FLAG_0x020", "VAR_0x402A", "ITEM_034",
                            "FLAG_WONDER_CARD_UNUSED_1", "MAPSEC_NONE", "MAPSEC_COUNT",
                            "MAP_GROUPS_COUNT", "NATIONAL_DEX_COUNT",
                            "ITEM_TO_BERRY" }) do
        fw.assert_eq(ids[name], nil, "R the record leaves out " .. name)
    end

    fw.log("=== C2: ITEM_POTION's number changed ===")
    local c2_tree = tool.scratch_tree()
    tool.edit_header(c2_tree, "constants/items.h", "#define ITEM_POTION 13\n",
        "#define ITEM_POTION 999\n", "C2")
    code, output = tool.run_tool(tool.on_tree(c2_tree, IDS))
    tool.expect_exit(code, output, 1, "C2 renumbered")
    fw.assert_true(not has(output, "save format"), "C2 given only --ids-record, the tool checks only the saved IDs")
    local c2 = listed(output)
    fw.assert_true(c2.changed.ITEM_POTION == true, "C2 ITEM_POTION is listed as changed")
    local heading = c2.headings.changed or ""
    fw.assert_true(has(heading, "a breaking change") and not has(heading, "not a breaking change"),
        "C2 the changed IDs' heading calls them a breaking change")
    fw.assert_eq(count(c2.gone) + count(c2.new), 0, "C2 nothing is listed as gone or new")
    fw.assert_true(has(output, "python3 test/tools/save_format.py --write"),
        "C2 the output names python3 test/tools/save_format.py --write")
    fw.assert_true(has(output, "CLAUDE.md") and has(output, "Breaking changes"),
        "C2 the output names CLAUDE.md's \"Breaking changes\"")

    fw.log("=== C3: MOVE_KARATE_CHOP's #define deleted ===")
    local c3_tree = tool.scratch_tree()
    -- Nothing else in include/ may name it, or deleting it would break the
    -- probe's build instead.
    local _, uses = tool.run("grep -rw MOVE_KARATE_CHOP " .. tool.quote(c3_tree .. "/include") .. " | wc -l")
    fw.assert_eq(tonumber(uses), 1, "C3 MOVE_KARATE_CHOP is named once in include/, by its #define")
    tool.edit_header(c3_tree, "constants/moves.h", "#define MOVE_KARATE_CHOP 2\n", "", "C3")
    code, output = tool.run_tool(tool.on_tree(c3_tree, IDS))
    tool.expect_exit(code, output, 1, "C3 removed")
    local c3 = listed(output)
    fw.assert_true(c3.gone.MOVE_KARATE_CHOP == true, "C3 MOVE_KARATE_CHOP is listed as gone")
    fw.assert_true(has(c3.headings.gone or "", "a breaking change"),
        "C3 the gone IDs' heading calls them a breaking change")
    fw.assert_eq(count(c3.changed) + count(c3.new), 0, "C3 nothing is listed as changed or new")

    fw.log("=== C4: #define ITEM_NEW_THING 376 added ===")
    local c4_tree = tool.scratch_tree()
    tool.edit_header(c4_tree, "constants/items.h", "#define ITEM_POKE_RADAR 375\n",
        "#define ITEM_POKE_RADAR 375\n#define ITEM_NEW_THING 376\n", "C4")
    code, output = tool.run_tool(tool.on_tree(c4_tree, IDS))
    tool.expect_exit(code, output, 1, "C4 new")
    local c4 = listed(output)
    fw.assert_true(c4.new.ITEM_NEW_THING == true, "C4 ITEM_NEW_THING is listed as new")
    fw.assert_true(has(c4.headings.new or "", "not a breaking change: run --write to add it"),
        "C4 the new IDs' heading says \"not a breaking change: run --write to add it\"")
    fw.assert_eq(count(c4.changed) + count(c4.gone), 0, "C4 nothing is listed as changed or gone")
    local c4_record = { ids = tool.scratch_record("save_ids.txt") }
    code, output = tool.run_tool("--write " .. tool.on_tree(c4_tree, c4_record))
    tool.expect_exit(code, output, 0, "C4 --write to a scratch ID record")
    fw.assert_true(not has(output, "save format"), "C4 given only --ids-record, --write writes only that record")
    code, output = tool.run_tool(tool.on_tree(c4_tree, c4_record))
    tool.expect_exit(code, output, 0, "C4 the tree against the record --write gave it")
    -- The scratch record is the repo's with lines added: every line of the
    -- repo's comes in it, in order, and the only other line is the new ID.
    local repo_lines = record_lines(record)
    local scratch_lines = record_lines(tool.read_file(c4_record.ids))
    local added, at = {}, 1
    for _, line in ipairs(scratch_lines) do
        if line == repo_lines[at] then
            at = at + 1
        else
            table.insert(added, line)
        end
    end
    fw.assert_eq(at, #repo_lines + 1, "C4 every line of test/save_ids.txt is in the scratch record, in order")
    fw.assert_eq(table.concat(added, "; "), "ITEM_NEW_THING 376", "C4 the only line added is ITEM_NEW_THING's")
    fw.assert_eq((tool.read_file(c4_record.ids) or ""):match("^(.-)\n[^#]"), record:match("^(.-)\n[^#]"),
        "C4 the scratch record has the same header")

    fw.log("=== C5: VAR_0x402A renamed VAR_NEW_THING (a placeholder's first use) ===")
    local c5_tree = tool.scratch_tree()
    tool.edit_header(c5_tree, "constants/vars.h", "#define VAR_0x402A ", "#define VAR_NEW_THING ", "C5")
    code, output = tool.run_tool(tool.on_tree(c5_tree, IDS))
    tool.expect_exit(code, output, 1, "C5 placeholder used")
    local c5 = listed(output)
    fw.assert_true(c5.new.VAR_NEW_THING == true, "C5 VAR_NEW_THING is listed as new")
    fw.assert_eq(count(c5.new), 1, "C5 one ID is listed as new")
    fw.assert_eq(count(c5.gone), 0, "C5 nothing is listed as gone")
    fw.assert_eq(count(c5.changed), 0, "C5 nothing is listed as changed")

    fw.log("=== C6: a tree without include/constants/map_groups.h ===")
    local c6_tree = tool.scratch_tree()
    local header = c6_tree .. "/include/constants/map_groups.h"
    fw.assert_true(os.remove(header) ~= nil, "C6 removed the copy's map_groups.h")
    code, output = tool.run_tool(tool.on_tree(c6_tree, IDS))
    tool.expect_exit(code, output, 2, "C6 no build")
    fw.assert_true(has(output, "include/constants/map_groups.h"),
        "C6 the output names include/constants/map_groups.h")
    fw.assert_true(has(output, "make firered"), "C6 the output says to build with make firered")

    fw.log("=== N: a map section added at the end of its list ===")
    local n_tree = tool.scratch_tree()
    tool.edit_header(n_tree, "constants/region_map_sections.h", "    MAPSEC_NONE,\n",
        "    MAPSEC_NEW_THING,\n    MAPSEC_NONE,\n", "N")
    code, output = tool.run_tool(tool.on_tree(n_tree, IDS))
    tool.expect_exit(code, output, 1, "N new map section")
    local n = listed(output)
    fw.assert_true(n.new.MAPSEC_NEW_THING == true, "N MAPSEC_NEW_THING is listed as new")
    fw.assert_eq(count(n.new), 1, "N one ID is listed as new")
    fw.assert_eq(count(n.changed) + count(n.gone), 0, "N nothing is listed as changed or gone")

    tool.remove_scratch()
    fw.finish()
end)
