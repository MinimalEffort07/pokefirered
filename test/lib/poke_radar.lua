-------------------------------------------------------------------------------
-- poke_radar.lua
-------------------------------------------------------------------------------
-- Helper for reading/writing the PokeRadar struct inside SaveBlock1.
--
-- The PokeRadar shiny-chain state is embedded at SaveBlock1::unused_348C[0..]
-- (400 bytes of pre-existing slack inside SaveBlock1). The struct is 36 bytes:
--
--   offset  field               size  meaning
--   ------  ------------------  ----  -----------------------------------------
--   0x00    magic               u32   'PRDR' = 0x50524452 when initialized
--   0x04    chainCount          u16   0..40
--   0x06    chainSpecies        u16   SPECIES_NONE (0) when chain inactive
--   0x08    stepsUntilCharge    u8    0..50
--   0x09    charges             u8    0 or 1
--   0x0A    flags               u8    bit0 = patches active
--                                     bit1 = from-patch encounter latch
--   0x0B    chainMapGroup       u8    diagnostic only
--   0x0C    chainMapNum         u8    diagnostic only
--   0x0D    padding             u8
--   0x0E    patchX[0..3]        s16x4 8 bytes
--   0x16    patchY[0..3]        s16x4 8 bytes
--   0x1E    patchSpriteId[0..3] u8x4  4 bytes (MAX_SPRITES=64 means "none")
--   0x22    reserved            u8x2  2 bytes (aligns total to 0x24)
--
-- These offsets mirror include/poke_radar.h; update both together.
--
-- PLAYING WITH THE RADAR (issue #87). The second half of this file plays the
-- radar the way a person does, for the tests that check what the player
-- sees (test_poke_radar_no_freeze.lua and the ones after it):
--   * pr.give_party(): one strong, fast Pokemon of the player's own;
--   * pr.use_from_bag(), pr.register_in_bag(), pr.use_with_select(): the
--     ways to use the radar, pressing buttons through the menus, and
--     pr.oak_message_up(): Prof. Oak's "This isn't the time to use that!";
--   * pr.is_live_patch(id), pr.is_patch_tile(x, y), pr.assert_live_patches():
--     what a patch is;
--   * pr.assert_not_frozen(label, within, opts, elapsed): the player has
--     control (its two halves are pr.assert_back_on_field and
--     pr.assert_step, for a test that checks more in between);
--   * pr.assert_object_sprites(): every object still has its own sprite;
--   * pr.raise_encounter_rate(), pr.wait_for_battle(), pr.play_battle(action):
--     a wild battle, ended by RUN or by FIGHT;
--   * pr.reset(): a test's setup between scenarios.
-- Tiles there are map coordinates (data/maps/<Map>/map.json), as in
-- test/lib/walk.lua; the radar struct stores its patch tiles 7 higher
-- (MAP_OFFSET, the engine's margin around every map).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "framework.lua")
local ADDR = dofile(script_dir .. "addresses.lua")
local sym = dofile(script_dir .. "symbols.lua")
local mm = dofile(script_dir .. "mt_moon.lua")
local follower = dofile(script_dir .. "follower.lua")

local pr = {}

-- Struct offsets. The `unused_348C` field is misnamed — despite what the
-- comment in include/global.h says, MysteryGiftSave grew by 40 bytes at some
-- point and the slack region actually starts at 0x34B4. src/poke_radar.c
-- carries a STATIC_ASSERT that pins this to 0x34B4, so if a future refactor
-- breaks it the build will fail before the test runs.
pr.OFF_UNUSED_348C       = 0x34B4  -- inside SaveBlock1
pr.OFF_MAGIC             = 0x00
pr.OFF_CHAIN_COUNT       = 0x04
pr.OFF_CHAIN_SPECIES     = 0x06
pr.OFF_STEPS_UNTIL_CHG   = 0x08
pr.OFF_CHARGES           = 0x09
pr.OFF_FLAGS             = 0x0A
pr.OFF_CHAIN_MAP_GROUP   = 0x0B
pr.OFF_CHAIN_MAP_NUM     = 0x0C
pr.OFF_PATCH_X           = 0x0E  -- s16[4]
pr.OFF_PATCH_Y           = 0x16  -- s16[4]
pr.OFF_PATCH_SPRITE_ID   = 0x1E  -- u8[4]

-- Magic constant — must match POKE_RADAR_MAGIC in include/poke_radar.h.
pr.MAGIC                 = 0x50524452  -- 'PRDR'

-- Flag bits — must match POKE_RADAR_FLAG_* in the header.
pr.FLAG_PATCHES_ACTIVE       = 0x01
pr.FLAG_FROM_PATCH_ENCOUNTER = 0x02

-- Chain cap / recharge constants.
pr.MAX_CHAIN        = 40
pr.RECHARGE_STEPS   = 50
pr.PATCH_COUNT      = 4

-- Species / item constants.
pr.SPECIES_NONE     = 0
pr.ITEM_POKE_RADAR  = 375

-- Resolve the live base address of the PokeRadar struct. We always go
-- through gSaveBlock1Ptr because the game shifts SaveBlock1 to a
-- randomized offset during init (DMA-safety trick) and any cached
-- address captured before that shift is stale.
function pr.base()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    return sb1 + pr.OFF_UNUSED_348C
end

-- Field readers
function pr.get_magic()           return fw.read32(pr.base() + pr.OFF_MAGIC) end
function pr.get_chain_count()     return fw.read16(pr.base() + pr.OFF_CHAIN_COUNT) end
function pr.get_chain_species()   return fw.read16(pr.base() + pr.OFF_CHAIN_SPECIES) end
function pr.get_steps_to_charge() return fw.read8(pr.base() + pr.OFF_STEPS_UNTIL_CHG) end
function pr.get_charges()         return fw.read8(pr.base() + pr.OFF_CHARGES) end
function pr.get_flags()           return fw.read8(pr.base() + pr.OFF_FLAGS) end

-- Patch readers (i = 0..3)
function pr.get_patch_x(i)
    return fw.read16(pr.base() + pr.OFF_PATCH_X + i * 2)
end
function pr.get_patch_y(i)
    return fw.read16(pr.base() + pr.OFF_PATCH_Y + i * 2)
end
function pr.get_patch_sprite_id(i)
    return fw.read8(pr.base() + pr.OFF_PATCH_SPRITE_ID + i)
end

-- Field writers. Tests use these to construct arbitrary radar states
-- without having to walk/battle through the full in-game path.
function pr.set_magic(v)          emu:write32(pr.base() + pr.OFF_MAGIC, v) end
function pr.set_chain_count(v)    emu:write16(pr.base() + pr.OFF_CHAIN_COUNT, v) end
function pr.set_chain_species(v)  emu:write16(pr.base() + pr.OFF_CHAIN_SPECIES, v) end
function pr.set_steps_to_charge(v) emu:write8(pr.base() + pr.OFF_STEPS_UNTIL_CHG, v) end
function pr.set_charges(v)        emu:write8(pr.base() + pr.OFF_CHARGES, v) end
function pr.set_flags(v)          emu:write8(pr.base() + pr.OFF_FLAGS, v) end

-- Snapshot the whole struct as a Lua table for logging/asserting.
function pr.snapshot()
    return {
        magic             = pr.get_magic(),
        chainCount        = pr.get_chain_count(),
        chainSpecies      = pr.get_chain_species(),
        stepsUntilCharge  = pr.get_steps_to_charge(),
        charges           = pr.get_charges(),
        flags             = pr.get_flags(),
        patchSprites = {
            pr.get_patch_sprite_id(0),
            pr.get_patch_sprite_id(1),
            pr.get_patch_sprite_id(2),
            pr.get_patch_sprite_id(3),
        },
    }
end

function pr.log_snapshot(label)
    local s = pr.snapshot()
    fw.log(string.format(
        "[PR] %s: magic=0x%08X chain=%d species=%d steps=%d charges=%d flags=0x%02X sprites=%d,%d,%d,%d",
        label or "radar", s.magic, s.chainCount, s.chainSpecies,
        s.stepsUntilCharge, s.charges, s.flags,
        s.patchSprites[1], s.patchSprites[2], s.patchSprites[3], s.patchSprites[4]))
end

-------------------------------------------------------------------------------
-- Bag search: scan the key items pocket for ITEM_POKE_RADAR.
-- The key items pocket is at SaveBlock1 + 0x3b8, 30 slots of 4 bytes each.
-- ItemSlot.itemId is plaintext u16 at offset 0; quantity (offset 2) is
-- encrypted but we don't need it for presence checking.
-------------------------------------------------------------------------------

pr.BAG_KEY_ITEMS_OFFSET = 0x3b8
pr.BAG_KEY_ITEMS_COUNT  = 30
pr.ITEM_SLOT_SIZE       = 4

function pr.key_items_has(itemId)
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local pocket = sb1 + pr.BAG_KEY_ITEMS_OFFSET
    for i = 0, pr.BAG_KEY_ITEMS_COUNT - 1 do
        local slot = pocket + i * pr.ITEM_SLOT_SIZE
        local id = fw.read16(slot)
        if id == itemId then
            return true, i
        end
    end
    return false, nil
end

-------------------------------------------------------------------------------
-- RNG manipulation: the game's Random() function uses gRngValue at IWRAM
-- 0x03005010 (confirmed via pokefirered.map). Writing a known value lets
-- a test produce deterministic encounter outcomes without having to
-- pre-shuffle the real game state.
-------------------------------------------------------------------------------

pr.ADDR_gRngValue = 0x03005010

function pr.get_rng()         return fw.read32(pr.ADDR_gRngValue) end
function pr.set_rng(seed)     emu:write32(pr.ADDR_gRngValue, seed) end

-------------------------------------------------------------------------------
-- Small helpers for struct-level assertions
-------------------------------------------------------------------------------

-- Assert that the struct is in the "freshly initialized" state after a
-- new-game flow: magic installed, no chain, one charge, no patches.
function pr.assert_fresh_init()
    fw.assert_eq(pr.get_magic(), pr.MAGIC,
        "PokeRadar magic installed (POKE_RADAR_MAGIC)")
    fw.assert_eq(pr.get_chain_count(), 0,
        "PokeRadar chainCount starts at 0")
    fw.assert_eq(pr.get_chain_species(), pr.SPECIES_NONE,
        "PokeRadar chainSpecies starts as SPECIES_NONE")
    fw.assert_eq(pr.get_charges(), 1,
        "PokeRadar has 1 charge on fresh init")
    fw.assert_eq(pr.get_steps_to_charge(), 0,
        "PokeRadar stepsUntilCharge starts at 0")
    fw.assert_eq(pr.get_flags(), 0,
        "PokeRadar flags start at 0")
end

-------------------------------------------------------------------------------
-- PLAYING WITH THE RADAR (see the header)
-------------------------------------------------------------------------------

local MAP_OFFSET = mm.MAP_OFFSET

-- Constants read from the game's own headers.
pr.MB_TALL_GRASS = mm.read_defines("include/constants/metatile_behaviors.h").MB_TALL_GRASS
pr.FLDEFF_UNUSED_GRASS = mm.read_defines("include/constants/field_effects.h").FLDEFF_UNUSED_GRASS
local SPECIES_PIKACHU = mm.read_defines("include/constants/species.h").SPECIES_PIKACHU
local MOVE_THUNDERBOLT = mm.read_defines("include/constants/moves.h").MOVE_THUNDERBOLT
local ITEM_POKE_BALL = mm.read_defines("include/constants/items.h").ITEM_POKE_BALL
local GLOBAL = mm.read_defines("include/constants/global.h")
local REGISTERED_ITEMS_MAX = mm.read_defines("include/global.h").REGISTERED_ITEMS_MAX
local LOCALID_FOLLOWER = mm.read_defines("include/constants/event_objects.h").LOCALID_FOLLOWER
local BATTLE = mm.read_defines("include/constants/battle.h")
pr.B_OUTCOME_WON = BATTLE.B_OUTCOME_WON
pr.B_OUTCOME_RAN = BATTLE.B_OUTCOME_RAN

-- FLAG_SYS_POKEMON_GET (SYS_FLAGS + 0x28 = 0x828, include/constants/
-- flags.h): set once the player has a Pokemon; it puts POKEMON in the
-- START menu.
local FLAG_SYS_POKEMON_GET = 0x828

local function s16(v)
    if v >= 0x8000 then return v - 0x10000 end
    return v
end

local function callback2()
    return fw.read32(sym.addr("gMain") + 4)
end

-- The game's main loop (gMain.callback2) is the field's.
function pr.on_field()
    return sym.is_func(callback2(), "CB2_Overworld")
end

-- The main loop is the battle's.
function pr.in_battle()
    return sym.is_func(callback2(), "BattleMainCB2")
end

-- sLockFieldControls (src/script.c): 1 while a menu, a message or a script
-- holds the player still, 0 when the player has control.
function pr.field_controls_locked()
    return fw.read8(sym.addr("sLockFieldControls")) ~= 0
end

-- gPlayerAvatar.tileTransitionState (offset 3) is 0 when the player stands
-- on a tile, not in the middle of a step.
local function standing_still()
    return fw.read8(sym.addr("gPlayerAvatar") + 3) == 0
end

-- The player could take a step or open a menu right now.
function pr.player_ready()
    return pr.on_field() and mm.player_has_control() and standing_still()
end

-------------------------------------------------------------------------------
-- Patches
-------------------------------------------------------------------------------

-- struct Sprite (include/sprite.h), 0x44 bytes: s16 data[8] starts at 0x2E,
-- and bit 0 of the byte at 0x3E is inUse.
local SPRITE_OFF_DATA0 = 0x2E

local function sprite_addr(id)
    return sym.addr("gSprites") + id * ADDR.SPRITE_STRUCT_SIZE
end

function pr.sprite_in_use(id)
    if id >= ADDR.MAX_SPRITES then return false end
    return (fw.read8(sprite_addr(id) + ADDR.SPRITE_OFF_FLAGS_3E) & 1) ~= 0
end

local function sprite_data0(id)
    return s16(fw.read16(sprite_addr(id) + SPRITE_OFF_DATA0))
end

-- Is sprite `id` a live shaking-grass patch? The radar keeps each patch's
-- sprite id in its struct, but an id alone proves nothing: before the radar
-- runs those slots hold 0 (a live sprite, usually the player's) or whatever
-- a test wrote. FldEff_UnusedGrass marks its sprites by storing the field
-- effect number (FLDEFF_UNUSED_GRASS) in data[0], so check the sprite is in
-- use and marked so.
function pr.is_live_patch(id)
    return pr.sprite_in_use(id) and sprite_data0(id) == pr.FLDEFF_UNUSED_GRASS
end

function pr.patches_active()
    return (pr.get_flags() & pr.FLAG_PATCHES_ACTIVE) ~= 0
end

-- Patch i's tile (i = 0..3), in map coordinates.
function pr.patch_tile(i)
    return s16(pr.get_patch_x(i)) - MAP_OFFSET, s16(pr.get_patch_y(i)) - MAP_OFFSET
end

-- Is map tile (x, y) one of the four patch tiles?
function pr.is_patch_tile(x, y)
    for i = 0, pr.PATCH_COUNT - 1 do
        local px, py = pr.patch_tile(i)
        if px == x and py == y then return true end
    end
    return false
end

-- Assert the radar is shaking four patches: PATCHES_ACTIVE is set and every
-- stored sprite id is a live patch.
function pr.assert_live_patches(label)
    fw.assert_true(pr.patches_active(), label .. ": PATCHES_ACTIVE is 1")
    for i = 0, pr.PATCH_COUNT - 1 do
        local id = pr.get_patch_sprite_id(i)
        local x, y = pr.patch_tile(i)
        fw.assert_true(pr.is_live_patch(id), string.format(
            "%s: patch %d at (%d,%d) is a live shaking-grass sprite (id %d)", label, i, x, y, id))
    end
end

-- A test's setup between scenarios: no patches and no chain, and `charges`
-- charges (default 1) with no recharge running, as if the player had walked
-- the 50 steps since the last use. It only rewrites the radar's struct, so
-- call it before a warp: the warp's map load frees every sprite, the
-- patches' with them.
function pr.reset(charges)
    pr.set_chain_count(0)
    pr.set_chain_species(pr.SPECIES_NONE)
    pr.set_flags(0)
    pr.set_charges(charges or 1)
    pr.set_steps_to_charge(0)
    for i = 0, pr.PATCH_COUNT - 1 do
        emu:write8(pr.base() + pr.OFF_PATCH_SPRITE_ID + i, ADDR.MAX_SPRITES)
        emu:write16(pr.base() + pr.OFF_PATCH_X + i * 2, 0)
        emu:write16(pr.base() + pr.OFF_PATCH_Y + i * 2, 0)
    end
end

-------------------------------------------------------------------------------
-- The party: one strong, fast Pokemon of the player's own
-------------------------------------------------------------------------------

-- struct Pokemon (include/pokemon.h), 100 bytes. The first 0x50 bytes are
-- the struct BoxPokemon that the PC stores too.
local MON_SIZE        = 100
local MON_PERSONALITY = 0x00  -- u32
local MON_OT_ID       = 0x04  -- u32: the original trainer's id
local MON_NICKNAME    = 0x08  -- u8[10]
local MON_LANGUAGE    = 0x12
local MON_FLAGS       = 0x13  -- bit 1 = hasSpecies (the slot holds a Pokemon)
local MON_OT_NAME     = 0x14  -- u8[7]
local MON_CHECKSUM    = 0x1C  -- u16: sum of the 24 u16s of the secure data
local MON_SECURE      = 0x20  -- 48 bytes: four 12-byte blocks, encrypted
local MON_LEVEL       = 0x54
local MON_MAIL        = 0x55  -- 0xFF = no mail
local MON_HP          = 0x56
local MON_MAX_HP      = 0x58
local MON_STATS       = 0x5A  -- u16 attack, defense, speed, sp. attack, sp. defense

-- SaveBlock2 (include/global.h): playerName u8[8] at 0x00, playerTrainerId
-- u8[4] at 0x0A.
local SB2_TRAINER_ID = 0x0A

-- Put a level-100 PIKACHU (it can WALK as the follower) in party slot 0 as
-- the only Pokemon, and set FLAG_SYS_POKEMON_GET as receiving one does. It
-- knows THUNDERBOLT and every stat is 500, so one attack knocks out any
-- Route 1 Pokemon (levels 2-5) and RUN always works (a wild Pokemon slower
-- than yours can't stop you running). Level 100 also means a Repel keeps
-- every wild Pokemon away, and a win brings no EXP and no level-up screens.
--
-- It is the player's own: its original trainer is the player (id and name),
-- because a level-100 Pokemon from another trainer disobeys a player
-- without the EARTH BADGE (IsMonDisobedient). The game encrypts the 48
-- bytes of secure data by XOR-ing each u32 with (personality XOR OT id) and
-- orders its four 12-byte blocks by personality % 24. Personality 0 gives
-- the order Growth, Attacks, EVs, Misc and the key "OT id".
function pr.give_party()
    local base = sym.addr("gPlayerParty")
    local sb2 = fw.read32(sym.addr("gSaveBlock2Ptr"))
    local ot_id = 0
    for i = 0, 3 do
        ot_id = ot_id | (fw.read8(sb2 + SB2_TRAINER_ID + i) << (8 * i))
    end

    for i = 0, MON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write32(base + MON_PERSONALITY, 0)
    emu:write32(base + MON_OT_ID, ot_id)
    -- Nickname: the species name, from the ROM's table of 11-byte names.
    local name = sym.addr("gSpeciesNames") + SPECIES_PIKACHU * 11
    for i = 0, 9 do emu:write8(base + MON_NICKNAME + i, fw.read8(name + i)) end
    emu:write8(base + MON_LANGUAGE, GLOBAL.LANGUAGE_ENGLISH)
    emu:write8(base + MON_FLAGS, 0x02)
    for i = 0, 6 do emu:write8(base + MON_OT_NAME + i, fw.read8(sb2 + i)) end

    -- The secure data in the clear, as 48 bytes.
    local plain = {}
    for i = 0, 47 do plain[i] = 0 end
    local function put16(off, v) plain[off] = v & 0xFF; plain[off + 1] = (v >> 8) & 0xFF end
    local function put32(off, v) put16(off, v & 0xFFFF); put16(off + 2, (v >> 16) & 0xFFFF) end
    put16(0x00, SPECIES_PIKACHU)      -- Growth: species
    put32(0x04, 100 * 100 * 100)      -- Growth: experience, level 100 (PIKACHU grows "medium fast", n^3)
    put16(0x0C, MOVE_THUNDERBOLT)     -- Attacks: moves[0]
    plain[0x14] = 15                  -- Attacks: pp[0]
    put16(0x24 + 2, ITEM_POKE_BALL << 11)  -- Misc: caught in a POKE BALL (bits 11-14)

    local sum = 0
    for i = 0, 47, 2 do sum = (sum + plain[i] + (plain[i + 1] << 8)) & 0xFFFF end
    emu:write16(base + MON_CHECKSUM, sum)
    for i = 0, 47, 4 do
        local word = plain[i] | (plain[i + 1] << 8) | (plain[i + 2] << 16) | (plain[i + 3] << 24)
        emu:write32(base + MON_SECURE + i, word ~ ot_id)
    end

    emu:write8(base + MON_LEVEL, 100)
    emu:write8(base + MON_MAIL, 0xFF)
    emu:write16(base + MON_HP, 300)
    emu:write16(base + MON_MAX_HP, 300)
    for i = 0, 4 do emu:write16(base + MON_STATS + i * 2, 500) end
    emu:write8(sym.addr("gPlayerPartyCount"), 1)
    mm.set_flag(FLAG_SYS_POKEMON_GET)
end

-------------------------------------------------------------------------------
-- Menus
-------------------------------------------------------------------------------

-- struct Task (include/task.h), 0x28 bytes: TaskFunc func at 0x00, bool8
-- isActive at 0x04, s16 data[16] at 0x08. gTasks has 16. Menus keep their
-- state in data[], so tests read pr.TASK_DATA + 2 * i for data[i].
local TASK_SIZE, NUM_TASKS, TASK_DATA = 0x28, 16, 0x08
pr.TASK_DATA = TASK_DATA

-- The address of an active task running function `name`, or nil. Menus
-- are tasks, so this says which menu is waiting for input. A stored
-- function pointer has bit 0 set (the Thumb-mode flag, see symbols.lua),
-- which `& ~1` clears before comparing it with the symbol's address.
function pr.task_running(name)
    local tasks = sym.addr("gTasks")
    local func = sym.addr(name)
    for i = 0, NUM_TASKS - 1 do
        local t = tasks + i * TASK_SIZE
        if fw.read8(t + 4) ~= 0 and (fw.read32(t) & ~1) == func then return t end
    end
    return nil
end

local function wait_task(name, timeout, what)
    return fw.wait_until(function() return pr.task_running(name) ~= nil end,
        timeout or 300, what or name)
end

-- The START menu's entries (an enum in src/start_menu.c).
pr.STARTMENU_POKEMON = 1
pr.STARTMENU_BAG     = 2

-- Open the START menu and choose `entry` (pr.STARTMENU_*). Returns true
-- once A was pressed on it.
function pr.choose_start_menu(entry, label)
    fw.press("START")
    -- The menu takes input once Task_StartMenuHandleInput reaches its
    -- second step (data[0] = 1) with StartCB_HandleInput as the callback.
    -- (sStartMenuCallback alone isn't enough: it keeps StartCB_HandleInput
    -- from the last time the menu was open, and a key pressed the frame
    -- before that step is lost.)
    local open = fw.wait_until(function()
        local t = pr.task_running("Task_StartMenuHandleInput")
        return t ~= nil and fw.read16(t + TASK_DATA) == 1
            and sym.is_func(fw.read32(sym.addr("sStartMenuCallback")), "StartCB_HandleInput")
    end, 120, "START menu open")
    if not fw.assert_true(open, label .. ": the START menu opened") then return false end
    -- The list remembers its cursor from the last entry chosen, so move
    -- from there (as mm.save_game does).
    local order = sym.addr("sStartMenuOrder")
    local target = nil
    for i = 0, fw.read8(sym.addr("sNumStartMenuItems")) - 1 do
        if fw.read8(order + i) == entry then target = i end
    end
    if not fw.assert_true(target ~= nil, string.format(
            "%s: START menu entry %d is in the menu", label, entry)) then
        return false
    end
    local current = fw.read16(sym.addr("sListCursorPos")) + fw.read16(sym.addr("sListScrollOffset"))
    fw.log(string.format("[%s] START menu: cursor on row %d, entry %d is row %d", label, current, entry, target))
    for _ = 1, math.abs(target - current) do
        fw.press(target > current and "DOWN" or "UP")
        fw.wait_frames(6)
    end
    fw.press("A")
    return true
end

-- struct BagStruct gBagMenuState (include/item_menu.h): u16 pocket at 0x06
-- (0 ITEMS, 1 KEY ITEMS, 2 POKE BALLS).
local BAG_POCKET = 0x06
local POCKET_KEY_ITEMS = GLOBAL.POCKET_KEY_ITEMS - 1

-- From the START menu: open the BAG, go to KEY ITEMS and open the POKe
-- RADAR's menu (USE / REGISTER / CANCEL, the cursor on USE). Asserts each
-- step; returns true when the menu is open.
local function open_radar_menu(label)
    if not pr.choose_start_menu(pr.STARTMENU_BAG, label) then return false end
    if not fw.assert_true(wait_task("Task_BagMenu_HandleInput", 300, "bag open"),
            label .. ": the bag opened") then
        return false
    end
    -- The bag ignores input while it fades in, so press until the pocket
    -- changes. The radar is the new game's only key item, so it is on the
    -- first row, under the cursor (checked below by the item A picks).
    local pocket = sym.addr("gBagMenuState") + BAG_POCKET
    for _ = 1, 10 do
        if fw.read16(pocket) == POCKET_KEY_ITEMS
            and pr.task_running("Task_BagMenu_HandleInput") then break end
        fw.press(fw.read16(pocket) < POCKET_KEY_ITEMS and "RIGHT" or "LEFT")
        fw.wait_frames(30)
    end
    if not fw.assert_eq(fw.read16(pocket), POCKET_KEY_ITEMS, label .. ": the KEY ITEMS pocket is open") then
        return false
    end
    for _ = 1, 10 do
        if pr.task_running("Task_FieldItemContextMenuHandleInput") then break end
        fw.press("A")
        fw.wait_frames(20)
    end
    local open = pr.task_running("Task_FieldItemContextMenuHandleInput") ~= nil
    fw.assert_true(open, label .. ": the item's menu (USE / REGISTER / CANCEL) opened")
    fw.assert_eq(fw.read16(sym.addr("gSpecialVar_ItemId")), pr.ITEM_POKE_RADAR,
        label .. ": the item is the POKe RADAR")
    return open
end

-- START > BAG > KEY ITEMS > POKe RADAR > USE. Returns true right after the
-- A press on USE (the action the "not frozen" check counts from), and the
-- frames since that press: 2 (fw.press holds A for a frame, then lets go
-- for one).
function pr.use_from_bag(label)
    if not open_radar_menu(label) then return false end
    fw.wait_frames(10)
    fw.press("A")   -- USE, under the cursor
    return true, 2
end

-- SaveBlock1.registeredItems (u16[REGISTERED_ITEMS_MAX] at 0x296,
-- include/global.h): the items in the SELECT menu.
local SB1_REGISTERED_ITEMS = 0x296

-- The slot the radar is registered in, or nil.
function pr.registered_slot()
    local items = fw.read32(ADDR.gSaveBlock1Ptr) + SB1_REGISTERED_ITEMS
    for i = 0, REGISTERED_ITEMS_MAX - 1 do
        if fw.read16(items + i * 2) == pr.ITEM_POKE_RADAR then return i end
    end
    return nil
end

-- START > BAG > KEY ITEMS > POKe RADAR > REGISTER, then B out of the bag
-- and B out of the START menu it returns to. Returns true once the radar
-- is registered and the player has control again.
function pr.register_in_bag(label)
    if not open_radar_menu(label) then return false end
    fw.wait_frames(10)
    fw.press("DOWN")   -- USE -> REGISTER
    fw.wait_frames(6)
    fw.press("A")
    fw.wait_frames(30)
    if not fw.assert_true(pr.registered_slot() ~= nil,
            label .. ": REGISTER put the POKe RADAR in the SELECT menu") then
        return false
    end
    -- B closes the bag, back to the START menu; B again closes that.
    wait_task("Task_BagMenu_HandleInput", 120, "back in the bag's list")
    fw.wait_frames(10)
    fw.press("B")
    wait_task("Task_StartMenuHandleInput", 600, "START menu back")
    fw.wait_frames(20)
    fw.press("B")
    return fw.assert_true(fw.wait_until(pr.player_ready, 300, "control after the bag"),
        label .. ": back on the field with control after registering")
end

-- The SELECT menu (src/quick_select_menu.c). sQS: u8 numEntries at 3 (the
-- entries in this opening's list), u16 cursorPos at 4 and u16 itemsAbove
-- at 6 (where its cursor opens). Each sListItems entry is a struct
-- ListMenuItem (include/list_menu.h), 8 bytes: a label pointer and a u32
-- index, 100 + n for registeredItems[n].
local QS_NUM_ENTRIES, QS_CURSOR_POS, QS_ITEMS_ABOVE = 3, 4, 6
local QS_ID_ITEM_BASE, QS_STATE_INPUT = 100, 1

-- Press SELECT and choose the POKe RADAR in the SELECT menu. Returns true
-- once the A press on it has closed the menu, and the frames since that
-- press.
function pr.use_with_select(label)
    if not fw.assert_true(fw.wait_until(pr.player_ready, 300, "ready for SELECT"),
            label .. ": the player can press SELECT") then
        return false
    end
    local slot = pr.registered_slot()
    if not fw.assert_true(slot ~= nil, label .. ": the POKe RADAR is registered") then return false end
    fw.press("SELECT")
    local open = fw.wait_until(function()
        local t = pr.task_running("Task_QuickSelectMenu")
        return t ~= nil and fw.read16(t + TASK_DATA) == QS_STATE_INPUT
    end, 120, "SELECT menu open")
    if not fw.assert_true(open, label .. ": the SELECT menu opened") then return false end
    local target = nil
    for i = 0, fw.read8(sym.addr("sQS") + QS_NUM_ENTRIES) - 1 do
        if fw.read32(sym.addr("sListItems") + i * 8 + 4) == QS_ID_ITEM_BASE + slot then
            target = i
            break
        end
    end
    if not fw.assert_true(target ~= nil, label .. ": the POKe RADAR is in the SELECT menu") then
        return false
    end
    local qs = sym.addr("sQS")
    local current = fw.read16(qs + QS_CURSOR_POS) + fw.read16(qs + QS_ITEMS_ABOVE)
    fw.wait_frames(10)
    for _ = 1, math.abs(target - current) do
        fw.press(target > current and "DOWN" or "UP")
        fw.wait_frames(6)
    end
    -- An A on the frame the menu opens is lost, so press until the menu
    -- has closed (its task ends when an entry is chosen).
    for _ = 1, 5 do
        fw.press("A")
        for since = 2, 12 do
            if pr.task_running("Task_QuickSelectMenu") == nil then return true, since end
            fw.yield()
        end
    end
    return fw.assert_true(false, label .. ": A chose the POKe RADAR in the SELECT menu")
end

-- Prof. Oak's "This isn't the time to use that!" is on screen: a field
-- message is printing or waiting for A (Task_ContinueTaskAfterMessagePrints)
-- and the text it was given (gStringVar4) starts with the ROM's
-- gText_OakForbidsUseOfItemHere ("OAK: {PLAYER}!..."), compared up to its
-- first placeholder (0xFD, here {PLAYER}, which the game replaces with the
-- player's name) or its end (0xFF, EOS: the game's strings end with 0xFF,
-- not 0).
function pr.oak_message_up()
    if pr.task_running("Task_ContinueTaskAfterMessagePrints") == nil then return false end
    local text = sym.addr("gText_OakForbidsUseOfItemHere")
    local shown = sym.addr("gStringVar4")
    for i = 0, 31 do
        local c = fw.read8(text + i)
        if c == 0xFD or c == 0xFF then return i > 0 end
        if fw.read8(shown + i) ~= c then return false end
    end
    return true
end

-------------------------------------------------------------------------------
-- "Not frozen" (the design on issue #87)
-------------------------------------------------------------------------------

local DIRS = {
    { key = "DOWN",  dx = 0,  dy = 1 },
    { key = "RIGHT", dx = 1,  dy = 0 },
    { key = "UP",    dx = 0,  dy = -1 },
    { key = "LEFT",  dx = -1, dy = 0 },
}

-- A tile next to the player they can walk onto: not solid, not a warp, not
-- water, no object standing on it or stepping off it, and never a patch
-- tile (stepping into a patch is a radar action of its own). With `grass`,
-- only tall grass: a step that keeps the patches (stepping off the grass
-- clears them). Returns x, y and the D-pad key, or nil.
function pr.free_neighbour(grass)
    local px, py = mm.player_tile()
    if px == nil then return nil end
    local snap = follower.map_snapshot()
    local taken = {}
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player then
            taken[mm.key(o.x, o.y)] = true
            taken[mm.key(o.prev_x, o.prev_y)] = true
        end
    end
    for _, d in ipairs(DIRS) do
        local x, y = px + d.dx, py + d.dy
        if not snap.solid(x, y) and not snap.warp(x, y) and not snap.water(x, y)
            and not taken[mm.key(x, y)] and not pr.is_patch_tile(x, y)
            and (not grass or snap.behavior(x, y) == pr.MB_TALL_GRASS) then
            return x, y, d.key
        end
    end
    return nil
end

-- The player is not frozen: within `within` frames of the action that just
-- finished (a menu choice, closing a message, the end of a battle)
--   1. the game's main loop is the field's (gMain.callback2 = CB2_Overworld)
--      and 2. the field controls are unlocked (sLockFieldControls = 0);
-- then 3. holding a direction toward a free tile (pr.free_neighbour, with
-- opts.grass) moves the player onto it within 60 frames. Asserts all
-- three. `elapsed`: see pr.assert_back_on_field. Returns true when all
-- hold, and the new tile.
function pr.assert_not_frozen(label, within, opts, elapsed)
    local back = pr.assert_back_on_field(label, within, elapsed)
    local stepped, x, y = pr.assert_step(label, opts)
    return back and stepped, x, y
end

-- Checks 1 and 2 of pr.assert_not_frozen: wait for both until `within`
-- frames after the action, then assert each. `elapsed` (default 0) is the
-- frames the caller has already let pass since the action.
function pr.assert_back_on_field(label, within, elapsed)
    elapsed = elapsed or 0
    local waited = nil
    for i = elapsed, within do
        if pr.on_field() and not pr.field_controls_locked() then
            waited = i
            break
        end
        if i < within then fw.yield() end
    end
    local why = waited and string.format(" (after %d frames)", waited) or ""
    local ok = fw.assert_true(pr.on_field(), string.format(
        "%s: the main loop is the field's (CB2_Overworld) within %d frames%s", label, within, why))
    ok = fw.assert_eq(fw.read8(sym.addr("sLockFieldControls")), 0, string.format(
        "%s: the field controls are unlocked (sLockFieldControls) within %d frames%s",
        label, within, why)) and ok
    return ok
end

-- Hold the D-pad toward a free tile next to the player (pr.free_neighbour,
-- with `grass`) until the player's tile changes, for up to 60 frames, then
-- let go and wait up to 60 more frames for the step to finish. Returns a
-- table: { x, y, key } (the target tile and the key held; x is nil if no
-- tile next to the player was free), { from_x, from_y } (the player's tile
-- before), and `moved`, the frames it took to move onto the target (nil if
-- the player didn't).
function pr.try_step(grass)
    local x, y, key = pr.free_neighbour(grass)
    local px, py = mm.player_tile()
    local step = { x = x, y = y, key = key, from_x = px, from_y = py }
    if x == nil then return step end
    local idx = fw.KEY[key]
    emu:addKey(idx)
    for i = 1, 60 do
        fw.yield()
        local nx, ny = mm.player_tile()
        if nx ~= px or ny ~= py then
            if nx == x and ny == y then step.moved = i end
            break
        end
    end
    emu:clearKey(idx)
    for _ = 1, 60 do
        if standing_still() then break end
        fw.yield()
    end
    return step
end

-- Check 3 of pr.assert_not_frozen: pr.try_step, asserting there was a free
-- tile (with opts.grass, tall grass that isn't a patch) and the player
-- stepped onto it within 60 frames. Returns true if they did, and the tile.
function pr.assert_step(label, opts)
    opts = opts or {}
    local step = pr.try_step(opts.grass)
    if not fw.assert_true(step.x ~= nil, string.format("%s: a free %stile next to the player",
            label, opts.grass and "tall-grass " or "")) then
        return false
    end
    local ok = fw.assert_true(step.moved ~= nil, string.format(
        "%s: holding %s moves the player from (%d,%d) onto %s(%d,%d) within 60 frames%s",
        label, step.key, step.from_x, step.from_y, opts.grass and "the tall grass at " or "",
        step.x, step.y, step.moved and string.format(" (after %d)", step.moved) or ""))
    return ok, step.x, step.y
end

-------------------------------------------------------------------------------
-- Object sprites
-------------------------------------------------------------------------------

-- Is object event `index`'s sprite in use and still its own? When the game
-- gives an object its sprite it stores the object's index in the sprite's
-- data[0] (src/event_object_movement.c), so a sprite slot taken over by
-- something else fails this. Returns the check and the sprite id.
function pr.object_sprite_ok(index)
    local oe = sym.addr("gObjectEvents") + index * ADDR.OBJECT_EVENT_SIZE
    local id = fw.read8(oe + ADDR.OE_SPRITE_ID)
    return pr.sprite_in_use(id) and sprite_data0(id) == index, id
end

-- Assert every active object's sprite (the player's, the follower's, the
-- NPCs') is in use and linked to its object.
function pr.assert_object_sprites(label)
    local objects = mm.live_objects()
    fw.assert_true(#objects > 0, label .. ": there are active objects")
    for _, o in ipairs(objects) do
        local ok, id = pr.object_sprite_ok(o.index)
        local who = o.is_player and ", the player" or (o.local_id == LOCALID_FOLLOWER and ", the follower" or "")
        fw.assert_true(ok, string.format(
            "%s: object %d's sprite (id %d%s) is in use and linked to it", label, o.index, id, who))
    end
end

-------------------------------------------------------------------------------
-- Wild battles
-------------------------------------------------------------------------------

-- Make the next step in grass start a wild battle, as test_shiny_chaining's
-- Test E does: stepsSinceLastEncounter at 255 skips the cool-down after the
-- last battle, and encounterRateBuff at 15800 lifts the encounter rate to
-- the cap (1600 of 1600, saturated: 15800 * 16 / 200 = 1264 on top of
-- Route 1's base rate). Only without a Repel (VAR_REPEL_STEP_COUNT = 0).
function pr.raise_encounter_rate()
    emu:write16(ADDR.sWildEncounterData + ADDR.WED_ENCOUNTER_RATE_BUFF, 15800)
    emu:write8(ADDR.sWildEncounterData + ADDR.WED_STEPS_SINCE_LAST, 255)
end

local VAR_REPEL_STEP_COUNT = mm.read_defines("include/constants/vars.h").VAR_REPEL_STEP_COUNT

-- Start a Repel of `steps` steps (0 ends it). With the level-100 party of
-- pr.give_party() it keeps every wild Pokemon away.
function pr.set_repel(steps)
    mm.set_var(VAR_REPEL_STEP_COUNT, steps)
end

-- Wait up to `timeout` frames for a wild battle to start (gMain.callback2
-- becomes BattleMainCB2). Returns true if one did.
function pr.wait_for_battle(timeout)
    return fw.wait_until(pr.in_battle, timeout or 300, "wild battle")
end

-- The player's action menu is up: battler 0's controller function
-- (gBattlerControllerFuncs[0]) is battle_controller_player.c's
-- HandleInputChooseAction. Four controllers (the player's, the old man's,
-- the POKe DUDE's, the Safari Zone's) each have a static of that name; the
-- player's is the one just below HandleInputChooseMove, the next global
-- function in the same file.
local function player_choose_action_addr()
    local below = sym.addr("HandleInputChooseMove")
    local best = nil
    for _, a in ipairs(sym.find_all("HandleInputChooseAction")) do
        if a < below and (best == nil or a > best) then best = a end
    end
    return best
end

-- Play the wild battle on screen as a player does: A through its messages,
-- then on the action menu RUN (action "RUN") or FIGHT and the first move
-- (action "FIGHT"), until the battle is decided (gBattleOutcome, 0 while it
-- runs, is set). Then A through the closing messages until the battle's
-- main loop ends. Returns the outcome (0 if undecided after 3600 frames)
-- and the frames from the decision to now.
--
-- The action menu is a 2x2 grid, gActionSelectionCursor[0]: 0 FIGHT,
-- 1 BAG, 2 POKEMON, 3 RUN, so bit 0 is the column and bit 1 the row.
function pr.play_battle(action)
    local target = action == "RUN" and 3 or 0
    local choose_action = player_choose_action_addr()
    fw.assert_true(choose_action ~= nil,
        "battle_controller_player.c's HandleInputChooseAction is in the symbol table")
    local funcs = sym.addr("gBattlerControllerFuncs")
    local cursor = sym.addr("gActionSelectionCursor")
    local outcome_addr = sym.addr("gBattleOutcome")
    -- gBattleOutcome still holds the last battle's outcome until
    -- BeginBattleIntro, the battle's first step on BattleMainCB2's first
    -- frame, clears it.
    fw.wait_until(function()
        return not sym.is_func(fw.read32(sym.addr("gBattleMainFunc")), "BeginBattleIntro")
    end, 60, "battle set up")

    -- Every frame goes through tick(), which notes the frame the battle is
    -- decided on.
    local frames, decided_at = 0, nil
    local function tick()
        fw.yield()
        frames = frames + 1
        if decided_at == nil and fw.read8(outcome_addr) ~= 0 then decided_at = frames end
    end
    local function press(key, wait)
        local idx = fw.KEY[key]
        emu:addKey(idx)
        tick()
        emu:clearKey(idx)
        for _ = 0, wait do tick() end
    end
    while decided_at == nil and frames < 3600 do
        local f = fw.read32(funcs) & ~1   -- clear the Thumb bit (see pr.task_running)
        if f == choose_action then
            local c = fw.read8(cursor)
            if (c & 1) ~= (target & 1) then
                press((target & 1) ~= 0 and "RIGHT" or "LEFT", 4)
            elseif (c & 2) ~= (target & 2) then
                press((target & 2) ~= 0 and "DOWN" or "UP", 4)
            else
                fw.log(string.format("[battle] %s chosen on the action menu", action))
                press("A", 4)
            end
        elseif f == sym.addr("HandleInputChooseMove") then
            press("A", 4)   -- the move under the cursor: the first, the only one
        else
            press("A", 8)   -- the next message
        end
    end
    if decided_at == nil then return 0, 0 end
    while pr.in_battle() and frames - decided_at < 600 do
        press("A", 8)
    end
    return fw.read8(outcome_addr), frames - decided_at
end

return pr
