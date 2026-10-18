-------------------------------------------------------------------------------
-- test_walking_pokemon_return.lua
--
-- Regression test for issue #4: the walking Pokemon (the "follower") must be
-- tracked by identity, not by party slot index, so its RETURN option goes
-- wherever the Pokemon goes and it can always be put away.
--
-- Identity is the pair the game itself uses for an individual Pokemon: its
-- personality value (PID, a random 32-bit number rolled when it is created)
-- and its original trainer ID (OTID). Neither changes when the Pokemon moves
-- slot or evolves. Species is NOT part of it, because evolution changes it.
--
-- Fixture: boot to the overworld and inject a party of three Pokemon that
-- all have walking sprites, each with its own PID and OTID:
--   slot 0: Pikachu   slot 1: Meowth   slot 2: Machop
-- Every scenario below starts again from that saved fixture, so they don't
-- depend on each other.
--
-- Scenario S (agreed in issue #4): SWITCH moves the walker.
--   1. WALK Pikachu (slot 0).          Check: active, species Pikachu.
--   2. SWITCH Pikachu with Meowth.     Check: still active.
--   3. Slot 1 (Pikachu now) -> option [1], which must be RETURN.
--                                      Check: follower deactivated.
--      (Before the fix option [1] there is SWITCH: B backs out, still active.)
--   4. WALK Meowth (slot 0 now).       Check: active, species Meowth.
--
-- Scenario E1: the walker evolves into another species that can walk.
--   1. WALK Machop (slot 2).           Check: active, species Machop.
--   2. Evolve it in RAM to Machoke: only the species changes, the PID and
--      OTID stay, exactly as real evolution does.
--   3. Slot 2 -> option [1], which must be RETURN.
--                                      Check: follower deactivated.
--
-- Scenario E2: the walker evolves into a species with no walking sprite.
--   1. WALK Pikachu (slot 0).          Check: active.
--   2. Evolve it in RAM to Raichu (no overworld sprite, so it could never
--      be picked with WALK itself).
--   3. Slot 0 -> option [1], which must still be RETURN.
--                                      Check: follower deactivated.
--
-- Scenario D: the walker is deposited with the party menu's MOVE TO PC.
--   1. WALK Pikachu (slot 0).          Check: active.
--   2. Slot 0 -> MOVE TO PC.           Check: party is down to 2, the
--      follower is deactivated and its sprite is gone (a follower whose
--      Pokemon is in a PC box could never be RETURNed).
--   3. WALK Meowth (slot 0 now).       Check: active, species Meowth.
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
local sym  = dofile(project_dir .. "test/lib/symbols.lua")

-- fw.out_path: private to each run_test.sh run, so a state saved by another
-- run (or another worktree's ROM) is never loaded by mistake.
local STATE_PATH   = fw.out_path("pokefirered-walking-pokemon.ss")
-- gPlayerParty's address comes from the built pokefirered.elf, so it stays
-- right when RAM variables move between builds.
local PARTY_BASE   = sym.addr("gPlayerParty")
local POKEMON_SIZE = 100         -- sizeof(struct Pokemon)

-- sFollower field offsets defined in test/lib/addresses.lua
local FOLLOWER_ACTIVE  = ADDR.sFollower + ADDR.FOLLOWER_OFF_ACTIVE
local FOLLOWER_SPECIES = ADDR.sFollower + ADDR.FOLLOWER_OFF_SPECIES  -- u16
local FOLLOWER_SPRITE_SPAWNED = ADDR.sFollower + ADDR.FOLLOWER_OFF_SPRITE_SPAWNED

local SPECIES_PIKACHU = 25
local SPECIES_RAICHU  = 26
local SPECIES_MEOWTH  = 52
local SPECIES_MACHOP  = 66
local SPECIES_MACHOKE = 67

-- The fixture party. Each Pokemon gets its own PID and OTID, so the game can
-- only tell them apart the way it should: by identity. Every PID is a
-- multiple of 24 (see write_pokemon), and none of the pairs is shiny.
local PARTY = {
    { species = SPECIES_PIKACHU, pid = 0x5A3C9F18, otid = 0x3039D431 },
    { species = SPECIES_MEOWTH,  pid = 0x7E21B6B8, otid = 0x8B1E0F27 },
    { species = SPECIES_MACHOP,  pid = 0x1C94E398, otid = 0x4C7A21D6 },
}

-- Address of party slot `slot` (0-5) in gPlayerParty.
local function slot_addr(slot)
    return PARTY_BASE + slot * POKEMON_SIZE
end

-- Write a minimal valid level-5 Pokemon into party memory at `base`.
--
-- A party Pokemon (struct Pokemon, 100 bytes) starts with a struct
-- BoxPokemon: PID at +0x00, OTID at +0x04, a "has species" flag at +0x13, a
-- checksum at +0x1C and a 48-byte "secure" block at +0x20. The secure block
-- holds four 12-byte substructs (Growth, Attacks, EVs, Misc) whose ORDER
-- depends on PID % 24; a PID that is a multiple of 24 puts Growth first, and
-- Growth starts with the species (u16). The game stores the block encrypted:
-- every 32-bit word is XORed with (PID XOR OTID). The checksum is the sum of
-- the block's 24 decrypted 16-bit halves; if it doesn't match, the game turns
-- the Pokemon into a Bad Egg. Here only the species is non-zero, so the
-- checksum is simply the species number.
local function write_pokemon(base, species, pid, otid)
    assert(pid % 24 == 0, "PID must be a multiple of 24 (Growth substruct first)")
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write32(base + 0x00, pid)
    emu:write32(base + 0x04, otid)
    emu:write8(base + 0x13, 0x02)                       -- hasSpecies flag
    emu:write16(base + 0x1C, species)                   -- checksum (see above)
    local key = (pid ~ otid) & 0xFFFFFFFF
    for word = 0, 11 do                                 -- 12 words = 48 bytes
        local plain = (word == 0) and species or 0      -- Growth.species
        emu:write32(base + 0x20 + word * 4, (plain ~ key) & 0xFFFFFFFF)
    end
    emu:write8(base + 0x54, 5)                          -- level
    emu:write8(base + 0x55, 0xFF)                       -- no mail
    emu:write16(base + 0x56, 20)                        -- currentHP
    emu:write16(base + 0x58, 20)                        -- maxHP
end

-- check() records each result with fw.assert_true, so a failed check makes
-- fw.finish() write a non-zero exit code and test/run_test.sh exit 1.
local function check(name, cond)
    fw.assert_true(cond, name)
end

-- Open the field party menu (START -> POKEMON) and the action list of party
-- slot `slot`. The party menu cursor always opens on slot 0.
local function open_actions(slot)
    fw.press("START"); fw.wait_frames(80)
    fw.press("A");     fw.wait_frames(120)  -- open party menu
    for _ = 1, slot do
        fw.press("DOWN"); fw.wait_frames(20)
    end
    fw.press("A");     fw.wait_frames(80)   -- open the slot's action list
end

-- Pick action `index` (0 = SUMMARY at the top) from the open action list.
local function pick_action(index)
    for _ = 1, index do
        fw.press("DOWN"); fw.wait_frames(20)
    end
    fw.press("A");     fw.wait_frames(120)
end

-- Back out to the overworld. WALK and RETURN have already closed the menus,
-- so these presses then do nothing; if option [1] was something else (the
-- bug), they back out of whatever it opened.
local function back_out()
    for _ = 1, 3 do
        fw.press("B"); fw.wait_frames(30)
    end
end

-- Action list of a walk-capable Pokemon with no field moves (ours have no
-- moves at all), in a party of two or more:
--   no follower active:            [0] SUMMARY [1] WALK   [2] SWITCH ...
--   this Pokemon is the follower:  [0] SUMMARY [1] RETURN [2] SWITCH ...
--   another Pokemon is following:  [0] SUMMARY [1] SWITCH [2] ITEM ...
local ACTION_WALK_OR_RETURN = 1
local ACTION_SWITCH_WHILE_FOLLOWER = 2
-- The follower's own list: [0] SUMMARY [1] RETURN [2] SWITCH [3] ITEM
-- [4] MOVE TO PC [5] CANCEL
local ACTION_MOVE_TO_PC_ON_FOLLOWER = 4

-- WALK the Pokemon in party slot `slot`.
local function walk(slot)
    open_actions(slot)
    pick_action(ACTION_WALK_OR_RETURN)
end

-- Load the fixture: the overworld with the three-Pokemon party.
local function load_fixture()
    if not fw.try_load_state(STATE_PATH) then
        fw.log("No cached state — booting through Oak's speech...")
        local sel = cs.find_selection_press()
        if not sel then
            error("character select discovery failed")
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()
        fw.log("Reached overworld. Injecting Pikachu + Meowth + Machop...")

        for i, mon in ipairs(PARTY) do
            write_pokemon(slot_addr(i - 1), mon.species, mon.pid, mon.otid)
        end
        emu:write8(ADDR.gPlayerPartyCount, #PARTY)

        -- Set FLAG_SYS_POKEMON_GET (0x828) so Start menu shows POKEMON.
        -- byte index = 0x828/8 = 0x105, bit = 0
        local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
        local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
        emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
        fw.wait_frames(30)
        fw.save_state(STATE_PATH)
    end
    fw.wait_frames(60)
end

-------------------------------------------------------------------------------
-- Scenario S: SWITCH moves the walker (the case issue #4 reported)
-------------------------------------------------------------------------------
local function scenario_switch()
    fw.log("--- Scenario S: WALK, SWITCH, RETURN from the new slot ---")
    load_fixture()

    walk(0)
    check("S1: follower active after WALK Pikachu",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("S1: follower species is Pikachu",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_PIKACHU)

    -- SWITCH Pikachu (slot 0, now showing RETURN) with Meowth (slot 1).
    open_actions(0)
    pick_action(ACTION_SWITCH_WHILE_FOLLOWER)   -- enter SWITCH mode
    fw.press("DOWN");  fw.wait_frames(20)       -- cursor to slot 1
    fw.press("A");     fw.wait_frames(80)       -- confirm the swap
    fw.press("B");     fw.wait_frames(80)       -- close party menu
    fw.press("B");     fw.wait_frames(30)       -- close start menu
    check("S2: follower still active after SWITCH",
          fw.read8(FOLLOWER_ACTIVE) == 1)

    open_actions(1)                             -- Pikachu's new slot
    pick_action(ACTION_WALK_OR_RETURN)          -- must be RETURN
    back_out()
    check("S3: follower deactivated by RETURN from its new slot",
          fw.read8(FOLLOWER_ACTIVE) == 0)

    walk(0)                                     -- Meowth's new slot
    check("S4: follower active after WALK Meowth",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("S4: follower species is Meowth",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_MEOWTH)
end

-------------------------------------------------------------------------------
-- Scenario E1: the walker evolves into another species that can walk
-------------------------------------------------------------------------------
local function scenario_evolve_to_walker()
    fw.log("--- Scenario E1: WALK Machop, it evolves into Machoke, RETURN ---")
    load_fixture()

    walk(2)
    check("E1: follower active after WALK Machop",
          fw.read8(FOLLOWER_ACTIVE) == 1)
    check("E1: follower species is Machop",
          fw.read16(FOLLOWER_SPECIES) == SPECIES_MACHOP)

    -- Evolution rewrites the species and keeps the PID and OTID.
    local machop = PARTY[3]
    write_pokemon(slot_addr(2), SPECIES_MACHOKE, machop.pid, machop.otid)
    fw.wait_frames(10)

    open_actions(2)
    pick_action(ACTION_WALK_OR_RETURN)          -- must be RETURN
    back_out()
    check("E1: evolved follower (Machoke) can be RETURNed",
          fw.read8(FOLLOWER_ACTIVE) == 0)
end

-------------------------------------------------------------------------------
-- Scenario E2: the walker evolves into a species with no walking sprite
-------------------------------------------------------------------------------
local function scenario_evolve_to_non_walker()
    fw.log("--- Scenario E2: WALK Pikachu, it evolves into Raichu, RETURN ---")
    load_fixture()

    walk(0)
    check("E2: follower active after WALK Pikachu",
          fw.read8(FOLLOWER_ACTIVE) == 1)

    -- Raichu has no overworld sprite, so WALK is never offered for it; the
    -- Pokemon that is already walking must still be returnable.
    local pikachu = PARTY[1]
    write_pokemon(slot_addr(0), SPECIES_RAICHU, pikachu.pid, pikachu.otid)
    fw.wait_frames(10)

    open_actions(0)
    pick_action(ACTION_WALK_OR_RETURN)          -- must be RETURN
    back_out()
    check("E2: evolved follower without a walking sprite (Raichu) can be RETURNed",
          fw.read8(FOLLOWER_ACTIVE) == 0)
end

-------------------------------------------------------------------------------
-- Scenario D: the walker is deposited with the party menu's MOVE TO PC
-------------------------------------------------------------------------------
local function scenario_move_to_pc()
    fw.log("--- Scenario D: WALK Pikachu, MOVE TO PC, WALK Meowth ---")
    load_fixture()

    walk(0)
    check("D: follower active after WALK Pikachu",
          fw.read8(FOLLOWER_ACTIVE) == 1)

    -- MOVE TO PC prints "<name> was sent to the PC!" and then closes the
    -- party menu by itself; B only closes the start menu if it reopened.
    open_actions(0)
    pick_action(ACTION_MOVE_TO_PC_ON_FOLLOWER)
    fw.wait_frames(120)
    fw.press("B");     fw.wait_frames(60)
    check("D: Pikachu was deposited (party of 2)",
          fw.read8(ADDR.gPlayerPartyCount) == 2)
    check("D: follower deactivated when its Pokemon went to the PC",
          fw.read8(FOLLOWER_ACTIVE) == 0)
    check("D: follower sprite gone after MOVE TO PC",
          fw.read8(FOLLOWER_SPRITE_SPAWNED) == 0)

    walk(0)                                     -- Meowth moved up to slot 0
    back_out()
    check("D: WALK offered on another Pokemon afterwards (Meowth follows)",
          fw.read8(FOLLOWER_ACTIVE) == 1
          and fw.read16(FOLLOWER_SPECIES) == SPECIES_MEOWTH)
end

fw.run(function()
    fw.log("=== Walking Pokemon Return Test ===")
    scenario_switch()
    scenario_evolve_to_walker()
    scenario_evolve_to_non_walker()
    scenario_move_to_pc()
    fw.finish()
end)
