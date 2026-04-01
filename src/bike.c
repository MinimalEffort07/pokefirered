/**
 * @file bike.c
 * @brief Bicycle Movement and Physics System
 *
 * FILE OVERVIEW:
 * This file implements the bicycle riding mechanics for the overworld. In Pokemon
 * FireRed, the player can ride a bicycle to move faster. The bike system handles:
 *
 * - INPUT PROCESSING: Reading D-pad direction and determining what transition to perform
 * - COLLISION DETECTION: Checking if the bike can move in a given direction
 * - SLOPE MECHANICS: Cycling Road (Route 17) has slopes that pull the player downhill
 *   automatically unless they hold B to resist
 * - RAIL MECHANICS: Some paths restrict movement to one axis (like minecart rails)
 * - SPEED MANAGEMENT: The Mach Bike has variable speed (slow, fast, fastest),
 *   while the Acro Bike has a fixed speed
 *
 * BIKE TYPES (from Ruby/Sapphire):
 * - Mach Bike: Speeds up over time, reaches max speed for cracked floor puzzles
 * - Acro Bike: Fixed speed, can do tricks (bunny hops) on bumpy slopes
 * In FireRed, the player's bike functions similarly to the Acro Bike.
 *
 * STATE MACHINE:
 * The bike uses a 3-state input handler system:
 * - NORMAL: Standard movement — can move, face direction, or start turning
 * - TURNING: Brief turn animation when changing direction
 * - SLOPE: On Cycling Road, automatic downhill movement or uphill resistance
 *
 * Each state returns a "transition ID" that determines what movement to perform:
 * - FACE_DIRECTION: Just turn to face (no movement)
 * - TURNING: Play the turn animation
 * - MOVE: Move one tile in the direction
 * - DOWNHILL: Auto-slide downhill (faster speed)
 * - UPHILL: Move uphill against gravity (slower speed)
 */
#include "global.h"
#include "bike.h"
#include "field_player_avatar.h"
#include "metatile_behavior.h"
#include "event_object_movement.h"
#include "fieldmap.h"
#include "field_camera.h"
#include "overworld.h"
#include "constants/map_types.h"
#include "constants/songs.h"

static u8 GetBikeTransitionId(u8 *, u16, u16);
static void Bike_SetBikeStill(void);
static u8 CanBikeFaceDirectionOnRail(u8 direction, u8 metatileBehavior);
static u8 GetBikeCollision(u8);
static u8 GetBikeCollisionAt(struct ObjectEvent *playerObjEvent, s16 x, s16 y, u8 direction, u8 metatileBehavior);
static bool8 MetatileBehaviorForbidsBiking(u8);
static void BikeTransition_FaceDirection(u8);
static void BikeTransition_TurnDirection(u8);
static void BikeTransition_MoveDirection(u8);
static void BikeTransition_Downhill(u8);
static void BikeTransition_Uphill(u8);
static u8 BikeInputHandler_Normal(u8 *, u16, u16);
static u8 BikeInputHandler_Turning(u8 *, u16, u16);
static u8 BikeInputHandler_Slope(u8 *, u16, u16);

/*
 * Bike transition function table. Each transition performs a different type of
 * movement based on what the input handler determined should happen.
 */
static void (*const sBikeTransitions[])(u8) =
{
    [BIKE_TRANS_FACE_DIRECTION] = BikeTransition_FaceDirection,  /* Just face, don't move */
    [BIKE_TRANS_TURNING]        = BikeTransition_TurnDirection,  /* Play turn animation */
    [BIKE_TRANS_MOVE]           = BikeTransition_MoveDirection,  /* Normal movement */
    [BIKE_TRANS_DOWNHILL]       = BikeTransition_Downhill,       /* Auto-slide down slope */
    [BIKE_TRANS_UPHILL]         = BikeTransition_Uphill,         /* Slow uphill movement */
};

/*
 * Input handler table for each bike state. The handler reads the current input
 * and returns which transition to perform.
 */
