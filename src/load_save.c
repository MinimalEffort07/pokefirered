/**
 * load_save.c - Save Data Management and Memory Layout
 *
 * ============================================================================
 * GBA SAVE SYSTEM ARCHITECTURE
 * ============================================================================
 *
 * FLASH MEMORY:
 * The GBA game cartridge contains Flash ROM for persistent save storage.
 * Pokemon FireRed uses a 128KB Flash chip (e.g., Macronix MX29L010 or
 * Sanyo LE26FV10N1TS). Flash memory has special characteristics:
 *   - Must be erased in 4KB sectors before writing
 *   - Writes are slow (~10ms per byte) compared to reads
 *   - Has a limited number of write cycles (~10,000-100,000 per sector)
 *
 * SAVE BLOCK ARCHITECTURE:
 * The game's save data is split into three main blocks:
 *
 *   SaveBlock2 (gSaveBlock2): Player identity and settings
 *     - Player name, gender, trainer ID
 *     - Options (text speed, sound, battle style)
 *     - Pokedex data (seen/owned flags)
 *     - Battle Tower records
 *     - Encryption key
 *
 *   SaveBlock1 (gSaveBlock1): World state
 *     - Current location, map, and position
 *     - Event flags and variables (story progress)
 *     - Player party Pokemon
 *     - Bag items (5 pockets)
 *     - Money, coins
 *     - Mail data
 *     - Object event states (NPCs)
 *
 *   PokemonStorage (gPokemonStorage): PC box system
 *     - 14 boxes of 30 Pokemon each = 420 Pokemon
 *     - Box names and wallpapers
 *
 * ADDRESS SPACE LAYOUT RANDOMIZATION (ASLR):
 * As a primitive anti-cheating/anti-tampering measure, the save blocks are
 * accessed through pointers (gSaveBlock1Ptr, gSaveBlock2Ptr) that are offset
 * by a random amount within a 128-byte range. This means the actual memory
 * address of save data changes each time the game boots, making it harder
 * for cheat devices (GameShark/Action Replay) to find fixed addresses.
 *
 * ENCRYPTION:
 * Certain sensitive values (money, coins, game stats, bag items) are XOR'd
 * with an encryption key stored in SaveBlock2. When the key changes (after
 * each save), all encrypted values must be re-encrypted with the new key.
 * This prevents simple memory editing to change money/item counts.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "gba/flash_internal.h"
#include "load_save.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "random.h"
#include "item.h"
#include "save_location.h"
#include "berry_powder.h"
#include "overworld.h"
#include "quest_log.h"
#include "poke_radar.h"
#include "pokemon_follower.h"
#include "multiplayer.h"

/*
 * SAVEBLOCK_MOVE_RANGE: The size of the random offset range for ASLR.
 * Each save block has an extra 128 bytes of padding after it, and the
 * pointer to the block can be offset by 0 to 124 bytes (aligned to 4).
 * This means the actual struct starts somewhere within the first 128 bytes
 * of the allocated region.
 */
#define SAVEBLOCK_MOVE_RANGE    128

/**
 * LoadedSaveData: A temporary structure used to hold bag items and mail
 * during save/load operations.
 *
 * When loading a save, the bag items are copied here from the save block,
 * then decrypted in place. When saving, they're encrypted and copied back.
 * This intermediate buffer prevents corruption of the working save data
 * if the encryption key changes during the save process.
 */
struct LoadedSaveData
{
 /*0x0000*/ struct ItemSlot items[BAG_ITEMS_COUNT];       /* Regular items pocket */
 /*0x0078*/ struct ItemSlot keyItems[BAG_KEYITEMS_COUNT]; /* Key items pocket */
 /*0x00F0*/ struct ItemSlot pokeBalls[BAG_POKEBALLS_COUNT]; /* Poke Balls pocket */
 /*0x0130*/ struct ItemSlot TMsHMs[BAG_TMHM_COUNT];      /* TMs & HMs pocket */
 /*0x0230*/ struct ItemSlot berries[BAG_BERRIES_COUNT];   /* Berries pocket */
 /*0x02E8*/ struct Mail mail[MAIL_COUNT];                 /* Held mail data */
};

