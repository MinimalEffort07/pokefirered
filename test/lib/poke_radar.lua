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
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "framework.lua")
local ADDR = dofile(script_dir .. "addresses.lua")

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

return pr
