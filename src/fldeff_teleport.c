/**
 * @file fldeff_teleport.c
 * @brief Teleport Field Effect — Warp to Last Pokemon Center
 *
 * FILE OVERVIEW:
 * Implements the field effect for the move Teleport. When used outside of battle,
 * Teleport warps the player back to the last Pokemon Center they visited.
 * Can only be used on maps that allow Teleport and Fly (outdoor maps).
 *
 * SEQUENCE:
 * 1. SetUpFieldMove_Teleport checks if the current map allows teleporting
 * 2. After the party menu closes, FieldCallback_Teleport resets the overworld
 *    state and starts the teleport field effect (spinning animation)
 * 3. FldEff_UseTeleport creates the "show Pokemon" animation and ensures the
 *    player is on foot (dismounts bike if necessary)
 * 4. StartTeleportFieldEffect removes the field effect from the active list
 *    and creates the actual teleport warp task (spinning + fade out)
 */
#include "global.h"
#include "field_effect.h"
#include "field_player_avatar.h"
#include "fldeff.h"
#include "party_menu.h"
#include "overworld.h"

static void FieldCallback_Teleport(void);
static void StartTeleportFieldEffect(void);

/**
 * FUNCTION: SetUpFieldMove_Teleport
 *
 * PURPOSE: Validates whether Teleport can be used on the current map.
 * Teleport can only be used on outdoor maps (routes, cities) — not inside
 * buildings, caves, or special areas.
 *
 * @return TRUE if Teleport can be used, FALSE otherwise
 */
bool8 SetUpFieldMove_Teleport(void)
{
    if (Overworld_MapTypeAllowsTeleportAndFly(gMapHeader.mapType) == TRUE)
    {
        gFieldCallback2 = FieldCallback_PrepareFadeInFromMenu;
        gPostMenuFieldCallback = FieldCallback_Teleport;
        return TRUE;
    }
    return FALSE;
}

/**
 * FUNCTION: FieldCallback_Teleport
 *
 * PURPOSE: After the party menu closes, resets the overworld state (prepares
 * for the warp) and starts the teleport visual effect.
 */
static void FieldCallback_Teleport(void)
{
    Overworld_ResetStateAfterTeleport();
    FieldEffectStart(FLDEFF_USE_TELEPORT);
    gFieldEffectArguments[0] = (u32)GetCursorSelectionMonId();
}

/**
 * FUNCTION: FldEff_UseTeleport
 *
 * PURPOSE: Creates the "show Pokemon" animation and sets the player to on-foot
 * mode (in case they were on a bike — can't teleport while biking).
 *
 * @return FALSE (field effects handle their own cleanup)
 */
bool8 FldEff_UseTeleport(void)
{
    u8 taskId = CreateFieldEffectShowMon();
    FLDEFF_SET_FUNC_TO_DATA(StartTeleportFieldEffect);
    SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT);
    return FALSE;
}

/**
 * FUNCTION: StartTeleportFieldEffect
 *
 * PURPOSE: Cleanup callback that removes the show-mon field effect and starts
 * the actual teleport task (player spins and the screen fades out, then warp).
 */
static void StartTeleportFieldEffect(void)
{
    FieldEffectActiveListRemove(FLDEFF_USE_TELEPORT);
    CreateTeleportFieldEffectTask();
}