// EWRAM DATA
/*
 * The three save blocks and their DMA padding buffers.
 * Each block is followed by SAVEBLOCK_MOVE_RANGE (128) bytes of padding
 * to allow the ASLR offset without overflowing into adjacent data.
 *
 * These live in EWRAM (External Work RAM, 256KB at 0x02000000).
 * EWRAM is the largest RAM region on the GBA and is where most game
 * data resides during execution.
 */
EWRAM_DATA struct SaveBlock2 gSaveBlock2 = {0};
EWRAM_DATA u8 gSaveBlock2_DMA[SAVEBLOCK_MOVE_RANGE] = {0};

EWRAM_DATA struct SaveBlock1 gSaveBlock1 = {0};
EWRAM_DATA u8 gSaveBlock1_DMA[SAVEBLOCK_MOVE_RANGE] = {0};

EWRAM_DATA struct PokemonStorage gPokemonStorage = {0};
EWRAM_DATA u8 gSaveBlock3_DMA[SAVEBLOCK_MOVE_RANGE] = {0};

EWRAM_DATA struct LoadedSaveData gLoadedSaveData = {0};
EWRAM_DATA u32 gLastEncryptionKey = 0;

// IWRAM common
/*
 * Global pointers to the save blocks. These are the primary access points
 * used throughout the entire codebase. They point into the save block
 * arrays above, offset by a random amount for ASLR.
 *
 * gFlashMemoryPresent: Whether Flash ROM was detected on the cartridge.
 * Without Flash, the game cannot save.
 */
COMMON_DATA bool32 gFlashMemoryPresent = 0;
COMMON_DATA struct SaveBlock1 *gSaveBlock1Ptr = NULL;
COMMON_DATA struct SaveBlock2 *gSaveBlock2Ptr = NULL;
COMMON_DATA struct PokemonStorage *gPokemonStoragePtr = NULL;

/**
 * FUNCTION: CheckForFlashMemory
 *
 * PURPOSE: Detect whether Flash ROM is present on the cartridge.
 *
 * HOW IT WORKS:
 * Calls IdentifyFlash() (a BIOS/library function) which attempts to
 * communicate with the Flash chip using manufacturer ID commands.
 * If the chip responds (returns 0), Flash is present and we initialize
 * the Flash timer (needed for write timing). If not (returns non-zero),
 * saving will be disabled.
 *
 * GBA CONTEXT:
 * Flash ROM communication uses a specific protocol: writing magic values
 * to specific addresses (0x5555, 0x2AAA) to enter command mode, then
 * reading back manufacturer/device IDs. The InitFlashTimer sets up timing
 * for Flash write operations, which require precise delays.
 */
void CheckForFlashMemory(void)
{
    if (!IdentifyFlash())
    {
        gFlashMemoryPresent = TRUE;
        InitFlashTimer();
    }
    else
    {
        gFlashMemoryPresent = FALSE;
    }
}

/**
 * FUNCTION: ClearSav2
 *
 * PURPOSE: Zero out the entire SaveBlock2 structure (including DMA padding).
 *
 * HOW IT WORKS:
 * Uses CpuFill16 to write 16-bit zeros across the save block. The size
 * includes the DMA padding buffer (SAVEBLOCK_MOVE_RANGE bytes) since the
 * ASLR offset means the struct may extend into that region.
 */
void ClearSav2(void)
{
    CpuFill16(0, &gSaveBlock2, sizeof(struct SaveBlock2) + sizeof(gSaveBlock2_DMA));
}

/**
 * FUNCTION: ClearSav1
 *
 * PURPOSE: Zero out the entire SaveBlock1 structure (including DMA padding).
 */
void ClearSav1(void)
{
    CpuFill16(0, &gSaveBlock1, sizeof(struct SaveBlock1) + sizeof(gSaveBlock1_DMA));
}

