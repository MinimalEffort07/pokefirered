-------------------------------------------------------------------------------
-- test_player_sprite.lua
-------------------------------------------------------------------------------
-- A basic smoke test that verifies the mGBA test infrastructure works:
--   1. Boots the ROM and waits for the game to initialize
--   2. Presses buttons to navigate past the title screen
--   3. Reads game memory to check the player sprite state
--   4. Reports pass/fail results
--
-- This test does NOT start a new game or reach the overworld — it just
-- proves that button injection, memory reading, and the test framework
-- all function correctly.
--
-- HOW TO RUN:
--   Headless:  bash test/run_test.sh
--   GUI:       Load this file in mgba-qt via Tools > Scripting > Load script
--
-------------------------------------------------------------------------------

-- Resolve paths relative to this script's location so it works
-- regardless of what directory mGBA was launched from.
-- debug.getinfo(1, "S").source returns the path to THIS file (e.g.,
-- "@/home/.../test/tests/test_player_sprite.lua"). We strip the filename
-- to get the directory, then go up two levels to reach the project root.
local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

-- Load the test framework and address constants.
-- dofile() executes a Lua file and returns whatever it returns.
local fw = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

-- fw.run() starts the test. The function passed to it is the actual test
-- logic. It runs as a coroutine — each fw.wait_frames() or fw.press()
-- call pauses the function and lets the game advance a frame before
-- continuing. See framework.lua for how this works.
fw.run(function()
    fw.log("=== Test: Player Sprite ===")

    -- Wait 200 frames (~3.3 seconds) for the game to boot.
    -- The GBA runs at 60fps. The GameFreak intro logo plays during this time.
    fw.wait_frames(200)
    fw.log("Boot complete")

    -- Read gMain.callback2 to verify the game is running.
    -- callback2 is a function pointer that tells us what "screen" is active.
    -- If it's non-zero, the game has initialized and is running some screen.
    local main_cb2 = fw.read32(ADDR.gMain_callback2)
    fw.log(string.format("callback2 = 0x%08X", main_cb2))
    fw.assert_neq(main_cb2, 0, "gMain.callback2 is initialized")

    -- Press A 10 times with 60-frame gaps to navigate past the title screen.
    -- Each fw.press("A") holds A for 1 frame then releases for 1 frame.
    -- The 60-frame wait gives the game time to process each press and
    -- advance to the next text box or menu option.
    fw.log("Navigating title screen...")
    for i = 1, 10 do
        fw.log("Press A (" .. i .. ")")
        fw.press("A")
        fw.wait_frames(60)
    end
    fw.log("Navigation done")

    -- Read the player's ObjectEvent data from memory.
    --
    -- The player's overworld state works like this:
    --   gPlayerAvatar.objectEventId = which index in gObjectEvents[] is the player
    --   gObjectEvents[index].graphicsId = which sprite the player is using
    --
    -- We read the objectEventId first, then calculate the address of
    -- that ObjectEvent in the array, then read its graphicsId field.
    local obj_event_id = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local obj_addr = ADDR.gObjectEvents + (obj_event_id * ADDR.OBJECT_EVENT_SIZE)
    local graphics_id = fw.read8(obj_addr + ADDR.OE_GRAPHICS_ID)
    fw.log(string.format("Player: objEventId=%d, graphicsId=%d", obj_event_id, graphics_id))

    -- This assertion always passes — it just proves the test ran to completion.
    -- The real sprite verification happens in test_seel_character.lua.
    fw.assert_true(true, "test infrastructure works end-to-end")

    -- Report results and exit. In headless mode this kills the process.
    -- In GUI mode this restores normal speed and stops the test.
    fw.finish()
end)
