/**
 * @file roamer.c
 * @brief Roaming Pokemon System (Entei/Raikou/Suicune)
 *
 * FILE OVERVIEW:
 * This file implements the "roaming Pokemon" mechanic — a legendary Pokemon
 * (Entei, Raikou, or Suicune depending on the player's starter) that moves
 * between routes on the Kanto map each time the player changes areas. The player
 * must track its location using the Pokedex and encounter it in wild grass.
 *
 * GAME LOGIC — HOW ROAMING WORKS:
 * 1. After a story event, one of the three Legendary Beasts begins roaming
 * 2. The roamer has a current route location, visible in the Pokedex area map
 * 3. Every time the player transitions between maps, the roamer moves to an
 *    adjacent route (using the sRoamerLocations adjacency table)
 * 4. There's a 1/16 chance per move that the roamer jumps to a completely
 *    random route instead of an adjacent one
 * 5. When the player is on the same route as the roamer, there's a 1/4 chance
 *    of encountering it in wild grass
 * 6. After running from battle or the roamer fleeing, it moves to a new location
 * 7. The roamer retains its HP and status between encounters — if you got it to
 *    half HP, it stays at half HP next time you find it
 *
 * LOCATION GRAPH:
 * The sRoamerLocations table defines an adjacency graph — each row starts with
 * a route, followed by routes the roamer can move to from there. This roughly
 * mirrors Kanto's actual geography (Route 1 connects to Route 2, Route 21, etc.)
 *
 * ANTI-PATTERN NOTE:
 * The roamer avoids returning to a route it was on 2 moves ago (tracked in
 * sLocationHistory). This prevents the annoying case where the roamer just
 * bounces between two adjacent routes.
 */
#include "global.h"
#include "random.h"
#include "overworld.h"
#include "field_specials.h"
#include "constants/maps.h"
#include "constants/region_map_sections.h"

// Despite having a variable to track it, the roamer is
// hard-coded to only ever be in map group 3
#define ROAMER_MAP_GROUP 3

enum
{
    MAP_GRP, // map group
    MAP_NUM, // map number
};

/* Shorthand macro to access the roamer data structure in the save block */
#define ROAMER (&gSaveBlock1Ptr->roamer)

/* Location tracking arrays stored in EWRAM (external work RAM).
 * sLocationHistory: The last 3 map locations the player has been on (for preventing
 *                   the roamer from backtracking to a recently-visited location).
 * sRoamerLocation: The roamer's current map [group, number]. */
EWRAM_DATA u8 sLocationHistory[3][2] = {};
EWRAM_DATA u8 sRoamerLocation[2] = {};

/* Sentinel value for empty slots in the location adjacency table */
#define ___ MAP_NUM(MAP_UNDEFINED) // For empty spots in the location table

// Note: There are two potential softlocks that can occur with this table if its maps are
//       changed in particular ways. They can be avoided by ensuring the following:
//       - There must be at least 2 location sets that start with a different map,
//         i.e. every location set cannot start with the same map. This is because of
//         the while loop in RoamerMoveToOtherLocationSet.
//       - Each location set must have at least 3 unique maps. This is because of
//         the while loop in RoamerMove. In this loop the first map in the set is
//         ignored, and an additional map is ignored if the roamer was there recently.
//       - Additionally, while not a softlock, it's worth noting that if for any
//         map in the location table there is not a location set that starts with
//         that map then the roamer will be significantly less likely to move away
//         from that map when it lands there.
/**
 * ROAMER LOCATION ADJACENCY TABLE
 *
 * Each row represents: [current_route, adjacent_route_1, adjacent_route_2, ...]
 * The first element is the route this row is "for" — when the roamer is on that route,
 * it can move to any of the other routes in the same row.
 * Empty slots are filled with ___ (MAP_UNDEFINED).
 *
 * This table models Kanto's route connectivity. For example, Route 1 (first row)
 * connects to Routes 2, 21, and 22 — matching the actual game map geography.
 */
