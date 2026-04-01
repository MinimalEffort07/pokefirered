/**
 * @file event_object_lock.c
 * @brief NPC and Player Movement Freezing/Locking System
 *
 * FILE OVERVIEW:
 * This file handles "freezing" (locking) the player and NPCs in place during scripted
 * events. When the player talks to an NPC, both characters need to stop moving before
 * the dialogue script can start — the player must finish their current step, and the
 * NPC must stop their patrol route. This file manages that synchronization.
 *
 * GAME LOGIC — WHY FREEZING IS NEEDED:
 * On the overworld, NPCs walk around on patrol routes, and the player can be mid-step
 * when they press A to talk. If a script started immediately, the characters would
 * slide around during dialogue, which looks buggy. The freeze system:
 * 1. Freezes all NPC movement (stops their patrol routines)
 * 2. Waits for the player to finish their current tile transition
 * 3. Waits for the target NPC to finish their current step
 * 4. Only then allows the script to proceed
 *
 * This uses the Task system — small state-machine functions that run each frame
 * until their conditions are met, then self-destruct.
 *
 * TERMINOLOGY:
 * - "Freeze": Stop an object from starting new movements
 * - "Held movement": A scripted movement assigned to an object (like "walk left")
 * - "Single movement": A one-off movement that an NPC is currently executing
 * - "Tile transition": The animation of a character moving from one tile to the next
 */
#include "global.h"
#include "task.h"
#include "field_player_avatar.h"
#include "event_object_movement.h"
#include "script_movement.h"
#include "event_data.h"
#include "constants/event_objects.h"

/**
 * FUNCTION: walkrun_is_standing_still
 *
 * PURPOSE: Checks whether the player character has finished moving and is standing
 *          still on a tile (not mid-step).
 *
 * GAME LOGIC:
 * tileTransitionState tracks where the player is in their movement animation:
 * - State 1: Currently transitioning between tiles (mid-step)
 * - Other states: Standing still on a tile
 * We need to wait for state != 1 before starting scripts.
 *
 * RETURNS: TRUE if the player is standing still, FALSE if mid-step
 */
bool8 walkrun_is_standing_still(void)
{
    if (gPlayerAvatar.tileTransitionState == 1)
        return FALSE;  /* Player is mid-step — still transitioning between tiles */
    else
        return TRUE;   /* Player is stationary on a tile */
}

/**
 * FUNCTION: Task_WaitPlayerStopMoving
 *
 * PURPOSE: Task that polls each frame until the player finishes moving, then
 *          enforces their facing direction and self-destructs.
 *
 * GAME LOGIC:
 * This task runs as a background task (priority 80) after FreezeObjects_WaitForPlayer
 * is called. Each frame it checks if the player is standing still. Once they are,
 * it calls HandleEnforcedLookDirectionOnPlayerStopMoving to make the player face
 * the NPC they're interacting with, then destroys itself.
 *
 * @param taskId — The task's ID in the task system (used for self-destruction)
 */
void Task_WaitPlayerStopMoving(u8 taskId)
{
    if (walkrun_is_standing_still())
    {
        HandleEnforcedLookDirectionOnPlayerStopMoving();  /* Face the NPC */
        DestroyTask(taskId);  /* Task is done — remove it from the task list */
    }
}

/**
 * FUNCTION: IsFreezePlayerFinished
 *
 * PURPOSE: Checks whether the player freeze process has completed (the wait task
 *          has finished and destroyed itself).
 *
 * GAME LOGIC:
 * Called by the script engine each frame to check if it can proceed. Returns FALSE
 * while the task is still active (player still moving), TRUE once the player has
 * fully stopped. When TRUE, also calls StopPlayerAvatar to ensure no residual
 * movement is happening.
 *
 * RETURNS: TRUE if the player has fully stopped, FALSE if still waiting
 */
bool8 IsFreezePlayerFinished(void)
{
    if (FuncIsActiveTask(Task_WaitPlayerStopMoving))
        return FALSE;  /* Still waiting for the player to stop */
    else
    {
        StopPlayerAvatar();  /* Fully halt all player movement state */
        return TRUE;
    }
}

/**
 * FUNCTION: FreezeObjects_WaitForPlayer
 *
 * PURPOSE: Freezes all NPC objects and creates a task to wait for the player to
 *          finish their current step.
 *
 * GAME LOGIC:
 * This is the entry point for "freeze everyone and wait for the player to stop."
 * Used before simple scripts that only care about the player (e.g., examining a sign).
 * Task priority 80 is in the "normal" range — higher numbers mean lower priority.
 */
void FreezeObjects_WaitForPlayer(void)
{
    FreezeObjectEvents();  /* Stop all NPCs from starting new movements */
    CreateTask(Task_WaitPlayerStopMoving, 80);  /* Create polling task */
}

/**
 * FUNCTION: Task_WaitPlayerAndTargetNPCStopMoving
 *
 * PURPOSE: Task that waits for BOTH the player AND a specific target NPC to finish
 *          their current movements before self-destructing.
 *
 * GAME LOGIC:
 * Uses task data[0] and data[1] as boolean flags:
 * - data[0]: Has the player finished stopping? (0 = waiting, 1 = done)
 * - data[1]: Has the target NPC finished stopping? (0 = waiting, 1 = done)
 * Both must become 1 before the task destroys itself.
 *
 * gSelectedObjectEvent is a global set by the script/interaction system to identify
 * which NPC the player is currently interacting with.
 *
 * singleMovementActive indicates the NPC is in the middle of executing a one-off
 * movement command (like walking one step). We wait for it to finish before freezing.
 *
 * @param taskId — This task's ID for self-destruction and data access
 */
