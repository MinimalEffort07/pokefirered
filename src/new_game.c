/**
 * new_game.c - New Game Initialization
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * This file handles all the data initialization that occurs when the player
 * starts a brand new game (selects "NEW GAME" from the title screen). It
 * sets up every subsystem to its fresh state: trainer ID, options, Pokedex,
 * bag items, money, play time, event flags, Pokemon storage, and more.
 *
 * The save data is organized into two main blocks:
 *   gSaveBlock1: Game world state (event flags, items, map data, rival name, etc.)
 *   gSaveBlock2: Player-specific data (trainer ID, options, Pokedex, battle tower, etc.)
 *
 * These save blocks are stored in EWRAM (External Work RAM) during gameplay
 * and written to Flash ROM when the player saves.
 *
 * The initialization order matters -- some systems depend on others being
 * cleared first. For example, the bag must be cleared before initializing
 * PC items, and event flags must be reset before running the map flag reset script.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "random.h"
#include "overworld.h"
#include "constants/maps.h"
#include "load_save.h"
#include "item_menu.h"
#include "tm_case.h"
#include "berry_pouch.h"
#include "quest_log.h"
#include "wild_encounter.h"
#include "event_data.h"
#include "mail_data.h"
#include "play_time.h"
#include "money.h"
#include "battle_records.h"
#include "pokemon_size_record.h"
#include "pokemon_storage_system.h"
#include "roamer.h"
#include "item.h"
#include "player_pc.h"
#include "berry.h"
#include "easy_chat.h"
#include "union_room_chat.h"
#include "mystery_gift.h"
#include "renewable_hidden_items.h"
#include "trainer_tower.h"
#include "script.h"
#include "berry_powder.h"
#include "pokemon_jump.h"
#include "event_scripts.h"

// this file's functions
static void ResetMiniGamesResults(void);

/*
 * gDifferentSaveFile: Flag set to TRUE when starting a new game. Used by
 * the loading system to detect that the current in-memory data is from a
 * new game rather than a loaded save file. Some systems behave differently
 * depending on whether data comes from a fresh start or a loaded save.
 */
// EWRAM vars
EWRAM_DATA bool8 gDifferentSaveFile = FALSE;

/**
 * FUNCTION: SetTrainerId
 *
 * PURPOSE: Store a 32-bit trainer ID into a byte array in little-endian order.
 *
 * HOW IT WORKS:
 * The trainer ID is a 32-bit value, but it's stored as 4 individual bytes
 * in the save data. This function splits the 32-bit value by extracting
 * each byte with right shifts:
 *   dst[0] = bits 0-7   (least significant byte)
 *   dst[1] = bits 8-15
 *   dst[2] = bits 16-23
 *   dst[3] = bits 24-31 (most significant byte)
 *
 * GBA CONTEXT:
 * The GBA uses little-endian byte order (least significant byte first),
 * which matches the storage order here. The trainer ID is split into bytes
 * rather than stored as a u32 because the save data structure uses a byte
 * array for maximum portability across different memory alignment scenarios.
 *
 * GAME LOGIC:
 * The trainer ID uniquely identifies a save file. It's used for:
 *   - Determining if a traded Pokemon is "yours" or not (OT check)
 *   - The lower 16 bits become the visible trainer ID shown on the card
 *   - The upper 16 bits are the "secret ID" (hidden from the player)
 *   - Combined with the trainer name, determines shiny Pokemon appearance
 *
 * PARAMETERS:
 * @param trainerId -- 32-bit trainer ID value
 * @param dst       -- Byte array to store the ID in (must be at least 4 bytes)
 */
void SetTrainerId(u32 trainerId, u8 *dst)
{
    dst[0] = trainerId;
    dst[1] = trainerId >> 8;
    dst[2] = trainerId >> 16;
    dst[3] = trainerId >> 24;
}

/**
 * FUNCTION: CopyTrainerId
 *
 * PURPOSE: Copy a 4-byte trainer ID from one location to another.
 *
 * PARAMETERS:
 * @param dst -- Destination byte array
 * @param src -- Source byte array
 */
void CopyTrainerId(u8 *dst, u8 *src)
{
    s32 i;
    for (i = 0; i < 4; i++)
        dst[i] = src[i];
}