static u8 (*const sBikeInputHandlers[])(u8 *, u16, u16) =
{
    [BIKE_STATE_NORMAL]  = BikeInputHandler_Normal,   /* Standard riding */
    [BIKE_STATE_TURNING] = BikeInputHandler_Turning,  /* Mid-turn animation */
    [BIKE_STATE_SLOPE]   = BikeInputHandler_Slope,    /* On Cycling Road slope */
};

/**
 * FUNCTION: MovePlayerOnBike
 *
 * PURPOSE: Main entry point for bike movement. Called every frame when the player
 * is riding a bicycle. Processes input through the state machine and executes
 * the resulting movement transition.
 *
 * HOW IT WORKS:
 * 1. GetBikeTransitionId() processes the current input through the active
 *    input handler (normal/turning/slope), which may modify the direction
 *    and returns a transition ID
 * 2. The transition function from sBikeTransitions[] is called with the
 *    (potentially modified) direction to execute the movement
 *
 * @param direction — D-pad direction pressed (or DIR_NONE if no input)
 * @param newKeys   — buttons pressed this frame (not held from previous frame)
 * @param heldKeys  — buttons currently held down
 */
void MovePlayerOnBike(u8 direction, u16 newKeys, u16 heldKeys)
{
    sBikeTransitions[GetBikeTransitionId(&direction, newKeys, heldKeys)](direction);
}

/**
 * FUNCTION: GetBikeTransitionId
 *
 * PURPOSE: Dispatches to the current state's input handler to determine what
 * movement transition should occur.
 *
 * @param direction — pointer to direction (may be modified by the handler)
 * @param newKeys   — newly pressed buttons
 * @param heldKeys  — held buttons
 * @return Transition ID (BIKE_TRANS_* constant)
 */
static u8 GetBikeTransitionId(u8 *direction, u16 newKeys, u16 heldKeys)
{
    return sBikeInputHandlers[gPlayerAvatar.acroBikeState](direction, newKeys, heldKeys);
}

/* ========================================================================
 * INPUT HANDLERS — One per bike state
 * ======================================================================== */

/**
 * FUNCTION: BikeInputHandler_Normal
 *
 * PURPOSE: Handles input when the bike is in its normal (default) riding state.
 *
 * HOW IT WORKS:
 * 1. First checks if the player is on a Cycling Road pull-down tile (slope):
 *    - If not holding B: auto-slide downhill (or uphill if pressing up)
 *    - If holding B: resist gravity, only move if pressing a direction
 * 2. If no direction pressed: face current direction, don't move
 * 3. If pressing a different direction than currently facing: start a turn
 * 4. If pressing the same direction: move forward
 *
 * @param direction_p — pointer to input direction (may be modified)
 * @param newKeys     — newly pressed buttons
 * @param heldKeys    — held buttons
 * @return Transition ID to execute
 */
static u8 BikeInputHandler_Normal(u8 *direction_p, u16 newKeys, u16 heldKeys)
{
    struct ObjectEvent *playerObjEvent = &gObjectEvents[gPlayerAvatar.objectEventId];
    u8 direction = GetPlayerMovementDirection();

    gPlayerAvatar.bikeFrameCounter = 0;

    /* Check for Cycling Road slope mechanics */
    if (MetatileBehavior_IsCyclingRoadPullDownTile(playerObjEvent->currentMetatileBehavior) == TRUE)
    {
        if (!JOY_HELD(B_BUTTON))
        {
            /* Not holding B: gravity pulls the player downhill automatically */
            gPlayerAvatar.acroBikeState = BIKE_STATE_SLOPE;
            gPlayerAvatar.runningState = MOVING;
            if (*direction_p < DIR_NORTH)
                return BIKE_TRANS_DOWNHILL;   /* Moving south (or no input) = slide down */
            else
                return BIKE_TRANS_UPHILL;     /* Pressing up = struggle uphill */
        }
        else
        {
            /* Holding B: resist gravity, but can still choose to go uphill */
            if (*direction_p != DIR_NONE)
            {
                gPlayerAvatar.acroBikeState = BIKE_STATE_SLOPE;
                gPlayerAvatar.runningState = MOVING;
                return BIKE_TRANS_UPHILL;
            }
        }
    }

    /* Non-slope normal movement */
    if (*direction_p == DIR_NONE)
    {
        /* No input: face current direction, stand still */
        *direction_p = direction;
        gPlayerAvatar.runningState = NOT_MOVING;
        return BIKE_TRANS_FACE_DIRECTION;
    }
    else
    {
        if (*direction_p != direction && gPlayerAvatar.runningState != MOVING)
        {
            /* Pressing a new direction while not already moving: start turning */
            gPlayerAvatar.acroBikeState = BIKE_STATE_TURNING;
            gPlayerAvatar.newDirBackup = *direction_p;
            gPlayerAvatar.runningState = NOT_MOVING;
            return GetBikeTransitionId(direction_p, newKeys, heldKeys);
        }
        else
        {
            /* Same direction or continuing movement: move forward */
            gPlayerAvatar.runningState = MOVING;
            return BIKE_TRANS_MOVE;
        }
    }
}

