/**
 * multiplayer.c - Custom Real-Time Multiplayer System via GBA Serial I/O
 *
 * ============================================================================
 * CUSTOM MULTIPLAYER OVERVIEW
 * ============================================================================
 *
 * This file implements a custom multiplayer system that allows up to 4 GBAs
 * connected via link cable to see each other's player avatars in the overworld
 * in real time. This is NOT the standard Pokemon link/trade system -- it's a
 * custom addition that makes other players appear as walking sprites on the map.
 *
 * HOW IT WORKS (HIGH LEVEL):
 *
 * 1. Each GBA sends its player state (map, position, direction, graphics)
 *    to all other connected GBAs every frame via the SIO Multi-Player mode.
 *
 * 2. Each GBA receives the other players' states and creates/updates sprite
 *    objects for them on the local screen.
 *
 * 3. Sprites are smoothly interpolated between received positions to create
 *    fluid movement despite the transmission delay.
 *
 * ============================================================================
 * GBA SERIAL I/O (SIO) MULTI-PLAYER MODE
 * ============================================================================
 *
 * The GBA's serial port supports a "Multi-Player" mode that allows up to 4
 * GBAs to communicate simultaneously. Key hardware details:
 *
 * REGISTERS:
 *   REG_RCNT (0x04000134): Communication mode selector. Set to 0 for SIO mode.
 *   REG_SIOCNT (0x04000128): Serial I/O Control register.
 *     Bits 0-1: Baud rate (0=9600, 1=38400, 2=57600, 3=115200)
 *     Bit 7:    Start bit (master sets this to begin a transfer)
 *     Bits 4-5: Local player ID (read-only, hardware-assigned: 0=master, 1-3=slaves)
 *     Bit 13:   Multi-player mode enable
 *     Bit 14:   Interrupt enable (fire IRQ when transfer completes)
 *   REG_SIOMLT_SEND (0x0400012A): 16-bit value this GBA sends during the next transfer
 *   REG_SIOMULTI0-3 (0x04000120-0x04000126): Received data from players 0-3
 *
 * HOW A TRANSFER WORKS:
 * 1. Each GBA writes its 16-bit send value to REG_SIOMLT_SEND
 * 2. The master (player 0) sets SIO_START_BIT in REG_SIOCNT
 * 3. All 4 GBAs simultaneously exchange their 16-bit values
 * 4. When complete, a serial interrupt fires (if enabled)
 * 5. Each GBA can now read all 4 values from REG_SIOMULTI0-3
 *
 * This means each GBA gets 16 bits of data per transfer. With 60 transfers
 * per second (one per frame), that's 960 bits/sec of data from each player.
 *
 * ============================================================================
 * PACKET FORMAT (16 bits per frame, 3-frame cycle)
 * ============================================================================
 *
 * Since 16 bits isn't enough to send all player state at once, data is split
 * across a 3-frame cycle:
 *
 * Frame 0 (bits 15-14 = 00): MAP INFO
 *   Bits 13-7: Map number (7 bits, 0-127)
 *   Bits 6-0:  Map group (6 bits, 0-63)
 *   This tells other players which map you're on.
 *
 * Frame 1 (bits 15-14 = 01): POSITION
 *   Bits 13-7: Y coordinate (7 bits, 0-127)
 *   Bits 6-0:  X coordinate (7 bits, 0-127)
 *   Your tile position on the current map.
 *
 * Frame 2 (bit 15 = 1): APPEARANCE
 *   Bits 13-5: Graphics ID (8 bits, which sprite to display, 0-255)
 *   Bits 4-2:  Facing direction (3 bits, values 1-4)
 *   Bit 1:     In battle flag
 *   Bit 0:     Active flag (1 = player is connected and playing)
 *
 * A full state update completes every 3 frames = 20 updates per second.
 *
 * ============================================================================
 * SMOOTH MOVEMENT INTERPOLATION
 * ============================================================================
 *
 * Because position data arrives every 3 frames (50ms), simply snapping to
 * received positions would look jerky. The system interpolates:
 *
 * - Each frame, remote sprites move 1-2 pixels toward their target position
 * - Movement direction must match the player's facing direction to prevent
 *   backward sliding (if direction doesn't match, the sprite snaps instantly)
 * - Speed increases when the sprite falls behind (>8 pixels from target)
 *   to compensate for accumulated transmission delay
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h" // IWYU pragma: keep
#include "multiplayer.h"
#include "task.h" // IWYU pragma: keep
#include "main.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "overworld.h" // IWYU pragma: keep
#include "new_menu_helpers.h"
#include "menu.h"
#include "string_util.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h" // IWYU pragma: keep
#include "constants/songs.h"

/*
 * SIO Multi-Player mode register bit definitions.
 * These control the serial communication hardware.
 */
#define SIO_MULTI_MODE    0x2000  /* Bit 13: Enable multi-player mode (4-player) */
#define SIO_115200_BPS    0x0003  /* Bits 0-1: 115200 baud rate (fastest available) */
#define SIO_INTR_ENABLE   0x4000  /* Bit 14: Fire interrupt when transfer completes */
#define SIO_START_BIT     0x0080  /* Bit 7: Master sets this to initiate a transfer */
#define SIO_MULTI_BUSY    0x0080  /* Bit 7: When read, indicates transfer in progress */
#define SIO_ID_BITS       0x0030  /* Bits 4-5: This GBA's assigned player ID (0-3) */
#define SIO_ID_SHIFT      4       /* Shift amount to extract the player ID */

