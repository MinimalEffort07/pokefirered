-------------------------------------------------------------------------------
-- demo_type_effectiveness_colors.lua
-------------------------------------------------------------------------------
-- Visual demonstration of the double-battle type effectiveness color fix.
-- Loads the same fixture used by test_type_effectiveness_colors.lua and
-- plays through the three scenarios slowly so they are clearly visible
-- in the recorded GIF.
--
-- REQUIRES: /tmp/pokefirered-route14-double.ss
-- See test/tools/save_route14_battle_state.lua to create the fixture.
--
-- HOW TO RECORD:
--   bash test/record_test.sh test/tests/demo_type_effectiveness_colors.lua
--   mv /tmp/mgba-recordings/demo_type_effectiveness_colors_*.mp4 ~/recordings/
--
-- HOW TO RUN (GUI only — not useful headless):
--   Load in mgba-qt via Tools > Scripting > Load script
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH = "/tmp/pokefirered-route14-double.ss"

fw.run(function()
    fw.log("=== Demo: Type Effectiveness Colors Fix (issue #7) ===")

    -- Run at real-time throughout so the viewer sees everything clearly.
    fw.slow_down("demo — entire script at real-time")
    fw.wait_frames(30)

    if not fw.try_load_state(STATE_PATH) then
        fw.log_error("FIXTURE MISSING: " .. STATE_PATH)
        fw.log_error("Run test/tools/save_route14_battle_state.lua to create it.")
        fw.finish()
        return
    end

    fw.wait_frames(60)  -- 1 second at 60fps — let viewer see the loaded state

    -- ----------------------------------------------------------------
    -- Scene 1: Both opponents alive — move menu shows Fire effectiveness
    -- Viewer sees: Water = GREEN (super), Grass = AMBER (not very)
    -- ----------------------------------------------------------------
    fw.log(">>> Scene 1: Both opponents alive — colors reflect Charmander (Fire)")
    fw.wait_frames(120)  -- 2 seconds to observe the initial colors

    fw.screenshot("/tmp/mgba-demo-eff-scene1.png")

    -- ----------------------------------------------------------------
    -- Scene 2: Move cursor to right opponent (Squirtle / Water type)
    -- Press A to enter target selection, then DPAD_LEFT to Squirtle.
    -- Colors update in real-time.
    -- ----------------------------------------------------------------
    fw.log(">>> Scene 2: DPAD to right opponent — colors update live to Water")
    fw.press("A")         -- select move slot 0 (Water Gun) → target selection opens
    fw.wait_frames(60)    -- let target selection animate open

    fw.log("  Pressing DPAD_LEFT to move cursor from Charmander to Squirtle...")
    fw.press("LEFT")      -- cursor: battler 1 → battler 3
    fw.wait_frames(120)   -- 2 seconds to observe the updated colors

    fw.screenshot("/tmp/mgba-demo-eff-scene2.png")

    -- Move cursor back to left opponent to reset state
    fw.press("RIGHT")     -- cursor back to battler 1... actually goes to player partner
    fw.wait_frames(30)
    -- Keep pressing RIGHT until we wrap back to OPPONENT_LEFT
    fw.press("RIGHT")
    fw.wait_frames(30)

    -- Cancel target selection
    fw.press("B")
    fw.wait_frames(60)

    -- ----------------------------------------------------------------
    -- Scene 3: Left opponent absent — colors fall back to Squirtle
    -- Inject gAbsentBattlerFlags, re-enter move selection.
    -- Viewer sees: colors now show Water effectiveness (not Fire).
    -- ----------------------------------------------------------------
    fw.log(">>> Scene 3: Inject absent flag for left opponent, reopen move menu")
    emu:write8(ADDR.gAbsentBattlerFlags, 0x02)
    fw.log("  gAbsentBattlerFlags = 0x02 (Charmander slot absent)")

    fw.press("B")         -- leave move selection → action menu
    fw.wait_frames(90)    -- wait for action menu
    fw.press("A")         -- select FIGHT
    fw.wait_frames(120)   -- wait for move selection to reopen

    fw.log("  Move menu re-opened. Colors should now reflect Squirtle (Water type).")
    fw.wait_frames(120)   -- 2 seconds to observe

    fw.screenshot("/tmp/mgba-demo-eff-scene3.png")

    fw.log("=== Demo complete ===")
    fw.finish()
end)