/**
 * FUNCTION: BikeInputHandler_Turning
 *
 * PURPOSE: Handles the brief turning state. When the player changes direction,
 * there's one frame where the bike plays a turn animation before moving.
 *
 * HOW IT WORKS:
 * Restores the saved direction from when the turn started, resets the bike
 * to its standing speed, and transitions back to NORMAL state.
 *
 * @return Always BIKE_TRANS_TURNING
 */
static u8 BikeInputHandler_Turning(u8 *direction_p, u16 newKeys, u16 heldKeys)
{
    *direction_p = gPlayerAvatar.newDirBackup;  /* Use the direction from when turn started */
    gPlayerAvatar.runningState = TURN_DIRECTION;
    gPlayerAvatar.acroBikeState = BIKE_STATE_NORMAL;
    Bike_SetBikeStill();  /* Reset speed to standing */
    return BIKE_TRANS_TURNING;
}

/**
 * FUNCTION: BikeInputHandler_Slope
 *
 * PURPOSE: Handles input while on a Cycling Road slope tile. Manages the
 * automatic downhill pull and directional control while on slopes.
 *
 * HOW IT WORKS:
 * If still on a slope tile:
 * - Pressing a different direction: start turning
 * - Same direction: continue with downhill or uphill based on direction
 * If no longer on a slope: return to NORMAL state
 *
 * @return Appropriate transition ID for slope movement
 */
static u8 BikeInputHandler_Slope(u8 *direction_p, u16 newKeys, u16 heldKeys)
{
    u8 direction = GetPlayerMovementDirection();
    u8 playerObjEventId = gPlayerAvatar.objectEventId;

    if (MetatileBehavior_IsCyclingRoadPullDownTile(playerObjEventId[gObjectEvents].currentMetatileBehavior) == TRUE)
    {
        if (*direction_p != direction)
        {
            /* Pressing a new direction on the slope: turn first */
            gPlayerAvatar.acroBikeState = BIKE_STATE_TURNING;
            gPlayerAvatar.newDirBackup = *direction_p;
            gPlayerAvatar.runningState = NOT_MOVING;
            return GetBikeTransitionId(direction_p, newKeys, heldKeys);
        }
        else
        {
            /* Continue moving on slope */
            gPlayerAvatar.runningState = MOVING;
            gPlayerAvatar.acroBikeState = BIKE_STATE_SLOPE;
            if (*direction_p < DIR_NORTH)
                return BIKE_TRANS_DOWNHILL;  /* South/West = downhill */
            else
                return BIKE_TRANS_UPHILL;    /* North/East = uphill */
        }
    }

    /* No longer on a slope tile: return to normal bike movement */
    gPlayerAvatar.acroBikeState = BIKE_STATE_NORMAL;
    if (*direction_p == DIR_NONE)
    {
        *direction_p = direction;
        gPlayerAvatar.runningState = NOT_MOVING;
        return BIKE_TRANS_FACE_DIRECTION;
    }
    else
    {
        gPlayerAvatar.runningState = MOVING;
        return BIKE_TRANS_MOVE;
    }
}