/**
 * FUNCTION: InitPlayerTrainerId
 *
 * PURPOSE: Generate a random 32-bit trainer ID for a new game.
 *
 * HOW IT WORKS:
 * Creates a 32-bit trainer ID from two 16-bit random numbers:
 *   Upper 16 bits: from Random() (the standard PRNG)
 *   Lower 16 bits: from GetGeneratedTrainerIdLower() (may use additional
 *     entropy sources or a separate PRNG)
 *
 * The Random() result is shifted left 16 bits and OR'd with the lower half.
 *
 * GAME LOGIC:
 * The lower 16 bits of the trainer ID become the player's visible "Trainer ID"
 * number (0-65535). The upper 16 bits are the "Secret ID" that the player
 * never sees but which affects shiny Pokemon determination.
 */
static void InitPlayerTrainerId(void)
{
    u32 trainerId = (Random() << 0x10) | GetGeneratedTrainerIdLower();
    SetTrainerId(trainerId, gSaveBlock2Ptr->playerTrainerId);
}

/**
 * FUNCTION: SetDefaultOptions
 *
 * PURPOSE: Initialize all game options to their default values for a new game.
 *
 * GAME LOGIC:
 * These are the settings the player can change in the OPTIONS menu:
 *   - Text Speed: MID (how fast dialog text appears)
 *   - Window Frame: 0 (the default dialog box border style)
 *   - Sound: MONO (single-speaker output -- GBA has one speaker)
 *   - Battle Style: SHIFT (game asks to switch Pokemon when opponent faints)
 *   - Battle Scene: ON (show battle animations)
 *   - Region Map Zoom: OFF (start map zoomed out)
 *   - Button Mode: HELP (L button opens the help system in FR/LG)
 */
static void SetDefaultOptions(void)
{
    gSaveBlock2Ptr->optionsTextSpeed = OPTIONS_TEXT_SPEED_MID;
    gSaveBlock2Ptr->optionsWindowFrameType = 0;
    gSaveBlock2Ptr->optionsSound = OPTIONS_SOUND_MONO;
    gSaveBlock2Ptr->optionsBattleStyle = OPTIONS_BATTLE_STYLE_SHIFT;
    gSaveBlock2Ptr->optionsBattleSceneOff = FALSE;
    gSaveBlock2Ptr->regionMapZoom = FALSE;
    gSaveBlock2Ptr->optionsButtonMode = OPTIONS_BUTTON_MODE_HELP;
}

/**
 * FUNCTION: ClearPokedexFlags
 *
 * PURPOSE: Clear the Pokedex "owned" and "seen" bitfields to zero.
 *
 * GAME LOGIC:
 * The Pokedex tracks which Pokemon the player has seen (encountered in battle
 * or shown by NPCs) and which they've owned (caught or received). These are
 * stored as bitfields where each Pokemon gets one bit. For 386 Pokemon,
 * that's about 49 bytes per bitfield.
 */
static void ClearPokedexFlags(void)
{
    memset(&gSaveBlock2Ptr->pokedex.owned, 0, sizeof(gSaveBlock2Ptr->pokedex.owned));
    memset(&gSaveBlock2Ptr->pokedex.seen, 0, sizeof(gSaveBlock2Ptr->pokedex.seen));
}

/**
 * FUNCTION: ClearBattleTower
 *
 * PURPOSE: Zero out all Battle Tower save data.
 *
 * HOW IT WORKS:
 * Uses CpuFill32 (a fast 32-bit fill function) to write zeros across the
 * entire battle tower struct. This clears win streaks, saved teams, etc.
 *
 * GAME LOGIC:
 * The Battle Tower is an endgame facility where players fight AI trainers
 * in consecutive rounds. Progress (win streaks, records) is tracked here.
 */
static void ClearBattleTower(void)
{
    CpuFill32(0, &gSaveBlock2Ptr->battleTower, sizeof(gSaveBlock2Ptr->battleTower));
}