/**
 * FUNCTION: SetSaveBlocksPointers
 *
 * PURPOSE: Set the global save block pointers with a random ASLR offset.
 *
 * HOW IT WORKS:
 * 1. Generates a random offset (0-124, aligned to 4 bytes) from the PRNG
 * 2. Sets each save block pointer to the base address + this offset
 * 3. Updates bag pocket pointers (which are derived from gSaveBlock1Ptr)
 * 4. Notifies the Quest Log system of the address change
 *
 * The masking expression (Random()) & ((SAVEBLOCK_MOVE_RANGE - 1) & ~3):
 *   SAVEBLOCK_MOVE_RANGE - 1 = 127 = 0x7F (limit to 0-127 range)
 *   & ~3 = clear bottom 2 bits (4-byte alignment)
 *   Result: random value from {0, 4, 8, 12, ..., 124}
 *
 * GBA CONTEXT:
 * 4-byte alignment is important because the ARM7TDMI CPU requires 32-bit
 * (4-byte) alignment for 32-bit memory accesses. Misaligned accesses
 * cause a rotation of the read data, which would corrupt the save blocks.
 */
void SetSaveBlocksPointers(void)
{
    u32 offset;
    struct SaveBlock1** sav1_LocalVar = &gSaveBlock1Ptr;
    void *oldSave = (void *)gSaveBlock1Ptr;

    /* Generate random 4-byte-aligned offset within the ASLR range */
    offset = (Random()) & ((SAVEBLOCK_MOVE_RANGE - 1) & ~3);

    /* Set all three save block pointers with the same offset */
    gSaveBlock2Ptr = (void *)(&gSaveBlock2) + offset;
    *sav1_LocalVar = (void *)(&gSaveBlock1) + offset;
    gPokemonStoragePtr = (void *)(&gPokemonStorage) + offset;

    /* Update derived pointers that depend on gSaveBlock1Ptr's address */
    SetBagPocketsPointers();
    QL_AddASLROffset(oldSave);  /* Quest Log needs to know the new offset */
}

/**
 * FUNCTION: MoveSaveBlocks_ResetHeap
 *
 * PURPOSE: Relocate save blocks to a new random ASLR offset and regenerate
 *          the encryption key.
 *
 * HOW IT WORKS:
 * This is a complex operation performed when loading a save or during
 * certain game transitions. The steps are:
 *
 * 1. DISABLE INTERRUPTS: Save and clear VBlank/HBlank callbacks to prevent
 *    them from accessing save data while it's being moved.
 *
 * 2. BACKUP SAVE DATA: Copy all three save blocks into the heap (temporary
 *    storage). The heap is at the end of EWRAM and is large enough to hold
 *    all three blocks.
 *
 * 3. RANDOMIZE POINTERS: Call SetSaveBlocksPointers to assign new random
 *    offsets to the save block pointers.
 *
 * 4. RESTORE SAVE DATA: Copy the backed-up data back to the new pointer
 *    locations. The data itself is unchanged; only its position in memory
 *    has moved.
 *
 * 5. RESET HEAP: The heap was used as temporary storage and is now full of
 *    save block data. Reinitialize it to reclaim the memory.
 *
 * 6. RESTORE INTERRUPTS: Re-enable the VBlank/HBlank callbacks.
 *
 * 7. NEW ENCRYPTION KEY: Generate a new random encryption key and re-encrypt
 *    all sensitive data (money, coins, items, etc.) with it.
 *
 * GBA CONTEXT:
 * VBlank/HBlank callbacks must be disabled during this operation because
 * they may access save block data through the global pointers. If a VBlank
 * interrupt fires while the pointers are being changed, the callback could
 * read from an invalid address, causing a crash or data corruption.
 */
