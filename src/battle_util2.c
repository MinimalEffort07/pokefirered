/**
 * @file battle_util2.c
 * @brief Battle Resource Management and Friendship Penalties
 *
 * FILE OVERVIEW:
 * This file handles two key battle utilities:
 *
 * 1. BATTLE MEMORY MANAGEMENT: Allocates and frees all the large data structures
 *    needed during battle. Since the GBA has limited RAM, these structures are
 *    dynamically allocated from the heap when a battle starts and freed when it ends.
 *
 * 2. FRIENDSHIP PENALTIES: Adjusts a Pokemon's friendship (happiness) when it
 *    faints in battle. Losing a Pokemon to a stronger opponent has a bigger
 *    friendship penalty than losing to a same-level opponent.
 *
 * BATTLE MEMORY ARCHITECTURE:
 * A Pokemon battle requires many large data structures that would waste RAM if
 * they were always allocated. Instead, they're dynamically allocated at battle
 * start and freed at battle end:
 * - gBattleStruct: Core battle state (turns, selected moves, etc.)
 * - gBattleResources: AI state, script stacks, battle history, stat snapshots
 * - gLinkBattle*Buffer: Send/receive buffers for link multiplayer battles
 * - gBattleAnim*Buffer: Tile and tilemap buffers for battle move animations
 */
#include "global.h"
#include "bg.h"
#include "battle.h"
#include "battle_anim.h"
#include "malloc.h"
#include "pokemon.h"
#include "trainer_tower.h"

/**
 * FUNCTION: AllocateBattleResources
 *
 * PURPOSE: Allocates all dynamically-sized battle data structures from the heap.
 * Called at the start of every battle. All allocations are zero-initialized
 * (AllocZeroed) to prevent stale data from previous battles.
 *
 * HOW IT WORKS:
 * Allocates the core battle struct, all sub-structures of gBattleResources,
 * link battle buffers (for multiplayer), and animation buffers. Also sets
 * BG1 and BG2 tilemap buffers to the animation tilemap buffer for battle
 * animation rendering.
 *
 * Special cases:
 * - Trainer Tower battles get additional structures
 * - Pokedude (tutorial) battles allocate per-battler state arrays
 */
void AllocateBattleResources(void)
{
    if (gBattleTypeFlags & BATTLE_TYPE_TRAINER_TOWER)
        InitTrainerTowerBattleStruct();
    if (gBattleTypeFlags & BATTLE_TYPE_POKEDUDE)
    {
        s32 i;

        /* Allocate state for each of the 4 battle positions (tutorial mode) */
        for (i = 0; i < 4; i++)
            gPokedudeBattlerStates[i] = AllocZeroed(sizeof(struct PokedudeBattlerState));
    }

    /* Core battle state structure */
    gBattleStruct = AllocZeroed(sizeof(*gBattleStruct));

    /* Battle resources: AI data, script stacks, history tracking */
    gBattleResources = AllocZeroed(sizeof(*gBattleResources));
    gBattleResources->secretBase = AllocZeroed(sizeof(*gBattleResources->secretBase));
    gBattleResources->flags = AllocZeroed(sizeof(*gBattleResources->flags));
    gBattleResources->battleScriptsStack = AllocZeroed(sizeof(*gBattleResources->battleScriptsStack));
    gBattleResources->battleCallbackStack = AllocZeroed(sizeof(*gBattleResources->battleCallbackStack));
    gBattleResources->beforeLvlUp = AllocZeroed(sizeof(*gBattleResources->beforeLvlUp));
    gBattleResources->ai = AllocZeroed(sizeof(*gBattleResources->ai));
    gBattleResources->battleHistory = AllocZeroed(sizeof(*gBattleResources->battleHistory));
    gBattleResources->AI_ScriptsStack = AllocZeroed(sizeof(*gBattleResources->AI_ScriptsStack));

    /* Link battle communication buffers */
    gLinkBattleSendBuffer = AllocZeroed(BATTLE_BUFFER_LINK_SIZE);
    gLinkBattleRecvBuffer = AllocZeroed(BATTLE_BUFFER_LINK_SIZE);

    /* Animation rendering buffers: 0x2000 for tiles, 0x1000 for tilemap */
    gBattleAnimBgTileBuffer = AllocZeroed(0x2000);
    gBattleAnimBgTilemapBuffer = AllocZeroed(0x1000);

    /* Point BG1 and BG2 tilemap buffers to the animation buffer */
    SetBgTilemapBuffer(1, gBattleAnimBgTilemapBuffer);
    SetBgTilemapBuffer(2, gBattleAnimBgTilemapBuffer);
}

