-------------------------------------------------------------------------------
-- demo_type_effectiveness_colors.lua
-------------------------------------------------------------------------------
-- Visual demonstration of the double-battle type effectiveness color fix.
-- Starts the same Kiri & Jan double battle as test_type_effectiveness_colors.lua
-- (test/lib/route14_battle.lua) and plays through the three scenes.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua): one per scene. Scene 1 holds on the colours
-- against Charmander. Scene 2 starts half a second before LEFT, so the
-- video shows the cursor on Charmander, then the jump to Squirtle and the
-- colours swapping. Scene 3 holds on the reopened menu with Squirtle's
-- colours. Everything else fast-forwards.
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
fw.allow_no_assertions("demo: plays the double battle's three scenes for the type colours recording")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local r14  = dofile(project_dir .. "test/lib/route14_battle.lua")

fw.run(function()
    fw.log("=== Demo: Type Effectiveness Colors Fix (issue #7) ===")

    -- Boot and start the battle.
    if not r14.reach_move_selection() then
        fw.finish()
        return
    end

    fw.wait_frames(60)  -- let the move menu finish drawing

    -- ----------------------------------------------------------------
    -- Scene 1: Both opponents alive — move menu shows Fire effectiveness
    -- Viewer sees: Water = GREEN (super), Grass = AMBER (not very)
    -- ----------------------------------------------------------------
    fw.log(">>> Scene 1: Both opponents alive — colors reflect Charmander (Fire)")
    fw.show("Scene 1: Water green and Grass amber against Charmander")

    fw.screenshot(fw.out_path("mgba-demo-eff-scene1.png"))

    -- ----------------------------------------------------------------
    -- Scene 2: Move cursor to right opponent (Squirtle / Water type)
    -- Press A to enter target selection, then DPAD_LEFT to Squirtle.
    -- Colors update in real-time.
    -- ----------------------------------------------------------------
    fw.log(">>> Scene 2: DPAD to right opponent — colors update live to Water")
    fw.press("A")         -- select move slot 0 (Water Gun) → target selection opens
    fw.wait_frames(30)    -- let target selection animate open

    -- The moment's 90 frames: 30 with the cursor on Charmander, the 2 of
    -- the LEFT press, then 58 of Squirtle's colours.
    fw.show_next("Scene 2: LEFT moves the cursor to Squirtle, and the colours swap", 90)
    fw.wait_frames(30)
    fw.log("  Pressing DPAD_LEFT to move cursor from Charmander to Squirtle...")
    fw.press("LEFT")      -- cursor: battler 1 → battler 3
    fw.wait_frames(58)

    fw.screenshot(fw.out_path("mgba-demo-eff-scene2.png"))

    -- Move the cursor back: RIGHT goes to Charmander (battler 1, colours
    -- switch back to the Fire matchups), RIGHT again to the player's partner
    -- (an ally: the colours stay as they are).
    fw.press("RIGHT")
    fw.wait_frames(30)
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
    fw.show("Scene 3: the reopened menu shows Squirtle's colours")

    fw.screenshot(fw.out_path("mgba-demo-eff-scene3.png"))

    fw.log("=== Demo complete ===")
    fw.finish()
end)
