/**
 * @file main.h
 * @brief Main Engine Header — Callback System, Input, and Core Game Loop Structure
 *
 * FILE OVERVIEW:
 * This header defines the central data structures and function types for the
 * game's main loop system. The GBA game runs as a series of callbacks:
 *
 *   - callback1: Runs every frame before rendering (game logic updates)
 *   - callback2: The primary screen/state callback (different for each screen)
 *   - vblankCallback: Runs during the vertical blanking period (safe to write VRAM)
 *   - hblankCallback: Runs during each horizontal blank (used for scanline effects)
 *
 * The struct Main also holds the input state — which buttons are pressed, newly
 * pressed, or held — and the OAM (Object Attribute Memory) buffer where sprite
 * data is staged before being DMA-transferred to hardware OAM during VBlank.
 *
 * GBA CONTEXT:
 * MainCallback (function pointer type) is the mechanism used to switch between
 * game screens. Each screen (title, overworld, battle, menu) sets callback2 to
 * its own update function. This callback pattern replaces the OS-level process
 * switching that doesn't exist on the GBA.
 *
 * The OAM buffer (128 entries of OamData) is a shadow copy of the GBA's hardware
 * OAM. Sprite properties are written here during the frame, then DMA-copied to
 * real OAM at 0x07000000 during VBlank to avoid visual tearing.
 *
 * Input is read from REG_KEYINPUT (0x04000130) each frame. The "raw" versions
 * preserve the original button mapping, while the non-raw versions apply the
 * "L=A" accessibility remapping if the player has enabled it in options.
 */
#ifndef GUARD_MAIN_H
#define GUARD_MAIN_H

typedef void (*MainCallback)(void);  /* Function pointer for frame update callbacks */
typedef void (*IntrCallback)(void);  /* Function pointer for interrupt callbacks */
typedef void (*IntrFunc)(void);      /* Function pointer for interrupt table entries */

#include "global.h"

/* Interrupt jump table — indexed by interrupt type (VBlank, HBlank, etc.) */
extern IntrFunc gIntrTable[];

/**
 * STRUCTURE: Main
 *
 * PURPOSE: Central game state structure holding callbacks, input state, and OAM buffer.
 *          The global instance gMain is the heart of the game loop.
 */
struct Main
{
    /*0x000*/ MainCallback callback1;   /* Primary game logic callback (runs before rendering) */
    /*0x004*/ MainCallback callback2;   /* Screen-specific update callback (title, battle, etc.) */

    /*0x008*/ MainCallback savedCallback; /* Saved callback for temporary screen switches */

    /*0x00C*/ IntrCallback vblankCallback; /* Called during VBlank — safe to update VRAM/OAM/palettes */
    /*0x010*/ IntrCallback hblankCallback; /* Called each HBlank — used for scanline effects */
    /*0x014*/ IntrCallback vcountCallback; /* Called at a specific scanline (V-Count match) */
    /*0x018*/ IntrCallback serialCallback; /* Called when serial transfer completes */

    /*0x01C*/ vu16 intrCheck; /* Bitmask of which interrupts have fired this frame */

    /*0x020*/ u32 *vblankCounter1; /* Pointer to an external VBlank counter (for timing) */
    /*0x024*/ u32 vblankCounter2;  /* Internal VBlank frame counter */

    /* --- Input State ---
     * These are updated each frame by reading the GBA's key input register.
     * The GBA has 10 buttons: A, B, Select, Start, Right, Left, Up, Down, R, L */
    /*0x028*/ u16 heldKeysRaw;           /* Currently held buttons (no L=A remap) */
    /*0x02A*/ u16 newKeysRaw;            /* Buttons pressed this frame (no L=A remap) */
    /*0x02C*/ u16 heldKeys;              /* Currently held buttons (with L=A remap if enabled) */
    /*0x02E*/ u16 newKeys;               /* Buttons pressed this frame (with L=A remap) */
    /*0x030*/ u16 newAndRepeatedKeys;    /* New presses + auto-repeated keys (for menu scrolling) */
    /*0x032*/ u16 keyRepeatCounter;      /* Frames until next key repeat triggers */
    /*0x034*/ bool16 watchedKeysPressed; /* TRUE if any watched key was pressed this frame */
    /*0x036*/ u16 watchedKeysMask;       /* Bitmask of keys being watched for press events */

    /* --- OAM Shadow Buffer ---
     * All 128 OAM entries are staged here, then DMA-copied to hardware OAM
     * (0x07000000) during VBlank. Each entry controls one hardware sprite. */
    /*0x038*/ struct OamData oamBuffer[128];

    /*0x438*/ u8 state; /* General-purpose state variable for screen init sequences */

    /* Bitfield flags */
    /*0x439*/ u8 oamLoadDisabled:1; /* When set, prevents OAM DMA transfer during VBlank */
    /*0x439*/ u8 inBattle:1;       /* TRUE when the game is in a battle */
    /*0x439*/ u8 field_439_x4:1;   /* Unknown/unused flag */
};

extern struct Main gMain;
extern bool8 gSoftResetDisabled;
extern bool8 gLinkVSyncDisabled;

extern const u8 gGameVersion;
extern const u8 gGameLanguage;

void AgbMain(void);
void SetMainCallback2(MainCallback callback);
void InitKeys(void);
void SetVBlankCallback(IntrCallback callback);
void SetHBlankCallback(IntrCallback callback);
void SetVCountCallback(IntrCallback callback);
void SetSerialCallback(IntrCallback callback);
void InitFlashTimer(void);
void DoSoftReset(void);
void ClearPokemonCrySongs(void);
void RestoreSerialTimer3IntrHandlers(void);
void SetVBlankCounter1Ptr(u32 *ptr);
void DisableVBlankCounter1(void);
void StartTimer1(void);
void SeedRngAndSetTrainerId(void);
u16 GetGeneratedTrainerIdLower(void);

#define GAME_CODE_LENGTH 4
extern const char RomHeaderGameCode[GAME_CODE_LENGTH];
extern const char RomHeaderSoftwareVersion;

extern u8 gLinkTransferringData;
extern u16 gKeyRepeatStartDelay;

#endif // GUARD_MAIN_H
