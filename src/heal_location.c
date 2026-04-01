/**
 * @file heal_location.c
 * @brief Heal Location and Whiteout Respawn System
 *
 * FILE OVERVIEW:
 * This file manages "heal locations" — the places where the player respawns after
 * losing all their Pokemon (a "whiteout" or "blackout"). In Pokemon FireRed, when
 * all your Pokemon faint, you wake up at the last Pokemon Center you visited (or
 * at your house in Pallet Town at the start of the game).
 *
 * The system works with three parallel data arrays (defined in data/heal_locations.h):
 * 1. sHealLocations — The map/coordinates of each checkpoint (used for Fly destinations)
 * 2. sWhiteoutRespawnHealCenterMapIdxs — Which Pokemon Center map to respawn on
 * 3. sWhiteoutRespawnHealerNpcIds — Which NPC (nurse/mom) to interact with on respawn
 *
 * GAME LOGIC:
 * When you heal at a Pokemon Center, the game saves that location as your
 * "lastHealLocation." If you whiteout, the game looks up the corresponding
 * respawn point — usually the same Pokemon Center, but the player's position
 * within the center is specifically set so the cutscene plays correctly
 * (player appears at the counter talking to the nurse).
 */
#include "global.h"
#include "heal_location.h"
#include "event_data.h"
#include "constants/maps.h"
#include "constants/map_event_ids.h"
#include "constants/heal_locations.h"

static void SetWhiteoutRespawnHealerNpcAsLastTalked(u32 healLocationIdx);

// Arrays described here because mapjson will overrwrite the below data file

// sHealLocations
// This array defines the fly points for unlocked spawns.

// sWhiteoutRespawnHealCenterMapIdxs
// This array defines the map where you actually respawn when you white out,
// based on where you last checkpointed.
// This is either the player's house or a Pokémon Center.
// The data are u16 instead of u8 for reasons unknown.

// sWhiteoutRespawnHealerNpcIds
// When you respawn, your character scurries back to either their house
// or a Pokémon Center, and hands their fainted Pokémon to their mother
// or the Nurse for healing.
// This array defines the index of the NPC on the map defined above
// with whom your character interacts in this cutscene.

#include "data/heal_locations.h"

/**
 * FUNCTION: GetHealLocationIndexFromMapGroupAndNum
 *
 * PURPOSE: Finds the heal location index for a given map by searching
 *          the sHealLocations array.
 *
 * GAME LOGIC:
 * Maps in Pokemon FireRed are identified by a (mapGroup, mapNum) pair rather
 * than a single ID. This function searches all registered heal locations to
 * find which one corresponds to the given map. The returned index is 1-based
 * (not 0-based), with 0 (HEAL_LOCATION_NONE) meaning "not found."
 *
 * @param mapGroup — The map group number (maps are organized into groups by area)
 * @param mapNum — The map number within its group
 * RETURNS: 1-based heal location index, or HEAL_LOCATION_NONE (0) if not found
 */
static u32 GetHealLocationIndexFromMapGroupAndNum(u16 mapGroup, u16 mapNum)
{
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sHealLocations); i++) {
        if (sHealLocations[i].mapGroup == mapGroup && sHealLocations[i].mapNum == mapNum)
        {
            return i + 1;  /* Return 1-based index (0 is reserved for "none") */
        }
    }

    return HEAL_LOCATION_NONE;  /* No heal location exists on this map */
}

/**
 * FUNCTION: GetHealLocationPointerFromMapGroupAndNum
 *
 * PURPOSE: Returns a pointer to the HealLocation struct for a given map.
 *
 * @param mapGroup — The map group number
 * @param mapNum — The map number within its group
 * RETURNS: Pointer to the HealLocation entry, or NULL if no heal location on this map
 */
static const struct HealLocation * GetHealLocationPointerFromMapGroupAndNum(u16 mapGroup, u16 mapNum)
{
    u32 i = GetHealLocationIndexFromMapGroupAndNum(mapGroup, mapNum);
    if (i == HEAL_LOCATION_NONE)
        return NULL;

    return &sHealLocations[i - 1];  /* Convert 1-based index back to 0-based for array access */
}

/**
 * FUNCTION: GetHealLocation
 *
 * PURPOSE: Returns a pointer to a HealLocation struct by its 1-based index.
 *
 * GAME LOGIC:
 * This is the public interface for looking up heal location data by index.
 * Used by the save system and the Fly map to get coordinates and map info.
 *
 * @param idx — 1-based heal location index (from constants/heal_locations.h)
 * RETURNS: Pointer to the HealLocation struct, or NULL if idx is invalid
 */
const struct HealLocation * GetHealLocation(u32 idx)
{
    if (idx == HEAL_LOCATION_NONE)
        return NULL;
    if (idx > ARRAY_COUNT(sHealLocations))
        return NULL;
    return &sHealLocations[idx - 1];
}

