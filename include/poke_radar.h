#ifndef GUARD_POKE_RADAR_H
#define GUARD_POKE_RADAR_H

// Gen 4 PokeRadar mechanic: using the key item in grass spawns four shaking
// grass patches. Stepping into a patch forces a wild encounter of a chained
// species. Repeatedly defeating/catching the same species builds a chain that
// raises the number of shiny PID rerolls on subsequent chained encounters.
//
// State lives in SaveBlock1::unused_348C[] (pre-existing 400-byte slack), so
// the save format size is unchanged. A magic number detects first-run and
// triggers zero-init for saves that predate this feature.

#define POKE_RADAR_MAX_CHAIN          40
#define POKE_RADAR_RECHARGE_STEPS     50
#define POKE_RADAR_PATCH_COUNT         4
#define POKE_RADAR_PATCH_RADIUS        6   // tile radius around player

#define POKE_RADAR_MAGIC                     0x50524452u  // 'PRDR'
#define POKE_RADAR_FLAG_PATCHES_ACTIVE       (1 << 0)
#define POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER (1 << 1)

struct PokeRadar
{
    /* 0x00 */ u32 magic;                // POKE_RADAR_MAGIC once initialized
    /* 0x04 */ u16 chainCount;           // 0..POKE_RADAR_MAX_CHAIN
    /* 0x06 */ u16 chainSpecies;         // SPECIES_NONE when chain inactive
    /* 0x08 */ u8  stepsUntilCharge;     // 0..POKE_RADAR_RECHARGE_STEPS
    /* 0x09 */ u8  charges;              // 0 or 1
    /* 0x0A */ u8  flags;                // POKE_RADAR_FLAG_*
    /* 0x0B */ u8  chainMapGroup;        // diagnostic only
    /* 0x0C */ u8  chainMapNum;          // diagnostic only
    /* 0x0D */ u8  padding;
    /* 0x0E */ s16 patchX[POKE_RADAR_PATCH_COUNT];
    /* 0x16 */ s16 patchY[POKE_RADAR_PATCH_COUNT];
    /* 0x1E */ u8  patchSpriteId[POKE_RADAR_PATCH_COUNT];
    /* 0x22 */ u8  reserved[2];
}; // size = 0x24 (36 bytes; fits in SaveBlock1::unused_348C[400])

struct PokeRadar *PokeRadar_Get(void);
bool8 PokeRadar_IsUsableHere(void);
void  PokeRadar_Activate(void);
void  PokeRadar_OnStep(void);
void  PokeRadar_OnEncounterStart(u16 species);
bool8 PokeRadar_TryInjectShiny(u16 species, u32 *personalityOut);
void  PokeRadar_OnBattleEnd(u8 outcome);
void  PokeRadar_ClearPatches(void);
void  PokeRadar_BreakChain(void);
void  PokeRadar_ResetOnNewGame(void);
void  PokeRadar_EnsureInit(void);

#endif // GUARD_POKE_RADAR_H
