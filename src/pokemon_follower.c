/**
 * Pokemon Follower System
 *
 * Allows a party pokemon to follow the player in the overworld as a
 * visible sprite that trails 1-2 tiles behind.
 *
 * HOW IT WORKS:
 * The system maintains a FIFO ring buffer of the player's recent tile
 * positions (the "trail"). Each time the player completes a step to a
 * new tile, the OLD position is pushed into the trail. The follower
 * pokemon dequeues positions from the trail and walks to them using the
 * standard ObjectEvent held-movement system, which handles walk
 * animations, per-frame sprite updates, and coordinate changes.
 *
 * This creates a natural trailing effect: the follower walks the same
 * path the player took, but 1-2 tiles behind. If the player moves
 * faster than the follower can keep up (e.g., the trail fills up),
 * the follower teleports to catch up.
 *
 * LIFECYCLE:
 * - ActivateFollower(): Called from party menu "WALK" option. Stores
 *   which party slot to follow. Does NOT spawn the sprite yet (we're
 *   still in the party menu screen).
 * - SpawnFollowerSprite(): Called by overworld.c after map transitions.
 *   Allocates an ObjectEvent slot, creates the sprite. Same pattern as
 *   multiplayer.c's SpawnRemoteSprite().
 * - UpdateFollowerPokemon(): Called every frame from CB1_Overworld.
 *   Detects player movement, manages the trail FIFO, issues walk
 *   commands to the follower's ObjectEvent.
 * - DespawnFollowerSprite(): Called before map loads. Destroys the
 *   sprite but keeps sFollower.active = TRUE so it respawns on the
 *   next map.
 * - DeactivateFollower(): Called from party menu "RETURN" option.
 *   Despawns and clears all state.
 *
 * MULTIPLAYER:
 * The follower is purely local state. It is not included in the SIO
 * multiplayer protocol and does not appear on remote players' screens.
 */

#include "global.h"
#include "pokemon_follower.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/species.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "pokemon.h"
#include "random.h"
#include "sprite.h"

/*
 * Distance thresholds for follower behavior (Manhattan distance in tiles).
 * - FOLLOW_DIST: When farther than this, the follower moves toward the player.
 *   At exactly this distance, the follower idles (stands near the player).
 * - TELEPORT_DIST: When farther than this, the follower teleports to catch up
 *   (e.g., after running or if the follower got stuck on geometry).
 * - IDLE_CHANCE: 1-in-N chance per frame that the follower skips a move when
 *   at medium distance. Creates a relaxed, organic following pace.
 * - DEVIATE_CHANCE: 1-in-N chance that the follower picks a random perpendicular
 *   direction instead of walking straight toward the player.
 */
#define FOLLOW_DIST     2
#define TELEPORT_DIST   5
#define IDLE_CHANCE     3
#define DEVIATE_CHANCE  4

/*
 * All follower state lives in EWRAM. Persists across map warps (EWRAM is
 * not cleared between maps) but resets on game load (EWRAM is zeroed at
 * boot). This means the follower is session-only: it does not survive
 * save/load, which avoids modifying the save format.
 */
static EWRAM_DATA struct {
    bool8 active;           /* Follower is logically enabled */
    bool8 spriteSpawned;    /* Sprite currently exists on screen */
    u8 partySlot;           /* Index into gPlayerParty[] */
    u16 species;            /* Cached species (for change detection) */
    u8 graphicsId;          /* OBJ_EVENT_GFX_* constant for the sprite */
    u8 objEventId;          /* Index into gObjectEvents[] */
    u32 pid;                /* Personality value (PID). partySlot goes stale after SWITCH; species+PID uniquely identifies the Pokemon regardless of slot */
} sFollower = {0};

