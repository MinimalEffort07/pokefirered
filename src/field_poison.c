/**
 * @file field_poison.c
 * @brief Overworld Poison Damage System
 *
 * FILE OVERVIEW:
 * This file implements the poison effect on the overworld (outside of battle).
 * In Pokemon games, poisoned Pokemon lose HP as the player walks. Every few steps,
 * each poisoned Pokemon loses 1 HP. If a Pokemon's HP reaches 0 from field poison,
 * it faints and a message is displayed. If ALL Pokemon faint from poison, the player
 * whites out.
 *
 * GAME LOGIC:
 * The poison damage system works in two phases:
 * 1. DoPoisonFieldEffect: Called every N steps (typically 4), reduces HP of all
 *    poisoned Pokemon by 1 and triggers the screen flash effect
 * 2. TryFieldPoisonWhiteOut: After HP reduction, checks if any Pokemon fainted.
 *    Displays faint messages one at a time, then checks for whiteout condition.
 *
 * The friendship penalty for fainting from poison is different from fainting in
 * battle — FRIENDSHIP_EVENT_FAINT_OUTSIDE_BATTLE typically has a smaller penalty.
 */
#include "global.h"
#include "gflib.h"
#include "strings.h"
#include "task.h"
#include "field_message_box.h"
#include "script.h"
#include "event_data.h"
#include "fldeff.h"
#include "party_menu.h"
#include "field_poison.h"
#include "constants/battle.h"

/**
 * FUNCTION: IsMonValidSpecies
 *
 * PURPOSE: Checks whether a Pokemon slot contains a real Pokemon (not empty, not an egg).
 *
 * GAME LOGIC:
 * The player's party has 6 slots, but not all may be filled. Empty slots have
 * SPECIES_NONE, and eggs have SPECIES_EGG. Both should be skipped when checking
 * for poison effects since only hatched Pokemon can be poisoned.
 *
 * @param pokemon — Pointer to the Pokemon struct to check
 * RETURNS: TRUE if this is a real, hatched Pokemon
 */
