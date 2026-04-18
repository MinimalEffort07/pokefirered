/**
 * Roaming Pokemon System
 *
 * Spawns visible wild Pokemon as object events on the overworld so the
 * player can see them wandering before triggering a battle. Walking into
 * a roaming Pokemon's tile starts a wild battle (the same kind that
 * grass-step encounters trigger). Flying-type pokemon additionally fly
 * across the screen as non-interactable flybys -- atmospheric only.
 *
 * The species pool is the per-map wild encounter table from
 * gWildMonHeaders. We do not modify wild_encounter.c -- this is a
 * separate visual / interaction layer on top of the existing system.
 * Random grass encounters still fire normally; roaming is purely additive.
 *
 * MEMORY: All state is in EWRAM (~50 bytes). Session-only -- nothing
 * persists across save/load. The save format is untouched.
 *
 * SLOTS: At most ROAMER_CAP_VISIBLE roamers are alive simultaneously
 * (default 6). We always leave OBJECT_EVENTS_RESERVED slots free for
 * NPCs/scripts so the map's regular events can still spawn. Flybys are
 * raw sprites and do NOT consume gObjectEvents slots.
 *
 * BUMP-TO-BATTLE: When the player walks into a roamer's tile, the
 * collision check in field_player_avatar.c queues a battle. Next frame,
 * UpdateRoamingPokemon dispatches it via CreateScriptedWildMon +
 * StartScriptedWildBattle. The roamer's object event is removed when
 * the battle starts so it doesn't reappear after the battle ends.
 */

#include "global.h"
#include "gflib.h"
#include "roaming_pokemon.h"
#include "battle_setup.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "overworld.h"
#include "quest_log.h"
#include "random.h"
#include "script_pokemon_util.h"
#include "wild_encounter.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/pokemon.h"
#include "constants/sound.h"
#include "constants/species.h"

/*
 * Tuning knobs. These are conservative; the cap shares the 16-slot
 * gObjectEvents budget with the player, the follower, and any NPCs the
 * map defined. Leaving 4 slots free (RESERVED) preserves room for
 * scripted spawns (story NPCs that appear after a flag check, etc.).
 */
/*
 * EWRAM in this project is at ~100% utilization, so the roamer state
 * pool is sized conservatively. The user's design target was 4-6
 * simultaneous roamers; we sit at the low end (4) to keep our memory
 * footprint to a few dozen bytes. Increasing these caps requires
 * freeing EWRAM elsewhere first.
 */
#define ROAMER_CAP_VISIBLE          4
#define ROAMER_CAP_DISTINCT_SPECIES 4
#define OBJECT_EVENTS_RESERVED      4

#define FLYBY_CAP_VISIBLE           4

/*
 * Local IDs for spawned roamer object events. Map-defined object events
 * use small local IDs starting from 1, the player is 255, the follower
 * is 254, and the Berry Blender peers occupy 236-240. The 200-209 range
 * is unused by the engine, so we carve out 200-205 for our 6 roamer
 * slots and 206-208 as scratch (currently unused). Each roamer's index
 * in gRoamers[] determines its local ID: roamer i -> 200 + i.
 */
#define LOCALID_ROAMER_BASE         200

/*
 * Despawn a roamer when it gets this far from the player (Manhattan
 * distance in tiles). The visible window is ~15x10 tiles, so 14 is just
 * past the edge -- the player won't see it pop out of existence.
 */
#define ROAMER_DESPAWN_DISTANCE     14

/*
 * Tile-search ring for picking new spawn locations. We sample random
 * offsets in a ring 6-12 tiles from the player so roamers spawn just
 * off-screen and walk onto the visible area organically.
 */
#define SPAWN_RING_INNER            6
#define SPAWN_RING_OUTER            12
#define SPAWN_TILE_SAMPLE_COUNT     12

/*
 * Per-tick periodic spawn cadence for grounded roamers. Cheap try every
 * SPAWN_TIMER_BASE frames if we're below cap. ~1 second at 60fps.
 */
#define SPAWN_TIMER_BASE            60

/*
 * Per-tick periodic flyby cadence. Long enough that the sky doesn't
 * feel busy. ~5 seconds at 60fps; rolled with FLYBY_CHANCE_DENOM odds.
 */
#define FLYBY_TIMER_BASE            300
#define FLYBY_CHANCE_DENOM          4

/*
 * Flyby motion. Crosses the visible screen left-to-right or right-to-
 * left in roughly 8 seconds at 1 px/frame on a 240px screen. A slower
 * cruise reads as graceful birds drifting past, not hummingbirds.
 */
#define FLYBY_SPEED_PX              1
#define FLYBY_OFFSCREEN_MARGIN      48

/*
 * Pack flybys. Occasionally a spawn event produces a small flock of
 * the same species moving in the same direction, offset into a V-ish
 * formation. FLYBY_PACK_CHANCE_DENOM of 1-in-N spawn ticks become
 * packs (others remain single birds). FLYBY_PACK_SIZE is the target
 * flock size; fewer birds may actually appear if we run out of flyby
 * slots. FLYBY_PACK_X_STEP/Y_STEP give the tight V layout: each
 * follower trails the previous by X pixels horizontally and is
 * shifted Y pixels vertically (alternating up/down) so the flock
 * reads as a formation and not a stack.
 */
#define FLYBY_PACK_CHANCE_DENOM     5
#define FLYBY_PACK_SIZE             3
#define FLYBY_PACK_X_STEP           24
#define FLYBY_PACK_Y_STEP           8

/*
 * Each roamer's full state, packed into 4 bytes to fit the tight
 * EWRAM budget. We store an INDEX into sRoamerGfxTable rather than
 * the full species id (u16) -- the table has fewer than 256 entries
 * so a u8 index is enough, and species/gfxId are derivable from it.
 *
 * objEventId == OBJECT_EVENTS_COUNT (16) means the slot is free;
 * this matches the engine's "no such object event" sentinel and lets
 * us skip free slots cheaply.
 */
