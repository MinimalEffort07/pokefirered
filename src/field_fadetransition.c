/**
 * @file field_fadetransition.c
 * @brief Map Warp Transitions: Fades, Door Animations, Stairs, and Teleports
 *
 * FILE OVERVIEW:
 * This file orchestrates all the visual transitions that occur when the player
 * moves between maps (warps). Every time the player walks through a door, goes
 * up stairs, uses Teleport, enters a cave, or any other map transition, this
 * file controls the sequence of:
 *   1. Fading out the current screen (to black or white)
 *   2. Freezing/unfreezing player and NPC movement
 *   3. Playing door open/close animations
 *   4. Actually performing the warp (loading new map data)
 *   5. Fading back in on the new map
 *
 * TRANSITION TYPES:
 * - Door warp: Player walks up to door, door opens, player walks in, door closes,
 *   fade out, load new map, player walks out of door on other side
 * - Stair warp: Player slides diagonally while fading (ascending/descending stairs)
 * - Teleport: Spinning animation, warp out sound, load new map, land animation
 * - Simple warp: Just fade out and fade in (cave entrances, ladders)
 * - Cable Club/Link Room: Special warps that maintain multiplayer connections
 *
 * FADE DIRECTION CONVENTION:
 * - Entering indoor areas (enter): fade to WHITE (bright flash, like stepping into light)
 * - Exiting to outdoor areas (exit): fade to BLACK (dramatic, like stepping into darkness)
 * - This convention comes from Ruby/Sapphire where outdoor=bright, indoor=dark
 *
 * GBA CONTEXT:
 * Screen fading is accomplished by the palette fade system (palette.c). The GBA
 * stores colors in palette RAM (0x05000000). The fade system interpolates between
 * the "unfaded" palette buffer and a target color (black or white), writing the
 * result to the "faded" palette buffer which the GPU actually reads.
 */
#include "global.h"
#include "gflib.h"
#include "field_fadetransition.h"
#include "overworld.h"
#include "fldeff.h"
#include "field_weather.h"
#include "map_preview_screen.h"
#include "field_player_avatar.h"
#include "task.h"
#include "script.h"
#include "cable_club.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "quest_log.h"
#include "link.h"
#include "event_object_movement.h"
#include "field_door.h"
#include "field_effect.h"
#include "field_screen_effect.h"
#include "field_specials.h"
#include "event_object_lock.h"
#include "start_menu.h"
#include "constants/songs.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/field_weather.h"

static void ExitWarpFadeInScreen(u8 playerNotMoving);
static void Task_ExitDoor(u8 taskId);
static void Task_ExitNonAnimDoor(u8 taskId);
static void Task_ExitNonDoor(u8 taskId);
static void Task_TeleportWarpIn(u8 taskId);
static void Task_Teleport2Warp(u8 taskId);
static void Task_TeleportWarp(u8 taskId);
static void Task_DoorWarp(u8 taskId);
static void Task_StairWarp(u8 taskId);
static void ForceStairsMovement(u16 metatileBehavior, s16 *x, s16 *y);
static void GetStairsMovementDirection(u8 metatileBehavior, s16 *x, s16 *y);
static void UpdateStairsMovement(s16 speedX, s16 speedY, s16 *offsetX, s16 *offsetY, s16 *timer);
static void Task_ExitStairs(u8 taskId);
static void ExitStairsMovement(s16 *speedX, s16 *speedY, s16 *offsetX, s16 *offsetY, s16 *timer);
static bool8 WaitStairExitMovementFinished(s16 *speedX, s16 *speedY, s16 *offsetX, s16 *offsetY, s16 *timer);

/* ========================================================================
 * PALETTE FILL UTILITIES
 * ========================================================================
 * These instantly fill the "faded" palette buffer with a solid color.
 * Used to set the initial screen state before a fade animation begins.
 *
 * GBA CONTEXT:
 * The GBA has two palette buffers in this engine:
 * - gPlttBufferUnfaded: The "true" colors that the game wants to display
 * - gPlttBufferFaded: The colors actually sent to palette RAM each frame
 * By filling gPlttBufferFaded with white or black, the entire screen
 * appears that color regardless of what graphics are loaded.
 * PLTT_SIZE is the total palette RAM size (512 bytes = 256 colors * 2 bytes each).
 */

/**
 * FUNCTION: palette_bg_faded_fill_white
 *
 * PURPOSE: Fills the entire faded palette with white, making the screen pure white.
 * Used as the starting state for "fade from white" transitions.
 */
void palette_bg_faded_fill_white(void)
{
    CpuFastFill16(RGB_WHITE, gPlttBufferFaded, PLTT_SIZE);
}

/**
 * FUNCTION: palette_bg_faded_fill_black
 *
 * PURPOSE: Fills the entire faded palette with black, making the screen pure black.
 * Used as the starting state for "fade from black" transitions.
 */
void palette_bg_faded_fill_black(void)
{
    CpuFastFill16(RGB_BLACK, gPlttBufferFaded, PLTT_SIZE);
}

/* ========================================================================
 * WARP FADE-IN/FADE-OUT FUNCTIONS
 * ========================================================================
 * These determine WHETHER to fade to/from black or white based on the
 * transition type (entering vs exiting an area).
 */

/**
 * FUNCTION: WarpFadeInScreen
 *
 * PURPOSE: Begins a fade-in from the appropriate color after arriving on a new map.
 *
 * HOW IT WORKS:
 * Checks if this transition is an "exit" (going from indoor to outdoor) using
 * the source and destination map types. Exits fade from white; non-exits fade
 * from black. The palette is pre-filled before the fade starts so the first
 * frame shows the solid color.
 *
 * Note: palette_bg_faded_fill is called both before AND after FadeScreen because
 * FadeScreen only starts the fade process — it doesn't instantly apply the color.
 * The pre-fill ensures the screen is the right color on the very first frame.
 */
void WarpFadeInScreen(void)
{
    switch (MapTransitionIsExit(GetLastUsedWarpMapType(), GetCurrentMapType()))
    {
    case FALSE:
        /* Non-exit transition (entering indoor): fade in from black */
        palette_bg_faded_fill_black();
        FadeScreen(FADE_FROM_BLACK, 0);
        palette_bg_faded_fill_black();
        break;
    case TRUE:
        /* Exit transition (going outdoor): fade in from white */
        palette_bg_faded_fill_white();
        FadeScreen(FADE_FROM_WHITE, 0);
        palette_bg_faded_fill_white();
        break;
    }
}

/**
 * FUNCTION: WarpFadeInScreenWithDelay
 *
 * PURPOSE: Same as WarpFadeInScreen but with a 3-frame delay before the fade begins.
 * Used for door exits where there's a brief pause after the door opens.
 */
static void WarpFadeInScreenWithDelay(void)
{
    switch (MapTransitionIsExit(GetLastUsedWarpMapType(), GetCurrentMapType()))
    {
    case FALSE:
        palette_bg_faded_fill_black();
        FadeScreen(FADE_FROM_BLACK, 3);  /* 3-frame delay before fade starts */
        palette_bg_faded_fill_black();
        break;
    case TRUE:
        palette_bg_faded_fill_white();
        FadeScreen(FADE_FROM_WHITE, 3);
        palette_bg_faded_fill_white();
        break;
    }
}

