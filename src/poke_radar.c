/*
 * poke_radar.c - Gen 4 PokeRadar Shiny Chaining
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * Implements the Gen 4 Diamond/Pearl/Platinum PokeRadar mechanic as a custom
 * addition to Pokemon FireRed.
 *
 * MECHANIC SUMMARY:
 *   1. Player uses the PokeRadar key item while standing in tall grass.
 *   2. Four nearby grass patches visually "shake" (field-effect sprites).
 *   3. Stepping into a shaking patch starts a wild battle at once (see
 *      PATCH BATTLES below). Its species becomes locked as the "chained
 *      species": later patch battles are that species.
 *   4. Each successful KO or capture increments a chain counter (cap: 40).
 *   5. The chain counter grants extra shiny PID rerolls per encounter:
 *        rerolls = 1 + floor(min(chain, 40) / 5)
 *      giving 1 roll at chain 0, 2 at chain 5, ... and 8 at chain 40.
 *   6. The chain breaks on:
 *        - Engaging a different species in a wild encounter (patch or normal)
 *        - Fleeing / teleporting / losing the wild battle
 *        - Stepping off tall grass
 *
 * PATCH BATTLES:
 *   The overworld handles the player's input once per frame in
 *   ProcessPlayerFieldInput (src/field_control_avatar.c). On the frame a
 *   step ends it runs, in order: the trainers' line of sight, map scripts,
 *   the per-step work (PokeRadar_OnStep among it, then step-triggered
 *   scripts such as a coord event or the Repel running out), walk-in signs,
 *   and last the random wild encounter roll. Each of these may "take" the
 *   step, and then nothing after it runs. PokeRadar_TryStartPatchBattle
 *   sits just before the random roll, in its place: if the step ended on a
 *   live patch tile, the battle starts and the roll never happens. The
 *   battle ignores Repel and the encounter rate (the patch IS the
 *   encounter); its Pokemon comes from the map's grass table as usual
 *   (PokeRadarWildEncounter in src/wild_encounter.c). If anything earlier
 *   took the step, the patch is left alone and still shakes.
 *
 *   All four patches go away as the battle starts. The next use needs a
 *   charge again.
 *
 * THE FROM-PATCH FLAG:
 *   POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER tells the wild-battle code that
 *   the battle being set up came out of a patch, so GenerateWildMon
 *   (src/wild_encounter.c) uses the chain species and PokeRadar_OnBattleEnd
 *   grows the chain on a win. It is set only for that battle: just before
 *   the patch battle's Pokemon is made, and cleared when the battle ends.
 *   At every other moment it is 0, so a random battle never counts as a
 *   patch's.
 *
 * STATE STORAGE:
 *   `struct PokeRadar` is laid out inside the pre-existing 400-byte
 *   `SaveBlock1::unused_348C[]` padding region. This keeps the save format
 *   size unchanged. A 4-byte magic number 'PRDR' detects first-time use on
 *   pre-existing saves so we can zero the struct before reading any field.
 *
 * CHARGE / RECHARGE:
 *   The PokeRadar starts with 1 charge. Using it consumes the charge and
 *   sets `stepsUntilCharge = 50`. Each step decrements the counter; reaching
 *   zero restores one charge.
 *
 * VISUAL PATCHES:
 *   Reuses the pre-existing `FldEff_UnusedGrass` (FLDEFF_UNUSED_GRASS=19)
 *   field effect. Its sprite template has a perpetual 2-frame shake loop,
 *   which is exactly what we want. `FldEff_UnusedGrass` is modified to
 *   return the created `spriteId` via `FieldEffectStart`'s result path so
 *   we can track each patch for later destruction.
 *
 * FIELD RELOAD (and why a stored sprite id is checked before it is used):
 *   The GBA draws every moving picture (the player, the NPCs, the shaking
 *   grass) as a "sprite": a small image the hardware places anywhere on
 *   screen, described by an entry in its OAM table (Object Attribute
 *   Memory). The game keeps its own table of sprites, gSprites[], with
 *   MAX_SPRITES (64) slots, and copies it into OAM every frame. Each patch
 *   is one slot, and the struct keeps the slot's number (patchSpriteId) so
 *   the patch can be removed later. But a slot number is only a seat
 *   number: it says nothing about who sits there now.
 *
 *   The field (the overworld) empties all 64 slots whenever it is rebuilt
 *   (ResumeMap in src/overworld.c calls ResetSpriteData) and then makes the
 *   sprites it needs again from scratch. That happens on coming back from
 *   a full-screen menu such as the BAG or POKEMON (those screens clear the
 *   sprite table for their own pictures), on Continue, and on every warp.
 *   A battle clears it too. The patches' tiles and flag survive in
 *   SaveBlock1, but their sprites are gone, and their old slot numbers may
 *   now belong to anything: an NPC's tall grass, the follower, the player,
 *   or during a battle one of the battle's sprites.
 *
 *   So, once the objects' sprites exist again, every rebuild calls
 *   PokeRadar_OnFieldReload (from InitObjectEventsLocal,
 *   ReloadObjectsAndRunReturnToFieldMapScript and InitObjectEventsLink):
 *     - on the patches' own map (patchMapGroup/patchMapNum), each patch
 *       gets a fresh shaking-grass sprite on its tile, and its new slot
 *       number is stored;
 *     - on any other map, the patches are forgotten: the flag is cleared
 *       and the slots set to "none", and no sprite is stopped, since none
 *       of the sprites there is ours. Coming back to the first map does
 *       not bring them back.
 *   (Walking across a map's edge into the next map is not a rebuild: the
 *   sprites live on and nothing here runs. The patches stay, their tiles
 *   in the old map's coordinates, until a step off the grass or the next
 *   rebuild clears them. This was so before #90 and is left as it is.)
 *   And PokeRadar_ClearPatches stops a sprite only if it is one of the
 *   radar's own live patches (IsOwnPatchSprite: in use, and made from the
 *   shaking grass's sprite template). Any other stored slot is just
 *   forgotten: stopping it would destroy whatever sprite sits there now,
 *   the player's own, say, or a battle sprite at the end of a battle.
 *
 * HOOKS (files outside this module):
 *   - src/new_game.c       : grant item + init struct on new game
 *   - src/load_save.c      : ensure-init + grant item on pre-existing saves
 *   - src/item_use.c       : field-use handler (ItemUseOutOfBattle_PokeRadar)
 *   - src/wild_encounter.c : start the patch battle, force the chain
 *                            species on it + inject shiny
 *   - src/battle_main.c    : chain increment / break on wild-battle exit
 *   - src/field_control_avatar.c : per-step tick for recharge + off-grass
 *                            break, and the patch battle in place of the
 *                            random encounter roll
 *   - src/overworld.c      : bring the patches back, or forget them, when
 *                            the field is rebuilt (FIELD RELOAD)
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_effect.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "item.h"
#include "metatile_behavior.h"
#include "overworld.h"
#include "random.h"
#include "poke_radar.h"
#include "wild_encounter.h"
#include "constants/battle.h"
#include "constants/field_effects.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/species.h"

#define PATCH_SPRITE_NONE  MAX_SPRITES  // 64; valid sprite IDs are 0..63

/* Build-time sanity checks: the PokeRadar struct must live at a known offset
 * inside SaveBlock1 and fit inside the 400-byte slack region. The Lua test
 * helper hard-codes the same offset constant, so if this assertion ever
 * fails, the test's pointer math will drift from the running ROM and every
 * struct read will return stale bytes.
 *
 * The field's name (`unused_348C`) implies offset 0x348C, but that comment in
 * include/global.h is stale — MysteryGiftSave grew by 40 bytes at some point
 * and the surrounding struct layout was never renamed. The real runtime
 * offset is 0x34B4. */