struct RoamingMon
{
    u8 objEventId;       /* index into gObjectEvents, or OBJECT_EVENTS_COUNT if free */
    u8 tableIdx;         /* index into sRoamerGfxTable */
    u8 level;            /* level rolled at spawn (used for the battle) */
    u8 pendingBattle;    /* set by CheckForObjectEventCollision; cleared after battle starts */
};

/*
 * EWRAM_DATA places these in External Work RAM (256KB). CLAUDE.md
 * says to avoid IWRAM (32KB, scarce) for new features unless
 * absolutely necessary. Total footprint here is ~24 bytes.
 *
 * NOT static -- the mGBA Lua test harness needs to read these symbols
 * by name from pokefirered.map to assert state invariants. File-local
 * statics are stripped from the symbol map. The `g` prefix marks them
 * as file-internal-but-globally-symbol-visible (the codebase uses `s`
 * for true file-locals; we deliberately don't follow that here).
 */
EWRAM_DATA struct RoamingMon gRoamers[ROAMER_CAP_VISIBLE] = {0};
EWRAM_DATA u8 gRoamingFlybySpriteIds[FLYBY_CAP_VISIBLE] = {0};
EWRAM_DATA u8 gRoamerCount = 0;
EWRAM_DATA u16 gRoamerNextSpawnTimer = 0;
EWRAM_DATA u16 gRoamerNextFlybyTimer = 0;
/*
 * EWRAM is zero-initialized at boot. Both objEventId == 0 and
 * spriteId == 0 are VALID indices in their respective arrays (slot 0
 * is usually the player). We can't use 0 as our "free slot" sentinel,
 * so we run a one-time init that writes the proper sentinels
 * (OBJECT_EVENTS_COUNT for roamers, MAX_SPRITES for flybys). Called
 * at the top of every public entry point; the bool flag makes
 * subsequent calls a single load+branch.
 */
static EWRAM_DATA bool8 sInitialized = FALSE;

/*
 * Species-to-overworld-graphics mapping table.
 *
 * Only species that have an OBJ_EVENT_GFX_* sprite in
 * include/constants/event_objects.h can be roamers. If a per-map
 * encounter slot rolls a species that's NOT in this table, that spawn
 * attempt is silently skipped -- no object event is created. This keeps
 * roaming density naturally proportional to OW sprite coverage; we
 * don't substitute fallback sprites because the user prefers visual
 * accuracy over coverage.
 *
 * Legendaries (Zapdos, Articuno, Moltres, Mewtwo, Mew, Entei, Suicune,
 * Raikou, Lugia, Ho-Oh, Celebi, Deoxys variants) have OW sprites but
 * are intentionally excluded -- they do not appear in standard wild
 * encounter tables anyway, but listing them would let scripted
 * encounter tables surface them unexpectedly.
 */