/**
 * FUNCTION: FadeInFromBlack
 *
 * PURPOSE: Unconditionally fades in from black (ignoring map transition type).
 * Used for situations where black fade is always appropriate, like whiteout recovery.
 */
void FadeInFromBlack(void)
{
    palette_bg_faded_fill_black();
    FadeScreen(FADE_FROM_BLACK, 0);
    palette_bg_faded_fill_black();
}

/**
 * FUNCTION: WarpFadeOutScreen
 *
 * PURPOSE: Begins fading out the screen before a map warp. Determines whether
 * to fade to black or white based on the destination map type.
 *
 * HOW IT WORKS:
 * Has a special case: if the destination is in a different region map section AND
 * has a cave preview screen, always fades to black (cave entrances). Otherwise,
 * uses the standard enter/exit logic (entering = white, exiting = black).
 *
 * GAME LOGIC:
 * The cave preview screen is the "Entering [CAVE NAME]" text that appears when
 * you walk into a cave. If it's going to show, we always want a black fade
 * so the preview text appears on a black background.
 */
void WarpFadeOutScreen(void)
{
    const struct MapHeader *header = GetDestinationWarpMapHeader();
    if (header->regionMapSectionId != gMapHeader.regionMapSectionId && MapHasPreviewScreen(header->regionMapSectionId, MPS_TYPE_CAVE))
        FadeScreen(FADE_TO_BLACK, 0);  /* Cave entrance: always black */
    else
    {
        switch (MapTransitionIsEnter(GetCurrentMapType(), header->mapType))
        {
        case FALSE:
            FadeScreen(FADE_TO_BLACK, 0);   /* Exit: fade to black */
            break;
        case TRUE:
            FadeScreen(FADE_TO_WHITE, 0);   /* Enter: fade to white */
            break;
        }
    }
}

/**
 * FUNCTION: WarpFadeOutScreenWithDelay
 *
 * PURPOSE: Same as WarpFadeOutScreen but with a 3-frame delay. Currently unused.
 */
static void WarpFadeOutScreenWithDelay(void) // Unused
{
    switch (MapTransitionIsEnter(GetCurrentMapType(), GetDestinationWarpMapHeader()->mapType))
    {
    case FALSE:
        FadeScreen(FADE_TO_BLACK, 3);
        break;
    case TRUE:
        FadeScreen(FADE_TO_WHITE, 3);
        break;
    }
}

/**
 * FUNCTION: SetPlayerVisibility
 *
 * PURPOSE: Helper to show or hide the player sprite on screen.
 * Inverts the parameter because the underlying function takes "invisible" not "visible".
 *
 * @param visible — TRUE to show the player, FALSE to hide
 */
static void SetPlayerVisibility(bool8 visible)
{
    SetPlayerInvisibility(!visible);
}

/* ========================================================================
 * FIELD CALLBACKS — Warp Destination Handlers
 * ========================================================================
 * "Field callbacks" (FieldCB) are functions called by the overworld system
 * after a new map has been fully loaded. They handle the fade-in and any
 * special behavior needed when arriving on the new map.
 */

/**
 * FUNCTION: Task_ContinueScriptUnionRoom
 *
 * PURPOSE: Waits for fade to complete after returning to the Union Room.
 * Does NOT re-enable scripts (Union Room handles its own script flow).
 */
static void Task_ContinueScriptUnionRoom(u8 taskId)
{
    if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
        DestroyTask(taskId);
}

/**
 * FUNCTION: FieldCB_ContinueScriptUnionRoom
 *
 * PURPOSE: Field callback for returning to the Union Room from a sub-screen.
 * Locks controls, starts music, fades in from black.
 */
void FieldCB_ContinueScriptUnionRoom(void)
{
    LockPlayerFieldControls();
    Overworld_PlaySpecialMapMusic();
    FadeInFromBlack();
    CreateTask(Task_ContinueScriptUnionRoom, 10);
}

/**
 * FUNCTION: Task_ContinueScript
 *
 * PURPOSE: Waits for fade to complete, then re-enables the script engine.
 * The most common "wait then resume" task pattern.
 */
static void Task_ContinueScript(u8 taskId)
{
    if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
    {
        DestroyTask(taskId);
        ScriptContext_Enable();
    }
}

/**
 * FUNCTION: FieldCB_ContinueScriptHandleMusic
 *
 * PURPOSE: Field callback that starts map music, fades in, and resumes scripts.
 * Used for script-triggered warps that need music to change.
 */
void FieldCB_ContinueScriptHandleMusic(void)
{
    LockPlayerFieldControls();
    Overworld_PlaySpecialMapMusic();
    FadeInFromBlack();
    CreateTask(Task_ContinueScript, 10);
}

/**
 * FUNCTION: FieldCB_ContinueScript
 *
 * PURPOSE: Field callback that fades in and resumes scripts WITHOUT changing music.
 * Used when the warp destination shares the same music as the source.
 */
void FieldCB_ContinueScript(void)
{
    LockPlayerFieldControls();
    FadeInFromBlack();
    CreateTask(Task_ContinueScript, 10);
}

/* ========================================================================
 * LINK / CABLE CLUB TRANSITIONS
 * ======================================================================== */

/**
 * FUNCTION: Task_ReturnToFieldCableLink
 *
 * PURPOSE: Multi-step task for returning to the overworld after a Cable Club
 * link session (trading, battling via link cable).
 *
 * State 0: Starts re-establishing the link cable connection
 * State 1: Waits for link to reconnect, then fades in
 * State 2: Waits for fade to finish, then unlocks player controls
 *
 * @param taskId — task identifier
 */
static void Task_ReturnToFieldCableLink(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->data[0])
    {
    case 0:
        task->data[1] = CreateTask_ReestablishCableClubLink();
        task->data[0]++;
        break;
    case 1:
        if (gTasks[task->data[1]].isActive != TRUE)
        {
            WarpFadeInScreen();
            task->data[0]++;
        }
        break;
    case 2:
        if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
        {
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
        }
        break;
    }
}

/**
 * FUNCTION: FieldCB_ReturnToFieldCableLink
 *
 * PURPOSE: Field callback for returning from Cable Club activities.
 */
void FieldCB_ReturnToFieldCableLink(void)
{
    LockPlayerFieldControls();
    Overworld_PlaySpecialMapMusic();
    palette_bg_faded_fill_black();
    CreateTask(Task_ReturnToFieldCableLink, 10);
}

/**
 * FUNCTION: Task_ReturnToFieldRecordMixing
 *
 * PURPOSE: Multi-step task for returning after wireless record mixing.
 * Similar to cable link but uses wireless-specific link synchronization.
 *
 * State 0: Sets link to standby mode
 * State 1: Waits for link task, then fades in
 * State 2: Waits for fade, starts sending key inputs to link partner, unlocks controls
 */
static void Task_ReturnToFieldRecordMixing(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->data[0])
    {
    case 0:
        SetLinkStandbyCallback();
        task->data[0]++;
        break;
    case 1:
        if (IsLinkTaskFinished())
        {
            WarpFadeInScreen();
            task->data[0]++;
        }
        break;
    case 2:
        if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
        {
            StartSendingKeysToLink();
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
        }
        break;
    }
}

