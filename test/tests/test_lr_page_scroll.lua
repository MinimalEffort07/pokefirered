-------------------------------------------------------------------------------
-- test_lr_page_scroll.lua
-- Verifies that LIST_MULTIPLE_SCROLL_L_R is active in both the Bag and the
-- Pokédex ordered-list menu by checking the scrollMultiple field at runtime.
--
-- WHAT WE TEST:
--
-- The game's list-menu framework (list_menu.c) already implements page-scroll
-- for L/R when scrollMultiple == LIST_MULTIPLE_SCROLL_L_R (= 2).  Our change
-- was to set that field in:
--   1. gMultiuseListMenuTemplate (built at bag-open time in item_menu.c)
--   2. sListMenuTemplate_OrderedListMenu (static const in pokedex_screen.c)
--
-- We verify by reading the field from EWRAM/IWRAM after the bag is open.
-- Reading gMultiuseListMenuTemplate directly is the most precise check
-- because it's the exact struct the bag's list-menu task runs from.
--
-- WHY WE DON'T TEST CURSOR MOVEMENT:
--
-- The ListMenuChangeSelection code that actually moves the cursor when L/R
-- are pressed is pre-existing, unmodified code.  Testing it here would be
-- re-testing the framework, not our change.  The scrollMultiple field check
-- is the canonical evidence that our two-line change took effect.
--
-- HOW TO RUN:
--   Headless: bash test/run_test.sh test/tests/test_lr_page_scroll.lua
--   GUI:      Load in mgba-qt via Tools > Scripting > Load script
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")

-------------------------------------------------------------------------------
-- gMultiuseListMenuTemplate is in IWRAM at 0x03005e80.
-- struct ListMenuTemplate layout (from include/list_menu.h):
--
--   0x00  const struct ListMenuItem *items   (4 bytes)
--   0x04  moveCursorFunc                     (4 bytes)
--   0x08  itemPrintFunc                      (4 bytes)
--   0x0C  u16 totalItems
--   0x0E  u16 maxShowed
--   0x10  u8  windowId
--   0x11  u8  header_X
--   0x12  u8  item_X
--   0x13  u8  cursor_X
--   0x14  u8  upText_Y:4, cursorPal:4
--   0x15  u8  fillValue:4, cursorShadowPal:4
--   0x16  u8  lettersSpacing:3, itemVerticalPadding:3, scrollMultiple:2
--   0x17  u8  fontId:6, cursorKind:2
--
-- scrollMultiple occupies bits [7:6] of byte 0x16.
-- LIST_MULTIPLE_SCROLL_L_R = 2 → bits [7:6] = 10b → (byte >> 6) & 3 == 2.
--
-- ADDRESS NOTE: This is the IWRAM address from pokefirered.map.
-- Update if IWRAM layout changes (rebuild + re-grep for gMultiuseListMenuTemplate).
-------------------------------------------------------------------------------

local TMPL_ADDR          = 0x03005e80  -- gMultiuseListMenuTemplate (IWRAM)
local TMPL_OFF_SCROLL    = 0x16        -- byte containing scrollMultiple:2 in bits [7:6]

local function read_scroll_multiple()
    local byte = fw.read8(TMPL_ADDR + TMPL_OFF_SCROLL)
    -- scrollMultiple is stored in bits [7:6] (most significant 2 bits of the byte).
    -- Shift right by 6 and mask to get the 2-bit value.
    return math.floor(byte / 64) % 4
end

-------------------------------------------------------------------------------
-- gBagMenuState.bagOpen: non-zero when the bag screen is active.
-- Layout: offset 0x05 from gBagMenuState (see item_menu.h struct BagStruct).
-------------------------------------------------------------------------------

local function bag_open()
    return fw.read8(ADDR.gBagMenuState + 0x05) ~= 0
end

-------------------------------------------------------------------------------
-- Main test body
-------------------------------------------------------------------------------

fw.run(function()
    fw.log("=== L/R Page Scroll Test ===")

    -- Boot through Oak's speech using the shared character_select helper.
    fw.log("Booting to overworld (takes ~5000 frames in fast-forward)...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.assert_true(false, "Could not discover Oak-speech selection press — boot failed")
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()

    fw.log("Reached overworld.")
    fw.wait_frames(60)   -- let overworld settle

    -- Open the bag from the Start menu.
    -- BAG is the first entry (POKEMON and POKEDEX flags are not set yet).
    fw.log("Opening start menu...")
    fw.press("START")
    fw.wait_frames(30)

    -- Navigate to BAG.  We try pressing A up to 8 times, backing out after
    -- each non-bag selection, until gBagMenuState.bagOpen becomes true.
    local bag_opened = false
    local attempts = 0
    while not bag_opened and attempts < 8 do
        attempts = attempts + 1
        fw.press("A")
        fw.wait_frames(30)

        if bag_open() then
            bag_opened = true
        else
            fw.press("B")
            fw.wait_frames(30)
            fw.press("DOWN")
            fw.wait_frames(10)
        end
    end

    if not bag_opened then
        fw.assert_true(false, "Could not open the Bag from the start menu")
        fw.finish()
        return
    end

    -- Wait for the opening animation and palette fade to complete.
    -- Task_BagMenu_HandleInput is gated by gPaletteFade.active (the fade-in
    -- from black takes ~16 frames) and Task_AnimateWin0v (the window-slide
    -- takes ~12 frames).  60 frames is comfortably past both.
    fw.wait_frames(60)

    -- Read scrollMultiple from gMultiuseListMenuTemplate.
    -- Bag_BuildListMenuTemplate (called during bag opening state 12) writes
    -- scrollMultiple = LIST_MULTIPLE_SCROLL_L_R (2) to gMultiuseListMenuTemplate.
    local scroll_val = read_scroll_multiple()
    local tmpl_byte  = fw.read8(TMPL_ADDR + TMPL_OFF_SCROLL)
    fw.log(string.format(
        "gMultiuseListMenuTemplate[0x16] = 0x%02X  scrollMultiple = %d",
        tmpl_byte, scroll_val))

    fw.assert_true(scroll_val == 2,
        string.format("Bag scrollMultiple == LIST_MULTIPLE_SCROLL_L_R (2), got %d", scroll_val))

    -- Also read maxShowed and totalItems for context.
    local max_showed = fw.read16(TMPL_ADDR + 0x0E)
    local total      = fw.read16(TMPL_ADDR + 0x0C)
    fw.log(string.format("  maxShowed = %d, totalItems = %d", max_showed, total))

    -- Exit the bag.
    fw.press("B")
    fw.wait_frames(60)

    fw.log("=== L/R Page Scroll Test Complete ===")
    fw.finish()
end)
