/**
 * Pokemon Follower System
 *
 * Allows a party pokemon to follow the player in the overworld as a
 * visible sprite that trails 1-2 tiles behind.
 *
 * HOW IT WORKS:
 * Every frame, UpdateFollowerPokemon() looks at how far the follower is
 * from the player (counted in tiles, east-west plus north-south) and, when
 * it should move, starts a one-tile step toward the player. Steps are
 * "held movements" of the standard ObjectEvent system, which plays the walk
 * animation and moves the sprite over the following frames.
 *   - Within FOLLOW_DIST tiles it stands still.
 *   - Up to TELEPORT_DIST tiles it follows at a relaxed pace, with random
 *     pauses and the odd step to the side.
 *   - Farther away (the player ran, cycled or surfed off faster than it
 *     walks) it is in "catch-up mode": it hurries straight toward the
 *     player, and only if it can't make a step for STUCK_FRAMES frames does
 *     it teleport next to them.
 *
 * WALKABLE TILES (issue #46):
 * A held movement moves its object without any collision check of its own,
 * so the follower checks every tile itself before stepping, spawning or
 * teleporting onto it (IsFollowerWalkableTile): the tile must pass the
 * engine's NPC collision check, hold no warp, and not be water. A blocked
 * step tries the other direction that also leads toward the player, or
 * waits for the next frame; a ledge in the step's own direction is jumped,
 * as the player does. So the follower never walks through walls, onto
 * ledges from the side, onto water or onto other characters. While the
 * player surfs, it waits on the shore and rejoins them when they land.
 *
 * SWAPPING PLACES (issue #99):
 * The follower is an object like any NPC, so the player's step code finds
 * it in the way like one. That alone would let it block a cave mouth, a door
 * or a one-tile path for good. So when the player walks, runs or rides the
 * bicycle into it, the two swap places, as in HeartGold and SoulSilver: the
 * player steps onto its tile and it steps onto the player's old tile, both
 * starting on the same frame and lasting as many frames, so they cross
 * without ever sharing a tile.
 *   - The player's step code decides it, at the point where it would
 *     otherwise bump into an object: PlayerNotOnBikeMoving in
 *     field_player_avatar.c, and the bicycle moves in bike.c. It asks
 *     CanPlayerSwapWithFollower, takes its normal step, then calls
 *     StartFollowerSwapStep. The decision is not made inside the shared
 *     collision check (CheckForObjectEventCollision) on purpose: moves the
 *     game makes by itself (ice slides, spinner tiles, sea currents) call
 *     that check too and must keep stopping in front of the follower, and
 *     the bicycle code can still refuse a step after that check (a tile
 *     that forbids biking).
 *   - Both steps are held movements (see WALKABLE TILES): each step code
 *     only stores a one-step command on its object this frame, and the
 *     sprite callbacks play both later in the same frame (AnimateSprites).
 *     So both objects move onto their new tiles on the same frame.
 *   - The follower takes a step exactly as long as the player's: see
 *     sSwapSteps, the speed table.
 *   - Only a follower standing still swaps. One in the middle of a step
 *     blocks like a walking NPC: the player bumps, and swaps on the next
 *     try once its step has ended.
 *   - No Random() call, so the random sequence tests and Mt. Moon's seed
 *     rolls depend on is untouched.
 *
 * LIFECYCLE:
 * - ActivateFollower(): Called from party menu "WALK" option. Stores
 *   the chosen Pokemon's identity (PID + OTID, see IsFollowerPokemon) so
 *   it is recognised in any party slot. Does NOT spawn the sprite yet
 *   (we're still in the party menu screen).
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
 * WHEN THE WALKING POKEMON LEAVES THE PARTY:
 * The follower's Pokemon can leave the party without RETURN: the party
 * menu's MOVE TO PC, a deposit or release in Bill's PC, the Day Care, a
 * trade. A follower whose Pokemon is gone could never be RETURNed (RETURN
 * is offered on the Pokemon itself), so the field code puts it away by
 * itself, calling DeactivateFollower() when IsFollowerInParty() says the
 * Pokemon is gone. Two places check:
 *   - SpawnFollowerSprite(), on every map load and return to the field,
 *     which covers everything done in a menu or another screen (PC, party
 *     menu, trade) before the first frame is drawn;
 *   - UpdateFollowerPokemon(), every overworld frame, which covers what
 *     happens on the field itself (the Day Care's deposit is a map script:
 *     no menu closes and no map loads afterwards).
 * The per-frame check is cheap: at most 6 Pokemon, and IsFollowerPokemon
 * reads two unencrypted fields, a few hundred CPU cycles out of the
 * roughly 280,000 in a frame.
 *
 * This is done here rather than in each of those menus because the
 * follower's sprite only exists on the field. Menus like the party menu
 * and the PC wipe the sprite table when they open and fill it with their
 * own sprites, so the sprite index saved in the follower's ObjectEvent
 * then points at one of the menu's sprites; destroying "the follower's
 * sprite" from a menu would remove that menu sprite instead.
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
#include "field_control_avatar.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "pokemon.h"
#include "random.h"
#include "sprite.h"

/*
 * Distance thresholds for follower behavior (Manhattan distance in tiles).
 * - FOLLOW_DIST: When farther than this, the follower moves toward the player.
 *   At exactly this distance, the follower idles (stands near the player).
 * - TELEPORT_DIST: When farther than this, the follower enters catch-up mode
 *   (e.g., after the player ran or cycled ahead): it walks fast straight
 *   toward the player instead of following at a relaxed pace. Despite the
 *   name it does not teleport; teleporting is only the STUCK_FRAMES fallback
 *   below. The name is kept from when this distance did trigger a teleport.
 * - IDLE_CHANCE: 1-in-N chance per frame that the follower skips a move when
 *   at medium distance. Creates a relaxed, organic following pace.
 * - DEVIATE_CHANCE: 1-in-N chance that the follower tries a random step to
 *   the side (perpendicular to the way toward the player) instead of
 *   walking straight toward the player.
 */