STATIC_ASSERT(sizeof(struct PokeRadar) <= sizeof(((struct SaveBlock1 *)0)->unused_348C),
              PokeRadarFitsInSlack);
STATIC_ASSERT(offsetof(struct SaveBlock1, unused_348C) == 0x34B4,
              PokeRadarOffsetMatchesLuaTest);

struct PokeRadar *PokeRadar_Get(void)
{
    /* The PokeRadar struct is overlaid on unused_348C[], which is 400 bytes
     * of slack pre-allocated inside SaveBlock1. sizeof(struct PokeRadar) is
     * 0x24 (36 bytes) so it fits with plenty of headroom. */
    return (struct PokeRadar *)&gSaveBlock1Ptr->unused_348C[0];
}

void PokeRadar_ResetOnNewGame(void)
{
    struct PokeRadar *r = PokeRadar_Get();
    memset(r, 0, sizeof(*r));
    r->magic = POKE_RADAR_MAGIC;
    r->charges = 1;
    r->chainSpecies = SPECIES_NONE;
}

void PokeRadar_EnsureInit(void)
{
    /* Called after a save file is loaded. If the player's save predates this
     * feature, the unused_348C[] region holds zero or garbage. In that case,
     * zero the struct, install the magic, grant the key item, and start with
     * a single charge. `AddBagItem` is a no-op for key items the player
     * already owns (importance=1 caps stack at 1), so double-granting on a
     * re-patched save is harmless. */
    struct PokeRadar *r = PokeRadar_Get();
    if (r->magic != POKE_RADAR_MAGIC)
    {
        memset(r, 0, sizeof(*r));
        r->magic = POKE_RADAR_MAGIC;
        r->charges = 1;
        r->chainSpecies = SPECIES_NONE;
        if (!CheckBagHasItem(ITEM_POKE_RADAR, 1))
            AddBagItem(ITEM_POKE_RADAR, 1);
    }
}

