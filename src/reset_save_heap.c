/*
 * reset_save_heap.c - Emergency Save Recovery
 *
 * ============================================================================
 * PURPOSE
 * ============================================================================
 *
 * This file handles a "hard reset" that reloads the game from a save file.
 * This is used when something goes wrong (save corruption detected, or the
 * player needs to reload) and the game needs to cleanly restart from the
 * last save point without a full power cycle.
 *
 * Unlike DoSoftReset() (which reboots from the ROM entry point), this
 * function preserves the save data and jumps directly to the "continue
 * saved game" flow, skipping the title screen.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "m4a.h"
#include "load_save.h"
#include "save.h"
#include "new_game.h"
#include "overworld.h"

/**
 * FUNCTION: ReloadSave
 *
 * PURPOSE: Reinitialize the game state and reload the player's save file.
 *
 * HOW IT WORKS:
 * 1. Disables interrupts (REG_IME = 0) to prevent handlers from running
 *    during the EWRAM clear.
 * 2. Clears all of EWRAM (256 KB) via RegisterRamReset. This wipes ALL
 *    game data: save block buffers, sprite data, decompression buffers,
 *    heap allocations - everything. It's a clean slate.
 * 3. Clears the Forced Blank bit from DISPCNT. During the reset, the
 *    screen might have been forced blank; this re-enables display output.
 * 4. Re-enables interrupts (restores IME).
 * 5. Reinitializes critical game systems:
 *    - Save block pointers (SetSaveBlocksPointers)
 *    - Menu and Pokemon globals (ResetMenuAndMonGlobals)
 *    - Save system counters (Save_ResetSaveCounters)
 *    - Loads the actual save data from Flash (LoadGameSave)
 * 6. If the save is empty or invalid, sets up default save data.
 * 7. Restores sound settings from the loaded save.
 * 8. Reinitializes the heap (since EWRAM was just wiped).
 * 9. Sets CB2 to CB2_ContinueSavedGame to skip the title screen and
 *    go straight to the overworld at the player's last save location.
 *
 * GBA CONTEXT:
 * RegisterRamReset(RESET_EWRAM) is a BIOS call that zeros all 256 KB
 * of EWRAM. This is the nuclear option - it destroys ALL game state.
 * Everything must be rebuilt from the Flash save data.
 *
 * Interrupts are disabled during the clear because interrupt handlers
 * (VBlankIntr, etc.) access EWRAM data. If an interrupt fires while
 * EWRAM is being zeroed, it would read garbage and crash.
 */
void ReloadSave(void)
{
    u16 imeBackup = REG_IME;

    REG_IME = 0;                                                    /* Disable all interrupts */
    RegisterRamReset(RESET_EWRAM);                                  /* Zero all 256 KB of EWRAM */
    ClearGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_FORCED_BLANK);     /* Re-enable display */
    REG_IME = imeBackup;                                            /* Restore interrupt state */

    gMain.inBattle = FALSE;
    SetSaveBlocksPointers();                   /* Point save block pointers to correct RAM */
    ResetMenuAndMonGlobals();                  /* Reset menu state and Pokemon globals */
    Save_ResetSaveCounters();                  /* Reset save file counters */
    LoadGameSave(SAVE_NORMAL);                 /* Read save data from Flash memory */

    /* If no valid save exists, set up default values */
    if (gSaveFileStatus == SAVE_STATUS_EMPTY || gSaveFileStatus == SAVE_STATUS_INVALID)
        Sav2_ClearSetDefault();

    SetPokemonCryStereo(gSaveBlock2Ptr->optionsSound);  /* Restore sound settings */
    InitHeap(gHeap, HEAP_SIZE);                          /* Rebuild the heap (EWRAM was zeroed) */
    SetMainCallback2(CB2_ContinueSavedGame);             /* Skip title, go to overworld */
}
