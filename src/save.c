/**
 * save.c - Flash Memory Save System
 *
 * ============================================================================
 * GBA FLASH SAVE ARCHITECTURE
 * ============================================================================
 *
 * HOW FLASH MEMORY WORKS ON THE GBA:
 * The GBA cartridge contains a Flash ROM chip for persistent save storage.
 * Flash memory has these key properties:
 *   - Read: fast, works like normal ROM
 *   - Write: slow (~10ms per byte), requires special command sequences
 *   - Erase: must erase an entire 4KB sector before writing to it
 *   - Endurance: each sector can only be erased/written ~10,000-100,000 times
 *
 * SECTOR LAYOUT (32 sectors, 4KB each = 128KB total):
 *
 *   Sectors 0-13:   Save Slot 1 (14 sectors for one complete save)
 *   Sectors 14-27:  Save Slot 2 (14 sectors for the backup save)
 *   Sectors 28-29:  Hall of Fame data
 *   Sectors 30-31:  Trainer Tower data
 *
 * DUAL-SLOT SAVE SYSTEM:
 * The game maintains TWO complete copies of the save data (slot 1 and slot 2).
 * It alternates between them each time the player saves. This provides
 * redundancy: if a save operation is interrupted (power loss, cartridge bump),
 * the other slot still has the previous valid save. This is critical because
 * Flash writes are non-atomic -- a partial write leaves corrupted data.
 *
 * SECTOR ROTATION:
 * Within each save slot, the 14 sectors are "rotated" with each save.
 * The game tracks gLastWrittenSector to know which rotation offset to use.
 * This means sector 0's data might be in physical sector 3 one save,
 * then in physical sector 4 the next save. This may reduce Flash wear
 * by distributing writes more evenly.
 *
 * SAVE INTEGRITY:
 * Each sector contains:
 *   - Data payload (up to 3968 bytes)
 *   - Sector ID: which chunk of save data this contains
 *   - Signature: magic value (SECTOR_SIGNATURE) to identify valid sectors
 *   - Counter: monotonically increasing save counter (determines which slot is newer)
 *   - Checksum: 32-bit additive checksum of the data, folded to 16 bits
 *
 * When loading, the game verifies each sector's signature and checksum.
 * If any sector fails validation, the save is considered corrupt and the
 * game falls back to the other slot (if valid).
 *
 * HOW SAVE DATA IS SPLIT ACROSS SECTORS:
 *   Sector 0:    SaveBlock2 (1 sector -- player identity, options, Pokedex)
 *   Sectors 1-4: SaveBlock1 (4 sectors -- world state, items, events)
 *   Sectors 5-13: PokemonStorage (9 sectors -- PC boxes, 420 Pokemon)
 *
 * ============================================================================
 */

#include "global.h"
#include "save.h"
#include "decompress.h"
#include "overworld.h"
#include "load_save.h"
#include "task.h"
#include "link.h"
#include "save_failed_screen.h"
#include "fieldmap.h"
#include "pokemon_storage_system.h"
#include "gba/flash_internal.h"

static u8 HandleWriteSector(u16 sectorId, const struct SaveSectorLocation *locations);
static u8 TryWriteSector(u8 sectorNum, u8 *data);
static u8 HandleReplaceSector(u16 sectorId, const struct SaveSectorLocation *locations);
static u8 CopySaveSlotData(u16 sectorId, const struct SaveSectorLocation *locations);
static u8 GetSaveValidStatus(const struct SaveSectorLocation *locations);
static u8 ReadFlashSector(u8 sectorId, struct SaveSector *sector);
static u16 CalculateChecksum(void *data, u16 size);

/*
 * Sector Layout:
 *
 * Sectors 0 - 13:      Save Slot 1
 * Sectors 14 - 27:     Save Slot 2
 * Sectors 28 - 29:     Hall of Fame
 * Sectors 30 - 31:     Trainer Tower
 *
 * There are two save slots for saving the player's game data. We alternate between
 * them each time the game is saved, so that if the current save slot is corrupt,
 * we can load the previous one. We also rotate the sectors in each save slot
 * so that the same data is not always being written to the same sector. This
 * might be done to reduce wear on the flash memory, but I'm not sure, since all
 * 14 sectors get written anyway.
 *
 * See SECTOR_ID_* constants in save.h
 */

/**
 * SAVEBLOCK_CHUNK macro: Calculates the offset and size for a chunk of
 * save data that maps to one Flash sector.
 *
 * Since save structs can be larger than one sector (SECTOR_DATA_SIZE = 3968 bytes),
 * they need to be split across multiple sectors. chunkNum identifies which piece.
 *
 * For example, SaveBlock1 is split across 4 sectors (chunks 0-3):
 *   Chunk 0: bytes 0-3967
 *   Chunk 1: bytes 3968-7935
 *   Chunk 2: bytes 7936-11903
 *   Chunk 3: bytes 11904 to end (may be less than a full sector)
 *
 * The min() ensures the last chunk only contains the remaining bytes,
 * not a full sector's worth. The ternary handles the case where the chunk
 * number exceeds the structure's size (size = 0).
 */
// (u8 *)structure was removed from the first statement of the macro in Emerald
// and Fire Red/Leaf Green. This is because malloc is used to allocate addresses
// so storing the raw addresses should not be done in the offsets information.
#define SAVEBLOCK_CHUNK(structure, chunkNum)                                   \
{                                                                              \
    chunkNum * SECTOR_DATA_SIZE,                                               \
    sizeof(structure) >= chunkNum * SECTOR_DATA_SIZE ?                         \
    min(sizeof(structure) - chunkNum * SECTOR_DATA_SIZE, SECTOR_DATA_SIZE) : 0 \
}

/**
 * sSaveSlotLayout: Maps each sector ID to its offset and size within
 * the source save structure.
 *
 * This table tells the save system: "for sector N, copy 'size' bytes
 * starting at 'offset' bytes into the appropriate save block."
 */
struct
{
    u16 offset;
    u16 size;
} static const sSaveSlotLayout[NUM_SECTORS_PER_SLOT] =
{
    SAVEBLOCK_CHUNK(struct SaveBlock2, 0), // SECTOR_ID_SAVEBLOCK2

    SAVEBLOCK_CHUNK(struct SaveBlock1, 0), // SECTOR_ID_SAVEBLOCK1_START
    SAVEBLOCK_CHUNK(struct SaveBlock1, 1),
    SAVEBLOCK_CHUNK(struct SaveBlock1, 2),
    SAVEBLOCK_CHUNK(struct SaveBlock1, 3), // SECTOR_ID_SAVEBLOCK1_END

    SAVEBLOCK_CHUNK(struct PokemonStorage, 0), // SECTOR_ID_PKMN_STORAGE_START
    SAVEBLOCK_CHUNK(struct PokemonStorage, 1),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 2),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 3),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 4),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 5),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 6),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 7),
    SAVEBLOCK_CHUNK(struct PokemonStorage, 8), // SECTOR_ID_PKMN_STORAGE_END
};

/*
 * Compile-time size checks: ensure the save structures actually fit in
 * the allocated number of Flash sectors. If a struct is too large, the
 * build will fail with a clear error message.
 */
// These will produce an error if a save struct is larger than the space
// alloted for it in the flash.
STATIC_ASSERT(sizeof(struct SaveBlock2) <= SECTOR_DATA_SIZE, SaveBlock2FreeSpace);
STATIC_ASSERT(sizeof(struct SaveBlock1) <= SECTOR_DATA_SIZE * (SECTOR_ID_SAVEBLOCK1_END - SECTOR_ID_SAVEBLOCK1_START + 1), SaveBlock1FreeSpace);
STATIC_ASSERT(sizeof(struct PokemonStorage) <= SECTOR_DATA_SIZE * (SECTOR_ID_PKMN_STORAGE_END - SECTOR_ID_PKMN_STORAGE_START + 1), PokemonStorageFreeSpace);