static const u8 sRoamerLocations[][7] = {
    {MAP_NUM(MAP_ROUTE1), MAP_NUM(MAP_ROUTE2), MAP_NUM(MAP_ROUTE21_NORTH), MAP_NUM(MAP_ROUTE22), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE2), MAP_NUM(MAP_ROUTE1), MAP_NUM(MAP_ROUTE3), MAP_NUM(MAP_ROUTE22), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE3), MAP_NUM(MAP_ROUTE2), MAP_NUM(MAP_ROUTE4), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE4), MAP_NUM(MAP_ROUTE3), MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE9), MAP_NUM(MAP_ROUTE24), ___, ___},
    {MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE4), MAP_NUM(MAP_ROUTE6), MAP_NUM(MAP_ROUTE7), MAP_NUM(MAP_ROUTE8), MAP_NUM(MAP_ROUTE9), MAP_NUM(MAP_ROUTE24)},
    {MAP_NUM(MAP_ROUTE6), MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE7), MAP_NUM(MAP_ROUTE8), MAP_NUM(MAP_ROUTE11), ___, ___},
    {MAP_NUM(MAP_ROUTE7), MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE6), MAP_NUM(MAP_ROUTE8), MAP_NUM(MAP_ROUTE16), ___, ___},
    {MAP_NUM(MAP_ROUTE8), MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE6), MAP_NUM(MAP_ROUTE7), MAP_NUM(MAP_ROUTE10), MAP_NUM(MAP_ROUTE12), ___},
    {MAP_NUM(MAP_ROUTE9), MAP_NUM(MAP_ROUTE4), MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE10), MAP_NUM(MAP_ROUTE24), ___, ___},
    {MAP_NUM(MAP_ROUTE10), MAP_NUM(MAP_ROUTE8), MAP_NUM(MAP_ROUTE9), MAP_NUM(MAP_ROUTE12), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE11), MAP_NUM(MAP_ROUTE6), MAP_NUM(MAP_ROUTE12), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE12), MAP_NUM(MAP_ROUTE10), MAP_NUM(MAP_ROUTE11), MAP_NUM(MAP_ROUTE13), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE13), MAP_NUM(MAP_ROUTE12), MAP_NUM(MAP_ROUTE14), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE14), MAP_NUM(MAP_ROUTE13), MAP_NUM(MAP_ROUTE15), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE15), MAP_NUM(MAP_ROUTE14), MAP_NUM(MAP_ROUTE18), MAP_NUM(MAP_ROUTE19), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE16), MAP_NUM(MAP_ROUTE7), MAP_NUM(MAP_ROUTE17), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE17), MAP_NUM(MAP_ROUTE16), MAP_NUM(MAP_ROUTE18), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE18), MAP_NUM(MAP_ROUTE15), MAP_NUM(MAP_ROUTE17), MAP_NUM(MAP_ROUTE19), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE19), MAP_NUM(MAP_ROUTE15), MAP_NUM(MAP_ROUTE18), MAP_NUM(MAP_ROUTE20), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE20), MAP_NUM(MAP_ROUTE19), MAP_NUM(MAP_ROUTE21_NORTH), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE21_NORTH), MAP_NUM(MAP_ROUTE1), MAP_NUM(MAP_ROUTE20), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE22), MAP_NUM(MAP_ROUTE1), MAP_NUM(MAP_ROUTE2), MAP_NUM(MAP_ROUTE23), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE23), MAP_NUM(MAP_ROUTE22), MAP_NUM(MAP_ROUTE2), ___, ___, ___, ___},
    {MAP_NUM(MAP_ROUTE24), MAP_NUM(MAP_ROUTE4), MAP_NUM(MAP_ROUTE5), MAP_NUM(MAP_ROUTE9), ___, ___, ___},
    {MAP_NUM(MAP_ROUTE25), MAP_NUM(MAP_ROUTE24), MAP_NUM(MAP_ROUTE9), ___, ___, ___, ___},
    {___, ___, ___, ___, ___, ___, ___}  /* Sentinel row marking end of table */
};