bool8 PokeRadar_IsUsableHere(void)
{
    /* The PokeRadar is only useful when the player is standing in tall
     * grass. We check the player's own tile metatile behavior, since that
     * is the origin tile around which we spawn the 4 candidate patches. */
    s16 x, y;
    u8 behavior;
    PlayerGetDestCoords(&x, &y);
    behavior = MapGridGetMetatileBehaviorAt(x, y);
    return MetatileBehavior_IsTallGrass(behavior);
}

static bool8 IsPositionOccupiedByPatch(struct PokeRadar *r, s16 x, s16 y)
{
    u8 i;
    if (!(r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE))
        return FALSE;
    for (i = 0; i < POKE_RADAR_PATCH_COUNT; i++)
    {
        if (r->patchSpriteId[i] != PATCH_SPRITE_NONE
            && r->patchX[i] == x && r->patchY[i] == y)
            return TRUE;
    }
    return FALSE;
}

/* Is sprite slot `spriteId` one of the radar's own live patches? It must be
 * in use and made from the template FldEff_UnusedGrass makes its sprites
 * from: nothing else in the game uses that template, so this tells our
 * patches apart from whatever sprite took a stale slot (FIELD RELOAD in the
 * overview). CreateSprite stores the template's address in the sprite. */
static bool8 IsOwnPatchSprite(u8 spriteId)
{
    return spriteId < MAX_SPRITES
        && gSprites[spriteId].inUse
        && gSprites[spriteId].template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_UNUSED_GRASS];
}

/* Forget patch `i` without touching its sprite: no tile, no slot number. */
static void ForgetPatch(struct PokeRadar *r, u8 i)
{
    r->patchSpriteId[i] = PATCH_SPRITE_NONE;
    r->patchX[i] = 0;
    r->patchY[i] = 0;
}

/* Forget every patch without touching any sprite: no patch tiles, no slot
 * numbers, PATCHES_ACTIVE cleared. */
static void ForgetPatches(struct PokeRadar *r)
{
    u8 i;
    for (i = 0; i < POKE_RADAR_PATCH_COUNT; i++)
        ForgetPatch(r, i);
    r->patchMapGroup = 0;
    r->patchMapNum = 0;
    r->flags &= ~POKE_RADAR_FLAG_PATCHES_ACTIVE;
}

/* Start one shaking-grass field effect on map tile (x, y) (in the engine's
 * coordinates, with the MAP_OFFSET margin). Returns its sprite slot, or
 * PATCH_SPRITE_NONE if every slot was taken. */
static u8 SpawnPatchSprite(s16 x, s16 y)
{
    u32 spriteId;
    gFieldEffectArguments[0] = x;
    gFieldEffectArguments[1] = y;
    gFieldEffectArguments[2] = 0;   /* sprite subpriority */
    gFieldEffectArguments[3] = 2;   /* OAM priority */
    spriteId = FieldEffectStart(FLDEFF_UNUSED_GRASS);
    if (spriteId >= MAX_SPRITES)
        return PATCH_SPRITE_NONE;
    return spriteId;
}

