/**
 * @file fldeff_dig.c
 * @brief Dig Field Effect — Escape from Caves and Dungeons
 *
 * FILE OVERVIEW:
 * Implements the field effect for the move Dig when used outside of battle.
 * Dig functions identically to an Escape Rope item — it warps the player
 * out of a cave or dungeon to the last outdoor map entrance they used.
 * Can only be used on maps that allow Escape Rope usage (caves, dungeons).
 *
 * SEQUENCE:
 * 1. SetUpFieldMove_Dig checks if the current map allows Escape Rope
 * 2. FieldCallback_Dig resets the overworld state for a dig/escape warp
 * 3. FldEff_UseDig shows the Pokemon animation and sets the player on foot
 * 4. StartDigFieldEffect creates the dig escape task (same as Escape Rope)
 */
#include "global.h"
#include "field_effect.h"
#include "field_player_avatar.h"
#include "fldeff.h"
#include "item_use.h"
#include "overworld.h"
#include "party_menu.h"

static void FieldCallback_Dig(void);
static void StartDigFieldEffect(void);

/**
 * FUNCTION: SetUpFieldMove_Dig
 *
 * PURPOSE: Validates whether Dig can be used on the current map.
 * Only works on maps that allow Escape Rope (caves, dungeons).
 *
 * @return TRUE if Dig can be used, FALSE otherwise
 */
bool8 SetUpFieldMove_Dig(void)
{
    if (CanUseEscapeRopeOnCurrMap() == TRUE)
    {
        gFieldCallback2 = FieldCallback_PrepareFadeInFromMenu;
        gPostMenuFieldCallback = FieldCallback_Dig;
        return TRUE;
    }
    return FALSE;
}

/**
 * FUNCTION: FieldCallback_Dig
 *
 * PURPOSE: After the party menu closes, resets the overworld state for a
 * Dig/Escape Rope warp and starts the Dig field effect animation.
 */
static void FieldCallback_Dig(void)
{
    Overworld_ResetStateAfterDigEscRope();
    FieldEffectStart(FLDEFF_USE_DIG);
    gFieldEffectArguments[0] = GetCursorSelectionMonId();
}

/**
 * FUNCTION: FldEff_UseDig
 *
 * PURPOSE: Creates the "show Pokemon" animation and sets the player to on-foot.
 *
 * @return FALSE (field effects handle their own cleanup)
 */
bool8 FldEff_UseDig(void)
{
    u8 taskId = CreateFieldEffectShowMon();

    FLDEFF_SET_FUNC_TO_DATA(StartDigFieldEffect);
    SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT);
    return FALSE;
}

/**
 * FUNCTION: StartDigFieldEffect
 *
 * PURPOSE: Removes the show-mon field effect and creates the dig escape task.
 * Uses the same task as the Escape Rope item (Task_UseDigEscapeRopeOnField),
 * since Dig and Escape Rope have identical behavior.
 */
static void StartDigFieldEffect(void)
{
    u8 taskId;

    FieldEffectActiveListRemove(FLDEFF_USE_DIG);
    taskId = CreateTask(Task_UseDigEscapeRopeOnField, 8);
    gTasks[taskId].data[0] = 0;
}
