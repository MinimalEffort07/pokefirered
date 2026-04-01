/**
 * @file cereader_tool.c
 * @brief e-Reader Trainer Tower Save/Load — Reading Custom Battle Challenges from Flash
 *
 * FILE OVERVIEW:
 * This file handles saving and loading custom Trainer Tower challenge data that
 * was received via the e-Reader peripheral. The e-Reader was a GBA accessory that
 * could scan special dot-code cards to inject new content into games — in this case,
 * custom trainer battles for the Trainer Tower on Seven Island.
 *
 * The Trainer Tower data is too large to fit in a single save sector (4 KB), so
 * it is split across two dedicated save sectors (SECTOR_ID_TRAINER_TOWER_1 and
 * SECTOR_ID_TRAINER_TOWER_2). This file manages that split, along with checksum
 * validation to detect corrupted data.
 *
 * GBA CONTEXT:
 * The GBA uses flash memory (EEPROM or Flash ROM) for save data, organized into
 * fixed-size "sectors" of 4 KB (4096 bytes). Each sector has a small header/footer
 * area for metadata, leaving SECTOR_DATA_SIZE bytes for actual data and a counter
 * field at SECTOR_COUNTER_OFFSET. When data exceeds one sector, it must be manually
 * split across multiple sectors — there is no file system.
 */
#include "global.h"
#include "gflib.h"
#include "util.h"
#include "save.h"
#include "cereader_tool.h"

/* Calculate how much of the Trainer Tower data fits in the first save sector.
 * offsetof(struct EReaderTrainerTowerSet, floors[4]) gives the byte offset up to
 * (but not including) floors[4], which is the split point between sectors. */
#define SEC30_SIZE  (offsetof(struct EReaderTrainerTowerSet, floors[4]))
/* The remainder goes into the second sector */
#define SEC31_SIZE  (sizeof(struct EReaderTrainerTowerSet) - SEC30_SIZE)

// The trainer tower data exceeds SECTOR_DATA_SIZE. They're allowed to use the full save sector up to the counter field.
STATIC_ASSERT(SEC30_SIZE + SEC31_SIZE <= SECTOR_COUNTER_OFFSET * 2, EReaderTrainerTowerSetFreeSpace);

/**
 * FUNCTION: GetTrainerHillUnkVal
 *
 * PURPOSE: Computes a validation/versioning byte for the Trainer Tower save data.
 *
 * HOW IT WORKS:
 * Takes the unk9 field from the first trainer tower entry, adds 1, and wraps
 * at 256 (using modulo). This value is stored in the save sector as a simple
 * integrity/version marker.
 *
 * RETURNS: A u8 validation value derived from the existing save data.
 */
static u8 GetTrainerHillUnkVal(void)
{
    return (gSaveBlock1Ptr->trainerTower[0].unk9 + 1) % 256;
}

/**
 * FUNCTION: ValidateTrainerTowerTrainer
 *
 * PURPOSE: Validates that a single Trainer Tower floor's data is not corrupted.
 *
 * HOW IT WORKS:
 * Performs three checks:
 * 1. Floor index must be between 1 and MAX_TRAINER_TOWER_FLOORS (inclusive)
 * 2. Challenge type must not exceed CHALLENGE_TYPE_KNOCKOUT
 * 3. Checksum of all bytes before the checksum field must match the stored checksum
 *
 * This protects against corrupted e-Reader scans or flash memory errors.
 *
 * PARAMETERS:
 * @param floor — Pointer to a single floor's trainer data
 *
 * RETURNS: TRUE if valid, FALSE if any check fails.
 */
