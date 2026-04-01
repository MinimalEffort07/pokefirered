/*
 * event_data.c - Event Flags and Variables System
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * This file manages the game's event flags and variables -- the persistent
 * state that tracks the player's progress through the game. Every in-game
 * event (defeating a trainer, picking up an item, progressing a story
 * sequence, enabling the National Pokedex, etc.) is tracked here.
 *
 * FLAGS (boolean on/off):
 *   Stored as individual bits in gSaveBlock1Ptr->flags[] (a byte array).
 *   Each flag index N is stored at byte [N/8], bit (N & 7).
 *   Example: FLAG_BADGE01_GET = trainer defeated, gym badge obtained.
 *
 *   Flag regions:
 *   - Permanent flags: Story progress, trainer defeats, item pickups
 *   - Temporary flags (TEMP_FLAGS): Cleared on every map transition
 *     (used for per-map state like "item already spawned")
 *   - System flags (SYS_FLAGS): Engine features (Strength active, etc.)
 *   - Special flags: Not saved; stored in sSpecialFlags[] (EWRAM only)
 *
 * VARIABLES (16-bit values):
 *   Stored in gSaveBlock1Ptr->vars[] (u16 array).
 *   Used for: step counters, map scene progress, item quantities, etc.
 *
 *   Variable regions:
 *   - Permanent vars: Persist across saves
 *   - Temporary vars (TEMP_VARS): Cleared on map transition
 *   - Special vars (0x8000+): Not saved; point to EWRAM globals like
 *     gSpecialVar_Result, gSpecialVar_LastTalked, etc.
 *
 * QUEST LOG INTEGRATION:
 * When Quest Log recording is active, flag/var changes are also logged
 * so they can be replayed during Quest Log playback. During playback,
 * flag/var reads are redirected to Quest Log stored values.
 *
 * NATIONAL POKEDEX:
 * Uses a triple-check system (magic number + variable + flag) to detect
 * tampering. All three must match for the Pokedex to be recognized as
 * enabled. This is an anti-cheat measure.
 * ============================================================================
 */

#include "global.h"
#include "event_data.h"
#include "item_menu.h"
#include "quest_log.h"

static bool8 IsFlagOrVarStoredInQuestLog(u16 idx, u8 a1);

#define NUM_SPECIAL_FLAGS  (SPECIAL_FLAGS_END - SPECIAL_FLAGS_START + 1)
#define NUM_TEMP_FLAGS     (TEMP_FLAGS_END - TEMP_FLAGS_START + 1)
#define NUM_TEMP_VARS      (TEMP_VARS_END - TEMP_VARS_START + 1)

#define SPECIAL_FLAGS_SIZE (NUM_SPECIAL_FLAGS / 8)  /* 8 flags packed per byte */
#define TEMP_FLAGS_SIZE    (NUM_TEMP_FLAGS / 8)
#define TEMP_VARS_SIZE     (NUM_TEMP_VARS * 2)      /* Each var is 2 bytes (u16) */

EWRAM_DATA u16 gSpecialVar_0x8000 = 0;
EWRAM_DATA u16 gSpecialVar_0x8001 = 0;
EWRAM_DATA u16 gSpecialVar_0x8002 = 0;
EWRAM_DATA u16 gSpecialVar_0x8003 = 0;
EWRAM_DATA u16 gSpecialVar_0x8004 = 0;
EWRAM_DATA u16 gSpecialVar_0x8005 = 0;
EWRAM_DATA u16 gSpecialVar_0x8006 = 0;
EWRAM_DATA u16 gSpecialVar_0x8007 = 0;
EWRAM_DATA u16 gSpecialVar_0x8008 = 0;
EWRAM_DATA u16 gSpecialVar_0x8009 = 0;
EWRAM_DATA u16 gSpecialVar_0x800A = 0;
EWRAM_DATA u16 gSpecialVar_0x800B = 0;
EWRAM_DATA u16 gSpecialVar_Result = 0;
EWRAM_DATA u16 gSpecialVar_LastTalked = 0;
EWRAM_DATA u16 gSpecialVar_Facing = 0;
EWRAM_DATA u16 gSpecialVar_MonBoxId = 0;
EWRAM_DATA u16 gSpecialVar_MonBoxPos = 0;
EWRAM_DATA u16 gSpecialVar_TextColor = 0;
EWRAM_DATA u16 gSpecialVar_PrevTextColor = 0;
EWRAM_DATA u16 gSpecialVar_0x8014 = 0;
EWRAM_DATA u8 sSpecialFlags[SPECIAL_FLAGS_SIZE] = {};