/*
 * Packet type identification bits (top 2 bits of the 16-bit packet).
 * 4-frame cycle: map (00), position (01), appearance (10), name (11).
 *
 *   00xxxxxxxxxxxxxx = map info (mapNum + mapGroup)
 *   01xxxxxxxxxxxxxx = position (y + x)
 *   10xxxxxxxxxxxxxx = appearance (graphicsId + direction + flags)
 *   11xxxxxxxxxxxxxx = name byte (byte index + character value)
 */
#define PKT_TYPE_MAP   0x0000  /* Bits 15-14 = 00: map packet */
#define PKT_TYPE_POS   0x4000  /* Bits 15-14 = 01: position packet */
#define PKT_TYPE_DIR   0x8000  /* Bits 15-14 = 10: appearance packet */
#define PKT_TYPE_NAME  0xC000  /* Bits 15-14 = 11: name byte packet */
#define PKT_TYPE_MASK  0xC000  /* Top 2 bits identify packet type */

/*
 * sRemotePlayers: State tracking for each remote (non-local) player.
 * MP_MAX_REMOTE = 3 (we can see up to 3 other players).
 *
 * gMultiplayerEnabled: Global flag indicating whether multiplayer mode is active.
 * sLocalId: This GBA's player ID (0-3, assigned by hardware based on cable position).
 * sFrame: Frame counter for the 3-frame packet cycle (0, 1, 2, 0, 1, 2, ...).
 * sRecvBuffer: Received 16-bit values from all 4 players (copied from SIO registers).
 * sTransferDone: Flag set by the serial interrupt when a transfer completes.
 */
static EWRAM_DATA struct RemotePlayer sRemotePlayers[MP_MAX_REMOTE] = {0};
EWRAM_DATA bool8 gMultiplayerEnabled = FALSE;
static EWRAM_DATA u8 sLocalId = 0;
static EWRAM_DATA u8 sFrame = 0;
static EWRAM_DATA u8 sNameIdx = 0;  /* Current byte index for name packet rotation (0-7) */
/* volatile: written by ISR (MP_SerialCallback), read by main loop.
 * Without volatile, the compiler may cache these in registers and
 * never re-read from memory, especially with MODERN=1 optimizations. */
static EWRAM_DATA volatile u16 sRecvBuffer[MP_MAX_PLAYERS] = {0};
static EWRAM_DATA volatile bool8 sTransferDone = FALSE;

/*
 * MP_SPRITE_TAG: Magic value stored in sprite->data[7] to identify sprites
 * created by the multiplayer system. This prevents the system from
 * accidentally destroying sprites that belong to other game systems
 * (NPCs, items, etc.) if object event IDs get reused after a battle
 * or menu transition.
 * 0x4D50 = ASCII "MP" (MultiPlayer).
 */
// Magic tag stored in sprite->data[7] to identify our remote player sprites
#define MP_SPRITE_TAG 0x4D50  // "MP"

/*
 * Notification popup: tracks which remote players we've already shown
 * a "joined" notification for. Reset on disconnect so a reconnecting
 * player triggers a new notification.
 */
static EWRAM_DATA bool8 sPlayerNotified[MP_MAX_REMOTE] = {0};

/* Buffer for building notification text like "Player 2 joined!" */
static EWRAM_DATA u8 sNotifyTextBuf[24] = {0};

/* Notification hold time in frames (120 = 2 seconds at 60fps) */
#define NOTIFY_HOLD_FRAMES 120

static const u8 sText_Joined[] = _(" joined!");
static const u8 sText_Left[] = _(" left!");
static const u8 sText_FallbackPlayer[] = _("Player ");

static void Task_MPNotify(u8 taskId);
static void ShowMPNotification(u8 remoteIdx, bool8 joined);

static void SpriteCB_RemotePlayer(struct Sprite *sprite);

/**
 * FUNCTION: MP_SerialCallback
 *
 * PURPOSE: Interrupt handler called when a serial transfer completes.
 *
 * HOW IT WORKS:
 * When the GBA's serial hardware finishes exchanging data with all connected
 * GBAs, it fires a serial interrupt. This callback reads all four received
 * values from the SIO Multi-Player registers into the receive buffer.
 *
 * GBA CONTEXT:
 * REG_SIOMULTI0 through REG_SIOMULTI3 (0x04000120-0x04000126) contain the
 * 16-bit values received from players 0 through 3 respectively. These
 * registers are only valid immediately after a transfer completes, so we
 * must read them in the interrupt handler before the next transfer overwrites them.
 *
 * This function runs in interrupt context -- it must be fast and not call
 * any non-reentrant functions.
 */
static void MP_SerialCallback(void)
{
    sRecvBuffer[0] = REG_SIOMULTI0;  /* Value sent by player 0 (master) */
    sRecvBuffer[1] = REG_SIOMULTI1;  /* Value sent by player 1 */
    sRecvBuffer[2] = REG_SIOMULTI2;  /* Value sent by player 2 */
    sRecvBuffer[3] = REG_SIOMULTI3;  /* Value sent by player 3 */
    sTransferDone = TRUE;            /* Signal to main loop that new data is available */
}

/**
 * FUNCTION: ReadLocalId
 *
 * PURPOSE: Read this GBA's player ID from the serial hardware.
 *
 * GBA CONTEXT:
 * In SIO Multi-Player mode, the hardware automatically assigns player IDs
 * based on how the GBAs are physically connected in the link cable chain.
 * Player 0 is the master (controls timing), players 1-3 are slaves.
 * The ID is read from bits 4-5 of REG_SIOCNT.
 *
 * RETURNS: Player ID (0-3)
 */
static u8 ReadLocalId(void)
{
    return (REG_SIOCNT & SIO_ID_BITS) >> SIO_ID_SHIFT;
}

