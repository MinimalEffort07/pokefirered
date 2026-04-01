/**
 * @file fldeff_rocksmash.c
 * @brief Rock Smash Field Effect and Field Move Show-Mon Animation System
 *
 * FILE OVERVIEW:
 * This file serves two purposes:
 *
 * 1. ROCK SMASH FIELD EFFECT: Implements the HM06 Rock Smash field usage.
 *    When the player uses Rock Smash from the party menu while facing a
 *    smashable rock, the Pokemon animation plays and the rock is destroyed.
 *
 * 2. SHARED FIELD MOVE ANIMATION SYSTEM: Contains the common "show Pokemon
 *    using a field move" animation task used by ALL HM field effects (Strength,
 *    Rock Smash, Cut, etc.). This is the animation where the player does a
 *    summoning pose and a small Pokemon portrait slides in before the move executes.
 *
 * SHOW-MON ANIMATION SEQUENCE:
 * 1. Init: Lock controls, prevent player stepping, wait for current movement to finish
 * 2. WaitPlayerAnim: Player does a summoning animation (arm thrust)
 * 3. WaitFldeff: The "show mon" field effect plays (Pokemon portrait slides in)
 * 4. Cleanup: Restore player graphics to normal facing direction, call the
 *    move-specific callback (e.g., StartRockSmashFieldEffect)
 *
 * GAME LOGIC — gPlayerFacingPosition:
 * This global struct stores the map position (x, y, elevation) of the tile
 * directly in front of the player. It's used by several field effects to know
 * where to apply the effect (where the boulder is, where the rock is, etc.).
 */
#include "global.h"
#include "gflib.h"
#include "field_player_avatar.h"
#include "field_effect.h"
#include "party_menu.h"
#include "event_data.h"
#include "script.h"
#include "fldeff.h"
#include "event_scripts.h"
#include "overworld.h"
#include "event_object_movement.h"
#include "constants/songs.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/maps.h"

static void Task_FieldEffectShowMon_Init(u8 taskId);
static void Task_FieldEffectShowMon_WaitFldeff(u8 taskId);
static void Task_FieldEffectShowMon_WaitPlayerAnim(u8 taskId);
static void Task_FieldEffectShowMon_Cleanup(u8 taskId);
static void FieldCallback_UseRockSmash(void);
static void StartRockSmashFieldEffect(void);

/* The position of the tile directly in front of the player.
 * Set when checking for HM-targetable objects (boulders, rocks, etc.)
 * and used by field effect scripts to know where to apply the effect. */
EWRAM_DATA struct MapPosition gPlayerFacingPosition = {};

/**
 * FUNCTION: CheckObjectGraphicsInFrontOfPlayer
 *
 * PURPOSE: Checks if there is an object event with a specific graphics ID
 * on the tile directly in front of the player.
 *
 * HOW IT WORKS:
 * 1. Calculates the coordinates one step in front of the player
 * 2. Gets the player's elevation (for multi-level maps)
 * 3. Searches for an object event at those coordinates
 * 4. If found and it has the expected graphics (e.g., boulder, rock), returns TRUE
 *    and stores its local ID in gSpecialVar_LastTalked for script access
 *
 * @param graphicsId — the OBJ_EVENT_GFX_* constant to look for
 * @return TRUE if an object with that graphic is in front of the player
 */
bool8 CheckObjectGraphicsInFrontOfPlayer(u8 graphicsId)
{
    u8 mapObjId;

    GetXYCoordsOneStepInFrontOfPlayer(&gPlayerFacingPosition.x, &gPlayerFacingPosition.y);
    gPlayerFacingPosition.elevation = PlayerGetElevation();
    mapObjId = GetObjectEventIdByPosition(gPlayerFacingPosition.x, gPlayerFacingPosition.y, gPlayerFacingPosition.elevation);
    if (gObjectEvents[mapObjId].graphicsId != graphicsId)
        return FALSE;
    /* Store the object's local ID so scripts can reference it */
    gSpecialVar_LastTalked = gObjectEvents[mapObjId].localId;
    return TRUE;
}

