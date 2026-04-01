/**
 * @file field_tasks.c
 * @brief Overworld Persistent Tasks: Per-Step Callbacks and Time-Based Events
 *
 * FILE OVERVIEW:
 * This file manages persistent background tasks that run continuously while the
 * player is on the overworld. These tasks monitor the player's movement and
 * trigger environmental effects:
 *
 * 1. PER-STEP CALLBACKS: Functions called every frame that check if the player
 *    has moved to a new tile, and if so, apply tile-based effects:
 *    - IcefallCaveIcePerStepCallback: Cracks and breaks ice tiles in Icefall Cave
 *      when the player steps on them (ice puzzle mechanic)
 *    - AshGrassPerStepCallback: (RSE leftover) Removes ash from grass tiles
 *    - CrackedFloorPerStepCallback: (RSE leftover) Breaks cracked floor tiles
 *    - DummyPerStepCallback: Default no-op, does nothing
 *
 * 2. TIME-BASED EVENTS: In Ruby/Sapphire this handled real-time clock events.
 *    In FireRed, it only handles ambient Pokemon cries (random Pokemon sounds
 *    that play in the background while walking around).
 *
 * NOTE ON "PER-STEP":
 * Despite the name, per-step callbacks run every FRAME (60 times/second), not
 * once per step. However, most callbacks early-exit if the player hasn't moved
 * to a new tile coordinate, so they effectively only trigger "per step."
 *
 * METATILE BEHAVIORS:
 * The GBA Pokemon games divide each map into a grid of 16x16 pixel "metatiles."
 * Each metatile has a "behavior" value that tells the game how to interact with it
 * (walkable, water, ice, door, etc.). This file checks metatile behaviors to
 * determine what effect to apply when the player steps on a tile.
 */
#include "global.h"
#include "gflib.h"
#include "bike.h"
#include "event_data.h"
#include "field_camera.h"
#include "field_effect_helpers.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "overworld.h"
#include "quest_log.h"
#include "script.h"
#include "task.h"
#include "constants/field_tasks.h"
#include "constants/metatile_labels.h"
#include "constants/songs.h"

static void DummyPerStepCallback(u8 taskId);
static void AshGrassPerStepCallback(u8 taskId);
static void IcefallCaveIcePerStepCallback(u8 taskId);
static void CrackedFloorPerStepCallback(u8 taskId);

/*
 * Table of per-step callback functions, indexed by STEP_CB_* constants.
 * Each map can specify which callback to use. Most maps use STEP_CB_DUMMY (no-op).
 * Icefall Cave uses STEP_CB_ICE for the ice-cracking puzzle.
 *
 * Several entries (FORTREE_BRIDGE, PACIFIDLOG_BRIDGE, TRUCK, SECRET_BASE) are
 * dummied out because they were Ruby/Sapphire features not present in FireRed.
 */
static const TaskFunc sPerStepCallbacks[] =
{
    [STEP_CB_DUMMY]             = DummyPerStepCallback,
    [STEP_CB_ASH]               = AshGrassPerStepCallback,
    [STEP_CB_FORTREE_BRIDGE]    = DummyPerStepCallback,
    [STEP_CB_PACIFIDLOG_BRIDGE] = DummyPerStepCallback,
    [STEP_CB_ICE]               = IcefallCaveIcePerStepCallback,
    [STEP_CB_TRUCK]             = DummyPerStepCallback,
    [STEP_CB_SECRET_BASE]       = DummyPerStepCallback,
    [STEP_CB_CRACKED_FLOOR]     = CrackedFloorPerStepCallback
};

/*
 * Coordinates of each crackable ice tile in Icefall Cave.
 * These are in MAP tile coordinates (before adding MAP_OFFSET for the border).
 * Each entry is {x, y}. There are 9 ice tiles total in the puzzle.
 *
 * The player must step on these tiles in the correct order to solve the puzzle.
 * Each tile cracks on first step and breaks (creates a hole) on second step.
 */
static const u8 sIcefallCaveIceCoords[][2] =
{
    {  8,  3 },
    { 10,  5 },
    { 15,  5 },
    {  8,  9 },
    {  9,  9 },
    { 16,  9 },
    {  8, 10 },
    {  9, 10 },
    {  8, 14 }
};