/*
 * Save system state variables (in fast IWRAM):
 *
 * gLastWrittenSector: Rotation offset for sector ordering. Increments each save.
 * gLastSaveCounter: Backup of gSaveCounter for rollback on error.
 * gLastKnownGoodSector: Backup of gLastWrittenSector for rollback on error.
 * gDamagedSaveSectors: Bitmask -- bit N set means sector N had a write error.
 * gSaveCounter: Monotonically increasing counter; determines which slot is newer.
 * gSaveDataBufferPtr: Points to the sector-sized buffer in EWRAM used for staging.
 * gIncrementalSectorId: Tracks which sector to write next during incremental saves.
 * gSaveFileStatus: Result of the most recent load operation.
 * gGameContinueCallback: Callback to run after loading (if any).
 * gRamSaveSectorLocations: Maps sector IDs to RAM addresses and sizes.
 * gSaveAttemptStatus: Result of the most recent save attempt.
 */
// Sector num to begin writing save data. Sectors are rotated each time the game is saved. (possibly to avoid wear on flash memory?)
COMMON_DATA u16 gLastWrittenSector = 0;
COMMON_DATA u32 gLastSaveCounter = 0;
COMMON_DATA u16 gLastKnownGoodSector = 0;
COMMON_DATA u32 gDamagedSaveSectors = 0;
COMMON_DATA u32 gSaveCounter = 0;
COMMON_DATA struct SaveSector *gSaveDataBufferPtr = NULL; // the pointer is in fast IWRAM but points to the slower EWRAM.
COMMON_DATA u16 gIncrementalSectorId = 0;
COMMON_DATA u16 gSaveUnusedVar = 0;
COMMON_DATA u16 gSaveFileStatus = 0;
COMMON_DATA void (*gGameContinueCallback)(void) = NULL;
COMMON_DATA struct SaveSectorLocation gRamSaveSectorLocations[NUM_SECTORS_PER_SLOT] = {0};
COMMON_DATA u16 gSaveAttemptStatus = 0;

/*
 * gSaveDataBuffer: A sector-sized buffer in EWRAM used as a staging area.
 * Data is assembled here before being written to Flash, and read into here
 * when loading from Flash. Using a staging buffer avoids partial writes
 * to the actual save blocks.
 */
EWRAM_DATA struct SaveSector gSaveDataBuffer = {0};
EWRAM_DATA u32 gSaveUnusedVar2 = 0;

/**
 * FUNCTION: ClearSaveData
 *
 * PURPOSE: Erase all save data from Flash memory.
 *
 * HOW IT WORKS:
 * Erases every sector in Flash (all 32). Each sector must be erased
 * individually; there's no "erase all" command on these Flash chips.
 * After this, all bytes in Flash read as 0xFF (the erased state).
 *
 * GBA CONTEXT:
 * EraseFlashSector sends the erase command sequence to the Flash chip
 * (0x5555=0xAA, 0x2AAA=0x55, 0x5555=0x80, 0x5555=0xAA, 0x2AAA=0x55,
 * sector_addr=0x30). The chip then takes about 100ms to erase the sector.
 */
void ClearSaveData(void)
{
    u16 i;

    for (i = 0; i < SECTORS_COUNT; i++)
        EraseFlashSector(i);
}

/**
 * FUNCTION: Save_ResetSaveCounters
 *
 * PURPOSE: Reset save system counters to their initial state.
 */
void Save_ResetSaveCounters(void)
{
    gSaveCounter = 0;
    gLastWrittenSector = 0;
    gDamagedSaveSectors = 0;
}

/**
 * FUNCTION: SetDamagedSectorBits
 *
 * PURPOSE: Track which Flash sectors have write errors.
 *
 * HOW IT WORKS:
 * Uses gDamagedSaveSectors as a 32-bit bitmask where each bit represents
 * one sector. Operations:
 *   ENABLE: Set the bit (mark sector as damaged)
 *   DISABLE: Clear the bit (mark sector as OK)
 *   CHECK: Test if the bit is set
 *
 * PARAMETERS:
 * @param op        -- ENABLE, DISABLE, or CHECK
 * @param sectorNum -- Which sector (0-31)
 *
 * RETURNS: TRUE if the sector is damaged (CHECK only), FALSE otherwise
 */
static bool32 SetDamagedSectorBits(u8 op, u8 sectorNum)
{
    bool32 retVal = FALSE;

    switch (op)
    {
    case ENABLE:
        gDamagedSaveSectors |= (1 << sectorNum);
        break;
    case DISABLE:
        gDamagedSaveSectors &= ~(1 << sectorNum);
        break;
    case CHECK: // unused
        if (gDamagedSaveSectors & (1 << sectorNum))
            retVal = TRUE;
        break;
    }

    return retVal;
}

/**
 * FUNCTION: WriteSaveSectorOrSlot
 *
 * PURPOSE: Write either a single sector or an entire save slot to Flash.
 *
 * HOW IT WORKS:
 * If sectorId is a specific sector number, writes just that one sector.
 * If sectorId is FULL_SAVE_SLOT (0xFFFF), writes all 14 sectors:
 *   1. Backs up the current sector rotation and save counter
 *   2. Advances the rotation (gLastWrittenSector) and save counter
 *   3. Writes all sectors
 *   4. If any sector was damaged, rolls back the counter/rotation
 *
 * The save counter determines which slot is "newest." When loading, the
 * game compares both slots' counters and uses the higher one.
 *
 * PARAMETERS:
 * @param sectorId  -- Specific sector to write, or FULL_SAVE_SLOT for all
 * @param locations -- Array mapping sector IDs to RAM data pointers
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 WriteSaveSectorOrSlot(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u32 status;
    u16 i;

    gSaveDataBufferPtr = &gSaveDataBuffer;

    if (sectorId != FULL_SAVE_SLOT)  // write single sector
        status = HandleWriteSector(sectorId, locations);

    else  // write all sectors
    {
        gLastKnownGoodSector = gLastWrittenSector; // backup the current written sector before attempting to write.
        gLastSaveCounter = gSaveCounter;
        gLastWrittenSector++;
        gLastWrittenSector %= NUM_SECTORS_PER_SLOT; // array count save sector locations
        gSaveCounter++;
        status = SAVE_STATUS_OK;

        for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
            HandleWriteSector(i, locations);

        // Check for any bad sectors
        if (gDamagedSaveSectors != 0) // skip the damaged sector.
        {
            status = SAVE_STATUS_ERROR;
            gLastWrittenSector = gLastKnownGoodSector;
            gSaveCounter = gLastSaveCounter;
        }
    }

    return status;
}

/**
 * FUNCTION: HandleWriteSector
 *
 * PURPOSE: Write one sector of save data to Flash.
 *
 * HOW IT WORKS:
 * 1. Calculate which physical Flash sector to write to, based on rotation
 *    (gLastWrittenSector) and which save slot we're using (even/odd save counter)
 * 2. Clear the staging buffer to all zeros
 * 3. Fill in the sector metadata: ID, signature, counter
 * 4. Copy the save data payload from RAM
 * 5. Calculate and store the checksum
 * 6. Write the assembled sector to Flash via TryWriteSector
 *
 * The physical sector number calculation:
 *   sectorNum = (gLastWrittenSector + sectorId) % NUM_SECTORS_PER_SLOT
 *   sectorNum += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS)
 * This handles both rotation (% NUM_SECTORS_PER_SLOT) and slot selection
 * (0-13 for slot 1, 14-27 for slot 2).
 *
 * PARAMETERS:
 * @param sectorId  -- Which piece of save data (0-13)
 * @param locations -- RAM data source pointers
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 HandleWriteSector(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 i;
    u16 sectorNum;
    u8 *data;
    u16 size;

    /* Calculate physical Flash sector number with rotation and slot selection */
    sectorNum = gLastWrittenSector + sectorId;
    sectorNum %= NUM_SECTORS_PER_SLOT;
    sectorNum += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    data = locations[sectorId].data;
    size = locations[sectorId].size;

    // clear buffer
    for (i = 0; i < SECTOR_SIZE; i++)
        ((char *)gSaveDataBufferPtr)[i] = 0;

    // fill buffer with save data
    gSaveDataBufferPtr->id = sectorId;
    gSaveDataBufferPtr->signature = SECTOR_SIGNATURE;
    gSaveDataBufferPtr->counter = gSaveCounter;

    for (i = 0; i < size; i++)
        gSaveDataBufferPtr->data[i] = data[i];

    gSaveDataBufferPtr->checksum = CalculateChecksum(data, size);
    return TryWriteSector(sectorNum, gSaveDataBufferPtr->data);
}

