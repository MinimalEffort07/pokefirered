/**
 * @file pokemon_storage_system.c
 * @brief PC Box Storage — Core Data Access Functions (Bill's PC)
 *
 * FILE OVERVIEW:
 * This file provides the foundational data access layer for the Pokemon Storage
 * System (known in-game as "Someone's PC" or "Bill's PC"). The storage system
 * lets players store up to 420 Pokemon across 14 boxes (30 per box), separate
 * from their 6-Pokemon party.
 *
 * These are pure data manipulation functions — they read/write Pokemon data in
 * the storage boxes but do NOT handle any graphics or UI. The UI is handled by
 * the pokemon_storage_system_tasks/graphics/menu files.
 *
 * All functions include bounds checking: box IDs must be < TOTAL_BOXES_COUNT
 * and positions must be < IN_BOX_COUNT. Out-of-bounds requests return 0/NULL
 * or are silently ignored, preventing crashes from bad data.
 *
 * GAME LOGIC:
 * The storage data lives in gPokemonStoragePtr (a pointer to a PokemonStorage
 * struct in the save block). Each box slot holds a BoxPokemon struct — a compact
 * version of the full Pokemon struct that omits battle-only data (current HP,
 * status conditions, stats). When withdrawing a Pokemon, BoxMonToMon() expands
 * a BoxPokemon into a full Pokemon by recalculating stats.
 */
#include "global.h"
#include "gflib.h"
#include "pokemon_storage_system_internal.h"

/**
 * FUNCTION: BackupPokemonStorage
 *
 * PURPOSE: Creates a full backup copy of the entire Pokemon storage system.
 *          Used during save operations to protect against data corruption.
 */
void BackupPokemonStorage(struct PokemonStorage * dest)
{
    *dest = *gPokemonStoragePtr;
}

/**
 * FUNCTION: RestorePokemonStorage
 *
 * PURPOSE: Restores Pokemon storage from a backup copy.
 */
void RestorePokemonStorage(struct PokemonStorage * src)
{
    *gPokemonStoragePtr = *src;
}

/**
 * FUNCTION: StorageGetCurrentBox
 *
 * PURPOSE: Returns the index of the currently selected/active box.
 *          The current box is what opens by default when the player accesses the PC.
 */
/* Functions here are general utility functions. */
u8 StorageGetCurrentBox(void)
{
    return gPokemonStoragePtr->currentBox;
}

/**
 * FUNCTION: SetCurrentBox
 *
 * PURPOSE: Sets which box opens by default when the player accesses the PC.
 */
void SetCurrentBox(u8 boxId)
{
    if (boxId < TOTAL_BOXES_COUNT)
        gPokemonStoragePtr->currentBox = boxId;
}

/**
 * FUNCTION: GetBoxMonDataAt
 *
 * PURPOSE: Retrieves a specific data field from a Pokemon in a given box and position.
 *
 * PARAMETERS:
 * @param boxId       — Box index (0 to TOTAL_BOXES_COUNT-1)
 * @param boxPosition — Slot within the box (0 to IN_BOX_COUNT-1)
 * @param request     — Which data field to get (MON_DATA_SPECIES, MON_DATA_LEVEL, etc.)
 *
 * RETURNS: The requested data value, or 0 if box/position is out of bounds.
 */
u32 GetBoxMonDataAt(u8 boxId, u8 boxPosition, s32 request)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        return GetBoxMonData(&gPokemonStoragePtr->boxes[boxId][boxPosition], request);
    else
        return 0;
}

void SetBoxMonDataAt(u8 boxId, u8 boxPosition, s32 request, const void *value)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        SetBoxMonData(&gPokemonStoragePtr->boxes[boxId][boxPosition], request, value);
}

u32 GetCurrentBoxMonData(u8 boxPosition, s32 request)
{
    return GetBoxMonDataAt(gPokemonStoragePtr->currentBox, boxPosition, request);
}

void SetCurrentBoxMonData(u8 boxPosition, s32 request, const void *value)
{
    SetBoxMonDataAt(gPokemonStoragePtr->currentBox, boxPosition, request, value);
}

void GetBoxMonNickAt(u8 boxId, u8 boxPosition, u8 *dst)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        GetBoxMonData(&gPokemonStoragePtr->boxes[boxId][boxPosition], MON_DATA_NICKNAME, dst);
    else
        *dst = EOS;
}