/**
 * FUNCTION: FieldCB_ReturnToFieldWirelessLink
 *
 * PURPOSE: Field callback for returning from wireless link activities.
 */
void FieldCB_ReturnToFieldWirelessLink(void)
{
    LockPlayerFieldControls();
    Overworld_PlaySpecialMapMusic();
    palette_bg_faded_fill_black();
    CreateTask(Task_ReturnToFieldRecordMixing, 10);
}

/* ========================================================================
 * WARP EXIT TASK SETUP
 * ========================================================================
 * After arriving on a new map, this system determines what kind of exit
 * animation to play based on the metatile the player is standing on.
 */

/**
 * FUNCTION: SetUpWarpExitTask
 *
 * PURPOSE: Examines the destination tile and creates the appropriate warp exit
 * animation task (door exit, stair exit, or simple fade-in).
 *
 * HOW IT WORKS:
 * Reads the metatile behavior at the player's destination coordinates:
 * - If it's a warp door tile: Uses Task_ExitDoor (barn door wipe + door animation)
 * - If it's a non-animated door: Uses Task_ExitNonAnimDoor (player walks out, no door anim)
 * - If it's a directional stair warp: Uses Task_ExitStairs (diagonal slide-in)
 * - Otherwise: Uses Task_ExitNonDoor (simple fade-in)
 *
 * GAME LOGIC:
 * "Metatile behavior" is a property of each map tile that defines how the game
 * interacts with it (walkable, warp, door, water, etc.). This system uses it
 * to determine what animation to play when the player appears on the tile.
 *
 * @param playerNotMoving — if TRUE, always fades from black (no directional logic)
 */
static void SetUpWarpExitTask(bool8 playerNotMoving)
{
    s16 x, y;
    u32 metatileBehavior;
    TaskFunc func;

    PlayerGetDestCoords(&x, &y);
    metatileBehavior = MapGridGetMetatileBehaviorAt(x, y);

    if (MetatileBehavior_IsWarpDoor_2(metatileBehavior) == TRUE)
    {
        /* Animated door exit: use barn door wipe effect */
        func = Task_ExitDoor;
        /* Pre-fill palette based on transition direction */
        switch (MapTransitionIsExit(GetLastUsedWarpMapType(), GetCurrentMapType()))
        {
        case FALSE:
            palette_bg_faded_fill_black();
            break;
        case TRUE:
            palette_bg_faded_fill_white();
            break;
        }
    }
    else
    {
        /* Non-door exits: use standard fade-in */
        ExitWarpFadeInScreen(playerNotMoving);
        if (MetatileBehavior_IsNonAnimDoor(metatileBehavior) == TRUE)
            func = Task_ExitNonAnimDoor;
        else if (MetatileBehavior_IsDirectionalStairWarp(metatileBehavior) == TRUE)
        {
            u8 tmp = gExitStairsMovementDisabled;
            func = Task_ExitNonDoor;
            if (!tmp)
                func = Task_ExitStairs;  /* Stair slide-in animation */
        }
        else
            func = Task_ExitNonDoor;     /* Simple appear-and-fade */
    }
    gExitStairsMovementDisabled = FALSE;
    CreateTask(func, 10);
}

/**
 * FUNCTION: ExitWarpFadeInScreen
 *
 * PURPOSE: Chooses the fade-in method based on whether the player is moving.
 * If the player walked into the warp, uses directional fade (black/white based
 * on map type). If stationary, always fades from black.
 *
 * @param playerNotMoving — TRUE if the player didn't walk (e.g., script warp)
 */
static void ExitWarpFadeInScreen(bool8 playerNotMoving)

{
    if (!playerNotMoving)
        WarpFadeInScreen();   /* Directional fade (black or white) */
    else
        FadeInFromBlack();    /* Always black for stationary warps */
}

/* ========================================================================
 * STANDARD FIELD CALLBACKS
 * ======================================================================== */

/**
 * FUNCTION: FieldCB_DefaultWarpExit
 *
 * PURPOSE: The default field callback for most warps. Plays map music, draws
 * quest log header if in playback mode, sets up the exit animation, and locks
 * player controls until the animation completes.
 */
void FieldCB_DefaultWarpExit(void)
{
    Overworld_PlaySpecialMapMusic();
    QuestLog_DrawPreviouslyOnQuestHeaderIfInPlaybackMode();
    SetUpWarpExitTask(FALSE);
    LockPlayerFieldControls();
}

/**
 * FUNCTION: FieldCB_WarpExitFadeFromBlack
 *
 * PURPOSE: Like FieldCB_DefaultWarpExit but always fades from black.
 * Used for warps where the player doesn't walk (e.g., loading a save).
 */
void FieldCB_WarpExitFadeFromBlack(void)
{
    Overworld_PlaySpecialMapMusic();
    QuestLog_DrawPreviouslyOnQuestHeaderIfInPlaybackMode();
    SetUpWarpExitTask(TRUE);  /* TRUE = always fade from black */
    LockPlayerFieldControls();
}

/**
 * FUNCTION: FieldCB_TeleportWarpIn
 *
 * PURPOSE: Field callback for arriving at a destination via Teleport/Fly.
 * Plays the warp-out sound effect and starts the teleport landing animation
 * (player spins down from above).
 */
static void FieldCB_TeleportWarpIn(void)
{
    Overworld_PlaySpecialMapMusic();
    WarpFadeInScreen();
    QuestLog_DrawPreviouslyOnQuestHeaderIfInPlaybackMode();
    PlaySE(SE_WARP_OUT);  /* The "whoosh" landing sound */
    CreateTask(Task_TeleportWarpIn, 10);
    LockPlayerFieldControls();
}

/* ========================================================================
 * DOOR EXIT ANIMATION
 * ========================================================================
 * When the player arrives on a map through a door, this complex sequence plays:
 * 1. Screen starts black, player is hidden
 * 2. Barn door wipe opens (two bars slide apart) with delayed fade-in
 * 3. After 25 frames, door opens with sound effect
 * 4. Player becomes visible and walks downward out of the doorway
 * 5. After 14 frames of walking, door closes behind the player
 * 6. When everything is done, unfreeze all objects and unlock controls
 */

/**
 * FUNCTION: Task_ExitDoor
 *
 * PURPOSE: Orchestrates the door exit animation — the player walking out of a
 * building onto the map.
 *
 * HOW IT WORKS:
 * Uses a complex state machine with two paths:
 * - FRLG path (states 5-9): Uses a barn door wipe effect before showing the door
 * - Legacy RS path (states 0-4): Simpler fade-in without barn door wipe
 *
 * State 5: Hide player, freeze NPCs, start barn door wipe + delayed fade
 * State 6: Wait 25 frames (delay for wipe to partially complete)
 * State 7: Play door sound, animate door opening, wait for it to finish
 * State 8: Show player, walk downward out of doorway
 * State 9: After 14 frames, close door behind player, wait for everything to finish
 * State 4: Unfreeze all objects, unlock controls, destroy task
 *
 * @param taskId — task identifier
 */
