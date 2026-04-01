/**
 * @file fldeff_softboiled.c
 * @brief Softboiled / Milk Drink Field Move — HP Transfer Between Party Pokemon
 *
 * FILE OVERVIEW:
 * Implements the Softboiled (and Milk Drink) field move, which allows a Pokemon
 * to transfer 1/5 of its max HP to another party member. This is unique among
 * field moves because it works entirely within the party menu rather than
 * interacting with the overworld map.
 *
 * GAME LOGIC — HOW SOFTBOILED WORKS:
 * 1. Player selects Softboiled on a Pokemon in the party menu
 * 2. SetUpFieldMove_SoftBoiled checks if the user has more than 1/5 of max HP
 *    (needs enough HP to donate without fainting)
 * 3. Player is prompted to choose a recipient Pokemon from the party
 * 4. The recipient must be: alive, not the same Pokemon, not at full HP
 * 5. The user loses max_HP / 5 HP, and the recipient gains max_HP / 5 HP
 * 6. Both HP bars animate to show the change
 *
 * This is one of the most useful out-of-battle moves, as it allows healing
 * party members without using items. Chansey and Blissey learn Softboiled,
 * while Miltank learns Milk Drink (same effect).
 */
#include "global.h"
#include "gflib.h"
#include "party_menu.h"
#include "menu.h"
#include "new_menu_helpers.h"
#include "constants/songs.h"

static void Task_SoftboiledRestoreHealth(u8 taskId);
static void Task_DisplayHPRestoredMessage(u8 taskId);
static void Task_FinishSoftboiled(u8 taskId);
static void CantUseSoftboiledOnMon(u8 taskId);

extern const u8 gText_CantBeUsedOnPkmn[];
extern const u8 gText_PkmnHPRestoredByVar2[];

/**
 * FUNCTION: SetUpFieldMove_SoftBoiled
 *
 * PURPOSE: Validates whether the selected Pokemon has enough HP to use Softboiled.
 * Requires more than 1/5 of max HP remaining (since using the move costs 1/5 max HP).
 *
 * GAME LOGIC:
 * If the user has 100 max HP, they need more than 20 HP to use Softboiled.
 * This prevents the user from fainting themselves through repeated use.
 *
 * @return TRUE if the Pokemon has enough HP, FALSE otherwise
 */
bool8 SetUpFieldMove_SoftBoiled(void)
{
    u16 maxHp = GetMonData(&gPlayerParty[GetCursorSelectionMonId()], MON_DATA_MAX_HP);
    u16 curHp = GetMonData(&gPlayerParty[GetCursorSelectionMonId()], MON_DATA_HP);

    if (curHp > maxHp / 5)
        return TRUE;
    else
        return FALSE;
}

/**
 * FUNCTION: ChooseMonForSoftboiled
 *
 * PURPOSE: Sets up the party menu for the player to choose which Pokemon should
 * receive the HP. Highlights the current user and prompts "Use on which POKeMON?"
 *
 * @param taskId — the party menu task that will handle input
 */
void ChooseMonForSoftboiled(u8 taskId)
{
    gPartyMenu.action = PARTY_ACTION_SOFTBOILED;
    gPartyMenu.slotId2 = gPartyMenu.slotId;  /* Save the user's party slot */
    AnimatePartySlot(GetCursorSelectionMonId(), 1);
    DisplayPartyMenuStdMessage(PARTY_MSG_USE_ON_WHICH_MON);
    gTasks[taskId].func = Task_HandleChooseMonInput;
}

/**
 * FUNCTION: Task_TryUseSoftboiledOnPartyMon
 *
 * PURPOSE: Validates the chosen recipient and begins the HP transfer if valid.
 *
 * HOW IT WORKS:
 * The recipient is invalid if:
 * - They're beyond PARTY_SIZE (player pressed B to cancel)
 * - They have 0 HP (fainted — can't heal a fainted Pokemon with Softboiled)
 * - They're the same Pokemon as the user (can't use on yourself)
 * - They're already at full HP (no need to heal)
 *
 * If valid, plays a healing sound and starts the HP drain animation on the user
 * (losing maxHP/5 health), followed by the HP restore animation on the recipient.
 *
 * @param taskId — party menu task identifier
 */
