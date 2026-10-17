#ifndef GUARD_MT_MOON_GEN_H
#define GUARD_MT_MOON_GEN_H

/*
 * Procedurally generated Mt. Moon (custom feature, src/mt_moon_gen.c).
 *
 * Each visit to a generated Mt. Moon floor has a 16-bit seed stored in
 * VAR_MT_MOON_MAP_SEED, and the cave is built from that seed alone. These
 * are the only entry points the rest of the game uses; src/fieldmap.c calls
 * them right after the floor's vanilla layout has been copied into the map
 * buffer.
 */

/* TRUE when the map in gMapHeader is a Mt. Moon floor that gets generated. */
bool8 MtMoonGen_IsGeneratedMap(void);

/*
 * Fresh entry (door, ladder or warp; called from InitMap): roll a new seed
 * with Random(), build the floor from it, and put the player on the
 * generated tile of the warp they arrived through.
 */
void MtMoonGen_GenerateOnFreshEntry(void);

/*
 * Continue or quest-log replay (called from InitMapFromSavedGame): rebuild
 * the floor from the seed stored in the save, so it matches the cave the
 * player saved in. If the player's saved tile isn't reachable floor in the
 * rebuilt cave (for example a save made before seeds existed), move them to
 * the nearest reachable floor tile.
 */
void MtMoonGen_RegenerateFromSave(void);

#endif /* GUARD_MT_MOON_GEN_H */