/* ========================================================================
 * TRANSITION FUNCTIONS — Execute the actual movement
 * ======================================================================== */

/**
 * FUNCTION: BikeTransition_FaceDirection
 *
 * PURPOSE: Makes the player face a direction without moving. Used when
 * the player is stationary on the bike.
 */
static void BikeTransition_FaceDirection(u8 direction)
{
    PlayerFaceDirection(direction);
}

/**
 * FUNCTION: BikeTransition_TurnDirection
 *
 * PURPOSE: Turns the bike to face a new direction. Checks rail restrictions
 * first — if the player is on a rail tile, they can't turn perpendicular.
 *
 * @param direction — direction to turn toward
 */
static void BikeTransition_TurnDirection(u8 direction)
{
    struct ObjectEvent *playerObjEvent = &gObjectEvents[gPlayerAvatar.objectEventId];

    /* If on a rail that doesn't allow this direction, keep current facing */
    if (!CanBikeFaceDirectionOnRail(direction, playerObjEvent->currentMetatileBehavior))
        direction = playerObjEvent->movementDirection;
    PlayerFaceDirection(direction);
}

/**
 * FUNCTION: BikeTransition_MoveDirection
 *
 * PURPOSE: Attempts to move the bike one tile in the given direction, handling
 * collisions, ledge jumps, and rail restrictions.
 *
 * HOW IT WORKS:
 * 1. Check rail restrictions (can't move perpendicular on rails)
 * 2. If restricted: just face the rail's direction
 * 3. If clear: check for collisions at the destination tile
 *    - COLLISION_NONE: Move normally (or ride water current if applicable)
 *    - COLLISION_LEDGE_JUMP: Jump over a ledge
 *    - COLLISION_COUNT: Move onto cracked ice (special fast movement)
 *    - Other collisions: Play bump/collide animation
 *
 * @param direction — direction to move
 */
static void BikeTransition_MoveDirection(u8 direction)
{
    struct ObjectEvent *playerObjEvent;

    playerObjEvent = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!CanBikeFaceDirectionOnRail(direction, playerObjEvent->currentMetatileBehavior))
    {
        /* Rail restricts this direction */
        BikeTransition_FaceDirection(playerObjEvent->movementDirection);
    }
    else
    {
        u8 collision = GetBikeCollision(direction);

        if (collision > COLLISION_NONE && collision <= COLLISION_ISOLATED_HORIZONTAL_RAIL)
        {
            /* Some kind of collision detected */
            if (collision == COLLISION_LEDGE_JUMP)
                PlayerJumpLedge(direction);  /* Jump over the ledge */
            else if (collision != COLLISION_STOP_SURFING
                  && collision != COLLISION_LEDGE_JUMP
                  && collision != COLLISION_PUSHED_BOULDER
                  && collision != COLLISION_DIRECTIONAL_STAIR_WARP)
                PlayerOnBikeCollide(direction);  /* Bump into obstacle */
        }
        else
        {
            /* No blocking collision */
            if (collision == COLLISION_COUNT)
                PlayerWalkFast(direction);  /* Cracked ice: move at fast speed */
            else if (PlayerIsMovingOnRockStairs(direction))
                PlayerWalkFast(direction);  /* Rock stairs: fast movement */
            else
                PlayerRideWaterCurrent(direction);  /* Normal move or water current */
        }
    }
}

/**
 * FUNCTION: BikeTransition_Downhill
 *
 * PURPOSE: Automatically moves the bike downhill (south) on Cycling Road slopes.
 * Only moves if the path is clear or if there's a ledge to jump.
 *
 * @param v — unused direction parameter
 */
static void BikeTransition_Downhill(u8 v)
{
    u8 collision = GetBikeCollision(DIR_SOUTH);

    if (collision == COLLISION_NONE)
        PlayerWalkFaster(DIR_SOUTH);       /* Move south at increased speed */
    else if (collision == COLLISION_LEDGE_JUMP)
        PlayerJumpLedge(DIR_SOUTH);        /* Jump ledge at bottom of slope */
}

