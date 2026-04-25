-------------------------------------------------------------------------------
-- demo_lr_page_scroll.lua
-- Captures screenshots of the Bag menu demonstrating L/R page scrolling.
-- Run headless; outputs frames to /tmp/lr_demo_frames/.
--
--   bash test/run_test.sh test/tests/demo_lr_page_scroll.lua
-- Then assemble:
--   ffmpeg -framerate 8 -pattern_type glob -i '/tmp/lr_demo_frames/*.png' \
--     -vf "scale=480:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
--     ~/recordings/lr_page_scroll.gif
--
-- NOTE: This script boots from a fresh ROM reset through Oak's speech using
-- the character-select sentinel trick (same approach as test_lr_page_scroll.lua).
-- The pokefirered.ss1 save state boots into a multiplayer-waiting state that
-- blocks the START menu, so we bypass it entirely. After reaching the overworld,
-- we programmatically fill the bag with items before opening it, so there are
-- enough entries for L/R page scrolling to be visible.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")

local FRAME_DIR = "/tmp/lr_demo_frames"
os.execute("mkdir -p " .. FRAME_DIR .. " && rm -f " .. FRAME_DIR .. "/*.png")

local frame_idx = 0
local function snap()
    frame_idx = frame_idx + 1
    fw.screenshot(string.format("%s/frame_%04d.png", FRAME_DIR, frame_idx))
end

-------------------------------------------------------------------------------
-- BAG menu helpers (from test_lr_page_scroll.lua)
-------------------------------------------------------------------------------

-- gBagMenuState + 0x05 is bool8 bagOpen. Non-zero while the bag is active.
local BAG_OFF_BAG_OPEN = 0x05
local function bag_open()
    return fw.read8(ADDR.gBagMenuState + BAG_OFF_BAG_OPEN) ~= 0
end

-- gTasks scanner: find Task_BagMenu_HandleInput in the task table.
-- Address is ROM-build-specific; cross-check with pokefirered.map.
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
-- Item injection helpers
--
-- SaveBlock1 layout (relevant fields):
--   +0x0338: struct ItemSlot bagPocket_Items[BAG_ITEMS_COUNT]  (42 slots)
--
-- struct ItemSlot { u16 itemId; u16 quantity; } — 4 bytes each.
--
-- Item quantities are XOR-encrypted with the low 16 bits of
-- SaveBlock2.encryptionKey (at offset 0xF20 within SaveBlock2).
-- We zero the key before writing so raw_qty == displayed_qty.
--
-- optionsButtonMode (SaveBlock2+0x013):
--   OPTIONS_BUTTON_MODE_HELP (0) — L/R open the in-game HELP overlay.
--   OPTIONS_BUTTON_MODE_LR   (1) — L/R are passed to menus (page scroll etc.)
-- A fresh NEW GAME defaults to HELP mode, which intercepts L/R before the
-- bag's list menu sees them. We switch to LR mode so the page-scroll feature
-- is reachable from the demo.
--
-- Item IDs from include/constants/items.h:
--   13 = POTION, 14 = ANTIDOTE, 22 = SUPER POTION, 23 = BURN HEAL, ...
-------------------------------------------------------------------------------

-- bagPocket_Items offset within SaveBlock1.
-- global.h annotates this as 0x0310, but the actual compiled offset for this
-- build is 0x0338 (verified at runtime: gBagPockets[0].itemSlots - gSaveBlock1Ptr
-- = 0x02025864 - 0x0202552C = 0x0338). The extra 0x28 bytes come from struct
-- alignment and any build-specific padding before bagPocket_Items.
local SB1_BAG_ITEMS_OFFSET = 0x0338   -- bagPocket_Items within SaveBlock1 (verified)
local SB2_ENCRYPTION_KEY   = 0xF20    -- encryptionKey within SaveBlock2
local SB2_OPTIONS_BTN_MODE = 0x013    -- optionsButtonMode within SaveBlock2
local OPTIONS_BUTTON_MODE_LR = 1      -- pass L/R to menus (not HELP system)

-- Fill the Items pocket with a variety of items so there are enough rows
-- for L/R page scrolling (need more than ~6 items to show a full page jump).
-- We write 20 items, which is well over the displayed page (6 rows).
local DEMO_ITEMS = {
    { id = 13, qty = 5  },  -- POTION
    { id = 14, qty = 3  },  -- ANTIDOTE
    { id = 22, qty = 5  },  -- SUPER POTION
    { id = 23, qty = 2  },  -- BURN HEAL
    { id = 24, qty = 2  },  -- ICE HEAL
    { id = 25, qty = 2  },  -- AWAKENING
    { id = 26, qty = 2  },  -- PARLYZ HEAL
    { id = 27, qty = 1  },  -- FULL RESTORE
    { id = 28, qty = 3  },  -- MAX POTION
    { id = 29, qty = 1  },  -- HYPER POTION
    { id = 30, qty = 2  },  -- FULL HEAL
    { id = 31, qty = 1  },  -- REVIVE
    { id = 32, qty = 1  },  -- MAX REVIVE
    { id = 33, qty = 3  },  -- FRESH WATER
    { id = 34, qty = 2  },  -- SODA POP
    { id = 35, qty = 1  },  -- LEMONADE
    { id = 38, qty = 1  },  -- X ATTACK
    { id = 39, qty = 1  },  -- X DEFEND
    { id = 40, qty = 1  },  -- X SPEED
    { id = 41, qty = 1  },  -- X SPECIAL
}

