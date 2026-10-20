#ifndef GUARD_POKEMON_FOLLOWER_H
#define GUARD_POKEMON_FOLLOWER_H

#include "global.h"

/*
 * Pokemon Follower System
 *
 * Allows a party pokemon to follow the player in the overworld.
 * The follower trails 1-2 tiles behind using a position history FIFO:
 * each time the player steps to a new tile, the old position is queued,
 * and the follower walks to queued positions with a delay.
 *
 * Only pokemon with overworld walking animation sprites (27 species)
 * are eligible. One follower at a time. Persists across map warps
 * (EWRAM state survives) but not across save/load.
 *
 * The follower is purely local -- it is NOT transmitted over the
 * multiplayer SIO protocol and does not appear on remote screens.
 */

void UpdateFollowerPokemon(void);
void SpawnFollowerSprite(void);
void DespawnFollowerSprite(void);
bool8 IsFollowerActive(void);
u8 GetFollowerObjEventId(void);
u8 GetFollowerPartySlot(void);
bool8 IsFollowerPokemon(struct Pokemon *mon);
bool8 CanSpeciesFollowPlayer(u16 species);
void ActivateFollower(u8 partySlot);
void DeactivateFollower(void);
void ClearFollowerFromSaveBlock(void);

/*
 * Swapping places (issue #99): the player's step code calls
 * CanPlayerSwapWithFollower where a step in `direction` is blocked by an
 * object; if it returns TRUE it takes its normal step and then calls
 * StartFollowerSwapStep(direction) in the same frame. See "SWAPPING PLACES"
 * in pokemon_follower.c.
 */
bool8 CanPlayerSwapWithFollower(u8 direction);
void StartFollowerSwapStep(u8 direction);

/*
 * Making way (issue #100): when the player is about to land on tile (x, y)
 * (object coordinates) moving in `direction`, by a ledge jump or a hop off
 * the water, GetFollowerMakeWayDirection says which way a follower standing
 * still there steps off it (DIR_NONE: it isn't there, or can't), and
 * StartFollowerMakeWayStep takes that step, on the frame the jump or hop
 * starts. See "MAKING WAY" in pokemon_follower.c.
 */
u8 GetFollowerMakeWayDirection(s16 x, s16 y, u8 direction);
void StartFollowerMakeWayStep(u8 stepDirection);

#endif /* GUARD_POKEMON_FOLLOWER_H */
