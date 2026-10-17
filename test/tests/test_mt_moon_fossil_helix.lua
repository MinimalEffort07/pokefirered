-------------------------------------------------------------------------------
-- test_mt_moon_fossil_helix.lua: S5, take the Helix Fossil
-------------------------------------------------------------------------------
--
-- The mirror of S4 (test_mt_moon_fossil_dome.lua), in a game of its own.
--
-- SETUP: a new game, every Mt. Moon trainer marked defeated (Super Nerd
-- Miguel too: VAR_MAP_SCENE_MT_MOON_B2F = 1), and no wild battles
-- (test/lib/mt_moon_chain.lua's prepare()).
--
-- Enter B2F, walk to the Helix Fossil with real D-pad steps, face it, press
-- A and answer YES (test/lib/mt_moon_fossil.lua has the details).
-- PASS: FLAG_GOT_HELIX_FOSSIL and FLAG_GOT_FOSSIL_FROM_MT_MOON are set, the
-- bag holds the HELIX FOSSIL, the Dome Fossil's object is gone, Miguel
-- stands right below the Dome Fossil's old tile, and the player has control
-- again within 600 frames.
--
-- Run:
--   MGBA_TEST_TIMEOUT=300 bash test/run_test.sh test/tests/test_mt_moon_fossil_helix.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local fossil  = dofile(project_dir .. "test/lib/mt_moon_fossil.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

-- Random state for the fresh entry into B2F.
local RNG = 0x5EED2705

fw.run(function()
    fw.log("=== Mt. Moon: take the Helix Fossil (S5) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    chain.prepare()

    capture.start()
    fossil.take("helix", RNG, fw, capture)
    capture.stop()
    fw.finish()
end)