void MoveSaveBlocks_ResetHeap(void)
{
    void *vblankCB, *hblankCB;
    u32 encryptionKey;
    struct SaveBlock2 *saveBlock2Copy;
    struct SaveBlock1 *saveBlock1Copy;
    struct PokemonStorage *pokemonStorageCopy;

    /* Save and disable interrupt callbacks to prevent concurrent access */
    vblankCB = gMain.vblankCallback;
    hblankCB = gMain.hblankCallback;
    gMain.vblankCallback = NULL;
    gMain.hblankCallback = NULL;
    gMain.vblankCounter1 = NULL;

    /*
     * Use the heap as temporary backup storage.
     * The three blocks are laid out consecutively in the heap.
     */
    saveBlock2Copy = (struct SaveBlock2 *)(gHeap);
    saveBlock1Copy = (struct SaveBlock1 *)(gHeap + sizeof(struct SaveBlock2));
    pokemonStorageCopy = (struct PokemonStorage *)(gHeap + sizeof(struct SaveBlock2) + sizeof(struct SaveBlock1));

    /* Backup current save data to heap */
    *saveBlock2Copy = *gSaveBlock2Ptr;
    *saveBlock1Copy = *gSaveBlock1Ptr;
    *pokemonStorageCopy = *gPokemonStoragePtr;

    /* Randomize the save block pointer offsets */
    SetSaveBlocksPointers(); // unlike Emerald, this does not use
                             // the trainer ID sum for an offset.

    /* Restore save data at the new pointer locations */
    *gSaveBlock2Ptr = *saveBlock2Copy;
    *gSaveBlock1Ptr = *saveBlock1Copy;
    *gPokemonStoragePtr = *pokemonStorageCopy;

    /* Heap was used as scratch space -- reinitialize it */
    InitHeap(gHeap, HEAP_SIZE);

    /* Restore interrupt callbacks */
    gMain.hblankCallback = hblankCB;
    gMain.vblankCallback = vblankCB;

    /*
     * Generate a new encryption key from two random 16-bit values
     * combined into a 32-bit key. Re-encrypt all sensitive data
     * with the new key.
     */
    encryptionKey = (Random() << 0x10) + (Random());
    ApplyNewEncryptionKeyToAllEncryptedData(encryptionKey);
    gSaveBlock2Ptr->encryptionKey = encryptionKey;
}

/**
 * FUNCTION: UseContinueGameWarp
 *
 * PURPOSE: Check if the player should warp to a special continue location.
 *
 * GAME LOGIC:
 * The "continue game warp" flag is set when the player saves in certain
 * special locations (like the Pokemon League). When loading that save,
 * instead of resuming at the save location, the game warps the player
 * to a different (safer) location.
 *
 * RETURNS: Non-zero if the continue warp flag is set
 */
u32 UseContinueGameWarp(void)
{
    return gSaveBlock2Ptr->specialSaveWarpFlags & CONTINUE_GAME_WARP;
}

/** Clear the continue-game-warp flag */
void ClearContinueGameWarpStatus(void)
{
    gSaveBlock2Ptr->specialSaveWarpFlags &= ~CONTINUE_GAME_WARP;
}

/** Set the continue-game-warp flag */
void SetContinueGameWarpStatus(void)
{
    gSaveBlock2Ptr->specialSaveWarpFlags |= CONTINUE_GAME_WARP;
}

/**
 * FUNCTION: SetContinueGameWarpStatusToDynamicWarp
 *
 * PURPOSE: Set both the continue warp destination and flag.
 *
 * GAME LOGIC:
 * "Dynamic warp" (slot 0) stores a warp destination that was set
 * programmatically during gameplay, as opposed to a fixed warp pad.
 */
void SetContinueGameWarpStatusToDynamicWarp(void)
{
    SetContinueGameWarpToDynamicWarp(0);
    gSaveBlock2Ptr->specialSaveWarpFlags |= CONTINUE_GAME_WARP;
}

/** Clear the continue warp flag (duplicate of ClearContinueGameWarpStatus) */
void ClearContinueGameWarpStatus2(void)
{
    gSaveBlock2Ptr->specialSaveWarpFlags &= ~CONTINUE_GAME_WARP;
}

