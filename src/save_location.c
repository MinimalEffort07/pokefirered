/**
 * @file save_location.c
 * @brief Save Warp Status and Location-Based Game Flags
 *
 * FILE OVERVIEW:
 * This file determines what kind of location the player is currently in (Pokemon Center,
 * lobby, etc.) and sets flags in the save data accordingly. These flags control what
 * happens when the game is loaded — for example, if you saved inside a Pokemon Center,
 * the game knows to reload that center map properly.
 *
 * It also handles setting special flags for game progression milestones like becoming
 * Champion and unlocking the National Pokedex.
 *
 * GAME LOGIC — SAVE WARP FLAGS:
 * When the player saves and quits, the game needs to remember the "type" of location
 * they were in so it can handle the reload correctly. The specialSaveWarpFlags bitfield
 * tracks this:
 * - POKECENTER_SAVEWARP: Player saved in a Pokemon Center
 * - LOBBY_SAVEWARP: Player saved in a multiplayer lobby/link room
 * - CHAMPION_SAVEWARP: Player has beaten the Champion (postgame flag)
 * - UNK_SPECIAL_SAVE_WARP_FLAG_3: Unknown/unused flag
 *
 * MAP IDENTIFICATION:
 * Maps are identified by a (mapGroup, mapNum) pair. This file packs them into a
 * single u16 value (group << 8 | num) for efficient comparison against lookup tables.
 */
#include "global.h"
#include "save_location.h"
#include "constants/maps.h"

/**
 * FUNCTION: IsCurMapInLocationList
 *
 * PURPOSE: Checks whether the player's current map appears in a given list of map IDs.
 *
 * HOW IT WORKS:
 * Packs the current map's group and number into a single u16 (group in high byte,
 * number in low byte), then iterates through the list until it finds a match or
 * hits the MAP_UNDEFINED sentinel value that terminates the list.
 *
 * @param list — Null-terminated array of packed map IDs to check against
 * RETURNS: TRUE if the current map is in the list, FALSE otherwise
 */
static bool32 IsCurMapInLocationList(const u16 *list)
{
    s32 i;
    /* Pack map group and number into a single 16-bit value for comparison.
     * Group goes in bits 15-8, number goes in bits 7-0. */
    u16 locSum = (gSaveBlock1Ptr->location.mapGroup << 8) + (gSaveBlock1Ptr->location.mapNum);

    for (i = 0; list[i] != MAP_UNDEFINED; i++)
    {
        if (list[i] == locSum)
            return TRUE;
    }
    return FALSE;
}

/**
 * List of all Pokemon Center maps in the game (both floors).
 * Also includes multiplayer link rooms (Battle Colosseum, Trade Center, Union Room)
 * since these function similarly to Pokemon Centers for save/load purposes.
 * Terminated by MAP_UNDEFINED as a sentinel value.
 */