static const struct
{
    u16 species;
    u8 graphicsId;
} sRoamerGfxTable[] = {
    { SPECIES_SNORLAX,    OBJ_EVENT_GFX_SNORLAX },
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
    { SPECIES_PIKACHU,    OBJ_EVENT_GFX_PIKACHU },
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

#define ROAMER_TABLE_NONE NELEMS(sRoamerGfxTable)

/*
 * Custom animation table used only by flyby sprites.
 *
 * Why not reuse sAnimTable_Standard's ANIM_STD_GO_WEST/EAST?
 * Those are the grounded-walk cycles: frames 7 -> 2 -> 8 -> 2 at
 * 8 ticks each, i.e. they return to the neutral side-facing idle
 * frame (2) between every step. On a bird that reads as hopping on
 * the ground, not flying -- the "wings parked" idle keeps popping in
 * and the step rhythm is indistinguishable from a walk animation.
 *
 * What flight looks like instead: the two motion frames (7 and 8 --
 * the side-profile flap-poses for Pidgey/Spearow/Pidgeot/Fearow)
 * cycle back and forth continuously, no idle frame between beats, at
 * a cadence that matches a small bird's wingbeat (roughly 4 Hz). The
 * frame swap alone is NOT enough -- 16x16 pixel art for these species
 * changes wing position only by ~1-2 pixels, which on a fast-moving
 * sprite reads as foot shuffle, not flap. The real flight cue comes
 * from FlybySpriteCallback adding a vertical bob (sFlybyBobCurve)
 * synchronized to this flap cycle. The two together are what convinces
 * the eye the bird is airborne.
 *
 * Frame index convention across the bird OW sheets in
 * src/data/object_events/object_event_pic_tables.h:
 *   frame 2 = side profile, wings at rest
 *   frame 7 = side profile, flap pose A
 *   frame 8 = side profile, flap pose B
 *
 * Doduo has no literal wings in its OW sprite (Normal/Flying but
 * drawn as a running two-head), so on Doduo this cycle reads as
 * "running very fast with a body bob" -- accepted trade-off. The
 * alternative would be a per-species flap table, which is more data
 * for a marginal visual gain.
 *
 * 8 ticks per frame -> 16-frame full flap cycle -> ~3.75 Hz wingbeat
 * at 60fps. Slow enough that the eye registers each wing position,
 * fast enough to read as purposeful flight not soaring. The bob curve
 * below is written for exactly this 16-frame period so flap-pose and
 * body-height line up (wing-down-stroke lifts the bird, up-stroke
 * drops it, like a real bird).
 */
static const union AnimCmd sFlybyFlap_West[] = {
    ANIMCMD_FRAME(7, 8),
    ANIMCMD_FRAME(8, 8),
    ANIMCMD_JUMP(0),
};

static const union AnimCmd sFlybyFlap_East[] = {
    ANIMCMD_FRAME(7, 8, .hFlip = TRUE),
    ANIMCMD_FRAME(8, 8, .hFlip = TRUE),
    ANIMCMD_JUMP(0),
};

#define FLYBY_FLAP_ANIM_WEST 0
#define FLYBY_FLAP_ANIM_EAST 1

static const union AnimCmd *const sFlybyFlapAnimTable[] = {
    [FLYBY_FLAP_ANIM_WEST] = sFlybyFlap_West,
    [FLYBY_FLAP_ANIM_EAST] = sFlybyFlap_East,
};

/*
 * Vertical bob curve indexed by (sFlybyPhase & 15), one entry per
 * frame of the 16-frame flap cycle above. Triangle wave, peak
 * amplitude 3 px, aligned so:
 *   phases 0-7  (flap-pose A / frame 7): body rises 0 -> -3 px
 *   phases 8-15 (flap-pose B / frame 8): body falls -3 -> 0 px
 *
 * Written into sprite->y2 (a renderer-only offset that the OAM draw
 * path adds to sprite->y each frame). Using y2 keeps the baseline
 * sprite->y unchanged, so the 8-second crossing doesn't accumulate
 * vertical drift the way the old phase-driven sprite->y += 1 did.
 *
 * Negative numbers raise the sprite on screen because the GBA's Y
 * axis grows downward.
 */
static const s8 sFlybyBobCurve[16] = {
    0,  0, -1, -1, -2, -2, -3, -3,
   -3, -3, -2, -2, -1, -1,  0,  0,
};

static void EnsureInitialized(void);
static u8 GetRoamerTableIdx(u16 species);
static const struct WildPokemonHeader *GetCurrentMapWildHeader(void);
static u8 CountActiveObjectEvents(void);
static bool8 TrySpawnOneRoamer(const struct WildPokemonHeader *header);
static bool8 PickRoamerSpawnTile(s16 *outX, s16 *outY);
static u8 PickWildSlot(void);
static u8 RollLevelInRange(u8 minLevel, u8 maxLevel);
static void DispatchPendingBattles(void);
static void RemoveRoamerByIndex(u8 idx);
static bool8 SpeciesIsFlyingType(u16 species);
static void SpawnOneFlyby(const struct WildPokemonHeader *header);
static void RemoveFlybyByIndex(u8 idx);
static void FlybySpriteCallback(struct Sprite *sprite);

/* ----- public API ----- */

/**
 * Spawn an initial wave of roamers on the current map.
 *
 * Called from overworld.c at the end of LoadMapFromWarp (after a real
 * map change) and from ReloadObjectsAndRunReturnToFieldMapScript (after
 * exiting a menu/script back to the overworld). Both call sites already
 * spawn the follower sprite right before us, so the gObjectEvents array
 * is fully populated except for our slots.
 *
 * If the current map has no entry in gWildMonHeaders (e.g. a town or
 * indoor building), this is a no-op -- there is nothing to roll for.
 */
void SpawnRoamingPokemonOnMap(void)
{
    const struct WildPokemonHeader *header;
    u8 i;

    EnsureInitialized();

    /*
     * Quest log playback recreates events from a recorded session. Our
     * spawn rolls use the live RNG which would desync the recording.
     * Skip entirely while playback is active; we'll resume on normal
     * gameplay.
     */
    if (QL_GetPlaybackState() == QL_PLAYBACK_STATE_RUNNING)
        return;

    header = GetCurrentMapWildHeader();
    if (header == NULL)
        return;

    /* Only land encounters drive grounded roamers in v1. Water-only
     * pokemon (Lapras etc.) need a surfing player to be reachable, so
     * we leave water/fishing/rock-smash slots out for now. */
    if (header->landMonsInfo == NULL)
        return;

    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (gRoamerCount >= ROAMER_CAP_VISIBLE)
            break;
        if (CountActiveObjectEvents() + 1 > OBJECT_EVENTS_COUNT - OBJECT_EVENTS_RESERVED)
            break;
        TrySpawnOneRoamer(header);
    }

    gRoamerNextSpawnTimer = SPAWN_TIMER_BASE;
    gRoamerNextFlybyTimer = FLYBY_TIMER_BASE;
}

/**
 * Remove every roamer object event and every flyby sprite.
 *
 * Called from LoadMapFromWarp BEFORE the current map's data is unloaded
 * (overworld.c:835). At that point gObjectEvents still references the
 * outgoing map's object events, so DestroySprite + active=FALSE cleans
 * them up before ResetObjectEvents() blows the array away.
 */
void DespawnAllRoamingPokemon(void)
{
    u8 i;

    EnsureInitialized();
    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (gRoamers[i].objEventId < OBJECT_EVENTS_COUNT)
            RemoveRoamerByIndex(i);
    }
    gRoamerCount = 0;

    for (i = 0; i < FLYBY_CAP_VISIBLE; i++)
    {
        if (gRoamingFlybySpriteIds[i] < MAX_SPRITES)
            RemoveFlybyByIndex(i);
    }
}

/**
 * Per-frame tick. Despawns far-away roamers, periodically tries to
 * spawn new ones (top-up), and dispatches any pending bump-to-battle
 * triggers queued by the collision hook.
 */