static void Task_ExitDoor(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    s16 *x = &task->data[2];
    s16 *y = &task->data[3];

    /* FRLG always starts at state 5 (the enhanced door exit) */
    if (task->data[0] == 0)
        task->data[0] = 5;

    switch (task->data[0])
    {
    case 0: // Never reached (legacy RS entry point, skipped above)
        SetPlayerVisibility(0);
        FreezeObjectEvents();
        PlayerGetDestCoords(x, y);
        FieldSetDoorOpened(*x, *y);
        task->data[0] = 1;
        break;

    /* FRLG enhanced door exit path */
    case 5:
        /* Hide the player and freeze all NPCs while setting up the transition */
        SetPlayerVisibility(0);
        FreezeObjectEvents();
        /* Play the barn door wipe opening effect (two bars slide apart) */
        DoOutwardBarnDoorWipe();
        /* Start the fade-in with a delay so it syncs with the wipe */
        WarpFadeInScreenWithDelay();
        task->data[0] = 6;
        break;
    case 6:
        /* Wait 25 frames for the wipe to partially complete */
        task->data[15]++;
        if (task->data[15] == 25)
        {
            /* Now open the door with a sound effect */
            PlayerGetDestCoords(x, y);
            PlaySE(GetDoorSoundEffect(*x, *y));
            FieldAnimateDoorOpen(*x, *y);
            task->data[0] = 7;
        }
        break;
    case 7:
        /* Wait for door opening animation to complete */
        if (!FieldIsDoorAnimationRunning())
        {
            /* Save door coordinates for closing later */
            PlayerGetDestCoords(&task->data[12], &task->data[13]);
            /* Show the player and make them walk downward out of the doorway */
            SetPlayerVisibility(TRUE);
            ObjectEventSetHeldMovement(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)], MOVEMENT_ACTION_WALK_NORMAL_DOWN);
            task->data[0] = 8;
        }
        break;
    case 8:
        /* After 14 frames of walking, close the door behind the player */
        task->data[14]++;
        if (task->data[14] == 14)
        {
            FieldAnimateDoorClose(task->data[12], task->data[13]);
            task->data[0] = 9;
        }
        break;
    case 9:
        /* Wait for ALL effects to finish: fade, walking, door close, barn door wipe */
        if (FieldFadeTransitionBackgroundEffectIsFinished() && walkrun_is_standing_still() && !FieldIsDoorAnimationRunning() && !FuncIsActiveTask(Task_BarnDoorWipe))
        {
            ObjectEventClearHeldMovementIfFinished(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)]);
            task->data[0] = 4;  /* Go to shared cleanup state */
        }
        break;

    /* Legacy RS door exit path (states 1-3) */
    case 1:
        if (FieldFadeTransitionBackgroundEffectIsFinished())
        {
            SetPlayerVisibility(TRUE);
            ObjectEventSetHeldMovement(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)], MOVEMENT_ACTION_WALK_NORMAL_DOWN);
            task->data[0] = 2;
        }
        break;
    case 2:
        if (walkrun_is_standing_still())
        {
            task->data[1] = FieldAnimateDoorClose(*x, *y);
            ObjectEventClearHeldMovementIfFinished(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)]);
            task->data[0] = 3;
        }
        break;
    case 3:
        if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE)
            task->data[0] = 4;
        break;

    /* Shared cleanup state */
    case 4:
        UnfreezeObjectEvents();    /* Let NPCs move again */
        UnlockPlayerFieldControls();  /* Let the player move */
        DestroyTask(taskId);
        break;
    }
}

/**
 * FUNCTION: Task_ExitNonAnimDoor
 *
 * PURPOSE: Handles exiting through a non-animated door (like some indoor transitions
 * that have a door tile but no opening/closing animation).
 *
 * HOW IT WORKS:
 * State 0: Hide player, freeze NPCs
 * State 1: Wait for fade to finish, then show player and walk in facing direction
 * State 2: Wait for player to finish walking
 * State 3: Unfreeze everything and clean up
 *
 * @param taskId — task identifier
 */
static void Task_ExitNonAnimDoor(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    s16 *x = &task->data[2];
    s16 *y = &task->data[3];

    switch (task->data[0])
    {
    case 0:
        SetPlayerVisibility(0);
        FreezeObjectEvents();
        PlayerGetDestCoords(x, y);
        task->data[0] = 1;
        break;
    case 1:
        if (FieldFadeTransitionBackgroundEffectIsFinished())
        {
            /* Show player and walk in the direction they're facing */
            SetPlayerVisibility(TRUE);
            ObjectEventSetHeldMovement(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)], GetWalkNormalMovementAction(GetPlayerFacingDirection()));
            task->data[0] = 2;
        }
        break;
    case 2:
        if (walkrun_is_standing_still())
        {
            task->data[0] = 3;
        }
        break;
    case 3:
        UnfreezeObjectEvents();
        UnlockPlayerFieldControls();
        DestroyTask(taskId);
        break;
    }
}

/**
 * FUNCTION: Task_ExitNonDoor
 *
 * PURPOSE: Simplest warp exit — just freeze, wait for fade, then unfreeze.
 * Used for cave entrances, ladders, and other non-door warps.
 *
 * @param taskId — task identifier
 */
static void Task_ExitNonDoor(u8 taskId)
{
    switch (gTasks[taskId].data[0])
    {
    case 0:
        FreezeObjectEvents();
        LockPlayerFieldControls();
        gTasks[taskId].data[0]++;
        break;
    case 1:
        if (FieldFadeTransitionBackgroundEffectIsFinished())
        {
            UnfreezeObjectEvents();
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
        }
        break;
    }
}

/**
 * FUNCTION: Task_TeleportWarpIn
 *
 * PURPOSE: Handles the landing animation when arriving via Teleport or Fly.
 * The player spins down from above and lands on the destination tile.
 *
 * State 0: Freeze NPCs, lock controls, start the teleport landing animation
 * State 1: Wait for both the fade and the landing animation to finish
 *
 * @param taskId — task identifier
 */
static void Task_TeleportWarpIn(u8 taskId)
{
    switch (gTasks[taskId].data[0])
    {
    case 0:
        FreezeObjectEvents();
        LockPlayerFieldControls();
        StartTeleportInPlayerAnim();  /* Player spins/drops from sky */
        gTasks[taskId].data[0]++;
        break;
    case 1:
        /* Wait for both fade and teleport animation to complete */
        if (FieldFadeTransitionBackgroundEffectIsFinished() && WaitTeleportInPlayerAnim() != TRUE)
        {
            UnfreezeObjectEvents();
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
        }
        break;
    }
}

/* ========================================================================
 * START MENU RETURN TRANSITIONS
 * ======================================================================== */

/**
 * FUNCTION: Task_WaitFadeAndCreateStartMenuTask
 *
 * PURPOSE: After fading back from a sub-menu, recreates the start menu task
 * so the player returns to the menu they were in.
 */
static void Task_WaitFadeAndCreateStartMenuTask(u8 taskId)
{
    if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
    {
        DestroyTask(taskId);
        CreateTask(Task_StartMenuHandleInput, 80);
    }
}

/**
 * FUNCTION: FadeTransition_FadeInOnReturnToStartMenu
 *
 * PURPOSE: Sets up a fade-in and start menu recreation when returning from
 * a sub-screen (like the Pokedex or Bag) back to the start menu.
 */
