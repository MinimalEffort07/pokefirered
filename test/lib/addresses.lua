-------------------------------------------------------------------------------
-- GBA Memory Addresses for pokefirered
-------------------------------------------------------------------------------
--
-- This file maps important game variables to their memory addresses.
-- These addresses are extracted from the compiled ROM's symbol map
-- (pokefirered.map) and must be updated if the ROM is rebuilt and the
-- memory layout changes.
--
-- HOW GBA MEMORY WORKS:
--
-- The GBA has no operating system. All game data lives at fixed memory
-- addresses. The CPU accesses hardware, graphics, and game state through
-- "memory-mapped I/O" — reading/writing specific addresses.
--
-- Key memory regions:
--   0x02000000 - 0x0203FFFF : EWRAM (256KB)
--     "External Work RAM" — the main RAM for game data. This is where
--     most game variables live: the player's position, sprite data,
--     save blocks, map state, etc.
--
--   0x03000000 - 0x03007FFF : IWRAM (32KB)
--     "Internal Work RAM" — faster but smaller. Used for performance-
--     critical data like the main game loop state, input handling,
--     and pointers to save data.
--
--   0x08000000+ : ROM (read-only)
--     The game cartridge data. Contains compiled code, graphics,
--     maps, Pokemon data, and all static game content.
--
-- HOW STRUCTS WORK IN MEMORY:
--
-- C structs are laid out sequentially in memory. For example, if a
-- struct starts at address 0x02036e60 and has a field at offset 0x05,
-- that field is at address 0x02036e60 + 0x05 = 0x02036e65.
--
-- Arrays of structs are packed contiguously. The Nth element of an array
-- of structs (each of size S) starting at base address B is at B + N * S.
--
-------------------------------------------------------------------------------

ADDR = {}

-------------------------------------------------------------------------------
-- CORE GAME STATE (IWRAM — 0x03xxxxxx)
-------------------------------------------------------------------------------

-- gMain: The master game state struct. Controls what the game is doing
-- on any given frame (title screen, battle, overworld, menu, etc.)
ADDR.gMain              = 0x03003100

-- gMain.callback2: A function pointer at offset +0x04 inside gMain.
-- This tells you what "screen" the game is currently running.
-- Different callback addresses = different game states (title screen,
-- Oak speech, overworld, battle, etc.)
-- NOTE: The actual address values change every time the ROM is recompiled,
-- but WITHIN a single ROM build, they're consistent.
ADDR.gMain_callback2    = 0x03003104

-- Pointers to the save data blocks. The game uses an indirection layer
-- (pointers in IWRAM that point to structs in EWRAM) for DMA safety.
-- IMPORTANT: These pointer values can CHANGE during game initialization.
-- Always re-read the pointer before accessing save block fields.
ADDR.gSaveBlock1Ptr     = 0x03005018  -- points to SaveBlock1 (items, flags, map)
ADDR.gSaveBlock2Ptr     = 0x0300501c  -- points to SaveBlock2 (player info, options)

-------------------------------------------------------------------------------
-- OVERWORLD STATE (EWRAM — 0x02xxxxxx)
-------------------------------------------------------------------------------