void UpdateRoamingPokemon(void)
{
    struct ObjectEvent *playerObj;
    struct ObjectEvent *roamerObj;
    const struct WildPokemonHeader *header;
    s16 dx, dy;
    s16 dist;
    u8 i;

    EnsureInitialized();
    if (QL_GetPlaybackState() == QL_PLAYBACK_STATE_RUNNING)
        return;

    /* Pending bump-to-battle takes priority -- no other work this frame
     * if we're about to launch a battle. */
    DispatchPendingBattles();

    playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];

    /*
     * Despawn roamers that have wandered too far from the player. The
     * engine doesn't auto-cull off-camera object events, so without this
     * the OE table would slowly fill with stale roamers as the player
     * walks across the map.
     */
    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (gRoamers[i].objEventId >= OBJECT_EVENTS_COUNT)
            continue;
        roamerObj = &gObjectEvents[gRoamers[i].objEventId];
        if (!roamerObj->active)
        {
            /* Engine reaped it from under us -- clear our cache. */
            gRoamers[i].objEventId = OBJECT_EVENTS_COUNT;
            if (gRoamerCount > 0)
                gRoamerCount--;
            continue;
        }
        dx = roamerObj->currentCoords.x - playerObj->currentCoords.x;
        dy = roamerObj->currentCoords.y - playerObj->currentCoords.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        dist = dx + dy;
        if (dist > ROAMER_DESPAWN_DISTANCE)
            RemoveRoamerByIndex(i);
    }

    /* Periodic top-up: try to spawn one new roamer per timer tick if
     * we're below cap. SPAWN_TIMER_BASE is short enough (1 sec) that
     * the map fills up quickly after a warp, but slow enough that we
     * don't spam the tile picker. */
    if (gRoamerNextSpawnTimer > 0)
    {
        gRoamerNextSpawnTimer--;
        return;
    }
    gRoamerNextSpawnTimer = SPAWN_TIMER_BASE;

    if (gRoamerCount >= ROAMER_CAP_VISIBLE)
        return;
    if (CountActiveObjectEvents() + 1 > OBJECT_EVENTS_COUNT - OBJECT_EVENTS_RESERVED)
        return;

    header = GetCurrentMapWildHeader();
    if (header == NULL || header->landMonsInfo == NULL)
        return;

    TrySpawnOneRoamer(header);
}

/**
 * Per-frame tick for flying-type flybys. Independent of the grounded
 * roamer system -- flybys are raw sprites that don't touch
 * gObjectEvents. The sprite callback (FlybySpriteCallback) handles
 * per-frame motion; this function only handles spawn timing and
 * removing despawned entries from our tracking table.
 */
void UpdateRoamingFlybys(void)
{
    const struct WildPokemonHeader *header;
    u8 i;

    EnsureInitialized();
    if (QL_GetPlaybackState() == QL_PLAYBACK_STATE_RUNNING)
        return;

    /* Reap entries the sprite callback flagged as removed (it sets
     * sprite->inUse=FALSE via DestroySprite when off-screen). */
    for (i = 0; i < FLYBY_CAP_VISIBLE; i++)
    {
        if (gRoamingFlybySpriteIds[i] < MAX_SPRITES
            && !gSprites[gRoamingFlybySpriteIds[i]].inUse)
        {
            gRoamingFlybySpriteIds[i] = MAX_SPRITES;
        }
    }

    if (gRoamerNextFlybyTimer > 0)
    {
        gRoamerNextFlybyTimer--;
        return;
    }
    gRoamerNextFlybyTimer = FLYBY_TIMER_BASE;

    if ((Random() % FLYBY_CHANCE_DENOM) != 0)
        return;

    header = GetCurrentMapWildHeader();
    if (header == NULL || header->landMonsInfo == NULL)
        return;

    SpawnOneFlyby(header);
}

/**
 * Cheap O(N) check used by the player's collision routine. Returns
 * TRUE if objEventId belongs to a roamer we spawned. Validates that
 * the slot's graphicsId still matches our cached value -- gObjectEvents
 * slots can be reused, so a stale objEventId might now point to a
 * different (e.g., scripted) NPC.
 */
bool8 IsRoamingPokemonObjectEvent(u8 objEventId)
{
    u8 i;

    EnsureInitialized();
    if (objEventId >= OBJECT_EVENTS_COUNT)
        return FALSE;

    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (gRoamers[i].objEventId == objEventId
            && gRoamers[i].tableIdx < NELEMS(sRoamerGfxTable)
            && gObjectEvents[objEventId].active
            && gObjectEvents[objEventId].graphicsId
               == sRoamerGfxTable[gRoamers[i].tableIdx].graphicsId)
            return TRUE;
    }
    return FALSE;
}

/**
 * Queue a battle for the roamer at objEventId. Called from the bump
 * collision check in field_player_avatar.c. We don't start the battle
 * immediately because the collision check runs deep inside the input
 * pipeline; deferring to the next frame in UpdateRoamingPokemon lets
 * the field control system finish its tick cleanly first.
 */
void TryStartRoamingBattle(u8 objEventId)
{
    u8 i;

    EnsureInitialized();
    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (gRoamers[i].objEventId == objEventId)
        {
            gRoamers[i].pendingBattle = TRUE;
            return;
        }
    }
}

/* ----- internal helpers ----- */

/**
 * One-time setup of the EWRAM state. EWRAM is zero-initialized at boot,
 * but slot 0 is a valid index in both gObjectEvents and gSprites, so
 * we have to write proper "free" sentinels (OBJECT_EVENTS_COUNT and
 * MAX_SPRITES respectively) before any iteration treats slot 0 as a
 * real entry. Idempotent -- the bool guard short-circuits subsequent calls.
 */
static void EnsureInitialized(void)
{
    u8 i;

    if (sInitialized)
        return;
    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        gRoamers[i].objEventId = OBJECT_EVENTS_COUNT;
        gRoamers[i].tableIdx = ROAMER_TABLE_NONE;
        gRoamers[i].level = 0;
        gRoamers[i].pendingBattle = FALSE;
    }
    for (i = 0; i < FLYBY_CAP_VISIBLE; i++)
        gRoamingFlybySpriteIds[i] = MAX_SPRITES;
    gRoamerCount = 0;
    sInitialized = TRUE;
}