void FadeTransition_FadeInOnReturnToStartMenu(void)
{
    FadeInFromBlack();
    CreateTask(Task_WaitFadeAndCreateStartMenuTask, 80);
    LockPlayerFieldControls();
}

/**
 * FUNCTION: FieldCB_ReturnToFieldOpenStartMenu
 *
 * PURPOSE: Field callback that re-opens the start menu when returning to the
 * overworld from a sub-screen that was opened via the start menu.
 *
 * @return Always FALSE (field callbacks return bool to signal if they handled the CB)
 */
bool8 FieldCB_ReturnToFieldOpenStartMenu(void)
{
    SetUpReturnToStartMenu();
    return FALSE;
}

/* ========================================================================
 * SAFARI ZONE TRANSITION
 * ======================================================================== */

/**
 * FUNCTION: Task_SafariZoneRanOutOfBalls
 *
 * PURPOSE: Handles the transition after running out of Safari Balls.
 * Waits for fade, unlocks controls, and unfreezes the player.
 */
static void Task_SafariZoneRanOutOfBalls(u8 taskId)
{
    if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
    {
        UnlockPlayerFieldControls();
        DestroyTask(taskId);
        ClearPlayerHeldMovementAndUnfreezeObjectEvents();
    }
}

/**
 * FUNCTION: FieldCB_SafariZoneRanOutOfBalls
 *
 * PURPOSE: Field callback when the player is ejected from the Safari Zone
 * after using all their Safari Balls.
 */
void FieldCB_SafariZoneRanOutOfBalls(void)
{
    LockPlayerFieldControls();
    Overworld_PlaySpecialMapMusic();
    FadeInFromBlack();
    CreateTask(Task_SafariZoneRanOutOfBalls, 10);
}

/* ========================================================================
 * FADE STATUS CHECK
 * ======================================================================== */

/**
 * FUNCTION: WaitWarpFadeOutScreen
 *
 * PURPOSE: Returns TRUE if a palette fade is still in progress.
 * Used by warp tasks to wait for the screen to fully fade out.
 *
 * @return TRUE if fade is still active, FALSE when complete
 */
static bool32 WaitWarpFadeOutScreen(void)
{
    return gPaletteFade.active;
}

/**
 * FUNCTION: FieldFadeTransitionBackgroundEffectIsFinished
 *
 * PURPOSE: Returns TRUE when the field fade-in transition is fully complete.
 * Checks both the weather fade-in and the forest map preview screen.
 *
 * HOW IT WORKS:
 * Weather effects (rain, fog, etc.) have their own fade-in separate from the
 * palette fade. This function waits for the weather to finish fading in AND
 * for any forest map preview screen to finish running.
 *
 * @return TRUE if all background effects are done, FALSE if still in progress
 */
bool32 FieldFadeTransitionBackgroundEffectIsFinished(void)
{
    if (IsWeatherNotFadingIn() == TRUE && ForestMapPreviewScreenIsRunning())
        return TRUE;
    else
        return FALSE;
}

/* ========================================================================
 * WARP INITIATION FUNCTIONS
 * ========================================================================
 * These functions START a warp from the current map. They handle fade-out,
 * sound effects, and setting up the appropriate field callback for the
 * destination map. Each sets gFieldCallback to control what happens when
 * the new map finishes loading.
 */

/**
 * FUNCTION: DoWarp
 *
 * PURPOSE: Standard warp with full effects — music transition, fade out,
 * rain stop sound, exit sound effect. Used for most cave/building warps.
 */
void DoWarp(void)
{
    LockPlayerFieldControls();
    TryFadeOutOldMapMusic();
    WarpFadeOutScreen();
    PlayRainStoppingSoundEffect();
    PlaySE(SE_EXIT);
    gFieldCallback = FieldCB_DefaultWarpExit;
    CreateTask(Task_Teleport2Warp, 10);
}

/**
 * FUNCTION: DoDiveWarp
 *
 * PURPOSE: Warp for diving underwater. Same as DoWarp but without the exit
 * sound effect (you're diving, not walking through a door).
 */
void DoDiveWarp(void)
{
    LockPlayerFieldControls();
    TryFadeOutOldMapMusic();
    WarpFadeOutScreen();
    PlayRainStoppingSoundEffect();
    gFieldCallback = FieldCB_DefaultWarpExit;
    CreateTask(Task_Teleport2Warp, 10);
}

/**
 * FUNCTION: DoStairWarp
 *
 * PURPOSE: Initiates a stair warp with diagonal sliding animation.
 * The player visually slides along the stairs while fading out.
 *
 * @param metatileBehavior — determines stair direction (up-left, up-right, etc.)
 * @param delay — frames to wait before starting the stair movement
 */
void DoStairWarp(u16 metatileBehavior, u16 delay)
{
    u8 taskId = CreateTask(Task_StairWarp, 10);
    gTasks[taskId].data[1] = metatileBehavior;
    gTasks[taskId].data[15] = delay;
    Task_StairWarp(taskId);  /* Immediately run first frame */
}

/**
 * FUNCTION: DoDoorWarp
 *
 * PURPOSE: Initiates a door warp — player walks through a door with full
 * door open/close animation sequence.
 */
void DoDoorWarp(void)
{
    LockPlayerFieldControls();
    gFieldCallback = FieldCB_DefaultWarpExit;
    CreateTask(Task_DoorWarp, 10);
}

/**
 * FUNCTION: DoTeleport2Warp
 *
 * PURPOSE: Initiates a Teleport/Fly warp with spinning animation.
 * Sets FieldCB_TeleportWarpIn as the callback so the destination gets
 * the landing animation.
 */
void DoTeleport2Warp(void)
{
    LockPlayerFieldControls();
    CreateTask(Task_Teleport2Warp, 10);
    gFieldCallback = FieldCB_TeleportWarpIn;
}

/**
 * FUNCTION: DoUnionRoomWarp
 *
 * PURPOSE: Warp specifically for entering the Union Room (wireless multiplayer area).
 * Uses default warp exit with simple fade, no special animation.
 */
void DoUnionRoomWarp(void)
{
    LockPlayerFieldControls();
    gFieldCallback = FieldCB_DefaultWarpExit;
    CreateTask(Task_TeleportWarp, 10);
}

/**
 * FUNCTION: DoFallWarp
 *
 * PURPOSE: Warp for falling through a hole (e.g., Silph Co. warp tiles).
 * Uses dive warp mechanics but with a special fall landing callback.
 */
void DoFallWarp(void)
{
    DoDiveWarp();
    gFieldCallback = FieldCB_FallWarpExit;  /* Override callback for fall landing */
}

/**
 * FUNCTION: DoEscalatorWarp
 *
 * PURPOSE: Warp for riding an escalator (player visually rises/descends).
 *
 * @param metatileBehavior — determines escalator direction (up or down)
 */
void DoEscalatorWarp(u8 metatileBehavior)
{
    LockPlayerFieldControls();
    StartEscalatorWarp(metatileBehavior, 10);
}

/**
 * FUNCTION: DoLavaridgeGymB1FWarp
 *
 * PURPOSE: Special warp for Lavaridge Gym basement floor (Ruby/Sapphire feature).
 * The player launches upward through a steam vent.
 */
void DoLavaridgeGymB1FWarp(void)
{
    LockPlayerFieldControls();
    StartLavaridgeGymB1FWarp(10);
}

