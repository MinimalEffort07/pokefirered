#include "global.h"
#include "gflib.h"
#include "multiplayer.h"
#include "task.h"
#include "main.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "overworld.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"

#define SIO_MULTI_MODE    0x2000
#define SIO_115200_BPS    0x0003
#define SIO_INTR_ENABLE   0x4000
#define SIO_START_BIT     0x0080
#define SIO_MULTI_BUSY    0x0080
#define SIO_ID_BITS       0x0030
#define SIO_ID_SHIFT      4

#define PKT_TYPE_MASK  0x8000
#define PKT_TYPE_POS   0x0000
#define PKT_TYPE_DIR   0x8000

static EWRAM_DATA struct RemotePlayer sRemotePlayers[MP_MAX_REMOTE] = {0};
EWRAM_DATA bool8 gMultiplayerEnabled = FALSE;
static EWRAM_DATA u8 sLocalId = 0;
static EWRAM_DATA u8 sFrame = 0;
static EWRAM_DATA u16 sRecvBuffer[MP_MAX_PLAYERS] = {0};
static EWRAM_DATA bool8 sTransferDone = FALSE;

static void SpriteCB_RemotePlayer(struct Sprite *sprite);

static void MP_SerialCallback(void)
{
    sRecvBuffer[0] = REG_SIOMULTI0;
    sRecvBuffer[1] = REG_SIOMULTI1;
    sRecvBuffer[2] = REG_SIOMULTI2;
    sRecvBuffer[3] = REG_SIOMULTI3;
    sTransferDone = TRUE;
}

static u8 ReadLocalId(void)
{
    return (REG_SIOCNT & SIO_ID_BITS) >> SIO_ID_SHIFT;
}

static void InitSIO(void)
{
    REG_RCNT = 0;
    REG_SIOCNT = 0;
    REG_SIOCNT = SIO_MULTI_MODE | SIO_115200_BPS | SIO_INTR_ENABLE;
    REG_SIOMLT_SEND = 0;
    EnableInterrupts(INTR_FLAG_SERIAL);
    SetSerialCallback(MP_SerialCallback);
}

bool8 IsMultiplayerActive(void)
{
    return gMultiplayerEnabled;
}

static bool8 IsRemotePlayerOnSameMap(u8 remoteIdx)
{
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    return (rp->flags & MP_FLAG_ACTIVE)
        && rp->mapGroup == gSaveBlock1Ptr->location.mapGroup
        && rp->mapNum == gSaveBlock1Ptr->location.mapNum;
}

static void SpawnRemoteSprite(u8 remoteIdx)
{
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    u8 objEventId;
    struct ObjectEvent *objEvent;
    struct Sprite *sprite;
    u8 graphicsId;
    s16 px, py;

    if (rp->spriteSpawned)
        return;

    objEventId = GetFirstInactiveObjectEventId();
    if (objEventId >= OBJECT_EVENTS_COUNT)
        return;

    rp->objEventId = objEventId;
    objEvent = &gObjectEvents[objEventId];

    memset(objEvent, 0, sizeof(struct ObjectEvent));
    objEvent->active = TRUE;
    objEvent->currentCoords.x = rp->x + MAP_OFFSET;
    objEvent->currentCoords.y = rp->y + MAP_OFFSET;
    objEvent->previousCoords.x = rp->x + MAP_OFFSET;
    objEvent->previousCoords.y = rp->y + MAP_OFFSET;
    objEvent->facingDirection = rp->direction;
    objEvent->spriteId = MAX_SPRITES;

    SetSpritePosToMapCoords(rp->x + MAP_OFFSET, rp->y + MAP_OFFSET, &px, &py);
    objEvent->initialCoords.x = px + 8;
    objEvent->initialCoords.y = py;

    graphicsId = rp->graphicsId;
    if (graphicsId == 0 || graphicsId >= NUM_OBJ_EVENT_GFX)
        graphicsId = OBJ_EVENT_GFX_RED_NORMAL;

    objEvent->graphicsId = graphicsId;
    objEvent->spriteId = CreateObjectGraphicsSprite(graphicsId, SpriteCB_RemotePlayer, 0, 0, 0);

    if (objEvent->spriteId != MAX_SPRITES)
    {
        sprite = &gSprites[objEvent->spriteId];
        sprite->coordOffsetEnabled = TRUE;
        sprite->data[0] = remoteIdx;
        rp->spriteSpawned = TRUE;
        rp->displayX = objEvent->initialCoords.x;
        rp->displayY = objEvent->initialCoords.y;
    }
}