/**
 * FUNCTION: CreateFieldEffectShowMon
 *
 * PURPOSE: Creates the shared "show Pokemon using field move" animation task.
 * Used by all HM field effects to display the Pokemon summoning animation.
 *
 * HOW IT WORKS:
 * Records the position in front of the player (for the field effect to target)
 * and creates the animation task that will play the summoning sequence.
 *
 * @return The task ID of the created animation task
 */
u8 CreateFieldEffectShowMon(void)
{
    GetXYCoordsOneStepInFrontOfPlayer(&gPlayerFacingPosition.x, &gPlayerFacingPosition.y);
    return CreateTask(Task_FieldEffectShowMon_Init, 8);
}

/**
 * FUNCTION: Task_FieldEffectShowMon_Init
 *
 * PURPOSE: First stage of the show-mon animation. Locks player controls and
 * waits for any current movement to finish before starting the summoning pose.
 *
 * HOW IT WORKS:
 * On non-underwater maps (which is always in FireRed), starts the player's
 * "summon Pokemon for field move" animation (arm thrust pose) and waits for
 * the player object to start the animation. On underwater maps (RS leftover),
 * skips the player animation and goes straight to the Pokemon show effect.
 *
 * @param taskId — task identifier
 */
static void Task_FieldEffectShowMon_Init(u8 taskId)
{
    u8 mapObjId;

    LockPlayerFieldControls();
    gPlayerAvatar.preventStep = TRUE;  /* Block player from walking during animation */
    mapObjId = gPlayerAvatar.objectEventId;
    if (!ObjectEventIsMovementOverridden(&gObjectEvents[mapObjId])
     || ObjectEventClearHeldMovementIfFinished(&gObjectEvents[mapObjId]))
    {
        if (gMapHeader.mapType == MAP_TYPE_UNDERWATER)
        {
            /* RS leftover: skip player animation underwater */
            FieldEffectStart(FLDEFF_FIELD_MOVE_SHOW_MON_INIT);
            gTasks[taskId].func = Task_FieldEffectShowMon_WaitFldeff;
        }
        else
        {
            /* Start the player's summoning animation (arm thrust pose) */
            StartPlayerAvatarSummonMonForFieldMoveAnim();
            ObjectEventSetHeldMovement(&gObjectEvents[mapObjId], MOVEMENT_ACTION_START_ANIM_IN_DIRECTION);
            gTasks[taskId].func = Task_FieldEffectShowMon_WaitPlayerAnim;
        }
    }
}

/**
 * FUNCTION: Task_FieldEffectShowMon_WaitPlayerAnim
 *
 * PURPOSE: Waits for the player's summoning pose animation to finish, then
 * starts the Pokemon portrait slide-in effect.
 */
static void Task_FieldEffectShowMon_WaitPlayerAnim(u8 taskId)
{
    if (ObjectEventCheckHeldMovementStatus(&gObjectEvents[gPlayerAvatar.objectEventId]) == TRUE)
    {
        FieldEffectStart(FLDEFF_FIELD_MOVE_SHOW_MON_INIT);
        gTasks[taskId].func = Task_FieldEffectShowMon_WaitFldeff;
    }
}

/**
 * FUNCTION: Task_FieldEffectShowMon_WaitFldeff
 *
 * PURPOSE: Waits for the Pokemon portrait field effect to finish, then restores
 * the player's normal sprite and facing direction.
 *
 * HOW IT WORKS:
 * After the "show mon" effect is no longer active, restores the player sprite
 * to its standard graphics (undoing the summoning pose) and sets the sprite
 * animation frame to match the player's current facing direction.
 *
 * Direction to animation frame mapping: South=0, North=1, West=2, East=3
 */