/**
 * FUNCTION: HandleWriteSectorNBytes
 *
 * PURPOSE: Write arbitrary data to a specific Flash sector (for Hall of Fame).
 *
 * HOW IT WORKS:
 * Similar to HandleWriteSector but used for special-purpose sectors (Hall of Fame)
 * that don't follow the normal save block format. The checksum is stored in the
 * 'id' field instead of the 'checksum' field, which seems intentional for this
 * special sector type.
 *
 * PARAMETERS:
 * @param sectorId -- Physical Flash sector number
 * @param data     -- Data to write
 * @param size     -- Number of bytes
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 HandleWriteSectorNBytes(u8 sectorId, u8 *data, u16 size)
{
    u16 i;
    struct SaveSector *sector = &gSaveDataBuffer;

    for (i = 0; i < SECTOR_SIZE; i++)
        ((char *)sector)[i] = 0;

    sector->signature = SECTOR_SIGNATURE;

    for (i = 0; i < size; i++)
        sector->data[i] = data[i];

    sector->id = CalculateChecksum(data, size); // though this appears to be incorrect, it might be some sector checksum instead of a whole save checksum and only appears to be relevent to HOF data, if used.
    return TryWriteSector(sectorId, sector->data);
}

/**
 * FUNCTION: TryWriteSector
 *
 * PURPOSE: Attempt to write a sector to Flash and verify the write succeeded.
 *
 * HOW IT WORKS:
 * ProgramFlashSectorAndVerify erases the sector, writes the data, then reads
 * it back to verify. If verification fails (data doesn't match what was written),
 * the sector is marked as damaged.
 *
 * GBA CONTEXT:
 * Flash write failures can occur due to:
 *   - Flash chip wear (sector has been erased too many times)
 *   - Power fluctuation during write
 *   - Bad flash chip
 * The damaged sector tracking allows the game to show an error screen.
 *
 * PARAMETERS:
 * @param sectorNum -- Physical Flash sector number
 * @param data      -- Data to write (SECTOR_SIZE bytes)
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 TryWriteSector(u8 sectorNum, u8 *data)
{
    if (ProgramFlashSectorAndVerify(sectorNum, data)) // is damaged?
    {
        SetDamagedSectorBits(ENABLE, sectorNum); // set damaged sector bits.
        return SAVE_STATUS_ERROR;
    }
    else
    {
        SetDamagedSectorBits(DISABLE, sectorNum); // unset damaged sector bits. it's safe now.
        return SAVE_STATUS_OK;
    }
}

/**
 * FUNCTION: RestoreSaveBackupVarsAndIncrement
 *
 * PURPOSE: Back up save state variables and advance to the next save slot.
 *
 * HOW IT WORKS:
 * Saves current rotation and counter, then advances both for the new save.
 * Used at the start of an incremental save operation (writing sectors one
 * at a time across multiple frames, used during link play).
 */
static u32 RestoreSaveBackupVarsAndIncrement(const struct SaveSectorLocation *locations)
{
    gSaveDataBufferPtr = &gSaveDataBuffer;
    gLastKnownGoodSector = gLastWrittenSector;
    gLastSaveCounter = gSaveCounter;
    gLastWrittenSector++;
    gLastWrittenSector %= NUM_SECTORS_PER_SLOT;
    gSaveCounter++;
    gIncrementalSectorId = 0;
    gDamagedSaveSectors = 0;
    return 0;
}

/**
 * FUNCTION: RestoreSaveBackupVars
 *
 * PURPOSE: Back up save state variables WITHOUT advancing (for partial saves).
 *
 * HOW IT WORKS:
 * Similar to RestoreSaveBackupVarsAndIncrement but doesn't advance the
 * rotation or counter. Used when only writing a subset of sectors
 * (like just SaveBlock2 during a link save).
 */
static u32 RestoreSaveBackupVars(const struct SaveSectorLocation *locations)
{
    gSaveDataBufferPtr = &gSaveDataBuffer;
    gLastKnownGoodSector = gLastWrittenSector;
    gLastSaveCounter = gSaveCounter;
    gIncrementalSectorId = 0;
    gDamagedSaveSectors = 0;
    return 0;
}

/**
 * FUNCTION: HandleWriteIncrementalSector
 *
 * PURPOSE: Write one sector during an incremental (multi-frame) save.
 *
 * HOW IT WORKS:
 * Writes the sector at gIncrementalSectorId, then advances to the next.
 * Used during link play where the save must be spread across multiple
 * frames to avoid freezing the communication.
 *
 * PARAMETERS:
 * @param numSectors -- Total number of sectors to write
 * @param locations  -- RAM source data
 *
 * RETURNS: SAVE_STATUS_OK if more sectors remain, SAVE_STATUS_ERROR if done or error
 */
static u8 HandleWriteIncrementalSector(u16 numSectors, const struct SaveSectorLocation *locations)
{
    u8 status;

    if (gIncrementalSectorId < numSectors - 1)
    {
        status = SAVE_STATUS_OK;
        HandleWriteSector(gIncrementalSectorId, locations);
        gIncrementalSectorId++;
        if (gDamagedSaveSectors)
        {
            status = SAVE_STATUS_ERROR;
            gLastWrittenSector = gLastKnownGoodSector;
            gSaveCounter = gLastSaveCounter;
        }
    }
    else
        status = SAVE_STATUS_ERROR;

    return status;
}

