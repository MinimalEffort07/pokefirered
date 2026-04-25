-------------------------------------------------------------------------------
-- demo_pc_anywhere.lua
-- Demonstrates PC access from the field:
--   1. SELECT → Quick Select menu → BILL'S PC → PC storage opens
--   2. Party menu → MOVE TO PC option visible
--
-- Boots fresh through Oak's speech (same as demo_lr_page_scroll) to avoid
-- the multiplayer-waiting state in pokefirered.ss1 that blocks field input.
--
-- Run:
--   bash test/run_test.sh test/tests/demo_pc_anywhere.lua
-- Assemble GIF:
--   ffmpeg -framerate 8 -pattern_type glob -i '/tmp/pc_demo_frames/*.png' \
--     -vf "scale=480:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
--     ~/recordings/pc_anywhere.gif
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

local FRAME_DIR = "/tmp/pc_demo_frames"
os.execute("mkdir -p " .. FRAME_DIR .. " && rm -f " .. FRAME_DIR .. "/*.png")

local frame_idx = 0
local function snap()
    frame_idx = frame_idx + 1
    fw.screenshot(string.format("%s/frame_%04d.png", FRAME_DIR, frame_idx))
end

fw.run(function()
    fw.log("=== PC Anywhere Demo ===")

    -- Boot through Oak's speech; avoids the multiplayer-waiting state that
    -- the pokefirered.ss1 file loads into (which blocks all field input).
    fw.log("Booting to overworld via character-select...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log("ERROR: Could not discover Oak-speech selection press -- aborting")
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.log("Reached overworld.")
    fw.wait_frames(120)

    -----------------------------------------------------------------------
    -- Part 1: BILL'S PC via SELECT → Quick Select menu
    -- Player is in bedroom / Pallet Town. SELECT opens the quick-select
    -- list which always includes BILL'S PC at the bottom.
    -----------------------------------------------------------------------
    fw.log("Opening Quick Select with SELECT...")

    -- Show player in overworld first
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- Press SELECT to open Quick Select menu
    fw.press("SELECT")
    fw.wait_frames(60)

    -- Capture menu open (BILL'S PC is the only entry for a fresh game)
    for _ = 1, 32 do snap(); fw.wait_frames(4) end

    -- Press A to open BILL'S PC storage system
    fw.press("A")
    fw.wait_frames(120)

    -- Capture the PC storage screen
    for _ = 1, 40 do snap(); fw.wait_frames(4) end

    -- Press B to exit PC
    fw.press("B")
    fw.wait_frames(60)
    fw.press("B")
    fw.wait_frames(60)

    -----------------------------------------------------------------------
    -- Part 2: Show MOVE TO PC option in the party menu
    -- (Only visible if player has a Pokémon. In a fresh game the player
    -- starts with no party so we show the Quick Select approach only.)
    -----------------------------------------------------------------------

    -- Return to overworld, hold briefly to show back in field
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    fw.log(string.format("Captured %d frames to %s", frame_idx, FRAME_DIR))
    fw.finish()
end)