/**
 * FUNCTION: WarpToPlayersRoom
 *
 * PURPOSE: Set the player's starting location to their bedroom in Pallet Town.
 *
 * HOW IT WORKS:
 * Sets the warp destination to the upstairs room of the player's house in
 * Pallet Town (MAP_PALLET_TOWN_PLAYERS_HOUSE_2F), at tile coordinates (6, 6).
 * The warp ID of -1 means "use raw coordinates" instead of a named warp pad.
 *
 * GAME LOGIC:
 * This is where every new Pokemon FireRed/LeafGreen adventure begins --
 * the player wakes up in their room and walks downstairs to start their journey.
 */
static void WarpToPlayersRoom(void)
{
    SetWarpDestination(MAP_GROUP(MAP_PALLET_TOWN_PLAYERS_HOUSE_2F), MAP_NUM(MAP_PALLET_TOWN_PLAYERS_HOUSE_2F), -1, 6, 6);
    WarpIntoMap();
}

/**
 * FUNCTION: Sav2_ClearSetDefault
 *
 * PURPOSE: Clear SaveBlock2 and set options to defaults.
 *
 * HOW IT WORKS:
 * Used during new game setup to initialize the player data block.
 * ClearSav2 zeros out the entire SaveBlock2, then SetDefaultOptions
 * fills in the standard option values.
 */
void Sav2_ClearSetDefault(void)
{
    ClearSav2();
    SetDefaultOptions();
}

/**
 * FUNCTION: ResetMenuAndMonGlobals
 *
 * PURPOSE: Reset in-memory gameplay state that isn't part of the save data.
 *
 * HOW IT WORKS:
 * Clears runtime state that accumulates during gameplay:
 *   - gDifferentSaveFile flag
 *   - Player and enemy party Pokemon (in-memory battle/party data)
 *   - Cursor positions for the bag, TM case, and berry pouch menus
 *   - Quest Log state
 *   - Wild encounter RNG seed
 *   - Special script variables
 *
 * GAME LOGIC:
 * This is called when returning to the title screen or when resetting state
 * without performing a full new game initialization. It ensures that stale
 * data from a previous play session doesn't contaminate a new session.
 */
void ResetMenuAndMonGlobals(void)
{
    gDifferentSaveFile = FALSE;
    ZeroPlayerPartyMons();
    ZeroEnemyPartyMons();
    ResetBagCursorPositions();
    ResetTMCaseCursorPos();
    BerryPouch_CursorResetToTop();
    ResetQuestLog();
    SeedWildEncounterRng(Random());
    ResetSpecialVars();
}

/**
 * FUNCTION: NewGameInitData
 *
 * PURPOSE: Master initialization function for starting a completely new game.
 *
 * HOW IT WORKS:
 * This is the most important function in this file. It initializes EVERY
 * subsystem in the game to its fresh state, in the correct dependency order:
 *
 * 1. PRESERVE RIVAL NAME: The rival name may have been set during the intro
 *    sequence (before save data is cleared), so it's backed up first and
 *    restored at the very end.
 *
 * 2. CLEAR SAVE DATA: Zeros out SaveBlock1 (world state), zeroes party data,
 *    clears mail, battle tower, and special flags.
 *
 * 3. GENERATE IDENTITY: Creates a random trainer ID and resets the play timer.
 *
 * 4. CLEAR GAME STATE: Resets Pokedex, event flags/variables, fame checker,
 *    game statistics, and link battle records.
 *
 * 5. INITIALIZE RESOURCES: Sets starting money (3000), clears the bag,
 *    sets up PC items (the initial potion on the PC), initializes berries,
 *    chat phrases, mystery gift, and minigame records.
 *
 * 6. SET STARTING POSITION: Warps the player to their bedroom in Pallet Town
 *    and runs the map flag initialization script.
 *
 * 7. RESTORE RIVAL NAME: Copies the backed-up rival name back into save data.
 *
 * GAME LOGIC:
 * The player starts with 3000 money (enough to buy a few Pokeballs after
 * getting the Pokedex), no Pokemon (they'll receive their starter from
 * Professor Oak), and an empty bag (except for the Potion on their PC).
 */