/**
 * Look up the species in the roamer-eligible table and return its
 * index, or ROAMER_TABLE_NONE if the species has no overworld sprite.
 * Callers derive the OBJ_EVENT_GFX_* constant via
 * sRoamerGfxTable[idx].graphicsId.
 */
static u8 GetRoamerTableIdx(u16 species)
{
    u8 i;

    for (i = 0; i < NELEMS(sRoamerGfxTable); i++)
    {
        if (sRoamerGfxTable[i].species == species)
            return i;
    }
    return ROAMER_TABLE_NONE;
}

/**
 * Linear search through gWildMonHeaders for the current map. The
 * static helper of the same name in wild_encounter.c is not exported,
 * so we replicate the core lookup. We skip the Altering Cave special
 * case (it's a Sevii Islands feature gated by NUM_ALTERING_CAVE_TABLES)
 * since it doesn't materially affect roaming -- the cave will still
 * resolve via its first sub-table.
 */
static const struct WildPokemonHeader *GetCurrentMapWildHeader(void)
{
    u16 i;
    u8 mapGroup;
    u8 mapNum;

    mapGroup = gSaveBlock1Ptr->location.mapGroup;
    mapNum = gSaveBlock1Ptr->location.mapNum;

    for (i = 0; ; i++)
    {
        if (gWildMonHeaders[i].mapGroup == MAP_GROUP(MAP_UNDEFINED))
            return NULL;
        if (gWildMonHeaders[i].mapGroup == mapGroup
            && gWildMonHeaders[i].mapNum == mapNum)
            return &gWildMonHeaders[i];
    }
}

static u8 CountActiveObjectEvents(void)
{
    u8 i;
    u8 n = 0;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        if (gObjectEvents[i].active)
            n++;
    }
    return n;
}

/**
 * Try to spawn one roamer on the current map. Picks a species from
 * the encounter table, looks up its OW gfx, finds a walkable spawn
 * tile near the player, and creates the object event.
 *
 * Returns TRUE on success, FALSE if any step failed (no eligible
 * species, no free roamer slot, no walkable tile, OE creation failed).
 */
static bool8 TrySpawnOneRoamer(const struct WildPokemonHeader *header)
{
    const struct WildPokemonInfo *info;
    u8 slot;
    u16 species;
    u8 tableIdx;
    u8 level;
    s16 spawnX, spawnY;
    u8 freeIdx;
    u8 newObjId;
    u8 i;

    info = header->landMonsInfo;
    slot = PickWildSlot();
    species = info->wildPokemon[slot].species;

    tableIdx = GetRoamerTableIdx(species);
    if (tableIdx == ROAMER_TABLE_NONE)
        return FALSE; /* Species has no OW sprite -- skip silently. */

    /* Find a free slot in our tracking table. */
    freeIdx = ROAMER_CAP_VISIBLE;
    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (gRoamers[i].objEventId >= OBJECT_EVENTS_COUNT)
        {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx == ROAMER_CAP_VISIBLE)
        return FALSE;

    if (!PickRoamerSpawnTile(&spawnX, &spawnY))
        return FALSE;

    level = RollLevelInRange(info->wildPokemon[slot].minLevel,
                             info->wildPokemon[slot].maxLevel);

    /* MOVEMENT_TYPE_WANDER_AROUND_SLOWER (0x50) is the engine's
     * built-in random-walk behavior. It honors collision, picks a
     * random adjacent tile every few seconds, and naturally lets the
     * roamer stray onto adjacent passable tiles -- exactly the
     * "grass tiles preferred, adjacent passable tiles allowed"
     * behavior the user requested. */
    newObjId = SpawnSpecialObjectEventParameterized(
        sRoamerGfxTable[tableIdx].graphicsId,
        MOVEMENT_TYPE_WANDER_AROUND_SLOWER,
        LOCALID_ROAMER_BASE + freeIdx,
        spawnX,
        spawnY,
        0 /* elevation */
    );
    if (newObjId >= OBJECT_EVENTS_COUNT)
        return FALSE;

    gRoamers[freeIdx].objEventId = newObjId;
    gRoamers[freeIdx].tableIdx = tableIdx;
    gRoamers[freeIdx].level = level;
    gRoamers[freeIdx].pendingBattle = FALSE;
    gRoamerCount++;
    return TRUE;
}

/**
 * Pick a walkable, encounter-eligible tile inside a ring around the
 * player. Tries SPAWN_TILE_SAMPLE_COUNT random offsets and accepts
 * the first one that is (a) not a collision tile and (b) marked as a
 * land-encounter tile in the map's metatile attributes (i.e. tall grass
 * or cave floor). Returns FALSE if none of the samples qualified --
 * the caller should treat this as "couldn't spawn this tick" and try
 * again later.
 */
static bool8 PickRoamerSpawnTile(s16 *outX, s16 *outY)
{
    struct ObjectEvent *playerObj;
    s16 px, py;
    s16 tx, ty;
    s16 dx, dy;
    u8 attempts;
    u32 r;

    playerObj = &gObjectEvents[gPlayerAvatar.objectEventId];
    px = playerObj->currentCoords.x;
    py = playerObj->currentCoords.y;

    for (attempts = 0; attempts < SPAWN_TILE_SAMPLE_COUNT; attempts++)
    {
        r = Random();
        /* dx and dy each in [-(OUTER), +OUTER], biased away from zero
         * by adding/subtracting INNER. This keeps spawns out of the
         * tile right next to the player. */
        dx = (s16)((r & 0x1F) % (SPAWN_RING_OUTER * 2 + 1)) - SPAWN_RING_OUTER;
        r >>= 8;
        dy = (s16)((r & 0x1F) % (SPAWN_RING_OUTER * 2 + 1)) - SPAWN_RING_OUTER;

        if ((dx > -SPAWN_RING_INNER && dx < SPAWN_RING_INNER)
            && (dy > -SPAWN_RING_INNER && dy < SPAWN_RING_INNER))
            continue; /* too close to player */

        tx = px + dx;
        ty = py + dy;

        if (MapGridGetCollisionAt(tx, ty))
            continue;
        if (GetObjectEventIdByPosition(tx, ty, 0) != OBJECT_EVENTS_COUNT)
            continue;
        if (MapGridGetMetatileAttributeAt(tx, ty, METATILE_ATTRIBUTE_ENCOUNTER_TYPE)
            != TILE_ENCOUNTER_LAND)
            continue;

        *outX = tx;
        *outY = ty;
        return TRUE;
    }
    return FALSE;
}

