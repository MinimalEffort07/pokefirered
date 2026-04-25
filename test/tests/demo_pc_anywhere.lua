-------------------------------------------------------------------------------
-- demo_pc_anywhere.lua
-- Captures screenshots demonstrating BILL'S PC in the Quick Select menu.
-- Run headless; outputs frames to /tmp/pc_demo_frames/.
--
--   bash test/run_test.sh test/tests/demo_pc_anywhere.lua
-- Then assemble:
--   ffmpeg -framerate 8 -pattern_type glob -i '/tmp/pc_demo_frames/*.png' \
--     -vf "scale=480:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
--     ~/recordings/pc_anywhere.gif
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local FRAME_DIR = "/tmp/pc_demo_frames"
os.execute("mkdir -p " .. FRAME_DIR .. " && rm -f " .. FRAME_DIR .. "/*.png")

local frame_idx = 0
local function snap()
    frame_idx = frame_idx + 1
    fw.screenshot(string.format("%s/frame_%04d.png", FRAME_DIR, frame_idx))
end

fw.run(function()
    local state_path = project_dir .. "pokefirered.ss1"
    fw.try_load_state(state_path)
    fw.wait_frames(90)

    -- Capture player in overworld briefly
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    -- Press SELECT to open Quick Select menu
    fw.press("SELECT"); fw.wait_frames(60)

    -- Capture menu open showing entries including BILL'S PC
    for _ = 1, 32 do snap(); fw.wait_frames(4) end

    -- Scroll DOWN to show BILL'S PC is at the bottom
    -- Press DOWN several times to move cursor through list to BILL'S PC
    for _ = 1, 8 do
        fw.press("DOWN"); fw.wait_frames(12)
        for _ = 1, 4 do snap(); fw.wait_frames(4) end
    end

    -- Hold on BILL'S PC entry for a moment
    for _ = 1, 20 do snap(); fw.wait_frames(4) end

    -- Press A to open the PC
    fw.press("A"); fw.wait_frames(120)

    -- Capture PC storage screen
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    fw.log(string.format("Captured %d frames to %s", frame_idx, FRAME_DIR))
    fw.finish()
end)