/**
 * FUNCTION: HandleReplaceSectorAndVerify
 *
 * PURPOSE: Replace a sector using the safe byte-by-byte write method and
 *          check for errors.
 *
 * PARAMETERS:
 * @param sectorId  -- Sector to replace (1-based; internally adjusts to 0-based)
 * @param locations -- RAM source data
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 HandleReplaceSectorAndVerify(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u8 status = SAVE_STATUS_OK;

    HandleReplaceSector(sectorId - 1, locations);

    if (gDamagedSaveSectors)
    {
        status = SAVE_STATUS_ERROR;
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
    }
    return status;
}

/**
 * FUNCTION: HandleReplaceSector
 *
 * PURPOSE: Write a sector to Flash using a safe, two-phase approach.
 *
 * HOW IT WORKS:
 * Unlike HandleWriteSector (which uses ProgramFlashSectorAndVerify for the
 * whole sector at once), this function writes byte-by-byte for better error
 * recovery. The write is done in two phases:
 *
 * Phase 1: Write all data EXCEPT the signature field.
 *   If this fails, the sector won't have a valid signature, so the game
 *   will know the data is incomplete.
 *
 * Phase 2: Write the signature and counter fields (except the first signature byte).
 *   The first byte of the signature is deliberately skipped and written
 *   last by WriteSectorSignatureByte. This way, the signature only becomes
 *   valid after ALL other data has been successfully written.
 *
 * This two-phase approach is an ATOMIC WRITE pattern: the sector is only
 * considered valid when its signature is complete. If power is lost during
 * the write, the incomplete signature means the sector will be detected as
 * invalid, and the game will use the backup slot instead.
 *
 * PARAMETERS:
 * @param sectorId  -- Which save chunk (0-13)
 * @param locations -- RAM source data
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 HandleReplaceSector(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 i;
    u16 sectorNum;
    u8 *data;
    u16 size;
    u8 status;

    /* Calculate physical sector number */
    sectorNum = gLastWrittenSector + sectorId;
    sectorNum %= NUM_SECTORS_PER_SLOT;
    sectorNum += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    data = locations[sectorId].data;
    size = locations[sectorId].size;

    // clear buffer
    for (i = 0; i < SECTOR_SIZE; i++)
        ((char *)gSaveDataBufferPtr)[i] = 0;

    // fill buffer with save data
    gSaveDataBufferPtr->id = sectorId;
    gSaveDataBufferPtr->signature = SECTOR_SIGNATURE;
    gSaveDataBufferPtr->counter = gSaveCounter;
    for (i = 0; i < size; i++)
        gSaveDataBufferPtr->data[i] = data[i];

    gSaveDataBufferPtr->checksum = CalculateChecksum(data, size);

    // erase old save data
    EraseFlashSector(sectorNum);

    status = SAVE_STATUS_OK;

    /*
     * Phase 1: Write data bytes up to (but not including) the signature.
     * If this write fails, the sector has no valid signature = safe to discard.
     */
    // write new save data, excluding the signature and counter fields
    for (i = 0; i < SECTOR_SIGNATURE_OFFSET; i++)
    {
        if (ProgramFlashByte(sectorNum, i, gSaveDataBufferPtr->data[i]))
        {
            status = SAVE_STATUS_ERROR;
            break;
        }
    }

    if (status == SAVE_STATUS_ERROR)
    {
        SetDamagedSectorBits(ENABLE, sectorNum);
        return SAVE_STATUS_ERROR;
    }
    else
    {
        status = SAVE_STATUS_OK;

        /*
         * Phase 2: Write the signature (skipping byte 0) and counter fields.
         * The first byte of the signature is written later by
         * WriteSectorSignatureByte as the final "commit" step.
         */
        // write signature (skipping the first byte) and counter fields
        // the first signature byte skipped is instead written in WriteSectorSignatureByte
        for (i = 0; i < SECTOR_SIZE - (SECTOR_SIGNATURE_OFFSET + 1); i++)
        {
            if (ProgramFlashByte(sectorNum, SECTOR_SIGNATURE_OFFSET + 1 + i, ((u8 *)gSaveDataBufferPtr)[SECTOR_SIGNATURE_OFFSET + 1 + i]))
            {
                status = SAVE_STATUS_ERROR;
                break;
            }
        }

        if (status == SAVE_STATUS_ERROR)
        {
            SetDamagedSectorBits(ENABLE, sectorNum);
            return SAVE_STATUS_ERROR;
        }
        else
        {
            SetDamagedSectorBits(DISABLE, sectorNum);
            return SAVE_STATUS_OK;
        }
    }
}

/**
 * FUNCTION: CopySectorSignatureByte
 *
 * PURPOSE: Write the first byte of a sector's signature from the staging buffer.
 *
 * HOW IT WORKS:
 * Copies the first signature byte from gSaveDataBufferPtr to Flash.
 * This is the "commit" step -- once this byte is written, the sector's
 * signature is complete and the sector is considered valid.
 *
 * If this write fails, rolls back to the previous save state.
 *
 * PARAMETERS:
 * @param sectorId  -- Sector ID (1-based)
 * @param locations -- Unused (kept for API consistency)
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
static u8 CopySectorSignatureByte(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 sector;

    sector = gLastWrittenSector + sectorId - 1;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    if (ProgramFlashByte(sector, SECTOR_SIGNATURE_OFFSET, ((u8 *)gSaveDataBufferPtr)[SECTOR_SIGNATURE_OFFSET]))
    {
        // sector is damaged, so enable the bit in gDamagedSaveSectors and restore the last written sector and save counter.
        SetDamagedSectorBits(ENABLE, sector);
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
        return SAVE_STATUS_ERROR;
    }
    else
    {
        SetDamagedSectorBits(DISABLE, sector);
        return SAVE_STATUS_OK;
    }
}

/**
 * FUNCTION: WriteSectorSignatureByte
 *
 * PURPOSE: Write the first byte of SECTOR_SIGNATURE as the final commit step.
 *
 * HOW IT WORKS:
 * Similar to CopySectorSignatureByte, but writes the hardcoded first byte
 * of SECTOR_SIGNATURE (0x25 from 0x08012025) rather than copying from the
 * staging buffer. This is the last byte written during a safe sector replace,
 * making the sector valid.
 */
static u8 WriteSectorSignatureByte(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 sector;

    sector = gLastWrittenSector + sectorId - 1;
    sector %= NUM_SECTORS_PER_SLOT;
    sector += NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);

    // write only the first byte of the signature, which was skipped in HandleReplaceSector
    if (ProgramFlashByte(sector, SECTOR_SIGNATURE_OFFSET, SECTOR_SIGNATURE & 0xFF))
    {
        // sector is damaged, so enable the bit in gDamagedSaveSectors and restore the last written sector and save counter.
        SetDamagedSectorBits(ENABLE, sector);
        gLastWrittenSector = gLastKnownGoodSector;
        gSaveCounter = gLastSaveCounter;
        return SAVE_STATUS_ERROR;
    }
    else
    {
        SetDamagedSectorBits(DISABLE, sector);
        return SAVE_STATUS_OK;
    }
}

/**
 * FUNCTION: TryLoadSaveSlot
 *
 * PURPOSE: Attempt to load save data from Flash into RAM.
 *
 * HOW IT WORKS:
 * Only supports loading a full save slot (sectorId == FULL_SAVE_SLOT).
 * Validates both save slots to find the newest valid one, then copies
 * all sector data from Flash into the RAM save blocks.
 *
 * PARAMETERS:
 * @param sectorId  -- Must be FULL_SAVE_SLOT
 * @param locations -- RAM destination addresses
 *
 * RETURNS: SAVE_STATUS_OK, SAVE_STATUS_ERROR, SAVE_STATUS_EMPTY, or SAVE_STATUS_INVALID
 */
static u8 TryLoadSaveSlot(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u8 status;
    gSaveDataBufferPtr = &gSaveDataBuffer;
    if (sectorId != FULL_SAVE_SLOT)
        status = SAVE_STATUS_ERROR;

    else
    {
        status = GetSaveValidStatus(locations);
        CopySaveSlotData(FULL_SAVE_SLOT, locations);
    }

    return status;
}