/**
 * FUNCTION: BikeTransition_Uphill
 *
 * PURPOSE: Moves the bike uphill on Cycling Road slopes at normal (slower) speed.
 * Only moves if the path is clear.
 *
 * @param direction — direction to move (typically DIR_NORTH on slopes)
 */
static void BikeTransition_Uphill(u8 direction)
{
    if (GetBikeCollision(direction) == COLLISION_NONE)
        PlayerWalkNormal(direction);  /* Move at normal (slow) speed uphill */
}

/* ========================================================================
 * COLLISION DETECTION
 * ======================================================================== */

/**
 * FUNCTION: GetBikeCollision
 *
 * PURPOSE: Checks what collision (if any) exists one tile ahead of the player
 * in the given direction.
 *
 * @param direction — direction to check
 * @return Collision type (COLLISION_* constant)
 */
static u8 GetBikeCollision(u8 direction)
{
    struct ObjectEvent *playerObjEvent = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 x, y;
    u8 metatileBehavior;

    x = playerObjEvent->currentCoords.x;
    y = playerObjEvent->currentCoords.y;
    MoveCoords(direction, &x, &y);  /* Calculate destination coordinates */
    metatileBehavior = MapGridGetMetatileBehaviorAt(x, y);
    return GetBikeCollisionAt(playerObjEvent, x, y, direction, metatileBehavior);
}

/**
 * FUNCTION: GetBikeCollisionAt
 *
 * PURPOSE: Evaluates the collision at a specific destination tile, with special
 * handling for bike-specific tile types.
 *
 * HOW IT WORKS:
 * Calls the general collision check first, then applies bike-specific rules:
 * - Cracked ice tiles return COLLISION_COUNT as a special signal to use fast movement
 * - Tiles that forbid biking (like certain indoor tiles) return COLLISION_IMPASSABLE
 *
 * @param playerObjEvent — the player's object event
 * @param x, y          — destination coordinates
 * @param direction     — movement direction
 * @param metatileBehavior — behavior of the destination tile
 * @return Collision type
 */
static u8 GetBikeCollisionAt(struct ObjectEvent *playerObjEvent, s16 x, s16 y, u8 direction, u8 metatileBehavior)
{
    u8 retVal = CheckForObjectEventCollision(playerObjEvent, x, y, direction, metatileBehavior);

    if (retVal <= COLLISION_OBJECT_EVENT)
    {
        /* Check for bike-specific tile interactions */
        bool8 isCrackedIce = MetatileBehavior_IsCrackedIce(metatileBehavior);
        if (isCrackedIce == TRUE)
            return COLLISION_COUNT;  /* Special: cracked ice allows fast movement */
        if (retVal == COLLISION_NONE && MetatileBehaviorForbidsBiking(metatileBehavior))
            retVal = COLLISION_IMPASSABLE;  /* Can't bike on this tile */
    }
    return retVal;
}

/* ========================================================================
 * MOVEMENT PERMISSION CHECKS
 * ======================================================================== */

/**
 * FUNCTION: RS_IsRunningDisallowed
 *
 * PURPOSE: (Ruby/Sapphire version) Checks if running is disallowed on the
 * current tile. In RS, running was always disabled indoors.
 *
 * @param r0 — metatile behavior of the current tile
 * @return TRUE if running is not allowed
 */
bool8 RS_IsRunningDisallowed(u8 r0)
{
    if (MetatileBehaviorForbidsBiking(r0))
        return TRUE;
    if (gMapHeader.mapType != MAP_TYPE_INDOOR)
        return FALSE;
    else
        return TRUE;
}

/**
 * FUNCTION: IsRunningDisallowed
 *
 * PURPOSE: Checks if running shoes are disabled on the current map/tile.
 * In FireRed, maps have an explicit allowRunning flag in their header.
 *
 * @param metatileBehavior — behavior of the current tile
 * @return TRUE if running is not allowed
 */
