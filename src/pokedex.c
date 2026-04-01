/**
 * @file pokedex.c
 * @brief Pokedex Data Queries — Counting, Completion Checks, and Entry Lookups
 *
 * FILE OVERVIEW:
 * This file provides utility functions for querying Pokedex data: looking up
 * Pokemon entries (height, weight, category), counting how many Pokemon have
 * been seen or caught, and checking completion milestones (all Kanto, all Hoenn,
 * all National). These functions are used by the Pokedex UI, diploma checks,
 * and various game scripts.
 *
 * GAME LOGIC:
 * The Pokedex tracks two flags per Pokemon: "seen" (encountered in battle or
 * shown by an NPC) and "caught" (successfully captured). The game has three
 * regional Pokedex groupings:
 *   - Kanto Dex: Pokemon #1-151 (original 151)
 *   - Johto Dex: Pokemon #1-251 (adds Gold/Silver Pokemon)
 *   - National Dex: Pokemon #1-386 (all Pokemon through Gen 3)
 * Completion checks intentionally exclude certain mythical Pokemon
 * (Mew, Celebi, Jirachi, Deoxys, Lugia, Ho-Oh) since they require special events.
 */
#include "global.h"
#include "pokedex.h"
#include "pokedex_screen.h"

/**
 * FUNCTION: GetPokedexCategoryName
 *
 * PURPOSE: Returns the category name string for a Pokedex entry (e.g., "Seed Pokemon"
 *          for Bulbasaur). Currently unused in the game code.
 */
// Unused
const u8 *GetPokedexCategoryName(u16 dexNum)
{
    return gPokedexEntries[dexNum].categoryName;
}

/**
 * FUNCTION: GetPokedexHeightWeight
 *
 * PURPOSE: Retrieves the height or weight of a Pokemon from its Pokedex entry.
 *
 * PARAMETERS:
 * @param dexNum — National Pokedex number
 * @param data   — 0 for height, 1 for weight
 *
 * RETURNS: The requested value, or 1 as a safe default for invalid data param.
 */
u16 GetPokedexHeightWeight(u16 dexNum, u8 data)
{
    switch (data)
    {
    case 0:  // height
        return gPokedexEntries[dexNum].height;
    case 1:  // weight
        return gPokedexEntries[dexNum].weight;
    default:
        return 1;
    }
}

/**
 * FUNCTION: GetSetPokedexFlag
 *
 * PURPOSE: Wrapper to check or set a Pokemon's seen/caught flag in the Pokedex.
 *          Delegates to DexScreen_GetSetPokedexFlag with a mode of 0 (query only).
 *
 * PARAMETERS:
 * @param nationalDexNo — The Pokemon's National Dex number (1-based)
 * @param caseID        — FLAG_GET_SEEN or FLAG_GET_CAUGHT
 *
 * RETURNS: Nonzero if the flag is set, 0 if not.
 */
s8 GetSetPokedexFlag(u16 nationalDexNo, u8 caseID)
{
    return DexScreen_GetSetPokedexFlag(nationalDexNo, caseID, 0);
}

/**
 * FUNCTION: GetNationalPokedexCount
 *
 * PURPOSE: Counts how many Pokemon in the full National Dex have been seen or caught.
 *
 * PARAMETERS:
 * @param caseID — FLAG_GET_SEEN to count seen, FLAG_GET_CAUGHT to count caught
 *
 * RETURNS: The count of Pokemon matching the requested flag.
 */
u16 GetNationalPokedexCount(u8 caseID)
{
    u16 count = 0;
    u16 i;

    for (i = 0; i < NATIONAL_DEX_COUNT; i++)
    {
        switch (caseID)
        {
        case FLAG_GET_SEEN:
            if (GetSetPokedexFlag(i + 1, FLAG_GET_SEEN))
                count++;
            break;
        case FLAG_GET_CAUGHT:
            if (GetSetPokedexFlag(i + 1, FLAG_GET_CAUGHT))
                count++;
            break;
        }
    }
    return count;
}