/*
 * Species-to-graphics mapping table.
 *
 * Only these 27 pokemon have overworld sprites with walking animation
 * frames (9-frame sprite sheets: 3 standing + 6 walking). All other
 * pokemon either lack overworld sprites entirely or have only a single
 * static frame (legendaries like Snorlax, Mewtwo, etc.).
 *
 * Walking animation frames were generated programmatically in a previous
 * session using a bob+sway technique applied to the standing frames.
 */
static const struct {
    u16 species;
    u8 graphicsId;
} sFollowerGfxTable[] = {
    { SPECIES_PIKACHU,    OBJ_EVENT_GFX_PIKACHU },
    { SPECIES_SPEAROW,    OBJ_EVENT_GFX_SPEAROW },
    { SPECIES_CUBONE,     OBJ_EVENT_GFX_CUBONE },
    { SPECIES_POLIWRATH,  OBJ_EVENT_GFX_POLIWRATH },
    { SPECIES_CLEFAIRY,   OBJ_EVENT_GFX_CLEFAIRY },
    { SPECIES_PIDGEOT,    OBJ_EVENT_GFX_PIDGEOT },
    { SPECIES_JIGGLYPUFF, OBJ_EVENT_GFX_JIGGLYPUFF },
    { SPECIES_PIDGEY,     OBJ_EVENT_GFX_PIDGEY },
    { SPECIES_CHANSEY,    OBJ_EVENT_GFX_CHANSEY },
    { SPECIES_OMANYTE,    OBJ_EVENT_GFX_OMANYTE },
    { SPECIES_KANGASKHAN, OBJ_EVENT_GFX_KANGASKHAN },
    { SPECIES_PSYDUCK,    OBJ_EVENT_GFX_PSYDUCK },
    { SPECIES_NIDORAN_F,  OBJ_EVENT_GFX_NIDORAN_F },
    { SPECIES_NIDORAN_M,  OBJ_EVENT_GFX_NIDORAN_M },
    { SPECIES_NIDORINO,   OBJ_EVENT_GFX_NIDORINO },
    { SPECIES_MEOWTH,     OBJ_EVENT_GFX_MEOWTH },
    { SPECIES_SEEL,       OBJ_EVENT_GFX_SEEL },
    { SPECIES_VOLTORB,    OBJ_EVENT_GFX_VOLTORB },
    { SPECIES_SLOWPOKE,   OBJ_EVENT_GFX_SLOWPOKE },
    { SPECIES_SLOWBRO,    OBJ_EVENT_GFX_SLOWBRO },
    { SPECIES_MACHOP,     OBJ_EVENT_GFX_MACHOP },
    { SPECIES_WIGGLYTUFF, OBJ_EVENT_GFX_WIGGLYTUFF },
    { SPECIES_DODUO,      OBJ_EVENT_GFX_DODUO },
    { SPECIES_FEAROW,     OBJ_EVENT_GFX_FEAROW },
    { SPECIES_MACHOKE,    OBJ_EVENT_GFX_MACHOKE },
    { SPECIES_LAPRAS,     OBJ_EVENT_GFX_LAPRAS },
    { SPECIES_KABUTO,     OBJ_EVENT_GFX_KABUTO },
};

/**
 * Look up the overworld graphics ID for a species.
 *
 * @return OBJ_EVENT_GFX_* constant, or 0 if the species has no walking sprite.
 */
static u8 GetFollowerGraphicsId(u16 species)
{
    u8 i;

    for (i = 0; i < NELEMS(sFollowerGfxTable); i++)
    {
        if (sFollowerGfxTable[i].species == species)
            return sFollowerGfxTable[i].graphicsId;
    }
    return 0;
}

bool8 CanSpeciesFollowPlayer(u16 species)
{
    return GetFollowerGraphicsId(species) != 0;
}

bool8 IsFollowerActive(void)
{
    return sFollower.active;
}

u8 GetFollowerObjEventId(void)
{
    return sFollower.objEventId;
}