void Task_WaitPlayerAndTargetNPCStopMoving(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    /* Wait for the player to stop moving */
    if (task->data[0] == 0 && walkrun_is_standing_still() == TRUE)
    {
        HandleEnforcedLookDirectionOnPlayerStopMoving();
        task->data[0] = 1;  /* Mark player as stopped */
    }

    /* Wait for the target NPC to finish their current step */
    if (task->data[1] == 0 && !gObjectEvents[gSelectedObjectEvent].singleMovementActive)
    {
        FreezeObjectEvent(&gObjectEvents[gSelectedObjectEvent]);  /* Freeze the NPC in place */
        task->data[1] = 1;  /* Mark NPC as stopped */
    }

    /* Both stopped — task is complete */
    if (task->data[0] && task->data[1])
        DestroyTask(taskId);
}

/**
 * FUNCTION: IsFreezeSelectedObjectAndPlayerFinished
 *
 * PURPOSE: Checks whether both the player and the selected NPC have fully stopped.
 *
 * RETURNS: TRUE if both are stopped and the freeze task has completed
 */
bool8 IsFreezeSelectedObjectAndPlayerFinished(void)
{
    if (FuncIsActiveTask(Task_WaitPlayerAndTargetNPCStopMoving))
        return FALSE;
    else
    {
        StopPlayerAvatar();
        return TRUE;
    }
}

/**
 * FUNCTION: FreezeObjects_WaitForPlayerAndSelected
 *
 * PURPOSE: Freezes all NPCs (except the target one) and creates a task to wait
 *          for both the player and the target NPC to finish moving.
 *
 * GAME LOGIC:
 * This is used when the player talks to an NPC — all OTHER NPCs are frozen
 * immediately, but the target NPC is allowed to finish its current step first
 * so it doesn't freeze mid-animation. If the NPC is already idle (not in a
 * single movement), it's frozen immediately and data[1] is pre-set to 1.
 */
void FreezeObjects_WaitForPlayerAndSelected(void)
{
    u8 taskId;

    /* Freeze all NPCs except the one the player is interacting with */
    FreezeObjectEventsExceptOne(gSelectedObjectEvent);
    taskId = CreateTask(Task_WaitPlayerAndTargetNPCStopMoving, 80);

    /* If the target NPC is already idle, freeze it immediately */
    if (!gObjectEvents[gSelectedObjectEvent].singleMovementActive)
    {
        FreezeObjectEvent(&gObjectEvents[gSelectedObjectEvent]);
        gTasks[taskId].data[1] = 1;  /* Skip waiting for this NPC */
    }
}

/**
 * FUNCTION: ClearPlayerHeldMovementAndUnfreezeObjectEvents
 *
 * PURPOSE: After a script finishes, clears any scripted movement assigned to the
 *          player and unfreezes all NPCs so normal gameplay can resume.
 *
 * GAME LOGIC:
 * LOCALID_PLAYER (typically 0xFF or a special constant) identifies the player's
 * object event. The 0, 0 parameters for map group/number mean "current map."
 */
void ClearPlayerHeldMovementAndUnfreezeObjectEvents(void)
{
    u8 objectEventId = GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0);
    ObjectEventClearHeldMovementIfFinished(&gObjectEvents[objectEventId]);
    ScriptMovement_UnfreezeObjectEvents();  /* Unfreeze objects frozen by script movement system */
    UnfreezeObjectEvents();                  /* Unfreeze objects frozen by the general freeze system */
}

/**
 * FUNCTION: UnionRoom_UnlockPlayerAndChatPartner
 *
 * PURPOSE: Special unlock function for the Union Room — clears held movements
 *          for both the player and the NPC they were chatting with.
 *
 * GAME LOGIC:
 * The Union Room is a wireless multiplayer area where players appear as NPCs to
 * each other. When a chat session ends, both the local player and the remote
 * player's avatar need to be unlocked. The "active" check prevents errors if
 * the other player disconnected and their object was already removed.
 */
void UnionRoom_UnlockPlayerAndChatPartner(void)
{
    u8 objectEventId;
    /* Only clear the partner's movement if they still exist on the map */
    if (gObjectEvents[gSelectedObjectEvent].active)
        ObjectEventClearHeldMovementIfFinished(&gObjectEvents[gSelectedObjectEvent]);
    objectEventId = GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0);
    ObjectEventClearHeldMovementIfFinished(&gObjectEvents[objectEventId]);
    ScriptMovement_UnfreezeObjectEvents();
    UnfreezeObjectEvents();
}

/**
 * FUNCTION: Script_FacePlayer
 *
 * PURPOSE: Makes the selected NPC turn to face the player. Called by scripts
 *          at the start of dialogue so the NPC looks at who's talking to them.
 *
 * GAME LOGIC:
 * gSpecialVar_Facing holds the direction the player is facing. The NPC faces
 * the OPPOSITE direction (if the player faces right, the NPC faces left) to
 * create the appearance of face-to-face conversation.
 */
void Script_FacePlayer(void)
{
    ObjectEventFaceOppositeDirection(&gObjectEvents[gSelectedObjectEvent], gSpecialVar_Facing);
}

/**
 * FUNCTION: Script_ClearHeldMovement
 *
 * PURPOSE: Clears any scripted movement currently assigned to the selected NPC,
 *          allowing it to return to its default behavior.
 */
void Script_ClearHeldMovement(void)
{
    ObjectEventClearHeldMovementIfActive(&gObjectEvents[gSelectedObjectEvent]);
}