/**
 * FUNCTION: CopySaveSlotData
 *
 * PURPOSE: Read all 14 sectors from the active save slot and copy to RAM.
 *
 * HOW IT WORKS:
 * Reads each sector from Flash, verifies its signature and checksum,
 * and if valid, copies the payload data to the correct RAM location.
 * The sector ID field tells us which piece of save data this sector contains,
 * and the locations array tells us where in RAM to put it.
 *
 * Also identifies the sector rotation offset by finding which sector
 * contains chunk 0 (id == 0) -- that sector's position reveals gLastWrittenSector.
 *
 * PARAMETERS:
 * @param sectorId  -- Unused (always reads all sectors)
 * @param locations -- RAM destination addresses
 *
 * RETURNS: SAVE_STATUS_OK
 */
// sectorId is unused. All sectors in the save slot are read and copied.
static u8 CopySaveSlotData(u16 sectorId, const struct SaveSectorLocation *locations)
{
    u16 i;
    u16 checksum;
    u16 sector = NUM_SECTORS_PER_SLOT * (gSaveCounter % NUM_SAVE_SLOTS);
    u16 id;

    for (i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        ReadFlashSector(i + sector, gSaveDataBufferPtr);
        id = gSaveDataBufferPtr->id;

        /* If this sector contains chunk 0, its position reveals the rotation offset */
        if (id == 0)
            gLastWrittenSector = i;

        checksum = CalculateChecksum(gSaveDataBufferPtr->data, locations[id].size);
        if (gSaveDataBufferPtr->signature == SECTOR_SIGNATURE && gSaveDataBufferPtr->checksum == checksum)
        {
            /* Valid sector: copy data to the correct RAM location */
            u16 j;
            for (j = 0; j < locations[id].size; j++)
                locations[id].data[j] = gSaveDataBufferPtr->data[j];
        }
    }

    return SAVE_STATUS_OK;
}

/**
 * FUNCTION: GetSaveValidStatus
 *
 * PURPOSE: Determine which save slot to use by validating both slots.
 *
 * HOW IT WORKS:
 * Reads all sectors from both save slots and validates each one:
 *   - Check for the magic signature (SECTOR_SIGNATURE)
 *   - Verify the checksum matches the data
 *   - Track which sectors are valid using a bitmask
 *   - Record the save counter from valid sectors
 *
 * Then determines which slot to use based on validity and recency:
 *   - Both valid: use the one with the higher save counter (most recent)
 *   - One valid, one error: use the valid one (report error so game can
 *     attempt to repair the damaged slot)
 *   - Both empty: new game, no save data exists
 *   - Both corrupt: return INVALID status
 *
 * The save counter comparison handles the u32 wraparound case where
 * counter goes from 0xFFFFFFFF to 0x00000000.
 *
 * RETURNS: SAVE_STATUS_OK, SAVE_STATUS_ERROR, SAVE_STATUS_EMPTY, or SAVE_STATUS_INVALID
 */
static u8 GetSaveValidStatus(const struct SaveSectorLocation *locations)
{
    u16 sector;
    bool8 signatureValid;
    u16 checksum;
    u32 slot1saveCounter = 0;
    u32 slot2saveCounter = 0;
    u8 slot1Status;
    u8 slot2Status;
    u32 validSectors;
    const u32 ALL_SECTORS = (1 << NUM_SECTORS_PER_SLOT) - 1;  // bitmask of all saveblock sectors

    /* ---------- Validate Save Slot 1 (sectors 0-13) ---------- */
    // check save slot 1.
    validSectors = 0;
    signatureValid = FALSE;
    for (sector = 0; sector < NUM_SECTORS_PER_SLOT; sector++)
    {
        ReadFlashSector(sector, gSaveDataBufferPtr);
        if (gSaveDataBufferPtr->signature == SECTOR_SIGNATURE)
        {
            signatureValid = TRUE;
            checksum = CalculateChecksum(gSaveDataBufferPtr->data, locations[gSaveDataBufferPtr->id].size);
            if (gSaveDataBufferPtr->checksum == checksum)
            {
                slot1saveCounter = gSaveDataBufferPtr->counter;
                validSectors |= 1 << gSaveDataBufferPtr->id;
            }
        }
    }

    if (signatureValid)
    {
        if (validSectors == ALL_SECTORS)  /* All 14 sectors valid */
            slot1Status = SAVE_STATUS_OK;
        else
            slot1Status = SAVE_STATUS_ERROR;  /* Some sectors corrupt */
    }
    else
        slot1Status = SAVE_STATUS_EMPTY;  /* No valid signatures at all */

    /* ---------- Validate Save Slot 2 (sectors 14-27) ---------- */
    // check save slot 2.
    validSectors = 0;
    signatureValid = FALSE;
    for (sector = 0; sector < NUM_SECTORS_PER_SLOT; sector++)
    {
        ReadFlashSector(NUM_SECTORS_PER_SLOT + sector, gSaveDataBufferPtr);
        if (gSaveDataBufferPtr->signature == SECTOR_SIGNATURE)
        {
            signatureValid = TRUE;
            checksum = CalculateChecksum(gSaveDataBufferPtr->data, locations[gSaveDataBufferPtr->id].size);
            if (gSaveDataBufferPtr->checksum == checksum)
            {
                slot2saveCounter = gSaveDataBufferPtr->counter;
                validSectors |= 1 << gSaveDataBufferPtr->id;
            }
        }
    }

    if (signatureValid)
    {
        if (validSectors == ALL_SECTORS)
            slot2Status = SAVE_STATUS_OK;
        else
            slot2Status = SAVE_STATUS_ERROR;
    }
    else
        slot2Status = SAVE_STATUS_EMPTY;

    /* ---------- Choose which slot to use ---------- */

    if (slot1Status == SAVE_STATUS_OK && slot2Status == SAVE_STATUS_OK)
    {
        /*
         * Both slots valid: choose the one with the higher (more recent) counter.
         * Special case for counter wraparound: if one counter is -1 (0xFFFFFFFF)
         * and the other is 0, compare using (counter + 1) to handle the wrap.
         */
        // Choose counter of the most recent save file
        if ((slot1saveCounter == -1 && slot2saveCounter == 0) || (slot1saveCounter == 0 && slot2saveCounter == -1))
        {
            if ((unsigned)(slot1saveCounter + 1) < (unsigned)(slot2saveCounter + 1))
                gSaveCounter = slot2saveCounter;
            else
                gSaveCounter = slot1saveCounter;
        }
        else
        {
            if (slot1saveCounter < slot2saveCounter)
                gSaveCounter = slot2saveCounter;
            else
                gSaveCounter = slot1saveCounter;
        }
        return SAVE_STATUS_OK;
    }

    /* One slot OK, other damaged or empty */
    if (slot1Status == SAVE_STATUS_OK)
    {
        gSaveCounter = slot1saveCounter;
        if (slot2Status == SAVE_STATUS_ERROR)
            return SAVE_STATUS_ERROR;
        else
            return SAVE_STATUS_OK;
    }

    if (slot2Status == SAVE_STATUS_OK)
    {
        gSaveCounter = slot2saveCounter;
        if (slot1Status == SAVE_STATUS_ERROR)
            return SAVE_STATUS_ERROR;
        else
            return SAVE_STATUS_OK;
    }

    /* Both slots empty = fresh cartridge, no save data */
    if (slot1Status == SAVE_STATUS_EMPTY && slot2Status == SAVE_STATUS_EMPTY)
    {
        gSaveCounter = 0;
        gLastWrittenSector = 0;
        return SAVE_STATUS_EMPTY;
    }

    /* Both slots corrupt -- unrecoverable */
    gSaveCounter = 0;
    gLastWrittenSector = 0;
    return SAVE_STATUS_INVALID;
}