static const u16 sSaveLocationPokeCenterList[] =
{
    MAP_VIRIDIAN_CITY_POKEMON_CENTER_1F, MAP_VIRIDIAN_CITY_POKEMON_CENTER_2F,
    MAP_PEWTER_CITY_POKEMON_CENTER_1F, MAP_PEWTER_CITY_POKEMON_CENTER_2F,
    MAP_CERULEAN_CITY_POKEMON_CENTER_1F, MAP_CERULEAN_CITY_POKEMON_CENTER_2F,
    MAP_LAVENDER_TOWN_POKEMON_CENTER_1F, MAP_LAVENDER_TOWN_POKEMON_CENTER_2F,
    MAP_VERMILION_CITY_POKEMON_CENTER_1F, MAP_VERMILION_CITY_POKEMON_CENTER_2F,
    MAP_CELADON_CITY_POKEMON_CENTER_1F, MAP_CELADON_CITY_POKEMON_CENTER_2F,
    MAP_FUCHSIA_CITY_POKEMON_CENTER_1F, MAP_FUCHSIA_CITY_POKEMON_CENTER_2F,
    MAP_CINNABAR_ISLAND_POKEMON_CENTER_1F, MAP_CINNABAR_ISLAND_POKEMON_CENTER_2F,
    MAP_INDIGO_PLATEAU_POKEMON_CENTER_1F, MAP_INDIGO_PLATEAU_POKEMON_CENTER_2F,
    MAP_SAFFRON_CITY_POKEMON_CENTER_1F, MAP_SAFFRON_CITY_POKEMON_CENTER_2F,
    MAP_ROUTE4_POKEMON_CENTER_1F, MAP_ROUTE4_POKEMON_CENTER_2F,
    MAP_ROUTE10_POKEMON_CENTER_1F, MAP_ROUTE10_POKEMON_CENTER_2F,
    MAP_ONE_ISLAND_POKEMON_CENTER_1F, MAP_ONE_ISLAND_POKEMON_CENTER_2F,
    MAP_TWO_ISLAND_POKEMON_CENTER_1F, MAP_TWO_ISLAND_POKEMON_CENTER_2F,
    MAP_THREE_ISLAND_POKEMON_CENTER_1F, MAP_THREE_ISLAND_POKEMON_CENTER_2F,
    MAP_FOUR_ISLAND_POKEMON_CENTER_1F, MAP_FOUR_ISLAND_POKEMON_CENTER_2F,
    MAP_FIVE_ISLAND_POKEMON_CENTER_1F, MAP_FIVE_ISLAND_POKEMON_CENTER_2F,
    MAP_SEVEN_ISLAND_POKEMON_CENTER_1F, MAP_SEVEN_ISLAND_POKEMON_CENTER_2F,
    MAP_SIX_ISLAND_POKEMON_CENTER_1F, MAP_SIX_ISLAND_POKEMON_CENTER_2F,
    MAP_BATTLE_COLOSSEUM_2P,   /* 2-player battle link room */
    MAP_TRADE_CENTER,          /* Pokemon trading room */
    MAP_BATTLE_COLOSSEUM_4P,   /* 4-player battle link room */
    MAP_UNION_ROOM,            /* Wireless Union Room */
    MAP_UNDEFINED,             /* Sentinel — marks end of list */
};

/**
 * FUNCTION: IsCurMapPokeCenter
 *
 * PURPOSE: Checks if the player is currently inside a Pokemon Center (or link room).
 *
 * RETURNS: TRUE if on a Pokemon Center map
 */
bool32 IsCurMapPokeCenter(void)
{
    return IsCurMapInLocationList(sSaveLocationPokeCenterList);
}

/* Empty list — no maps trigger reload-location behavior in the final game */
static const u16 sSaveLocationReloadLocList[] = { MAP_UNDEFINED };

/**
 * FUNCTION: IsCurMapReloadLocation
 *
 * PURPOSE: Checks if the current map is a "reload location" (lobby that needs
 *          special handling on save/load). Currently empty — no maps qualify.
 */
static bool32 IsCurMapReloadLocation(void)
{
    return IsCurMapInLocationList(sSaveLocationReloadLocList);
}

// Nulled out list. Unknown what this would have been.
static const u16 sEmptyMapList[] = { MAP_UNDEFINED };

/**
 * FUNCTION: IsCurMapInEmptyList
 *
 * PURPOSE: Checks an unused/empty map list. Always returns FALSE since the list
 *          contains only the sentinel. Likely a remnant of a cut feature.
 */
static bool32 IsCurMapInEmptyList(void)
{
    return IsCurMapInLocationList(sEmptyMapList);
}

/**
 * FUNCTION: TrySetPokeCenterWarpStatus
 *
 * PURPOSE: Sets or clears the POKECENTER_SAVEWARP flag based on whether the
 *          player is currently in a Pokemon Center.
 *
 * GAME LOGIC:
 * If the flag is set when the game loads, the load code knows the player was
 * in a Pokemon Center and can handle the warp-in animation accordingly.
 * The &= ~(flag) pattern clears a specific bit, while |= (flag) sets it.
 */
