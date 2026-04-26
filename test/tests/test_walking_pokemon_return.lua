-------------------------------------------------------------------------------
-- test_walking_pokemon_return.lua
--
-- Regression test for issue #4: walking-pokemon RETURN must follow the
-- Pokemon by identity (species+PID), not party slot index.
--
-- Bug: after a party SWITCH that moves the walking Pokemon to a new slot,
-- the stale sFollower.partySlot caused RETURN to appear on the wrong slot
-- and the actual follower to lose its RETURN option entirely.
--
-- Full flow:
--   1. Boot to overworld. Inject Pikachu (slot 0) + Meowth (slot 1).
--   2. Open party menu -> WALK Pikachu.
--      Check: sFollower.active==1, sFollower.species==25.
--   3. Open party menu -> SWITCH Pikachu (slot 0) with Meowth (slot 1).
--      Check: sFollower.active still 1.
--   4. Open party menu -> slot 1 (Pikachu's new position) -> option [1].
--      After fix: option [1] = RETURN -> follower deactivates.
--      Before fix: option [1] = SWITCH -> sub-menu opens; B backs out; follower stays.
--      Check: sFollower.active==0.
--   5. Open party menu -> WALK Meowth (slot 0).
--      Check: sFollower.active==1, sFollower.species==52.
--
-- Run:
--   bash test/run_test.sh test/tests/test_walking_pokemon_return.lua
--
-- Record:
--   bash test/record_test.sh test/tests/test_walking_pokemon_return.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH   = "/tmp/pokefirered-walking-pokemon.ss"
local PARTY_BASE   = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE = 100         -- sizeof(struct Pokemon)

-- sFollower field offsets defined in test/lib/addresses.lua
local FOLLOWER_ACTIVE  = ADDR.sFollower + ADDR.FOLLOWER_OFF_ACTIVE
local FOLLOWER_SPECIES = ADDR.sFollower + ADDR.FOLLOWER_OFF_SPECIES  -- u16

local SPECIES_PIKACHU = 25
local SPECIES_MEOWTH  = 52

-- Write a minimal valid Pokemon into party memory at `base`.
-- Uses PID=0, OTID=0 so XOR encryption key = PID XOR OTID = 0 (no encryption).
-- Substruct order = PID % 24 = 0 = [Growth, Attacks, EVs, Misc].
-- Growth.species is the first u16 of the secure block (BoxPokemon+0x20).
local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)                       -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)                       -- no mail
    emu:write8(base + 0x20, species % 256)              -- Growth.species low
    emu:write8(base + 0x21, math.floor(species / 256))  -- Growth.species high
    emu:write8(base + 0x1C, species % 256)              -- checksum low
    emu:write8(base + 0x1D, math.floor(species / 256))  -- checksum high
    emu:write8(base + 0x54, level)                      -- level
    emu:write8(base + 0x56, 20)                         -- currentHP
    emu:write8(base + 0x58, 20)                         -- maxHP
end

local results = {}
-- check() logs PASS/FAIL but does not set a non-zero shell exit code on failure.
-- This matches the convention used by all tests in this project (see test_pc_anywhere.lua).
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.log((cond and "[PASS] " or "[FAIL] ") .. name)
end