/**
 * FUNCTION: TryLoadSaveSector
 *
 * PURPOSE: Load data from a specific Flash sector (for Hall of Fame/special data).
 *
 * HOW IT WORKS:
 * Reads the sector, verifies its signature, and checks the checksum
 * (stored in the 'id' field for these special sectors). If valid,
 * copies the data to the provided buffer.
 *
 * PARAMETERS:
 * @param sectorId -- Physical Flash sector number
 * @param data     -- Destination buffer
 * @param size     -- Number of bytes to copy
 *
 * RETURNS: SAVE_STATUS_OK, SAVE_STATUS_EMPTY, or SAVE_STATUS_INVALID
 */
static u8 TryLoadSaveSector(u8 sectorId, u8 *data, u16 size)
{
    u16 i;
    struct SaveSector *sector = &gSaveDataBuffer;

    ReadFlashSector(sectorId, sector);
    if (sector->signature == SECTOR_SIGNATURE)
    {
        u16 checksum = CalculateChecksum(sector->data, size);
        if (sector->id == checksum)
        {
            for (i = 0; i < size; i++)
                data[i] = sector->data[i];

            return SAVE_STATUS_OK;
        }
        else
            return SAVE_STATUS_INVALID;

    }
    else
        return SAVE_STATUS_EMPTY;
}

/**
 * FUNCTION: ReadFlashSector
 *
 * PURPOSE: Read one complete sector (4KB) from Flash into a buffer.
 *
 * GBA CONTEXT:
 * ReadFlash is a library function that reads bytes from the Flash chip.
 * Flash reads are fast (similar to ROM reads) and don't require special
 * command sequences unlike writes.
 *
 * PARAMETERS:
 * @param sectorId -- Physical Flash sector to read
 * @param sector   -- Destination buffer (must be SECTOR_SIZE bytes)
 *
 * RETURNS: 1 (always succeeds for reads)
 */
static u8 ReadFlashSector(u8 sectorId, struct SaveSector *sector)
{
    ReadFlash(sectorId, 0, sector->data, SECTOR_SIZE);
    return 1;
}

/**
 * FUNCTION: CalculateChecksum
 *
 * PURPOSE: Calculate a checksum over save data for integrity verification.
 *
 * HOW IT WORKS:
 * Sums the data as an array of 32-bit words, then folds the result into
 * 16 bits by adding the upper and lower 16-bit halves together.
 *
 * This is a simple additive checksum -- fast to compute but not as robust
 * as CRC for detecting all types of corruption. However, combined with the
 * signature check, it provides adequate protection against random bit errors.
 *
 * PARAMETERS:
 * @param data -- Pointer to the data to checksum
 * @param size -- Number of bytes (must be a multiple of 4)
 *
 * RETURNS: 16-bit checksum value
 */
static u16 CalculateChecksum(void *data, u16 size)
{
    u16 i;
    u32 checksum = 0;

    for (i = 0; i < (size / 4); i++)
    {
        // checksum += *(u32 *)data++;
        // For compatibility with modern gcc, these statements were separated.
        checksum += *(u32 *)data;
        data += 4;
    }

    /* Fold the 32-bit sum into 16 bits by adding upper and lower halves */
    return ((checksum >> 16) + checksum);
}

/**
 * FUNCTION: UpdateSaveAddresses
 *
 * PURPOSE: Update the RAM source/destination addresses for all save sectors.
 *
 * HOW IT WORKS:
 * Populates gRamSaveSectorLocations with the correct RAM addresses for
 * each sector. Since save block pointers can change due to ASLR
 * (SetSaveBlocksPointers), this must be called before any save/load
 * operation to ensure the sector map points to the current addresses.
 */
static void UpdateSaveAddresses(void)
{
    int i = 0;

    /* Sector 0: SaveBlock2 */
    gRamSaveSectorLocations[i].data = (void *)(gSaveBlock2Ptr) + sSaveSlotLayout[i].offset;
    gRamSaveSectorLocations[i].size = sSaveSlotLayout[i].size;

    /* Sectors 1-4: SaveBlock1 chunks */
    for (i = SECTOR_ID_SAVEBLOCK1_START; i <= SECTOR_ID_SAVEBLOCK1_END; i++)
    {
        gRamSaveSectorLocations[i].data = (void *)(gSaveBlock1Ptr) + sSaveSlotLayout[i].offset;
        gRamSaveSectorLocations[i].size = sSaveSlotLayout[i].size;
    }

    /* Sectors 5-13: PokemonStorage chunks */
    for (/*i = SECTOR_ID_PKMN_STORAGE_START*/; i <= SECTOR_ID_PKMN_STORAGE_END; i++) // do not initialize here to ensure matching
    {
        gRamSaveSectorLocations[i].data = (void *)(gPokemonStoragePtr) + sSaveSlotLayout[i].offset;
        gRamSaveSectorLocations[i].size = sSaveSlotLayout[i].size;
    }
}

/**
 * FUNCTION: HandleSavingData
 *
 * PURPOSE: Top-level save dispatcher that handles different save types.
 *
 * HOW IT WORKS:
 * Disables the VBlank counter (to prevent timing issues during slow Flash
 * writes), updates RAM addresses, then performs the save based on type:
 *
 *   SAVE_NORMAL: Save everything (all 14 sectors)
 *   SAVE_HALL_OF_FAME: Save HoF data + everything
 *   SAVE_LINK: Save only SaveBlock2 and SaveBlock1 (skip PC storage for speed)
 *   SAVE_EREADER: Save only SaveBlock2
 *   SAVE_OVERWRITE_DIFFERENT_FILE: Erase HoF/Tower data + save everything
 *     (used when starting a new game that overwrites an existing save)
 *
 * PARAMETERS:
 * @param saveType -- SAVE_NORMAL, SAVE_HALL_OF_FAME, SAVE_LINK, etc.
 *
 * RETURNS: Always 0
 */
u8 HandleSavingData(u8 saveType)
{
    u8 i;
    u32 *backupPtr = gMain.vblankCounter1;
    u8 *tempAddr;

    gMain.vblankCounter1 = NULL;
    UpdateSaveAddresses();
    switch (saveType)
    {
    case SAVE_HALL_OF_FAME_ERASE_BEFORE: // Unused
        /* Erase HoF and Trainer Tower sectors before writing */
        for (i = SECTOR_ID_HOF_1; i < SECTORS_COUNT; i++)
            EraseFlashSector(i);
        // fallthrough
    case SAVE_HALL_OF_FAME:
        /* Track Hall of Fame entries (capped at 999) */
        if (GetGameStat(GAME_STAT_ENTERED_HOF) < 999)
            IncrementGameStat(GAME_STAT_ENTERED_HOF);
        /* Write HoF data across 2 sectors (using the decompression buffer as source) */
        tempAddr = gDecompressionBuffer;
        HandleWriteSectorNBytes(SECTOR_ID_HOF_1, tempAddr, SECTOR_DATA_SIZE);
        HandleWriteSectorNBytes(SECTOR_ID_HOF_2, tempAddr + SECTOR_DATA_SIZE, SECTOR_DATA_SIZE);
        // fallthrough
    case SAVE_NORMAL:
    default:
        /* Standard save: serialize game state and write all sectors */
        SaveSerializedGame();
        WriteSaveSectorOrSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);
        break;
    case SAVE_LINK:
        /* Link save: only save player data, skip PC storage for speed */
        SaveSerializedGame();
        // only SaveBlock2 and SaveBlock1 (ignores storage in PC)
        for(i = SECTOR_ID_SAVEBLOCK2; i <= SECTOR_ID_SAVEBLOCK1_END; i++)
            WriteSaveSectorOrSlot(i, gRamSaveSectorLocations);
        break;
    case SAVE_EREADER: // unused
        /* Save only player identity data */
        SaveSerializedGame();
        // only SaveBlock2
        WriteSaveSectorOrSlot(SECTOR_ID_SAVEBLOCK2, gRamSaveSectorLocations);
        break;
    case SAVE_OVERWRITE_DIFFERENT_FILE:
        /* New save overwriting existing: erase extra data sectors first */
        for (i = SECTOR_ID_HOF_1; i < SECTORS_COUNT; i++)
            EraseFlashSector(i);
        SaveSerializedGame();
        WriteSaveSectorOrSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);
        break;
    }
    gMain.vblankCounter1 = backupPtr;
    return 0;
}