static void Task_FieldEffectShowMon_WaitFldeff(u8 taskId)
{
    if (!FieldEffectActiveListContains(FLDEFF_FIELD_MOVE_SHOW_MON))
    {
        /* Convert facing direction to sprite animation frame index */
        gFieldEffectArguments[1] = GetPlayerFacingDirection();
        if (gFieldEffectArguments[1] == DIR_SOUTH)
            gFieldEffectArguments[2] = 0;
        if (gFieldEffectArguments[1] == DIR_NORTH)
            gFieldEffectArguments[2] = 1;
        if (gFieldEffectArguments[1] == DIR_WEST)
            gFieldEffectArguments[2] = 2;
        if (gFieldEffectArguments[1] == DIR_EAST)
            gFieldEffectArguments[2] = 3;

        /* Restore player's normal sprite graphics */
        ObjectEventSetGraphicsId(&gObjectEvents[gPlayerAvatar.objectEventId], GetPlayerAvatarGraphicsIdByCurrentState());
        StartSpriteAnim(&gSprites[gPlayerAvatar.spriteId], gFieldEffectArguments[2]);
        FieldEffectActiveListRemove(FLDEFF_FIELD_MOVE_SHOW_MON);
        gTasks[taskId].func = Task_FieldEffectShowMon_Cleanup;
    }
}

/**
 * FUNCTION: Task_FieldEffectShowMon_Cleanup
 *
 * PURPOSE: Final cleanup — calls the move-specific callback function (stored
 * in task data by FLDEFF_SET_FUNC_TO_DATA), re-enables player stepping,
 * and destroys the task.
 *
 * The FLDEFF_CALL_FUNC_IN_DATA() macro retrieves and calls the function
 * pointer that was stored by the individual field effect's setup code.
 */
static void Task_FieldEffectShowMon_Cleanup(u8 taskId)
{
    FLDEFF_CALL_FUNC_IN_DATA();
    gPlayerAvatar.preventStep = FALSE;
    DestroyTask(taskId);
}

/* ========================================================================
 * ROCK SMASH FIELD EFFECT
 * ======================================================================== */

/**
 * FUNCTION: SetUpFieldMove_RockSmash
 *
 * PURPOSE: Validates whether Rock Smash can be used by checking if there's a
 * smashable rock directly in front of the player.
 *
 * @return TRUE if a smashable rock is in front of the player, FALSE otherwise
 */
bool8 SetUpFieldMove_RockSmash(void)
{
    if (CheckObjectGraphicsInFrontOfPlayer(OBJ_EVENT_GFX_ROCK_SMASH_ROCK) == TRUE)
    {
        gFieldCallback2 = FieldCallback_PrepareFadeInFromMenu;
        gPostMenuFieldCallback = FieldCallback_UseRockSmash;
        return TRUE;
    }
    return FALSE;
}

/**
 * FUNCTION: FieldCallback_UseRockSmash
 *
 * PURPOSE: Post-menu callback that starts the Rock Smash event script.
 * The script handles the "Pokemon used Rock Smash!" message and rock destruction.
 */
static void FieldCallback_UseRockSmash(void)
{
    gFieldEffectArguments[0] = GetCursorSelectionMonId();
    ScriptContext_SetupScript(EventScript_FldEffRockSmash);
}

/**
 * FUNCTION: FldEff_UseRockSmash
 *
 * PURPOSE: Creates the show-mon animation for Rock Smash and increments the
 * "used Rock Smash" game statistic (tracked on the Trainer Card).
 *
 * @return FALSE (field effects handle their own cleanup)
 */
bool8 FldEff_UseRockSmash(void)
{
    u8 taskId = CreateFieldEffectShowMon();

    FLDEFF_SET_FUNC_TO_DATA(StartRockSmashFieldEffect);
    IncrementGameStat(GAME_STAT_USED_ROCK_SMASH);
    return FALSE;
}

/**
 * FUNCTION: StartRockSmashFieldEffect
 *
 * PURPOSE: Plays the rock smash sound effect, removes the field effect from
 * the active list, and re-enables the script engine to continue the
 * rock destruction script.
 */
static void StartRockSmashFieldEffect(void)
{
    PlaySE(SE_M_ROCK_THROW);
    FieldEffectActiveListRemove(FLDEFF_USE_ROCK_SMASH);
    ScriptContext_Enable();
}