static void TrySetPokeCenterWarpStatus(void)
{
    if (IsCurMapPokeCenter() == FALSE)
        gSaveBlock2Ptr->specialSaveWarpFlags &= ~(POKECENTER_SAVEWARP);  /* Clear the bit */
    else
        gSaveBlock2Ptr->specialSaveWarpFlags |= POKECENTER_SAVEWARP;     /* Set the bit */
}

/**
 * FUNCTION: TrySetReloadWarpStatus
 *
 * PURPOSE: Sets or clears the LOBBY_SAVEWARP flag. Currently a no-op since
 *          the reload location list is empty.
 */
static void TrySetReloadWarpStatus(void)
{
    if (!IsCurMapReloadLocation())
        gSaveBlock2Ptr->specialSaveWarpFlags &= ~(LOBBY_SAVEWARP);
    else
        gSaveBlock2Ptr->specialSaveWarpFlags |= LOBBY_SAVEWARP;
}

// Unknown save warp flag. Never set because map list is empty.
/**
 * FUNCTION: TrySetUnknownWarpStatus
 *
 * PURPOSE: Sets or clears an unknown flag. Never actually set because the
 *          map list is empty. Remnant of a cut feature.
 */
static void TrySetUnknownWarpStatus(void)
{
    if (!IsCurMapInEmptyList())
        gSaveBlock2Ptr->specialSaveWarpFlags &= ~(UNK_SPECIAL_SAVE_WARP_FLAG_3);
    else
        gSaveBlock2Ptr->specialSaveWarpFlags |= UNK_SPECIAL_SAVE_WARP_FLAG_3;
}

/**
 * FUNCTION: TrySetMapSaveWarpStatus
 *
 * PURPOSE: Master function that updates all save warp flags based on the
 *          player's current map. Called when saving the game.
 */
void TrySetMapSaveWarpStatus(void)
{
    TrySetPokeCenterWarpStatus();
    TrySetReloadWarpStatus();
    TrySetUnknownWarpStatus();
}

/**
 * FUNCTION: SetUnlockedPokedexFlags
 *
 * PURPOSE: Sets flags indicating the Pokedex features are unlocked. These flags
 *          are used for communication with GameCube games (Pokemon Colosseum/XD).
 *
 * GAME LOGIC:
 * gcnLinkFlags (GameCube Network Link Flags) control what features are available
 * when connecting a GBA to a GameCube via the link cable:
 * - Bit 0: Basic Pokedex unlocked
 * - Bit 4: Unknown Pokedex feature
 * - Bit 5: Unknown Pokedex feature
 */
void SetUnlockedPokedexFlags(void)
{
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 0);   /* Basic Pokedex unlock flag */
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 4);   /* Additional Pokedex feature flag */
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 5);   /* Additional Pokedex feature flag */
}

/**
 * FUNCTION: SetPostgameFlags
 *
 * PURPOSE: Sets flags marking the player as Champion, enabling postgame content
 *          and additional GameCube connectivity features.
 *
 * GAME LOGIC:
 * Called after beating the Elite Four and Champion. Enables:
 * - CHAMPION_SAVEWARP: Continue game starts from Hall of Fame sequence
 * - GCN link bits 1-3: Unlock trading/battling features with GameCube games
 * - GCN link bit 15: Unknown postgame flag (possibly National Dex related)
 */
void SetPostgameFlags(void)
{
    gSaveBlock2Ptr->specialSaveWarpFlags |= CHAMPION_SAVEWARP;  /* Player is Champion */
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 1);   /* Postgame GCN feature 1 */
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 2);   /* Postgame GCN feature 2 */
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 3);   /* Postgame GCN feature 3 */
    gSaveBlock2Ptr->gcnLinkFlags |= (1 << 15);  /* Postgame GCN feature (possibly Nat Dex) */
}