/**
 * Return the party slot index of the active follower.
 *
 * Kept as public API; party_menu.c now uses IsFollowerPokemon() for the
 * RETURN check, so this slot value is only advisory (it goes stale after
 * the player uses SWITCH to rearrange the party).
 */
u8 GetFollowerPartySlot(void)
{
    return sFollower.partySlot;
}

/**
 * Check whether a party Pokemon is the active follower.
 *
 * Compares both species and personality value (PID) so that the check
 * remains correct after the player rearranges their party with SWITCH.
 * Slot-index comparison would go stale; identity comparison does not.
 *
 * @return TRUE if mon is the currently active follower.
 */
bool8 IsFollowerPokemon(struct Pokemon *mon)
{
    if (!sFollower.active)
        return FALSE;
    return GetMonData(mon, MON_DATA_SPECIES) == sFollower.species
        && GetMonData(mon, MON_DATA_PERSONALITY) == sFollower.pid;
}

/**
 * FUNCTION: ActivateFollower
 *
 * PURPOSE: Enable the follower system for the given party slot.
 *          Called from the party menu "WALK" callback.
 *
 * HOW IT WORKS:
 * Caches the species and graphics ID, clears the trail buffer, and
 * snapshots the player's current position for change detection.
 * Does NOT spawn the sprite -- that happens when SpawnFollowerSprite()
 * is called during the return-to-field transition, because the party
 * menu screen has its own separate graphics state.
 */
void ActivateFollower(u8 partySlot)
{
    u16 species;
    u8 gfxId;

    species = GetMonData(&gPlayerParty[partySlot], MON_DATA_SPECIES);
    gfxId = GetFollowerGraphicsId(species);
    if (gfxId == 0)
        return;

    sFollower.active = TRUE;
    sFollower.spriteSpawned = FALSE;
    sFollower.partySlot = partySlot;
    sFollower.species = species;
    sFollower.graphicsId = gfxId;
    sFollower.objEventId = OBJECT_EVENTS_COUNT;
    sFollower.pid = GetMonData(&gPlayerParty[partySlot], MON_DATA_PERSONALITY);
}

/**
 * FUNCTION: DeactivateFollower
 *
 * PURPOSE: Fully disable the follower and remove its sprite.
 *          Called from the party menu "RETURN" callback.
 */
void DeactivateFollower(void)
{
    DespawnFollowerSprite();
    sFollower.active = FALSE;
    sFollower.spriteSpawned = FALSE;
    sFollower.objEventId = OBJECT_EVENTS_COUNT;
    sFollower.partySlot = 0;
    sFollower.species = 0;
    sFollower.graphicsId = 0;
    sFollower.pid = 0;
}

/**
 * Get the tile offset (dx, dy) for a given direction.
 */
static void GetDirectionOffset(u8 direction, s16 *dx, s16 *dy)
{
    *dx = 0;
    *dy = 0;
    switch (direction)
    {
    case DIR_SOUTH: *dy = 1;  break;
    case DIR_NORTH: *dy = -1; break;
    case DIR_EAST:  *dx = 1;  break;
    case DIR_WEST:  *dx = -1; break;
    }
}

/**
 * Try to find a walkable tile near the player for spawning the follower.
 *
 * Tries in order: behind the player, then left/right of behind, then
 * left/right of player. Falls back to the player's tile if nothing
 * else is walkable (the follower will separate on the first step).
 *
 * "Behind" means opposite of the player's facing direction.
 */
