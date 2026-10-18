-------------------------------------------------------------------------------
-- route14_battle.lua: start the Kiri & Jan double battle from power-on and
-- stop on the player's first move-selection screen
-------------------------------------------------------------------------------
--
-- Used by test/tests/test_type_effectiveness_colors.lua (issue #7), its demo
-- test/tests/demo_type_effectiveness_colors.lua, and the fixture tool
-- test/tools/create_route14_battle_fixture.lua.
--
-- Kiri & Jan are the twins on Route 14, east of Fuchsia City. Theirs is a
-- double battle against a level 29 Charmander (Fire, opponent left, battler 1)
-- and Squirtle (Water, opponent right, battler 3), so a Water move and a Grass
-- move have opposite effectiveness against the two opponents.
--
-- WHY THE TEST BUILDS THIS STATE EVERY RUN instead of loading a saved one:
-- a save state holds the whole of RAM, including pointers into ROM code
-- (gMain.callback2, the battle controller functions). Any rebuild that moves
-- code makes an old state jump into the wrong code. Building it from
-- power-on costs a few seconds headless and is always right for the ROM
-- being tested.
--
-- WHAT r14.reach_move_selection() DOES:
--   1. Boots through Oak's speech into Pallet Town (character_select.lua).
--      A new game has an empty party.
--   2. Writes two Pikachu into the party (a double battle needs two Pokemon
--      that can fight). Slot 0, the lead, knows Water Gun (move slot 0) and
--      Vine Whip (move slot 1).
--   3. Clears Kiri's "trainer defeated" flag and moves the player onto
--      Route 14 at (12, 52), the tile just below Kiri (she stands at (12, 51)
--      facing down with a sight range of 1), so she spots the player at once.
--   4. Presses A through her approach and dialogue until gBattleTypeFlags
--      says a double battle started.
--   5. Presses A through the battle intro until the move-selection screen
--      opens: InitMoveSelectionsVarsAndStrings sets gMultiUsePlayerCursor to
--      0xFF the moment it does, so the loop checks that BEFORE each A press
--      and never overshoots into target selection.
-- It returns true with the move-selection screen open for the player's left
-- battler (battler 0), or false after logging why.
--
-- Every address is looked up by name in pokefirered.elf (symbols.lua), or
-- through addresses.lua, which does the same for its symbol entries.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local sym  = dofile(project_dir .. "test/lib/symbols.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")

local r14 = {}

-- Route 14 is MAP_ROUTE14 = (32 | (3 << 8)) in include/constants/map_groups.h.
r14.MAP_GROUP = 3
r14.MAP_NUM   = 32
-- Kiri stands at (12, 51) facing down with trainer_sight 1
-- (data/maps/Route14/map.json), so she sees the tile below her.
r14.PLAYER_X  = 12
r14.PLAYER_Y  = 52

-- include/constants/moves.h
r14.MOVE_WATER_GUN = 55
r14.MOVE_VINE_WHIP = 22

-- gBattleTypeFlags bits (include/constants/battle.h).
local BATTLE_TYPE_DOUBLE  = 0x01
local BATTLE_TYPE_TRAINER = 0x08

-- struct Pokemon (include/pokemon.h), 100 bytes. The first 0x50 bytes are the
-- struct BoxPokemon that the PC also stores.
local POKEMON_SIZE   = 100
local MON_FLAGS      = 0x13  -- bit 1 = hasSpecies (the slot holds a Pokemon)
local MON_CHECKSUM   = 0x1C  -- u16 sum of the 24 u16 words of secure data
local MON_SECURE     = 0x20  -- 48 bytes: four 12-byte substructs
local MON_LEVEL      = 0x54
local MON_MAIL       = 0x55  -- 0xFF = no mail
local MON_HP         = 0x56
local MON_MAX_HP     = 0x58

-- Secure data is normally encrypted with (personality XOR otId) and its four
-- substructs are shuffled by personality % 24. With personality = otId = 0
-- the key is 0 (plain text) and the order is Growth, Attacks, EVs, Misc, so:
local GROWTH_SPECIES = MON_SECURE + 0x00  -- u16
local ATTACKS_MOVES  = MON_SECURE + 0x0C  -- u16 moves[4]
local ATTACKS_PP     = MON_SECURE + 0x14  -- u8  pp[4]

-- Kiri's flag: TRAINER_FLAGS_START (0x500, include/constants/flags.h) +
-- TRAINER_TWINS_KIRI_JAN (487, include/constants/opponents.h). Flags are a
-- bit array in SaveBlock1, 8 per byte.
local KIRI_FLAG = 0x500 + 487

local SPECIES_PIKACHU = 25

local function party_slot(slot)
    return sym.addr("gPlayerParty") + slot * POKEMON_SIZE
end

local function callback2()
    return fw.read32(sym.addr("gMain") + 4)
end

-- Recompute a party slot's checksum after editing its (plain-text) secure data.
local function fix_checksum(base)
    local sum = 0
    for w = 0, 23 do
        sum = (sum + fw.read16(base + MON_SECURE + w * 2)) % 65536
    end
    emu:write16(base + MON_CHECKSUM, sum)
end

-- A level 5 Pikachu with 20 HP and all other stats 0. Zero personality and
-- trainer id keep its secure data unencrypted (see above).
local function write_pikachu(slot)
    local base = party_slot(slot)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + MON_FLAGS, 0x02)
    emu:write16(base + GROWTH_SPECIES, SPECIES_PIKACHU)
    emu:write8(base + MON_LEVEL, 5)
    emu:write8(base + MON_MAIL, 0xFF)
    emu:write16(base + MON_HP, 20)
    emu:write16(base + MON_MAX_HP, 20)
    fix_checksum(base)
