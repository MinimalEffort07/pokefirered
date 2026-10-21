-------------------------------------------------------------------------------
-- demo_slaveless_hm.lua
-- Demo recording for the slaveless HM obstacle feature (issue #5).
-- Shows, with an empty party (so nobody knows the moves):
--   1. CUT: BADGE02 + HM01 in the bag, A at the Viridian City tree → prompt →
--      YES → "A mysterious POKéMON appeared and used CUT!" → the grey
--      silhouette on the field-move banner → the tree is cut down.
--   2. SURF: BADGE05 + HM03 in the bag, walk to the Viridian pond, A at the
--      water → "Would you like to SURF?" → YES → silhouette → the player
--      surfs onto the pond.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua): the rest fast-forwards. For each of CUT and
-- SURF, the prompt with its YES/NO box (the last second before YES), then
-- the silhouette on the banner and what the move does: the tree cut down,
-- the player surfing onto the pond. The moments play frames the demo waits
-- anyway, so the GIF's frames don't change.
--
-- Starts from the fixture test_slaveless_hm_obstacle.lua saves. Each
-- test/run_test.sh run writes it to its own output directory (printed as
-- "Output:"), so pass its path in SLAVELESS_HM_FIXTURE. A fixture only loads
-- into the ROM build that saved it.
--
-- RECORD THE GIF (frame dumps, see test/lib/frame_capture.lua: every 2nd
-- frame of the 60 fps game, so real time at 30 fps):
--   bash test/run_test.sh test/tests/test_slaveless_hm_obstacle.lua
--   SLAVELESS_HM_FIXTURE=/tmp/mgba-test.XXXXXX/pokefirered-slaveless-hm.ss \
--   MGBA_FRAME_DIR=/tmp/slaveless_hm_frames MGBA_TEST_TIMEOUT=200 \
--       bash test/run_test.sh test/tests/demo_slaveless_hm.lua
--   ffmpeg -framerate 30 -i /tmp/slaveless_hm_frames/frame_%05d.png -vf \
--       "fps=15,split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=none" \
--       gifs/slaveless_hm.gif
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw      = dofile(project_dir .. "test/lib/framework.lua")
fw.allow_no_assertions("demo: captures frames for the slaveless HM GIF")
local ADDR    = dofile(project_dir .. "test/lib/addresses.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

local STATE_PATH = os.getenv("SLAVELESS_HM_FIXTURE")
if STATE_PATH == nil or STATE_PATH == "" then
    STATE_PATH = fw.out_path("pokefirered-slaveless-hm.ss")
end

-- Same layout facts as test_slaveless_hm_obstacle.lua (see the notes there).
local SB1_BAG_TMHM       = 0x048c  -- offsetof(SaveBlock1, bagPocket_TMHM)
local SB2_ENCRYPTION_KEY = 0xF20   -- offsetof(SaveBlock2, encryptionKey)
local FLAG_BADGE02_GET   = 2081
local FLAG_BADGE05_GET   = 2084
local ITEM_HM01          = 339
local ITEM_HM03          = 341

local function set_flag(sb1, flag_idx)
    local byte_off = math.floor(flag_idx / 8)
    local bit_mask = 1 << (flag_idx % 8)
    local addr = sb1 + ADDR.SB1_FLAGS + byte_off
    emu:write8(addr, fw.read8(addr) | bit_mask)
end

-- Put item_id with quantity 1 in TM/HM pocket slot `slot` (4 bytes each:
-- u16 itemId, u16 quantity XOR-ed with the save's encryption key).
local function inject_item(sb1, slot, item_id)
    local sb2     = fw.read32(ADDR.gSaveBlock2Ptr)
    local enc_key = fw.read32(sb2 + SB2_ENCRYPTION_KEY)
    local addr    = sb1 + SB1_BAG_TMHM + slot * 4
    emu:write16(addr,     item_id)
    emu:write16(addr + 2, (1 ~ enc_key) & 0xFFFF)
end

local function read_pos()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X), fw.read16(oa + ADDR.OE_CURRENT_Y)
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
    set_flag(sb1, FLAG_BADGE05_GET)
    inject_item(sb1, 0, ITEM_HM01)
    inject_item(sb1, 1, ITEM_HM03)
    fw.wait_frames(10)

    capture.start()

    -- 1. CUT: face south toward the tree at tile (11,24).
    capture.caption("CUT: no party POKEMON knows CUT; HM01 + CASCADE BADGE")
    fw.hold("DOWN", 8)
    fw.wait_frames(40)
    fw.press("A")          -- tree → "Use CUT?" YES/NO
    fw.wait_frames(270)    -- the YES/NO box is up by about 240 frames
    fw.show_next("CUT: \"Use CUT?\" YES/NO, with no party POKéMON that knows CUT")
    fw.wait_frames(60)
    fw.press("A")          -- YES → "A mysterious POKéMON appeared and used CUT!"
    fw.wait_frames(260)
    fw.press("A")          -- dismiss → silhouette on the banner, tree cut
    -- The banner slides in about 28 frames after A; the tree is cut down
    -- about 145 frames after that.
    fw.wait_frames(28)
    fw.show_next("CUT: the grey silhouette on the banner, then the tree is cut down", 150)
    fw.wait_frames(252)

    -- 2. SURF: walk to the pond edge at tile (15,25) and face the water.
    capture.caption("SURF: no party POKEMON knows SURF; HM03 + SOUL BADGE")
    fw.hold_until("RIGHT", function() local x, _ = read_pos(); return x >= 22 end, 200, 4)
    fw.wait_frames(16)
    fw.hold_until("DOWN", function() local _, y = read_pos(); return y >= 32 end, 200, 4)
    fw.wait_frames(16)
    fw.hold("DOWN", 4)
    fw.wait_frames(30)
    fw.press("A")          -- water → "Would you like to SURF?" YES/NO
    fw.wait_frames(270)    -- the YES/NO box is up by about 240 frames
    fw.show_next("SURF: \"Would you like to SURF?\" YES/NO, with no party POKéMON that knows SURF")
    fw.wait_frames(60)
    fw.press("A")          -- YES → "A mysterious POKéMON appeared and used SURF!"
    fw.wait_frames(260)
    fw.press("A")          -- dismiss → silhouette on the banner, surf onto the pond
    -- The banner slides in about 29 frames after A; the player has jumped
    -- onto the pond about 115 frames after that.
    fw.wait_frames(29)
    fw.show_next("SURF: the grey silhouette on the banner, then the player surfs onto the pond", 150)
    fw.wait_frames(171)

    capture.stop()
    fw.finish()
end)