local function inject_bag_items()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local sb2 = fw.read32(ADDR.gSaveBlock2Ptr)

    -- Switch button mode from HELP (0) to LR (1) so that L/R presses are
    -- routed to the bag list menu for page scrolling rather than intercepted
    -- by RunHelpSystemCallback(). This matches the in-game Options setting.
    emu:write8(sb2 + SB2_OPTIONS_BTN_MODE, OPTIONS_BUTTON_MODE_LR)

    -- Zero out the encryption key so that all quantities are stored and
    -- read back in plaintext (XOR with 0 is identity). This is safe for the
    -- demo: the key is regenerated on the next game save anyway.
    -- We must zero the FULL 32-bit key because GetBagItemQuantity XORs the
    -- full u32 encryptionKey with the stored u16 quantity.
    emu:write32(sb2 + SB2_ENCRYPTION_KEY, 0)

    for i, item in ipairs(DEMO_ITEMS) do
        local slot_addr = sb1 + SB1_BAG_ITEMS_OFFSET + (i - 1) * 4
        emu:write16(slot_addr + 0, item.id)   -- itemId (plaintext)
        emu:write16(slot_addr + 2, item.qty)  -- quantity (plaintext, enc_key=0)
    end
    -- Slot after the last item must have itemId=0 to terminate the list.
    -- (BAG_ITEMS_COUNT = 42; we wrote 20, slot 20 is the terminator.)
    local terminator_addr = sb1 + SB1_BAG_ITEMS_OFFSET + #DEMO_ITEMS * 4
    emu:write16(terminator_addr, 0)
    emu:write16(terminator_addr + 2, 0)

    fw.log(string.format("[items] Wrote %d items; optionsButtonMode=LR; enc_key=0",
        #DEMO_ITEMS))
end

-------------------------------------------------------------------------------
-- Main demo body
-------------------------------------------------------------------------------

fw.run(function()
    fw.log("=== L/R Page Scroll Demo ===")

    -- Boot through Oak's speech using the shared character-select helper.
    -- We use the sentinel trick to find which A-press opens the character list,
    -- then boot cleanly to the overworld. This bypasses the multiplayer-waiting
    -- state that the pokefirered.ss1 save file boots into.
    fw.log("Booting to overworld via character-select...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log("Could not discover Oak-speech selection press -- aborting")
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.log("Reached overworld.")
    fw.wait_frames(60)

    -- Inject items into the bag so the list is long enough to page-scroll.
    -- A fresh NEW GAME has an empty bag; without items, L/R opens the HELP
    -- screen instead of scrolling (the list engine ignores L/R on empty lists).
    -- We inject AFTER the overworld is fully settled (any auto-save triggered
    -- by NEW GAME completion has finished regenerating the encryption key).
    -- Reading the key here (not during boot) avoids stale-key mismatch.
    fw.wait_frames(120)  -- let any pending save / key regeneration complete
    inject_bag_items()
    fw.wait_frames(10)   -- let the write settle (no DMA involved, just RAM)

    -----------------------------------------------------------------------
    -- Open the bag from the Start menu.
    -- From test_lr_page_scroll.lua: try iterating A until the bag opens,
    -- backing off with B if something else opens instead.
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
        fw.log("WARNING: Could not open the Bag from the start menu after 8 attempts")
        fw.finish()
        return
    end

    -- Wait for Task_BagMenu_HandleInput to become active (bag fully loaded).
    local settled = false
    for _ = 1, 15 do
        if bag_input_handler_active() then
            settled = true
            break
        end
        fw.press("B")
        fw.wait_frames(20)
    end
    settled = settled or bag_input_handler_active()

    if not settled then
        fw.log("WARNING: Could not reach Task_BagMenu_HandleInput after opening bag")
        fw.finish()
        return
    end

    fw.log("Bag input handler is active. Settling...")
    fw.wait_frames(90)  -- let palette fade and window animations finish

    -----------------------------------------------------------------------
    -- Capture: initial Bag state (top of items list) — ~2 s at 8 fps output
    -----------------------------------------------------------------------
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- Press R to page down — cursor jumps a full page (6 rows)
    fw.press("R"); fw.wait_frames(20)
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    -- Press R again — scroll another page
    fw.press("R"); fw.wait_frames(20)
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    -- Press L to page back up
    fw.press("L"); fw.wait_frames(20)
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    -- Press L again — back to top
    fw.press("L"); fw.wait_frames(20)
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    -- Hold at top to show it clamps (no wrap)
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    fw.log(string.format("Captured %d frames to %s", frame_idx, FRAME_DIR))
    fw.finish()
end)