static bool32 IsMonValidSpecies(struct Pokemon *pokemon)
{
    u16 species = GetMonData(pokemon, MON_DATA_SPECIES_OR_EGG);
    if (species == SPECIES_NONE || species == SPECIES_EGG)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: AllMonsFainted
 *
 * PURPOSE: Checks if every Pokemon in the player's party has fainted (0 HP).
 *          This determines whether the player should white out.
 *
 * GAME LOGIC:
 * Iterates through all 6 party slots. If ANY valid Pokemon has HP > 0, the
 * player is still in the game. Only returns TRUE if every valid Pokemon is
 * at 0 HP.
 *
 * RETURNS: TRUE if all Pokemon have fainted, FALSE if at least one is alive
 */
static bool32 AllMonsFainted(void)
{
    int i;

    struct Pokemon *pokemon = gPlayerParty;
    for (i = 0; i < PARTY_SIZE; i++, pokemon++)
        if (IsMonValidSpecies(pokemon) && GetMonData(pokemon, MON_DATA_HP))
            return FALSE;  /* Found a living Pokemon — not a whiteout */
    return TRUE;  /* No living Pokemon remain */
}

/**
 * FUNCTION: FaintFromFieldPoison
 *
 * PURPOSE: Handles the side effects of a Pokemon fainting from overworld poison:
 *          reduces friendship, clears the poison status, and prepares the faint message.
 *
 * GAME LOGIC:
 * When a Pokemon faints from poison on the field:
 * 1. Its friendship (happiness) decreases — Pokemon don't like fainting
 * 2. Its status condition is cleared to STATUS1_NONE (it's fainted, not poisoned)
 * 3. Its nickname is copied to gStringVar1 for the "{Pokemon} fainted!" message
 *
 * @param partyIdx — Index (0-5) of the Pokemon in the party that fainted
 */
static void FaintFromFieldPoison(u8 partyIdx)
{
    struct Pokemon *pokemon = gPlayerParty + partyIdx;
    u32 status = STATUS1_NONE;  /* Clear all status conditions */
    AdjustFriendship(pokemon, FRIENDSHIP_EVENT_FAINT_OUTSIDE_BATTLE);
    SetMonData(pokemon, MON_DATA_STATUS, &status);  /* Remove poison status */
    GetMonData(pokemon, MON_DATA_NICKNAME, gStringVar1);  /* Get name for message */
    StringGet_Nickname(gStringVar1);  /* Process any special characters in the nickname */
}

/**
 * FUNCTION: MonFaintedFromPoison
 *
 * PURPOSE: Checks if a specific party Pokemon has 0 HP while still having the
 *          poison status — meaning it just fainted from field poison this step.
 *
 * GAME LOGIC:
 * A Pokemon only "fainted from poison" if all three conditions are true:
 * 1. It's a valid species (not empty/egg)
 * 2. Its HP is 0 (it just fainted)
 * 3. It still has the poison status (hasn't been cleared yet by FaintFromFieldPoison)
 * This distinguishes "newly fainted from poison" from "already fainted earlier."
 *
 * @param partyIdx — Party slot index to check (0-5)
 * RETURNS: TRUE if this Pokemon just fainted from poison
 */
static bool32 MonFaintedFromPoison(u8 partyIdx)
{
    struct Pokemon *pokemon = gPlayerParty + partyIdx;
    if (IsMonValidSpecies(pokemon) && !GetMonData(pokemon, MON_DATA_HP) && GetAilmentFromStatus(GetMonData(pokemon, MON_DATA_STATUS)) == AILMENT_PSN)
        return TRUE;
    return FALSE;
}

/*
 * Task data field aliases for Task_TryFieldPoisonWhiteOut.
 * Using #define aliases makes the task's state machine more readable
 * than raw data[0], data[1] references.
 */
#define tState   data[0]   /* Current state in the state machine */
#define tPartyId data[1]   /* Which party slot we're currently checking */

/**
 * FUNCTION: Task_TryFieldPoisonWhiteOut
 *
 * PURPOSE: State machine task that displays faint messages for each poisoned
 *          Pokemon one at a time, then checks for whiteout.
 *
 * GAME LOGIC — STATE MACHINE:
 * State 0: Loop through party slots looking for Pokemon that fainted from poison.
 *          When one is found, display its "{Pokemon} fainted!" message and advance
 *          to state 1. If no more fainted Pokemon, go to state 2.
 * State 1: Wait for the faint message to finish displaying (message box becomes
 *          hidden), then go back to state 0 to check for more fainted Pokemon.
 * State 2: All messages shown. Check if ALL Pokemon fainted (whiteout condition).
 *          Sets gSpecialVar_Result to TRUE for whiteout, FALSE otherwise.
 *          Re-enables the script context and self-destructs.
 *
 * @param taskId — This task's ID in the task system
 */
static void Task_TryFieldPoisonWhiteOut(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    switch (tState)
    {
    case 0:
        /* Search for the next Pokemon that fainted from poison */
        for (; tPartyId < PARTY_SIZE; tPartyId++)
        {
            if (MonFaintedFromPoison(tPartyId))
            {
                FaintFromFieldPoison(tPartyId);        /* Handle faint side effects */
                ShowFieldMessage(gText_PkmnFainted3);  /* Show "{Pokemon} fainted!" */
                tState++;                               /* Move to "wait for message" state */
                return;
            }
        }
        tState = 2;  /* No more fainted Pokemon — proceed to whiteout check */
        break;
    case 1:
        /* Wait for the message box to be dismissed before showing the next one */
        if (IsFieldMessageBoxHidden())
            tState--;  /* Go back to state 0 to check for more fainted Pokemon */
        break;
    case 2:
        /* All faint messages shown — determine if it's a whiteout */
        if (AllMonsFainted())
            gSpecialVar_Result = TRUE;   /* Whiteout — all Pokemon fainted */
        else
            gSpecialVar_Result = FALSE;  /* Some Pokemon still alive */
        ScriptContext_Enable();  /* Allow the calling script to continue */
        DestroyTask(taskId);     /* Clean up this task */
        break;
    }
}

/**
 * FUNCTION: TryFieldPoisonWhiteOut
 *
 * PURPOSE: Entry point for the poison whiteout check. Creates the state machine
 *          task and pauses script execution until it completes.
 *
 * GAME LOGIC:
 * Called by the overworld step handler after DoPoisonFieldEffect reports that
 * a Pokemon fainted (returned FLDPSN_FNT). The script is paused while the
 * faint messages are displayed, then resumed with the result indicating
 * whether a whiteout occurred.
 */
void TryFieldPoisonWhiteOut(void)
{
    CreateTask(Task_TryFieldPoisonWhiteOut, 80);
    ScriptContext_Stop();  /* Pause the script engine until the task finishes */
}

/**
 * FUNCTION: DoPoisonFieldEffect
 *
 * PURPOSE: Applies 1 HP of poison damage to each poisoned Pokemon in the party.
 *          Called every N steps while walking on the overworld.
 *
 * GAME LOGIC:
 * For each party Pokemon:
 * 1. Check if it's a valid species AND has poison status
 * 2. If so, reduce its HP by 1 (but don't go below 0)
 * 3. Track how many Pokemon are poisoned and how many just fainted
 *
 * After processing all Pokemon:
 * - If any Pokemon is poisoned or fainted: trigger the screen flash effect
 *   (FldEffPoison_Start makes the screen briefly flash purple)
 * - Return a code indicating the worst outcome:
 *   - FLDPSN_FNT: At least one Pokemon fainted (need to show messages)
 *   - FLDPSN_PSN: Pokemon are poisoned but none fainted this step
 *   - FLDPSN_NONE: No poisoned Pokemon at all
 *
 * RETURNS: FLDPSN_FNT if a Pokemon fainted, FLDPSN_PSN if poisoned but alive,
 *          FLDPSN_NONE if no poison in party
 */
s32 DoPoisonFieldEffect(void)
{
    int i;
    u32 hp;

    struct Pokemon *pokemon = gPlayerParty;
    u32 numPoisoned = 0;
    u32 numFainted = 0;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        /* Check if this Pokemon exists AND has poison status */
        if (GetMonData(pokemon, MON_DATA_SANITY_HAS_SPECIES) && GetAilmentFromStatus(GetMonData(pokemon, MON_DATA_STATUS)) == AILMENT_PSN)
        {
            hp = GetMonData(pokemon, MON_DATA_HP);
            /* Reduce HP by 1; if already 0 or becomes 0, count as fainted */
            if (hp == 0 || --hp == 0)
                numFainted++;
            SetMonData(pokemon, MON_DATA_HP, &hp);
            numPoisoned++;
        }
        pokemon++;
    }
    /* Trigger the purple screen flash if any Pokemon are affected */
    if (numFainted || numPoisoned)
        FldEffPoison_Start();
    /* Return the most severe outcome */
    if (numFainted)
        return FLDPSN_FNT;   /* At least one Pokemon fainted — show messages */
    if (numPoisoned)
        return FLDPSN_PSN;   /* Poisoned but no faints this step */
    return FLDPSN_NONE;      /* No poisoned Pokemon in party */
}