#define tCallbackId data[0]  /* Index into sPerStepCallbacks[] */

/**
 * FUNCTION: Task_RunPerStepCallback
 *
 * PURPOSE: Persistent task that runs every frame, dispatching to the currently
 * active per-step callback function.
 *
 * HOW IT WORKS:
 * Simply reads the callback index from its task data and calls the corresponding
 * function from sPerStepCallbacks[]. The callback handles its own logic for
 * checking player movement and applying effects.
 *
 * @param taskId — task identifier
 */
static void Task_RunPerStepCallback(u8 taskId)
{
    int idx = gTasks[taskId].tCallbackId;
    sPerStepCallbacks[idx](taskId);
}

#define tAmbientCryState data[1]  /* State for ambient Pokemon cry system */
#define tAmbientCryDelay data[2]  /* Countdown timer between ambient cries */

/**
 * FUNCTION: Task_RunTimeBasedEvents
 *
 * PURPOSE: Persistent task that handles time-dependent overworld events.
 * In FireRed, this only handles ambient Pokemon cries (the random Pokemon
 * sounds you hear while walking around in grass, caves, etc.).
 *
 * HOW IT WORKS:
 * Only runs if the player has control (not locked by a script/cutscene) and
 * the quest log is not in playback mode. The ambient cry system periodically
 * plays a random cry from a Pokemon that can be found in the current area.
 *
 * NOTE: In Ruby/Sapphire, this also handled real-time clock events like
 * berry growth and tide changes. That functionality was removed in FireRed
 * since FR/LG doesn't have a real-time clock.
 *
 * @param taskId — task identifier
 */
static void Task_RunTimeBasedEvents(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    if (!ArePlayerFieldControlsLocked() && !QL_IS_PLAYBACK_STATE)
        UpdateAmbientCry(&tAmbientCryState, &tAmbientCryDelay);
}

/**
 * FUNCTION: SetUpFieldTasks
 *
 * PURPOSE: Creates the two persistent overworld tasks if they don't already exist.
 * Called during overworld initialization.
 *
 * HOW IT WORKS:
 * Guards against duplicate creation using FuncIsActiveTask checks. Both tasks
 * run at priority 80 (lower priority number = higher priority, 80 is mid-range).
 * The per-step callback starts as STEP_CB_DUMMY (no-op) until a specific map
 * activates a callback via ActivatePerStepCallback().
 */
void SetUpFieldTasks(void)
{
    if (!FuncIsActiveTask(Task_RunPerStepCallback))
    {
        u8 taskId = CreateTask(Task_RunPerStepCallback, 80);
        gTasks[taskId].tCallbackId = STEP_CB_DUMMY;
    }

    if (!FuncIsActiveTask(Task_RunTimeBasedEvents))
        CreateTask(Task_RunTimeBasedEvents, 80);
}

/**
 * FUNCTION: ActivatePerStepCallback
 *
 * PURPOSE: Switches the per-step callback to a new function. Called when entering
 * a map that has special step-based mechanics (like Icefall Cave's ice puzzle).
 *
 * HOW IT WORKS:
 * Finds the per-step task, clears all its data (reset any previous state), and
 * sets the callback ID to the new one. If the ID is out of range, falls back
 * to STEP_CB_DUMMY for safety.
 *
 * @param callbackId — STEP_CB_* constant identifying which callback to activate
 */
void ActivatePerStepCallback(u8 callbackId)
{
    u8 taskId = FindTaskIdByFunc(Task_RunPerStepCallback);
    if (taskId != TASK_NONE)
    {
        s32 i;
        s16 *data = gTasks[taskId].data;

        /* Clear all task data to reset any leftover state from the previous callback */
        for (i = 0; i < NUM_TASK_DATA; i++)
            data[i] = 0;

        /* Set new callback, with bounds checking */
        if (callbackId >= ARRAY_COUNT(sPerStepCallbacks))
            tCallbackId = STEP_CB_DUMMY;
        else
            tCallbackId = callbackId;
    }
}

/**
 * FUNCTION: ResetFieldTasksArgs
 *
 * PURPOSE: Resets the data fields of both persistent overworld tasks without
 * destroying them. Used when the overworld needs to clear state (e.g., after
 * returning from a battle) without fully reinitializing.
 */