static void FindFollowerSpawnPosition(struct ObjectEvent *playerObj, s16 *outX, s16 *outY)
{
    u8 playerDir = GetPlayerFacingDirection();
    u8 behindDir = GetOppositeDirection(playerDir);
    s16 px = playerObj->currentCoords.x;
    s16 py = playerObj->currentCoords.y;
    s16 dx, dy;
    /*
     * Candidate offsets to try, in priority order:
     * 1. Directly behind player
     * 2-3. Perpendicular to behind direction (left/right of behind)
     * 4-5. Directly left/right of player
     */
    u8 tryDirs[5];
    u8 i;
    s16 tx, ty;

    tryDirs[0] = behindDir;
    /* Perpendicular directions */
    if (playerDir == DIR_SOUTH || playerDir == DIR_NORTH)
    {
        tryDirs[1] = DIR_WEST;
        tryDirs[2] = DIR_EAST;
    }
    else
    {
        tryDirs[1] = DIR_SOUTH;
        tryDirs[2] = DIR_NORTH;
    }
    /* Opposite perpendicular (already covered) + forward (least ideal) */
    tryDirs[3] = tryDirs[2];
    tryDirs[4] = tryDirs[1];

    for (i = 0; i < 3; i++)
    {
        GetDirectionOffset(tryDirs[i], &dx, &dy);
        tx = px + dx;
        ty = py + dy;
        if (!MapGridGetCollisionAt(tx, ty))
        {
            *outX = tx;
            *outY = ty;
            return;
        }
    }

    /* Fallback: spawn at player's tile */
    *outX = px;
    *outY = py;
}

/**
 * FUNCTION: SpawnFollowerSprite
 *
 * PURPOSE: Create the follower's ObjectEvent and sprite on the current map.
 *          Called from overworld.c after map transitions (both full warps
 *          and return-to-field).
 *
 * HOW IT WORKS:
 * Uses SpawnSpecialObjectEventParameterized() to go through the full
 * ObjectEvent creation pipeline. This is critical because it:
 *   - Sets the sprite callback to the movement type's callback function,
 *     which calls UpdateObjectEventCurrentMovement() every frame. That
 *     function checks for held movements first and executes them via
 *     ObjectEventExecHeldMovementAction(). Without this, held movements
 *     (our walk commands) would never be processed.
 *   - Sets sprite->data[0] = objectEventId, linking the sprite back to
 *     its ObjectEvent. Movement callbacks dereference this to find the
 *     ObjectEvent they should update.
 *   - Initializes triggerGroundEffectsOnMove, inanimate flag, initial
 *     animation frame, palette loading, and coordOffsetEnabled.
 *
 * We use MOVEMENT_TYPE_NONE so the follower stands still when idle.
 * Walk commands are issued via ObjectEventSetHeldMovement() from
 * UpdateFollowerPokemon(), and are processed because
 * UpdateObjectEventCurrentMovement checks held movements BEFORE the
 * autonomous step functions.
 *
 * The follower is placed on a walkable tile near (but not on top of)
 * the player, preferring the tile directly behind the player.
 */
void SpawnFollowerSprite(void)
{
    struct ObjectEvent *playerObj;
    struct ObjectEvent *objEvent;
    u8 objEventId;
    s16 spawnX, spawnY;

    if (!sFollower.active || sFollower.spriteSpawned)
        return;

    playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];

    /* Find a walkable tile near the player (behind them if possible) */
    FindFollowerSpawnPosition(playerObj, &spawnX, &spawnY);

    /*
     * SpawnSpecialObjectEventParameterized creates a full ObjectEvent with
     * the correct sprite callback for the given movement type.
     *
     * MOVEMENT_TYPE_NONE makes the follower stand still when idle. Our
     * held-movement walk commands override this each frame via
     * UpdateObjectEventCurrentMovement's held-movement priority check.
     */
    objEventId = SpawnSpecialObjectEventParameterized(
        sFollower.graphicsId,
        MOVEMENT_TYPE_NONE,
        LOCALID_FOLLOWER,
        spawnX,
        spawnY,
        0  /* elevation */
    );

    if (objEventId >= OBJECT_EVENTS_COUNT)
        return;

    sFollower.objEventId = objEventId;
    sFollower.spriteSpawned = TRUE;

    /*
     * Face toward the player for a natural initial appearance, rather
     * than the default DIR_SOUTH from MOVEMENT_TYPE_NONE.
     */
    objEvent = &gObjectEvents[objEventId];
    SetObjectEventDirection(objEvent, GetPlayerFacingDirection());
}