COMMON_DATA u16 gLastQuestLogStoredFlagOrVarIdx = 0;

extern u16 *const gSpecialVars[];

void InitEventData(void)
{
    memset(gSaveBlock1Ptr->flags, 0, sizeof(gSaveBlock1Ptr->flags));
    memset(gSaveBlock1Ptr->vars, 0, sizeof(gSaveBlock1Ptr->vars));
    memset(sSpecialFlags, 0, sizeof(sSpecialFlags));
}

void ClearTempFieldEventData(void)
{
    memset(gSaveBlock1Ptr->flags + (TEMP_FLAGS_START / 8), 0, TEMP_FLAGS_SIZE);
    memset(gSaveBlock1Ptr->vars + ((TEMP_VARS_START - VARS_START) * 2), 0, TEMP_VARS_SIZE);
    FlagClear(FLAG_SYS_WHITE_FLUTE_ACTIVE);
    FlagClear(FLAG_SYS_BLACK_FLUTE_ACTIVE);
    FlagClear(FLAG_SYS_USE_STRENGTH);
    FlagClear(FLAG_SYS_SPECIAL_WILD_BATTLE);
    FlagClear(FLAG_SYS_INFORMED_OF_LOCAL_WIRELESS_PLAYER);
}

// Unused
static void DisableNationalPokedex_RSE(void)
{
    u16 *ptr = GetVarPointer(VAR_0x403C);
    gSaveBlock2Ptr->pokedex.unused = 0;
    *ptr = 0;
    FlagClear(FLAG_0x838);
}

// The magic numbers used here (0xDA and 0x0302) correspond to those
// used in RSE for enabling the national Pokedex
void EnableNationalPokedex_RSE(void)
{
    // Note: the var, struct member, and flag are never used
    u16 *ptr = GetVarPointer(VAR_0x403C);
    gSaveBlock2Ptr->pokedex.unused = 0xDA;
    *ptr = 0x0302;
    FlagSet(FLAG_0x838);
}

// Unused
static bool32 IsNationalPokedexEnabled_RSE(void)
{
    if (gSaveBlock2Ptr->pokedex.unused == 0xDA
            && VarGet(VAR_0x403C) == 0x0302
            && FlagGet(FLAG_0x838))
        return TRUE;

    return FALSE;
}

void DisableNationalPokedex(void)
{
    u16 *nationalDexVar = GetVarPointer(VAR_NATIONAL_DEX);
    gSaveBlock2Ptr->pokedex.nationalMagic = 0;
    *nationalDexVar = 0;
    FlagClear(FLAG_SYS_NATIONAL_DEX);
}

void EnableNationalPokedex(void)
{
    u16 *nationalDexVar = GetVarPointer(VAR_NATIONAL_DEX);
    gSaveBlock2Ptr->pokedex.nationalMagic = 0xB9;
    *nationalDexVar = 0x6258;
    FlagSet(FLAG_SYS_NATIONAL_DEX);
}

bool32 IsNationalPokedexEnabled(void)
{
    if (gSaveBlock2Ptr->pokedex.nationalMagic == 0xB9
            && VarGet(VAR_NATIONAL_DEX) == 0x6258
            && FlagGet(FLAG_SYS_NATIONAL_DEX))
        return TRUE;

    return FALSE;
}

void DisableMysteryGift(void)
{
    FlagClear(FLAG_SYS_MYSTERY_GIFT_ENABLED);
}

void EnableMysteryGift(void)
{
    FlagSet(FLAG_SYS_MYSTERY_GIFT_ENABLED);
}

bool32 IsMysteryGiftEnabled(void)
{
    return FlagGet(FLAG_SYS_MYSTERY_GIFT_ENABLED);
}

void ClearMysteryGiftFlags(void)
{
    FlagClear(FLAG_MYSTERY_GIFT_DONE);
    FlagClear(FLAG_MYSTERY_GIFT_1);
    FlagClear(FLAG_MYSTERY_GIFT_2);
    FlagClear(FLAG_MYSTERY_GIFT_3);
    FlagClear(FLAG_MYSTERY_GIFT_4);
    FlagClear(FLAG_MYSTERY_GIFT_5);
    FlagClear(FLAG_MYSTERY_GIFT_6);
    FlagClear(FLAG_MYSTERY_GIFT_7);
    FlagClear(FLAG_MYSTERY_GIFT_8);
    FlagClear(FLAG_MYSTERY_GIFT_9);
    FlagClear(FLAG_MYSTERY_GIFT_10);
    FlagClear(FLAG_MYSTERY_GIFT_11);
    FlagClear(FLAG_MYSTERY_GIFT_12);
    FlagClear(FLAG_MYSTERY_GIFT_13);
    FlagClear(FLAG_MYSTERY_GIFT_14);
    FlagClear(FLAG_MYSTERY_GIFT_15);
}