void ResetFieldTasksArgs(void)
{
    u8 taskId;
    s16 *data;

    taskId = FindTaskIdByFunc(Task_RunPerStepCallback);
    if (taskId != TASK_NONE)
        data = gTasks[taskId].data;

    taskId = FindTaskIdByFunc(Task_RunTimeBasedEvents);
    if (taskId != TASK_NONE)
    {
        data = gTasks[taskId].data;
        tAmbientCryState = 0;
        tAmbientCryDelay = 0;
    }
}

#undef tAmbientCryState
#undef tAmbientCryDelay

/**
 * FUNCTION: DummyPerStepCallback
 *
 * PURPOSE: Default no-op per-step callback. Does nothing.
 * Used for maps that don't need any step-based environmental effects.
 */
static void DummyPerStepCallback(u8 taskId)
{
}

/* ========================================================================
 * ICEFALL CAVE ICE PUZZLE
 * ========================================================================
 * Icefall Cave has a puzzle where the player walks over thin ice tiles.
 * Each tile has 3 states: Thin Ice -> Cracked Ice -> Hole (impassable).
 * Stepping on thin ice cracks it; stepping on cracked ice breaks it.
 * The player must find a path across without breaking tiles they'll need later.
 *
 * Tile state is persisted using event flags (flags 1-9, one per tile).
 * When a flag is set, the corresponding tile starts as cracked on map load.
 */

/**
 * FUNCTION: MarkIcePuzzleCoordVisited
 *
 * PURPOSE: Marks an ice tile as visited by setting its corresponding event flag.
 * This persists the cracked state so it remains cracked if the player leaves
 * and re-enters the map.
 *
 * @param x — map grid X coordinate (with MAP_OFFSET applied)
 * @param y — map grid Y coordinate (with MAP_OFFSET applied)
 */
static void MarkIcePuzzleCoordVisited(s16 x, s16 y)
{
    u8 i;
    for (i = 0; i < ARRAY_COUNT(sIcefallCaveIceCoords); i++)
    {
        /* Compare with MAP_OFFSET added to convert from raw coords to grid coords */
        if (sIcefallCaveIceCoords[i][0] + MAP_OFFSET == x && sIcefallCaveIceCoords[i][1] + MAP_OFFSET == y)
        {
            FlagSet(i + 1);  /* Flags 1-9 correspond to ice tiles 0-8 */
            break;
        }
    }
}

/**
 * FUNCTION: SetIcefallCaveCrackedIceMetatiles
 *
 * PURPOSE: On map load, restores any previously cracked ice tiles by checking
 * their flags. If a flag is set, the corresponding tile is replaced with the
 * cracked ice metatile graphic.
 *
 * GAME LOGIC:
 * This is called when the Icefall Cave map loads. Without this, all ice tiles
 * would reset to their intact state every time the player re-enters the room.
 */
void SetIcefallCaveCrackedIceMetatiles(void)
{
    u8 i;
    for (i = 0; i < ARRAY_COUNT(sIcefallCaveIceCoords); i++)
    {
        if (FlagGet(i + 1) == TRUE)
        {
            int x = sIcefallCaveIceCoords[i][0] + MAP_OFFSET;
            int y = sIcefallCaveIceCoords[i][1] + MAP_OFFSET;
            MapGridSetMetatileIdAt(x, y, METATILE_SeafoamIslands_CrackedIce);
        }
    }
}

#define tState data[1]   /* State machine for ice cracking animation */
#define tPrevX data[2]   /* Player's previous X coordinate (to detect movement) */
#define tPrevY data[3]   /* Player's previous Y coordinate */
#define tIceX  data[4]   /* X coordinate of the ice tile being cracked/broken */
#define tIceY  data[5]   /* Y coordinate of the ice tile being cracked/broken */
#define tDelay data[6]   /* Frame delay before the crack/break effect happens */