#undef ___
#define NUM_LOCATION_SETS (ARRAY_COUNT(sRoamerLocations) - 1)  /* Exclude sentinel row */
#define NUM_LOCATIONS_PER_SET (ARRAY_COUNT(sRoamerLocations[0]))  /* 7 entries per row */

/**
 * FUNCTION: ClearRoamerData
 *
 * PURPOSE: Resets all roamer data to zero. Called when starting a new game.
 *
 * GAME LOGIC:
 * The compound literal (struct Roamer){} creates a zero-initialized Roamer struct.
 * The location history is also cleared so the roamer has no movement constraints
 * when first initialized.
 */
void ClearRoamerData(void)
{
    u32 i;
    *ROAMER = (struct Roamer){};
    sRoamerLocation[MAP_GRP] = 0;
    sRoamerLocation[MAP_NUM] = 0;
    for (i = 0; i < ARRAY_COUNT(sLocationHistory); i++)
    {
        sLocationHistory[i][MAP_GRP] = 0;
        sLocationHistory[i][MAP_NUM] = 0;
    }
}

/**
 * GetRoamerSpecies macro
 *
 * Determines which Legendary Beast roams based on the player's starter Pokemon.
 * This is a GCC statement expression (the ({...}) syntax) that acts like a function
 * but works as a macro:
 * - Squirtle (default) -> Raikou
 * - Bulbasaur -> Entei
 * - Charmander -> Suicune
 *
 * The logic is: each Beast is strong against the player's starter type.
 * Fire (Entei) vs Grass (Bulbasaur), Water (Suicune) vs Fire (Charmander),
 * Electric (Raikou) vs Water (Squirtle).
 */
#define GetRoamerSpecies() ({\
    u16 a;\
    switch (GetStarterSpecies())\
    {\
    default:\
        a = SPECIES_RAIKOU;\
        break;\
    case SPECIES_BULBASAUR:\
        a = SPECIES_ENTEI;\
        break;\
    case SPECIES_CHARMANDER:\
        a = SPECIES_SUICUNE;\
        break;\
    }\
    a;\
})

/**
 * FUNCTION: CreateInitialRoamerMon
 *
 * PURPOSE: Creates the roaming Pokemon with appropriate stats and places it
 *          on a random starting route.
 *
 * GAME LOGIC:
 * The roamer is created as a level 50 Pokemon with random IVs and the player's
 * trainer ID. Key stats are saved into the Roamer struct in the save block:
 * - Species, level, personality, IVs: define the Pokemon's identity
 * - HP: tracked between encounters so damage persists
 * - Status: poison, burn, etc. also persist between encounters
 * - Contest stats (cool, beauty, cute, smart, tough): preserved for completeness
 * - active: set to TRUE to indicate the roamer is currently roaming
 *
 * The starting location is randomly chosen from the first element of each row
 * in the location table (i.e., a random route).
 */