void PokeRadar_ClearPatches(void)
{
    struct PokeRadar *r = PokeRadar_Get();
    u8 i;

    if (!(r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE))
        return;

    for (i = 0; i < POKE_RADAR_PATCH_COUNT; i++)
    {
        u8 spriteId = r->patchSpriteId[i];
        /* Only our own sprite: a stored slot may hold another sprite by now
         * (FIELD RELOAD in the overview), and that one is left alone. */
        if (IsOwnPatchSprite(spriteId))
        {
            /* FieldEffectStop frees the sprite AND releases its tile/palette
             * allocations; ActiveListRemove removes one entry for the fldeff.
             * Each of our patches was added separately via FieldEffectStart,
             * so we must Stop once per patch. */
            FieldEffectStop(&gSprites[spriteId], FLDEFF_UNUSED_GRASS);
        }
    }
    ForgetPatches(r);
}

void PokeRadar_OnFieldReload(void)
{
    /* Called by src/overworld.c each time the field has been rebuilt, once
     * the objects' sprites exist again (see FIELD RELOAD in the overview).
     * The rebuild freed every sprite, the patches' with them, so the
     * stored slot numbers name nothing of ours: they are replaced or
     * forgotten here, never stopped. */
    struct PokeRadar *r = PokeRadar_Get();
    u8 i, live = 0;

    if (!(r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE))
        return;

    if (r->patchMapGroup != gSaveBlock1Ptr->location.mapGroup
     || r->patchMapNum != gSaveBlock1Ptr->location.mapNum)
    {
        ForgetPatches(r);
        return;
    }

    for (i = 0; i < POKE_RADAR_PATCH_COUNT; i++)
    {
        if (r->patchSpriteId[i] == PATCH_SPRITE_NONE)
            continue;
        r->patchSpriteId[i] = SpawnPatchSprite(r->patchX[i], r->patchY[i]);
        if (r->patchSpriteId[i] != PATCH_SPRITE_NONE)
            live++;
        else
            ForgetPatch(r, i);   /* no free slot: that tile stops being a patch */
    }
    if (live == 0)
        ForgetPatches(r);
}

void PokeRadar_BreakChain(void)
{
    struct PokeRadar *r = PokeRadar_Get();
    PokeRadar_ClearPatches();
    r->chainCount = 0;
    r->chainSpecies = SPECIES_NONE;
    r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;
}