bool32 IsRunningDisallowed(u8 metatileBehavior)
{
    if (!gMapHeader.allowRunning)
        return TRUE;  /* Map doesn't allow running at all */
    if (MetatileBehaviorForbidsBiking(metatileBehavior) != TRUE)
        return FALSE;  /* Tile allows running */
    else
        return TRUE;   /* Tile-level restriction */
}

/**
 * FUNCTION: MetatileBehaviorForbidsBiking
 *
 * PURPOSE: Checks if a specific metatile behavior prevents biking/running.
 *
 * HOW IT WORKS:
 * Returns TRUE if:
 * 1. The tile explicitly disallows running (MetatileBehavior_IsRunningDisallowed)
 * 2. The tile is a Fortree Bridge AND the player is on the wrong elevation
 *    (Fortree Bridge is a multi-level tile — you can bike on top but not below)
 *
 * @param metatileBehavior — the tile's behavior value
 * @return TRUE if biking is forbidden
 */
static bool8 MetatileBehaviorForbidsBiking(u8 metatileBehavior)
{
    if (MetatileBehavior_IsRunningDisallowed(metatileBehavior))
        return TRUE;
    if (!MetatileBehavior_IsFortreeBridge(metatileBehavior))
        return FALSE;
    /* On Fortree Bridge: only allow biking if on odd elevation (top level) */
    if (PlayerGetElevation() & 1)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: CanBikeFaceDirectionOnRail
 *
 * PURPOSE: Checks if the bike can turn to face a direction while on a rail tile.
 * Rails restrict movement to one axis (horizontal or vertical).
 *
 * HOW IT WORKS:
 * - On a vertical rail: can't face east or west
 * - On a horizontal rail: can't face north or south
 *
 * @param direction       — desired facing direction
 * @param metatileBehavior — current tile behavior
 * @return TRUE if the direction is allowed, FALSE if the rail blocks it
 */
static bool8 CanBikeFaceDirectionOnRail(u8 direction, u8 metatileBehavior)
{
    if (direction == DIR_EAST || direction == DIR_WEST)
    {
        /* Trying to go horizontal: blocked by vertical rails */
        if (MetatileBehavior_IsIsolatedVerticalRail(metatileBehavior) || MetatileBehavior_IsVerticalRail(metatileBehavior))
            return FALSE;
    }
    else
    {
        /* Trying to go vertical: blocked by horizontal rails */
        if (MetatileBehavior_IsIsolatedHorizontalRail(metatileBehavior) || MetatileBehavior_IsHorizontalRail(metatileBehavior))
            return FALSE;
    }
    return TRUE;
}

/**
 * FUNCTION: IsBikingDisallowedByPlayer
 *
 * PURPOSE: Checks if the player's current state prevents them from getting on
 * or riding a bike (e.g., surfing, underwater, or on a no-bike tile).
 *
 * @return TRUE if biking is currently not allowed
 */
bool8 IsBikingDisallowedByPlayer(void)
{
    s16 x, y;
    u8 metatileBehavior;

    /* Can't bike while surfing or underwater */
    if (!(gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_UNDERWATER | PLAYER_AVATAR_FLAG_SURFING)))
    {
        PlayerGetDestCoords(&x, &y);
        metatileBehavior = MapGridGetMetatileBehaviorAt(x, y);
        if (!MetatileBehaviorForbidsBiking(metatileBehavior))
            return FALSE;  /* Biking is allowed */
    }
    return TRUE;  /* Biking is not allowed */
}

/**
 * FUNCTION: IsPlayerNotUsingAcroBikeOnBumpySlope
 *
 * PURPOSE: Returns TRUE if the player is NOT currently on an Acro Bike on a
 * bumpy slope. Returns FALSE if they ARE on a bumpy slope with an Acro Bike
 * (meaning the bumpy slope jump mechanic should activate).
 *
 * @return FALSE if acro bike + bumpy slope (trigger jump), TRUE otherwise
 */
bool8 IsPlayerNotUsingAcroBikeOnBumpySlope(void)
{
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_ACRO_BIKE))
    {
        if (MetatileBehavior_IsBumpySlope(gObjectEvents[gPlayerAvatar.objectEventId].currentMetatileBehavior))
            return FALSE;  /* On acro bike AND on bumpy slope */
    }
    return TRUE;
}