/**
 * FUNCTION: TrySavingData
 *
 * PURPOSE: Attempt to save the game, showing an error screen on failure.
 *
 * HOW IT WORKS:
 * First checks if Flash memory is present. If not, saving is impossible.
 * If present, performs the save. If any sectors were damaged during the write,
 * shows the "Save Failed" error screen (a scary moment for any player!).
 *
 * PARAMETERS:
 * @param saveType -- Type of save operation
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
u8 TrySavingData(u8 saveType)
{
    if (gFlashMemoryPresent != TRUE)
    {
        gSaveAttemptStatus = SAVE_STATUS_ERROR;
        return SAVE_STATUS_ERROR;
    }

    HandleSavingData(saveType);
    if (!gDamagedSaveSectors)
    {
        gSaveAttemptStatus = SAVE_STATUS_OK;
        return SAVE_STATUS_OK;
    }
    else
    {
        DoSaveFailedScreen(saveType);
        gSaveAttemptStatus = SAVE_STATUS_ERROR;
        return SAVE_STATUS_ERROR;
    }
}

/**
 * FUNCTION: LinkFullSave_Init
 *
 * PURPOSE: Initialize a full save operation that will be done incrementally
 *          during link/multiplayer sessions.
 *
 * HOW IT WORKS:
 * During link play, the game can't spend many consecutive frames writing to
 * Flash (it would freeze the communication). Instead, it initializes the save
 * here, then writes one sector per frame using LinkFullSave_WriteSector.
 *
 * RETURNS: TRUE if Flash is not present (error), FALSE on success
 */
bool8 LinkFullSave_Init(void)
{
    if (gFlashMemoryPresent != TRUE)
        return TRUE;

    UpdateSaveAddresses();
    SaveSerializedGame();
    RestoreSaveBackupVarsAndIncrement(gRamSaveSectorLocations);
    return FALSE;
}

/**
 * FUNCTION: LinkFullSave_WriteSector
 *
 * PURPOSE: Write one sector during an incremental link save.
 *
 * RETURNS: TRUE when all sectors have been written, FALSE if more remain
 */
bool8 LinkFullSave_WriteSector(void)
{
    u8 status = HandleWriteIncrementalSector(NUM_SECTORS_PER_SLOT, gRamSaveSectorLocations);
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_NORMAL);

    if (status == SAVE_STATUS_ERROR)
        return TRUE;
    else
        return FALSE;
}

/**
 * FUNCTION: LinkFullSave_ReplaceLastSector
 *
 * PURPOSE: Replace the last sector using the safe byte-by-byte method.
 *
 * HOW IT WORKS:
 * The last sector is written using HandleReplaceSector (byte-by-byte with
 * deferred signature) instead of the normal sector write. This ensures
 * the save is atomic -- all data is written before the signature validates it.
 */
bool8 LinkFullSave_ReplaceLastSector(void)
{
    HandleReplaceSectorAndVerify(NUM_SECTORS_PER_SLOT, gRamSaveSectorLocations);
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_NORMAL);

    return FALSE;
}

/**
 * FUNCTION: LinkFullSave_SetLastSectorSignature
 *
 * PURPOSE: Write the final signature byte to commit the incremental save.
 *
 * HOW IT WORKS:
 * This is the "commit" step for the incremental save. Once this signature
 * byte is written, the entire save slot is valid and will be loaded on
 * next boot. Until this point, the old save slot remains the valid one.
 */
bool8 LinkFullSave_SetLastSectorSignature(void)
{
    CopySectorSignatureByte(NUM_SECTORS_PER_SLOT, gRamSaveSectorLocations);
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_NORMAL);

    return FALSE;
}

/**
 * FUNCTION: WriteSaveBlock2
 *
 * PURPOSE: Write only SaveBlock2 to Flash (used during certain link saves).
 *
 * HOW IT WORKS:
 * Initializes the save state without incrementing the counter/rotation,
 * then replaces just the SaveBlock2 sector. Used in conjunction with
 * WriteSaveBlock1Sector to save only player data during link play.
 */
bool8 WriteSaveBlock2(void)
{
    if (gFlashMemoryPresent != TRUE)
        return TRUE;

    UpdateSaveAddresses();
    SaveSerializedGame();
    RestoreSaveBackupVars(gRamSaveSectorLocations);

    // Because RestoreSaveBackupVars is called immediately prior,
    // gIncrementalSectorId will always be 0 (SECTOR_ID_SAVEBLOCK2) at this point,
    // so this function only saves the first sector (SECTOR_ID_SAVEBLOCK2)
    HandleReplaceSectorAndVerify(gIncrementalSectorId + 1, gRamSaveSectorLocations);
    return FALSE;
}

/**
 * FUNCTION: WriteSaveBlock1Sector
 *
 * PURPOSE: Write one sector of SaveBlock1 to Flash per call.
 *
 * HOW IT WORKS:
 * Called repeatedly in a task, writing one SaveBlock1 sector each time.
 * Uses HandleReplaceSector (safe byte-by-byte write) and WriteSectorSignatureByte
 * for each sector. When all SaveBlock1 sectors are written, writes the
 * final signature byte and returns TRUE.
 *
 * Used in conjunction with WriteSaveBlock2 for link saves that need to
 * save player data without saving PC storage.
 *
 * RETURNS: TRUE when all SaveBlock1 sectors are written, FALSE otherwise
 */
// Used in conjunction with WriteSaveBlock2 to write both for certain link saves.
// This is called repeatedly in a task, writing one sector of SaveBlock1 each time it is called.
// Returns TRUE when all sectors of SaveBlock1 have been written.
bool8 WriteSaveBlock1Sector(void)
{
    u8 finished = FALSE;
    u16 sectorId = ++gIncrementalSectorId;
    if (sectorId <= SECTOR_ID_SAVEBLOCK1_END)
    {
        HandleReplaceSectorAndVerify(gIncrementalSectorId + 1, gRamSaveSectorLocations);
        WriteSectorSignatureByte(sectorId, gRamSaveSectorLocations);
    }
    else
    {
        WriteSectorSignatureByte(sectorId, gRamSaveSectorLocations);
        finished = TRUE;
    }
    if (gDamagedSaveSectors)
        DoSaveFailedScreen(SAVE_LINK);

    return finished;
}

/**
 * FUNCTION: LoadGameSave
 *
 * PURPOSE: Load the game save from Flash memory.
 *
 * HOW IT WORKS:
 * For SAVE_NORMAL: loads the full save slot (validates both slots, picks
 * the newest valid one, copies all data to RAM), then deserializes the
 * game state (copies party and object events from save blocks to working arrays).
 *
 * For SAVE_HALL_OF_FAME: loads the two Hall of Fame sectors into the
 * decompression buffer.
 *
 * PARAMETERS:
 * @param saveType -- SAVE_NORMAL or SAVE_HALL_OF_FAME
 *
 * RETURNS: SAVE_STATUS_OK, SAVE_STATUS_ERROR, SAVE_STATUS_EMPTY, etc.
 */