void PokeRadar_Activate(void)
{
    /* Scan a (2R+1) x (2R+1) box centered on the player. Any tile whose
     * metatile behavior is tall grass and which is not the player's own
     * tile is a patch candidate. We then reservoir-pick 4 distinct
     * candidates uniformly and spawn a shaking-grass field effect on each.
     *
     * Using a fixed-size candidate buffer keeps the scan stack-bounded.
     * With R=6 we scan 13*13 = 169 tiles, minus 1 for the player. We cap
     * the candidate list at 96 (which still covers almost-all-grass maps). */
    struct PokeRadar *r = PokeRadar_Get();
    s16 px, py;
    s16 cx, cy;
    s16 candX[96];
    s16 candY[96];
    u16 nCand = 0;
    u8 i, j;

    PokeRadar_ClearPatches();

    PlayerGetDestCoords(&px, &py);

    for (cy = py - POKE_RADAR_PATCH_RADIUS; cy <= py + POKE_RADAR_PATCH_RADIUS; cy++)
    {
        for (cx = px - POKE_RADAR_PATCH_RADIUS; cx <= px + POKE_RADAR_PATCH_RADIUS; cx++)
        {
            u8 behavior;
            if (cx == px && cy == py)
                continue;
            behavior = MapGridGetMetatileBehaviorAt(cx, cy);
            if (!MetatileBehavior_IsTallGrass(behavior))
                continue;
            if (nCand < ARRAY_COUNT(candX))
            {
                candX[nCand] = cx;
                candY[nCand] = cy;
                nCand++;
            }
        }
    }

    /* Fisher-Yates shuffle up to 4 positions, picking from [0..nCand-1]. */
    for (i = 0; i < POKE_RADAR_PATCH_COUNT && i < nCand; i++)
    {
        u16 pick = i + (Random() % (nCand - i));
        s16 tx = candX[pick];
        s16 ty = candY[pick];
        candX[pick] = candX[i];
        candY[pick] = candY[i];
        candX[i] = tx;
        candY[i] = ty;
    }

    /* Spawn up to POKE_RADAR_PATCH_COUNT shaking-grass field effects. */
    ForgetPatches(r);
    for (i = 0, j = 0; i < POKE_RADAR_PATCH_COUNT && i < nCand; i++)
    {
        u8 spriteId = SpawnPatchSprite(candX[i], candY[i]);
        if (spriteId != PATCH_SPRITE_NONE)
        {
            r->patchSpriteId[j] = spriteId;
            r->patchX[j] = candX[i];
            r->patchY[j] = candY[i];
            j++;
        }
    }

    if (j > 0)
    {
        r->flags |= POKE_RADAR_FLAG_PATCHES_ACTIVE;
        /* The patches belong to this map: a field rebuilt on any other
         * map forgets them (PokeRadar_OnFieldReload). */
        r->patchMapGroup = gSaveBlock1Ptr->location.mapGroup;
        r->patchMapNum = gSaveBlock1Ptr->location.mapNum;
    }
    r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;

    /* Consume the charge and arm the recharge counter. */
    if (r->charges > 0)
        r->charges--;
    r->stepsUntilCharge = POKE_RADAR_RECHARGE_STEPS;
}

void PokeRadar_OnStep(void)
{
    /* Called on every completed step, before step-triggered scripts get
     * their turn (see PATCH BATTLES in the overview). A step INTO a patch
     * is not handled here: PokeRadar_TryStartPatchBattle does that later in
     * the same frame, so a script that takes this step leaves the patch. */
    struct PokeRadar *r = PokeRadar_Get();
    s16 px, py;
    u8 behavior;

    /* No battle is being set up during a step, so the from-patch flag must
     * be 0 here. Older builds set it on the patch step and could leave it
     * set in a save; clear it so the next random battle isn't taken for a
     * patch's. */
    r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;

    /* Recharge tick — independent of patches/chain. */
    if (r->stepsUntilCharge > 0)
    {
        r->stepsUntilCharge--;
        if (r->stepsUntilCharge == 0 && r->charges == 0)
            r->charges = 1;
    }

    if (!(r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE)
        && r->chainCount == 0)
        return;

    PlayerGetDestCoords(&px, &py);
    behavior = MapGridGetMetatileBehaviorAt(px, py);

    /* Off-grass detection: stepping onto a non-tall-grass tile breaks the
     * chain and removes the patches. This is the primary chain-break vector
     * besides wild-battle outcomes. We only care if the player has an
     * active chain or active patches — otherwise it's a normal step with no
     * radar state to clear. (A patch tile is always tall grass, so a step
     * into a patch never gets here.) */
    if (!MetatileBehavior_IsTallGrass(behavior))
    {
        if (r->chainCount > 0 || (r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE))
            PokeRadar_BreakChain();
    }
}

bool8 PokeRadar_TryStartPatchBattle(void)
{
    /* Called from ProcessPlayerFieldInput on the frame a step ends, in
     * place of the random wild encounter roll, once nothing earlier (a
     * trainer, a script, a sign) has taken the step. If the player now
     * stands on a live patch, start its wild battle and return TRUE: the
     * overworld then stops handling input, as for any battle. Otherwise
     * return FALSE and the random roll goes ahead as usual.
     *
     * Map coordinates here (PlayerGetDestCoords, patchX/patchY) include
     * the 7-tile margin the engine keeps around every map (MAP_OFFSET),
     * so they match the ones PokeRadar_Activate stored. */
    struct PokeRadar *r = PokeRadar_Get();
    s16 px, py;

    PlayerGetDestCoords(&px, &py);
    if (!IsPositionOccupiedByPatch(r, px, py))
        return FALSE;

    /* The flag must be set before the Pokemon is made: GenerateWildMon
     * reads it to pick the chain species. PokeRadar_OnBattleEnd clears it
     * when the battle is over. */
    r->flags |= POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;
    if (!PokeRadarWildEncounter())
    {
        /* No battle after all (a map without grass Pokemon, or wild
         * battles switched off): the patch stays, and so does the rule
         * that the flag is 0 outside a patch battle. */
        r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;
        return FALSE;
    }

    /* The battle's screen transition has only just begun, on the field, so
     * removing the four shaking sprites now makes them vanish as the player
     * lands in the grass. */
    PokeRadar_ClearPatches();
    return TRUE;
}