/**
 * FUNCTION: SavePlayerParty
 *
 * PURPOSE: Copy the player's party from the working array to the save block.
 *
 * HOW IT WORKS:
 * The player's party Pokemon are stored in two places:
 *   gPlayerParty[]: Working copy used during gameplay (fast IWRAM access)
 *   gSaveBlock1Ptr->playerParty[]: Save data copy (in EWRAM)
 *
 * This function syncs the working copy to the save copy. All 6 party
 * slots are always copied, even if the party has fewer than 6 Pokemon
 * (the unused slots contain zeroed-out data).
 */
void SavePlayerParty(void)
{
    int i;

    gSaveBlock1Ptr->playerPartyCount = gPlayerPartyCount;

    for (i = 0; i < PARTY_SIZE; i++)
        gSaveBlock1Ptr->playerParty[i] = gPlayerParty[i];
}

/**
 * FUNCTION: LoadPlayerParty
 *
 * PURPOSE: Copy the player's party from the save block to the working array.
 */
void LoadPlayerParty(void)
{
    int i;

    gPlayerPartyCount = gSaveBlock1Ptr->playerPartyCount;

    for (i = 0; i < PARTY_SIZE; i++)
        gPlayerParty[i] = gSaveBlock1Ptr->playerParty[i];
}

/**
 * FUNCTION: SaveObjectEvents
 *
 * PURPOSE: Copy NPC/object event data from the working array to the save block.
 *
 * GAME LOGIC:
 * Object events are the NPCs, items, and interactable objects on the map.
 * Their state (position, movement, visibility) is tracked in gObjectEvents[]
 * during gameplay and saved to the save block when the player saves.
 */
void SaveObjectEvents(void)
{
    int i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        gSaveBlock1Ptr->objectEvents[i] = gObjectEvents[i];
}

/**
 * FUNCTION: LoadObjectEvents
 *
 * PURPOSE: Copy object event data from the save block to the working array.
 */
void LoadObjectEvents(void)
{
    int i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        gObjectEvents[i] = gSaveBlock1Ptr->objectEvents[i];
}

/**
 * FUNCTION: SaveSerializedGame
 *
 * PURPOSE: Save all runtime game state that needs to persist.
 *
 * HOW IT WORKS:
 * Copies both the player party and object events from their working
 * arrays to the save block. Called before writing the save block to Flash.
 */
void SaveSerializedGame(void)
{
    SavePlayerParty();
    SaveObjectEvents();
    /* Strip session-only ObjectEvent slots (follower, remote players) so they
     * are not written to Flash -- EWRAM is zeroed on boot, so restoring them
     * would produce frozen ghost sprites with no driver. */
    ClearFollowerFromSaveBlock();
    ClearRemotePlayersFromSaveBlock();
}

/**
 * FUNCTION: LoadSerializedGame
 *
 * PURPOSE: Load all runtime game state from the save block.
 *
 * HOW IT WORKS:
 * Reverse of SaveSerializedGame -- populates the working arrays from
 * the save block data after reading from Flash.
 */
void LoadSerializedGame(void)
{
    LoadPlayerParty();
    LoadObjectEvents();
    /* PokeRadar lives in SaveBlock1::unused_348C[]. On saves that predate
     * this feature, the region is zero/garbage: magic number mismatches,
     * EnsureInit zero-fills the struct and grants the key item. On saves
     * already running this feature, it's a no-op. */
    PokeRadar_EnsureInit();
}

/**
 * FUNCTION: LoadPlayerBag
 *
 * PURPOSE: Copy all bag item data from the save block to the loaded save buffer.
 *
 * HOW IT WORKS:
 * Copies each of the five bag pockets and the mail data from the save block
 * into gLoadedSaveData. This intermediate buffer is used because bag items
 * may be encrypted -- the items need to be in a working buffer where they
 * can be decrypted without modifying the save block directly.
 *
 * Also saves the current encryption key so it can be used during the
 * save process to properly re-encrypt the data.
 *
 * GAME LOGIC:
 * The bag has five pockets, each holding different types of items:
 *   Items: Regular consumable items (Potions, status heals, etc.)
 *   Key Items: Story-important items (Bike, Town Map, etc.)
 *   Poke Balls: All varieties of Poke Balls
 *   TMs/HMs: Technical/Hidden Machines (teach moves to Pokemon)
 *   Berries: Berries (healing, stat-boosting, etc.)
 */