/**
 * Pick a wild encounter slot index (0..LAND_WILD_COUNT-1). For
 * roaming we use a flat uniform distribution rather than the weighted
 * grass-step distribution -- roaming is a separate visual layer and
 * a flat distribution gives players a fair shot at every species in
 * the table without re-implementing wild_encounter's slot weighting.
 */
static u8 PickWildSlot(void)
{
    return Random() % LAND_WILD_COUNT;
}

static u8 RollLevelInRange(u8 minLevel, u8 maxLevel)
{
    u8 lo, hi;

    if (maxLevel >= minLevel) { lo = minLevel; hi = maxLevel; }
    else                       { lo = maxLevel; hi = minLevel; }
    return lo + (Random() % (hi - lo + 1));
}

/**
 * Walk the roamer table; if any has pendingBattle set, fire the wild
 * battle and remove the roamer's object event so it doesn't reappear
 * after the battle ends.
 *
 * CreateScriptedWildMon populates gEnemyParty[0]; StartScriptedWildBattle
 * sets BATTLE_TYPE_WILD and queues the battle transition task. The
 * field control system locks the player on the next frame, so the
 * player visibly stays on their pre-bump tile and the screen flashes
 * into the standard wild battle intro.
 */
static void DispatchPendingBattles(void)
{
    u8 i;
    u16 species;

    for (i = 0; i < ROAMER_CAP_VISIBLE; i++)
    {
        if (!gRoamers[i].pendingBattle)
            continue;
        gRoamers[i].pendingBattle = FALSE;

        if (gRoamers[i].tableIdx >= NELEMS(sRoamerGfxTable))
            continue; /* Defensive: corrupt slot, just skip. */
        species = sRoamerGfxTable[gRoamers[i].tableIdx].species;

        CreateScriptedWildMon(species, gRoamers[i].level, ITEM_NONE);
        StartScriptedWildBattle();

        RemoveRoamerByIndex(i);
        return; /* one battle at a time */
    }
}

/**
 * Free a roamer slot and its object event. We can't call the engine's
 * RemoveObjectEvent (it's static in event_object_movement.c), so we
 * manually destroy the sprite and clear the active flag -- the same
 * pattern used by DespawnFollowerSprite in pokemon_follower.c.
 */
static void RemoveRoamerByIndex(u8 idx)
{
    struct ObjectEvent *objEvent;
    struct Sprite *spr;

    if (gRoamers[idx].objEventId < OBJECT_EVENTS_COUNT)
    {
        objEvent = &gObjectEvents[gRoamers[idx].objEventId];
        if (objEvent->active && objEvent->spriteId < MAX_SPRITES)
        {
            spr = &gSprites[objEvent->spriteId];
            if (spr->inUse)
                DestroySprite(spr);
        }
        objEvent->active = FALSE;
    }
    gRoamers[idx].objEventId = OBJECT_EVENTS_COUNT;
    gRoamers[idx].tableIdx = ROAMER_TABLE_NONE;
    gRoamers[idx].level = 0;
    gRoamers[idx].pendingBattle = FALSE;
    if (gRoamerCount > 0)
        gRoamerCount--;
}

/* ----- flyby helpers ----- */

static bool8 SpeciesIsFlyingType(u16 species)
{
    return gSpeciesInfo[species].types[0] == TYPE_FLYING
        || gSpeciesInfo[species].types[1] == TYPE_FLYING;
}

/*
 * Sprite data slots used by FlybySpriteCallback. Storing per-sprite
 * state in sprite->data[] keeps each flyby self-contained -- the
 * callback doesn't need to look anything up by sprite ID.
 *   data[0] = horizontal velocity in pixels per frame (signed)
 *   data[1] = bob phase (frame counter, increments each frame)
 *   data[2] = remaining lifespan in frames (counts down to 0)
 */
#define sFlybyVx    data[0]
#define sFlybyPhase data[1]
#define sFlybyLife  data[2]

/*
 * Maximum flyby lifespan as a hard safety net: even if the off-screen
 * detection misses (camera changes mid-flight, etc.), the sprite will
 * self-destruct after this many frames. 480 = 8 seconds.
 */
#define FLYBY_MAX_LIFE 480

/**
 * Pick a flying-type from the map's encounter table and create a raw
 * sprite that traverses the screen. The sprite is NOT an object event
 * -- it does not touch gObjectEvents and is invisible to interaction
 * checks. CreateObjectGraphicsSprite handles palette loading via the
 * tag-based VRAM system; if the palette table is full we fall through
 * (CreateSprite returns MAX_SPRITES) and just skip this flyby.
 */
/*
 * Try to spawn one flyby sprite at (startX, startY) moving at vx.
 * Returns TRUE on success, FALSE if we ran out of flyby slots or
 * CreateObjectGraphicsSprite could not allocate (palette/sprite
 * table full). Same-species pack members share gfxId and vx; only
 * x/y are staggered by the caller.
 */