void PokeRadar_OnEncounterStart(u16 species)
{
    /* Called from GenerateWildMon just before the enemy mon is created.
     * If this is a patch-forced encounter and no species has been chained
     * yet, adopt this encounter's species as the chain species. Otherwise,
     * if a chain is active and the natural roll produced a different
     * species (only possible for non-patch encounters in grass), break the
     * chain. */
    struct PokeRadar *r = PokeRadar_Get();

    if (r->flags & POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER)
    {
        if (r->chainSpecies == SPECIES_NONE)
            r->chainSpecies = species;
        return;
    }

    if (r->chainCount > 0 && r->chainSpecies != SPECIES_NONE
        && species != r->chainSpecies)
    {
        PokeRadar_BreakChain();
    }
}

bool8 PokeRadar_TryInjectShiny(u16 species, u32 *personalityOut)
{
    /* Gen 4 canon reroll formula. `rolls` includes the natural roll, so
     * we need only `rolls` PIDs total to decide: if any is shiny, we
     * short-circuit and return it. The caller treats a FALSE return as
     * "fall back to the default CreateMonWithNature path" — which itself
     * consumes a PID, but by then we've already rejected `rolls` rerolls
     * so the non-shiny output is still statistically unbiased. */
    struct PokeRadar *r = PokeRadar_Get();
    u32 otId;
    u16 rolls;
    u16 i;

    (void)species;

    if (r->chainCount == 0)
        rolls = 1;
    else
    {
        u16 clamped = r->chainCount;
        if (clamped > POKE_RADAR_MAX_CHAIN)
            clamped = POKE_RADAR_MAX_CHAIN;
        rolls = 1 + (clamped / 5);
    }

    otId = (u32)gSaveBlock2Ptr->playerTrainerId[0]
         | ((u32)gSaveBlock2Ptr->playerTrainerId[1] << 8)
         | ((u32)gSaveBlock2Ptr->playerTrainerId[2] << 16)
         | ((u32)gSaveBlock2Ptr->playerTrainerId[3] << 24);

    for (i = 0; i < rolls; i++)
    {
        u32 pid = ((u32)Random() << 16) | Random();
        if (GET_SHINY_VALUE(otId, pid) < SHINY_ODDS)
        {
            *personalityOut = pid;
            return TRUE;
        }
    }
    return FALSE;
}

void PokeRadar_OnBattleEnd(u8 outcome)
{
    /* Called from battle_main.c right as we exit back to the overworld
     * after a WILD battle. Trainer battles are filtered at the call site.
     * Note on non-patch wins: winning a non-patch wild encounter of a
     * different species breaks the chain in OnEncounterStart already; a
     * non-patch win of the matching species neither extends nor breaks
     * the chain, so we only act on patch-wins here. */
    struct PokeRadar *r = PokeRadar_Get();
    bool8 fromPatch = (r->flags & POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER) ? TRUE : FALSE;

    if (outcome == B_OUTCOME_WON || outcome == B_OUTCOME_CAUGHT)
    {
        if (fromPatch && r->chainSpecies != SPECIES_NONE)
        {
            if (r->chainCount < POKE_RADAR_MAX_CHAIN)
                r->chainCount++;
        }
    }
    else if (outcome == B_OUTCOME_RAN
          || outcome == B_OUTCOME_PLAYER_TELEPORTED
          || outcome == B_OUTCOME_LOST)
    {
        PokeRadar_BreakChain();
    }

    /* Whatever happens, sweep any remaining patches and clear the
     * patch-encounter latch. Activation next time starts fresh. */
    PokeRadar_ClearPatches();
    r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;
}