-- gMapHeader: Information about the currently loaded map.
-- When this is non-zero, a map is loaded (we're in the overworld).
ADDR.gMapHeader         = 0x02036e24

-- gObjectEvents: Array of 16 ObjectEvent structs.
-- An "ObjectEvent" represents any entity on the overworld map: the player,
-- NPCs, item balls, signs, etc. Each has a position, sprite, movement type,
-- and various flags.
ADDR.gObjectEvents      = 0x02036e60

-- gPlayerAvatar: Struct with player-specific overworld info.
-- Contains the player's sprite ID, which ObjectEvent they correspond to,
-- their gender, running state, etc.
ADDR.gPlayerAvatar      = 0x020370a0

-------------------------------------------------------------------------------
-- STRUCT FIELD OFFSETS: ObjectEvent
-------------------------------------------------------------------------------
-- Each ObjectEvent struct is 0x24 (36) bytes. To access the Nth object:
--   address = gObjectEvents + N * OBJECT_EVENT_SIZE
-- Then add the field offset to get the specific field.

ADDR.OBJECT_EVENT_SIZE  = 0x24  -- 36 bytes per ObjectEvent

-- Flags field (u32 bitfield at offset 0x00):
--   bit 0  = active (1 if this object exists on the current map)
--   bit 16 = isPlayer (1 if this is the player's object event)
ADDR.OE_FLAGS           = 0x00

ADDR.OE_SPRITE_ID       = 0x04  -- u8: index into the hardware sprite table (OAM)
ADDR.OE_GRAPHICS_ID     = 0x05  -- u8: which character sprite to use (e.g., Red=0, Seel=126)
ADDR.OE_MOVEMENT_TYPE   = 0x06  -- u8: how this NPC moves (stationary, wander, etc.)
ADDR.OE_LOCAL_ID        = 0x08  -- u8: this object's ID within its map
ADDR.OE_MAP_NUM         = 0x09  -- u8: which map this object belongs to
ADDR.OE_MAP_GROUP       = 0x0A  -- u8: which map group this object belongs to
-- struct Coords16 currentCoords starts at offset 0x10 (per global.fieldmap.h).
-- Previous addresses.lua had these as 0x12/0x14 — that pointed at currentCoords.y
-- and previousCoords.x respectively, which "worked" only because legacy tests
-- only checked for ANY change rather than reading correct semantic x/y.
ADDR.OE_CURRENT_X       = 0x10  -- s16: current X position on the map grid
ADDR.OE_CURRENT_Y       = 0x12  -- s16: current Y position on the map grid

-------------------------------------------------------------------------------
-- STRUCT FIELD OFFSETS: PlayerAvatar
-------------------------------------------------------------------------------
-- The PlayerAvatar struct tracks player-specific state that isn't part of
-- the ObjectEvent (like running state, bike state, etc.)

ADDR.PA_FLAGS            = 0x00  -- u8: movement flags
ADDR.PA_SPRITE_ID        = 0x04  -- u8: hardware sprite index
ADDR.PA_OBJECT_EVENT_ID  = 0x05  -- u8: which ObjectEvent is the player (index into gObjectEvents)
ADDR.PA_GENDER           = 0x07  -- u8: 0=male, 1=female

-------------------------------------------------------------------------------
-- PLAYER GRAPHICS IDs
-------------------------------------------------------------------------------
-- These constants identify which overworld sprite a character uses.
-- Defined in include/constants/event_objects.h in the game source.
-- The player's graphicsId is stored in both the ObjectEvent and in
-- gSaveBlock2.playerAvatarGfxId (the saved selection from character creation).

ADDR.OBJ_EVENT_GFX_RED_NORMAL         = 0    -- Default male player (Red)
ADDR.OBJ_EVENT_GFX_RED_BIKE           = 1    -- Red on a bicycle
ADDR.OBJ_EVENT_GFX_RED_SURF           = 2    -- Red surfing
ADDR.OBJ_EVENT_GFX_RED_FIELD_MOVE     = 3    -- Red using a field move
ADDR.OBJ_EVENT_GFX_GREEN_NORMAL       = 7    -- Default female player (Green/Leaf)

-------------------------------------------------------------------------------
-- SAVE BLOCK 2 (player profile data)
-------------------------------------------------------------------------------
-- SaveBlock2 contains the player's profile: name, gender, play time,
-- options, Pokedex, and the chosen avatar graphics ID.
--
-- The static struct is at 0x02024588, but the game accesses it through
-- gSaveBlock2Ptr (a pointer in IWRAM). The pointer gets adjusted during
-- game initialization for DMA safety, so it may NOT point to 0x02024588.
-- Always read the pointer first: ptr = read32(gSaveBlock2Ptr), then
-- access fields at ptr + offset.

ADDR.gSaveBlock2             = 0x02024588  -- static address (may differ from pointer)
ADDR.SB2_PLAYER_GENDER       = 0x008       -- u8: 0=male, 1=female
ADDR.SB2_PLAYER_AVATAR_GFX   = 0x090       -- u8: the graphicsId chosen during character creation

-------------------------------------------------------------------------------
-- CHARACTER SELECTION CONSTANTS
-------------------------------------------------------------------------------
-- This ROM has a custom character selection screen during Oak's speech.
-- Players can choose from 105 different overworld sprites (trainers,
-- NPCs, Pokemon, etc.) The selection is stored in playerAvatarGfxId.

ADDR.OBJ_EVENT_GFX_SEEL      = 126  -- Seel's overworld sprite graphics ID
ADDR.SEEL_LIST_INDEX          = 94   -- Seel's position in the character selection list
                                      -- (0-indexed: Red=0, Green=1, ..., Seel=94)

-------------------------------------------------------------------------------
-- ROAMING POKEMON SYSTEM (custom feature)
-------------------------------------------------------------------------------
-- Roaming Pokemon are visible wild Pokemon that wander on the overworld.
-- The system tracks per-roamer state in EWRAM at gRoamers[]. Each entry is
-- packed into 4 bytes:
--   offset 0: u8 objEventId  (16 = OBJECT_EVENTS_COUNT = "free slot" sentinel)
--   offset 1: u8 tableIdx    (index into sRoamerGfxTable in roaming_pokemon.c)
--   offset 2: u8 level       (rolled at spawn; passed to CreateScriptedWildMon)
--   offset 3: u8 pendingBattle (1 when a bump triggered a battle queued for next frame)
--
-- Addresses are extracted from pokefirered.map. They MUST be re-extracted
-- whenever roaming_pokemon.c or anything that affects EWRAM layout changes.

ADDR.gRoamers                 = 0x0203f890  -- struct RoamingMon[4]
ADDR.gRoamingFlybySpriteIds   = 0x0203f8a0  -- u8[4], MAX_SPRITES (64) = free
ADDR.gRoamerCount             = 0x0203f8a4  -- u8: number of active roamers (0..4)
ADDR.gRoamerNextSpawnTimer    = 0x0203f8a6  -- u16: frames until next spawn attempt
ADDR.gRoamerNextFlybyTimer    = 0x0203f8a8  -- u16: frames until next flyby attempt

ADDR.ROAMER_STRUCT_SIZE       = 4
ADDR.ROAMER_OFF_OBJ_EVENT_ID  = 0
ADDR.ROAMER_OFF_TABLE_IDX     = 1
ADDR.ROAMER_OFF_LEVEL         = 2
ADDR.ROAMER_OFF_PENDING       = 3

ADDR.ROAMER_CAP_VISIBLE       = 4   -- matches ROAMER_CAP_VISIBLE in roaming_pokemon.c
ADDR.FLYBY_CAP_VISIBLE        = 4   -- matches FLYBY_CAP_VISIBLE
ADDR.OBJECT_EVENTS_COUNT      = 16  -- matches OBJECT_EVENTS_COUNT (free-slot sentinel)
ADDR.MAX_SPRITES              = 64  -- matches MAX_SPRITES (free-flyby sentinel)

-------------------------------------------------------------------------------
-- SPRITE TABLE (gSprites)
-------------------------------------------------------------------------------
-- Hardware sprite mirrors. Indexed by sprite id (0..63). Layout is
-- struct Sprite defined in include/sprite.h. We only need a handful
-- of fields for testing.

ADDR.gSprites                 = 0x0202063c  -- struct Sprite[64]
ADDR.SPRITE_STRUCT_SIZE       = 0x44        -- sizeof(struct Sprite) = 68
ADDR.SPRITE_OFF_ANIM_NUM      = 0x2A        -- u8: index into sprite->anims table
ADDR.SPRITE_OFF_FLAGS_3E      = 0x3E        -- u8: bit 0 = inUse

-- Animation numbers for object-event Pokemon sprites using sAnimTable_Standard
-- (see src/data/object_events/object_event_anims.h:1022 and
-- include/constants/event_object_movement.h).
ADDR.ANIM_STD_GO_WEST         = 6
ADDR.ANIM_STD_GO_EAST         = 7

-- Flyby sprites no longer use sAnimTable_Standard; CreateOneFlybySprite
-- in src/roaming_pokemon.c swaps sprite->anims over to sFlybyFlapAnimTable,
-- a custom 2-entry table: index 0 = flap-West, index 1 = flap-East.
-- Those are the values tests should assert against for animNum.
ADDR.FLYBY_FLAP_ANIM_WEST     = 0
ADDR.FLYBY_FLAP_ANIM_EAST     = 1

-------------------------------------------------------------------------------
-- BATTLE STATE
-------------------------------------------------------------------------------
-- gBattleTypeFlags is a u32 bitfield set when a battle is initiated.
-- There is NO bit named BATTLE_TYPE_WILD in this codebase; a "regular"
-- wild grass-step battle leaves all the type flags zero. Our roaming
-- system uses StartScriptedWildBattle, which sets BATTLE_TYPE_WILD_SCRIPTED
-- (bit 17 = 0x20000). That flag is the canonical indicator that a
-- roamer-triggered battle has started.

ADDR.gBattleTypeFlags         = 0x02022b4c  -- u32
ADDR.BATTLE_TYPE_WILD_SCRIPTED = 0x20000  -- (1 << 17)
-- Legacy alias so older tests that reference BATTLE_TYPE_WILD keep working.
ADDR.BATTLE_TYPE_WILD         = 0x20000

-- gEnemyParty is the array of Pokemon the player faces in battle.
-- Only slot 0 is used for wild battles. Field offsets are derived from
-- struct Pokemon (encrypted, but species lives in a known offset of the
-- substructure indexed by personality % 24). For simplicity, tests can
-- read the post-battle-prepared "battle struct" gBattleMons instead, or
-- just call GetMonData equivalent via memory introspection. For our
-- bump-to-battle test we trust that StartScriptedWildBattle populated
-- gEnemyParty[0] with the species we passed; we'll verify by reading
-- substructure data.
ADDR.gEnemyParty              = 0x0202402c  -- struct Pokemon[6]
ADDR.POKEMON_STRUCT_SIZE      = 100         -- sizeof(struct Pokemon)

-------------------------------------------------------------------------------
-- MAP / LOCATION
-------------------------------------------------------------------------------
-- gSaveBlock1Ptr->location is a struct WarpData at offset 0x04 of SaveBlock1.
-- WarpData layout:
--   offset 0: u8  mapGroup
--   offset 1: u8  mapNum
--   offset 2: s8  warpId
--   offset 4: s16 x
--   offset 6: s16 y

-- SaveBlock1 struct field offsets.
-- NOTE: The custom fork expanded registeredItem (u16, 2 bytes) to
-- registeredItems[REGISTERED_ITEMS_MAX=20] (40 bytes), adding 38 extra bytes
-- to the struct. Every field that follows in the struct is shifted by 0x26.
-- The /*0xNNNN*/ comments in include/global.h are stale after this change;
-- use these constants (derived from offsetof / REGISTERED_ITEMS_MAX) instead.
ADDR.SB1_FLAGS                = 0x0F08  -- offsetof(SaveBlock1, flags)
                                         -- Verified from GetFlagAddr disassembly (word at 0x806ea88).
                                         -- = 0x0EE0 (stale comment) + 0x28: registeredItem grew
                                         -- from u16 (2B) to u16[REGISTERED_ITEMS_MAX=20] (40B),
                                         -- adding 38B, plus 2B compiler alignment pad before pcItems.

ADDR.SB1_LOC_MAP_GROUP        = 0x04
ADDR.SB1_LOC_MAP_NUM          = 0x05
ADDR.SB1_LOC_X                = 0x08
ADDR.SB1_LOC_Y                = 0x0A

-- Map IDs we care about for tests. mapGroup=3 is the Kanto routes/towns.
ADDR.MAP_GROUP_KANTO          = 3
ADDR.MAP_NUM_PALLET_TOWN      = 0
ADDR.MAP_NUM_ROUTE1           = 19
ADDR.MAP_NUM_VIRIDIAN_CITY    = 1   -- north of Route 1, used for warp-cleanup test

-------------------------------------------------------------------------------
-- WILD ENCOUNTER STATE
-------------------------------------------------------------------------------
-- sWildEncounterData is a static EWRAM struct in src/wild_encounter.c that
-- tracks the encounter-rate accumulator, its own RNG state (separate from
-- gRngValue!), and a cooldown counter. Tests that need to force or suppress
-- encounters poke these fields directly.
--
-- struct WildEncounterData:
--   0x00  u32 rngState                 -- separate LCG state for WildEncounterRandom
--   0x04  u16 prevMetatileBehavior
--   0x06  u16 encounterRateBuff        -- "pity" accumulator; saturates encounter rate
--   0x08  u8  stepsSinceLastEncounter  -- cooldown (must reach minSteps to bypass dice)
--   0x09  u8  abilityEffect
--   0x0A  u16 leadMonHeldItem
ADDR.sWildEncounterData              = 0x02038760
ADDR.WED_RNG_STATE                   = 0x00
ADDR.WED_PREV_METATILE               = 0x04
ADDR.WED_ENCOUNTER_RATE_BUFF         = 0x06
ADDR.WED_STEPS_SINCE_LAST            = 0x08
ADDR.WED_ABILITY_EFFECT              = 0x09
ADDR.WED_LEAD_MON_HELD_ITEM          = 0x0A

-------------------------------------------------------------------------------
-- ROAMING POKEMON STATE
-------------------------------------------------------------------------------
-- gRoamers is a 4-slot EWRAM array of RoamingMon (4 bytes each). A test that
-- wants to isolate the regular wild-encounter path must disable all roamers
-- first, because a roamer can hijack the step via StartScriptedWildBattle
-- (which sets BATTLE_TYPE_WILD_SCRIPTED and bypasses GenerateWildMon entirely).
-- The canonical "inactive slot" sentinel is objEventId == OBJECT_EVENTS_COUNT
-- (16) — see src/roaming_pokemon.c. pendingBattle=0 stops any in-flight
-- roamer battle from firing after a collision mark.
--
-- struct RoamingMon (4 bytes):
--   0x00  u8 objEventId    -- OBJECT_EVENTS_COUNT (16) means "slot free"
--   0x01  u8 tableIdx
--   0x02  u8 level
--   0x03  u8 pendingBattle -- set by collision check, cleared when battle starts
ADDR.gRoamers                        = 0x0203f890
ADDR.gRoamerCount                    = 0x0203f8a4
ADDR.ROAMER_CAP_VISIBLE              = 4
ADDR.ROAMER_SIZE                     = 4
ADDR.ROAMER_OFF_OBJ_EVENT_ID         = 0x00
ADDR.ROAMER_OFF_TABLE_IDX            = 0x01
ADDR.ROAMER_OFF_LEVEL                = 0x02
ADDR.ROAMER_OFF_PENDING_BATTLE       = 0x03
ADDR.OBJECT_EVENTS_COUNT             = 16   -- sentinel for "no such slot"

-------------------------------------------------------------------------------
-- MART ITEM LISTS (PP Items in Marts feature)
-------------------------------------------------------------------------------
-- ROM addresses for the static .2byte item arrays in mart scripts.
-- Each list is terminated by 0 (ITEM_NONE). Extracted from pokefirered.map
-- after building with the PP items feature applied.
-- Re-extract if mart scripts.inc changes cause map layout shifts.

-------------------------------------------------------------------------------
-- BAG MENU STATE (L/R page-scroll feature)
-------------------------------------------------------------------------------
-- gBagMenuState: EWRAM struct that tracks the bag cursor and scroll state.
-- Defined in include/item_menu.h as struct BagStruct.
--
-- Layout (extracted from pokefirered.map):
--   0x00  MainCallback bagCallback (4 bytes)
--   0x04  u8  location
--   0x05  bool8 bagOpen            -- non-zero when the bag screen is active
--   0x06  u16 pocket               -- index of the currently displayed pocket
--   0x08  u16 itemsAbove[3]        -- items above the viewport, per pocket
--   0x0E  u16 cursorPos[3]         -- cursor row within the viewport, per pocket
--
-- NUM_BAG_POCKETS_NO_CASES = 3 (items=0, key items=1, Poké Balls=2).
-- Address extracted from pokefirered.map: grep " gBagMenuState$" pokefirered.map
ADDR.gBagMenuState            = 0x0203ad8c

ADDR.CeruleanCity_Mart_Items         = 0x0816b60c  -- Super Potion tier
ADDR.CinnabarIsland_Mart_Items       = 0x0816f390  -- Hyper Potion tier
ADDR.FourIsland_Mart_Items           = 0x08172624  -- Max Potion tier
ADDR.SevenIsland_Mart_Items          = 0x081714a0  -- dual tier (Hyper + Max)

-- Item IDs referenced in mart tests
ADDR.ITEM_SUPER_POTION               = 22
ADDR.ITEM_HYPER_POTION               = 21
ADDR.ITEM_MAX_POTION                 = 20
ADDR.ITEM_ETHER                      = 34
ADDR.ITEM_MAX_ETHER                  = 35
ADDR.ITEM_ELIXIR                     = 36
ADDR.ITEM_MAX_ELIXIR                 = 37

-------------------------------------------------------------------------------
-- BATTLE DISPLAY STATE — Type Effectiveness Colors (issue #7)
-------------------------------------------------------------------------------
-- gPlttBufferFaded: the palette buffer the GBA reads each frame.
-- BG palette n, entry e is at: gPlttBufferFaded + (n * 16 + e) * 2
-- Move name windows use BG palettes 10-13 (one per move slot).
-- Entry 13 in each palette is the text foreground color, overwritten by
-- MoveSelectionSetEffectivenessColors() to reflect type matchup.
ADDR.gPlttBufferFaded          = 0x02037688

-- Read the text-foreground color for move slot i (0 = top-left move).
-- BG_PLTT_ID(pal) = pal * 16; entry 13 is text foreground; 2 bytes each.
ADDR.move_eff_color_addr = function(i)
    local pal = 10 + i
    return ADDR.gPlttBufferFaded + (pal * 16 + 13) * 2
end

-- Expected text-foreground RGB15 values from GetMoveEffectivenessColor:
ADDR.EFF_COLOR_NEUTRAL   = 0x2529  -- RGB(9,9,9)   neutral gray    (1x)
ADDR.EFF_COLOR_SUPER     = 0x0280  -- RGB(0,20,0)  dark green      (2x / 4x)
ADDR.EFF_COLOR_NOT_VERY  = 0x035A  -- RGB(26,26,0) amber           (0.5x / 0.25x)
ADDR.EFF_COLOR_IMMUNE    = 0x0018  -- RGB(24,0,0)  dark red        (0x)

-- gAbsentBattlerFlags: u8 bitmask — bit N set means battler N has fainted
-- with no replacement (absent from battle).
-- Bit 1 (value 0x02) = battler 1 (B_POSITION_OPPONENT_LEFT) is absent.
ADDR.gAbsentBattlerFlags       = 0x02023d70

-- gBattleMons: array of 4 BattlePokemon structs, one per battler slot.
-- B_POSITION_OPPONENT_LEFT = battler 1, B_POSITION_OPPONENT_RIGHT = battler 3.
-- sizeof(struct BattlePokemon) = 0x58 (88 bytes).
ADDR.gBattleMons               = 0x02023be4
ADDR.BATTLEMON_SIZE            = 0x58
ADDR.BP_TYPE1                  = 0x21   -- offsetof(BattlePokemon, type1)
ADDR.BP_TYPE2                  = 0x22   -- offsetof(BattlePokemon, type2)

-- gMultiUsePlayerCursor: u8 — battler ID of the currently highlighted target
-- in HandleInputChooseTarget. Set to 0xFF by InitMoveSelectionsVarsAndStrings
-- when the move selection screen first opens.
ADDR.gMultiUsePlayerCursor     = 0x03005004

-- Type IDs (from include/constants/pokemon.h):
ADDR.TYPE_FIRE   = 10
ADDR.TYPE_WATER  = 11
ADDR.TYPE_GRASS  = 12

-- Standard battler IDs in a double battle (AGBCC/vanilla layout):
ADDR.BATTLER_PLAYER_LEFT   = 0
ADDR.BATTLER_OPPONENT_LEFT = 1   -- B_POSITION_OPPONENT_LEFT
ADDR.BATTLER_PLAYER_RIGHT  = 2
ADDR.BATTLER_OPPONENT_RIGHT = 3  -- B_POSITION_OPPONENT_RIGHT

-------------------------------------------------------------------------------
-- PARTY STATE
-------------------------------------------------------------------------------
-- gPlayerPartyCount: number of Pokemon currently in the player's party (0-6).
-- Extracted from pokefirered.map for the default (AGBCC) build.
-- NOTE: The MODERN=1 build places this at 0x0203a8d5; only the AGBCC address
-- below is valid for test/run_test.sh (which runs pokefirered.gba, not
-- pokefirered_modern.gba).

ADDR.gPlayerPartyCount               = 0x02024029

return ADDR
