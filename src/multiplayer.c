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

// Magic tag stored in sprite->data[7] to identify our remote player sprites
#define MP_SPRITE_TAG 0x4D50  // "MP"

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
        sprite->data[7] = MP_SPRITE_TAG;  // tag so we can identify our sprites
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
    // Only destroy if the ObjectEvent still belongs to us
    if (objEvent->active && objEvent->spriteId < MAX_SPRITES)
    {
        struct Sprite *sprite = &gSprites[objEvent->spriteId];
        if (sprite->inUse)
        {
            sprite->callback = SpriteCallbackDummy;
            DestroySprite(sprite);
        }
        objEvent->spriteId = MAX_SPRITES;
    }
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

    if ((dx == 0 && dy == 0) || (rp->flags & MP_FLAG_IN_BATTLE))
    {
        // At target or in battle - stay still
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
        s16 speed;

        switch (rp->direction)
        {
        case DIR_SOUTH: directionMatches = (moveY > 0); break;
        case DIR_NORTH: directionMatches = (moveY < 0); break;
        case DIR_WEST:  directionMatches = (moveX < 0); break;
        case DIR_EAST:  directionMatches = (moveX > 0); break;
        }

        if (directionMatches)
        {
            // Move faster when further behind to stay in sync
            // (compensates for 3-frame transmission delay)
            speed = (abs(dx) + abs(dy) > 8) ? 2 : 1;
            rp->displayX += moveX * speed;
            rp->displayY += moveY * speed;

            // Don't overshoot
            if ((moveX > 0 && rp->displayX > targetPixelX) ||
                (moveX < 0 && rp->displayX < targetPixelX))
                rp->displayX = targetPixelX;
            if ((moveY > 0 && rp->displayY > targetPixelY) ||
                (moveY < 0 && rp->displayY < targetPixelY))
                rp->displayY = targetPixelY;
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

    if (rp->flags & MP_FLAG_IN_BATTLE)
    {
        // In battle - frozen facing pose
        StartSpriteAnimIfDifferent(sprite, GetFaceDirectionAnimNum(rp->direction));
    }
    else if (isMoving)
    {
        StartSpriteAnimIfDifferent(sprite, GetMoveDirectionAnimNum(rp->direction));
    }
    else
    {
        StartSpriteAnimIfDifferent(sprite, GetFaceDirectionAnimNum(rp->direction));
    }

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

    // Re-initialize SIO every frame in case battle/menu reset it
    SetSerialCallback(MP_SerialCallback);
    REG_RCNT = 0;
    REG_SIOCNT = SIO_MULTI_MODE | SIO_115200_BPS | SIO_INTR_ENABLE;
    EnableInterrupts(INTR_FLAG_SERIAL);

    sLocalId = ReadLocalId();

    // Check for sprites destroyed by battle/menu transitions.
    // Use the magic tag in sprite data[7] to verify the sprite still belongs to us.
    for (i = 0; i < MP_MAX_REMOTE; i++)
    {
        if (sRemotePlayers[i].spriteSpawned)
        {
            struct ObjectEvent *obj = &gObjectEvents[sRemotePlayers[i].objEventId];
            bool8 valid = FALSE;

            if (obj->active && obj->spriteId < MAX_SPRITES)
            {
                struct Sprite *spr = &gSprites[obj->spriteId];
                if (spr->inUse && spr->data[7] == MP_SPRITE_TAG)
                    valid = TRUE;
            }

            if (!valid)
                sRemotePlayers[i].spriteSpawned = FALSE;
        }
    }

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
                // Type 2 (bit15=1): gfxId(7)+dir(4)+inBattle(1)+active(1)
                rp->graphicsId = (recv >> 8) & 0x7F;
                rp->direction = (recv >> 4) & 0xF;
                rp->flags = recv & 0x3;  // bits 0-1: active + inBattle

                // Full cycle received - update sprite visibility
                if (rp->flags & MP_FLAG_ACTIVE)
                {
                    if (IsRemotePlayerOnSameMap(remoteIdx))
                    {
                        if (!rp->spriteSpawned)
                            SpawnRemoteSprite(remoteIdx);
                    }
                    else
                        DespawnRemoteSprite(remoteIdx);
                }
                else
                    DespawnRemoteSprite(remoteIdx);
            }
            else if (recv & (1 << 14))
            {
                // Type 1 (bits15-14=01): position - y(7)+x(7)
                rp->y = (recv >> 7) & 0x7F;
                rp->x = recv & 0x7F;
            }
            else
            {
                // Type 0 (bits15-14=00): map - mapNum(7)+mapGroup(6)
                rp->mapNum = (recv >> 7) & 0x7F;
                rp->mapGroup = recv & 0x3F;
            }
        }
    }

    // Update all spawned remote sprites every frame (for smooth movement)
    for (i = 0; i < MP_MAX_REMOTE; i++)
        if (sRemotePlayers[i].spriteSpawned)
            UpdateRemoteSprite(i);

    // Send our data
    // Even frame (bit15=0): bits14-9=mapGroup(6), bits8-6=x(3 high), bits5-0=mapNum_low(6)
    // Odd frame  (bit15=1): bits14-11=x_low4+y_high1, bits10-4=y(7), bits3-1=dir(3), bit0=active
    //
    // Simpler packing:
    //   Even: bit15=0, bits14-8=mapNum(7), bits7-6=mapGroup(2 low), bits5-0=x(6)
    //   Odd:  bit15=1, bits14-8=y(7), bits7-4=gfxId_low(4), bits3-1=dir(3), bit0=active
    //
    // This gives: mapNum 0-127, mapGroup low 2 bits, x 0-63, y 0-127, gfxId 0-15, dir 0-7
    // For full mapGroup+mapNum we use a 3-frame cycle instead.
    //
    // Actually let's just use 3 frame types:
    //   Frame%3==0: bit15=0,bit14=0: mapGroup(6)+mapNum(7) = 13 bits
    //   Frame%3==1: bit15=0,bit14=1: x(7)+y(7) = 14 bits
    //   Frame%3==2: bit15=1:         gfxId(7)+dir(3)+active(1) = 11 bits

    switch (sFrame % 3)
    {
    case 0:
        // Map info: bits15-14=00, bits13-7=mapNum(7), bits6-0=mapGroup(7... only need 6)
        sendVal = ((gSaveBlock1Ptr->location.mapNum & 0x7F) << 7)
                | (gSaveBlock1Ptr->location.mapGroup & 0x3F);
        break;
    case 1:
        // Position: bits15-14=01, bits13-7=y(7), bits6-0=x(7)
        sendVal = (1 << 14)
                | ((gSaveBlock1Ptr->pos.y & 0x7F) << 7)
                | (gSaveBlock1Ptr->pos.x & 0x7F);
        break;
    case 2:
    default:
        // Appearance: bit15=1, bits14-8=gfxId(7), bits7-4=dir(4), bit1=inBattle, bit0=active
        {
            u8 flags = MP_FLAG_ACTIVE;
            if (gMain.inBattle)
                flags |= MP_FLAG_IN_BATTLE;
            sendVal = PKT_TYPE_DIR
                    | ((gSaveBlock2Ptr->playerAvatarGfxId & 0x7F) << 8)
                    | ((GetPlayerFacingDirection() & 0xF) << 4)
                    | flags;
        }
        break;
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