static void DespawnRemoteSprite(u8 remoteIdx)
{
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    struct ObjectEvent *objEvent;

    if (!rp->spriteSpawned)
        return;

    objEvent = &gObjectEvents[rp->objEventId];
    if (objEvent->spriteId != MAX_SPRITES)
        DestroySprite(&gSprites[objEvent->spriteId]);

    objEvent->active = FALSE;
    rp->spriteSpawned = FALSE;
}

// Move display position toward target tile.
// Only animates smoothly when moving FORWARD in the facing direction.
// Snaps immediately on direction changes to prevent backward skating.
static void UpdateRemoteSprite(u8 remoteIdx)
{
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    struct ObjectEvent *objEvent;
    s16 targetPixelX, targetPixelY;
    s16 dx, dy;
    s16 moveX, moveY;

    if (!rp->spriteSpawned)
        return;

    objEvent = &gObjectEvents[rp->objEventId];
    objEvent->facingDirection = rp->direction;

    SetSpritePosToMapCoords(rp->x + MAP_OFFSET, rp->y + MAP_OFFSET,
        &targetPixelX, &targetPixelY);
    targetPixelX += 8;

    dx = targetPixelX - rp->displayX;
    dy = targetPixelY - rp->displayY;

    if (dx == 0 && dy == 0)
    {
        // Already at target - just update coords
        objEvent->currentCoords.x = rp->x + MAP_OFFSET;
        objEvent->currentCoords.y = rp->y + MAP_OFFSET;
        objEvent->initialCoords.x = rp->displayX;
        objEvent->initialCoords.y = rp->displayY;
        return;
    }

    // Determine which pixel direction we NEED to move
    if (abs(dx) >= abs(dy))
    {
        moveX = (dx > 0) ? 1 : -1;
        moveY = 0;
    }
    else
    {
        moveX = 0;
        moveY = (dy > 0) ? 1 : -1;
    }

    // Check if the movement direction matches the facing direction.
    // If not, snap to target to avoid backward walking.
    {
        bool8 directionMatches = FALSE;

        switch (rp->direction)
        {
        case DIR_SOUTH: directionMatches = (moveY > 0); break;
        case DIR_NORTH: directionMatches = (moveY < 0); break;
        case DIR_WEST:  directionMatches = (moveX < 0); break;
        case DIR_EAST:  directionMatches = (moveX > 0); break;
        }

        if (directionMatches)
        {
            // Moving forward in facing direction - smooth 1px step
            rp->displayX += moveX;
            rp->displayY += moveY;
        }
        else
        {
            // Direction mismatch - snap to target instantly
            rp->displayX = targetPixelX;
            rp->displayY = targetPixelY;
        }
    }

    objEvent->currentCoords.x = rp->x + MAP_OFFSET;
    objEvent->currentCoords.y = rp->y + MAP_OFFSET;
    objEvent->initialCoords.x = rp->displayX;
    objEvent->initialCoords.y = rp->displayY;
}

static void SpriteCB_RemotePlayer(struct Sprite *sprite)
{
    u8 remoteIdx = sprite->data[0];
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    struct ObjectEvent *objEvent = &gObjectEvents[rp->objEventId];
    s16 targetPixelX, targetPixelY;
    bool8 isMoving;

    sprite->x = objEvent->initialCoords.x;
    sprite->y = objEvent->initialCoords.y;
    sprite->oam.priority = 2;

    SetSpritePosToMapCoords(rp->x + MAP_OFFSET, rp->y + MAP_OFFSET,
        &targetPixelX, &targetPixelY);
    targetPixelX += 8;
    isMoving = (rp->displayX != targetPixelX || rp->displayY != targetPixelY);

    if (isMoving)
        StartSpriteAnimIfDifferent(sprite, GetMoveDirectionAnimNum(rp->direction));
    else
        StartSpriteAnimIfDifferent(sprite, GetFaceDirectionAnimNum(rp->direction));

    UpdateObjectEventSpriteInvisibility(sprite, FALSE);
}

void InitMultiplayer(void)
{
    u8 i;
    gMultiplayerEnabled = TRUE;
    sFrame = 0;
    sTransferDone = FALSE;
    for (i = 0; i < MP_MAX_PLAYERS; i++)
        sRecvBuffer[i] = 0xFFFF;
    for (i = 0; i < MP_MAX_REMOTE; i++)
        memset(&sRemotePlayers[i], 0, sizeof(struct RemotePlayer));
    InitSIO();
}