fw.run(function()
    fw.log("=== Walking Pokemon Return Fix Test ===")

    -- ------------------------------------------------------------------ --
    -- Fixture: boot to overworld with Pikachu+Meowth, save state
    -- ------------------------------------------------------------------ --
    if not fw.try_load_state(STATE_PATH) then
        fw.log("No cached state — booting through Oak's speech...")
        local sel = cs.find_selection_press()
        if not sel then
            fw.log("ERROR: character select discovery failed — aborting")
            fw.finish()
            return
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()
        fw.log("Reached overworld. Injecting Pikachu + Meowth...")

        inject_pokemon(PARTY_BASE,                SPECIES_PIKACHU, 5)
        inject_pokemon(PARTY_BASE + POKEMON_SIZE, SPECIES_MEOWTH,  5)
        emu:write8(ADDR.gPlayerPartyCount, 2)

        -- Set FLAG_SYS_POKEMON_GET (0x828) so Start menu shows POKEMON.
        -- byte index = 0x828/8 = 0x105, bit = 0
        local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
        local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
        emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
        fw.wait_frames(30)
        fw.save_state(STATE_PATH)
    end
    fw.wait_frames(60)

    -- ------------------------------------------------------------------ --
    -- Phase 1: WALK Pikachu from slot 0
    -- Action menu for slot 0 (Pikachu, eligible, no follower):
    --   [0] SUMMARY  [1] WALK  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 1: WALK Pikachu (slot 0) ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("A");     fw.wait_frames(80)   -- open slot 0 action menu
    fw.press("DOWN");  fw.wait_frames(20)   -- move to [1] WALK
    fw.press("A");     fw.wait_frames(120)  -- select WALK; returns to overworld

    check("Phase 1: follower active after WALK",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("Phase 1: follower species is Pikachu",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_PIKACHU)

    -- ------------------------------------------------------------------ --
    -- Phase 2: SWITCH Pikachu (slot 0) with Meowth (slot 1)
    -- Action menu for slot 0 (Pikachu, IS follower, active):
    --   [0] SUMMARY  [1] RETURN  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- After SWITCH: Meowth is at slot 0, Pikachu is at slot 1.
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 2: SWITCH Pikachu to slot 1 ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("A");     fw.wait_frames(80)   -- open slot 0 action menu (Pikachu)
    fw.press("DOWN");  fw.wait_frames(20)   -- to [1] RETURN
    fw.press("DOWN");  fw.wait_frames(20)   -- to [2] SWITCH
    fw.press("A");     fw.wait_frames(60)   -- enter SWITCH mode
    fw.press("DOWN");  fw.wait_frames(20)   -- move cursor to slot 1
    fw.press("A");     fw.wait_frames(80)   -- confirm swap; party now swapped
    fw.press("B");     fw.wait_frames(80)   -- close party menu
    fw.press("B");     fw.wait_frames(30)   -- close start menu if still open

    check("Phase 2: follower still active after SWITCH",
          fw.read8(FOLLOWER_ACTIVE) == 1)

    -- ------------------------------------------------------------------ --
    -- Phase 3: RETURN Pikachu from slot 1 (its new position)
    -- After fix — slot 1 action menu (Pikachu identified by species+PID):
    --   [0] SUMMARY  [1] RETURN  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- Before fix — slot 1 action menu (orphaned: follower is "not here" by slot):
    --   [0] SUMMARY  [1] SWITCH  [2] ITEM  [3] MOVE TO PC  [4] CANCEL
    --   Pressing [1] opens SWITCH sub-menu; pressing B backs out; active stays 1.
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 3: RETURN Pikachu from slot 1 ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("DOWN");  fw.wait_frames(20)   -- cursor to slot 1
    fw.press("A");     fw.wait_frames(80)   -- open slot 1 action menu
    fw.press("DOWN");  fw.wait_frames(20)   -- to option [1] (RETURN after fix)
    fw.press("A");     fw.wait_frames(120)  -- select it; returns to overworld if RETURN
    -- If SWITCH fired instead (pre-fix bug), back out of any sub-menus
    fw.press("B");     fw.wait_frames(30)
    fw.press("B");     fw.wait_frames(30)
    fw.press("B");     fw.wait_frames(30)

    check("Phase 3: follower deactivated by RETURN",
          fw.read8(FOLLOWER_ACTIVE) == 0)

    -- ------------------------------------------------------------------ --
    -- Phase 4: WALK Meowth from slot 0 (its new position after swap)
    -- Action menu for slot 0 (Meowth, eligible, no follower active):
    --   [0] SUMMARY  [1] WALK  [2] SWITCH  [3] ITEM  [4] MOVE TO PC  [5] CANCEL
    -- ------------------------------------------------------------------ --
    fw.log("--- Phase 4: WALK Meowth (slot 0) ---")
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    fw.press("A");     fw.wait_frames(80)   -- open slot 0 action menu (Meowth)
    fw.press("DOWN");  fw.wait_frames(20)   -- to [1] WALK
    fw.press("A");     fw.wait_frames(120)  -- select WALK; returns to overworld

    check("Phase 4: follower active after WALK Meowth",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("Phase 4: follower species is Meowth",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_MEOWTH)

    -- ------------------------------------------------------------------ --
    -- Report
    -- ------------------------------------------------------------------ --
    local passed = 0
    for _, r in ipairs(results) do if r.pass then passed = passed + 1 end end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