/**
 * FUNCTION: IcefallCaveIcePerStepCallback
 *
 * PURPOSE: Per-step callback for Icefall Cave that manages the ice cracking puzzle.
 *
 * HOW IT WORKS:
 * State 0: Initialize by recording the player's current position
 * State 1: Check if the player has moved to a new tile. If so:
 *   - If the new tile is thin ice: start cracking it (-> state 2)
 *   - If the new tile is cracked ice: start breaking it (-> state 3)
 *   - Otherwise: do nothing, stay in state 1
 * State 2: After a 4-frame delay, crack the ice (change thin -> cracked metatile)
 * State 3: After a 4-frame delay, break the ice (change cracked -> hole metatile)
 *
 * The 4-frame delay after stepping ensures the player has visually moved onto
 * the tile before the crack/break animation plays.
 *
 * @param taskId — task identifier
 */
static void IcefallCaveIcePerStepCallback(u8 taskId)
{
    s16 x, y;
    u8 tileBehavior;
    u16 *iceStepCount;
    s16 *data = gTasks[taskId].data;
    switch (tState)
    {
        case 0:
            /* Initialize: record starting position */
            PlayerGetDestCoords(&x, &y);
            tPrevX = x;
            tPrevY = y;
            tState = 1;
            break;
        case 1:
            /* Check if player has moved to a new tile */
            PlayerGetDestCoords(&x, &y);
            if (x == tPrevX && y == tPrevY)
                return;  /* Player hasn't moved — do nothing */

            /* Player moved to a new tile */
            tPrevX = x;
            tPrevY = y;
            tileBehavior = MapGridGetMetatileBehaviorAt(x, y);
            if (MetatileBehavior_IsThinIce(tileBehavior) == TRUE)
            {
                /* Thin ice: will crack after a short delay */
                MarkIcePuzzleCoordVisited(x, y);
                tDelay = 4;
                tState = 2;
                tIceX = x;
                tIceY = y;
            }
            else if (MetatileBehavior_IsCrackedIce(tileBehavior) == TRUE)
            {
                /* Cracked ice: will break (create hole) after a short delay */
                tDelay = 4;
                tState = 3;
                tIceX = x;
                tIceY = y;
            }
            break;
        case 2:
            /* Wait for delay, then crack the ice */
            if (tDelay != 0)
            {
                tDelay--;
            }
            else
            {
                x = tIceX;
                y = tIceY;
                PlaySE(SE_ICE_CRACK);
                /* Replace thin ice metatile with cracked ice metatile */
                MapGridSetMetatileIdAt(x, y, METATILE_SeafoamIslands_CrackedIce);
                CurrentMapDrawMetatileAt(x, y);  /* Redraw the tile visually */
                tState = 1;  /* Return to movement checking state */
            }
            break;
        case 3:
            /* Wait for delay, then break the ice (create hole) */
            if (tDelay != 0)
            {
                tDelay--;
            }
            else
            {
                x = tIceX;
                y = tIceY;
                PlaySE(SE_ICE_BREAK);
                /* Replace cracked ice metatile with hole metatile */
                MapGridSetMetatileIdAt(x, y, METATILE_SeafoamIslands_IceHole);
                CurrentMapDrawMetatileAt(x, y);
                VarSet(VAR_TEMP_1, 1);  /* Signal to scripts that ice was broken */
                tState = 1;
            }
            break;
    }
}

#undef tState
#undef tPrevX
#undef tPrevY
#undef tIceX
#undef tIceY
#undef tDelay

#define tPrevX data[1]
#define tPrevY data[2]

/**
 * FUNCTION: AshGrassPerStepCallback
 *
 * PURPOSE: (Ruby/Sapphire leftover — unused in FireRed)
 * Removes ash from ash-covered grass when the player steps on it.
 * In RS, Route 113 near Fallarbor Town has volcanic ash covering the grass.
 * The player collects ash in the Soot Sack as they walk through it.
 *
 * @param taskId — task identifier
 */