static bool8 CreateOneFlybySprite(u8 gfxId, s16 startX, s16 startY, s16 vx)
{
    u8 freeIdx;
    u8 spriteId;
    u8 i;

    freeIdx = FLYBY_CAP_VISIBLE;
    for (i = 0; i < FLYBY_CAP_VISIBLE; i++)
    {
        if (gRoamingFlybySpriteIds[i] >= MAX_SPRITES)
        {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx == FLYBY_CAP_VISIBLE)
        return FALSE;

    /*
     * Convert the caller's SCREEN-space spawn position into a WORLD-space
     * position so we can render the flyby with coordOffsetEnabled=TRUE
     * below. In world-space mode the OAM renderer computes
     *   oam.x = sprite->x + gSpriteCoordOffsetX
     * every frame, so a sprite at the same world.x is automatically
     * repositioned on screen as the camera scrolls. We want the caller
     * to keep thinking in screen coords (the bird enters at screen x
     * = -48, exits at 288) for readability, so we bake the current
     * camera offset into sprite->x at spawn time: world = screen -
     * offset. After this, sprite->x is a sky coordinate: the bird
     * flies along sky.x += vx regardless of where the player walks,
     * which is the physically correct behavior ("bird in the sky over
     * Route 1" stays in the sky over Route 1, doesn't get dragged
     * when the player starts running).
     */
    spriteId = CreateObjectGraphicsSprite(gfxId, FlybySpriteCallback,
                                          startX - gSpriteCoordOffsetX,
                                          startY - gSpriteCoordOffsetY,
                                          0);
    if (spriteId == MAX_SPRITES)
        return FALSE;

    gSprites[spriteId].sFlybyVx = vx;
    gSprites[spriteId].sFlybyPhase = 0;
    gSprites[spriteId].sFlybyLife = FLYBY_MAX_LIFE;

    /* World-space rendering: sprite->x/y are sky coordinates; the
     * renderer adds gSpriteCoordOffsetX/Y each frame to get the screen
     * position. Without this, the bird would be stuck in screen space
     * and appear to drift weirdly relative to the scrolling terrain as
     * the player walks. CreateSprite leaves this field FALSE by
     * default, so we have to opt in. */
    gSprites[spriteId].coordOffsetEnabled = TRUE;

    /* Swap the sprite's animation table from the species' stock walk
     * cycle (sAnimTable_Standard, which returns to an idle frame between
     * steps) to our flap-only cycle defined in sFlybyFlapAnimTable.
     * Same frame indices, but no idle frame in between and ~2x faster,
     * so bird sprites visibly flap rather than "hop" across the screen.
     *
     * sprite->anims is declared `const ... *const *` in struct Sprite,
     * so the runtime swap is legal here -- event_object_movement.c does
     * the same thing (see CopyObjectGraphicsInfoToSpriteTemplate users). */
    gSprites[spriteId].anims = sFlybyFlapAnimTable;

    /* Pin the flyby on top of everything. Overworld maps use BG layers
     * for terrain, tall grass, tree canopies, and upper storeys of
     * buildings -- some of those BGs run at OAM priority <= 2, which
     * makes stock-priority sprites disappear behind them as they fly
     * past. Priority 0 forces the sprite above every BG layer; subpriority
     * 0 wins any remaining sprite-vs-sprite Z fight. Subsprite tables
     * are installed with SUBSPRITES_IGNORE_PRIORITY by
     * CreateObjectGraphicsSprite, so sub-tiles inherit this priority. */
    gSprites[spriteId].oam.priority = 0;
    gSprites[spriteId].subpriority = 0;

    StartSpriteAnim(&gSprites[spriteId],
                    (vx > 0) ? FLYBY_FLAP_ANIM_EAST : FLYBY_FLAP_ANIM_WEST);

    gRoamingFlybySpriteIds[freeIdx] = spriteId;
    return TRUE;
}

static void SpawnOneFlyby(const struct WildPokemonHeader *header)
{
    const struct WildPokemonInfo *info;
    u16 candidate;
    u8 candidateTableIdx;
    u8 gfxId;
    s16 leaderX, leaderY;
    s16 vx;
    u8 attempts;
    u8 packSize;
    u8 i;
    bool8 picked;

    info = header->landMonsInfo;

    /* Sample a few slots looking for a flying-type with an OW sprite.
     * If the map's encounter table has no flying types, we just give
     * up (no flyby this tick). */
    picked = FALSE;
    gfxId = 0;
    for (attempts = 0; attempts < LAND_WILD_COUNT; attempts++)
    {
        candidate = info->wildPokemon[Random() % LAND_WILD_COUNT].species;
        if (!SpeciesIsFlyingType(candidate))
            continue;
        candidateTableIdx = GetRoamerTableIdx(candidate);
        if (candidateTableIdx == ROAMER_TABLE_NONE)
            continue;
        gfxId = sRoamerGfxTable[candidateTableIdx].graphicsId;
        picked = TRUE;
        break;
    }
    if (!picked)
        return;

    /* Pick a side of the screen to enter from and the corresponding
     * velocity. The GBA screen is 240x160 px; we spawn off-screen by
     * FLYBY_OFFSCREEN_MARGIN so the sprite slides in instead of popping. */
    if (Random() & 1)
    {
        leaderX = -FLYBY_OFFSCREEN_MARGIN;
        vx = FLYBY_SPEED_PX;
    }
    else
    {
        leaderX = 240 + FLYBY_OFFSCREEN_MARGIN;
        vx = -FLYBY_SPEED_PX;
    }
    /* Spawn in the upper third of the screen so flyers look like
     * they're in the sky, not skimming the ground. */
    leaderY = 16 + (Random() % 32);

    /*
     * Play the species' cry as a directional audio cue. Pan matches
     * the side the flyby is entering from: bird entering from the
     * left (vx > 0, coming from screen x = -48) -> cry pans LEFT so
     * the player hears it from the correct side. pan is s8 (-128..127)
     * where 0 is center; ~40 lateral magnitude is noticeable without
     * being gimmicky. Volume 70 is quieter than a battle cry so the
     * flyby reads as atmospheric, not as if a wild encounter started.
     * CRY_PRIORITY_AMBIENT (=1) lets any gameplay cry (pokedex, battle)
     * preempt it cleanly; we should never be blocking a real audio cue.
     *
     * For packs we intentionally play ONE cry, not one per bird: a
     * flock of Spearows does not sound like three overlapping cries
     * in real life, and the cry channel gets clobbered by priority
     * rules if we try to stack them anyway.
     */
    PlayCry_NormalNoDucking(candidate,
                            (vx > 0) ? -40 : 40,
                            70,
                            CRY_PRIORITY_AMBIENT);

    /* Roll pack chance. On a hit, attempt a tight V formation of
     * FLYBY_PACK_SIZE birds; otherwise fall through as a single bird
     * (packSize = 1). Pack members trail the leader along the direction
     * of motion with alternating vertical offsets. */
    packSize = ((Random() % FLYBY_PACK_CHANCE_DENOM) == 0)
                ? FLYBY_PACK_SIZE : 1;

    for (i = 0; i < packSize; i++)
    {
        s16 thisX;
        s16 thisY;

        /* Each pack member trails the previous by FLYBY_PACK_X_STEP
         * in the OPPOSITE direction of travel (i.e. behind the leader).
         * Y offset alternates +/-Y_STEP for members 1 and 2 so they
         * form a V rather than a straight line behind. */
        thisX = (vx > 0)
              ? leaderX - (s16)(i * FLYBY_PACK_X_STEP)
              : leaderX + (s16)(i * FLYBY_PACK_X_STEP);

        if (i == 0)
            thisY = leaderY;
        else if ((i & 1) != 0)
            thisY = leaderY - FLYBY_PACK_Y_STEP;
        else
            thisY = leaderY + FLYBY_PACK_Y_STEP;

        /* Stop early if a slot/sprite allocation fails -- the partial
         * flock is still a valid render; we just can't fit the rest. */
        if (!CreateOneFlybySprite(gfxId, thisX, thisY, vx))
            break;
    }
}

static void RemoveFlybyByIndex(u8 idx)
{
    struct Sprite *spr;

    if (gRoamingFlybySpriteIds[idx] < MAX_SPRITES)
    {
        spr = &gSprites[gRoamingFlybySpriteIds[idx]];
        if (spr->inUse)
            DestroySprite(spr);
    }
    gRoamingFlybySpriteIds[idx] = MAX_SPRITES;
}

/**
 * Per-frame motion for a flyby sprite. Translates horizontally at
 * sFlybyVx pixels per frame and bobs vertically by 2 px on a slow sine
 * (every 16 frames toggles). When the sprite goes off-screen on the
 * far side, or its lifespan expires, DestroySprite is called -- the
 * UpdateRoamingFlybys reaper picks up the now-dead slot next frame and
 * clears our tracking entry.
 */
static void FlybySpriteCallback(struct Sprite *sprite)
{
    sprite->x += sprite->sFlybyVx;
    sprite->sFlybyPhase++;

    /* Flap-synchronized vertical bob. The OW sprite sheets for bird
     * species only vary by 1-2 pixels of wing position between flap
     * poses A and B (frames 7 and 8), which on a 16x16 mover reads as
     * foot shuffle, not flight. The real visual cue that sells flight
     * is the BODY rising on the wing-downstroke and dropping on the
     * upstroke. sFlybyBobCurve encodes a 16-frame triangle wave (peak
     * -3 px) aligned with the flap animation below:
     *   phases 0-7  (frame 7, wing-down stroke):  y2 goes 0 -> -3
     *   phases 8-15 (frame 8, wing-up stroke):    y2 goes -3 -> 0
     * y2 is a renderer-only offset (added to sprite->y by the OAM
     * draw path) so this costs no permanent vertical drift the way
     * the old phase-triggered sprite->y += 1 did. */
    sprite->y2 = sFlybyBobCurve[sprite->sFlybyPhase & 15];

    if (sprite->sFlybyLife > 0)
        sprite->sFlybyLife--;

    /* Destroy only when the sprite has crossed the visible screen and
     * exited the FAR side relative to its direction of travel.
     *
     * sprite->x is a WORLD coordinate here (see CreateOneFlybySprite
     * comment on coordOffsetEnabled). Convert to a screen x for the
     * bounds check so "has the bird crossed the visible area" does
     * not get fooled by camera scroll -- if the player runs the same
     * direction as a flyby, the bird's world.x keeps advancing while
     * its screen.x barely moves; we must test the screen.x to
     * correctly retire the sprite only once it is actually off-camera.
     *
     * A direction-aware check (only test the FAR side) is still needed
     * because pack followers spawn BEHIND the leader in screen space,
     * i.e. further outside the entry margin, and would fail a symmetric
     * bounds check on their very first frame. sFlybyLife remains a hard
     * upper bound in case the camera stays pointed away from the bird's
     * exit margin forever. */
    {
        s16 screenX = sprite->x + gSpriteCoordOffsetX;
        if (sprite->sFlybyLife == 0
            || (sprite->sFlybyVx > 0 && screenX > 240 + FLYBY_OFFSCREEN_MARGIN)
            || (sprite->sFlybyVx < 0 && screenX < -FLYBY_OFFSCREEN_MARGIN))
        {
            DestroySprite(sprite);
        }
    }
}

#undef sFlybyVx
#undef sFlybyPhase
#undef sFlybyLife