void SetBoxMonNickAt(u8 boxId, u8 boxPosition, const u8 *nick)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        SetBoxMonData(&gPokemonStoragePtr->boxes[boxId][boxPosition], MON_DATA_NICKNAME, nick);
}

u32 GetAndCopyBoxMonDataAt(u8 boxId, u8 boxPosition, s32 request, void *dst)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        return GetBoxMonData(&gPokemonStoragePtr->boxes[boxId][boxPosition], request, dst);
    else
        return 0;
}

void SetBoxMonAt(u8 boxId, u8 boxPosition, struct BoxPokemon * src)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        gPokemonStoragePtr->boxes[boxId][boxPosition] = *src;
}

void CopyBoxMonAt(u8 boxId, u8 boxPosition, struct BoxPokemon * dst)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        *dst = gPokemonStoragePtr->boxes[boxId][boxPosition];
}

void CreateBoxMonAt(u8 boxId, u8 boxPosition, u16 species, u8 level, u8 fixedIV, u8 hasFixedPersonality, u32 personality, u8 otIDType, u32 otID)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
    {
        CreateBoxMon(&gPokemonStoragePtr->boxes[boxId][boxPosition],
                     species,
                     level,
                     fixedIV,
                     hasFixedPersonality, personality,
                     otIDType, otID);
    }
}

void ZeroBoxMonAt(u8 boxId, u8 boxPosition)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        ZeroBoxMonData(&gPokemonStoragePtr->boxes[boxId][boxPosition]);
}

void BoxMonAtToMon(u8 boxId, u8 boxPosition, struct Pokemon * dst)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        BoxMonToMon(&gPokemonStoragePtr->boxes[boxId][boxPosition], dst);
}

struct BoxPokemon * GetBoxedMonPtr(u8 boxId, u8 boxPosition)
{
    if (boxId < TOTAL_BOXES_COUNT && boxPosition < IN_BOX_COUNT)
        return &gPokemonStoragePtr->boxes[boxId][boxPosition];
    else
        return NULL;
}

u8 *GetBoxNamePtr(u8 boxId)
{
    if (boxId < TOTAL_BOXES_COUNT)
        return gPokemonStoragePtr->boxNames[boxId];
    else
        return NULL;
}

u8 GetBoxWallpaper(u8 boxId)
{
    if (boxId < TOTAL_BOXES_COUNT)
        return gPokemonStoragePtr->boxWallpapers[boxId];
    else
        return 0;
}

void SetBoxWallpaper(u8 boxId, u8 wallpaperId)
{
    if (boxId < TOTAL_BOXES_COUNT && wallpaperId < WALLPAPER_COUNT)
        gPokemonStoragePtr->boxWallpapers[boxId] = wallpaperId;
}

/**
 * FUNCTION: SeekToNextMonInBox
 *
 * PURPOSE: Searches for the next non-empty slot in a box, starting from curIndex.
 *          Supports forward/backward searching and optional egg filtering.
 *
 * PARAMETERS:
 * @param boxMons  — Array of BoxPokemon to search through
 * @param curIndex — Starting position (exclusive — search begins at curIndex +/- 1)
 * @param maxIndex — Maximum valid index to search up to
 * @param flags    — Bit 0: 1 = include eggs, 0 = skip eggs
 *                   Bit 1: 1 = search backwards, 0 = search forwards
 *
 * RETURNS: Index of the next occupied slot, or -1 if none found.
 */
s16 SeekToNextMonInBox(struct BoxPokemon * boxMons, s8 curIndex, u8 maxIndex, u8 flags)
{
    /* flags bit 0: Allow eggs in results
     * flags bit 1: Search direction (0 = forward, 1 = backward) */
    s16 i;
    s16 adder;
    if (flags == 0 || flags == 1)
        adder = 1;
    else
        adder = -1;

    if (flags == 1 || flags == 3)
    {
        for (i = curIndex + adder; i >= 0 && i <= maxIndex; i += adder)
        {
            if (GetBoxMonData(&boxMons[i], MON_DATA_SPECIES) != SPECIES_NONE)
                return i;
        }
    }
    else
    {
        for (i = curIndex + adder; i >= 0 && i <= maxIndex; i += adder)
        {
            if (GetBoxMonData(&boxMons[i], MON_DATA_SPECIES) != SPECIES_NONE
                && !GetBoxMonData(&boxMons[i], MON_DATA_IS_EGG))
                return i;
        }
    }

    return -1;
}