/**
 * FUNCTION: InitSIO
 *
 * PURPOSE: Initialize the GBA's serial port for Multi-Player communication.
 *
 * HOW IT WORKS:
 * 1. REG_RCNT = 0: Select SIO mode (not General Purpose or UART mode)
 * 2. REG_SIOCNT = 0: Reset all SIO settings
 * 3. REG_SIOCNT = multi mode | 115200 baud | interrupt enable:
 *    Configure for 4-player mode at maximum speed with interrupts
 * 4. REG_SIOMLT_SEND = 0: Clear the send register
 * 5. Enable the serial interrupt and set our callback
 *
 * GBA CONTEXT:
 * The GBA's serial port can operate in several modes:
 *   - Normal 8-bit / 32-bit (2 players)
 *   - Multi-Player (up to 4 players, 16 bits each)
 *   - UART (RS-232 compatible)
 *   - General Purpose I/O
 * Setting REG_RCNT to 0 and bit 13 of SIOCNT selects Multi-Player mode.
 */
static void InitSIO(void)
{
    REG_RCNT = 0;                                               /* Select SIO mode */
    REG_SIOCNT = 0;                                             /* Reset SIO */
    REG_SIOCNT = SIO_MULTI_MODE | SIO_115200_BPS | SIO_INTR_ENABLE; /* Configure multi-player */
    REG_SIOMLT_SEND = 0;                                        /* Clear send buffer */
    EnableInterrupts(INTR_FLAG_SERIAL);                         /* Enable serial IRQ */
    SetSerialCallback(MP_SerialCallback);                       /* Register our handler */
}

/**
 * FUNCTION: IsMultiplayerActive
 *
 * PURPOSE: Check whether the multiplayer system is currently active.
 *
 * RETURNS: TRUE if multiplayer is enabled, FALSE otherwise
 */
bool8 IsMultiplayerActive(void)
{
    return gMultiplayerEnabled;
}

/**
 * FUNCTION: IsRemotePlayerOnSameMap
 *
 * PURPOSE: Check if a remote player is on the same map as the local player.
 *
 * HOW IT WORKS:
 * Compares the remote player's mapGroup and mapNum (received via serial)
 * with the local player's current location. Only active remote players
 * on the same map should have visible sprites.
 *
 * PARAMETERS:
 * @param remoteIdx -- Index into sRemotePlayers[] (0-2)
 *
 * RETURNS: TRUE if the remote player is active and on the same map
 */
static bool8 IsRemotePlayerOnSameMap(u8 remoteIdx)
{
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    return (rp->flags & MP_FLAG_ACTIVE)
        && rp->mapGroup == gSaveBlock1Ptr->location.mapGroup
        && rp->mapNum == gSaveBlock1Ptr->location.mapNum;
}

/**
 * FUNCTION: SpawnRemoteSprite
 *
 * PURPOSE: Create a visible sprite for a remote player on the local screen.
 *
 * HOW IT WORKS:
 * 1. Finds an unused ObjectEvent slot (the same system used for NPCs)
 * 2. Initializes the ObjectEvent with the remote player's position and direction
 * 3. Creates a graphics sprite using the remote player's avatar graphics ID
 * 4. Tags the sprite with MP_SPRITE_TAG for later identification
 * 5. Records the initial pixel position for smooth interpolation
 *
 * The MAP_OFFSET is added because the game's coordinate system has an offset
 * between "world coordinates" (what the player sees) and "map coordinates"
 * (internal tilemap indices that include border tiles).
 *
 * PARAMETERS:
 * @param remoteIdx -- Which remote player to spawn (0-2)
 */
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

    /* Find a free ObjectEvent slot */
    objEventId = GetFirstInactiveObjectEventId();
    if (objEventId >= OBJECT_EVENTS_COUNT)
        return;  /* No free slots */

    rp->objEventId = objEventId;
    objEvent = &gObjectEvents[objEventId];

    /* Initialize the object event with remote player's state */
    memset(objEvent, 0, sizeof(struct ObjectEvent));
    objEvent->active = TRUE;
    objEvent->currentCoords.x = rp->x + MAP_OFFSET;
    objEvent->currentCoords.y = rp->y + MAP_OFFSET;
    objEvent->previousCoords.x = rp->x + MAP_OFFSET;
    objEvent->previousCoords.y = rp->y + MAP_OFFSET;
    objEvent->facingDirection = rp->direction;
    objEvent->spriteId = MAX_SPRITES;

    /* Convert map tile coordinates to screen pixel coordinates */
    SetSpritePosToMapCoords(rp->x + MAP_OFFSET, rp->y + MAP_OFFSET, &px, &py);
    objEvent->initialCoords.x = px + 8;  /* +8 to center in the tile */
    objEvent->initialCoords.y = py;

    /* Validate and set the graphics ID (fall back to Red's sprite if invalid) */
    graphicsId = rp->graphicsId;
    if (graphicsId == 0 || graphicsId >= NUM_OBJ_EVENT_GFX)
        graphicsId = OBJ_EVENT_GFX_RED_NORMAL;

    objEvent->graphicsId = graphicsId;
    objEvent->spriteId = CreateObjectGraphicsSprite(graphicsId, SpriteCB_RemotePlayer, 0, 0, 0);

    if (objEvent->spriteId != MAX_SPRITES)
    {
        sprite = &gSprites[objEvent->spriteId];
        sprite->coordOffsetEnabled = TRUE;   /* Follow camera scrolling */
        sprite->data[0] = remoteIdx;          /* Store which remote player this represents */
        sprite->data[7] = MP_SPRITE_TAG;      /* Tag so we can identify our sprites later */
        rp->spriteSpawned = TRUE;
        rp->displayX = objEvent->initialCoords.x;
        rp->displayY = objEvent->initialCoords.y;
    }
}

