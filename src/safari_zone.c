/**
 * @file safari_zone.c
 * @brief Safari Zone Game Mode Management
 *
 * FILE OVERVIEW:
 * The Safari Zone is a special area in Fuchsia City where the player pays 500
 * Pokedollars to enter and receives 30 Safari Balls. They can take up to 600
 * steps before being automatically removed. Inside, wild Pokemon are caught
 * using Safari Balls instead of normal battling (no attacking, only throwing
 * Safari Balls, using bait, or throwing rocks).
 *
 * This file manages:
 * - Entering/exiting Safari mode (setting/clearing the mode flag)
 * - Tracking the step counter (600 steps allowed)
 * - Tracking remaining Safari Balls (start with 30)
 * - Handling the end-of-safari transitions (time's up or out of balls)
 *
 * SAFARI ZONE RULES:
 * - 30 Safari Balls to start
 * - 600 steps maximum (each tile the player walks on counts as 1 step)
 * - When steps reach 0: "PA: Ding-dong! Time's up!" and player is ejected
 * - When balls reach 0 mid-battle: battle ends and player is ejected
 * - Catching a Pokemon on the last ball: player gets the Pokemon then is ejected
 */
#include "global.h"
#include "battle.h"
#include "event_scripts.h"
#include "overworld.h"
#include "script.h"
#include "event_data.h"
#include "field_screen_effect.h"

EWRAM_DATA u8 gNumSafariBalls = 0;        /* Remaining Safari Balls (starts at 30) */
EWRAM_DATA u16 gSafariZoneStepCounter = 0; /* Remaining steps allowed (starts at 600) */

/**
 * FUNCTION: GetSafariZoneFlag
 *
 * PURPOSE: Returns TRUE if the player is currently in Safari Zone mode.
 * Checks the system flag FLAG_SYS_SAFARI_MODE.
 */
bool32 GetSafariZoneFlag(void)
{
    return FlagGet(FLAG_SYS_SAFARI_MODE);
}

/** Sets the Safari Zone mode flag. */
void SetSafariZoneFlag(void)
{
    FlagSet(FLAG_SYS_SAFARI_MODE);
}

/** Clears the Safari Zone mode flag. */
void ResetSafariZoneFlag(void)
{
    FlagClear(FLAG_SYS_SAFARI_MODE);
}

/**
 * FUNCTION: EnterSafariMode
 *
 * PURPOSE: Initializes the Safari Zone game mode when the player enters.
 * Increments the "entered Safari Zone" game stat, sets the mode flag,
 * gives the player 30 Safari Balls, and sets the step counter to 600.
 */
void EnterSafariMode(void)
{
    IncrementGameStat(GAME_STAT_ENTERED_SAFARI_ZONE);
    SetSafariZoneFlag();
    gNumSafariBalls = 30;
    gSafariZoneStepCounter = 600;
}

/**
 * FUNCTION: ExitSafariMode
 *
 * PURPOSE: Cleans up Safari Zone mode when the player leaves (either voluntarily
 * through the exit or forced out by running out of time/balls).
 */
void ExitSafariMode(void)
{
    ResetSafariZoneFlag();
    gNumSafariBalls = 0;
    gSafariZoneStepCounter = 0;
}

/**
 * FUNCTION: SafariZoneTakeStep
 *
 * PURPOSE: Decrements the Safari Zone step counter. Called every time the player
 * takes a step on the overworld while in Safari mode.
 *
 * @return TRUE if time is up (step counter reached 0), FALSE otherwise
 */
bool8 SafariZoneTakeStep(void)
{
    if (GetSafariZoneFlag() == FALSE)
        return FALSE;
    gSafariZoneStepCounter--;
    if (gSafariZoneStepCounter == 0)
    {
        /* Time's up! Run the "PA: Ding-dong!" ejection script */
        ScriptContext_SetupScript(SafariZone_EventScript_TimesUp);
        return TRUE;
    }
    return FALSE;
}

/**
 * FUNCTION: SafariZoneRetirePrompt
 *
 * PURPOSE: Shows the "Would you like to retire?" prompt when the player tries
 * to leave through the main gate.
 */
void SafariZoneRetirePrompt(void)
{
    ScriptContext_SetupScript(SafariZone_EventScript_RetirePrompt);
}

/**
 * FUNCTION: CB2_EndSafariBattle
 *
 * PURPOSE: Callback that runs when a Safari Zone battle ends. Determines what
 * should happen next based on remaining Safari Balls and battle outcome.
 *
 * HOW IT WORKS:
 * Three possible outcomes:
 * 1. Still have balls: Return to the Safari Zone field normally
 * 2. Out of balls (mid-battle): Warp player to the Safari Zone exit gate
 *    and show the "ran out of balls" field callback
 * 3. Caught a Pokemon on the last ball: Player gets the Pokemon, then is
 *    ejected with the "out of balls" script (no more catching allowed)
 */
void CB2_EndSafariBattle(void)
{
    if (gNumSafariBalls != 0)
    {
        /* Still have balls — return to the field normally */
        SetMainCallback2(CB2_ReturnToField);
    }
    else if (gBattleOutcome == B_OUTCOME_NO_SAFARI_BALLS)
    {
        /* Ran out mid-battle without catching — warp to exit */
        RunScriptImmediately(SafariZone_EventScript_OutOfBallsMidBattle);
        WarpIntoMap();
        gFieldCallback = FieldCB_SafariZoneRanOutOfBalls;
        SetMainCallback2(CB2_LoadMap);
    }
    else if (gBattleOutcome == B_OUTCOME_CAUGHT)
    {
        /* Caught a Pokemon on the last ball — show success then eject */
        ScriptContext_SetupScript(SafariZone_EventScript_OutOfBalls);
        ScriptContext_Stop();
        SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
    }
}