u8 LoadGameSave(u8 saveType)
{
    u8 result;

    if (gFlashMemoryPresent != TRUE)
    {
        gSaveFileStatus = SAVE_STATUS_NO_FLASH;
        return SAVE_STATUS_ERROR;
    }

    UpdateSaveAddresses();
    switch (saveType)
    {
    case SAVE_NORMAL:
    default:
        result = TryLoadSaveSlot(FULL_SAVE_SLOT, gRamSaveSectorLocations);
        LoadSerializedGame();
        gSaveFileStatus = result;
        gGameContinueCallback = NULL;
        break;
    case SAVE_HALL_OF_FAME:
        result = TryLoadSaveSector(SECTOR_ID_HOF_1, gDecompressionBuffer, SECTOR_DATA_SIZE);
        if (result == SAVE_STATUS_OK)
            result = TryLoadSaveSector(SECTOR_ID_HOF_2, gDecompressionBuffer + SECTOR_DATA_SIZE, SECTOR_DATA_SIZE);
        break;
    }

    return result;
}

/**
 * FUNCTION: TryReadSpecialSaveSector
 *
 * PURPOSE: Read data from Trainer Tower save sectors.
 *
 * HOW IT WORKS:
 * The Trainer Tower uses sectors 30-31 for its data, with a special
 * sentinel value (SPECIAL_SECTOR_SENTINEL) at the start instead of
 * the normal sector ID/checksum. The actual data starts at offset 4
 * (after the sentinel).
 *
 * PARAMETERS:
 * @param sectorId -- Must be SECTOR_ID_TRAINER_TOWER_1 or _2
 * @param dst      -- Destination buffer
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
u32 TryReadSpecialSaveSector(u8 sectorId, u8 *dst)
{
    s32 i;
    s32 size;
    u8 *savData;

    if (sectorId != SECTOR_ID_TRAINER_TOWER_1 && sectorId != SECTOR_ID_TRAINER_TOWER_2)
        return SAVE_STATUS_ERROR;

    ReadFlash(sectorId, 0, (u8 *)&gSaveDataBuffer, SECTOR_SIZE);
    if (*(u32 *)(&gSaveDataBuffer.data[0]) != SPECIAL_SECTOR_SENTINEL)
        return SAVE_STATUS_ERROR;

    // copies whole save sector except the counter field
    i = 0;
    size = SECTOR_COUNTER_OFFSET - 1;
    savData = &gSaveDataBuffer.data[4]; // to skip past SPECIAL_SECTOR_SENTINEL
    for (; i <= size; i++)
        dst[i] = savData[i];

    return SAVE_STATUS_OK;
}

/**
 * FUNCTION: TryWriteSpecialSaveSector
 *
 * PURPOSE: Write data to Trainer Tower save sectors.
 *
 * HOW IT WORKS:
 * Writes the SPECIAL_SECTOR_SENTINEL at the start of the buffer, then
 * copies the source data starting at offset 4. Uses ProgramFlashSectorAndVerify
 * for the actual Flash write.
 *
 * PARAMETERS:
 * @param sector -- Must be SECTOR_ID_TRAINER_TOWER_1 or _2
 * @param src    -- Source data buffer
 *
 * RETURNS: SAVE_STATUS_OK or SAVE_STATUS_ERROR
 */
u32 TryWriteSpecialSaveSector(u8 sector, u8 *src)
{
    s32 i;
    s32 size;
    u8 *savData;
    void *savDataBuffer;

    if (sector != SECTOR_ID_TRAINER_TOWER_1 && sector != SECTOR_ID_TRAINER_TOWER_2)
        return SAVE_STATUS_ERROR;

    savDataBuffer = &gSaveDataBuffer;
    *(u32 *)(savDataBuffer) = SPECIAL_SECTOR_SENTINEL;

    // copies whole save sector except the counter field
    i = 0;
    size = SECTOR_COUNTER_OFFSET - 1;
    savData = &gSaveDataBuffer.data[4]; // to skip past SPECIAL_SECTOR_SENTINEL
    for (; i <= size; i++)
        savData[i] = src[i];

    if (ProgramFlashSectorAndVerify(sector, savDataBuffer) != 0)
        return SAVE_STATUS_ERROR;

    return SAVE_STATUS_OK;
}

/**
 * FUNCTION: Task_LinkFullSave
 *
 * PURPOSE: State machine task that performs a full save during link play,
 *          spread across many frames to avoid freezing the link.
 *
 * HOW IT WORKS:
 * This task runs as a background task during multiplayer link sessions.
 * It proceeds through 12 states:
 *
 *   State 0:  Disable soft reset (prevent player from resetting during save)
 *   State 1:  Set link standby (tell other GBAs we're busy saving)
 *   State 2:  Wait for link task to finish, then save the map view
 *   State 3:  Set continue-game warp and initialize the save
 *   State 4:  Wait 5 frames (give the link time to sync)
 *   State 5:  Write one sector; if more remain, go back to state 4
 *   State 6:  Replace the last sector (safe byte-by-byte write)
 *   State 7:  Clear continue-game warp, notify link we're done
 *   State 8:  Wait for link task, then write final signature byte
 *   State 9:  Notify link again
 *   State 10: Wait for link task to finish
 *   State 11: Wait 5 more frames, re-enable soft reset, destroy task
 *
 * GBA CONTEXT:
 * During link play, saving must be carefully coordinated:
 *   - Both GBAs must agree on when to pause communication
 *   - Soft reset is disabled to prevent partial saves
 *   - Writing is spread across frames so the link doesn't timeout
 *
 * PARAMETERS:
 * @param taskId -- ID of this task in the task system
 */
void Task_LinkFullSave(u8 taskId)
{
    switch (gTasks[taskId].data[0])
    {
    case 0:
        gSoftResetDisabled = TRUE;
        gTasks[taskId].data[0] = 1;
        break;
    case 1:
        SetLinkStandbyCallback();
        gTasks[taskId].data[0] = 2;
        break;
    case 2:
        if (IsLinkTaskFinished())
        {
            SaveMapView();
            gTasks[taskId].data[0] = 3;
        }
        break;
    case 3:
        SetContinueGameWarpStatusToDynamicWarp();
        LinkFullSave_Init();
        gTasks[taskId].data[0] = 4;
        break;
    case 4:
        /* Wait 5 frames between sector writes to keep link alive */
        if (++gTasks[taskId].data[1] == 5)
        {
            gTasks[taskId].data[1] = 0;
            gTasks[taskId].data[0] = 5;
        }
        break;
    case 5:
        if (LinkFullSave_WriteSector())
            gTasks[taskId].data[0] = 6;  /* All sectors written */
        else
            gTasks[taskId].data[0] = 4;  /* More sectors to write */
        break;
    case 6:
        LinkFullSave_ReplaceLastSector();
        gTasks[taskId].data[0] = 7;
        break;
    case 7:
        ClearContinueGameWarpStatus2();
        SetLinkStandbyCallback();
        gTasks[taskId].data[0] = 8;
        break;
    case 8:
        if (IsLinkTaskFinished())
        {
            /* Final commit: write the signature byte to make the save valid */
            LinkFullSave_SetLastSectorSignature();
            gTasks[taskId].data[0] = 9;
        }
        break;
    case 9:
        SetLinkStandbyCallback();
        gTasks[taskId].data[0] = 10;
        break;
    case 10:
        if (IsLinkTaskFinished())
            gTasks[taskId].data[0]++;
        break;
    case 11:
        if (++gTasks[taskId].data[1] > 5)
        {
            gSoftResetDisabled = FALSE;
            DestroyTask(taskId);
        }
        break;
    }
}
