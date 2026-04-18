-------------------------------------------------------------------------------
-- test_playable_characters.lua
-------------------------------------------------------------------------------
-- Runs the full boot → character-select → overworld → walk → verify cycle
-- for each entry in CHARACTERS below, using the shared helpers in
-- test/lib/character_select.lua.
--
-- What we verify per character:
--   1. playerAvatarGfxId in SaveBlock2 matches the expected OBJ_EVENT_GFX_*.
--   2. The player's ObjectEvent in the overworld shows the right sprite.
--   3. The player can actually move (at least one direction changes pos).
--   4. The sprite does not flip to a different gfx id after walking.
--
-- HOW TO RUN:
--   Headless:  bash test/run_test.sh test/tests/test_playable_characters.lua
--   GUI:       Tools > Scripting > Load script
--
-- Each full per-character cycle costs ~10k emulated frames. The sentinel
-- discovery inside character_select runs once and is reused for every
-- subsequent character.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

-- List of characters to test. Picked to span the full list:
--   - indices 0-1: the two default protagonists (Red/Green)
--   - indices 2-3: the R/S link avatars (Brendan/May)
--   - mid-list generic NPCs
--   - notable story NPCs (Oak, a gym leader, Mom)
--   - Pokemon-sprite avatars at various positions
--   - the last entry (Kabuto) to catch off-by-one boundary issues
--
-- { list_index, expected_gfx_id, display_name }
local CHARACTERS = {
    {   0,   0, "RED"        },
    {   1,   7, "GREEN"      },
    {   2,  14, "BRENDAN"    },
    {  13,  26, "ROCKER"     },
    {  50,  64, "NURSE"      },
    {  57,  71, "PROF_OAK"   },
    {  66,  80, "BROCK"      },
    {  74,  88, "MOM"        },
    {  78, 120, "PIKACHU"    },
    {  94, 126, "SEEL"       },
    { 103, 135, "LAPRAS"     },
    { 104, 147, "KABUTO"     },
}

fw.run(function()
    fw.log("=== Playable Characters Test ===")
    fw.log(string.format("Testing %d characters", #CHARACTERS))

    local all_results = {}
    for _, char in ipairs(CHARACTERS) do
        local r = cs.test_character(char[1], char[2], char[3])
        table.insert(all_results, r)
    end

    -- Summary: surface every failure clearly so the runner's output is
    -- actionable without having to re-read the full log.
    fw.log("=== Summary ===")
    local failed = {}
    for _, r in ipairs(all_results) do
        local pos = (r.dx or r.dy) and string.format(" (moved dx=%s dy=%s)",
            tostring(r.dx), tostring(r.dy)) or ""
        if r.passed then
            fw.log(string.format("  PASS %s%s", r.name, pos))
        else
            fw.log_error(string.format("  FAIL %s: %s",
                r.name, table.concat(r.reasons, "; ")))
            table.insert(failed, r.name)
        end
    end

    -- One assertion per character so the pass/fail count in the JSON
    -- reflects reality.
    for _, r in ipairs(all_results) do
        fw.assert_true(r.passed,
            string.format("character %s works", r.name))
    end

    if #failed > 0 then
        fw.log_error(string.format("FAILED: %s", table.concat(failed, ", ")))
    end

    fw.finish()
end)