/*
u16 GetHoennPokedexCount(u8 caseID)
{
    u16 count = 0;
    u16 i;

    for (i = 0; i < HOENN_DEX_COUNT; i++)
    {
        switch (caseID)
        {
        case FLAG_GET_SEEN:
            if (GetSetPokedexFlag(HoennToNationalOrder(i + 1), FLAG_GET_SEEN))
                count++;
            break;
        case FLAG_GET_CAUGHT:
            if (GetSetPokedexFlag(HoennToNationalOrder(i + 1), FLAG_GET_CAUGHT))
                count++;
            break;
        }
    }
    return count;
}
*/

/**
 * FUNCTION: GetKantoPokedexCount
 *
 * PURPOSE: Counts how many Kanto-region Pokemon (original 151) have been seen or caught.
 */
u16 GetKantoPokedexCount(u8 caseID)
{
    u16 count = 0;
    u16 i;

    for (i = 0; i < KANTO_DEX_COUNT; i++)
    {
        switch (caseID)
        {
        case FLAG_GET_SEEN:
            if (GetSetPokedexFlag(i + 1, FLAG_GET_SEEN))
                count++;
            break;
        case FLAG_GET_CAUGHT:
            if (GetSetPokedexFlag(i + 1, FLAG_GET_CAUGHT))
                count++;
            break;
        }
    }
    return count;
}

/**
 * FUNCTION: HasAllHoennMons
 *
 * PURPOSE: Checks if the player has caught all Hoenn-region Pokemon,
 *          excluding Jirachi (#385) and Deoxys (#386) which are event-only mythicals.
 *
 * RETURNS: TRUE if all non-mythical Hoenn Pokemon are caught, FALSE otherwise.
 */
bool16 HasAllHoennMons(void)
{
    u16 i;

    /* -2 excludes Jirachi and Deoxys (event-only mythical Pokemon) */
    for (i = 0; i < HOENN_DEX_COUNT - 2; i++)
    {
        if (!GetSetPokedexFlag(HoennToNationalOrder(i + 1), FLAG_GET_CAUGHT))
            return FALSE;
    }
    return TRUE;
}

/**
 * FUNCTION: HasAllKantoMons
 *
 * PURPOSE: Checks if the player has caught all Kanto-region Pokemon,
 *          excluding Mew (#151) which is an event-only mythical.
 *
 * RETURNS: TRUE if all non-mythical Kanto Pokemon are caught.
 */
bool16 HasAllKantoMons(void)
{
    u16 i;

    /* -1 excludes Mew (event-only mythical Pokemon) */
    for (i = 0; i < KANTO_DEX_COUNT - 1; i++)
    {
        if (!GetSetPokedexFlag(i + 1, FLAG_GET_CAUGHT))
            return FALSE;
    }
    return TRUE;
}

/**
 * FUNCTION: HasAllMons
 *
 * PURPOSE: Checks if the player has caught every Pokemon in the entire National
 *          Dex, excluding event-only mythicals from each generation.
 *
 * GAME LOGIC:
 * The check is split into three regional loops, each excluding specific mythicals:
 *   - Kanto (#1-151): excludes Mew
 *   - Johto (#152-251): excludes Lugia, Ho-Oh, and Celebi
 *   - Hoenn (#252-386): excludes Jirachi and Deoxys
 * This is used to determine if the player qualifies for the diploma.
 *
 * RETURNS: TRUE if all obtainable Pokemon are caught, FALSE otherwise.
 */
bool16 HasAllMons(void)
{
    u16 i;

    /* Kanto check: #1-150 (excludes #151 Mew) */
    for (i = 0; i < KANTO_DEX_COUNT - 1; i++)
    {
        if (!GetSetPokedexFlag(i + 1, FLAG_GET_CAUGHT))
            return FALSE;
    }

    /* Johto check: #152-248 (excludes #249 Lugia, #250 Ho-Oh, #251 Celebi) */
    for (i = KANTO_DEX_COUNT; i < JOHTO_DEX_COUNT - 3; i++)
    {
        if (!GetSetPokedexFlag(i + 1, FLAG_GET_CAUGHT))
            return FALSE;
    }

    /* Hoenn check: #252-384 (excludes #385 Jirachi, #386 Deoxys) */
    for (i = JOHTO_DEX_COUNT; i < NATIONAL_DEX_COUNT - 2; i++)
    {
        if (!GetSetPokedexFlag(i + 1, FLAG_GET_CAUGHT))
            return FALSE;
    }
    return TRUE;
}