/**
 * FUNCTION: DespawnRemoteSprite
 *
 * PURPOSE: Remove a remote player's sprite from the screen.
 *
 * HOW IT WORKS:
 * Destroys the sprite and deactivates the ObjectEvent slot. Checks that
 * the ObjectEvent and sprite still belong to us before destroying them,
 * since battle transitions and menu screens can destroy/reallocate sprites.
 *
 * PARAMETERS:
 * @param remoteIdx -- Which remote player to despawn (0-2)
 */
static void DespawnRemoteSprite(u8 remoteIdx)
{
    struct RemotePlayer *rp = &sRemotePlayers[remoteIdx];
    struct ObjectEvent *objEvent;

    if (!rp->spriteSpawned)
        return;

    objEvent = &gObjectEvents[rp->objEventId];
    /* Only destroy if the ObjectEvent still belongs to us. Setting
     * active = FALSE INSIDE the check prevents deactivating an unrelated
     * NPC that may have been assigned this slot after our sprite was
     * destroyed by a battle transition or menu. */
    if (objEvent->active && objEvent->spriteId < MAX_SPRITES)
    {
        struct Sprite *sprite = &gSprites[objEvent->spriteId];
        if (sprite->inUse)
        {
            sprite->callback = SpriteCallbackDummy;
            DestroySprite(sprite);
        }
        objEvent->spriteId = MAX_SPRITES;
        objEvent->active = FALSE;
    }
    rp->spriteSpawned = FALSE;
}

/**
 * FUNCTION: UpdateRemoteSprite
 *
 * PURPOSE: Smoothly interpolate a remote player's sprite toward their
 *          actual position, creating fluid movement.
 *
 * HOW IT WORKS:
 * Each frame, calculates the pixel-distance between the sprite's current
 * display position and the target position (derived from the latest received
 * tile coordinates). Then moves the sprite 1-2 pixels in the dominant
 * direction, but ONLY if the movement direction matches the player's
 * facing direction.
 *
 * WHY DIRECTION MATCHING:
 * Without this check, when a player turns around, the sprite would briefly
 * slide backward (walking animation playing forward while pixel position
 * moves backward). By snapping instantly when direction doesn't match,
 * we avoid this visual artifact at the cost of a tiny jump.
 *
 * SPEED COMPENSATION:
 * Due to the 3-frame transmission cycle, remote sprites are always
 * slightly behind. When the gap exceeds 8 pixels (more than half a tile),
 * the sprite moves at 2 pixels/frame instead of 1 to catch up.
 *
 * PARAMETERS:
 * @param remoteIdx -- Which remote player to update
 */
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

    /* Convert the remote player's tile position to screen pixel position */
    SetSpritePosToMapCoords(rp->x + MAP_OFFSET, rp->y + MAP_OFFSET,
        &targetPixelX, &targetPixelY);
    targetPixelX += 8;  /* Center in the 16-pixel-wide tile */

    /* Calculate distance from current display position to target */
    dx = targetPixelX - rp->displayX;
    dy = targetPixelY - rp->displayY;

    if ((dx == 0 && dy == 0) || (rp->flags & MP_FLAG_IN_BATTLE))
    {
        /* At target or in battle -- don't move */
        // At target or in battle - stay still
        objEvent->currentCoords.x = rp->x + MAP_OFFSET;
        objEvent->currentCoords.y = rp->y + MAP_OFFSET;
        objEvent->initialCoords.x = rp->displayX;
        objEvent->initialCoords.y = rp->displayY;
        return;
    }

    /* Determine which direction to move (favor the axis with more distance) */
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

    /*
     * Check if the movement direction matches the player's facing direction.
     * If not, snap to target to avoid backward walking animation.
     */
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
            /*
             * Moving in the correct direction: interpolate smoothly.
             * Speed up to 2px/frame when >8px behind to compensate for delay.
             */
            // Move faster when further behind to stay in sync
            // (compensates for 3-frame transmission delay)
            speed = (abs(dx) + abs(dy) > 8) ? 2 : 1;
            rp->displayX += moveX * speed;
            rp->displayY += moveY * speed;

            /* Clamp to prevent overshooting the target */
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
            /* Direction mismatch: snap instantly to avoid backward sliding */
            // Direction mismatch - snap to target instantly
            rp->displayX = targetPixelX;
            rp->displayY = targetPixelY;
        }
    }

    /* Update the ObjectEvent with the new position */
    objEvent->currentCoords.x = rp->x + MAP_OFFSET;
    objEvent->currentCoords.y = rp->y + MAP_OFFSET;
    objEvent->initialCoords.x = rp->displayX;
    objEvent->initialCoords.y = rp->displayY;
}

/**
 * FUNCTION: SpriteCB_RemotePlayer
 *
 * PURPOSE: Per-frame callback for remote player sprites, handling animation
 *          and position updates.
 *
 * HOW IT WORKS:
 * Called by the sprite system every frame for each remote player sprite.
 * Updates the sprite's screen position from the ObjectEvent's interpolated
 * coordinates, sets the OAM priority (draw layer), and selects the correct
 * animation based on whether the player is moving, standing, or in battle.
 *
 * Animation selection:
 *   - In battle: standing/facing animation (frozen pose)
 *   - Moving (display position != target position): walking animation
 *   - Stationary: standing/facing animation
 *
 * PARAMETERS:
 * @param sprite -- The sprite to update
 */