static bool32 ValidateTrainerTowerTrainer(struct TrainerTowerFloor * floor)
{
    /* Floor index must be in valid range (1-based) */
    if (floor->floorIdx < 1 || floor->floorIdx > MAX_TRAINER_TOWER_FLOORS)
        return FALSE;
    /* Challenge type must be a known type */
    if (floor->challengeType > CHALLENGE_TYPE_KNOCKOUT)
        return FALSE;
    /* Verify checksum: sum all bytes up to (but not including) the checksum field,
     * then compare against the stored checksum value */
    if (CalcByteArraySum((const u8 *)floor, offsetof(typeof(*floor), checksum)) != floor->checksum)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: ValidateTrainerTowerData
 *
 * PURPOSE: Validates an entire Trainer Tower dataset (all floors plus global checksum).
 *
 * HOW IT WORKS:
 * First checks that the number of floors is in the valid range (1 to MAX).
 * Then validates each individual floor. Finally, verifies a global checksum
 * computed across all floor data.
 *
 * PARAMETERS:
 * @param ttdata — Pointer to the complete e-Reader Trainer Tower dataset
 *
 * RETURNS: TRUE if all data is valid, FALSE if any corruption is detected.
 */
bool32 ValidateTrainerTowerData(struct EReaderTrainerTowerSet * ttdata)
{
    u32 numFloors = ttdata->numFloors;
    s32 i;
    if (numFloors < 1 || numFloors > MAX_TRAINER_TOWER_FLOORS)
        return FALSE;
    for (i = 0; i < numFloors; i++)
    {
        if (!ValidateTrainerTowerTrainer(&ttdata->floors[i]))
            return FALSE;
    }
    /* Global checksum across all floor data must match */
    if (CalcByteArraySum((const u8 *)ttdata->floors, numFloors * sizeof(ttdata->floors[0])) != ttdata->checksum)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: CEReaderTool_SaveTrainerTower_r
 *
 * PURPOSE: Internal implementation that writes Trainer Tower data to flash memory,
 *          splitting it across two save sectors.
 *
 * HOW IT WORKS:
 * 1. Asserts that the dummy and id fields are zero (debug validation)
 * 2. Copies the first portion of data (SEC30_SIZE bytes) into a buffer and writes
 *    it to save sector SECTOR_ID_TRAINER_TOWER_1
 * 3. Copies the remaining portion (SEC31_SIZE bytes) and writes to sector 2
 * The buffer is zeroed before each use to avoid writing stale data.
 *
 * GBA CONTEXT:
 * AGB_ASSERT_EX is a debug assertion macro that triggers a crash screen with
 * the file path and line number if the condition fails. These are only active
 * in debug builds and are stripped from release ROMs.
 *
 * PARAMETERS:
 * @param ttdata — The Trainer Tower data to save
 * @param buffer — A pre-allocated SECTOR_SIZE buffer for staging writes
 *
 * RETURNS: TRUE on success, FALSE if either sector write fails.
 */
static bool32 CEReaderTool_SaveTrainerTower_r(struct EReaderTrainerTowerSet * ttdata, u8 * buffer)
{
    AGB_ASSERT_EX(ttdata->dummy == 0, ABSPATH("cereader_tool.c"), 198);
    AGB_ASSERT_EX(ttdata->id == 0, ABSPATH("cereader_tool.c"), 199)

    /* Write first half of data to sector 1 */
    memset(buffer, 0, SECTOR_SIZE);
    memcpy(buffer, ttdata, SEC30_SIZE);
    buffer[1] = GetTrainerHillUnkVal(); /* Stamp validation byte */
    if (TryWriteSpecialSaveSector(SECTOR_ID_TRAINER_TOWER_1, buffer) != TRUE)
        return FALSE;
    /* Write second half of data to sector 2 */
    memset(buffer, 0, SECTOR_SIZE);
    memcpy(buffer, (u8 *)ttdata + SEC30_SIZE, SEC31_SIZE);
    if (TryWriteSpecialSaveSector(SECTOR_ID_TRAINER_TOWER_2, buffer) != TRUE)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: CEReaderTool_SaveTrainerTower
 *
 * PURPOSE: Public wrapper that saves Trainer Tower data to flash, managing
 *          its own temporary buffer allocation.
 *
 * PARAMETERS:
 * @param ttdata — The Trainer Tower data to save
 *
 * RETURNS: TRUE on success, FALSE on failure.
 */
bool32 CEReaderTool_SaveTrainerTower(struct EReaderTrainerTowerSet * ttdata)
{
    u8 * buffer = AllocZeroed(SECTOR_SIZE); /* Allocate a sector-sized staging buffer */
    bool32 result = CEReaderTool_SaveTrainerTower_r(ttdata, buffer);
    Free(buffer); /* Always free even on failure to avoid memory leaks */
    return result;
}

/**
 * FUNCTION: CEReaderTool_LoadTrainerTower_r
 *
 * PURPOSE: Internal implementation that reads Trainer Tower data from flash memory,
 *          reassembling it from two save sectors.
 *
 * HOW IT WORKS:
 * 1. Reads sector 1 into the buffer, then copies SEC30_SIZE bytes into ttdata
 * 2. Reads sector 2 into the buffer, then copies SEC31_SIZE bytes into the
 *    remainder of ttdata
 * 3. Validates the reassembled data with checksum verification
 *
 * PARAMETERS:
 * @param ttdata — Output: the reassembled Trainer Tower data
 * @param buffer — A pre-allocated SECTOR_SIZE buffer for staging reads
 *
 * RETURNS: TRUE if loaded and valid, FALSE if read failed or data is corrupt.
 */
static bool32 CEReaderTool_LoadTrainerTower_r(struct EReaderTrainerTowerSet * ttdata, void *buffer)
{
    /* Read first sector and copy into the beginning of the data structure */
    if (TryReadSpecialSaveSector(SECTOR_ID_TRAINER_TOWER_1, buffer) != 1)
        return FALSE;
    memcpy(ttdata + 0x000, buffer, SEC30_SIZE);

    /* Read second sector and copy into the remainder of the structure */
    if (TryReadSpecialSaveSector(SECTOR_ID_TRAINER_TOWER_2, buffer) != 1)
        return FALSE;
    memcpy((u8 *)ttdata + SEC30_SIZE, buffer, SEC31_SIZE);

    /* Verify the reassembled data is not corrupt */
    if (!ValidateTrainerTowerData(ttdata))
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: CEReaderTool_LoadTrainerTower
 *
 * PURPOSE: Public wrapper that loads Trainer Tower data from flash, managing
 *          its own temporary buffer allocation.
 *
 * PARAMETERS:
 * @param ttdata — Output: loaded Trainer Tower data (only valid if returns TRUE)
 *
 * RETURNS: TRUE on success, FALSE on failure.
 */
bool32 CEReaderTool_LoadTrainerTower(struct EReaderTrainerTowerSet * ttdata)
{
    void *buffer = AllocZeroed(SECTOR_SIZE);
    bool32 success = CEReaderTool_LoadTrainerTower_r(ttdata, buffer);
    Free(buffer);
    return success;
}

/**
 * FUNCTION: ReadTrainerTowerAndValidate
 *
 * PURPOSE: Stub function — would read and validate Trainer Tower data from
 *          flash memory. This is populated in Emerald but left as a stub in
 *          FireRed, always returning FALSE (no valid data).
 *
 * RETURNS: Always FALSE in FireRed.
 */
bool32 ReadTrainerTowerAndValidate(void)
{
    // Stubbed out. Populated in Emerald
    return FALSE;
}
