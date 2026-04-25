-------------------------------------------------------------------------------
-- test_pc_anywhere.lua
--
-- Tests: MOVE TO PC in party menu deposits a mon (party count decreases).
--
-- What this test does:
--   1. Loads a save state (pokefirered.ss1 in the project root) that has the
--      player standing in the overworld with at least 2 party Pokemon.
--   2. Opens the Start menu -> POKEMON party screen.
--   3. Opens the action menu for the first party slot.
--   4. Navigates to MOVE TO PC and selects it.
--   5. Verifies gPlayerPartyCount decreased by 1.
--
-- SKIP condition: if the save state is missing or the party has only 1 mon,
-- the test skips with a message rather than failing.
--
-- Run:
--   bash test/run_test.sh test/tests/test_pc_anywhere.lua
--
-- Record:
--   bash test/record_test.sh test/tests/test_pc_anywhere.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local results = {}
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.log((cond and "[PASS] " or "[FAIL] ") .. name)
end

fw.run(function()
    fw.log("=== PC Anywhere Test: MOVE TO PC in party menu ===")

    local state_path = project_dir .. "pokefirered.ss1"
    fw.try_load_state(state_path)
    fw.wait_frames(60)

    local party_count_before = fw.read8(ADDR.gPlayerPartyCount)
    fw.log(string.format("Party count before: %d", party_count_before))

    if party_count_before < 2 then
        fw.log("SKIP: Need at least 2 party mons to test deposit. " ..
               "Save state has only " .. party_count_before ..
               " (or no save state loaded).")
        fw.finish()
        return
    end

    -- Open Start menu
    fw.press("START")
    fw.wait_frames(40)

    -- Select POKEMON (first entry, already highlighted)
    fw.press("A")
    fw.wait_frames(60)

    -- Party menu is open. First slot is highlighted. Press A to open action menu.
    fw.press("A")
    fw.wait_frames(40)

    -- Navigate DOWN repeatedly to reach MOVE TO PC.
    -- The overworld party action menu order for a normal mon is:
    --   SUMMARY, [WALK/RETURN if eligible], [FIELD MOVES if any],
    --   [SWITCH if party > 1], [MAIL or ITEM], MOVE TO PC, CANCEL
    -- Pressing DOWN 8 times from SUMMARY will overshoot to CANCEL.
    -- Then UP once lands on MOVE TO PC (second-to-last entry).
    for _ = 1, 8 do
        fw.press("DOWN")
        fw.wait_frames(12)
    end

    -- Move back up one to land on MOVE TO PC (just before CANCEL)
    fw.press("UP")
    fw.wait_frames(12)

    -- Confirm MOVE TO PC
    fw.press("A")
    fw.wait_frames(120)

    local party_count_after = fw.read8(ADDR.gPlayerPartyCount)
    fw.log(string.format("Party count after: %d", party_count_after))

    check("Party count decreased by 1 after MOVE TO PC",
          party_count_after == party_count_before - 1)

    -- Exit menus with B presses
    fw.press("B")
    fw.wait_frames(30)
    fw.press("B")
    fw.wait_frames(30)

    local passed = 0
    for _, r in ipairs(results) do
        if r.pass then passed = passed + 1 end
    end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
