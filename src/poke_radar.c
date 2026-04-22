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
 *   3. Stepping into a shaking patch triggers a wild encounter of a species
 *      that becomes locked as the "chained species" (subsequent patch-forced
 *      encounters reuse that species).
 *   4. Each successful KO or capture increments a chain counter (cap: 40).
 *   5. The chain counter grants extra shiny PID rerolls per encounter:
 *        rerolls = 1 + floor(min(chain, 40) / 5)
 *      giving 1 roll at chain 0, 2 at chain 5, ... and 8 at chain 40.
 *   6. The chain breaks on:
 *        - Engaging a different species in a wild encounter (patch or normal)
 *        - Fleeing / teleporting / losing the wild battle
 *        - Stepping off tall grass
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
 * HOOKS (files outside this module):
 *   - src/new_game.c       : grant item + init struct on new game
 *   - src/load_save.c      : ensure-init + grant item on pre-existing saves
 *   - src/item_use.c       : field-use handler (ItemUseOutOfBattle_PokeRadar)
 *   - src/wild_encounter.c : force species on patch encounter + inject shiny
 *   - src/battle_main.c    : chain increment / break on wild-battle exit
 *   - src/field_control_avatar.c : per-step tick for recharge + off-grass break
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "event_data.h"
#include "field_effect.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "item.h"
#include "metatile_behavior.h"
#include "overworld.h"
#include "random.h"
#include "poke_radar.h"
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

void PokeRadar_ClearPatches(void)
{
    struct PokeRadar *r = PokeRadar_Get();
    u8 i;

    if (!(r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE))
        return;

    for (i = 0; i < POKE_RADAR_PATCH_COUNT; i++)
    {
        u8 spriteId = r->patchSpriteId[i];
        if (spriteId < MAX_SPRITES && gSprites[spriteId].inUse)
        {
            /* FieldEffectStop frees the sprite AND releases its tile/palette
             * allocations; ActiveListRemove removes one entry for the fldeff.
             * Each of our patches was added separately via FieldEffectStart,
             * so we must Stop once per patch. */
            FieldEffectStop(&gSprites[spriteId], FLDEFF_UNUSED_GRASS);
        }
        r->patchSpriteId[i] = PATCH_SPRITE_NONE;
        r->patchX[i] = 0;
        r->patchY[i] = 0;
    }
    r->flags &= ~POKE_RADAR_FLAG_PATCHES_ACTIVE;
}

void PokeRadar_BreakChain(void)
{
    struct PokeRadar *r = PokeRadar_Get();
    PokeRadar_ClearPatches();
    r->chainCount = 0;
    r->chainSpecies = SPECIES_NONE;
    r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;
    r->chainMapGroup = 0;
    r->chainMapNum = 0;
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
    for (i = 0; i < POKE_RADAR_PATCH_COUNT; i++)
    {
        r->patchSpriteId[i] = PATCH_SPRITE_NONE;
        r->patchX[i] = 0;
        r->patchY[i] = 0;
    }

    for (i = 0, j = 0; i < POKE_RADAR_PATCH_COUNT && i < nCand; i++)
    {
        u32 spriteId;
        gFieldEffectArguments[0] = candX[i];
        gFieldEffectArguments[1] = candY[i];
        gFieldEffectArguments[2] = 0;   /* elevation */
        gFieldEffectArguments[3] = 2;   /* OAM priority */
        spriteId = FieldEffectStart(FLDEFF_UNUSED_GRASS);
        if (spriteId < MAX_SPRITES)
        {
            r->patchSpriteId[j] = (u8)spriteId;
            r->patchX[j] = candX[i];
            r->patchY[j] = candY[i];
            j++;
        }
    }

    if (j > 0)
        r->flags |= POKE_RADAR_FLAG_PATCHES_ACTIVE;
    r->flags &= ~POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;

    /* Consume the charge and arm the recharge counter. */
    if (r->charges > 0)
        r->charges--;
    r->stepsUntilCharge = POKE_RADAR_RECHARGE_STEPS;

    /* Record the map so the chain's map-binding is visible for diagnostics
     * (we do not currently break chain on map change per the design spec). */
    r->chainMapGroup = gSaveBlock1Ptr->location.mapGroup;
    r->chainMapNum = gSaveBlock1Ptr->location.mapNum;
}

void PokeRadar_OnStep(void)
{
    struct PokeRadar *r = PokeRadar_Get();
    s16 px, py;
    u8 behavior;

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

    /* Patch-step detection: if the player just stepped onto one of the
     * patch tiles, flag the next encounter as a forced patch encounter.
     * We also sweep the sprites now so the visual disappears instantly. */
    if ((r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE)
        && IsPositionOccupiedByPatch(r, px, py))
    {
        r->flags |= POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER;
        PokeRadar_ClearPatches();
        return;
    }

    /* Off-grass detection: stepping onto a non-tall-grass tile breaks the
     * chain. This is the primary chain-break vector besides wild-battle
     * outcomes. We only care if the player has an active chain or active
     * patches — otherwise it's a normal step with no radar state to clear. */
    if (!MetatileBehavior_IsTallGrass(behavior))
    {
        if (r->chainCount > 0 || (r->flags & POKE_RADAR_FLAG_PATCHES_ACTIVE))
            PokeRadar_BreakChain();
    }
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
