-------------------------------------------------------------------------------
-- save_route1_state.lua
-------------------------------------------------------------------------------
-- Helper for manually creating the Route 1 fixture used by
-- test/tests/test_roaming_route1.lua.
--
-- HOW TO USE (one-time setup):
--   1. Open the ROM in mgba-qt:           mgba-qt pokefirered.gba
--   2. Play through Oak's speech.
--   3. Walk north out of Pallet Town until you're standing on Route 1
--      (verify by the "ROUTE 1" map name banner).
--   4. Make sure you're in the overworld, NOT in a menu or battle.
--   5. Tools > Scripting > File > Open this script.
--   6. Click Run. The script writes the current emulator state to
--      /tmp/pokefirered-route1.ss and prints a confirmation.
--   7. test_roaming_route1.lua will now load that state on every run
--      (no need to repeat steps 1-6 unless the ROM is rebuilt with
--      a layout change that invalidates EWRAM symbol addresses).
--
-- This script does NOT use the test framework. It's a one-shot
-- save-state dump intended to be run manually in the GUI scripting
-- console.
-------------------------------------------------------------------------------

local STATE_PATH = "/tmp/pokefirered-route1.ss"
local SAVESTATE_ALL = 31  -- screenshot + savedata + cheats + RTC + metadata

console:log("[save_route1] Writing save state to " .. STATE_PATH .. "...")
local ok = emu:saveStateFile(STATE_PATH, SAVESTATE_ALL)
if ok then
    console:log("[save_route1] OK -- state saved.")
    console:log("[save_route1] You can now run test_roaming_route1.lua.")
else
    console:error("[save_route1] FAILED to save state. Check disk and path.")
end
