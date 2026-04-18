#ifndef GUARD_ROAMING_POKEMON_H
#define GUARD_ROAMING_POKEMON_H

#include "global.h"

/*
 * Roaming Pokemon System
 *
 * Spawns visible wild Pokemon as object events on the overworld so the
 * player can see them wandering around. Walking into one starts a wild
 * battle. Flying-type pokemon additionally fly across the screen as
 * non-interactable flybys (raw sprites that do not consume an object
 * event slot).
 *
 * This is purely additive: random grass-step encounters still fire as
 * usual. Roaming pokemon are an extra battle entry-point that uses the
 * same per-map encounter tables (gWildMonHeaders) for species/level rolls.
 *
 * Lifecycle hooks (called from src/overworld.c):
 *   - SpawnRoamingPokemonOnMap()  on map load and return-to-field
 *   - DespawnAllRoamingPokemon()  before warps
 *   - UpdateRoamingPokemon()      every frame in CB1_Overworld
 *   - UpdateRoamingFlybys()       every frame in CB1_Overworld
 *
 * Bump-to-battle hook (called from src/field_player_avatar.c):
 *   - IsRoamingPokemonObjectEvent(id)  cheap test inside collision check
 *   - TryStartRoamingBattle(id)        sets up the wild battle
 *
 * State is EWRAM-only and session-only (does not touch save format).
 */

void SpawnRoamingPokemonOnMap(void);
void DespawnAllRoamingPokemon(void);
void UpdateRoamingPokemon(void);
void UpdateRoamingFlybys(void);
bool8 IsRoamingPokemonObjectEvent(u8 objEventId);
void TryStartRoamingBattle(u8 objEventId);

#endif /* GUARD_ROAMING_POKEMON_H */