static void SpriteCB_RemotePlayer(struct Sprite *sprite)
{
    u8 remoteIdx = sprite->data[0];
    struct RemotePlayer *rp;
    struct ObjectEvent *objEvent;
    s16 targetPixelX, targetPixelY;
    bool8 isMoving;

    /* Guard against out-of-bounds access if sprite->data[0] contains
     * garbage (e.g., sprite slot was reused after a battle transition). */
    if (remoteIdx >= MP_MAX_REMOTE)
        return;

    rp = &sRemotePlayers[remoteIdx];
    objEvent = &gObjectEvents[rp->objEventId];

    /* Position the sprite at the interpolated coordinates */
    sprite->x = objEvent->initialCoords.x;
    sprite->y = objEvent->initialCoords.y;
    sprite->oam.priority = 2;  /* Draw priority: behind text windows, above BG */

    /* Check if the sprite is currently moving toward its target */
    SetSpritePosToMapCoords(rp->x + MAP_OFFSET, rp->y + MAP_OFFSET,
        &targetPixelX, &targetPixelY);
    targetPixelX += 8;
    isMoving = (rp->displayX != targetPixelX || rp->displayY != targetPixelY);

    /* Select the appropriate animation */
    if (rp->flags & MP_FLAG_IN_BATTLE)
    {
        /* In battle: show standing pose in the facing direction */
        // In battle - frozen facing pose
        StartSpriteAnimIfDifferent(sprite, GetFaceDirectionAnimNum(rp->direction));
    }
    else if (isMoving)
    {
        /* Moving: show walking animation in the facing direction */
        StartSpriteAnimIfDifferent(sprite, GetMoveDirectionAnimNum(rp->direction));
    }
    else
    {
        /* Stationary: show standing pose */
        StartSpriteAnimIfDifferent(sprite, GetFaceDirectionAnimNum(rp->direction));
    }

    UpdateObjectEventSpriteInvisibility(sprite, FALSE);
}

/**
 * FUNCTION: InitMultiplayer
 *
 * PURPOSE: Initialize the multiplayer system and start serial communication.
 *
 * HOW IT WORKS:
 * Enables multiplayer mode, resets the frame counter and receive buffer,
 * clears all remote player state, and initializes the SIO hardware.
 * After this call, the system will begin sending/receiving player state
 * every frame.
 */
void InitMultiplayer(void)
{
    u8 i;
    gMultiplayerEnabled = TRUE;
    sFrame = 0;
    sNameIdx = 0;
    sTransferDone = FALSE;
    /* Initialize receive buffer to 0xFFFF (the "no data" sentinel) */
    for (i = 0; i < MP_MAX_PLAYERS; i++)
        sRecvBuffer[i] = 0xFFFF;
    /* Clear all remote player tracking state */
    for (i = 0; i < MP_MAX_REMOTE; i++)
        memset(&sRemotePlayers[i], 0, sizeof(struct RemotePlayer));
    InitSIO();
}

/**
 * FUNCTION: Special_StartMultiplayer
 *
 * PURPOSE: Script-callable function to start multiplayer mode.
 *
 * HOW IT WORKS:
 * Called from game scripts (e.g., when the player uses a special item or
 * talks to an NPC). Only initializes if not already active.
 */
void Special_StartMultiplayer(void)
{
    if (gMultiplayerEnabled)
        return;
    InitMultiplayer();
}

/**
 * FUNCTION: ShutdownMultiplayer
 *
 * PURPOSE: Shut down the multiplayer system, signal disconnect to other
 *          GBAs, and clean up all local state.
 *
 * HOW IT WORKS:
 * 1. Writes 0 to REG_SIOMLT_SEND so that if the master on another GBA
 *    initiates one more serial transfer, the other GBAs read 0 from our
 *    slot and trigger their sentinel check (recv == 0), immediately
 *    despawning our sprite on their screen.
 *
 * 2. Despawns all remote player sprites and clears their tracking state.
 *
 * 3. Disables the serial interrupt and clears the serial callback to
 *    prevent the ISR from writing stale data to sRecvBuffer after we
 *    stop processing it. Same pattern as link.c's DisableSerial().
 *
 * 4. Sets gMultiplayerEnabled to FALSE so UpdateMultiplayerState() stops.
 *
 * TIMING NOTE:
 * The 0 written to REG_SIOMLT_SEND only helps if this GBA is a slave
 * and the master on another GBA initiates another transfer. If THIS GBA
 * is the master, transfers stop immediately (no more SIO_START_BIT), so
 * slaves rely on the timeout in UpdateMultiplayerState() instead.
 */
void ShutdownMultiplayer(void)
{
    u8 i;

    /*
     * Signal disconnect FIRST. If the SIO hardware is still active and
     * another transfer occurs, other GBAs read 0 from our slot, which
     * triggers their sentinel check for immediate sprite despawn.
     */
    REG_SIOMLT_SEND = 0;

    for (i = 0; i < MP_MAX_REMOTE; i++)
    {
        DespawnRemoteSprite(i);
        memset(&sRemotePlayers[i], 0, sizeof(struct RemotePlayer));
    }

    /*
     * Disable serial interrupt and clear callback so MP_SerialCallback
     * stops writing to sRecvBuffer after we've stopped processing it.
     */
    DisableInterrupts(INTR_FLAG_SERIAL);
    SetSerialCallback(NULL);

    for (i = 0; i < MP_MAX_REMOTE; i++)
        sPlayerNotified[i] = FALSE;

    gMultiplayerEnabled = FALSE;
}