/* ========================================================================
 * BIKE STATE MANAGEMENT
 * ======================================================================== */

/**
 * FUNCTION: GetOnOffBike
 *
 * PURPOSE: Toggles the bike on or off. Handles the transition between walking
 * and biking states, including music changes.
 *
 * GAME LOGIC:
 * - Getting OFF the bike: Switch to on-foot mode, clear saved music, play
 *   the map's normal music
 * - Getting ON the bike: Switch to bike mode, start playing the cycling music
 *   (MUS_CYCLING) if it can override the current map music
 *
 * @param flags — the bike type flag to set (PLAYER_AVATAR_FLAG_MACH_BIKE or
 *                PLAYER_AVATAR_FLAG_ACRO_BIKE) when getting ON the bike
 */
void GetOnOffBike(u8 flags)
{
    gBikeCameraAheadPanback = FALSE;

    if (gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE))
    {
        /* Currently on bike: get off */
        SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT);
        Overworld_ClearSavedMusic();
        Overworld_PlaySpecialMapMusic();  /* Restore normal map music */
    }
    else
    {
        /* Currently on foot: get on bike */
        SetPlayerAvatarTransitionFlags(flags);
        if (Overworld_MusicCanOverrideMapMusic(MUS_CYCLING))
        {
            Overworld_SetSavedMusic(MUS_CYCLING);
            Overworld_ChangeMusicTo(MUS_CYCLING);  /* Play cycling music */
        }
    }
}

/**
 * FUNCTION: BikeClearState
 *
 * PURPOSE: Resets all bike state variables to their defaults. Called when the
 * player gets on the bike, enters a new map, or otherwise needs a clean state.
 *
 * @param directionHistory      — initial direction history value
 * @param abStartSelectHistory  — initial button history value
 */
void BikeClearState(u32 directionHistory, u32 abStartSelectHistory)
{
    u8 i;

    gPlayerAvatar.acroBikeState = BIKE_STATE_NORMAL;
    gPlayerAvatar.newDirBackup = 0;
    gPlayerAvatar.bikeFrameCounter = 0;
    gPlayerAvatar.bikeSpeed = PLAYER_SPEED_STANDING;
    gPlayerAvatar.directionHistory = directionHistory;
    gPlayerAvatar.abStartSelectHistory = abStartSelectHistory;
    gPlayerAvatar.lastSpinTile = 0;
    for (i = 0; i < NELEMS(gPlayerAvatar.dirTimerHistory); ++i)
            gPlayerAvatar.dirTimerHistory[i] = 0;
}

/**
 * FUNCTION: Bike_UpdateBikeCounterSpeed
 *
 * PURPOSE: Updates the bike's frame counter and calculates the current speed.
 * The Mach Bike's speed increases as the frame counter grows (acceleration).
 *
 * @param counter — the new frame counter value
 */
void Bike_UpdateBikeCounterSpeed(u8 counter)
{
    gPlayerAvatar.bikeFrameCounter = counter;
    gPlayerAvatar.bikeSpeed = counter + (gPlayerAvatar.bikeFrameCounter >> 1);
}

/**
 * FUNCTION: Bike_SetBikeStill
 *
 * PURPOSE: Resets the bike to standing still (zero speed, zero frame counter).
 */
static void Bike_SetBikeStill(void)
{
    gPlayerAvatar.bikeFrameCounter = 0;
    gPlayerAvatar.bikeSpeed = PLAYER_SPEED_STANDING;
}

/**
 * FUNCTION: GetPlayerSpeed
 *
 * PURPOSE: Returns the player's current movement speed based on their travel mode.
 *
 * SPEED HIERARCHY:
 * - Mach Bike: Variable speed (NORMAL -> FAST -> FASTEST) based on frame counter
 * - Acro Bike: Fixed at FASTER speed
 * - Surfing or Dashing: FAST speed
 * - Walking: NORMAL speed
 *
 * @return Player's current speed constant (PLAYER_SPEED_*)
 */