void CreateInitialRoamerMon(void)
{
    struct Pokemon * mon = &gEnemyParty[0];
    u16 species = GetRoamerSpecies();

    /* Create a temporary Pokemon to generate stats, then copy key data to the Roamer struct */
    CreateMon(mon, species, 50, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
    ROAMER->species = species;
    ROAMER->level = 50;
    ROAMER->status = 0;       /* No status conditions initially */
    ROAMER->active = TRUE;    /* The roamer is now actively moving around Kanto */
    ROAMER->ivs = GetMonData(mon, MON_DATA_IVS);
    ROAMER->personality = GetMonData(mon, MON_DATA_PERSONALITY);
    ROAMER->hp = GetMonData(mon, MON_DATA_MAX_HP);  /* Start at full HP */
    ROAMER->cool = GetMonData(mon, MON_DATA_COOL);
    ROAMER->beauty = GetMonData(mon, MON_DATA_BEAUTY);
    ROAMER->cute = GetMonData(mon, MON_DATA_CUTE);
    ROAMER->smart = GetMonData(mon, MON_DATA_SMART);
    ROAMER->tough = GetMonData(mon, MON_DATA_TOUGH);

    /* Place the roamer on a random starting route */
    sRoamerLocation[MAP_GRP] = ROAMER_MAP_GROUP;
    sRoamerLocation[MAP_NUM] = sRoamerLocations[Random() % NUM_LOCATION_SETS][0];
}

/**
 * FUNCTION: InitRoamer
 *
 * PURPOSE: Entry point for starting the roamer system. Clears old data and
 *          creates a fresh roaming Pokemon.
 */
void InitRoamer(void)
{
    ClearRoamerData();
    CreateInitialRoamerMon();
}

/**
 * FUNCTION: UpdateLocationHistoryForRoamer
 *
 * PURPOSE: Shifts the player's location history forward and records the current
 *          map. Called every time the player transitions to a new map.
 *
 * GAME LOGIC:
 * The history is a 3-slot FIFO (First In, First Out) buffer:
 * - Slot 0: Where the player is NOW
 * - Slot 1: Where the player was LAST
 * - Slot 2: Where the player was 2 maps ago
 * The roamer avoids moving to the map stored in slot 2 (the player's location
 * from 2 transitions ago), which prevents trivial tracking strategies.
 */
void UpdateLocationHistoryForRoamer(void)
{
    /* Shift history: slot 1 -> slot 2, slot 0 -> slot 1 */
    sLocationHistory[2][MAP_GRP] = sLocationHistory[1][MAP_GRP];
    sLocationHistory[2][MAP_NUM] = sLocationHistory[1][MAP_NUM];

    sLocationHistory[1][MAP_GRP] = sLocationHistory[0][MAP_GRP];
    sLocationHistory[1][MAP_NUM] = sLocationHistory[0][MAP_NUM];

    /* Record current location in slot 0 */
    sLocationHistory[0][MAP_GRP] = gSaveBlock1Ptr->location.mapGroup;
    sLocationHistory[0][MAP_NUM] = gSaveBlock1Ptr->location.mapNum;
}

/**
 * FUNCTION: RoamerMoveToOtherLocationSet
 *
 * PURPOSE: Moves the roamer to a completely different area of the map by choosing
 *          a random row from the location table whose starting route differs from
 *          the roamer's current route.
 *
 * GAME LOGIC:
 * This is the "long-distance jump" — with a 1/16 chance per move, the roamer
 * teleports to a random far-away route instead of moving to an adjacent one.
 * This prevents the player from easily cornering the roamer by approaching from
 * one direction.
 *
 * WARNING: The while(1) loop will infinite-loop if all location sets start with
 * the same map (see the note on sRoamerLocations above).
 */
void RoamerMoveToOtherLocationSet(void)
{
    u8 mapNum = 0;

    if (!ROAMER->active)
        return;  /* Roamer has been caught or defeated — don't move it */

    sRoamerLocation[MAP_GRP] = ROAMER_MAP_GROUP;

    // Choose a location set that starts with a map
    // different from the roamer's current map
    while (1)
    {
        mapNum = sRoamerLocations[Random() % NUM_LOCATION_SETS][0];
        if (sRoamerLocation[MAP_NUM] != mapNum)
        {
            sRoamerLocation[MAP_NUM] = mapNum;
            return;
        }
    }
}


/**
 * FUNCTION: RoamerMove
 *
 * PURPOSE: Moves the roamer to a new route. Called every time the player
 *          transitions between overworld maps.
 *
 * GAME LOGIC:
 * Two movement modes:
 * 1. With 1/16 probability: Jump to a completely random different area
 *    (via RoamerMoveToOtherLocationSet)
 * 2. Otherwise: Move to an adjacent route from the location table
 *    - Find the row that starts with the roamer's current route
 *    - Randomly pick one of the other routes in that row (index 1-6)
 *    - Exclude MAP_UNDEFINED entries (empty slots)
 *    - Exclude the route the player was on 2 transitions ago (prevents
 *      the roamer from always running to where the player just was)
 *
 * WARNING: The inner while(1) loop can infinite-loop if a location set has
 * fewer than 3 unique non-undefined maps (see the note on sRoamerLocations).
 */
void RoamerMove(void)
{
    u8 locSet = 0;

    if ((Random() % 16) == 0)
    {
        /* 1/16 chance: teleport to a random distant route */
        RoamerMoveToOtherLocationSet();
    }
    else
    {
        if (!ROAMER->active)
            return;

        while (locSet < NUM_LOCATION_SETS)
        {
            // Find the location set that starts with the roamer's current map
            if (sRoamerLocation[MAP_NUM] == sRoamerLocations[locSet][0])
            {
                u8 mapNum;
                while (1)
                {
                    // Choose a new map (excluding the first) within this set
                    // Also exclude a map if the roamer was there 2 moves ago
                    mapNum = sRoamerLocations[locSet][(Random() % (NUM_LOCATIONS_PER_SET - 1)) + 1];
                    if (!(sLocationHistory[2][MAP_GRP] == ROAMER_MAP_GROUP
                       && sLocationHistory[2][MAP_NUM] == mapNum)
                       && mapNum != MAP_NUM(MAP_UNDEFINED))
                        break;
                }
                sRoamerLocation[MAP_NUM] = mapNum;
                return;
            }
            locSet++;
        }
    }
}

/**
 * FUNCTION: IsRoamerAt
 *
 * PURPOSE: Checks whether the roaming Pokemon is currently on a specific map.
 *
 * @param mapGroup — Map group to check
 * @param mapNum — Map number to check
 * RETURNS: TRUE if the active roamer is on this map
 */
bool8 IsRoamerAt(u8 mapGroup, u8 mapNum)
{
    if (ROAMER->active && mapGroup == sRoamerLocation[MAP_GRP] && mapNum == sRoamerLocation[MAP_NUM])
        return TRUE;
    else
        return FALSE;
}

/**
 * FUNCTION: CreateRoamerMonInstance
 *
 * PURPOSE: Creates a full Pokemon struct from the roamer's saved data for use
 *          in a wild encounter battle.
 *
 * GAME LOGIC:
 * When the player encounters the roamer in wild grass, we need to reconstruct
 * a complete Pokemon from the compact Roamer struct. The roamer's personality
 * and IVs are preserved (so it looks the same each time — same nature, same
 * shiny status), and its HP and status carry over from previous encounters.
 *
 * BUG NOTE: The original code passes &ROAMER->status (a u8*) to SetMonData
 * which expects a u32*. This reads 3 extra bytes past the status field
 * (cool, beauty, cute) as part of the status value. The BUGFIX version
 * copies the status to a local u32 first.
 */
void CreateRoamerMonInstance(void)
{
    u32 status;
    struct Pokemon *mon = &gEnemyParty[0];
    ZeroEnemyPartyMons();  /* Clear the enemy party */

    /* Create a Pokemon with the roamer's exact personality and IVs */
    CreateMonWithIVsPersonality(mon, ROAMER->species, ROAMER->level, ROAMER->ivs, ROAMER->personality);

// The roamer's status field is u8, but SetMonData expects status to be u32, so will set the roamer's status
// using the status field and the following 3 bytes (cool, beauty, and cute).
#ifdef BUGFIX
    status = ROAMER->status;  /* Copy u8 to u32 to avoid reading garbage bytes */
    SetMonData(mon, MON_DATA_STATUS, &status);
#else
    SetMonData(mon, MON_DATA_STATUS, &ROAMER->status);  /* Bug: reads 3 extra bytes */
#endif

    /* Restore the roamer's current HP and contest stats */
    SetMonData(mon, MON_DATA_HP, &ROAMER->hp);
    SetMonData(mon, MON_DATA_COOL, &ROAMER->cool);
    SetMonData(mon, MON_DATA_BEAUTY, &ROAMER->beauty);
    SetMonData(mon, MON_DATA_CUTE, &ROAMER->cute);
    SetMonData(mon, MON_DATA_SMART, &ROAMER->smart);
    SetMonData(mon, MON_DATA_TOUGH, &ROAMER->tough);
}

/**
 * FUNCTION: TryStartRoamerEncounter
 *
 * PURPOSE: Attempts to start a wild encounter with the roaming Pokemon.
 *          Called when the player takes a step in grass on the roamer's current route.
 *
 * GAME LOGIC:
 * Two conditions must be met for an encounter:
 * 1. The roamer must be on the player's current map
 * 2. A 1/4 random chance must succeed
 * This means even when on the right route, the player doesn't always encounter
 * the roamer, making the hunt more challenging.
 *
 * RETURNS: TRUE if a roamer encounter should start, FALSE otherwise
 */
bool8 TryStartRoamerEncounter(void)
{
    if (IsRoamerAt(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum) == TRUE && (Random() % 4) == 0)
    {
        CreateRoamerMonInstance();  /* Build the enemy Pokemon for battle */
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/**
 * FUNCTION: UpdateRoamerHPStatus
 *
 * PURPOSE: After a battle where the roamer fled or the player ran, saves the
 *          roamer's current HP and status, then moves it to a new area.
 *
 * GAME LOGIC:
 * This is called when a roamer battle ends without the roamer being caught or
 * defeated. The damage dealt and status inflicted persist — next time the player
 * finds the roamer, it will still be at reduced HP with any status conditions.
 * After updating, the roamer flees to a completely different area.
 *
 * @param mon — The roamer's Pokemon struct from the just-ended battle
 */
void UpdateRoamerHPStatus(struct Pokemon *mon)
{
    ROAMER->hp = GetMonData(mon, MON_DATA_HP);
    ROAMER->status = GetMonData(mon, MON_DATA_STATUS);

    RoamerMoveToOtherLocationSet();  /* Flee to a distant route after battle */
}

/**
 * FUNCTION: SetRoamerInactive
 *
 * PURPOSE: Marks the roamer as no longer active — called when the roamer is
 *          caught or defeated. It will no longer appear on the map.
 */
void SetRoamerInactive(void)
{
    ROAMER->active = FALSE;
}

/**
 * FUNCTION: GetRoamerLocation
 *
 * PURPOSE: Gets the roamer's current map location. Used by the Pokedex area
 *          display to show the roamer's position on the region map.
 *
 * @param mapGroup — Output: receives the roamer's current map group
 * @param mapNum — Output: receives the roamer's current map number
 */
void GetRoamerLocation(u8 *mapGroup, u8 *mapNum)
{
    *mapGroup = sRoamerLocation[MAP_GRP];
    *mapNum = sRoamerLocation[MAP_NUM];
}

/**
 * FUNCTION: GetRoamerLocationMapSectionId
 *
 * PURPOSE: Returns the region map section ID for the roamer's current location.
 *          This is used to display the roamer's position on the Pokedex area map.
 *
 * RETURNS: MAPSEC_NONE if the roamer is inactive; otherwise the map section ID
 *          (e.g., MAPSEC_ROUTE_1, MAPSEC_ROUTE_2, etc.)
 */
u16 GetRoamerLocationMapSectionId(void)
{
    if (!ROAMER->active)
        return MAPSEC_NONE;
    return Overworld_GetMapHeaderByGroupAndId(sRoamerLocation[MAP_GRP], sRoamerLocation[MAP_NUM])->regionMapSectionId;
}