void LoadPlayerBag(void)
{
    int i;

    // load player items.
    for (i = 0; i < BAG_ITEMS_COUNT; i++)
        gLoadedSaveData.items[i] = gSaveBlock1Ptr->bagPocket_Items[i];

    // load player key items.
    for (i = 0; i < BAG_KEYITEMS_COUNT; i++)
        gLoadedSaveData.keyItems[i] = gSaveBlock1Ptr->bagPocket_KeyItems[i];

    // load player pokeballs.
    for (i = 0; i < BAG_POKEBALLS_COUNT; i++)
        gLoadedSaveData.pokeBalls[i] = gSaveBlock1Ptr->bagPocket_PokeBalls[i];

    // load player TMs and HMs.
    for (i = 0; i < BAG_TMHM_COUNT; i++)
        gLoadedSaveData.TMsHMs[i] = gSaveBlock1Ptr->bagPocket_TMHM[i];

    // load player berries.
    for (i = 0; i < BAG_BERRIES_COUNT; i++)
        gLoadedSaveData.berries[i] = gSaveBlock1Ptr->bagPocket_Berries[i];

    // load mail.
    for (i = 0; i < MAIL_COUNT; i++)
        gLoadedSaveData.mail[i] = gSaveBlock1Ptr->mail[i];

    /* Save the encryption key that was used to encrypt this data */
    gLastEncryptionKey = gSaveBlock2Ptr->encryptionKey;
}

/**
 * FUNCTION: SavePlayerBag
 *
 * PURPOSE: Copy bag item data from the loaded save buffer back to the save block.
 *
 * HOW IT WORKS:
 * Reverse of LoadPlayerBag. Additionally handles encryption key changes:
 * 1. Copies all bag data back to the save block
 * 2. Temporarily restores the old encryption key
 * 3. Re-encrypts bag items with the new key using ApplyNewEncryptionKeyToBagItems
 * 4. Restores the current encryption key
 *
 * This dance is necessary because the encryption key may have changed
 * since the bag was loaded (due to MoveSaveBlocks_ResetHeap), and the
 * item data needs to be encrypted with the current key for saving.
 */
void SavePlayerBag(void)
{
    int i;
    u32 encryptionKeyBackup;

    // save player items.
    for (i = 0; i < BAG_ITEMS_COUNT; i++)
        gSaveBlock1Ptr->bagPocket_Items[i] = gLoadedSaveData.items[i];

    // save player key items.
    for (i = 0; i < BAG_KEYITEMS_COUNT; i++)
        gSaveBlock1Ptr->bagPocket_KeyItems[i] = gLoadedSaveData.keyItems[i];

    // save player pokeballs.
    for (i = 0; i < BAG_POKEBALLS_COUNT; i++)
        gSaveBlock1Ptr->bagPocket_PokeBalls[i] = gLoadedSaveData.pokeBalls[i];

    // save player TMs and HMs.
    for (i = 0; i < BAG_TMHM_COUNT; i++)
        gSaveBlock1Ptr->bagPocket_TMHM[i] = gLoadedSaveData.TMsHMs[i];

    // save player berries.
    for (i = 0; i < BAG_BERRIES_COUNT; i++)
        gSaveBlock1Ptr->bagPocket_Berries[i] = gLoadedSaveData.berries[i];

    // save mail.
    for (i = 0; i < MAIL_COUNT; i++)
        gSaveBlock1Ptr->mail[i] = gLoadedSaveData.mail[i];

    /*
     * Re-encrypt bag items with the correct encryption key.
     * The items in gLoadedSaveData were encrypted with gLastEncryptionKey,
     * but gSaveBlock2Ptr->encryptionKey may have changed. We temporarily
     * set the key to the old value, re-encrypt with the new one, then restore.
     */
    encryptionKeyBackup = gSaveBlock2Ptr->encryptionKey;
    gSaveBlock2Ptr->encryptionKey = gLastEncryptionKey;
    ApplyNewEncryptionKeyToBagItems(encryptionKeyBackup);
    gSaveBlock2Ptr->encryptionKey = encryptionKeyBackup;
}