end

local function write_moves(slot, m0, pp0, m1, pp1)
    local base = party_slot(slot)
    emu:write16(base + ATTACKS_MOVES + 0, m0)
    emu:write16(base + ATTACKS_MOVES + 2, m1)
    emu:write8(base + ATTACKS_PP + 0, pp0)
    emu:write8(base + ATTACKS_PP + 1, pp1)
    fix_checksum(base)
end

local function clear_kiri_flag()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local addr = sb1 + ADDR.SB1_FLAGS + (KIRI_FLAG // 8)
    local bit = 1 << (KIRI_FLAG % 8)
    emu:write8(addr, fw.read8(addr) & ~bit & 0xFF)
end

local function fail(msg)
    fw.log_error("[r14] " .. msg)
    return false
end

function r14.reach_move_selection()
    -- 1. Power-on to the overworld.
    local sel = cs.find_selection_press()
    if not sel then return fail("character select list never opened") end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.wait_frames(60)
    if not cs.is_player_active() then return fail("did not reach the overworld") end

    -- 2. Two Pikachu; the lead knows Water Gun and Vine Whip. Written before
    -- the battle, so the battle copies them into gBattleMons and the
    -- move-selection buffer like any real party.
    write_pikachu(0)
    write_moves(0, r14.MOVE_WATER_GUN, 25, r14.MOVE_VINE_WHIP, 25)
    write_pikachu(1)
    emu:write8(ADDR.gPlayerPartyCount, 2)

    -- 3. Stand below Kiri. warp_to_pos's map load places the player from
    -- SaveBlock1.pos (offsets 0 and 2), so set that too.
    clear_kiri_flag()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    emu:write16(sb1 + 0, r14.PLAYER_X)
    emu:write16(sb1 + 2, r14.PLAYER_Y)
    if not warp.warp_to_pos(r14.MAP_GROUP, r14.MAP_NUM, r14.PLAYER_X, r14.PLAYER_Y) then
        return fail("Route 14 did not load")
    end
    -- Kiri can only spot the player once the overworld loop runs again.
    if not fw.wait_until(function() return sym.is_func(callback2(), "CB2_Overworld") end,
            600, "CB2_Overworld after warp") then
        return fail("overworld loop never resumed on Route 14")
    end

    -- 4. A through Kiri's approach and dialogue. Stop as soon as the double
    -- battle is confirmed, so later presses can't skip past move selection.
    for _ = 1, 40 do
        fw.press("A")
        fw.wait_frames(30)
        if (fw.read32(ADDR.gBattleTypeFlags) & BATTLE_TYPE_DOUBLE) ~= 0 then break end
    end
    fw.wait_frames(60)
    local flags = fw.read32(ADDR.gBattleTypeFlags)
    if (flags & BATTLE_TYPE_TRAINER) == 0 or (flags & BATTLE_TYPE_DOUBLE) == 0 then
        return fail(string.format("no double trainer battle (gBattleTypeFlags=0x%08X)", flags))
    end
    fw.log("[r14] Kiri & Jan double battle started")

    -- 5. A through the intro and the action menu (FIGHT is preselected)
    -- until move selection opens.
    for i = 1, 120 do
        if fw.read8(ADDR.gMultiUsePlayerCursor) == 0xFF then
            fw.log(string.format("[r14] move selection open after %d presses", i - 1))
            return true
        end
        fw.press("A")
        fw.wait_frames(30)
    end
    return fail("move selection never opened")
end

return r14