/**
 * FUNCTION: DoLavaridgeGym1FWarp
 *
 * PURPOSE: Special warp for Lavaridge Gym first floor (falling through a cracked tile).
 */
void DoLavaridgeGym1FWarp(void)
{
    LockPlayerFieldControls();
    StartLavaridgeGym1FWarp(10);
}

/**
 * FUNCTION: DoTeleportWarp
 *
 * PURPOSE: Full Teleport warp with spinning animation and warp-in sound effect.
 * Fades out music, plays spin animation, then loads destination map.
 */
void DoTeleportWarp(void)
{
    LockPlayerFieldControls();
    TryFadeOutOldMapMusic();
    CreateTask(Task_TeleportWarp, 10);
    gFieldCallback = FieldCB_TeleportWarpIn;
}

/**
 * FUNCTION: DoPortholeWarp
 *
 * PURPOSE: Warp for the S.S. Anne porthole view scene. Currently unused.
 * Would fade out and show the ship's porthole view of the destination.
 */
static void DoPortholeWarp(void) // Unused
{
    LockPlayerFieldControls();
    WarpFadeOutScreen();
    CreateTask(Task_Teleport2Warp, 10);
    gFieldCallback = FieldCB_ShowPortholeView;
}

/* ========================================================================
 * CABLE CLUB WARP
 * ======================================================================== */

/**
 * FUNCTION: Task_CableClubWarp
 *
 * PURPOSE: Handles the warp into a Cable Club room (link trading/battling).
 * Uses a special return callback (CB2_ReturnToFieldCableClub) that maintains
 * the link cable connection after the warp.
 *
 * State 0: Lock controls
 * State 1: Wait for fade and music to stop
 * State 2: Load the new map with cable club return callback
 */
static void Task_CableClubWarp(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->data[0])
    {
    case 0:
        LockPlayerFieldControls();
        task->data[0]++;
        break;
    case 1:
        if (!WaitWarpFadeOutScreen() && BGMusicStopped())
            task->data[0]++;
        break;
    case 2:
        WarpIntoMap();
        SetMainCallback2(CB2_ReturnToFieldCableClub);
        DestroyTask(taskId);
        break;
    }
}

/**
 * FUNCTION: DoCableClubWarp
 *
 * PURPOSE: Initiates a Cable Club warp with exit sound and music transition.
 */
void DoCableClubWarp(void)
{
    LockPlayerFieldControls();
    TryFadeOutOldMapMusic();
    WarpFadeOutScreen();
    PlaySE(SE_EXIT);
    CreateTask(Task_CableClubWarp, 10);
}

/* ========================================================================
 * LINK ROOM RETURN
 * ======================================================================== */

/**
 * FUNCTION: Task_ReturnFromLinkRoomWarp
 *
 * PURPOSE: Handles returning to the overworld from a wireless link room.
 * Must carefully tear down the link connection before loading the new map.
 *
 * State 0: Clear link callback, fade to black, transition music, play exit SE
 * State 1: Wait for fade and music, then close the link connection
 * State 2: Wait for all remote players to disconnect, then load the map
 */
static void Task_ReturnFromLinkRoomWarp(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    switch (data[0])
    {
    case 0:
        ClearLinkCallback_2();
        FadeScreen(FADE_TO_BLACK, 0);
        TryFadeOutOldMapMusic();
        PlaySE(SE_EXIT);
        data[0]++;
        break;
    case 1:
        if (!WaitWarpFadeOutScreen() && BGMusicStopped())
        {
            SetCloseLinkCallback();  /* Begin disconnecting from link partners */
            data[0]++;
        }
        break;
    case 2:
        /* Wait until all remote players have disconnected */
        if (!gReceivedRemoteLinkPlayers)
        {
            WarpIntoMap();
            SetMainCallback2(CB2_LoadMap);
            DestroyTask(taskId);
        }
        break;
    }
}

/**
 * FUNCTION: ReturnFromLinkRoom
 *
 * PURPOSE: Entry point for returning from a wireless link room (Union Room, etc.).
 */
void ReturnFromLinkRoom(void)
{
    CreateTask(Task_ReturnFromLinkRoomWarp, 10);
}

/* ========================================================================
 * CORE WARP EXECUTION TASKS
 * ======================================================================== */

/**
 * FUNCTION: Task_Teleport2Warp
 *
 * PURPOSE: The core simple warp task. Freezes objects, waits for fade and music
 * to finish, then loads the new map. Used by most warp types as their
 * final "actually load the map" step.
 *
 * State 0: Freeze everything
 * State 1: Wait for screen fade and background music to stop
 * State 2: Load the destination map
 */
static void Task_Teleport2Warp(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->data[0])
    {
    case 0:
        FreezeObjectEvents();
        LockPlayerFieldControls();
        task->data[0]++;
        break;
    case 1:
        if (!WaitWarpFadeOutScreen() && BGMusicStopped())
            task->data[0]++;
        break;
    case 2:
        WarpIntoMap();                    /* Apply the warp (set new map coordinates) */
        SetMainCallback2(CB2_LoadMap);    /* Switch main loop to map loading routine */
        DestroyTask(taskId);
        break;
    }
}

/**
 * FUNCTION: Task_TeleportWarp
 *
 * PURPOSE: Teleport warp with spinning animation. The player spins and shrinks
 * before the map transitions.
 *
 * State 0: Freeze, lock, play warp sound, start spin-out animation
 * State 1: Wait for spin animation to finish, then start screen fade
 * State 2: Wait for fade and music
 * State 3: Load destination map
 */
static void Task_TeleportWarp(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->data[0])
    {
    case 0:
        FreezeObjectEvents();
        LockPlayerFieldControls();
        PlaySE(SE_WARP_IN);                /* Teleport whoosh sound */
        StartTeleportWarpOutPlayerAnim();   /* Player spins and shrinks */
        task->data[0]++;
        break;
    case 1:
        if (!WaitTeleportWarpOutPlayerAnim())
        {
            WarpFadeOutScreen();  /* Start fading after spin finishes */
            task->data[0]++;
        }
        break;
    case 2:
        if (!WaitWarpFadeOutScreen() && BGMusicStopped())
            task->data[0]++;
        break;
    case 3:
        WarpIntoMap();
        SetMainCallback2(CB2_LoadMap);
        DestroyTask(taskId);
        break;
    }
}

/* ========================================================================
 * DOOR WARP (ENTERING A DOOR)
 * ======================================================================== */

/**
 * FUNCTION: Task_DoorWarp
 *
 * PURPOSE: Handles the full door-entering sequence: door opens, player walks in,
 * door closes, then fade out and load new map.
 *
 * State 0: Freeze NPCs, open the door above the player (y-1), play door sound
 * State 1: Wait for door to finish opening
 * State 2: Walk player upward into the doorway, wait for walk to finish
 * State 3: Close door, hide player, wait for door to close
 * State 4: Fade out screen, transition music, rain stop sound, then execute warp
 * State 5: Alternative path — skip fade (unused in practice)
 *
 * GAME LOGIC:
 * The door is at (x, y-1) because the player stands one tile below the door
 * when activating it. The player walks UP into the door tile.
 */
