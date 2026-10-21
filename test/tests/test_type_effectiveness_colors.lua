-------------------------------------------------------------------------------
-- test_type_effectiveness_colors.lua
-------------------------------------------------------------------------------
-- Verifies that MoveSelectionSetEffectivenessColors correctly reflects the
-- alive opponent in a double battle (issue #7).
--
-- SETUP (automatic, every run):
--   test/lib/route14_battle.lua boots a new game, gives the player two
--   Pikachu (the lead knows Water Gun in move slot 0 and Vine Whip in move
--   slot 1), and starts the Kiri & Jan double battle on Route 14 (east of
--   Fuchsia City): Charmander (Fire, battler 1, opponent left) and Squirtle
--   (Water, battler 3, opponent right), both level 29. It stops with the
--   move-selection screen open on turn 1 and the cursor on move slot 0.
--   No saved state is involved, so the test always matches the ROM it runs.
--
-- WHERE THE COLOURS ARE READ:
--   Each move name has its own BG palette (10..13 for move slots 0..3), and
--   entry 13 of that palette is the text colour. The test reads it from
--   gPlttBufferFaded, the copy the game sends to the GBA's palette memory
--   every frame, and compares it with the RGB15 values
--   GetMoveEffectivenessColor returns (ADDR.EFF_COLOR_*).
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
--   Sub-test D — Cancel, then pick a move again:
--     B (back to the move menu), then A on move slot 0 again. The target
--     cursor resets to Charmander, and the colors must follow it at once:
--     Water = SUPER, Grass = NOT_VERY (not Squirtle's, left over from C).
--
--   Sub-test B — Left opponent absent (injected via memory write):
--     Inject gAbsentBattlerFlags = 2 (battler 1 absent), then navigate
--     FIGHT menu -> re-open move selection. InitMoveSelectionsVarsAndStrings
--     must fall back to the right opponent (Squirtle / Water type).
--     gMultiUsePlayerCursor = 0xFF proves the menu really reopened.
--     Water = NOT_VERY, Grass = SUPER.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua): one per sub-test, while the move names show
-- their colours. A and B hold on the move menu once its colours are
-- checked (fw.show): A, Water green and Grass amber against Charmander; B,
-- the reopened menu with Squirtle's colours. C and D play the change
-- itself (fw.show_next), each 90 frames from just before its key presses
-- until a second after: C, the cursor on Charmander, then LEFT to
-- Squirtle and the colours swap; D, B back to the move menu, then A, and
-- the cursor and colours snap back to Charmander. Everything else
-- fast-forwards.
--
-- HOW TO RUN:
--   Headless: bash test/run_test.sh test/tests/test_type_effectiveness_colors.lua
--   GUI:      Load in mgba-qt via Tools > Scripting > Load script
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local r14  = dofile(project_dir .. "test/lib/route14_battle.lua")

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

    -- ----------------------------------------------------------------
    -- Reach move selection in the Kiri & Jan double battle
    -- ----------------------------------------------------------------
    if not r14.reach_move_selection() then
        fw.assert_true(false, "reached move selection in the Kiri & Jan double battle")
        fw.finish()
        return
    end

    fw.wait_frames(30)  -- let the move menu finish drawing

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
    -- InitMoveSelectionsVarsAndStrings set these colors when the move menu
    -- opened. Both battlers are alive, so B_POSITION_OPPONENT_LEFT (battler 1)
    -- is used. Water vs Fire = SUPER, Grass vs Fire = NOT_VERY.
    -- ----------------------------------------------------------------
    fw.log("--- Sub-test A: both alive — colors reflect left opponent (Fire) ---")
    local a0 = move_color(0)
    local a1 = move_color(1)
    fw.log(string.format("  slot 0 = 0x%04X (want SUPER    = 0x%04X)", a0, ADDR.EFF_COLOR_SUPER))
    fw.log(string.format("  slot 1 = 0x%04X (want NOT_VERY = 0x%04X)", a1, ADDR.EFF_COLOR_NOT_VERY))
    fw.assert_eq(a0, ADDR.EFF_COLOR_SUPER,    "A: Water move = super-effective vs Fire (slot 0)")
    fw.assert_eq(a1, ADDR.EFF_COLOR_NOT_VERY, "A: Grass move = not-very-effective vs Fire (slot 1)")

    fw.screenshot(fw.out_path("mgba-type-eff-sub-a.png"))
    fw.show("A: Water green and Grass amber against Charmander")

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

    -- The moment's 90 frames: 30 with the cursor on Charmander, the 2 of
    -- the LEFT press, 5 before the check below, then 53 of Squirtle's
    -- colours.
    fw.show_next("C: LEFT to Squirtle, and the colours swap", 90)
    fw.wait_frames(30)

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

    fw.screenshot(fw.out_path("mgba-type-eff-sub-c.png"))
    fw.wait_frames(53)  -- the rest of the moment

    -- ----------------------------------------------------------------
    -- Sub-test D: cancel, then pick a move again
    -- Sub-test C left the cursor and the colours on Squirtle. B cancels
    -- target selection (the move menu keeps Squirtle's colours, which were
    -- the last ones shown). Picking a move again puts the target cursor back
    -- on its default, Charmander, so the colours must switch back to
    -- Charmander's matchups at once, without waiting for a D-pad press.
    -- ----------------------------------------------------------------
    fw.log("--- Sub-test D: cancel and pick a move again, colors follow the reset cursor ---")

    -- The moment's 90 frames: the 54 of B, the move menu, A and target
    -- selection opening, then 36 of Charmander's colours back.
    fw.show_next("D: B then A again, and the colours snap back to Charmander's", 90)
    fw.press("B")       -- cancel target selection -> move selection
    fw.wait_frames(20)
    fw.press("A")       -- pick move slot 0 again -> target selection
    fw.wait_frames(30)

    local cursor_again = fw.read8(ADDR.gMultiUsePlayerCursor)
    fw.log(string.format("  cursor after re-picking: battler %d (expect %d = left opp)",
        cursor_again, ADDR.BATTLER_OPPONENT_LEFT))
    fw.assert_eq(cursor_again, ADDR.BATTLER_OPPONENT_LEFT,
        "D: cursor reset to left opponent (battler 1)")

    local d0 = move_color(0)
    local d1 = move_color(1)
    fw.log(string.format("  slot 0 = 0x%04X (want SUPER    = 0x%04X)", d0, ADDR.EFF_COLOR_SUPER))
    fw.log(string.format("  slot 1 = 0x%04X (want NOT_VERY = 0x%04X)", d1, ADDR.EFF_COLOR_NOT_VERY))
    fw.assert_eq(d0, ADDR.EFF_COLOR_SUPER,    "D: Water move = super-effective vs Fire again (slot 0)")
    fw.assert_eq(d1, ADDR.EFF_COLOR_NOT_VERY, "D: Grass move = not-very-effective vs Fire again (slot 1)")

    fw.screenshot(fw.out_path("mgba-type-eff-sub-d.png"))
    fw.wait_frames(36)  -- the rest of the moment

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

    -- InitMoveSelectionsVarsAndStrings sets gMultiUsePlayerCursor to 0xFF;
    -- sub-test D left it at 1 (the target cursor). 0xFF here proves the
    -- colours below were recomputed by the reopened menu.
    fw.assert_eq(fw.read8(ADDR.gMultiUsePlayerCursor), 0xFF,
        "B: move selection reopened (InitMoveSelectionsVarsAndStrings ran)")

    local b0 = move_color(0)
    local b1 = move_color(1)
    fw.log(string.format("  slot 0 = 0x%04X (want NOT_VERY = 0x%04X)", b0, ADDR.EFF_COLOR_NOT_VERY))
    fw.log(string.format("  slot 1 = 0x%04X (want SUPER    = 0x%04X)", b1, ADDR.EFF_COLOR_SUPER))
    fw.assert_eq(b0, ADDR.EFF_COLOR_NOT_VERY, "B: Water move = not-very-effective vs absent-left / Water-right (slot 0)")
    fw.assert_eq(b1, ADDR.EFF_COLOR_SUPER,    "B: Grass move = super-effective vs absent-left / Water-right (slot 1)")

    fw.screenshot(fw.out_path("mgba-type-eff-sub-b.png"))
    fw.show("B: the reopened menu shows Squirtle's colours")

    fw.log("=== All sub-tests complete ===")
    fw.finish()
end)
