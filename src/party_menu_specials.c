/**
 * @file party_menu_specials.c
 * @brief Special Party Menu Operations — Move Deleter, Move Relearner, Script Hooks
 *
 * FILE OVERVIEW:
 * This file provides script-callable functions that interface between the game's
 * event scripting system and the party menu. These are used by NPCs like the
 * Move Deleter (who removes moves from Pokemon) and the Move Relearner (who
 * re-teaches forgotten moves).
 *
 * Key operations:
 *   - Opening the party menu to select a Pokemon (for scripts)
 *   - Deleting/forgetting a specific move from a Pokemon
 *   - Shifting move slots to fill gaps when a move is removed
 *   - Querying how many moves a Pokemon knows
 *   - Checking if a selected party member is an Egg
 *
 * GAME LOGIC:
 * The Move Deleter and Move Relearner are special NPCs in the game world.
 * When the player talks to them, event scripts call these functions to open
 * the party menu, let the player select a Pokemon, then perform the requested
 * operation. Results are stored in gSpecialVar_Result for the script to read.
 */
#include "global.h"
#include "gflib.h"
#include "data.h"
#include "script.h"
#include "overworld.h"
#include "party_menu.h"
#include "field_fadetransition.h"
#include "pokemon_summary_screen.h"
#include "event_data.h"
#include "constants/moves.h"

static void Task_ChoosePartyMon(u8 taskId);

/**
 * FUNCTION: ChoosePartyMon
 *
 * PURPOSE: Opens the party menu so a script can let the player choose a Pokemon.
 *          Used by NPCs like the Name Rater or Move Deleter.
 *
 * HOW IT WORKS:
 * Locks player movement, creates a task to wait for the fade-out animation,
 * then opens the party menu in "choose single mon" mode. The selected Pokemon
 * index is stored in gSpecialVar_0x8004 for the calling script to use.
 */
void ChoosePartyMon(void)
{
    u8 taskId;

    LockPlayerFieldControls();
    taskId = CreateTask(Task_ChoosePartyMon, 10);
    gTasks[taskId].data[0] = PARTY_MENU_TYPE_CHOOSE_SINGLE_MON;
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK); /* Fade to black */
}

/**
 * FUNCTION: ChooseMonForMoveRelearner
 *
 * PURPOSE: Opens the party menu specifically for the Move Relearner NPC, which
 *          shows re-learnable moves for each Pokemon.
 */
void ChooseMonForMoveRelearner(void)
{
    u8 taskId;

    LockPlayerFieldControls();
    taskId = CreateTask(Task_ChoosePartyMon, 10);
    gTasks[taskId].data[0] = PARTY_MENU_TYPE_MOVE_RELEARNER;
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
}

/**
 * FUNCTION: Task_ChoosePartyMon
 *
 * PURPOSE: Waits for the screen fade to complete, then opens the party menu.
 *          Disabling bufferTransferDisabled prevents palette updates during
 *          the transition to the party menu screen.
 */
static void Task_ChoosePartyMon(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        gPaletteFade.bufferTransferDisabled = TRUE;
        ChoosePartyMonByMenuType((u8)gTasks[taskId].data[0]);
        DestroyTask(taskId);
    }
}

/**
 * FUNCTION: SelectMoveDeleterMove
 *
 * PURPOSE: Opens the Pokemon Summary Screen in "forget move" mode so the player
 *          can choose which move to delete.
 *
 * GAME LOGIC:
 * gSpecialVar_0x8004 holds the party slot of the selected Pokemon. The summary
 * screen is configured to show the move list and let the player select one to
 * forget. After returning, the script continues via FieldCB_ContinueScriptHandleMusic.
 */
void SelectMoveDeleterMove(void)
{
    ShowSelectMovePokemonSummaryScreen(gPlayerParty, gSpecialVar_0x8004, gPlayerPartyCount - 1, CB2_ReturnToField, 0);
    SetPokemonSummaryScreenMode(PSS_MODE_FORGET_MOVE);
    gFieldCallback = FieldCB_ContinueScriptHandleMusic;
}

/**
 * FUNCTION: GetNumMovesSelectedMonHas
 *
 * PURPOSE: Counts how many non-empty move slots the selected Pokemon has.
 *          Result is stored in gSpecialVar_Result for the calling script.
 *
 * GAME LOGIC:
 * A Pokemon can have 1-4 moves. Empty slots contain MOVE_NONE. This is checked
 * by the Move Deleter to ensure the Pokemon keeps at least one move.
 */
void GetNumMovesSelectedMonHas(void)
{
    u8 i;

    gSpecialVar_Result = 0;
    for (i = 0; i < MAX_MON_MOVES; ++i)
        if (GetMonData(&gPlayerParty[gSpecialVar_0x8004], MON_DATA_MOVE1 + i) != MOVE_NONE)
            ++gSpecialVar_Result;
}

