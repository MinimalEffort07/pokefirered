-------------------------------------------------------------------------------
-- demo_slaveless_hm.lua
-- Demo recording for the slaveless HM obstacle feature (issue #5).
-- Shows: badge + HM01 in bag, empty party → slaveless prompt → YES → tree cut.
-- Starts from the fixture test_slaveless_hm_obstacle.lua saves. Each
-- test/run_test.sh run writes it to its own output directory (printed as
-- "Output:"), and test/record_test.sh gives the demo a different one, so pass
-- its path in SLAVELESS_HM_FIXTURE:
--   bash test/run_test.sh test/tests/test_slaveless_hm_obstacle.lua
--   SLAVELESS_HM_FIXTURE=/tmp/mgba-test.XXXXXX/pokefirered-slaveless-hm.ss \
--       bash test/record_test.sh test/tests/demo_slaveless_hm.lua
-- Without it the demo looks in its own output directory (fw.out_path).
-- A fixture only loads into the ROM build that saved it.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH = os.getenv("SLAVELESS_HM_FIXTURE")
if STATE_PATH == nil or STATE_PATH == "" then
    STATE_PATH = fw.out_path("pokefirered-slaveless-hm.ss")
end

local SB1_BAG_TMHM       = 0x048c
local SB2_ENCRYPTION_KEY  = 0xF20
local FLAG_BADGE02_GET    = 2081
local ITEM_HM01           = 339

local function set_flag(sb1, flag_idx)
    local byte_off = math.floor(flag_idx / 8)
    local bit_mask = 1 << (flag_idx % 8)
    local addr = sb1 + ADDR.SB1_FLAGS + byte_off
    emu:write8(addr, fw.read8(addr) | bit_mask)
end

local function inject_hm01(sb1)
    local sb2     = fw.read32(ADDR.gSaveBlock2Ptr)
    local enc_key = fw.read32(sb2 + SB2_ENCRYPTION_KEY)
    local slot0   = sb1 + SB1_BAG_TMHM
    emu:write16(slot0,     ITEM_HM01)
    emu:write16(slot0 + 2, (1 ~ enc_key) & 0xFFFF)
end

fw.run(function()
    -- Load fixture: player at tile(11,23), empty party, no items/badges.
    if not fw.try_load_state(STATE_PATH) then
        fw.log("ERROR: fixture not found at " .. STATE_PATH)
        fw.log("Run the test first: bash test/run_test.sh test/tests/test_slaveless_hm_obstacle.lua")
        fw.log("then set SLAVELESS_HM_FIXTURE to the .ss in its Output directory.")
        fw.finish()
        return
    end
    fw.wait_frames(60)

    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    set_flag(sb1, FLAG_BADGE02_GET)
    inject_hm01(sb1)
    fw.wait_frames(10)

    -- Slow down so the demo is watchable.
    fw.slow_down("showing slaveless HM prompt")

    -- Face south toward the CUT tree at tile(11,24).
    fw.hold("DOWN", 8)
    fw.wait_frames(80)

    -- Press A to interact with the tree (triggers obstacle YESNO).
    fw.press("A")
    fw.wait_frames(360)   -- wait for text to finish scrolling

    -- Confirm YES → "mysterious POKéMON appeared" message shows.
    fw.press("A")
    fw.wait_frames(300)   -- wait for appeared text to scroll

    -- Dismiss appeared message → silhouette animation plays.
    fw.press("A")
    fw.wait_frames(900)   -- wait for silhouette + cut animation + tree removal

    fw.finish()
end)
