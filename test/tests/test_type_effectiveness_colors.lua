-------------------------------------------------------------------------------
-- test_type_effectiveness_colors.lua
-------------------------------------------------------------------------------
-- Verifies that MoveSelectionSetEffectivenessColors correctly reflects the
-- alive opponent in a double battle (issue #7).
--
-- *** ONE-TIME MANUAL SETUP REQUIRED ***
--
-- This test requires a save state captured inside the Kiri & Jan double
-- battle on Route 14 (east of Fuchsia City, two sisters with Charmander +
-- Squirtle, both level 29). The state must be captured with the MOVE
-- SELECTION screen open on TURN 1 (both opponents alive).
--
-- Your lead Pokemon MUST have:
--   Move slot 0 — any Water-type move  (e.g., Water Gun, Surf)
--   Move slot 1 — any Grass-type move  (e.g., Absorb, Vine Whip)
--   Move slot 0's cursor must be selected (default)
--
-- HOW TO CREATE THE FIXTURE (do this once):
--   1. Open the ROM in mgba-qt:
--        mgba-qt pokefirered.gba
--   2. Set up a party lead with Water (slot 0) and Grass (slot 1) moves.
--   3. Walk to Route 14 and trigger the Kiri & Jan double battle.
--   4. When the MOVE SELECTION screen appears for YOUR FIRST TURN,
--      open the scripting console and run:
--        Tools > Scripting > File > Open >
--          test/tools/save_route14_battle_state.lua
--      That script saves the emulator state to
--        /tmp/pokefirered-route14-double.ss
--   5. Re-run this test; it will load the state and execute.
--
-- WHAT THIS TESTS:
--   Sub-test A — Both opponents alive (default):
--     Colors reflect left opponent (Charmander / Fire type).
--     Water = SUPER (green), Grass = NOT_VERY (amber).
--
--   Sub-test C — Real-time cursor update during target selection:
--     Press DPAD_LEFT to move cursor from Charmander to Squirtle.
--     Colors update immediately: Water = NOT_VERY, Grass = SUPER.
--
--   Sub-test B — Left opponent absent (injected via memory write):
--     Inject gAbsentBattlerFlags = 2 (battler 1 absent), then navigate
--     FIGHT menu -> re-open move selection. InitMoveSelectionsVarsAndStrings
--     must fall back to the right opponent (Squirtle / Water type).
--     Water = NOT_VERY, Grass = SUPER.
--
-- HOW TO RUN:
--   Headless: bash test/run_test.sh test/tests/test_type_effectiveness_colors.lua
--   GUI:      Load in mgba-qt via Tools > Scripting > Load script
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH = "/tmp/pokefirered-route14-double.ss"

-- Read the GBA palette text-foreground color for move slot i (0=first move).
local function move_color(slot)
    return fw.read16(ADDR.move_eff_color_addr(slot))
end

-- Read type1 from a BattlePokemon slot.
local function battler_type1(battler_id)
    return fw.read8(ADDR.gBattleMons + battler_id * ADDR.BATTLEMON_SIZE + ADDR.BP_TYPE1)
end

local function battler_type2(battler_id)
    return fw.read8(ADDR.gBattleMons + battler_id * ADDR.BATTLEMON_SIZE + ADDR.BP_TYPE2)
end

fw.run(function()
    fw.log("=== Test: Type Effectiveness Colors in Double Battle (issue #7) ===")

    -- This test is visual — run at real-time so a human watching the GUI
    -- can see the move menu colors changing as the cursor moves.
    fw.slow_down("visual battle test — runs at real-time")
    fw.wait_frames(10)

    -- ----------------------------------------------------------------
    -- Load fixture
    -- ----------------------------------------------------------------
    if not fw.try_load_state(STATE_PATH) then
        fw.log_error("===")
        fw.log_error("FIXTURE MISSING: " .. STATE_PATH)
        fw.log_error("This test requires a one-time manual setup.")
        fw.log_error("See the comment block at the top of this file for instructions.")
        fw.log_error("Short version:")
        fw.log_error("  1. Set up lead with Water move (slot 0) + Grass move (slot 1)")
        fw.log_error("  2. Trigger Kiri & Jan double battle on Route 14")
        fw.log_error("  3. When move selection opens (turn 1), run:")
        fw.log_error("       test/tools/save_route14_battle_state.lua")
        fw.log_error("===")
        fw.assert_true(false, "fixture exists at " .. STATE_PATH)
        fw.finish()
        return
    end

    fw.wait_frames(30)  -- let renderer settle after state load

    -- ----------------------------------------------------------------
    -- Sanity-check: confirm we are facing Fire-type and Water-type opponents
    -- ----------------------------------------------------------------
    local left_t1  = battler_type1(ADDR.BATTLER_OPPONENT_LEFT)
    local right_t1 = battler_type1(ADDR.BATTLER_OPPONENT_RIGHT)
    fw.log(string.format("Battler %d (left opp): type1=%d | Battler %d (right opp): type1=%d",
        ADDR.BATTLER_OPPONENT_LEFT,  left_t1,
        ADDR.BATTLER_OPPONENT_RIGHT, right_t1))
    fw.assert_eq(left_t1,  ADDR.TYPE_FIRE,  "left opponent is Fire-type (Charmander)")
    fw.assert_eq(right_t1, ADDR.TYPE_WATER, "right opponent is Water-type (Squirtle)")

    -- ----------------------------------------------------------------
    -- Sub-test A: both opponents alive — colors target left (Fire) opp
    -- InitMoveSelectionsVarsAndStrings set these colors when the state was
    -- saved. Both battlers are alive, so B_POSITION_OPPONENT_LEFT (battler 1)
    -- is used. Water vs Fire = SUPER, Grass vs Fire = NOT_VERY.
    -- ----------------------------------------------------------------
    fw.log("--- Sub-test A: both alive — colors reflect left opponent (Fire) ---")
    local a0 = move_color(0)
    local a1 = move_color(1)
    fw.log(string.format("  slot 0 = 0x%04X (want SUPER    = 0x%04X)", a0, ADDR.EFF_COLOR_SUPER))
    fw.log(string.format("  slot 1 = 0x%04X (want NOT_VERY = 0x%04X)", a1, ADDR.EFF_COLOR_NOT_VERY))
    fw.assert_eq(a0, ADDR.EFF_COLOR_SUPER,    "A: Water move = super-effective vs Fire (slot 0)")
    fw.assert_eq(a1, ADDR.EFF_COLOR_NOT_VERY, "A: Grass move = not-very-effective vs Fire (slot 1)")

    fw.screenshot("/tmp/mgba-type-eff-sub-a.png")

    -- ----------------------------------------------------------------
    -- Sub-test C: real-time color update when cursor moves targets
    -- Press A on move slot 0 to enter target selection. Cursor starts on
    -- battler 1 (Charmander). DPAD_LEFT moves to battler 3 (Squirtle) in
    -- the sTargetIdentities ring. HandleInputChooseTarget must call
    -- MoveSelectionSetEffectivenessColors(battler_3) immediately.
    -- ----------------------------------------------------------------
    fw.log("--- Sub-test C: DPAD_LEFT moves cursor to right opponent, colors update live ---")

    fw.press("A")       -- select move slot 0 → enter target selection
    fw.wait_frames(30)  -- wait for target-selection setup (cursor on battler 1)

    local cursor_before = fw.read8(ADDR.gMultiUsePlayerCursor)
    fw.log(string.format("  cursor before DPAD: battler %d (expect %d = left opp)",
        cursor_before, ADDR.BATTLER_OPPONENT_LEFT))
    fw.assert_eq(cursor_before, ADDR.BATTLER_OPPONENT_LEFT,
        "C: cursor starts on left opponent (battler 1)")

    -- DPAD_LEFT decrements the sTargetIdentities ring:
    --   OPPONENT_LEFT (index 3) -> OPPONENT_RIGHT (index 2) = battler 3
    fw.press("LEFT")
    fw.wait_frames(5)   -- allow HandleInputChooseTarget to update palette

    local cursor_after = fw.read8(ADDR.gMultiUsePlayerCursor)
    fw.log(string.format("  cursor after DPAD: battler %d (expect %d = right opp)",
        cursor_after, ADDR.BATTLER_OPPONENT_RIGHT))
    fw.assert_eq(cursor_after, ADDR.BATTLER_OPPONENT_RIGHT,
        "C: cursor moved to right opponent (battler 3)")

    local c0 = move_color(0)
    local c1 = move_color(1)
    fw.log(string.format("  slot 0 = 0x%04X (want NOT_VERY = 0x%04X)", c0, ADDR.EFF_COLOR_NOT_VERY))
    fw.log(string.format("  slot 1 = 0x%04X (want SUPER    = 0x%04X)", c1, ADDR.EFF_COLOR_SUPER))
    fw.assert_eq(c0, ADDR.EFF_COLOR_NOT_VERY, "C: Water move = not-very-effective vs Water (slot 0)")
    fw.assert_eq(c1, ADDR.EFF_COLOR_SUPER,    "C: Grass move = super-effective vs Water (slot 1)")

    fw.screenshot("/tmp/mgba-type-eff-sub-c.png")

    -- ----------------------------------------------------------------
    -- Sub-test B: absent-opponent fallback
    -- Navigate back to move selection, inject gAbsentBattlerFlags = 0x02
    -- (battler 1 absent), then exit and re-enter the FIGHT menu so that
    -- InitMoveSelectionsVarsAndStrings runs with the absent flag set.
    -- The fix must fall back to battler 3 (Squirtle / Water).
    -- ----------------------------------------------------------------
    fw.log("--- Sub-test B: left opponent absent, colors fall back to right opponent ---")

    -- Cancel target selection -> back to move selection
    fw.press("B")
    fw.wait_frames(20)

    -- Inject absent flag for battler 1 (bit 1 = 0x02)
    emu:write8(ADDR.gAbsentBattlerFlags, 0x02)
    fw.log("  Injected gAbsentBattlerFlags = 0x02 (battler 1 absent)")

    -- Exit move selection -> action menu, then re-enter FIGHT
    fw.press("B")       -- leave move selection (emits cancel, brings up action menu)
    fw.wait_frames(90)  -- generous wait for action menu to appear
    fw.press("A")       -- select FIGHT
    fw.wait_frames(120) -- wait for move selection to reopen and InitMoveSelectionsVarsAndStrings to run

    local b0 = move_color(0)
    local b1 = move_color(1)
    fw.log(string.format("  slot 0 = 0x%04X (want NOT_VERY = 0x%04X)", b0, ADDR.EFF_COLOR_NOT_VERY))
    fw.log(string.format("  slot 1 = 0x%04X (want SUPER    = 0x%04X)", b1, ADDR.EFF_COLOR_SUPER))
    fw.assert_eq(b0, ADDR.EFF_COLOR_NOT_VERY, "B: Water move = not-very-effective vs absent-left / Water-right (slot 0)")
    fw.assert_eq(b1, ADDR.EFF_COLOR_SUPER,    "B: Grass move = super-effective vs absent-left / Water-right (slot 1)")

    fw.screenshot("/tmp/mgba-type-eff-sub-b.png")

    fw.log("=== All sub-tests complete ===")
    fw.finish()
end)