#define FOLLOW_DIST     2
#define TELEPORT_DIST   5
#define IDLE_CHANCE     3
#define DEVIATE_CHANCE  4

/*
 * STUCK_FRAMES: in catch-up mode, how many frames in a row the follower may
 * go without making a step before it teleports next to the player (a last
 * resort, so it is never left behind for good). 120 frames is 2 seconds at
 * the GBA's 60 frames per second.
 *
 * "Without a step" means every frame it tried and found no walkable tile
 * toward the player: a wall or a tree in the way, water between it and a
 * surfing player, a ledge from the side. The count starts again from 0
 * whenever it does step, and whenever it gets back within TELEPORT_DIST.
 * Frames in the middle of a step don't count either way: a step resets the
 * count when it starts.
 *
 * The teleport only lands on a walkable tile behind or beside the player
 * (FindFollowerTileNearPlayer). If there is none, for example while the
 * player is surfing with water all around, the follower keeps waiting and
 * tries again every frame.
 */
#define STUCK_FRAMES    120

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
    u8 stuckFrames;         /* Catch-up frames in a row with no step (see STUCK_FRAMES) */
    u16 species;            /* Species at WALK time (not identity: evolution changes it) */
    u8 graphicsId;          /* OBJ_EVENT_GFX_* constant for the sprite */
    u8 objEventId;          /* Index into gObjectEvents[] */
    /*
     * Identity of the walking Pokemon: its personality value (PID) and its
     * original trainer's ID (OTID). partySlot can't identify it, because it
     * goes stale when the player uses SWITCH; species can't either, because
     * evolution changes it. PID and OTID are both fixed when the Pokemon is
     * created and never change afterwards, and the chance of two different
     * Pokemon sharing both is about 1 in 2^64 (the game uses the same pair
     * to tell individual Pokemon apart). See IsFollowerPokemon().
     *
     * Both are u32, so the compiler places them at offsets that are
     * multiples of 4 (8 and 12 here), as the ARM CPU requires for 32-bit
     * loads; test/lib/addresses.lua lists the full layout.
     */
    u32 pid;                /* Personality value of the walking Pokemon */
    u32 otId;               /* Original trainer ID of the walking Pokemon */
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
 * The follower's ObjectEvent, or NULL when it has no sprite on this map.
 */
static struct ObjectEvent *GetFollowerObjectEvent(void)
{
    struct ObjectEvent *followerObj;