void NewGameInitData(void)
{
    u8 rivalName[PLAYER_NAME_LENGTH + 1];

    /* Back up the rival name (may have been set during intro/naming screen) */
    StringCopy(rivalName, gSaveBlock1Ptr->rivalName);

    gDifferentSaveFile = TRUE;
    gSaveBlock2Ptr->encryptionKey = 0;  /* Reset save encryption key */

    /* Clear party data */
    ZeroPlayerPartyMons();
    ZeroEnemyPartyMons();

    /* Clear major save data subsystems */
    ClearBattleTower();
    ClearSav1();       /* Zero out all of SaveBlock1 */
    ClearMailData();

    /* Clear special flags used for GameCube connectivity and save states */
    gSaveBlock2Ptr->specialSaveWarpFlags = 0;
    gSaveBlock2Ptr->gcnLinkFlags = 0;        /* GameCube-GBA link flags */
    gSaveBlock2Ptr->unkFlag1 = TRUE;
    gSaveBlock2Ptr->unkFlag2 = FALSE;

    /* Generate the player's unique trainer ID */
    InitPlayerTrainerId();

    /* Reset the play time counter (hours:minutes:seconds) */
    PlayTimeCounter_Reset();

    /* Clear Pokedex seen/owned flags */
    ClearPokedexFlags();

    /* Initialize event system (flags and variables used by scripts) */
    InitEventData();
    ResetFameChecker();

    /* Give the player 3000 starting money */
    SetMoney(&gSaveBlock1Ptr->money, 3000);

    /* Clear game statistics (steps taken, battles fought, etc.) */
    ResetGameStats();

    /* Clear multiplayer battle records */
    ClearPlayerLinkBattleRecords();

    /* Initialize size record tracking for Heracross and Magikarp */
    InitHeracrossSizeRecord();
    InitMagikarpSizeRecord();

    /* Enable National Pokedex for RSE compatibility */
    EnableNationalPokedex_RSE();

    /* Reset Pokemon party and storage */
    gPlayerPartyCount = 0;
    ZeroPlayerPartyMons();
    ResetPokemonStorageSystem();  /* Clear all PC boxes */

    /* Clear roaming Pokemon data (e.g., the legendary beasts) */
    ClearRoamerData();

    /* Clear bag and registered items */
    memset(gSaveBlock1Ptr->registeredItems, 0, sizeof(gSaveBlock1Ptr->registeredItems));
    ClearBag();

    /* Put a Potion on the player's PC (the classic starting gift) */
    NewGameInitPCItems();

    /* Clear e-Reader berry data */
    ClearEnigmaBerries();

    /* Initialize Easy Chat (preset phrases for communication) */
    InitEasyChatPhrases();

    /* Reset multiplayer social features */
    ResetTrainerFanClub();
    UnionRoomChat_InitializeRegisteredTexts();

    /* Reset minigame scores (Berry Crush, Pokemon Jump, Berry Pick) */
    ResetMiniGamesResults();

    /* Clear Mystery Gift data */
    ClearMysteryGift();

    /* Mark all hidden items as available for pickup */
    SetAllRenewableItemFlags();

    /* Set the player's starting location to their bedroom */
    WarpToPlayersRoom();

    /* Run the script that initializes all map-specific event flags */
    RunScriptImmediately(EventScript_ResetAllMapFlags);

    /* Restore the rival name that was backed up at the start */
    StringCopy(gSaveBlock1Ptr->rivalName, rivalName);

    /* Reset Trainer Tower completion records */
    ResetTrainerTowerResults();
}

/**
 * FUNCTION: ResetMiniGamesResults
 *
 * PURPOSE: Clear all minigame score and result data.
 *
 * HOW IT WORKS:
 * Zeros out the Berry Crush results structure, resets berry powder count,
 * clears Pokemon Jump records, and zeros Berry Pick results.
 *
 * GAME LOGIC:
 * These are wireless multiplayer minigames accessible in the Union Room:
 *   - Berry Crush: Players crush berries to make berry powder
 *   - Pokemon Jump: Players time jumps to a rotating rope
 *   - Berry Pick: Players collect berries competitively
 */
static void ResetMiniGamesResults(void)
{
    CpuFill16(0, &gSaveBlock2Ptr->berryCrush, sizeof(struct BerryCrush));
    SetBerryPowder(&gSaveBlock2Ptr->berryCrush.berryPowderAmount, 0);
    ResetPokemonJumpRecords();
    CpuFill16(0, &gSaveBlock2Ptr->berryPick, sizeof(struct BerryPickingResults));
}
