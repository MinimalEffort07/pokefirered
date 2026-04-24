-------------------------------------------------------------------------------
-- demo_pp_items_in_marts.lua
-- Captures screenshots of mart menus showing PP items for GIF assembly.
-- Run headless; outputs frames to /tmp/pp_demo_frames/.
--
--   bash test/run_test.sh test/tests/demo_pp_items_in_marts.lua
-- Then assemble:
--   ffmpeg -framerate 8 -pattern_type glob -i '/tmp/pp_demo_frames/*.png' \
--     -vf "scale=480:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
--     ~/recordings/pp_items_in_marts.gif
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local FRAME_DIR = "/tmp/pp_demo_frames"
os.execute("mkdir -p " .. FRAME_DIR .. " && rm -f " .. FRAME_DIR .. "/*.png")

local frame_idx = 0
local function snap()
    frame_idx = frame_idx + 1
    fw.screenshot(string.format("%s/frame_%04d.png", FRAME_DIR, frame_idx))
end

-- Both marts share this layout:
--   Clerk at map tile (2,3), facing right (counter at y=2, player approaches from y=4).
--   We spawn the player at (2,5) via warp_to_pos, walk UP one step to (2,4),
--   then press A facing up to trigger the mart menu.

local MAP_GROUP_CERULEAN_MART = 7
local MAP_NUM_CERULEAN_MART   = 7

local MAP_GROUP_CINNABAR_MART = 12
local MAP_NUM_CINNABAR_MART   = 7

-- Helper: open the mart menu from spawn position (2,5) and capture frames.
-- Returns with the menu dismissed. Caller must call fw.finish() when done.
-- OE_BORDER is the 7-tile constant FireRed adds to all map-tile coords.
-- Clerk is at map (2,3); place player at map (2,4) facing up.
local OE_BORDER   = 7
local CLERK_MAP_X = 2
local CLERK_MAP_Y = 3

local function open_mart_and_capture(map_group, map_num, scene_label)
    warp.warp_to_pos(map_group, map_num, 2, 5)

    -- Directly write player OE currentCoords/previousCoords to map (2,4).
    -- The warp engine resets OE from save state; we override it here.
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    local target_oe_x = CLERK_MAP_X + OE_BORDER   -- 9
    local target_oe_y = (CLERK_MAP_Y + 1) + OE_BORDER  -- 11  (one tile below clerk)
    emu:write16(oa + 0x0C, target_oe_x)  -- initialCoords.x
    emu:write16(oa + 0x0E, target_oe_y)  -- initialCoords.y
    emu:write16(oa + ADDR.OE_CURRENT_X,  target_oe_x)
    emu:write16(oa + ADDR.OE_CURRENT_Y,  target_oe_y)
    emu:write16(oa + 0x14, target_oe_x)  -- previousCoords.x
    emu:write16(oa + 0x16, target_oe_y)  -- previousCoords.y
    fw.wait_frames(30)  -- let sprite renderer pick up the new position

    local px = fw.read16(oa + ADDR.OE_CURRENT_X)
    local py = fw.read16(oa + ADDR.OE_CURRENT_Y)
    fw.log(string.format("%s: player OE x=%d y=%d (map %d,%d)",
        scene_label, px, py, px - OE_BORDER, py - OE_BORDER))

    -- Press UP to set facing direction (blocked by clerk/counter, no movement).
    fw.press("UP"); fw.wait_frames(30)

    -- A1: starts clerk script → "Hi, there! May I help you?" text appears.
    fw.press("A"); fw.wait_frames(120)
    -- A2: satisfies waitmessage → pokemart runs → BUY/SELL/SEE YA menu opens.
    fw.press("A"); fw.wait_frames(120)
    -- A3: selects BUY → item list with PP items opens.
    fw.press("A"); fw.wait_frames(120)

    -- Capture item list — should show PP items (~3.5 s at 8 fps output).
    for _ = 1, 28 do snap(); fw.wait_frames(4) end

    -- Scroll down to highlight the PP item (Ether / Elixir).
    fw.press("DOWN"); fw.wait_frames(20)
    for _ = 1, 20 do snap(); fw.wait_frames(4) end

    fw.press("DOWN"); fw.wait_frames(20)
    for _ = 1, 20 do snap(); fw.wait_frames(4) end

    -- Exit: B closes item list, B closes BUY/SELL menu, B closes clerk dialog.
    fw.press("B"); fw.wait_frames(40)
    fw.press("B"); fw.wait_frames(40)
    fw.press("B"); fw.wait_frames(40)
end

fw.run(function()
    local state_path = project_dir .. "pokefirered.ss1"
    fw.try_load_state(state_path)
    fw.wait_frames(60)

    -----------------------------------------------------------------------
    -- Scene 1: Cerulean City Mart — Ether + Max Ether after Super Potion
    -----------------------------------------------------------------------
    open_mart_and_capture(MAP_GROUP_CERULEAN_MART, MAP_NUM_CERULEAN_MART, "Cerulean")

    -----------------------------------------------------------------------
    -- Scene 2: Cinnabar Island Mart — Elixir after Hyper Potion
    -----------------------------------------------------------------------
    open_mart_and_capture(MAP_GROUP_CINNABAR_MART, MAP_NUM_CINNABAR_MART, "Cinnabar")

    fw.log(string.format("Captured %d frames to %s", frame_idx, FRAME_DIR))
    fw.finish()
end)