void Special_StartMultiplayer(void)
{
    if (gMultiplayerEnabled)
        return;
    InitMultiplayer();
}

void ShutdownMultiplayer(void)
{
    u8 i;
    for (i = 0; i < MP_MAX_REMOTE; i++)
        DespawnRemoteSprite(i);
    gMultiplayerEnabled = FALSE;
}

void UpdateMultiplayerState(void)
{
    u8 i;
    u16 sendVal;

    if (!gMultiplayerEnabled)
        return;

    sLocalId = ReadLocalId();

    if (sTransferDone)
    {
        sTransferDone = FALSE;

        for (i = 0; i < MP_MAX_PLAYERS; i++)
        {
            u16 recv;
            u8 remoteIdx;
            struct RemotePlayer *rp;

            if (i == sLocalId)
                continue;

            remoteIdx = (i < sLocalId) ? i : i - 1;
            if (remoteIdx >= MP_MAX_REMOTE)
                continue;

            rp = &sRemotePlayers[remoteIdx];
            recv = sRecvBuffer[i];

            if (recv == 0xFFFF || recv == 0)
            {
                rp->flags &= ~MP_FLAG_ACTIVE;
                DespawnRemoteSprite(remoteIdx);
                continue;
            }

            if (recv & PKT_TYPE_MASK)
            {
                // Odd frame: bit15=1, bits14-8=y(7), bit7=active, bits6-0=gfxId(7)
                rp->y = (recv >> 8) & 0x7F;
                rp->flags = (recv >> 7) & 0x1;
                rp->graphicsId = recv & 0x7F;

                if (rp->flags & MP_FLAG_ACTIVE)
                {
                    rp->mapGroup = gSaveBlock1Ptr->location.mapGroup;
                    if (IsRemotePlayerOnSameMap(remoteIdx))
                    {
                        if (!rp->spriteSpawned)
                            SpawnRemoteSprite(remoteIdx);
                    }
                    else
                        DespawnRemoteSprite(remoteIdx);
                }
            }
            else
            {
                // Even frame: bit15=0, bits14-10=mapNum(5), bits9-7=dir(3), bits6-0=x(7)
                rp->mapNum = (recv >> 10) & 0x1F;
                rp->direction = (recv >> 7) & 0x7;
                rp->x = recv & 0x7F;
            }
        }
    }

    // Update all spawned remote sprites every frame (for smooth movement)
    for (i = 0; i < MP_MAX_REMOTE; i++)
        if (sRemotePlayers[i].spriteSpawned)
            UpdateRemoteSprite(i);

    // Send our data
    // Even frame (bit15=0): bits14-10=mapNum(5), bits9-7=direction(3), bits6-0=x(7)
    // Odd frame  (bit15=1): bits14-8=y(7), bit7=active, bits6-0=gfxId(7)
    if (sFrame & 1)
    {
        sendVal = PKT_TYPE_DIR
                | ((gSaveBlock1Ptr->pos.y & 0x7F) << 8)
                | (1 << 7)  // active flag
                | (gSaveBlock2Ptr->playerAvatarGfxId & 0x7F);
    }
    else
    {
        sendVal = PKT_TYPE_POS
                | ((gSaveBlock1Ptr->location.mapNum & 0x1F) << 10)
                | ((GetPlayerFacingDirection() & 0x7) << 7)
                | (gSaveBlock1Ptr->pos.x & 0x7F);
    }

    REG_SIOMLT_SEND = sendVal;
    if (sLocalId == 0 && !(REG_SIOCNT & SIO_MULTI_BUSY))
        REG_SIOCNT |= SIO_START_BIT;

    sFrame++;
}

void SpawnRemotePlayerSprites(void)
{
    u8 i;
    if (!gMultiplayerEnabled)
        return;
    for (i = 0; i < MP_MAX_REMOTE; i++)
        if (IsRemotePlayerOnSameMap(i) && !sRemotePlayers[i].spriteSpawned)
            SpawnRemoteSprite(i);
}

void DespawnRemotePlayerSprites(void)
{
    u8 i;
    for (i = 0; i < MP_MAX_REMOTE; i++)
        DespawnRemoteSprite(i);
}
