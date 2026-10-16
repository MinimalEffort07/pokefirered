-------------------------------------------------------------------------------
-- create_route14_battle_fixture.lua
-------------------------------------------------------------------------------
-- Programmatically creates /tmp/pokefirered-route14-double.ss:
-- a save state captured inside the Kiri & Jan double battle on Route 14
-- with the move selection screen open, player battler injected with
-- Water Gun (slot 0) and Vine Whip (slot 1).
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tools/create_route14_battle_fixture.lua
--
-- WHAT IT DOES:
--   1. Boots from reset through Oak's speech and character select into
--      Pallet Town (clean overworld state — CB1_Overworld + CB2_Overworld
--      already live, no stale SS1 callback corruption).
--   2. Injects 2 dummy Pikachu (fresh new game has 0 Pokemon; double
--      battle requires ≥2 non-fainted non-egg Pokemon in the party).
--   3. Clears Kiri's trainer defeat flag for robustness (new game already
--      has it cleared, but explicit to be safe).
--   4. Uses warp.warp_to_pos() to move to Route 14 at (12, 52) — one tile
--      inside Kiri's sight range (Kiri at y=51 facing DOWN, sight=1).
--   5. Waits for CB2_Overworld; mashes A through trainer approach + intro.
--   6. Verifies BATTLE_TYPE_DOUBLE flag is set.
--   7. Waits for battle intro and move selection screen.
--   8. Injects MOVE_WATER_GUN (55) and MOVE_VINE_WHIP (22) into
--      gPlayerParty[0] (with correct checksum) BEFORE the battle, so the
--      server populates gBattleBufferA correctly when it sends CONTROLLER_CHOOSE_MOVE.
--   9. Uses gMultiUsePlayerCursor==0xFF sentinel (set by InitMoveSelectionsVarsAndStrings)
--      to reliably detect move selection is open for battler 0.
--  10. Saves state to /tmp/pokefirered-route14-double.ss.
--
-- REQUIREMENTS:
--   No pre-existing save state needed. The full boot sequence is
--   automated via test/lib/character_select.lua (~6000 frames for
--   Oak's speech, plus ~240 frames for the warp).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")

local FIXTURE_PATH = "/tmp/pokefirered-route14-double.ss"

local ROUTE14_GROUP = 3
local ROUTE14_NUM   = 32
local WARP_X        = 12
local WARP_Y        = 52  -- Kiri at y=51 facing DOWN, sight=1 → sees y=52

local MOVE_WATER_GUN = 55
local MOVE_VINE_WHIP = 22

local PARTY_BASE   = 0x02024284
local POKEMON_SIZE = 100

local G_ACTIVE_BATTLER = 0x02023bc4  -- gActiveBattler (u8 EWRAM)

local BATTLE_TYPE_TRAINER = 0x08
local BATTLE_TYPE_DOUBLE  = 0x01

local function log_party_slot(label, slot)
    local base = PARTY_BASE + slot * POKEMON_SIZE
    local pid      = fw.read32(base + 0x00)
    local otid     = fw.read32(base + 0x04)
    local flags    = fw.read8(base + 0x13)
    local checksum = fw.read16(base + 0x1C)
    local species  = fw.read16(base + 0x20)
    local hp_cur   = fw.read16(base + 0x56)
    local hp_max   = fw.read16(base + 0x58)
    local computed = 0
    for w = 0, 23 do
        computed = (computed + fw.read16(base + 0x20 + w * 2)) % 65536
    end
    fw.log(string.format(
        "[diag] %s slot%d: PID=0x%08X OTID=0x%08X flags=0x%02X species=%d checksum=%d(computed=%d) HP=%d/%d",
        label, slot, pid, otid, flags, species, checksum, computed, hp_cur, hp_max))
end

local function inject_dummy_mon(slot)
    local base = PARTY_BASE + slot * POKEMON_SIZE
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)  -- flags: valid, not egg
    emu:write8(base + 0x55, 0xFF)  -- level = 255
    emu:write8(base + 0x20, 25)    -- species = Pikachu (low byte)
    emu:write8(base + 0x21, 0)     -- species (high byte)
    emu:write8(base + 0x1C, 25)    -- checksum (low) = species low
    emu:write8(base + 0x1D, 0)     -- checksum (high)
    emu:write8(base + 0x54, 5)     -- level (party field)
    emu:write8(base + 0x56, 20)    -- HP current
    emu:write8(base + 0x58, 20)    -- HP max
end

