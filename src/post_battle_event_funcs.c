/**
 * @file post_battle_event_funcs.c
 * @brief Post-Battle Callback Functions (Hall of Fame and Whiteout)
 *
 * FILE OVERVIEW:
 * This file contains the two major post-battle callbacks:
 * 1. EnterHallOfFame: Called when the player defeats the Champion — handles game
 *    completion, ribbon awarding, and transitioning to the Hall of Fame screen.
 * 2. SetCB2WhiteOut: Called when the player loses all Pokemon — triggers the
 *    whiteout (black out) sequence.
 *
 * Both functions set the main callback (CB2 = Callback 2, the main game loop function)
 * to transition the game into the appropriate screen.
 *
 * GBA CONTEXT — CALLBACK SYSTEM:
 * The GBA Pokemon engine uses a callback-based architecture. The main game loop calls
 * CB2 (Callback 2) every frame to drive whichever screen/mode is active. Changing
 * CB2 is how the game transitions between screens (battle -> overworld -> credits, etc.).
 * SetMainCallback2 schedules this change for the next frame.
 */
#include "global.h"
#include "script_pokemon_util.h"
#include "event_data.h"
#include "credits.h"
#include "overworld.h"
#include "hall_of_fame.h"
#include "load_save.h"
#include "constants/heal_locations.h"

/**
 * FUNCTION: EnterHallOfFame
 *
 * PURPOSE: Handles everything that needs to happen when the player beats the
 *          Champion for the first (or subsequent) time and enters the Hall of Fame.
 *
 * GAME LOGIC:
 * This is one of the most important functions in the game — it handles the moment
 * of becoming Champion. Here's what happens step by step:
 *
 * 1. Heal the player's entire party to full HP/PP
 * 2. Check if this is the first time or a repeat victory:
 *    - First time: Set FLAG_SYS_GAME_CLEAR (marks game as beaten), gHasHallOfFameRecords = FALSE
 *    - Repeat: gHasHallOfFameRecords = TRUE (tells Hall of Fame screen to show previous records)
 * 3. Record the first completion time in game stats if not already recorded.
 *    The time is packed into a u32: hours in bits 31-16, minutes in bits 15-8, seconds in bits 7-0
 * 4. Set the continue game warp to Pallet Town (where the player will be when they
 *    load the save after watching credits)
 * 5. Award the Champion Ribbon to all party Pokemon that don't already have it:
 *    - Only real Pokemon (not eggs) are eligible
 *    - If any new ribbons were given, increment the ribbon game stat and set the
 *      ribbon notification flag
 * 6. Transition to the Hall of Fame screen (which then leads to credits)
 *
 * @returns FALSE always (return value is used by the script command system)
 */
bool8 EnterHallOfFame(void)
{
    bool8 ribbonState;
    bool8 *r7;
    int i;
    bool8 gaveAtLeastOneRibbon;

    /* Fully heal the player's party — they just beat the Champion! */
    HealPlayerParty();

    /* Check if this is the player's first Championship victory */
    if (FlagGet(FLAG_SYS_GAME_CLEAR) == TRUE)
    {
        /* Repeat victory — previous Hall of Fame records exist */
        gHasHallOfFameRecords = TRUE;
    }
    else
    {
        /* First-time victory — no previous records yet */
        gHasHallOfFameRecords = FALSE;
        FlagSet(FLAG_SYS_GAME_CLEAR);  /* Mark the game as completed */
    }

    /* Record the first completion time if it hasn't been recorded yet.
     * Time is packed as: [hours:16 | minutes:8 | seconds:8] */
    if (GetGameStat(GAME_STAT_FIRST_HOF_PLAY_TIME) == 0)
    {
        SetGameStat(GAME_STAT_FIRST_HOF_PLAY_TIME, (gSaveBlock2Ptr->playTimeHours << 16) | (gSaveBlock2Ptr->playTimeMinutes << 8) | gSaveBlock2Ptr->playTimeSeconds);
    }

    /* Set where the player will appear when they load the save after credits.
     * They'll be back home in Pallet Town — the classic Pokemon ending. */
    SetContinueGameWarpStatus();
    SetContinueGameWarpToHealLocation(HEAL_LOCATION_PALLET_TOWN);

    /* Award the Champion Ribbon to all eligible party Pokemon */
    gaveAtLeastOneRibbon = FALSE;
    for (i = 0, r7 = &ribbonState; i < PARTY_SIZE; i++)
    {
        /* Skip empty slots and eggs — only real Pokemon get ribbons */
        if (GetMonData(&gPlayerParty[i], MON_DATA_SANITY_HAS_SPECIES) && !GetMonData(&gPlayerParty[i], MON_DATA_SANITY_IS_EGG))
        {
            /* Only award if this Pokemon doesn't already have the Champion Ribbon
             * (from a previous Championship victory) */
            if (!GetMonData(&gPlayerParty[i], MON_DATA_CHAMPION_RIBBON))
            {
                *r7 = TRUE;
                SetMonData(&gPlayerParty[i], MON_DATA_CHAMPION_RIBBON, &ribbonState);
                gaveAtLeastOneRibbon = TRUE;
            }
        }
    }

    /* If any Pokemon received a new ribbon, update the ribbon stat and notification */
    if (gaveAtLeastOneRibbon == TRUE)
    {
        IncrementGameStat(GAME_STAT_RECEIVED_RIBBONS);
        FlagSet(FLAG_SYS_RIBBON_GET);  /* Triggers "Check the Pokemon Summary!" notification */
    }

    /* Transition to the Hall of Fame screen — this changes the main game loop
     * callback to the Hall of Fame/credits sequence */
    SetMainCallback2(CB2_DoHallOfFameScreen);
    return FALSE;
}

/**
 * FUNCTION: SetCB2WhiteOut
 *
 * PURPOSE: Triggers the whiteout (blackout) sequence when the player has no
 *          usable Pokemon left.
 *
 * GAME LOGIC:
 * A "whiteout" occurs when all the player's Pokemon faint. The screen fades to
 * white/black, the player loses some money, and they respawn at the last Pokemon
 * Center they visited (handled by heal_location.c). CB2_WhiteOut handles the
 * transition animation and save state updates.
 *
 * RETURNS: FALSE (for script command system compatibility)
 */
bool8 SetCB2WhiteOut(void)
{
    SetMainCallback2(CB2_WhiteOut);
    return FALSE;
}