/*
 * ShowMPNotification - Shows a brief "Player N joined!" or "Player N left!" popup.
 *
 * Builds the notification string from the remote player index, creates a
 * bordered window at the top-left using the same frame style as the start
 * menu (DrawStdWindowFrame), plays a chime, and auto-dismisses after 2 sec.
 *
 * If a notification is already showing, the new one is dropped. Notifications
 * are brief enough (~2 sec) that this rarely matters in practice.
 *
 * @param remoteIdx  Index into sRemotePlayers (0 to MP_MAX_REMOTE-1)
 * @param joined     TRUE for "joined!", FALSE for "left!"
 */
static void ShowMPNotification(u8 remoteIdx, bool8 joined)
{
    u8 *ptr;
    u8 sioSlot;

    if (FindTaskIdByFunc(Task_MPNotify) != TASK_NONE)
        return;

    /*
     * Use the remote player's name if we've received it via name packets.
     * Otherwise, fall back to "Player N" using the SIO slot number.
     */
    if (sRemotePlayers[remoteIdx].nameReceived)
    {
        ptr = StringCopy(sNotifyTextBuf, sRemotePlayers[remoteIdx].name);
    }
    else
    {
        sioSlot = (remoteIdx < sLocalId) ? remoteIdx : remoteIdx + 1;
        ptr = StringCopy(sNotifyTextBuf, sText_FallbackPlayer);
        ptr = ConvertIntToDecimalStringN(ptr, sioSlot + 1, STR_CONV_MODE_LEFT_ALIGN, 1);
    }
    StringCopy(ptr, joined ? sText_Joined : sText_Left);

    CreateTask(Task_MPNotify, 90);
    PlaySE(SE_DING_DONG);
}

/*
 * Task_MPNotify - State machine for the multiplayer notification popup.
 *
 * Uses DrawStdWindowFrame for the border (same frame as the start menu
 * and other standard game windows). LoadStdWindowFrameGfx ensures the
 * frame tile graphics and palette are loaded before drawing.
 *
 * State 0: Create window, draw frame, print text.
 * State 1: Wait for DMA.
 * State 2: Hold for NOTIFY_HOLD_FRAMES.
 * State 3: Clear window and frame.
 * State 4: Wait for DMA, remove window, destroy task.
 */
static void Task_MPNotify(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    switch (task->data[0])
    {
    case 0:
    {
        struct WindowTemplate tmpl;
        tmpl.bg = 0;
        tmpl.tilemapLeft = 1;
        tmpl.tilemapTop = 1;
        tmpl.width = 15;
        tmpl.height = 2;
        tmpl.paletteNum = 15;
        tmpl.baseBlock = 0x050;

        task->data[2] = AddWindow(&tmpl);

        /*
         * Draw the standard window frame (same border as the start menu).
         * The frame tile graphics and palette are already loaded during
         * overworld initialization (InitOverworldBgs → LoadStdWindowFrameGfx),
         * so we do NOT call LoadStdWindowFrameGfx() here — calling it would
         * trigger DMA transfers that can interfere with other windows
         * (e.g., the event script msgbox) if they're drawing simultaneously.
         */
        DrawStdWindowFrame(task->data[2], FALSE);
        FillWindowPixelBuffer(task->data[2], PIXEL_FILL(1));
        AddTextPrinterParameterized(task->data[2], FONT_NORMAL, sNotifyTextBuf, 4, 1, 0xFF, NULL);
        CopyWindowToVram(task->data[2], COPYWIN_FULL);
        task->data[0] = 1;
        break;
    }
    case 1:
        if (!IsDma3ManagerBusyWithBgCopy())
            task->data[0] = 2;
        break;
    case 2:
        task->data[1]++;
        if (task->data[1] > NOTIFY_HOLD_FRAMES)
            task->data[0] = 3;
        break;
    case 3:
        ClearStdWindowAndFrame(task->data[2], FALSE);
        CopyWindowToVram(task->data[2], COPYWIN_MAP);
        task->data[0] = 4;
        break;
    case 4:
        if (!IsDma3ManagerBusyWithBgCopy())
        {
            RemoveWindow(task->data[2]);
            DestroyTask(taskId);
        }
        break;
    }
}

/**
 * FUNCTION: UpdateMultiplayerState
 *
 * PURPOSE: Main per-frame update for the entire multiplayer system.
 *
 * HOW IT WORKS:
 * This is the heart of the multiplayer system, called every frame. It:
 *
 * 1. RE-INITIALIZES SIO: The serial callback and registers are reset every
 *    frame because battle screens, menus, and other systems may override
 *    the serial configuration.
 *
 * 2. VALIDATES SPRITES: Checks that remote player sprites haven't been
 *    destroyed by battle transitions or menu systems. Uses the MP_SPRITE_TAG
 *    magic value to verify sprite ownership.
 *
 * 3. PROCESSES RECEIVED DATA: When a serial transfer completes, reads the
 *    received data and updates remote player state based on packet type:
 *    - Type 0 (bits 15-14 = 00): Map info (mapGroup + mapNum)
 *    - Type 1 (bits 15-14 = 01): Position (x + y coordinates)
 *    - Type 2 (bit 15 = 1): Appearance (graphicsId + direction + flags)
 *
 * 4. MANAGES SPRITES: Creates sprites for remote players on the same map,
 *    destroys sprites for players who left or disconnected.
 *
 * 5. INTERPOLATES MOVEMENT: Updates all spawned remote sprites for smooth
 *    pixel-by-pixel movement.
 *
 * 6. SENDS LOCAL STATE: Packs the local player's current state into a 16-bit
 *    value and writes it to REG_SIOMLT_SEND for the next transfer. Uses a
 *    3-frame cycle to send map, position, and appearance data.
 *
 * 7. INITIATES TRANSFER: If this GBA is the master (player 0) and the serial
 *    hardware is not busy, sets the start bit to begin the next transfer.
 *    Slave GBAs don't need to set this -- the master controls timing.
 */