static void AshGrassPerStepCallback(u8 taskId)
{
    s16 x, y;
    u16 *ashGatherCount;
    s16 *data = gTasks[taskId].data;
    PlayerGetDestCoords(&x, &y);

    if (x == tPrevX && y == tPrevY)
        return;  /* Player hasn't moved */

    tPrevX = x;
    tPrevY = y;
    if (MetatileBehavior_IsAshGrass((u8)MapGridGetMetatileBehaviorAt(x, y)))
    {
        /* Replace ash-covered grass with normal grass and play the ash effect */
        if (MapGridGetMetatileIdAt(x, y) == METATILE_Fallarbor_AshGrass)
            StartAshFieldEffect(x, y, METATILE_Fallarbor_NormalGrass, 4);
        else
            StartAshFieldEffect(x, y, METATILE_Lavaridge_NormalGrass, 4);
    }
}

#undef tPrevX
#undef tPrevY

/**
 * FUNCTION: SetCrackedFloorHoleMetatile
 *
 * PURPOSE: (Ruby/Sapphire leftover — unused in FireRed)
 * Replaces a cracked floor tile with its "hole" variant. Used in Sky Pillar
 * where the floor crumbles as you walk over it.
 *
 * @param x — map grid X coordinate
 * @param y — map grid Y coordinate
 */
static void SetCrackedFloorHoleMetatile(s16 x, s16 y)
{
    MapGridSetMetatileIdAt(x, y, MapGridGetMetatileIdAt(x, y) == METATILE_RSCave_CrackedFloor ? METATILE_RSCave_CrackedFloor_Hole : METATILE_Pacifidlog_SkyPillar_CrackedFloor_Hole);
    CurrentMapDrawMetatileAt(x, y);
}

#define tPrevX       data[2]   /* Previous player X position */
#define tPrevY       data[3]   /* Previous player Y position */
#define tFloor1Delay data[4]   /* Countdown for first tracked cracked tile */
#define tFloor1X     data[5]   /* X of first tracked cracked tile */
#define tFloor1Y     data[6]   /* Y of first tracked cracked tile */
#define tFloor2Delay data[7]   /* Countdown for second tracked cracked tile */
#define tFloor2X     data[8]   /* X of second tracked cracked tile */
#define tFloor2Y     data[9]   /* Y of second tracked cracked tile */

/**
 * FUNCTION: CrackedFloorPerStepCallback
 *
 * PURPOSE: (Ruby/Sapphire leftover — unused in FireRed)
 * Handles cracked floors in Sky Pillar. The player walks on cracked tiles,
 * which collapse into holes after a 3-frame delay. Tracks up to 2 tiles
 * at once (the current and previous cracked tile). On the Mach Bike at
 * full speed, the tiles don't reset the step counter, allowing the player
 * to outrun the collapsing floor.
 *
 * @param taskId — task identifier
 */
static void CrackedFloorPerStepCallback(u8 taskId)
{
    s16 x, y;
    u16 behavior;
    s16 *data = gTasks[taskId].data;
    PlayerGetDestCoords(&x, &y);
    behavior = MapGridGetMetatileBehaviorAt(x, y);

    /* Tick down timers for up to 2 tracked cracked tiles */
    if (tFloor1Delay != 0 && (--tFloor1Delay) == 0)
        SetCrackedFloorHoleMetatile(tFloor1X, tFloor1Y);
    if (tFloor2Delay != 0 && (--tFloor2Delay) == 0)
        SetCrackedFloorHoleMetatile(tFloor2X, tFloor2Y);

    if (x == tPrevX && y == tPrevY)
        return;  /* Player hasn't moved */

    tPrevX = x;
    tPrevY = y;
    if (MetatileBehavior_IsCrackedFloor(behavior))
    {
        /* On a Mach Bike at slow/medium speed, reset the step counter */
        if (GetPlayerSpeed() != PLAYER_SPEED_FASTEST)
            VarSet(VAR_ICE_STEP_COUNT, 0); /* This var does double duty for cracked floors */

        /* Track this tile for delayed collapse (up to 2 tiles tracked simultaneously) */
        if (tFloor1Delay == 0)
        {
            tFloor1Delay = 3;
            tFloor1X = x;
            tFloor1Y = y;
        }
        else if (tFloor2Delay == 0)
        {
            tFloor2Delay = 3;
            tFloor2X = x;
            tFloor2Y = y;
        }
    }
}

#undef tPrevX
#undef tPrevY
#undef tFloor1Delay
#undef tFloor1X
#undef tFloor1Y
#undef tFloor2Delay
#undef tFloor2X
#undef tFloor2Y
