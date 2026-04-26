-------------------------------------------------------------------------------
-- save_route14_battle_state.lua
-------------------------------------------------------------------------------
-- Helper for creating the Route 14 double-battle fixture used by
-- test/tests/test_type_effectiveness_colors.lua.
--
-- HOW TO USE (one-time setup):
--   1. Open the ROM in mgba-qt:
--        mgba-qt pokefirered.gba
--   2. Make sure your LEAD Pokemon has:
--        Move slot 0 — a Water-type move  (e.g. Water Gun, Surf)
--        Move slot 1 — a Grass-type move  (e.g. Absorb, Vine Whip)
--        Move cursor on slot 0 (the default)
--   3. Walk east from Fuchsia City onto Route 14 and talk to Kiri (or Jan)
--      to trigger the double battle.
--   4. Wait for the MOVE SELECTION screen to open on your first turn.
--   5. While the move selection screen is showing, open the scripting console:
--        Tools > Scripting > File > Open this file
--   6. Click Run. The script saves the emulator state to
--        /tmp/pokefirered-route14-double.ss
--      and prints a confirmation.
--   7. test_type_effectiveness_colors.lua will now load that state every run.
--
-- NOTE: Re-run this setup only if the ROM is rebuilt and EWRAM symbol
-- addresses change (rare — only when struct layouts are modified).
-------------------------------------------------------------------------------

local STATE_PATH = "/tmp/pokefirered-route14-double.ss"
local SAVESTATE_ALL = 31  -- screenshot + savedata + cheats + RTC + metadata

console:log("[save_route14] Writing battle state to " .. STATE_PATH .. " ...")
local ok = emu:saveStateFile(STATE_PATH, SAVESTATE_ALL)
if ok then
    console:log("[save_route14] OK — state saved.")
    console:log("[save_route14] You can now run test_type_effectiveness_colors.lua.")
    console:log("[save_route14] Reminder: move slot 0 = Water-type, slot 1 = Grass-type.")
else
    console:error("[save_route14] FAILED to save state. Check disk space and path.")
end