/**
 * FUNCTION: ApplyNewEncryptionKeyToHword
 *
 * PURPOSE: Re-encrypt a 16-bit value when the encryption key changes.
 *
 * HOW IT WORKS:
 * XOR-based encryption is self-inverse: XOR'ing with the same key twice
 * returns the original value. To change the encryption key:
 *   1. XOR with old key (decrypts to plaintext)
 *   2. XOR with new key (re-encrypts with new key)
 *
 * PARAMETERS:
 * @param hWord  -- Pointer to the encrypted 16-bit value
 * @param newKey -- The new encryption key to apply (only low 16 bits used)
 */
void ApplyNewEncryptionKeyToHword(u16 *hWord, u32 newKey)
{
    *hWord ^= gSaveBlock2Ptr->encryptionKey;  /* Decrypt with old key */
    *hWord ^= newKey;                          /* Re-encrypt with new key */
}

/**
 * FUNCTION: ApplyNewEncryptionKeyToWord
 *
 * PURPOSE: Re-encrypt a 32-bit value when the encryption key changes.
 *
 * HOW IT WORKS:
 * Same XOR key-change approach as ApplyNewEncryptionKeyToHword but for
 * 32-bit values.
 *
 * PARAMETERS:
 * @param word   -- Pointer to the encrypted 32-bit value
 * @param newKey -- The new encryption key
 */
void ApplyNewEncryptionKeyToWord(u32 *word, u32 newKey)
{
    *word ^= gSaveBlock2Ptr->encryptionKey;   /* Decrypt with old key */
    *word ^= newKey;                           /* Re-encrypt with new key */
}

/**
 * FUNCTION: ApplyNewEncryptionKeyToAllEncryptedData
 *
 * PURPOSE: Re-encrypt ALL sensitive save data with a new encryption key.
 *
 * HOW IT WORKS:
 * Iterates through every encrypted field in the save data and applies
 * the key change. The encrypted fields are:
 *   - Trainer Tower best times (one per challenge type)
 *   - Game statistics (steps, battles, etc.)
 *   - Bag items (counts are encrypted)
 *   - Berry powder amount
 *   - Money (gSaveBlock1Ptr->money)
 *   - Game Corner coins (gSaveBlock1Ptr->coins)
 *
 * GAME LOGIC:
 * This is called whenever a new encryption key is generated (during
 * MoveSaveBlocks_ResetHeap). Every encrypted value must be re-encrypted
 * atomically -- if the process were interrupted, some values would use
 * the old key and some the new key, corrupting the save.
 *
 * PARAMETERS:
 * @param encryptionKey -- The new encryption key to apply
 */
void ApplyNewEncryptionKeyToAllEncryptedData(u32 encryptionKey)
{
    int i;

    /* Re-encrypt Trainer Tower best times */
    for(i = 0; i < NUM_TOWER_CHALLENGE_TYPES; i++)
        ApplyNewEncryptionKeyToWord(&gSaveBlock1Ptr->trainerTower[i].bestTime, encryptionKey);

    /* Re-encrypt game statistics */
    ApplyNewEncryptionKeyToGameStats(encryptionKey);

    /* Re-encrypt bag item data */
    ApplyNewEncryptionKeyToBagItems_(encryptionKey);

    /* Re-encrypt berry powder amount */
    ApplyNewEncryptionKeyToBerryPowder(encryptionKey);

    /* Re-encrypt money and coins */
    ApplyNewEncryptionKeyToWord(&gSaveBlock1Ptr->money, encryptionKey);
    ApplyNewEncryptionKeyToHword(&gSaveBlock1Ptr->coins, encryptionKey);
}
