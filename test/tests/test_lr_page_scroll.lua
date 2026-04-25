-------------------------------------------------------------------------------
-- test_lr_page_scroll.lua
-- Verifies that the bag item list is configured for L/R page-scroll.
--
-- WHAT WE TEST (and why the config check is sufficient):
--
--   Our change sets scrollMultiple = LIST_MULTIPLE_SCROLL_L_R (= 2) in the
--   bag's ListMenuTemplate (in item_menu.c, Bag_BuildListMenuTemplate).
--
--   The full verification chain confirmed by source-code review:
--
--   1. ProcessPocketSwitchInput (item_menu.c) explicitly excludes L and R
--      from its input mask, so those buttons are NOT consumed there.
--
--   2. Task_BagMenu_HandleInput (item_menu.c) calls ListMenu_ProcessInput
--      (list_menu.c) with the template.  ListMenu_ProcessInput reaches
--      case LIST_MULTIPLE_SCROLL_L_R: and reads gMain.newAndRepeatedKeys
--      & {L_BUTTON, R_BUTTON} to drive page-scroll.
--
--   3. ListMenuChangeSelection is called with count=maxShowed (nominally 6
--      with 20+ items in the Items pocket), advancing or retreating the
--      cursor by a full page.
--
--   Behavioural (cursor-move) assertions are NOT included here because
--   they depend on item-quantity decryption (gSaveBlock2->encryptionKey XOR
--   raw_qty) that makes it difficult to produce a canonical non-empty list
--   in headless mGBA without a real save file.  The config check proves the
--   routing is wired; the list_menu engine is upstream code (pret/pokefirered)
--   and is not modified by this feature.
--
-- gBagMenuState layout (EWRAM, from include/item_menu.h struct BagStruct):
--   0x00  MainCallback bagCallback  (4 bytes)
--   0x04  u8  location
--   0x05  bool8 bagOpen             -- non-zero while bag screen is active
--   0x06  u16 pocket                -- index of the currently displayed pocket
--   0x08  u16 itemsAbove[3]         -- items scrolled above viewport, per pocket
--   0x0E  u16 cursorPos[3]          -- cursor row within viewport, per pocket
--
-- gMultiuseListMenuTemplate lives at 0x03005e80 in IWRAM; scrollMultiple is
-- packed into bits [7:6] of the byte at template offset 0x16.
-- LIST_MULTIPLE_SCROLL_L_R = 2  =>  bits[7:6] = 0b10  =>  byte value has
-- (2 << 6) = 0x80 contributed by scrollMultiple (other bits may hold
-- itemVerticalPadding).
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
-- gBagMenuState field offsets (relative to ADDR.gBagMenuState).
-------------------------------------------------------------------------------
local BAG_OFF_BAG_OPEN = 0x05  -- bool8 bagOpen

-- Return true when the bag screen is active.
local function bag_open()
    return fw.read8(ADDR.gBagMenuState + BAG_OFF_BAG_OPEN) ~= 0
end

-------------------------------------------------------------------------------
-- Task scanner constants.
--
-- gTasks is at 0x030050a0, 16 entries, 40 bytes each:
--   offset 0: u32 func pointer (stored as Thumb ptr with bit 0 set)
--   offset 4: u8 isActive
--
-- Task_BagMenu_HandleInput lives at 0x08109404 in this build; Thumb pointers
-- have bit 0 set, so gTasks stores 0x08109405.
--
-- NOTE: This address is ROM-build-specific. If the ROM is rebuilt with a
-- different code layout this constant must be updated. Cross-check with:
--   grep Task_BagMenu_HandleInput pokefirered.map
-------------------------------------------------------------------------------
local GTASKS_ADDR     = 0x030050a0
local TASK_SIZE_B     = 40
local BAG_INPUT_THUMB = 0x08109405  -- Task_BagMenu_HandleInput | 1 (Thumb bit)

local function bag_input_handler_active()
    for i = 0, 15 do
        local t_addr   = GTASKS_ADDR + i * TASK_SIZE_B
        local t_func   = fw.read32(t_addr)
        local t_active = fw.read8(t_addr + 4)
        if t_active ~= 0 and t_func == BAG_INPUT_THUMB then
            return true
        end
    end
    return false
end

-------------------------------------------------------------------------------
-- Main test body
-------------------------------------------------------------------------------
fw.run(function()
    fw.log("=== L/R Page Scroll Config Test ===")

    -- Boot through Oak's speech using the shared character_select helper.
    fw.log("Booting to overworld...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.assert_true(false, "Could not discover Oak-speech selection press -- boot failed")
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()

    fw.log("Reached overworld.")
    fw.wait_frames(60)

    -----------------------------------------------------------------------
    -- Open the bag from the Start menu.
    -----------------------------------------------------------------------
    fw.log("Opening start menu...")
    fw.press("START")
    fw.wait_frames(30)

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
        fw.assert_true(false, "Could not open the Bag from the start menu after 8 attempts")
        fw.finish()
        return
    end

    -- Wait for Task_BagMenu_HandleInput to become active (press B to dismiss
    -- any incidental sub-menus that may have opened along the way).
    local escaped = false
    for _attempt = 1, 15 do
        if bag_input_handler_active() then
            escaped = true
            break
        end
        fw.press("B")
        fw.wait_frames(20)
    end
    escaped = escaped or bag_input_handler_active()

    if not escaped then
        fw.assert_true(false, "Could not reach Task_BagMenu_HandleInput after opening bag")
        fw.finish()
        return
    end

    fw.log("Bag input handler is active. Settling...")
    fw.wait_frames(90)  -- let palette fade and window animations finish

    -----------------------------------------------------------------------
    -- CONFIG CHECK: gMultiuseListMenuTemplate.scrollMultiple == 2.
    --
    -- gMultiuseListMenuTemplate is the IWRAM template used by
    -- Bag_BuildListMenuTemplate (item_menu.c).  It lives at 0x03005e80.
    -- The scrollMultiple field occupies bits [7:6] of the byte at +0x16;
    -- the lower 6 bits are itemVerticalPadding.
    -- LIST_MULTIPLE_SCROLL_L_R = 2 means bits[7:6] = 0b10.
    --
    -- This proves the bag is configured to route L/R to ListMenu_ProcessInput
    -- as page-scroll keys.  ProcessPocketSwitchInput does NOT consume L/R
    -- (confirmed by its explicit input mask in item_menu.c).
    -----------------------------------------------------------------------
    local tmpl_byte  = fw.read8(0x03005e80 + 0x16)
    local scroll_val = math.floor(tmpl_byte / 64) % 4
    fw.log(string.format("[config] scrollMultiple=%d (expect 2)  tmpl_byte=0x%02X",
        scroll_val, tmpl_byte))

    fw.assert_true(scroll_val == 2,
        string.format("gMultiuseListMenuTemplate.scrollMultiple == LIST_MULTIPLE_SCROLL_L_R (2), got %d",
            scroll_val))

    -- Exit bag cleanly.
    fw.press("B")
    fw.wait_frames(60)

    fw.log("=== L/R Page Scroll Config Test Complete ===")
    fw.finish()
end)
