-------------------------------------------------------------------------------
-- test_pp_items_in_marts.lua
-------------------------------------------------------------------------------
-- Verifies that PP-restoring items appear in the correct mart item lists
-- after the "PP Items in Marts" feature (issue #9) is applied.
--
-- APPROACH — pure ROM reads:
-- Mart item lists are static .2byte arrays baked into ROM at link time.
-- They are always accessible at their ROM address (0x08xxxxxx) regardless
-- of game state. We read them directly without navigating to any mart.
-- This makes the test fast and deterministic.
--
-- Sub-tests:
--   A — Cerulean City Mart (Super Potion tier): Ether + Max Ether after Super Potion
--   B — Cinnabar Island Mart (Hyper Potion tier): Elixir after Hyper Potion
--   C — Four Island Mart (Max Potion tier): Max Elixir after Max Potion
--   D — Seven Island Mart (dual tier): Elixir after Hyper Potion AND
--                                       Max Elixir after Max Potion
--
-- HOW TO RUN:
--   Headless:  bash test/run_test.sh test/tests/test_pp_items_in_marts.lua
--   GUI:       Load in mgba-qt via Tools > Scripting > Load script
--
-- UPDATING ADDRESSES:
--   If you rebuild the ROM after changing mart scripts, re-extract addresses:
--     grep "Mart_Items" pokefirered.map
--   Then update ADDR.CeruleanCity_Mart_Items etc. in test/lib/addresses.lua.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

-- Read all item IDs from a ROM item list, stopping at ITEM_NONE (0).
-- rom_addr is the GBA ROM address (0x08xxxxxx).
-- Returns a Lua array of item ID numbers.
local function read_mart_items(rom_addr)
    local items = {}
    local offset = 0
    while true do
        local id = fw.read16(rom_addr + offset)
        if id == 0 then break end
        table.insert(items, id)
        offset = offset + 2
    end
    return items
end

-- Assert that `new_id` immediately follows `anchor_id` in `items`.
-- `label` is the human-readable sub-test name used in assert messages.
local function assert_item_after(items, anchor_id, new_id, label)
    for i = 1, #items do
        if items[i] == anchor_id then
            fw.assert_eq(items[i + 1], new_id,
                label .. ": item " .. new_id .. " should follow anchor " .. anchor_id)
            return
        end
    end
    -- Anchor not found at all — the list is malformed or the wrong address.
    fw.assert_true(false, label .. ": anchor item " .. anchor_id .. " not found in list")
end

fw.run(function()
    fw.log("=== Test: PP Items in Marts (issue #9) ===")

    -- ROM is memory-mapped at 0x08000000+ and readable immediately;
    -- no need to wait for the game to boot or navigate anywhere.

    -- ----------------------------------------------------------------
    -- Sub-test A: Cerulean City Mart — Super Potion tier
    -- Expected: ETHER (34) and MAX_ETHER (35) appear immediately after
    --           SUPER_POTION (22) in that order.
    -- ----------------------------------------------------------------
    fw.log("Sub-test A: Cerulean City Mart (Super Potion tier)")
    local cerulean_items = read_mart_items(ADDR.CeruleanCity_Mart_Items)
    fw.log("  Items: " .. table.concat(cerulean_items, ", "))
    assert_item_after(cerulean_items, ADDR.ITEM_SUPER_POTION, ADDR.ITEM_ETHER,     "A1 Ether after Super Potion")
    assert_item_after(cerulean_items, ADDR.ITEM_ETHER,        ADDR.ITEM_MAX_ETHER, "A2 Max Ether after Ether")

    -- ----------------------------------------------------------------
    -- Sub-test B: Cinnabar Island Mart — Hyper Potion tier
    -- Expected: ELIXIR (36) appears immediately after HYPER_POTION (21).
    -- ----------------------------------------------------------------
    fw.log("Sub-test B: Cinnabar Island Mart (Hyper Potion tier)")
    local cinnabar_items = read_mart_items(ADDR.CinnabarIsland_Mart_Items)
    fw.log("  Items: " .. table.concat(cinnabar_items, ", "))
    assert_item_after(cinnabar_items, ADDR.ITEM_HYPER_POTION, ADDR.ITEM_ELIXIR, "B Elixir after Hyper Potion")

    -- ----------------------------------------------------------------
    -- Sub-test C: Four Island Mart — Max Potion tier
    -- Expected: MAX_ELIXIR (37) appears immediately after MAX_POTION (20).
    -- ----------------------------------------------------------------
    fw.log("Sub-test C: Four Island Mart (Max Potion tier)")
    local four_items = read_mart_items(ADDR.FourIsland_Mart_Items)
    fw.log("  Items: " .. table.concat(four_items, ", "))
    assert_item_after(four_items, ADDR.ITEM_MAX_POTION, ADDR.ITEM_MAX_ELIXIR, "C Max Elixir after Max Potion")

    -- ----------------------------------------------------------------
    -- Sub-test D: Seven Island Mart — dual tier
    -- Expected: ELIXIR after HYPER_POTION, and MAX_ELIXIR after MAX_POTION.
    -- ----------------------------------------------------------------
    fw.log("Sub-test D: Seven Island Mart (dual tier)")
    local seven_items = read_mart_items(ADDR.SevenIsland_Mart_Items)
    fw.log("  Items: " .. table.concat(seven_items, ", "))
    assert_item_after(seven_items, ADDR.ITEM_HYPER_POTION, ADDR.ITEM_ELIXIR,    "D1 Elixir after Hyper Potion")
    assert_item_after(seven_items, ADDR.ITEM_MAX_POTION,   ADDR.ITEM_MAX_ELIXIR, "D2 Max Elixir after Max Potion")

    fw.log("=== All sub-tests complete ===")
    fw.finish()
end)