void ClearMysteryGiftVars(void)
{
    VarSet(VAR_EVENT_PICHU_SLOT, 0);
    VarSet(VAR_MYSTERY_GIFT_1,  0);
    VarSet(VAR_MYSTERY_GIFT_2,  0);
    VarSet(VAR_MYSTERY_GIFT_3,  0);
    VarSet(VAR_MYSTERY_GIFT_4,  0);
    VarSet(VAR_MYSTERY_GIFT_5,  0);
    VarSet(VAR_MYSTERY_GIFT_6,  0);
    VarSet(VAR_MYSTERY_GIFT_7,  0);
    VarSet(VAR_ALTERING_CAVE_WILD_SET, 0);
}

void DisableResetRTC(void)
{
    VarSet(VAR_RESET_RTC_ENABLE, 0);
    FlagClear(FLAG_SYS_RESET_RTC_ENABLE);
}

void EnableResetRTC(void)
{
    VarSet(VAR_RESET_RTC_ENABLE, 0x0920);
    FlagSet(FLAG_SYS_RESET_RTC_ENABLE);
}

bool32 CanResetRTC(void)
{
    if (!FlagGet(FLAG_SYS_RESET_RTC_ENABLE))
        return FALSE;
    if (VarGet(VAR_RESET_RTC_ENABLE) != 0x0920)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: GetVarPointer
 *
 * PURPOSE: Get a writable pointer to a game variable by its index.
 *
 * HOW IT WORKS:
 * Variables are divided into two regions:
 * - Normal vars (VARS_START to SPECIAL_VARS_START-1): Stored in save data
 *   at gSaveBlock1Ptr->vars[idx - VARS_START]. These persist across saves.
 * - Special vars (SPECIAL_VARS_START+): Point to specific EWRAM globals
 *   (gSpecialVar_Result, gSpecialVar_LastTalked, etc.) via gSpecialVars[].
 *   These are NOT saved -- they're transient per-session values.
 *
 * Also handles Quest Log: during recording, var changes are logged.
 * During playback, vars are read from Quest Log data instead of save data.
 *
 * @param idx — Variable index (e.g., VAR_NATIONAL_DEX, TEMP_VARS, 0x8000+)
 * @return Pointer to the variable's storage, or NULL if invalid index
 */
u16 *GetVarPointer(u16 idx)
{
    u16 *ptr;
    if (idx < VARS_START)
        return NULL;
    if (idx < SPECIAL_VARS_START)
    {
        switch (gQuestLogPlaybackState)
        {
        case QL_PLAYBACK_STATE_STOPPED:
        default:
            break;
        case QL_PLAYBACK_STATE_RUNNING:
            ptr = QuestLogGetFlagOrVarPtr(FALSE, idx);
            if (ptr != NULL)
                gSaveBlock1Ptr->vars[idx - VARS_START] = *ptr;
            break;
        case QL_PLAYBACK_STATE_RECORDING:
            if (IsFlagOrVarStoredInQuestLog(idx - VARS_START, TRUE) == TRUE)
            {
                gLastQuestLogStoredFlagOrVarIdx = idx - VARS_START;
                QuestLogSetFlagOrVar(FALSE, idx, gSaveBlock1Ptr->vars[idx - VARS_START]);
            }
            break;
        }
        return &gSaveBlock1Ptr->vars[idx - VARS_START];
    }
    return gSpecialVars[idx - SPECIAL_VARS_START];
}

static bool8 IsFlagOrVarStoredInQuestLog(u16 idx, bool8 isVar)
{
    if (!isVar)
    {
        if (idx < STORY_FLAGS_START)
            return FALSE;
        if (idx >= SYS_FLAGS && idx < PERMA_SYS_FLAGS_START)
            return FALSE;
    }
    else
    {
        if (idx < VAR_ICE_STEP_COUNT - VARS_START)
            return FALSE;
        if (idx >= VAR_MAP_SCENE_PALLET_TOWN_OAK - VARS_START && idx < VAR_PORTHOLE - VARS_START)
            return FALSE;
    }
    return TRUE;
}

u16 VarGet(u16 idx)
{
    u16 *ptr = GetVarPointer(idx);
    if (ptr == NULL)
        return idx;
    return *ptr;
}

bool8 VarSet(u16 idx, u16 val)
{
    u16 *ptr = GetVarPointer(idx);
    if (ptr == NULL)
        return FALSE;
    *ptr = val;
    return TRUE;
}

u8 VarGetObjectEventGraphicsId(u8 idx)
{
    return VarGet(VAR_OBJ_GFX_ID_0 + idx);
}

u8 *GetFlagAddr(u16 idx)
{
    u8 *ptr;
    if (idx == 0)
        return NULL;
    if (idx < SPECIAL_FLAGS_START)
    {
        switch (gQuestLogPlaybackState)
        {
        case QL_PLAYBACK_STATE_STOPPED:
        default:
            break;
        case QL_PLAYBACK_STATE_RUNNING:
            ptr = QuestLogGetFlagOrVarPtr(TRUE, idx);
            if (ptr != NULL)
                gSaveBlock1Ptr->flags[idx / 8] = *ptr;
            break;
        case QL_PLAYBACK_STATE_RECORDING:
            if (IsFlagOrVarStoredInQuestLog(idx, FALSE) == TRUE)
            {
                gLastQuestLogStoredFlagOrVarIdx = idx;
                QuestLogSetFlagOrVar(TRUE, idx, gSaveBlock1Ptr->flags[idx / 8]);
            }
            break;
        }
        return &gSaveBlock1Ptr->flags[idx / 8];
    }
    return &sSpecialFlags[(idx - SPECIAL_FLAGS_START) / 8];
}

/**
 * FUNCTION: FlagSet
 *
 * PURPOSE: Set a flag (turn it ON / TRUE).
 *
 * HOW IT WORKS:
 * Flags are stored as individual bits in a byte array. To set flag N:
 * - Find the byte: ptr = flags[N / 8]
 * - Set the bit: *ptr |= (1 << (N % 8))
 * The expression (idx & 7) is equivalent to (idx % 8) but faster
 * because 7 = 0b111 masks the lowest 3 bits.
 */
bool8 FlagSet(u16 idx)
{
    u8 *ptr = GetFlagAddr(idx);
    if (ptr != NULL)
        *ptr |= 1 << (idx & 7);  /* Set bit (idx % 8) in the byte */
    return FALSE;
}

/**
 * FUNCTION: FlagClear
 *
 * PURPOSE: Clear a flag (turn it OFF / FALSE).
 *
 * HOW IT WORKS:
 * Creates a bitmask with all bits set EXCEPT bit (idx % 8), then ANDs
 * it with the byte. The ~ (bitwise NOT) inverts the single-bit mask.
 * Example: if idx & 7 == 3, mask = ~(1<<3) = ~0b00001000 = 0b11110111
 */
bool8 FlagClear(u16 idx)
{
    u8 *ptr = GetFlagAddr(idx);
    if (ptr != NULL)
        *ptr &= ~(1 << (idx & 7));  /* Clear bit (idx % 8) in the byte */
    return FALSE;
}

/**
 * FUNCTION: FlagGet
 *
 * PURPOSE: Check whether a flag is set (TRUE) or clear (FALSE).
 *
 * HOW IT WORKS:
 * ANDs the byte with a single-bit mask. If the result is non-zero,
 * the flag is set. Otherwise it's clear.
 */
bool8 FlagGet(u16 idx)
{
    u8 *ptr = GetFlagAddr(idx);
    if (ptr == NULL)
        return FALSE;
    if (!(*ptr & 1 << (idx & 7)))  /* Test bit (idx % 8) */
        return FALSE;
    return TRUE;
}

void ResetSpecialVars(void)
{
    gSpecialVar_0x8000 = 0;
    gSpecialVar_0x8001 = 0;
    gSpecialVar_0x8002 = 0;
    gSpecialVar_0x8003 = 0;
    gSpecialVar_0x8004 = 0;
    gSpecialVar_0x8005 = 0;
    gSpecialVar_0x8006 = 0;
    gSpecialVar_0x8007 = 0;
    gSpecialVar_0x8008 = 0;
    gSpecialVar_0x8009 = 0;
    gSpecialVar_0x800A = 0;
    gSpecialVar_0x800B = 0;
    gSpecialVar_Facing = 0;
    gSpecialVar_Result = 0;
    gSpecialVar_ItemId = 0;
    gSpecialVar_LastTalked = 0;
    gSpecialVar_MonBoxId = 0;
    gSpecialVar_MonBoxPos = 0;
    gSpecialVar_TextColor = 0;
    gSpecialVar_PrevTextColor = 0;
    gSpecialVar_0x8014 = 0;
}