static void Task_DoorWarp(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    s16 *xp = &task->data[2];
    s16 *yp = &task->data[3];
    switch (task->data[0])
    {
    case 0:
        FreezeObjectEvents();
        PlayerGetDestCoords(xp, yp);
        /* Open the door one tile above the player (y-1 is the door tile) */
        PlaySE(GetDoorSoundEffect(*xp, *yp - 1));
        task->data[1] = FieldAnimateDoorOpen(*xp, *yp - 1);
        task->data[0] = 1;
        break;
    case 1:
        /* Wait for door opening animation to complete */
        if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE)
        {
            /* Walk the player upward into the open doorway */
            ObjectEventClearHeldMovementIfActive(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)]);
            ObjectEventSetHeldMovement(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)], MOVEMENT_ACTION_WALK_NORMAL_UP);
            task->data[0] = 2;
        }
        break;
    case 2:
        /* Wait for the player to finish walking into the doorway */
        if (walkrun_is_standing_still())
        {
            /* Close the door and hide the player (they're "inside" now) */
            task->data[1] = FieldAnimateDoorClose(*xp, *yp - 1);
            ObjectEventClearHeldMovementIfFinished(&gObjectEvents[GetObjectEventIdByLocalIdAndMap(LOCALID_PLAYER, 0, 0)]);
            SetPlayerVisibility(FALSE);
            task->data[0] = 3;
        }
        break;
    case 3:
        /* Wait for door closing animation */
        if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE)
            task->data[0] = 4;
        break;
    case 4:
        /* Door is closed, player is hidden — now fade out and do the actual warp */
        TryFadeOutOldMapMusic();
        WarpFadeOutScreen();
        PlayRainStoppingSoundEffect();
        /*
         * Transition this task into Task_Teleport2Warp by changing its function
         * pointer. Reset state to 0 so Task_Teleport2Warp starts from its beginning.
         * This is a common pattern — reuse an existing task by swapping its function.
         */
        task->data[0] = 0;
        task->func = Task_Teleport2Warp;
        break;
    case 5:
        /* Alternative path without screen fade (appears unused) */
        TryFadeOutOldMapMusic();
        PlayRainStoppingSoundEffect();
        task->data[0] = 0;
        task->func = Task_Teleport2Warp;
        break;
    }
}

/* ========================================================================
 * STAIR WARP ANIMATION
 * ========================================================================
 * The stair warp creates a smooth diagonal sliding effect when the player
 * walks up or down stairs. The player sprite moves diagonally while the
 * screen fades, creating the illusion of ascending/descending stairs.
 *
 * The movement uses fixed-point math: speeds and offsets are stored in
 * units of 1/32 pixel (shifted left by 5), then divided by 32 when
 * applied to sprite coordinates. This allows sub-pixel precision for
 * smooth diagonal movement.
 */

/**
 * FUNCTION: Task_StairWarp
 *
 * PURPOSE: Orchestrates the stair warp: delay, start diagonal movement, fade out,
 * then load the new map.
 *
 * State 0: Lock controls, freeze NPCs, reset camera
 * State 1: Wait for any current movement to finish, then wait for optional delay,
 *          then start music transition, set sprite priority, begin stair movement
 * State 2: Continue stair movement for 12 frames, then start screen fade
 * State 3: Continue stair movement while waiting for fade and music to stop
 * Default: Set callback and load the new map
 *
 * @param taskId — task identifier
 */
static void Task_StairWarp(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct ObjectEvent *playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct Sprite *playerSpr = &gSprites[gPlayerAvatar.spriteId];
    switch (data[0])
    {
    case 0:
        LockPlayerFieldControls();
        FreezeObjectEvents();
        CameraObjectReset2();   /* Detach camera from player for smooth sliding */
        data[0]++;
        break;
    case 1:
        if (!ObjectEventIsMovementOverridden(playerObj) || ObjectEventClearHeldMovementIfFinished(playerObj))
        {
            /* Wait for optional delay (e.g., for stair tile walk-on animation) */
            if (data[15] != 0)
                data[15]--;
            else
            {
                TryFadeOutOldMapMusic();
                PlayRainStoppingSoundEffect();
                /*
                 * Set sprite OAM priority to 1 (behind BG0 but in front of BG2).
                 * This makes the player appear to go "behind" foreground tiles
                 * like stair railings during the slide animation.
                 */
                playerSpr->oam.priority = 1;
                /* Calculate stair direction and start diagonal movement */
                ForceStairsMovement(data[1], &data[2], &data[3]);
                PlaySE(SE_EXIT);
                data[0]++;
            }
        }
        break;
    case 2:
        /* Continue sliding diagonally for 12 frames before starting fade */
        UpdateStairsMovement(data[2], data[3], &data[4], &data[5], &data[6]);
        data[15]++;
        if (data[15] >= 12)
        {
            WarpFadeOutScreen();
            data[0]++;
        }
        break;
    case 3:
        /* Keep sliding while waiting for fade to complete */
        UpdateStairsMovement(data[2], data[3], &data[4], &data[5], &data[6]);
        if (!WaitWarpFadeOutScreen() && BGMusicStopped())
            data[0]++;
        break;
    default:
        /* Load the destination map */
        gFieldCallback = FieldCB_DefaultWarpExit;
        WarpIntoMap();
        SetMainCallback2(CB2_LoadMap);
        DestroyTask(taskId);
        break;
    }
}

/**
 * FUNCTION: UpdateStairsMovement
 *
 * PURPOSE: Advances the stair sliding animation by one frame, updating the
 * player sprite's sub-pixel offset.
 *
 * HOW IT WORKS:
 * Adds the speed values to running offset accumulators, then divides by 32
 * (right shift 5) to get the actual pixel offset applied to the sprite.
 * The vertical speed has a delayed start (doesn't apply until timer > 6)
 * unless the movement is downward (speedY > 0), creating a slight horizontal
 * lead before the vertical component kicks in.
 *
 * When the player's current walk-in-place animation finishes, it's restarted
 * to keep the walking animation looping continuously.
 *
 * @param speedX  — horizontal speed in 1/32 pixel per frame
 * @param speedY  — vertical speed in 1/32 pixel per frame
 * @param offsetX — running horizontal offset accumulator
 * @param offsetY — running vertical offset accumulator
 * @param timer   — frame counter for delayed vertical start
 */
static void UpdateStairsMovement(s16 speedX, s16 speedY, s16 *offsetX, s16 *offsetY, s16 *timer)
{
    struct Sprite *playerSpr = &gSprites[gPlayerAvatar.spriteId];
    struct ObjectEvent *playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];

    /* Apply vertical speed immediately if going down, or after 6 frames if going up */
    if (speedY > 0 || *timer > 6)
        *offsetY += speedY;
    *offsetX += speedX;
    (*timer)++;

    /* Convert from 1/32 pixel fixed-point to actual pixel offset */
    playerSpr->x2 = *offsetX >> 5;
    playerSpr->y2 = *offsetY >> 5;

    /* Keep the walking animation looping */
    if (playerObj->heldMovementFinished)
        ObjectEventForceSetHeldMovement(playerObj, GetWalkInPlaceNormalMovementAction(GetPlayerFacingDirection()));
}

