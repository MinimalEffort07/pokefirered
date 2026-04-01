/**
 * @file fldeff_strength.c
 * @brief HM Strength Field Effect — Push Boulders on the Overworld
 *
 * FILE OVERVIEW:
 * This file implements the field effect for using HM05 Strength outside of battle.
 * When the player selects Strength from the party menu while facing a pushable
 * boulder, this system validates the conditions and triggers the Strength script.
 *
 * GAME LOGIC — HOW STRENGTH WORKS IN THE FIELD:
 * 1. Player opens party menu and selects "Strength" on a Pokemon that knows it
 * 2. SetUpFieldMove_Strength checks: is the player NOT surfing? Is there a
 *    pushable boulder directly in front of them?
 * 3. If conditions are met, the party menu closes and fades back to the overworld
 * 4. FieldCB_UseStrength runs, which triggers EventScript_FldEffStrength
 * 5. The script plays the "Pokemon used Strength" animation and enables boulder pushing
 *
 * FIELD EFFECT PATTERN:
 * All HM field effects follow the same pattern:
 * - SetUpFieldMove_X: Validates conditions, sets up callbacks, returns TRUE/FALSE
 * - FieldCB_X: Post-menu callback that starts the field effect
 * - FldEff_X: Creates the "show Pokemon using move" visual effect
 * - ShowMonCB_X: Cleanup callback after the Pokemon animation finishes
 */
#include "global.h"
#include "field_player_avatar.h"
#include "field_effect.h"
#include "party_menu.h"
#include "event_data.h"
#include "script.h"
#include "fldeff.h"
#include "event_scripts.h"
#include "constants/event_objects.h"

static void FieldCB_UseStrength(void);
static void ShowMonCB_UseStrength(void);

/**
 * FUNCTION: SetUpFieldMove_Strength
 *
 * PURPOSE: Validates whether Strength can be used and sets up the field effect.
 *
 * HOW IT WORKS:
 * Fails if the player is surfing (can't push boulders from water) or if there's
 * no pushable boulder in front of the player. On success, stores the selected
 * Pokemon ID and sets up callbacks to fade from the menu and trigger the effect.
 *
 * @return TRUE if Strength can be used, FALSE otherwise
 */
bool8 SetUpFieldMove_Strength(void)
{
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING) || CheckObjectGraphicsInFrontOfPlayer(OBJ_EVENT_GFX_PUSHABLE_BOULDER) != TRUE)
    {
        return FALSE;
    }
    else
    {
        gSpecialVar_Result = GetCursorSelectionMonId();
        gFieldCallback2 = FieldCallback_PrepareFadeInFromMenu;
        gPostMenuFieldCallback = FieldCB_UseStrength;
        return TRUE;
    }
}

/**
 * FUNCTION: FieldCB_UseStrength
 *
 * PURPOSE: Callback that runs after the party menu closes. Passes the selected
 * Pokemon's party index to the Strength event script.
 */
static void FieldCB_UseStrength(void)
{
    gFieldEffectArguments[0] = GetCursorSelectionMonId();
    ScriptContext_SetupScript(EventScript_FldEffStrength);
}

/**
 * FUNCTION: FldEff_UseStrength
 *
 * PURPOSE: Creates the visual effect of a Pokemon being summoned to use Strength.
 * Shows the "Pokemon used Strength!" animation with the Pokemon's nickname.
 *
 * @return FALSE (field effects return FALSE to signal they handle cleanup themselves)
 */
bool8 FldEff_UseStrength(void)
{
    u8 taskId = CreateFieldEffectShowMon();
    FLDEFF_SET_FUNC_TO_DATA(ShowMonCB_UseStrength);
    GetMonNickname(&gPlayerParty[gFieldEffectArguments[0]], gStringVar1);
    return FALSE;
}

/**
 * FUNCTION: ShowMonCB_UseStrength
 *
 * PURPOSE: Cleanup callback after the Pokemon animation finishes.
 * Removes the field effect and re-enables the script engine.
 */
static void ShowMonCB_UseStrength(void)
{
    FieldEffectActiveListRemove(FLDEFF_USE_STRENGTH);
    ScriptContext_Enable();
}