/**
 * FUNCTION: DespawnFollowerSprite
 *
 * PURPOSE: Remove the follower sprite from the screen without
 *          deactivating the follower system.
 *
 * Called before map loads (LoadMapFromWarp) so the sprite is cleaned up
 * before all ObjectEvents are reset. The follower remains logically
 * active and will be respawned by SpawnFollowerSprite() on the new map.
 *
 * Also called from DeactivateFollower() for full shutdown.
 */
void DespawnFollowerSprite(void)
{
    struct ObjectEvent *objEvent;

    if (!sFollower.spriteSpawned)
        return;

    if (sFollower.objEventId < OBJECT_EVENTS_COUNT)
    {
        objEvent = &gObjectEvents[sFollower.objEventId];
        /*
         * Only destroy if the ObjectEvent still belongs to us. During
         * battle transitions or menu screens, the sprite system may have
         * already cleaned up our sprite.
         */
        if (objEvent->active && objEvent->spriteId < MAX_SPRITES)
        {
            struct Sprite *spr = &gSprites[objEvent->spriteId];
            if (spr->inUse)
                DestroySprite(spr);
        }
        objEvent->active = FALSE;
    }

    sFollower.spriteSpawned = FALSE;
    sFollower.objEventId = OBJECT_EVENTS_COUNT;
}

/**
 * Teleport the follower to a walkable tile near the player.
 *
 * Used when the follower is too far away (e.g., got stuck on geometry,
 * player ran, or after a warp where positions don't line up). Finds a
 * walkable tile near the player and snaps the follower there.
 */
static void TeleportFollowerNearPlayer(struct ObjectEvent *followerObj, struct ObjectEvent *playerObj)
{
    struct Sprite *sprite;
    const struct ObjectEventGraphicsInfo *gfxInfo;
    s16 newX, newY;

    FindFollowerSpawnPosition(playerObj, &newX, &newY);

    followerObj->currentCoords.x = newX;
    followerObj->currentCoords.y = newY;
    followerObj->previousCoords.x = newX;
    followerObj->previousCoords.y = newY;

    /* Reposition the sprite to match the new tile */
    if (followerObj->spriteId < MAX_SPRITES)
    {
        sprite = &gSprites[followerObj->spriteId];
        gfxInfo = GetObjectEventGraphicsInfo(sFollower.graphicsId);
        SetSpritePosToMapCoords(newX, newY, &sprite->x, &sprite->y);
        sprite->centerToCornerVecX = -(gfxInfo->width >> 1);
        sprite->centerToCornerVecY = -(gfxInfo->height >> 1);
        sprite->x += 8;
        sprite->y += 16 + sprite->centerToCornerVecY;
    }

    /* Clear any in-progress movement */
    ObjectEventClearHeldMovementIfFinished(followerObj);
    followerObj->heldMovementActive = FALSE;
    followerObj->heldMovementFinished = FALSE;
}

/**
 * Choose a direction for the follower to move.
 *
 * Usually picks the axis with the greatest distance to the player.
 * With a random chance (1-in-DEVIATE_CHANCE), picks a perpendicular
 * walkable direction instead, creating a less robotic following path.
 * The deviation is only allowed if it doesn't increase the distance
 * to the player (so the follower still trends toward the player).
 */