/**
 * FUNCTION: SetWhiteoutRespawnWarpAndHealerNpc
 *
 * PURPOSE: Configures the warp destination and healer NPC for when the player
 *          whites out (loses all Pokemon).
 *
 * GAME LOGIC:
 * After a whiteout, the player is warped to a specific location (usually a Pokemon
 * Center) and positioned at specific (x, y) coordinates so the respawn cutscene
 * plays correctly. Different locations have different player positions:
 * - Player's House in Pallet Town: (8, 5) — standing near Mom
 * - Indigo Plateau Pokemon Center: (13, 12) — at the counter
 * - One Island Pokemon Center: (5, 4) — at the counter (smaller center)
 * - Trainer Tower Lobby: (4, 11) — at the entrance area
 * - All other Pokemon Centers: (7, 4) — standard counter position
 *
 * The Trainer Tower is a special case — if the player was mid-challenge, the
 * tower scene state is reset, and the warp goes to the tower lobby instead
 * of the last Pokemon Center.
 *
 * The warpId of 0xFF (WARP_ID_NONE) means "use the x/y coordinates directly"
 * rather than using a predefined warp pad destination.
 *
 * @param warp — WarpData struct to fill with the respawn destination
 */
void SetWhiteoutRespawnWarpAndHealerNpc(struct WarpData * warp)
{
    u32 healLocationIdx;

    /* Special case: Trainer Tower challenge in progress */
    if (VarGet(VAR_MAP_SCENE_TRAINER_TOWER) == 1)
    {
        /* If the player hasn't spoken to the tower owner yet, reset the tower state */
        if (!gSaveBlock1Ptr->trainerTower[gSaveBlock1Ptr->towerChallengeId].spokeToOwner)
            VarSet(VAR_MAP_SCENE_TRAINER_TOWER, 0);
        gSpecialVar_LastTalked = 1;  /* NPC ID 1 in the Trainer Tower lobby */
        warp->x = 4;
        warp->y = 11;
        warp->mapGroup = MAP_GROUP(MAP_TRAINER_TOWER_LOBBY);
        warp->mapNum = MAP_NUM(MAP_TRAINER_TOWER_LOBBY);
        warp->warpId = 0xFF;  /* WARP_ID_NONE — use raw x/y coordinates */
    }
    else
    {
        /* Normal case: look up the heal location from the player's last checkpoint */
        healLocationIdx = GetHealLocationIndexFromMapGroupAndNum(gSaveBlock1Ptr->lastHealLocation.mapGroup, gSaveBlock1Ptr->lastHealLocation.mapNum);
#ifdef BUGFIX
        /* Original game had a bug: if healLocationIdx is 0 (NONE), the subtraction
         * below would access index -1, reading garbage memory. This fix prevents that. */
        // Avoid out of bounds read
        if (healLocationIdx == HEAL_LOCATION_NONE)
            return;
#endif
        /* Look up which map the player should respawn on */
        warp->mapGroup = sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][0];
        warp->mapNum = sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][1];
        warp->warpId = WARP_ID_NONE;

        /* Set player position based on which building they're respawning in.
         * Each building has a different layout, so the player needs to be placed
         * at the correct coordinates to face the healer NPC. */
        if (sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][0] == MAP_GROUP(MAP_PALLET_TOWN_PLAYERS_HOUSE_1F) && sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][1] == MAP_NUM(MAP_PALLET_TOWN_PLAYERS_HOUSE_1F))
        {
            /* Player's house — position near Mom */
            warp->x = 8;
            warp->y = 5;
        }
        else if (sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][0] == MAP_GROUP(MAP_INDIGO_PLATEAU_POKEMON_CENTER_1F) && sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][1] == MAP_NUM(MAP_INDIGO_PLATEAU_POKEMON_CENTER_1F))
        {
            /* Indigo Plateau center has a wider layout */
            warp->x = 13;
            warp->y = 12;
        }
        else if (sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][0] == MAP_GROUP(MAP_ONE_ISLAND_POKEMON_CENTER_1F) && sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][1] == MAP_NUM(MAP_ONE_ISLAND_POKEMON_CENTER_1F))
        {
            /* Sevii Islands center has a slightly different layout */
            warp->x = 5;
            warp->y = 4;
        }
        else if (sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][0] == MAP_GROUP(MAP_TRAINER_TOWER_LOBBY) && sWhiteoutRespawnHealCenterMapIdxs[healLocationIdx - 1][1] == MAP_NUM(MAP_TRAINER_TOWER_LOBBY))
        {
            /* Trainer Tower lobby — also resets the tower challenge state */
            warp->x = 4;
            warp->y = 11;
            VarSet(VAR_MAP_SCENE_TRAINER_TOWER, 0);
        }
        else
        {
            /* Default for all standard Pokemon Centers */
            warp->x = 7;
            warp->y = 4;
        }
        /* Set which NPC the player will talk to after respawning (nurse or mom) */
        SetWhiteoutRespawnHealerNpcAsLastTalked(healLocationIdx);
    }
}

/**
 * FUNCTION: SetWhiteoutRespawnHealerNpcAsLastTalked
 *
 * PURPOSE: Sets gSpecialVar_LastTalked to the healer NPC's local ID so the
 *          respawn cutscene script knows which NPC to interact with.
 *
 * GAME LOGIC:
 * gSpecialVar_LastTalked is a special variable used by the scripting engine
 * to track which NPC the player last interacted with. Setting it here ensures
 * the whiteout script can reference the correct nurse/mom character.
 *
 * @param healLocationIdx — 1-based heal location index
 */
static void SetWhiteoutRespawnHealerNpcAsLastTalked(u32 healLocationIdx)
{
    gSpecialVar_LastTalked = sWhiteoutRespawnHealerNpcIds[healLocationIdx - 1];
}