s16 GetPlayerSpeed(void)
{
    s16 machBikeSpeeds[] = { PLAYER_SPEED_NORMAL, PLAYER_SPEED_FAST, PLAYER_SPEED_FASTEST };

    if (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_MACH_BIKE)
        return machBikeSpeeds[gPlayerAvatar.bikeFrameCounter];
    else if (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_ACRO_BIKE)
        return PLAYER_SPEED_FASTER;
    else if (gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_DASH))
        return PLAYER_SPEED_FAST;
    else
        return PLAYER_SPEED_NORMAL;
}

/**
 * FUNCTION: Bike_HandleBumpySlopeJump
 *
 * PURPOSE: Handles the Acro Bike's automatic jump on bumpy slope tiles.
 * When riding the Acro Bike over a bumpy slope, the player bounces/hops
 * automatically.
 *
 * GAME LOGIC:
 * Bumpy slopes are special tiles (like the slopes near Mauville City in RS)
 * where the Acro Bike automatically performs small hops. This creates a
 * unique traversal mechanic for certain areas.
 */
void Bike_HandleBumpySlopeJump(void)
{
    s16 x, y;
    u8 tileBehavior;

    if (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_ACRO_BIKE)
    {
        PlayerGetDestCoords(&x, &y);
        tileBehavior = MapGridGetMetatileBehaviorAt(x, y);
        if (MetatileBehavior_IsBumpySlope(tileBehavior))
        {
            gPlayerAvatar.acroBikeState = BIKE_STATE_SLOPE;
            PlayerUseAcroBikeOnBumpySlope(GetPlayerMovementDirection());
        }
    }
}

/* ========================================================================
 * ACRO BIKE TRICK SYSTEM (Ruby/Sapphire leftover)
 * ========================================================================
 * This data defines the input combinations for Acro Bike tricks.
 * In the final game, only the "bunny hop" (B + direction) is used.
 * The extensible struct suggests Game Freak may have planned more complex
 * trick combos that were cut during development.
 */

/*
 * The BikeHistoryInputInfo struct stores the requirements for triggering
 * an Acro Bike trick. The game records recent input history (last few frames
 * of direction and button presses) and checks if they match a trick pattern.
 */
struct BikeHistoryInputInfo
{
    u32 dirHistoryMatch;            /* Direction that must appear in history */
    u32 abStartSelectHistoryMatch;  /* Button that must appear in history */
    u32 dirHistoryMask;             /* Mask to isolate the most recent direction entry */
    u32 abStartSelectHistoryMask;   /* Mask to isolate the most recent button entry */
    const u8 *dirTimerHistoryList;  /* Timing requirements for the direction input */
    const u8 *abStartSelectHistoryList; /* Timing requirements for the button input */
    u32 direction;                  /* Direction of the resulting trick */
};

/* Timer list for trick input validation. The input must be held for at least 4 frames. */
static const u8 sAcroBikeJumpTimerList[] = {4, 0};

/*
 * Table of all Acro Bike tricks. Each entry requires pressing B + a direction.
 * The 0xF mask ensures only the most recent input frame is checked (lowest nibble
 * of the history byte). There's one entry for each of the 4 cardinal directions.
 */
static const struct BikeHistoryInputInfo sAcroBikeTricksList[] =
{
    {DIR_SOUTH, B_BUTTON, 0xF, 0xF, sAcroBikeJumpTimerList, sAcroBikeJumpTimerList, DIR_SOUTH},
    {DIR_NORTH, B_BUTTON, 0xF, 0xF, sAcroBikeJumpTimerList, sAcroBikeJumpTimerList, DIR_NORTH},
    {DIR_WEST, B_BUTTON, 0xF, 0xF, sAcroBikeJumpTimerList, sAcroBikeJumpTimerList, DIR_WEST},
    {DIR_EAST, B_BUTTON, 0xF, 0xF, sAcroBikeJumpTimerList, sAcroBikeJumpTimerList, DIR_EAST},
};
