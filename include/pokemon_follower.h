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

#endif /* GUARD_POKEMON_FOLLOWER_H */