-- Write two moves (+PP) into the Attacker substruct of a party slot and
-- recompute the checksum.  Must be called AFTER inject_dummy_mon() has zeroed
-- the slot so only the explicitly-written fields are non-zero.
--
-- Substruct layout (PID=0 → encryption key=0 → plaintext; PID%24=0 → GAEM order):
--   Growth  @ base+0x20  (species, heldItem, exp, ppBonuses/friendship, pad)
--   Attacker@ base+0x2C  (moves[0..3], pp[0..3])
-- Checksum = sum of all 24 u16 words at base+0x20..base+0x4E (the 4 substructs).
local function inject_party_moves(slot, m0, pp0, m1, pp1)
    local base = PARTY_BASE + slot * POKEMON_SIZE
    emu:write16(base + 0x2C, m0)   -- moves[0]
    emu:write16(base + 0x2E, m1)   -- moves[1]
    emu:write8( base + 0x34, pp0)  -- pp[0]
    emu:write8( base + 0x35, pp1)  -- pp[1]
    local cksum = 0
    for w = 0, 23 do
        cksum = (cksum + fw.read16(base + 0x20 + w * 2)) % 65536
    end
    emu:write16(base + 0x1C, cksum)
    fw.log(string.format("[1] slot%d moves: %d(%dPP) / %d(%dPP)  checksum=0x%04X",
        slot, m0, pp0, m1, pp1, cksum))
end