void Task_TryUseSoftboiledOnPartyMon(u8 taskId)
{
    u8 userPartyId = gPartyMenu.slotId;
    u8 recipientPartyId = gPartyMenu.slotId2;
    u16 curHp;

    if (recipientPartyId > PARTY_SIZE)
    {
        /* Player cancelled — return to normal party menu */
        gPartyMenu.action = PARTY_ACTION_CHOOSE_MON;
        DisplayPartyMenuStdMessage(PARTY_MSG_CHOOSE_MON);
        gTasks[taskId].func = Task_HandleChooseMonInput;
    }
    else
    {
        curHp = GetMonData(&gPlayerParty[recipientPartyId], MON_DATA_HP);
        if (curHp == 0                                          /* Fainted */
            || userPartyId == recipientPartyId                  /* Same Pokemon */
            || GetMonData(&gPlayerParty[recipientPartyId], MON_DATA_MAX_HP) == curHp)  /* Full HP */
            CantUseSoftboiledOnMon(taskId);
        else
        {
            /* Valid target — drain HP from user, then restore to recipient */
            PlaySE(SE_USE_ITEM);
            /* -1 direction = subtract HP. Amount = user's maxHP / 5 */
            PartyMenuModifyHP(taskId, userPartyId, -1, GetMonData(&gPlayerParty[userPartyId], MON_DATA_MAX_HP) / 5, Task_SoftboiledRestoreHealth);
        }
    }
}

/**
 * FUNCTION: Task_SoftboiledRestoreHealth
 *
 * PURPOSE: After the user's HP has been drained, adds the same amount of HP
 * to the recipient Pokemon with an animated HP bar increase.
 *
 * @param taskId — party menu task identifier
 */
static void Task_SoftboiledRestoreHealth(u8 taskId)
{
    PlaySE(SE_USE_ITEM);
    /* +1 direction = add HP. Amount = user's maxHP / 5 (same amount that was drained) */
    PartyMenuModifyHP(taskId, gPartyMenu.slotId2, 1, GetMonData(&gPlayerParty[gPartyMenu.slotId], MON_DATA_MAX_HP) / 5, Task_DisplayHPRestoredMessage);
}

/**
 * FUNCTION: Task_DisplayHPRestoredMessage
 *
 * PURPOSE: Shows the "[Pokemon]'s HP was restored" message after the HP transfer
 * completes successfully.
 *
 * @param taskId — party menu task identifier
 */
static void Task_DisplayHPRestoredMessage(u8 taskId)
{
    GetMonNickname(&gPlayerParty[gPartyMenu.slotId2], gStringVar1);
    StringExpandPlaceholders(gStringVar4, gText_PkmnHPRestoredByVar2);
    DisplayPartyMenuMessage(gStringVar4, FALSE);
    ScheduleBgCopyTilemapToVram(2);
    gTasks[taskId].func = Task_FinishSoftboiled;
}

/**
 * FUNCTION: Task_FinishSoftboiled
 *
 * PURPOSE: Cleanup after Softboiled completes. Waits for the message to finish
 * printing, then restores the party menu to its normal "Choose a POKeMON" state.
 *
 * HOW IT WORKS:
 * Deselects the user's party slot animation, selects the recipient's slot,
 * clears the message window, and returns to normal party menu input handling.
 *
 * @param taskId — party menu task identifier
 */
static void Task_FinishSoftboiled(u8 taskId)
{
    if (IsPartyMenuTextPrinterActive() != TRUE)
    {
        gPartyMenu.action = PARTY_ACTION_CHOOSE_MON;
        AnimatePartySlot(gPartyMenu.slotId, 0);    /* Deselect user */
        gPartyMenu.slotId = gPartyMenu.slotId2;
        AnimatePartySlot(gPartyMenu.slotId2, 1);    /* Select recipient */
        ClearStdWindowAndFrameToTransparent(6, 0);
        ClearWindowTilemap(6);
        DisplayPartyMenuStdMessage(PARTY_MSG_CHOOSE_MON);
        gTasks[taskId].func = Task_HandleChooseMonInput;
    }
}

/**
 * FUNCTION: Task_ChooseNewMonForSoftboiled
 *
 * PURPOSE: After showing the "can't be used" error, returns to the recipient
 * selection prompt so the player can choose a different Pokemon.
 */
static void Task_ChooseNewMonForSoftboiled(u8 taskId)
{
    if (IsPartyMenuTextPrinterActive() != TRUE)
    {
        DisplayPartyMenuStdMessage(PARTY_MSG_USE_ON_WHICH_MON);
        gTasks[taskId].func = Task_HandleChooseMonInput;
    }
}

/**
 * FUNCTION: CantUseSoftboiledOnMon
 *
 * PURPOSE: Displays the "Can't be used on this POKeMON" error message when
 * the player selects an invalid recipient (fainted, self, or full HP).
 *
 * @param taskId — party menu task identifier
 */
static void CantUseSoftboiledOnMon(u8 taskId)
{
    PlaySE(SE_SELECT);
    DisplayPartyMenuMessage(gText_CantBeUsedOnPkmn, FALSE);
    ScheduleBgCopyTilemapToVram(2);
    gTasks[taskId].func = Task_ChooseNewMonForSoftboiled;
}