void UpdateMultiplayerState(void)
{
    u8 i;
    u16 sendVal;

    if (!gMultiplayerEnabled)
        return;

    /*
     * Re-initialize SIO every frame. Battle systems and menus may have
     * reconfigured the serial port, so we need to restore our settings.
     */
    // Re-initialize SIO every frame in case battle/menu reset it
    SetSerialCallback(MP_SerialCallback);
    REG_RCNT = 0;
    REG_SIOCNT = SIO_MULTI_MODE | SIO_115200_BPS | SIO_INTR_ENABLE;
    EnableInterrupts(INTR_FLAG_SERIAL);

    sLocalId = ReadLocalId();

    /*
     * Validate that our sprites still exist. Battle transitions destroy all
     * sprites, and menus may reallocate object events. Check the magic tag
     * to verify each sprite is still ours.
     */
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
                sRemotePlayers[i].spriteSpawned = FALSE;  /* Mark for re-spawn */
        }
    }

    /* Process received data from the last serial transfer */
    if (sTransferDone)
    {
        /* Copy the volatile receive buffer to a local snapshot under
         * interrupt protection. Without this, a new serial interrupt
         * could fire mid-loop and overwrite sRecvBuffer, causing us
         * to mix data from two different transfer cycles. */
        u16 localBuf[MP_MAX_PLAYERS];
        sTransferDone = FALSE;
        DisableInterrupts(INTR_FLAG_SERIAL);
        for (i = 0; i < MP_MAX_PLAYERS; i++)
            localBuf[i] = sRecvBuffer[i];
        EnableInterrupts(INTR_FLAG_SERIAL);

        for (i = 0; i < MP_MAX_PLAYERS; i++)
        {
            u16 recv;
            u8 remoteIdx;
            struct RemotePlayer *rp;

            /* Skip our own data (we already know our state) */
            if (i == sLocalId)
                continue;

            /*
             * Map player index to remote index.
             * If we're player 1, then player 0 -> remote 0, player 2 -> remote 1, player 3 -> remote 2
             * If we're player 0, then player 1 -> remote 0, player 2 -> remote 1, player 3 -> remote 2
             */
            remoteIdx = (i < sLocalId) ? i : i - 1;
            if (remoteIdx >= MP_MAX_REMOTE)
                continue;

            rp = &sRemotePlayers[remoteIdx];
            recv = localBuf[i];

            /* 0xFFFF or 0 means no player connected at this position */
            if (recv == 0xFFFF || recv == 0)
            {
                if (sPlayerNotified[remoteIdx])
                    ShowMPNotification(remoteIdx, FALSE);
                rp->flags &= ~MP_FLAG_ACTIVE;
                DespawnRemoteSprite(remoteIdx);
                sPlayerNotified[remoteIdx] = FALSE;
                continue;
            }

            /*
             * Valid data received -- reset disconnect timeout counter.
             * Reset on ANY packet type (map, position, or appearance)
             * because all three indicate the player is still transmitting.
             */
            rp->timeoutCounter = 0;

            /*
             * Decode packet based on top 2 bits (4-frame cycle):
             *   00 = map, 01 = position, 10 = appearance, 11 = name byte
             */
            switch (recv & PKT_TYPE_MASK)
            {
            case PKT_TYPE_MAP:
                /* Map packet: bits 13-7 = mapNum (7), bits 6-0 = mapGroup (6) */
                rp->mapNum = (recv >> 7) & 0x7F;
                rp->mapGroup = recv & 0x3F;
                break;
            case PKT_TYPE_POS:
                /* Position packet: bits 13-7 = y (7), bits 6-0 = x (7) */
                rp->y = (recv >> 7) & 0x7F;
                rp->x = recv & 0x7F;
                break;
            case PKT_TYPE_DIR:
            {
                /*
                 * Appearance packet: bits 13-5 = graphicsId (8),
                 * bits 4-2 = direction (3), bits 1-0 = flags (2)
                 */
                rp->graphicsId = (recv >> 5) & 0xFF;
                rp->direction = (recv >> 2) & 0x7;
                rp->flags = recv & 0x3;

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
                break;
            }
            case PKT_TYPE_NAME:
            {
                /*
                 * Name byte packet: bits 13-11 = byte index (0-7),
                 * bits 7-0 = character value in game encoding.
                 * The sender rotates through all 8 bytes (7 chars + EOS)
                 * continuously, so late joiners eventually get the full name.
                 *
                 * We track received bytes via a bitmask (nameRecvMask) rather
                 * than triggering on the first EOS seen, because a late-joining
                 * receiver may catch the sender mid-rotation (e.g., indices
                 * 5,6,7) and would fire the "joined" notification with an
                 * incomplete name. Waiting for all 8 bits ensures the full
                 * name is present before we display it.
                 */
                u8 nameIdx = (recv >> 11) & 0x7;
                u8 ch = recv & 0xFF;

                if (nameIdx < PLAYER_NAME_LENGTH + 1)
                    rp->name[nameIdx] = ch;
                rp->nameRecvMask |= (1 << nameIdx);

                /* All 8 bytes (indices 0-7) received → name is complete */
                if (rp->nameRecvMask == 0xFF && !rp->nameReceived)
                {
                    rp->nameReceived = TRUE;
                    /* Show "joined" notification once name is complete */
                    if ((rp->flags & MP_FLAG_ACTIVE) && !sPlayerNotified[remoteIdx])
                    {
                        sPlayerNotified[remoteIdx] = TRUE;
                        ShowMPNotification(remoteIdx, TRUE);
                    }
                }
                break;
            }
            }
        }
    }

    /*
     * DISCONNECT TIMEOUT CHECK
     *
     * Increment timeout counter for every active remote player each frame.
     * This runs OUTSIDE the sTransferDone block because when the master
     * disconnects, slaves stop receiving transfers entirely (sTransferDone
     * is never set TRUE), so code inside that block never runs. The counter
     * still increments here, eventually triggering cleanup.
     *
     * The counter is reset to 0 inside the sTransferDone block whenever
     * valid data is received, so it only reaches the threshold when no
     * data arrives at all (master disconnect, cable unplug, power off).
     */
    for (i = 0; i < MP_MAX_REMOTE; i++)
    {
        if (sRemotePlayers[i].flags & MP_FLAG_ACTIVE)
        {
            sRemotePlayers[i].timeoutCounter++;
            if (sRemotePlayers[i].timeoutCounter >= MP_DISCONNECT_TIMEOUT)
            {
                if (sPlayerNotified[i])
                    ShowMPNotification(i, FALSE);
                sRemotePlayers[i].flags &= ~MP_FLAG_ACTIVE;
                DespawnRemoteSprite(i);
                memset(&sRemotePlayers[i], 0, sizeof(struct RemotePlayer));
                sPlayerNotified[i] = FALSE;
            }
        }
    }

    /* Smooth interpolation: update all spawned remote sprites every frame */
    for (i = 0; i < MP_MAX_REMOTE; i++)
        if (sRemotePlayers[i].spriteSpawned)
            UpdateRemoteSprite(i);

    /*
     * SEND LOCAL PLAYER STATE
     * 4-frame cycle: map (00), position (01), appearance (10), name (11).
     * Name bytes rotate through indices 0-7 (7 chars + EOS), so a full
     * name takes 8 rotations × 4 frames = 32 frames (~0.53 sec) to transmit.
     * The name keeps resending so late joiners eventually receive it.
     */
    switch (sFrame % 4)
    {
    case 0:
        /* Map packet: bits15-14=00, mapNum in bits 13-7, mapGroup in bits 6-0 */
        sendVal = PKT_TYPE_MAP
                | ((gSaveBlock1Ptr->location.mapNum & 0x7F) << 7)
                | (gSaveBlock1Ptr->location.mapGroup & 0x3F);
        break;
    case 1:
        /* Position packet: bits15-14=01, Y in bits 13-7, X in bits 6-0 */
        sendVal = PKT_TYPE_POS
                | ((gSaveBlock1Ptr->pos.y & 0x7F) << 7)
                | (gSaveBlock1Ptr->pos.x & 0x7F);
        break;
    case 2:
        /* Appearance packet: bits15-14=10, gfxId(8) in bits 13-5,
         * direction(3) in bits 4-2, flags(2) in bits 1-0 */
        {
            u8 flags = MP_FLAG_ACTIVE;
            if (gMain.inBattle)
                flags |= MP_FLAG_IN_BATTLE;
            sendVal = PKT_TYPE_DIR
                    | ((gSaveBlock2Ptr->playerAvatarGfxId & 0xFF) << 5)
                    | ((GetPlayerFacingDirection() & 0x7) << 2)
                    | flags;
        }
        break;
    case 3:
    default:
        /*
         * Name byte packet: bits15-14=11, bits13-11=byte index (0-7),
         * bits7-0=character byte from local player's name.
         * sNameIdx rotates 0→7→0→... so the full name is resent continuously.
         */
        {
            u8 ch = gSaveBlock2Ptr->playerName[sNameIdx];
            sendVal = PKT_TYPE_NAME
                    | ((sNameIdx & 0x7) << 11)
                    | (ch & 0xFF);
            sNameIdx++;
            if (sNameIdx >= PLAYER_NAME_LENGTH + 1)
                sNameIdx = 0;
        }
        break;
    }

    /* Write our data to the send register for the next transfer */
    REG_SIOMLT_SEND = sendVal;

    /*
     * If we're the master (player 0) and the serial hardware is idle,
     * initiate the next transfer by setting the start bit.
     * Slave GBAs wait for the master to start each transfer.
     */
    if (sLocalId == 0 && !(REG_SIOCNT & SIO_MULTI_BUSY))
        REG_SIOCNT |= SIO_START_BIT;

    sFrame++;
}

/**
 * FUNCTION: SpawnRemotePlayerSprites
 *
 * PURPOSE: Re-create sprites for all remote players on the current map.
 *
 * HOW IT WORKS:
 * Called after map transitions, battle exits, and menu returns to restore
 * remote player sprites that were destroyed during the transition.
 * Only spawns sprites for active players on the same map.
 */
void SpawnRemotePlayerSprites(void)
{
    u8 i;
    if (!gMultiplayerEnabled)
        return;
    for (i = 0; i < MP_MAX_REMOTE; i++)
        if (IsRemotePlayerOnSameMap(i) && !sRemotePlayers[i].spriteSpawned)
            SpawnRemoteSprite(i);
}

/**
 * FUNCTION: DespawnRemotePlayerSprites
 *
 * PURPOSE: Remove all remote player sprites from the screen.
 *
 * HOW IT WORKS:
 * Called before map transitions, entering battles, or opening menus to
 * clean up multiplayer sprites that would interfere with those screens.
 * The sprites will be re-created by SpawnRemotePlayerSprites afterward.
 */
void DespawnRemotePlayerSprites(void)
{
    u8 i;
    for (i = 0; i < MP_MAX_REMOTE; i++)
        DespawnRemoteSprite(i);
}