fw.run(function()
    fw.log("=== create_route14_battle_fixture.lua ===")

    -- ----------------------------------------------------------------
    -- 1. Boot from reset → overworld → inject party → warp to Route 14.
    --    Starting from emu:reset() guarantees CB1_Overworld + CB2_Overworld
    --    are live before we trigger the warp, so warp.warp_to_pos() can
    --    dispatch CB2_LoadMap without any stale SS1 callback interference.
    -- ----------------------------------------------------------------
    fw.log("[1] Discovering character-select timing (first boot)...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log_error("[1] Could not discover character list opening press; aborting")
        fw.assert_true(false, "character select discovery succeeded")
        fw.finish()
        return
    end
    fw.log(string.format("[1] List opens at press %d. Booting to overworld...", sel))

    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.wait_frames(60)

    fw.assert_true(cs.is_player_active(), "player is active in overworld after boot")
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    fw.log(string.format("[1] In overworld. sb1=0x%08X cb1=0x%08X cb2=0x%08X",
        sb1, fw.read32(0x03003100), fw.read32(0x03003104)))

    -- Inject 2 dummy Pikachu. A fresh new game has 0 Pokemon in the
    -- party; the double battle engine requires ≥2 non-fainted Pokemon
    -- (GetMonsStateToDoubles checks HP > 0 and not-egg).
    fw.log("[1] Injecting 2 dummy Pikachu (party was empty after new game)...")
    inject_dummy_mon(0)
    inject_party_moves(0, MOVE_WATER_GUN, 25, MOVE_VINE_WHIP, 25)
    inject_dummy_mon(1)
    emu:write8(ADDR.gPlayerPartyCount, 2)
    log_party_slot("post-inject", 0)
    log_party_slot("post-inject", 1)
    fw.log(string.format("[1] Party count = %d", fw.read8(ADDR.gPlayerPartyCount)))

    -- Clear Kiri's trainer defeat flag so her ObjectEvent spawns on
    -- Route 14 and her battle can trigger.
    -- flag = TRAINER_FLAGS_START(0x500) + TRAINER_TWINS_KIRI_JAN_ID(487) = 0x6E7
    -- byte offset = 0xDC in flags[], bit = 7.
    local TRAINER_FLAGS_START       = 0x500
    local TRAINER_TWINS_KIRI_JAN_ID = 487
    local kiri_flag_idx  = TRAINER_FLAGS_START + TRAINER_TWINS_KIRI_JAN_ID
    local flag_byte_off  = math.floor(kiri_flag_idx / 8)
    local flag_bit       = kiri_flag_idx % 8
    local flag_addr      = sb1 + ADDR.SB1_FLAGS + flag_byte_off
    local old_byte       = fw.read8(flag_addr)
    local bit_val        = 1
    for _ = 1, flag_bit do bit_val = bit_val * 2 end
    local new_byte = old_byte
    if math.floor(old_byte / bit_val) % 2 == 1 then
        new_byte = old_byte - bit_val
    end
    emu:write8(flag_addr, new_byte)
    fw.log(string.format("[1] Kiri flag 0x%03X: @0x%08X bit%d 0x%02X→0x%02X",
        kiri_flag_idx, flag_addr, flag_bit, old_byte, new_byte))

    -- Warp to Route 14 at (12, 52). Kiri is at y=51 facing DOWN with
    -- trainer_sight=1, so she detects the player at y=52.
    --
    -- warp_to_pos() writes sWarpDestination + SB1->location.x/y but does
    -- NOT write SB1->pos.x/y.  The map-load chain (CB2_LoadMap direct call)
    -- bypasses WarpIntoMap/SetPlayerCoordsFromWarp, so InitObjectEventsLocal
    -- reads pos.x/y directly to determine the player spawn.
    -- GetCameraFocusCoords() returns (pos.x+7, pos.y+7) = the full-map tile
    -- coord where the player ObjectEvent is placed.
    -- For player at full-map (19,59) [= Kiri's sight target]:
    --   pos.x = 19-7 = 12 = WARP_X
    --   pos.y = 59-7 = 52 = WARP_Y
    -- We write these before the warp so CB2_LoadMap uses them.
    local pre_sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    emu:write16(pre_sb1 + 0x00, WARP_X)  -- pos.x = WARP_X (camera x = tile_x - 7)
    emu:write16(pre_sb1 + 0x02, WARP_Y)  -- pos.y = WARP_Y (camera y = tile_y - 7)
    fw.log(string.format("[1] Warping to Route 14 (%d.%d) at (%d,%d) [pos pre-set]...",
        ROUTE14_GROUP, ROUTE14_NUM, WARP_X, WARP_Y))
    local warp_ok = warp.warp_to_pos(ROUTE14_GROUP, ROUTE14_NUM, WARP_X, WARP_Y)
    if not warp_ok then
        fw.log_error("[1] warp.warp_to_pos failed — Route 14 did not load")
        fw.assert_true(false, "warp to Route 14 succeeded")
        fw.finish()
        return
    end
    local post_sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    fw.log(string.format("[1] Warp done. pos=(%d,%d) cb2=0x%08X party=%d",
        fw.read16(post_sb1 + 0x00), fw.read16(post_sb1 + 0x02),
        fw.read32(0x03003104), fw.read8(ADDR.gPlayerPartyCount)))

    -- ----------------------------------------------------------------
    -- 2. Wait for CB2_Overworld, then mash A through trainer intro
    -- ----------------------------------------------------------------
    -- CB2_LoadMap chains through CB2_DoChangeMap → CB2_LoadMap2 → CB2_Overworld
    -- before the overworld input loop runs.  CB1_Overworld checks
    -- callback2 == CB2_Overworld+thumb (0x0805697d) and returns early until
    -- that is set — so CheckForTrainersWantingBattle never fires until then.
    local CB2_OVERWORLD = 0x0805697d  -- CB2_Overworld + ARM thumb bit
    fw.log("[2] Waiting for CB2_Overworld (0x0805697d) to be set...")
    local cb2_ok = fw.wait_until(
        function() return fw.read32(0x03003104) == CB2_OVERWORLD end,
        600, "CB2_Overworld set")
    if not cb2_ok then
        local cb2v = fw.read32(0x03003104)
        fw.log_error(string.format("[2] CB2_Overworld not set after 600 frames (cb2=0x%08X)", cb2v))
        fw.assert_true(false, "CB2_Overworld set after warp")
        fw.finish()
        return
    end
    local post_cb2 = fw.read32(0x03003104)
    local post_sb1_2 = fw.read32(ADDR.gSaveBlock1Ptr)
    fw.log(string.format("[2] Overworld running: cb2=0x%08X pos=(%d,%d) party=%d",
        post_cb2,
        fw.read16(post_sb1_2 + 0x00), fw.read16(post_sb1_2 + 0x02),
        fw.read8(ADDR.gPlayerPartyCount)))

    -- Player is at (12, 52); Kiri is at y=51 facing DOWN (sight range=1).
    -- Once the warp-exit task releases sLockFieldControls (~60 frames),
    -- CheckForTrainersWantingBattle fires → Kiri walks to player → pre-battle
    -- dialogue plays.  Mash A ONLY until gBattleTypeFlags becomes non-zero
    -- (the battle transition has started) — then stop.  If we keep mashing
    -- after the battle starts we blow past the action menu and into move/target
    -- selection before the sentinel in step 4 can detect it.
    -- Mash A until the DOUBLE flag is confirmed (0x01 appears in gBattleTypeFlags).
    -- The TRAINER bit (0x08) appears ~4 presses in; the DOUBLE bit follows ~6
    -- more presses later as GetMonsStateToDoubles() runs.  We MUST stop as soon
    -- as double is confirmed — continuing past this point risks overshooting the
    -- action menu into move/target selection before step 4's sentinel can fire.
    fw.log("[2] Mashing A until double battle confirmed...")
    for i = 1, 40 do
        fw.press("A")
        fw.wait_frames(30)
        local btf = fw.read32(ADDR.gBattleTypeFlags)
        if (math.floor(btf / BATTLE_TYPE_DOUBLE) % 2) == 1 then
            fw.log(string.format("[2] Double confirmed at iter %d: btf=0x%08X — stopping mash", i, btf))
            break
        end
        if i <= 5 or i % 5 == 0 then
            fw.log(string.format("[2] iter %d: btf=0x%08X", i, btf))
        end
    end
    fw.wait_frames(60)  -- let battle-start tasks settle before step 3 reads flags

    -- ----------------------------------------------------------------
    -- 3. Verify double battle started
    -- ----------------------------------------------------------------
    local flags = fw.read32(ADDR.gBattleTypeFlags)
    fw.log(string.format("[3] gBattleTypeFlags = 0x%08X", flags))
    local party_count = fw.read8(ADDR.gPlayerPartyCount)
    fw.log(string.format("[3] gPlayerPartyCount = %d", party_count))
    log_party_slot("post-battle-start", 0)
    log_party_slot("post-battle-start", 1)

    local is_trainer = (math.floor(flags / BATTLE_TYPE_TRAINER) % 2) == 1
    local is_double  = (math.floor(flags / BATTLE_TYPE_DOUBLE)  % 2) == 1
    fw.log(string.format("[3] is_trainer=%s is_double=%s", tostring(is_trainer), tostring(is_double)))

    if not is_trainer then
        fw.log_error("[3] Trainer battle did not start!")
        fw.log_error("Check: player at y=" .. WARP_Y .. ", Kiri at y=51 facing DOWN (sight=1).")
        fw.assert_true(false, "trainer battle started")
        fw.finish()
        return
    end
    if not is_double then
        fw.log_error("[3] Battle started but NOT as double (missing BATTLE_TYPE_DOUBLE=0x01)!")
        fw.log_error("Check: party count >=2, trainer doubleBattle flag, GetMonsStateToDoubles().")
        fw.assert_true(false, "Kiri/Jan double battle started")
        fw.finish()
        return
    end
    fw.log("[3] Double trainer battle confirmed (Kiri & Jan).")

    -- ----------------------------------------------------------------
    -- 4. Advance through battle intro → action menu → move selection.
    --    InitMoveSelectionsVarsAndStrings sets gMultiUsePlayerCursor = 0xFF
    --    the instant move selection opens (before any cursor movement).
    --    Check BEFORE each A-press so we stop at move selection rather than
    --    overshooting into target selection.
    -- ----------------------------------------------------------------
    fw.log("[4] Waiting for move selection (gMultiUsePlayerCursor==0xFF)...")
    local got_move_selection = false
    for i = 1, 120 do
        local cursor = fw.read8(ADDR.gMultiUsePlayerCursor)
        local active = fw.read8(G_ACTIVE_BATTLER)
        fw.log(string.format("[4] iter %d: cursor=0x%02X active=%d btf=0x%08X",
            i, cursor, active, fw.read32(ADDR.gBattleTypeFlags)))
        if cursor == 0xFF then
            got_move_selection = true
            fw.log(string.format("[4] Move selection open at iteration %d (active=%d)", i, active))
            break
        end
        fw.press("A")
        fw.wait_frames(30)
    end
    if not got_move_selection then
        fw.log_error("[4] Move selection screen never opened after 120 A-presses!")
        fw.log_error("Check: battle started, party has valid moves.")
        fw.assert_true(false, "move selection screen opened")
        fw.finish()
        return
    end

    -- ----------------------------------------------------------------
    -- 5. Save fixture
    -- ----------------------------------------------------------------
    fw.screenshot("/tmp/mgba-fixture-presave.png")
    fw.log("[5] Saving fixture to " .. FIXTURE_PATH)
    local SAVESTATE_ALL = 31
    local ok = emu:saveStateFile(FIXTURE_PATH, SAVESTATE_ALL)
    if ok then
        fw.log("[5] SUCCESS: fixture saved to " .. FIXTURE_PATH)
        fw.log("[5] Run test_type_effectiveness_colors.lua to verify.")
    else
        fw.log_error("[5] emu:saveStateFile failed — check disk space and path.")
        fw.assert_true(false, "fixture saved successfully")
    end

    fw.log("=== create_route14_battle_fixture.lua complete ===")
    fw.finish()
end)