/**
 * FUNCTION: ForceStairsMovement
 *
 * PURPOSE: Starts the player's walk-in-place animation and calculates the
 * stair movement direction based on the metatile behavior.
 *
 * @param metatileBehavior — the metatile behavior of the stair tile
 * @param x — output: horizontal speed for the stair slide
 * @param y — output: vertical speed for the stair slide
 */
static void ForceStairsMovement(u16 metatileBehavior, s16 *x, s16 *y)
{
    ObjectEventForceSetHeldMovement(&gObjectEvents[gPlayerAvatar.objectEventId], GetWalkInPlaceNormalMovementAction(GetPlayerFacingDirection()));
    GetStairsMovementDirection(metatileBehavior, x, y);
}

/**
 * FUNCTION: GetStairsMovementDirection
 *
 * PURPOSE: Maps a stair metatile behavior to X/Y movement speeds for the
 * diagonal sliding animation.
 *
 * HOW IT WORKS:
 * Each stair direction has different X and Y speeds (in 1/32 pixel units):
 * - Up-Right:   X=+16, Y=-10 (move right and up)
 * - Up-Left:    X=-17, Y=-10 (move left and up)
 * - Down-Right: X=+17, Y=+3  (move right and slightly down)
 * - Down-Left:  X=-17, Y=+3  (move left and slightly down)
 *
 * The asymmetry (up=-10, down=+3) creates a natural-looking stair perspective.
 *
 * @param metatileBehavior — identifies which stair direction
 * @param x — output: horizontal speed
 * @param y — output: vertical speed
 */
static void GetStairsMovementDirection(u8 metatileBehavior, s16 *x, s16 *y)
{
    if (MetatileBehavior_IsDirectionalUpRightStairWarp(metatileBehavior))
    {
        *x = 16;
        *y = -10;
    }
    else if (MetatileBehavior_IsDirectionalUpLeftStairWarp(metatileBehavior))
    {
        *x = -17;
        *y = -10;
    }
    else if (MetatileBehavior_IsDirectionalDownRightStairWarp(metatileBehavior))
    {
        *x = 17;
        *y = 3;
    }
    else if (MetatileBehavior_IsDirectionalDownLeftStairWarp(metatileBehavior))
    {
        *x = -17;
        *y = 3;
    }
    else
    {
        *x = 0;
        *y = 0;
    }
}

/* ========================================================================
 * STAIR EXIT ANIMATION (arriving on stairs)
 * ======================================================================== */

/**
 * FUNCTION: Task_ExitStairs
 *
 * PURPOSE: Plays the stair arrival animation — the player slides from an offset
 * position back to their normal position, as if walking down stairs onto the map.
 *
 * State 0: Start music, fade in, set up exit movement (reverse of entry)
 * State 1: Animate the slide until the offset returns to zero
 * Default: Reset camera, unlock controls, destroy task
 *
 * @param taskId — task identifier
 */
static void Task_ExitStairs(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    switch (data[0])
    {
    default:
        if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
        {
            CameraObjectReset1();  /* Re-attach camera to player */
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
        }
        break;
    case 0:
        Overworld_PlaySpecialMapMusic();
        WarpFadeInScreen();
        LockPlayerFieldControls();
        /* Set up the reverse movement (player slides from offset to center) */
        ExitStairsMovement(&data[1], &data[2], &data[3], &data[4], &data[5]);
        data[0]++;
        break;
    case 1:
        /* Animate until the slide-in is complete */
        if (!WaitStairExitMovementFinished(&data[1], &data[2], &data[3], &data[4], &data[5]))
            data[0]++;
        break;
    }
}

/**
 * FUNCTION: ExitStairsMovement
 *
 * PURPOSE: Initializes the stair exit sliding animation by setting the player's
 * initial offset (where they appear to be coming from) and reverse speed.
 *
 * HOW IT WORKS:
 * 1. Determines stair direction from the metatile the player is standing on
 * 2. Picks whether the player faces left or right based on stair direction
 * 3. Calculates the movement speed for the stair direction
 * 4. Sets initial offset to speed * 16 frames worth of movement
 * 5. Negates the speed so the player slides BACK to center (offset -> 0)
 *
 * The timer counts down from 16 frames, one frame per update.
 *
 * @param speedX  — output: horizontal speed (negated for reverse)
 * @param speedY  — output: vertical speed (negated for reverse)
 * @param offsetX — output: initial horizontal offset (fixed-point)
 * @param offsetY — output: initial vertical offset (fixed-point)
 * @param timer   — output: number of frames for the animation (16)
 */
static void ExitStairsMovement(s16 *speedX, s16 *speedY, s16 *offsetX, s16 *offsetY, s16 *timer)
{
    s16 x, y;
    u8 metatileBehavior;
    s32 direction;
    struct Sprite *sprite;

    PlayerGetDestCoords(&x, &y);
    metatileBehavior = MapGridGetMetatileBehaviorAt(x, y);

    /* Determine which direction the player should face while sliding in */
    if (MetatileBehavior_IsDirectionalDownRightStairWarp(metatileBehavior) || MetatileBehavior_IsDirectionalUpRightStairWarp(metatileBehavior))
        direction = DIR_WEST;   /* Coming from the right, face left */
    else
        direction = DIR_EAST;   /* Coming from the left, face right */

    ObjectEventForceSetHeldMovement(&gObjectEvents[gPlayerAvatar.objectEventId], GetWalkInPlaceFastMovementAction(direction));

    /* Get the movement speeds for this stair type */
    GetStairsMovementDirection(metatileBehavior, speedX, speedY);

    /* Start offset = 16 frames worth of movement (player starts far away) */
    *offsetX = *speedX * 16;
    *offsetY = *speedY * 16;
    *timer = 16;

    /* Apply initial offset to sprite */
    sprite = &gSprites[gPlayerAvatar.spriteId];
    sprite->x2 = *offsetX >> 5;  /* Convert from 1/32 pixel to pixels */
    sprite->y2 = *offsetY >> 5;

    /* Negate speeds so the player moves toward center (offset shrinks to 0) */
    *speedX *= -1;
    *speedY *= -1;
}

/**
 * FUNCTION: WaitStairExitMovementFinished
 *
 * PURPOSE: Advances the stair exit animation by one frame and checks if it's done.
 *
 * HOW IT WORKS:
 * Each frame, adds the (negated) speed to the offset, moving the sprite closer
 * to its final position. When the timer reaches 0, snaps the sprite offset to
 * exactly (0, 0) and returns FALSE to indicate completion.
 *
 * @return TRUE if animation is still in progress, FALSE when complete
 */
static bool8 WaitStairExitMovementFinished(s16 *speedX, s16 *speedY, s16 *offsetX, s16 *offsetY, s16 *timer)
{
    struct Sprite *sprite;
    sprite = &gSprites[gPlayerAvatar.spriteId];
    if (*timer != 0)
    {
        /* Still animating: advance offset toward zero */
        *offsetX += *speedX;
        *offsetY += *speedY;
        sprite->x2 = *offsetX >> 5;
        sprite->y2 = *offsetY >> 5;
        (*timer)--;
        return TRUE;   /* Still in progress */
    }
    else
    {
        /* Done: snap to exact center position */
        sprite->x2 = 0;
        sprite->y2 = 0;
        return FALSE;  /* Animation complete */
    }
}