    if (!sFollower.active || !sFollower.spriteSpawned || sFollower.objEventId >= OBJECT_EVENTS_COUNT)
        return NULL;
    followerObj = &gObjectEvents[sFollower.objEventId];
    if (!followerObj->active)
        return NULL;
    return followerObj;
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
 * Compares the Pokemon's identity (PID and OTID, see the sFollower struct)
 * with the one stored by ActivateFollower(). Unlike a party slot index this
 * stays right after SWITCH, and unlike species it stays right after the
 * walking Pokemon evolves.
 *
 * Both values sit in the unencrypted header of the Pokemon's data, so
 * reading them is cheap: GetMonData only decrypts the 48-byte "secure"
 * block for fields stored inside it (species, moves, ...).
 *
 * @return TRUE if mon is the currently active follower.
 */
bool8 IsFollowerPokemon(struct Pokemon *mon)
{
    if (!sFollower.active)
        return FALSE;
    return GetMonData(mon, MON_DATA_PERSONALITY) == sFollower.pid
        && GetMonData(mon, MON_DATA_OT_ID) == sFollower.otId;
}

/**
 * Check whether the walking Pokemon is still in the player's party (in any
 * slot). See "WHEN THE WALKING POKEMON LEAVES THE PARTY" at the top of
 * this file.
 *
 * Only the first gPlayerPartyCount slots hold Pokemon; the game keeps the
 * party packed at the front of gPlayerParty (CompactPartySlots) and the
 * count up to date after every removal.
 */
static bool8 IsFollowerInParty(void)
{
    u8 i;

    for (i = 0; i < gPlayerPartyCount; i++)
    {
        if (IsFollowerPokemon(&gPlayerParty[i]))
            return TRUE;
    }
    return FALSE;
}

/**
 * FUNCTION: ActivateFollower
 *
 * PURPOSE: Enable the follower system for the given party slot.
 *          Called from the party menu "WALK" callback.
 *
 * HOW IT WORKS:
 * Caches the species and graphics ID, and stores the Pokemon's identity
 * (PID + OTID) so IsFollowerPokemon() can find it after SWITCH or
 * evolution.
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
    sFollower.otId = GetMonData(&gPlayerParty[partySlot], MON_DATA_OT_ID);
}

/**
 * FUNCTION: DeactivateFollower
 *
 * PURPOSE: Fully disable the follower and remove its sprite.
 *          Called from the party menu "RETURN" callback, and by
 *          SpawnFollowerSprite() / UpdateFollowerPokemon() when the
 *          follower's Pokemon has left the party.
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
    sFollower.otId = 0;
}

/**
 * FUNCTION: ClearFollowerFromSaveBlock
 *
 * PURPOSE: Zero out the follower's ObjectEvent entry in SaveBlock1 so
 *          it is not persisted across save/load.
 *
 * WHY THIS IS NEEDED:
 * gSaveBlock1Ptr->objectEvents[] is the on-disk copy of the overworld
 * ObjectEvent array. When the game saves, any active ObjectEvent slot
 * (including the follower's) is written to this array. On the next
 * load, EWRAM is zeroed (wiping the in-memory follower driver state),
 * but SpawnObjectEventsOnReturnToField() reads from the save block and
 * recreates every non-zero entry as a sprite -- producing a frozen
 * follower ghost with no driver.
 *
 * This function must be called just before DoSaveData() writes to flash
 * so the follower slot is clean in the snapshot that gets persisted.
 *
 * NOTE: gObjectEvents[] (EWRAM) and gSaveBlock1Ptr->objectEvents[]
 * (save) are parallel arrays sharing the same index space, so
 * sFollower.objEventId is the correct index for both.
 *
 * WHEN IT MUST LEAVE THE SAVE ALONE:
 * sFollower lives in EWRAM, and the boot code fills EWRAM with zeroes on
 * every power-on and soft reset -- so also before every Continue. Until
 * the player picks WALK, every field of sFollower is therefore 0,
 * including objEventId. Slot 0 is normally the player's own ObjectEvent,
 * and Continue rebuilds the player from the saved copy of it. Blindly
 * clearing "slot objEventId" would erase the player from the save, and
 * the next Continue would load a map with no player to control.
 *
 * So the slot is cleared only when all three of these hold:
 *   1. sFollower.spriteSpawned: a follower sprite exists right now.
 *      objEventId only names a real slot while it does (SpawnFollowerSprite
 *      sets both; DespawnFollowerSprite resets objEventId to
 *      OBJECT_EVENTS_COUNT, meaning "no slot").
 *   2. objEventId < OBJECT_EVENTS_COUNT: the index is inside the 16-entry
 *      array, so the memset cannot write past its end into the save data
 *      that follows it.
 *   3. The saved slot's localId is LOCALID_FOLLOWER (254), the id every
 *      follower ObjectEvent is created with: a last check that the entry
 *      about to be erased really is the follower, never the player or an
 *      NPC that has ended up at that index.
 */
void ClearFollowerFromSaveBlock(void)
{
    u8 id = sFollower.objEventId;

    if (!sFollower.spriteSpawned || id >= OBJECT_EVENTS_COUNT)
        return;
    if (gSaveBlock1Ptr->objectEvents[id].localId != LOCALID_FOLLOWER)
        return;

    memset(&gSaveBlock1Ptr->objectEvents[id], 0,
           sizeof(gSaveBlock1Ptr->objectEvents[0]));
}

/**
 * FUNCTION: IsFollowerWalkableTile
 *
 * PURPOSE: Decide whether the follower may stand on tile (x, y), arriving
 *          there while moving in `direction`. Every step, spawn tile and
 *          teleport tile of the follower is checked with this.
 *
 * (x, y) are object coordinates: map coordinates plus MAP_OFFSET, the
 * 7-tile margin the engine keeps around every map.
 *
 * WHY THE FOLLOWER NEEDS ITS OWN CHECK:
 * The follower moves with "held movements": a one-step command (e.g.
 * "walk east") stored on its ObjectEvent, which the sprite system plays out
 * over the next frames. Starting one moves the ObjectEvent's tile to the
 * next tile straight away, with no collision check at all
 * (InitNpcForMovement in event_object_movement.c). NPCs that wander on their
 * own check first; the follower has to do the same before every step.
 *
 * A tile is walkable when all three of these hold:
 *   1. GetCollisionAtCoords() returns COLLISION_NONE. This is the engine's
 *      own check for wandering NPCs. It refuses the tile when:
 *        - its collision bits are set (walls, trees, ledges: a ledge is
 *          solid, only a jump crosses it, see GetFollowerStepAction);
 *        - it is past the edge of the map with no connected map there;
 *        - it is one-way (MB_IMPASSABLE_* behaviours) from this direction;
 *        - its elevation (height level, 1 for water, 3 for most ground,
 *          4 and up for bridges) differs from the follower's;
 *        - an object stands on it or is walking onto or off it: an NPC, an
 *          item ball, a roaming Pokemon or the player.
 *   2. No warp event sits on it. A building door is solid anyway, but
 *      walk-on warps such as cave mouths (MB_CAVE_DOOR) and arrow warps
 *      have collision 0, and a held movement never triggers a warp, so the
 *      follower would end up standing on one.
 *   3. It is not water (MetatileBehavior_IsSurfable). Check 1 usually
 *      refuses water already, because water has elevation 1 and land 3.
 *      But the engine skips the height test while an object's elevation is
 *      0, and the follower's is 0 just after it spawns and while it stands
 *      on a ledge's landing or a door tile (elevation-0 tiles). So water is
 *      refused by its tile type too.
 *
 * `mover` is the ObjectEvent being moved: the follower, or a stand-in for
 * it when choosing a spawn or teleport tile (see FindFollowerTileNearPlayer).
 * The collision check reads its elevation and the behaviour of the tile it
 * stands on, and skips it when looking for objects in the way.
 */
static bool8 IsFollowerWalkableTile(struct ObjectEvent *mover, s16 x, s16 y, u8 direction)
{
    if (GetCollisionAtCoords(mover, x, y, direction) != COLLISION_NONE)
        return FALSE;
    if (IsWarpEventAt(x, y))
        return FALSE;
    if (MetatileBehavior_IsSurfable(MapGridGetMetatileBehaviorAt(x, y)))
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: GetFollowerStepAction
 *
 * PURPOSE: Find the held movement that takes the follower one step in
 *          `direction`, or MOVEMENT_ACTION_NONE if it can't go that way.
 *
 * - The next tile is walkable: walk there. Normal walk (16 frames per tile)
 *   when following at a relaxed pace; walk fast (8 frames per tile, the
 *   player's running speed) in catch-up mode (`fast`).
 * - The next tile is a ledge that can be jumped in this direction
 *   (GetLedgeJumpDirection: e.g. MB_JUMP_SOUTH when stepping south) and the
 *   landing tile beyond it is walkable: jump over it with the same 2-tile
 *   jump the player uses (GetJump2MovementAction). The jump moves the
 *   follower's tile onto the ledge as it starts and onto the landing tile
 *   halfway, while the sprite is in the air.
 * - Anything else, including a ledge seen from its side or from below, is a
 *   wall.
 */
static u8 GetFollowerStepAction(struct ObjectEvent *followerObj, u8 direction, bool8 fast)
{
    s16 x = followerObj->currentCoords.x;
    s16 y = followerObj->currentCoords.y;

    MoveCoords(direction, &x, &y);
    if (IsFollowerWalkableTile(followerObj, x, y, direction))
        return fast ? GetWalkFastMovementAction(direction) : GetWalkNormalMovementAction(direction);

    if (GetLedgeJumpDirection(x, y, direction) != DIR_NONE)
    {
        MoveCoords(direction, &x, &y);
        if (IsFollowerWalkableTile(followerObj, x, y, direction))
            return GetJump2MovementAction(direction);
    }
    return MOVEMENT_ACTION_NONE;
}

/**
 * Is the follower's main way toward the player east-west? TRUE when the
 * east-west gap (dx, in tiles) is larger than the north-south gap (dy), and
 * on a random half of the times the two are equal. FALSE means north-south.
 */
static bool8 IsMainGapEastWest(s16 dx, s16 dy)
{
    s16 absDx = dx < 0 ? -dx : dx;
    s16 absDy = dy < 0 ? -dy : dy;

    return absDx > absDy || (absDx == absDy && (Random() & 1));
}

/**
 * FUNCTION: TryFollowerStepTowardPlayer
 *
 * PURPOSE: Start a step that brings the follower closer to the player, if
 *          one is possible. Returns TRUE if a step was started.
 *
 * First choice: along the axis with the larger gap (east-west or
 * north-south; a random one of the two when the gaps are equal). If that
 * way is blocked, the other axis, but only when the player is off that axis
 * too, so that a step along it also gets closer. If both are blocked the
 * follower makes no step this frame and tries again on the next one. It
 * never steps away from the player: there is no pathfinding, so a follower
 * walled off from the player waits (and, in catch-up mode, teleports after
 * STUCK_FRAMES).
 */
static bool8 TryFollowerStepTowardPlayer(struct ObjectEvent *followerObj, struct ObjectEvent *playerObj, bool8 fast)
{
    s16 dx = playerObj->currentCoords.x - followerObj->currentCoords.x;
    s16 dy = playerObj->currentCoords.y - followerObj->currentCoords.y;
    u8 xDir = DIR_NONE;
    u8 yDir = DIR_NONE;
    u8 firstDir, secondDir;
    u8 action = MOVEMENT_ACTION_NONE;

    if (dx != 0)
        xDir = (dx > 0) ? DIR_EAST : DIR_WEST;
    if (dy != 0)
        yDir = (dy > 0) ? DIR_SOUTH : DIR_NORTH;

    if (IsMainGapEastWest(dx, dy))
    {
        firstDir = xDir;
        secondDir = yDir;
    }
    else
    {
        firstDir = yDir;
        secondDir = xDir;
    }

    if (firstDir != DIR_NONE)
        action = GetFollowerStepAction(followerObj, firstDir, fast);
    if (action == MOVEMENT_ACTION_NONE && secondDir != DIR_NONE)
        action = GetFollowerStepAction(followerObj, secondDir, fast);
    if (action == MOVEMENT_ACTION_NONE)
        return FALSE;

    ObjectEventSetHeldMovement(followerObj, action);
    return TRUE;
}

/**
 * The direction to the left of someone facing `facing` (e.g. west for
 * someone facing north). The direction to their right is the opposite.
 */
static u8 GetLeftOfDirection(u8 facing)
{
    switch (facing)
    {
    case DIR_NORTH: return DIR_WEST;
    case DIR_SOUTH: return DIR_EAST;
    case DIR_WEST:  return DIR_SOUTH;
    default:        return DIR_NORTH;  /* DIR_EAST */
    }
}

/**
 * FUNCTION: FindFollowerTileNearPlayer
 *
 * PURPOSE: Find where the follower should appear next to the player, when
 *          it spawns or teleports. Returns FALSE if no candidate is
 *          walkable.
 *
 * Candidates, in this order: the tile behind the player (opposite the way
 * they face), the tile to their left, the tile to their right. Never the
 * tile in front of them, where the follower would block their way, and
 * never their own tile. The first one that passes IsFollowerWalkableTile
 * wins, judged at height `elevation` (the follower's current elevation for
 * a teleport, 0 for a spawn: see the callers).
 *
 * Each candidate is judged as if the follower stepped onto it from the
 * player's tile, moving away from the player. The walkable check needs an
 * ObjectEvent to judge for, so a blank one stands in for the follower,
 * holding just what GetCollisionAtCoords reads:
 *   - its elevation: `elevation`;
 *   - the behaviour of the tile it "stands on": the player's tile's, so a
 *     tile that the player's tile can't be left toward (one-way
 *     MB_IMPASSABLE_* edges) is refused. The follower's own tile, far away
 *     for a teleport, has nothing to do with it;
 *   - no movement range (rangeX/rangeY 0) and not followed by the camera,
 *     like the real follower.
 * The stand-in is not in gObjectEvents, so every live object counts as
 * being in the way, the player included. The real follower is an object
 * too, but it is never on a candidate tile: it only teleports from more
 * than TELEPORT_DIST tiles away, and doesn't exist yet when it spawns.
 */
static bool8 FindFollowerTileNearPlayer(u8 elevation, struct ObjectEvent *playerObj, s16 *outX, s16 *outY)
{
    struct ObjectEvent standIn;
    u8 facing = GetPlayerFacingDirection();
    u8 candidates[3];
    u8 i;
    s16 x, y;

    memset(&standIn, 0, sizeof(standIn));
    standIn.currentCoords = playerObj->currentCoords;
    standIn.currentElevation = elevation;
    standIn.currentMetatileBehavior = MapGridGetMetatileBehaviorAt(playerObj->currentCoords.x,
                                                                   playerObj->currentCoords.y);

    candidates[0] = GetOppositeDirection(facing);
    candidates[1] = GetLeftOfDirection(facing);
    candidates[2] = GetOppositeDirection(candidates[1]);

    for (i = 0; i < NELEMS(candidates); i++)
    {
        x = playerObj->currentCoords.x;
        y = playerObj->currentCoords.y;
        MoveCoords(candidates[i], &x, &y);
        if (IsFollowerWalkableTile(&standIn, x, y, candidates[i]))
        {
            *outX = x;
            *outY = y;
            return TRUE;
        }
    }
    return FALSE;
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
 * The follower is placed on a walkable tile next to the player: behind
 * them, else to their left, else to their right (FindFollowerTileNearPlayer).
 * If none of those is walkable it spawns on the player's own tile and
 * steps off once the player has moved away.
 */
void SpawnFollowerSprite(void)
{
    struct ObjectEvent *playerObj;
    struct ObjectEvent *objEvent;
    u8 objEventId;
    s16 spawnX, spawnY;

    if (!sFollower.active)
        return;

    /*
     * Put the follower away if its Pokemon has left the party (see the top
     * of this file). This runs before the spriteSpawned check on purpose:
     * on a return to the field (e.g. closing the party menu after MOVE TO
     * PC) the overworld has already re-created the follower's sprite from
     * its ObjectEvent, so spriteSpawned is TRUE and its sprite index is
     * valid again, and DeactivateFollower() can destroy it. The screen is
     * still black at this point, so it is never seen.
     */
    if (!IsFollowerInParty())
    {
        DeactivateFollower();
        return;
    }

    if (sFollower.spriteSpawned)
        return;

    playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];

    /*
     * Choose the spawn tile, judged at elevation 0: the height the follower
     * spawns with (see the spawn call below). The engine skips its height
     * test at elevation 0; IsFollowerWalkableTile still refuses water by
     * tile type.
     */
    if (!FindFollowerTileNearPlayer(0, playerObj, &spawnX, &spawnY))
    {
        /* Fallback: spawn on the player's tile (see the comment above) */
        spawnX = playerObj->currentCoords.x;
        spawnY = playerObj->currentCoords.y;
    }

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
 * Teleport the follower to a walkable tile next to the player. Returns
 * FALSE, leaving it where it is, if there is none.
 *
 * Only used as catch-up mode's last resort, when the follower has gone
 * STUCK_FRAMES frames in a row without a step (see STUCK_FRAMES). A
 * follower that is merely far away walks back instead.
 *
 * The tile is judged at the follower's own height (its current elevation),
 * so it never lands on a bridge above it or the ground below one.
 */
static bool8 TeleportFollowerNearPlayer(struct ObjectEvent *followerObj, struct ObjectEvent *playerObj)
{
    struct Sprite *sprite;
    const struct ObjectEventGraphicsInfo *gfxInfo;
    s16 newX, newY;

    if (!FindFollowerTileNearPlayer(followerObj->currentElevation, playerObj, &newX, &newY))
        return FALSE;

    followerObj->currentCoords.x = newX;
    followerObj->currentCoords.y = newY;
    followerObj->previousCoords.x = newX;
    followerObj->previousCoords.y = newY;
    /*
     * Record what the new tile is, as the end of a normal step would: its
     * metatile behaviour (read by the next collision check as "the tile
     * being left") and its elevation.
     */
    followerObj->currentMetatileBehavior = MapGridGetMetatileBehaviorAt(newX, newY);
    followerObj->previousMetatileBehavior = followerObj->currentMetatileBehavior;
    ObjectEventUpdateElevation(followerObj);

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
    return TRUE;
}

/**
 * Random side-step for the relaxed following pace: with a 1-in-
 * DEVIATE_CHANCE chance, try one step perpendicular to the way toward the
 * player (the axis with the larger gap; a random one on a tie), to a random
 * side. This makes the follower wander a little instead of beelining.
 *
 * The step is only taken onto a walkable tile (IsFollowerWalkableTile), and
 * it is a plain walk: a side-step never jumps a ledge. Returns TRUE if a
 * side-step was started; otherwise the caller steps toward the player.
 */
static bool8 TryFollowerSideStep(struct ObjectEvent *followerObj, struct ObjectEvent *playerObj)
{
    s16 dx = playerObj->currentCoords.x - followerObj->currentCoords.x;
    s16 dy = playerObj->currentCoords.y - followerObj->currentCoords.y;
    u8 sideDir;
    s16 x, y;

    if ((Random() % DEVIATE_CHANCE) != 0)
        return FALSE;

    if (IsMainGapEastWest(dx, dy))
        sideDir = (Random() & 1) ? DIR_SOUTH : DIR_NORTH;  /* toward the player is east/west */
    else
        sideDir = (Random() & 1) ? DIR_EAST : DIR_WEST;    /* toward the player is north/south */

    x = followerObj->currentCoords.x;
    y = followerObj->currentCoords.y;
    MoveCoords(sideDir, &x, &y);
    if (!IsFollowerWalkableTile(followerObj, x, y, sideDir))
        return FALSE;

    ObjectEventSetHeldMovement(followerObj, GetWalkNormalMovementAction(sideDir));
    return TRUE;
}

/**
 * FUNCTION: UpdateFollowerPokemon
 *
 * PURPOSE: Per-frame update for the follower system. Called from
 *          CB1_Overworld() every frame, right after UpdateMultiplayerState().
 *
 * HOW IT WORKS:
 * First, if the follower's Pokemon is no longer in the party, the follower
 * is put away (DeactivateFollower) and nothing else happens.
 *
 * Uses a distance-based approach with randomness for natural movement:
 * 1. Compute Manhattan distance from follower to player.
 * 2. If distance > TELEPORT_DIST, catch-up mode: walk fast straight toward
 *    the player, with no random idle or side-step (steps 3 and 5 are
 *    skipped). When no step toward the player is possible, count the frame;
 *    after STUCK_FRAMES of them in a row, teleport next to the player.
 * 3. If distance > FOLLOW_DIST, move toward the player (with an occasional
 *    random side-step for organic movement).
 * 4. If distance <= FOLLOW_DIST, idle — the follower is close enough.
 * 5. With IDLE_CHANCE probability, skip a movement even when far,
 *    creating a relaxed "pet following" pace.
 *
 * Every step goes only onto a walkable tile (IsFollowerWalkableTile), or
 * jumps a ledge in its own direction (GetFollowerStepAction). A blocked
 * step tries the other direction that also leads toward the player, or
 * waits for the next frame.
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

    if (!sFollower.active)
        return;

    /*
     * Put the follower away if its Pokemon has just left the party on the
     * field (e.g. the Day Care took it); see the top of this file. We are on
     * the field here, so the follower's sprite index is valid and
     * DeactivateFollower() destroys the right sprite.
     */
    if (!IsFollowerInParty())
    {
        DeactivateFollower();
        return;
    }

    followerObj = GetFollowerObjectEvent();
    if (followerObj == NULL)
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

    /* Too far away — catch-up mode: move directly toward player without randomness.
     * No IDLE_CHANCE skip, no DEVIATE_CHANCE side-step. Uses the walk-fast
     * movement (GetWalkFastMovementAction): the normal walking frames played
     * at double speed, 2 pixels per frame, so one 16-pixel tile takes 8
     * frames instead of 16. That is the player's running speed, so the
     * follower visibly hurries.
     *
     * Stuck detection: a frame with no possible step toward the player
     * counts toward STUCK_FRAMES; any step starts the count again. Once it
     * reaches STUCK_FRAMES the follower teleports next to the player, or,
     * if no tile there is walkable, keeps the count where it is and tries
     * again next frame. The count stops at STUCK_FRAMES, so the u8 never
     * wraps around to 0. */
    if (dist > TELEPORT_DIST)
    {
        if (TryFollowerStepTowardPlayer(followerObj, playerObj, TRUE))
        {
            sFollower.stuckFrames = 0;
            return;
        }

        if (sFollower.stuckFrames < STUCK_FRAMES)
            sFollower.stuckFrames++;
        if (sFollower.stuckFrames >= STUCK_FRAMES
         && TeleportFollowerNearPlayer(followerObj, playerObj))
            sFollower.stuckFrames = 0;
        return;
    }

    /* Back within TELEPORT_DIST — reset catch-up state */
    sFollower.stuckFrames = 0;

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

    /* Sometimes a step to the side; otherwise (or if that side is blocked)
     * a step toward the player, or none if both ways are blocked. */
    if (!TryFollowerSideStep(followerObj, playerObj))
        TryFollowerStepTowardPlayer(followerObj, playerObj, FALSE);
}

/*
 * ============================================================================
 * SWAPPING PLACES (issue #99): see the top of this file
 * ============================================================================
 */

/*
 * THE SPEED TABLE: which step the follower takes for each step the player
 * can take into it, so that both last exactly as many frames.
 *
 * A step's length is set by its movement action: every one moves the sprite
 * 16 pixels (one tile) at its own pace, e.g. 1 pixel a frame for a normal
 * walk. The follower can't simply copy the player's action, because some
 * player actions play animations its sprite doesn't have: a walking
 * Pokemon's sprite sheet has standing and walking frames only (its
 * animation table is sAnimTable_Standard), not the player's running or
 * bicycle frames. So each row maps a player step to an NPC step of the same
 * length that only uses walking frames.
 *
 * Each player step comes as a family of four actions, one per direction, in
 * the order DOWN, UP, LEFT, RIGHT (so DIR_SOUTH..DIR_EAST, 1..4, map to
 * family + 0..3). A row names the family by its _DOWN action. The frame
 * counts come from the step tables in event_object_movement.c (sStepTimes,
 * UpdateWalkSlowAnim, UpdateRunSlowAnim).
 *
 *   player's step         when                        frames  follower's step
 *   WALK_NORMAL           walking; bicycle uphill       16    WALK_NORMAL
 *   WALK_SLOW             walking on rock stairs        23    WALK_SLOW
 *   PLAYER_RUN            running (B held)               8    WALK_FAST
 *   PLAYER_RUN_SLOW       running on rock stairs        11    PLAYER_RUN_SLOW
 *   WALK_FAST             bicycle on rock stairs         8    WALK_FAST
 *   RIDE_WATER_CURRENT    bicycle                        6    RIDE_WATER_CURRENT
 *   WALK_FASTER           bicycle down the slope         4    WALK_FASTER
 *                         (Cycling Road)
 *
 * PLAYER_RUN_SLOW is the one step with no walking-only twin, so the follower
 * plays the player's own action; InitRunSlow (event_object_movement.c) gives
 * any object other than the player its walk frames for it.
 */
static const struct {
    u8 playerStepDown;              /* the player's step family, its _DOWN action */
    u8 (*followerStep)(u32 dir);    /* the follower's step of the same length */
} sSwapSteps[] = {
    { MOVEMENT_ACTION_WALK_NORMAL_DOWN,        GetWalkNormalMovementAction },
    { MOVEMENT_ACTION_WALK_SLOW_DOWN,          GetWalkSlowMovementAction },
    { MOVEMENT_ACTION_PLAYER_RUN_DOWN,         GetWalkFastMovementAction },
    { MOVEMENT_ACTION_PLAYER_RUN_DOWN_SLOW,    GetPlayerRunSlowMovementAction },
    { MOVEMENT_ACTION_WALK_FAST_DOWN,          GetWalkFastMovementAction },
    { MOVEMENT_ACTION_RIDE_WATER_CURRENT_DOWN, GetRideWaterCurrentMovementAction },
    { MOVEMENT_ACTION_WALK_FASTER_DOWN,        GetWalkFasterMovementAction },
};

/**
 * The engine's collision check for `mover` stepping onto (x, y) in
 * `direction` (GetCollisionAtCoords), made as if `ignored` weren't on the
 * map at all.
 *
 * The object test inside it (DoesObjectCollideWithObjectAt) skips objects
 * whose `active` flag is clear, so `ignored` is switched off for the length
 * of the call and straight back on. Nothing else runs in between: the check
 * only reads, and no interrupt handler touches gObjectEvents. This keeps
 * every other rule of the check exactly the engine's: walls, one-way edges,
 * the map edge, height, and every other object, including one walking onto
 * or off the tile.
 */
static u8 GetCollisionWithout(struct ObjectEvent *ignored, struct ObjectEvent *mover, s16 x, s16 y, u8 direction)
{
    u8 collision;

    ignored->active = FALSE;
    collision = GetCollisionAtCoords(mover, x, y, direction);
    ignored->active = TRUE;
    return collision;
}

/**
 * FUNCTION: CanPlayerSwapWithFollower
 *
 * PURPOSE: Decide whether the player's step in `direction`, which their step
 *          code found blocked by an object, is a step into the follower that
 *          swaps places with it. Called by the player's own step code
 *          (walking and running in field_player_avatar.c, the bicycle in
 *          bike.c; see the top of this file); if it returns TRUE the caller
 *          takes its normal step and then calls StartFollowerSwapStep.
 *
 * All of these must hold:
 *   1. The player isn't surfing (the follower never goes on water).
 *   2. The follower stands on the tile the player steps onto, standing
 *      still: no step of its own in progress, and its previous tile is its
 *      current one. A follower in the middle of a step blocks, as a walking
 *      NPC does.
 *   3. Without the follower, the player's step would be legal: the engine's
 *      check passes with the follower ignored (no wall, no one-way edge
 *      against it, the same height, inside the map, nobody else on the
 *      tile). The bicycle's own extra rule, a tile that forbids biking, is
 *      checked by the caller in bike.c.
 *   4. The follower can make the reverse step onto the player's tile: the
 *      engine's NPC check passes with the player ignored, and the tile isn't
 *      water. One-way edges are the same in both directions in this engine,
 *      so this practically always holds once 3 does. Unlike the follower's
 *      own steps (IsFollowerWalkableTile), a warp is allowed: the player may
 *      have arrived on a doormat or a ladder, and refusing would box them in
 *      when the follower spawned in the only way out.
 * Calls no Random().
 */
bool8 CanPlayerSwapWithFollower(u8 direction)
{
    struct ObjectEvent *playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct ObjectEvent *followerObj = GetFollowerObjectEvent();
    s16 x = playerObj->currentCoords.x;
    s16 y = playerObj->currentCoords.y;

    if (followerObj == NULL || (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_SURFING))
        return FALSE;

    MoveCoords(direction, &x, &y);
    if (followerObj->currentCoords.x != x || followerObj->currentCoords.y != y
     || followerObj->previousCoords.x != x || followerObj->previousCoords.y != y
     || followerObj->singleMovementActive
     || (followerObj->heldMovementActive && !followerObj->heldMovementFinished))
        return FALSE;

    if (GetCollisionWithout(followerObj, playerObj, x, y, direction) != COLLISION_NONE)
        return FALSE;

    x = playerObj->currentCoords.x;
    y = playerObj->currentCoords.y;
    if (GetCollisionWithout(playerObj, followerObj, x, y, GetOppositeDirection(direction)) != COLLISION_NONE)
        return FALSE;
    if (MetatileBehavior_IsSurfable(MapGridGetMetatileBehaviorAt(x, y)))
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: StartFollowerSwapStep
 *
 * PURPOSE: Start the follower's half of a swap: one step onto the player's
 *          old tile, opposite to `direction`, the player's step direction.
 *          Called right after the player's step code has stored the player's
 *          own step (for a swap CanPlayerSwapWithFollower allowed), in the
 *          same frame, so the sprite callbacks start both steps together.
 *
 * The step lasts as long as the player's: it is looked up in sSwapSteps from
 * the step just stored on the player (its movementActionId), so it follows
 * whatever the player's code chose, running, rock stairs and the Cycling
 * Road included. A player step missing from the table (none of the callers
 * makes one) gets a normal walk, which still takes the follower off the
 * player's new tile.
 *
 * Moving the follower's tile needs no collision check of its own:
 * CanPlayerSwapWithFollower has already made it. Its previous step may have
 * ended without being cleared yet (UpdateFollowerPokemon, which clears it,
 * runs after the player's step code each frame), so that is cleared first:
 * the engine refuses a new held movement while an old one is still stored.
 * After the step it faces the way it walked and idles one tile behind the
 * player, as UpdateFollowerPokemon does with any follower that close.
 */
void StartFollowerSwapStep(u8 direction)
{
    struct ObjectEvent *playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct ObjectEvent *followerObj = GetFollowerObjectEvent();
    u8 playerAction = playerObj->movementActionId;
    u8 followerDirection = GetOppositeDirection(direction);
    u8 action = GetWalkNormalMovementAction(followerDirection);
    u8 i;

    if (followerObj == NULL)
        return;

    for (i = 0; i < NELEMS(sSwapSteps); i++)
    {
        if (playerAction >= sSwapSteps[i].playerStepDown
         && playerAction < sSwapSteps[i].playerStepDown + 4)
        {
            action = sSwapSteps[i].followerStep(followerDirection);
            break;
        }
    }

    ObjectEventClearHeldMovementIfFinished(followerObj);
    ObjectEventSetHeldMovement(followerObj, action);
}