/**
 * FUNCTION: FreeBattleResources
 *
 * PURPOSE: Frees all dynamically-allocated battle data structures.
 * Called when the battle ends and we're returning to the overworld.
 *
 * HOW IT WORKS:
 * Uses FREE_AND_SET_NULL macro which frees the memory and sets the pointer
 * to NULL. This prevents double-free bugs and use-after-free crashes.
 * The NULL check on gBattleResources prevents freeing if allocation failed.
 */
void FreeBattleResources(void)
{
    if (gBattleTypeFlags & BATTLE_TYPE_TRAINER_TOWER)
        FreeTrainerTowerBattleStruct();
    if (gBattleTypeFlags & BATTLE_TYPE_POKEDUDE)
    {
        s32 i;

        for (i = 0; i < 4; i++)
        {
            FREE_AND_SET_NULL(gPokedudeBattlerStates[i]);
        }
    }
    if (gBattleResources != NULL)
    {
        FREE_AND_SET_NULL(gBattleStruct);

        FREE_AND_SET_NULL(gBattleResources->secretBase);
        FREE_AND_SET_NULL(gBattleResources->flags);
        FREE_AND_SET_NULL(gBattleResources->battleScriptsStack);
        FREE_AND_SET_NULL(gBattleResources->battleCallbackStack);
        FREE_AND_SET_NULL(gBattleResources->beforeLvlUp);
        FREE_AND_SET_NULL(gBattleResources->ai);
        FREE_AND_SET_NULL(gBattleResources->battleHistory);
        FREE_AND_SET_NULL(gBattleResources->AI_ScriptsStack);
        FREE_AND_SET_NULL(gBattleResources);

        FREE_AND_SET_NULL(gLinkBattleSendBuffer);
        FREE_AND_SET_NULL(gLinkBattleRecvBuffer);

        FREE_AND_SET_NULL(gBattleAnimBgTileBuffer);
        FREE_AND_SET_NULL(gBattleAnimBgTilemapBuffer);
    }
}

/**
 * FUNCTION: AdjustFriendshipOnBattleFaint
 *
 * PURPOSE: Reduces a Pokemon's friendship (happiness) when it faints in battle.
 * The penalty depends on the level difference between the fainted Pokemon and
 * the opponent.
 *
 * GAME LOGIC:
 * Pokemon have a hidden "friendship" value (0-255) that affects:
 * - Return/Frustration move power
 * - Certain evolution requirements (e.g., Golbat -> Crobat)
 * - Various NPC reactions
 *
 * Friendship DECREASES when a Pokemon faints:
 * - If the opponent is 30+ levels higher: FRIENDSHIP_EVENT_FAINT_LARGE (bigger penalty)
 * - Otherwise: FRIENDSHIP_EVENT_FAINT_SMALL (smaller penalty)
 *
 * In double battles, the stronger of the two opponents is used for the level comparison.
 *
 * @param battlerId — the battle position of the Pokemon that fainted
 */
void AdjustFriendshipOnBattleFaint(u8 battlerId)
{
    u8 opposingBattlerId;

    if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
    {
        /* In double battles, compare against the stronger opponent */
        u8 opposingBattlerId2;

        opposingBattlerId = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
        opposingBattlerId2 = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);

        if (gBattleMons[opposingBattlerId2].level > gBattleMons[opposingBattlerId].level)
            opposingBattlerId = opposingBattlerId2;
    }
    else
    {
        opposingBattlerId = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
    }

    /* Apply friendship penalty based on level difference */
    if (gBattleMons[opposingBattlerId].level > gBattleMons[battlerId].level)
    {
        if (gBattleMons[opposingBattlerId].level - gBattleMons[battlerId].level > 29)
            AdjustFriendship(&gPlayerParty[gBattlerPartyIndexes[battlerId]], FRIENDSHIP_EVENT_FAINT_LARGE);
        else
            AdjustFriendship(&gPlayerParty[gBattlerPartyIndexes[battlerId]], FRIENDSHIP_EVENT_FAINT_SMALL);
    }
    else
    {
        /* Opponent is same level or lower — still penalize, but with small amount */
        AdjustFriendship(&gPlayerParty[gBattlerPartyIndexes[battlerId]], FRIENDSHIP_EVENT_FAINT_SMALL);
    }
}
