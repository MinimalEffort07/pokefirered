#ifndef GUARD_HALL_OF_FAME_H
#define GUARD_HALL_OF_FAME_H

#include "global.h"

/* The Hall of Fame is saved in flash sectors of its own (SECTOR_ID_HOF_1 and
 * SECTOR_ID_HOF_2 in save.h), as HALL_OF_FAME_MAX_TEAMS of these teams in a
 * row. They are in this header, not in hall_of_fame.c, so that
 * test/tools/save_format.py can see them: their layout is part of the save
 * format (CLAUDE.md, "Breaking changes"). */

/* Maximum number of Hall of Fame entries stored in flash memory.
 * After 50 entries, old ones are overwritten. */
#define HALL_OF_FAME_MAX_TEAMS 50

/**
 * HallofFameMon — data stored for each Pokemon in a Hall of Fame entry.
 *
 * Uses bitfields to pack species (9 bits = 0-511, enough for Gen 3's 386
 * species) and level (7 bits = 0-127, enough for max level 100) into a
 * single 16-bit word, saving precious flash storage space.
 */
struct HallofFameMon
{
    u32 tid;                           /* Original Trainer ID (for shiny check) */
    u32 personality;                   /* Personality value (for sprite variant) */
    u16 species:9;                     /* Bitfield: Pokemon species (0-511) */
    u16 lvl:7;                         /* Bitfield: Pokemon level (0-127) */
    u8 nick[POKEMON_NAME_LENGTH];      /* Nickname string */
};

/**
 * HallofFameTeam — one complete team entry (up to 6 Pokemon).
 * Multiple of these are stored sequentially in flash memory.
 */
struct HallofFameTeam
{
    struct HallofFameMon mon[PARTY_SIZE];
};

void CB2_DoHallOfFameScreen(void);
void CB2_DoHallOfFameScreenDontSaveData(void);
void CB2_DoHallOfFamePC(void);
void CB2_InitHofPC(void);
void HallOfFamePCBeginFade(void);
void ReturnFromHallOfFamePC(void);

#endif // GUARD_HALL_OF_FAME_H
