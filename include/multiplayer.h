#ifndef GUARD_MULTIPLAYER_H
#define GUARD_MULTIPLAYER_H

#include "global.h"

#define MP_MAX_PLAYERS 4
#define MP_MAX_REMOTE  (MP_MAX_PLAYERS - 1)

// Link command ID for multiplayer position broadcast
#define LINKCMD_MP_POSITION 0xAA00

// Flags packed into position packet
#define MP_FLAG_ACTIVE    (1 << 0)
#define MP_FLAG_IN_BATTLE (1 << 1)

struct RemotePlayer {
    u8 mapGroup;
    u8 mapNum;
    s16 x;           // target tile from network
    s16 y;
    s16 displayX;    // current display pixel pos
    s16 displayY;
    u8 direction;
    u8 graphicsId;
    u8 flags;
    u8 objEventId;
    u8 spriteSpawned;
};

extern bool8 gMultiplayerEnabled;

void InitMultiplayer(void);
void ShutdownMultiplayer(void);
void UpdateMultiplayerState(void);
void SpawnRemotePlayerSprites(void);
void DespawnRemotePlayerSprites(void);
bool8 IsMultiplayerActive(void);

#endif // GUARD_MULTIPLAYER_H