/**
 * FUNCTION: BufferMoveDeleterNicknameAndMove
 *
 * PURPOSE: Stores the Pokemon's nickname and the selected move's name into
 *          string buffers for use in dialogue text (e.g., "PIKACHU forgot THUNDER!").
 */
void BufferMoveDeleterNicknameAndMove(void)
{
    struct Pokemon *mon = &gPlayerParty[gSpecialVar_0x8004];
    u16 move = GetMonData(mon, MON_DATA_MOVE1 + gSpecialVar_0x8005);

    GetMonNickname(mon, gStringVar1);  /* Pokemon nickname -> {STR_VAR_1} */
    StringCopy(gStringVar2, gMoveNames[move]); /* Move name -> {STR_VAR_2} */
}

/**
 * FUNCTION: ShiftMoveSlot
 *
 * PURPOSE: Swaps the move, PP, and PP bonus data between two move slots.
 *          Used to compact move slots after one is deleted (shift remaining
 *          moves up to fill the gap).
 *
 * HOW IT WORKS:
 * PP bonuses (from PP Ups/PP Max items) are packed into a single byte using
 * 2 bits per move slot. Each move slot's PP bonus (0-3, representing 0-3 PP Ups
 * used) is stored at bits [slot*2, slot*2+1]. This function carefully extracts,
 * clears, and re-inserts the bonus bits when swapping slots.
 *
 * PARAMETERS:
 * @param mon      — The Pokemon whose moves are being rearranged
 * @param slotTo   — Destination move slot index (0-3)
 * @param slotFrom — Source move slot index (0-3)
 */
static void ShiftMoveSlot(struct Pokemon *mon, u8 slotTo, u8 slotFrom)
{
    u16 move1 = GetMonData(mon, MON_DATA_MOVE1 + slotTo);
    u16 move0 = GetMonData(mon, MON_DATA_MOVE1 + slotFrom);
    u8 pp1 = GetMonData(mon, MON_DATA_PP1 + slotTo);
    u8 pp0 = GetMonData(mon, MON_DATA_PP1 + slotFrom);
    u8 ppBonuses = GetMonData(mon, MON_DATA_PP_BONUSES);
    /* Extract the 2-bit PP bonus for each slot using bitmasks */
    u8 ppBonusMask1 = gPPUpGetMask[slotTo];       /* e.g., 0x03 for slot 0, 0x0C for slot 1 */
    u8 ppBonusMove1 = (ppBonuses & ppBonusMask1) >> (slotTo * 2);
    u8 ppBonusMask2 = gPPUpGetMask[slotFrom];
    u8 ppBonusMove2 = (ppBonuses & ppBonusMask2) >> (slotFrom * 2);

    /* Clear both slots' bits, then write them back swapped */
    ppBonuses &= ~ppBonusMask1;
    ppBonuses &= ~ppBonusMask2;
    ppBonuses |= (ppBonusMove1 << (slotFrom * 2)) + (ppBonusMove2 << (slotTo * 2));
    /* Swap the actual move IDs and PP values */
    SetMonData(mon, MON_DATA_MOVE1 + slotTo, &move0);
    SetMonData(mon, MON_DATA_MOVE1 + slotFrom, &move1);
    SetMonData(mon, MON_DATA_PP1 + slotTo, &pp0);
    SetMonData(mon, MON_DATA_PP1 + slotFrom, &pp1);
    SetMonData(mon, MON_DATA_PP_BONUSES, &ppBonuses);
}

/**
 * FUNCTION: MoveDeleterForgetMove
 *
 * PURPOSE: Removes the selected move from the Pokemon and shifts remaining
 *          moves up to fill the gap (no empty slots in the middle).
 *
 * GAME LOGIC:
 * gSpecialVar_0x8004 = party slot, gSpecialVar_0x8005 = move slot to delete.
 * After clearing the move, all subsequent moves shift up by one slot.
 */
void MoveDeleterForgetMove(void)
{
    u16 i;

    SetMonMoveSlot(&gPlayerParty[gSpecialVar_0x8004], MOVE_NONE, gSpecialVar_0x8005);
    RemoveMonPPBonus(&gPlayerParty[gSpecialVar_0x8004], gSpecialVar_0x8005);
    /* Shift all moves after the deleted one up by one slot to fill the gap */
    for (i = gSpecialVar_0x8005; i < MAX_MON_MOVES - 1; ++i)
        ShiftMoveSlot(&gPlayerParty[gSpecialVar_0x8004], i, i + 1);
}

/**
 * FUNCTION: IsSelectedMonEgg
 *
 * PURPOSE: Checks whether the selected party Pokemon is an Egg.
 *          Sets gSpecialVar_Result to TRUE if it is, FALSE otherwise.
 *          Used by scripts to prevent operations on Eggs (e.g., can't
 *          delete moves from an Egg).
 */
void IsSelectedMonEgg(void)
{
    if (GetMonData(&gPlayerParty[gSpecialVar_0x8004], MON_DATA_IS_EGG))
        gSpecialVar_Result = TRUE;
    else
        gSpecialVar_Result = FALSE;
}