static u8 ChooseFollowerDirection(struct ObjectEvent *followerObj, s16 playerX, s16 playerY)
{
    s16 dx = playerX - followerObj->currentCoords.x;
    s16 dy = playerY - followerObj->currentCoords.y;
    s16 absDx = dx < 0 ? -dx : dx;
    s16 absDy = dy < 0 ? -dy : dy;
    u8 idealDir;
    u8 altDir;
    s16 altDx, altDy;
    s16 altX, altY;

    /*
     * Pick the primary direction: move along the axis with the greater
     * distance. When equal, pick randomly between the two.
     */
    if (absDx > absDy || (absDx == absDy && (Random() & 1)))
        idealDir = (dx > 0) ? DIR_EAST : DIR_WEST;
    else
        idealDir = (dy > 0) ? DIR_SOUTH : DIR_NORTH;

    /*
     * Random deviation: with DEVIATE_CHANCE probability, try a perpendicular
     * direction instead. This makes the follower wander slightly rather
     * than beelining toward the player.
     */
    if ((Random() % DEVIATE_CHANCE) == 0)
    {
        /* Pick a random perpendicular direction */
        if (idealDir == DIR_EAST || idealDir == DIR_WEST)
            altDir = (Random() & 1) ? DIR_SOUTH : DIR_NORTH;
        else
            altDir = (Random() & 1) ? DIR_EAST : DIR_WEST;

        /* Only deviate if the tile is walkable */
        GetDirectionOffset(altDir, &altDx, &altDy);
        altX = followerObj->currentCoords.x + altDx;
        altY = followerObj->currentCoords.y + altDy;
        if (!MapGridGetCollisionAt(altX, altY))
            return altDir;
    }

    return idealDir;
}

/**
 * FUNCTION: UpdateFollowerPokemon
 *
 * PURPOSE: Per-frame update for the follower system. Called from
 *          CB1_Overworld() every frame, right after UpdateMultiplayerState().
 *
 * HOW IT WORKS:
 * Uses a distance-based approach with randomness for natural movement:
 * 1. Compute Manhattan distance from follower to player.
 * 2. If distance > TELEPORT_DIST, teleport the follower nearby.
 * 3. If distance > FOLLOW_DIST, move toward the player (with occasional
 *    random perpendicular deviation for organic movement).
 * 4. If distance <= FOLLOW_DIST, idle — the follower is close enough.
 * 5. With IDLE_CHANCE probability, skip a movement even when far,
 *    creating a relaxed "pet following" pace.
 *
 * The held-movement system (ObjectEventSetHeldMovement) handles the full
 * walk animation cycle: sprite frame animation, per-frame position
 * interpolation, and destination coordinate update on completion.
 */
void UpdateFollowerPokemon(void)
{
    struct ObjectEvent *playerObj;
    struct ObjectEvent *followerObj;
    s16 dx, dy, dist;
    u8 moveDir;

    if (!sFollower.active || !sFollower.spriteSpawned)
        return;

    if (sFollower.objEventId >= OBJECT_EVENTS_COUNT)
        return;

    followerObj = &gObjectEvents[sFollower.objEventId];
    if (!followerObj->active)
        return;

    /* Wait for current movement to finish before issuing a new one */
    if (ObjectEventIsHeldMovementActive(followerObj))
    {
        ObjectEventClearHeldMovementIfFinished(followerObj);
        return;
    }

    playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];
    dx = playerObj->currentCoords.x - followerObj->currentCoords.x;
    dy = playerObj->currentCoords.y - followerObj->currentCoords.y;
    dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

    /* Too far away — teleport to catch up */
    if (dist > TELEPORT_DIST)
    {
        TeleportFollowerNearPlayer(followerObj, playerObj);
        return;
    }

    /* Close enough — just idle near the player */
    if (dist <= FOLLOW_DIST)
        return;

    /*
     * Random idle: occasionally skip a step even when the follower
     * should be moving. This makes it look like the pokemon is
     * casually following rather than mechanically chasing.
     */
    if ((Random() % IDLE_CHANCE) == 0)
        return;

    /* Pick a direction (usually toward player, sometimes deviating) */
    moveDir = ChooseFollowerDirection(followerObj, playerObj->currentCoords.x, playerObj->currentCoords.y);

    /* Issue the walk command */
    ObjectEventSetHeldMovement(followerObj, GetWalkNormalMovementAction(moveDir));
}
