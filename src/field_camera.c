/**
 * field_camera.c - Overworld Camera and Map Scrolling System
 *
 * ============================================================================
 * GBA CAMERA AND SCROLLING OVERVIEW
 * ============================================================================
 *
 * The GBA screen is 240x160 pixels, but game maps can be much larger.
 * The "camera" system manages which portion of the map is visible by
 * controlling the BG scroll registers and incrementally updating the
 * off-screen tilemap as the player moves.
 *
 * HOW SCROLLING WORKS ON THE GBA:
 *
 * The GBA's BG layers in text mode use 32x32 tile tilemaps (256x256 pixels).
 * The hardware wraps the tilemap: if you scroll past tile 31, it wraps to tile 0.
 * The screen shows a 30x20 tile window (240x160 pixels) of this wrapping 32x32
 * tilemap, controlled by the BGxHOFS and BGxVOFS scroll registers.
 *
 * Since the visible area (30x20) is slightly smaller than the tilemap (32x32),
 * there are 2 columns and 12 rows of "off-screen" tiles. As the camera scrolls,
 * new map data is drawn into these off-screen tiles just before they come into
 * view. This creates the illusion of an infinite scrolling map.
 *
 * METATILES:
 * Pokemon maps use 16x16 pixel "metatiles" (2x2 hardware tiles). Each metatile
 * consists of 8 tile entries: a bottom layer (4 tiles) and a top layer (4 tiles).
 * These are distributed across three BG layers for depth/layering effects:
 *   - BG3: Bottom-most layer (ground, floor)
 *   - BG1: Middle layer (objects that the player walks behind)
 *   - BG2: Top layer (rooftops, tree canopy that covers the player)
 *
 * CAMERA MOVEMENT:
 * The camera tracks the player sprite. When the player moves one tile (16 pixels),
 * the camera smoothly scrolls over multiple frames (16 pixels / movement speed).
 * At the moment a new tile column/row scrolls into view, RedrawMapSlice draws
 * the new tiles into the tilemap buffer.
 *
 * CAMERA PANNING:
 * An additional offset can be applied for effects like "pan ahead" on the bike
 * (showing more of the road ahead) or cutscene camera movements.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "event_object_movement.h"
#include "new_menu_helpers.h"
#include "overworld.h"

/* Flag for the bike "pan ahead" camera mode (look further in direction of travel) */
EWRAM_DATA bool8 gBikeCameraAheadPanback = FALSE;

/**
 * FieldCameraOffset: Tracks the camera's position within the 32x32 tilemap.
 *
 * xPixelOffset/yPixelOffset: Sub-tile pixel offset (0-15) for smooth scrolling.
 *   These are written to the BG scroll registers to shift the view by individual pixels.
 *
 * xTileOffset/yTileOffset: Which tile in the 32x32 tilemap is at the top-left
 *   corner of the screen. Wraps around 0-31 due to the tilemap's circular nature.
 *
 * copyBGToVRAM: Flag indicating that tilemap changes need to be flushed to VRAM.
 */
// Static type declarations
struct FieldCameraOffset
{
    u8 xPixelOffset;
    u8 yPixelOffset;
    u8 xTileOffset;
    u8 yTileOffset;
    bool8 copyBGToVRAM;
};

// static functions
static void RedrawMapSliceNorth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout);
static void RedrawMapSliceSouth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout);
static void RedrawMapSliceEast(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout);
static void RedrawMapSliceWest(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout);
static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *a, s32 x, s32 y);
static void DrawWholeMapViewInternal(int x, int y, const struct MapLayout *mapLayout);
static void DrawMetatileAt(const struct MapLayout *mapLayout, u16, int, int);
static void DrawMetatile(s32 a, const u16 *b, u16 c);
static void CameraPanningCB_PanAhead(void);

// IWRAM bss vars
static struct FieldCameraOffset sFieldCameraOffset;
static s16 sHorizontalCameraPan;   /* Additional horizontal pan offset (pixels) */
static s16 sVerticalCameraPan;     /* Additional vertical pan offset (pixels) */
static u8 sBikeCameraPanFlag;
static void (*sFieldCameraPanningCallback)(void);

/*
 * gFieldCamera: The camera object that tracks the player sprite.
 * Contains the camera's sub-tile position and movement speed.
 *
 * gTotalCameraPixelOffsetX/Y: Cumulative pixel offset of the camera from
 * the origin. Used to position sprites relative to the camera.
 */
COMMON_DATA struct CameraObject gFieldCamera = {0};
COMMON_DATA u16 gTotalCameraPixelOffsetY = 0;
COMMON_DATA u16 gTotalCameraPixelOffsetX = 0;

/**
 * FUNCTION: move_tilemap_camera_to_upper_left_corner_
 *
 * PURPOSE: Reset the camera's tilemap offset to (0,0) with no pixel offset.
 *
 * HOW IT WORKS:
 * Clears all offset fields and flags that a full redraw is needed.
 * Called when loading a new map or resetting the camera position.
 *
 * PARAMETERS:
 * @param cameraOffset -- The camera offset struct to reset
 */
// text
static void move_tilemap_camera_to_upper_left_corner_(struct FieldCameraOffset *cameraOffset)
{
    cameraOffset->xTileOffset = 0;
    cameraOffset->yTileOffset = 0;
    cameraOffset->xPixelOffset = 0;
    cameraOffset->yPixelOffset = 0;
    cameraOffset->copyBGToVRAM = TRUE;
}

/**
 * FUNCTION: tilemap_move_something
 *
 * PURPOSE: Advance the camera's tile offset within the 32x32 tilemap.
 *
 * HOW IT WORKS:
 * Adds the given tile displacement to the camera's tilemap offset, wrapping
 * around at 32 (the tilemap width/height). This tracks which part of the
 * circular tilemap is currently at the top-left corner of the screen.
 *
 * PARAMETERS:
 * @param cameraOffset -- Camera offset to update
 * @param b -- Horizontal tile displacement (in hardware tiles, not metatiles)
 * @param c -- Vertical tile displacement
 */
static void tilemap_move_something(struct FieldCameraOffset *cameraOffset, u32 b, u32 c)
{
    cameraOffset->xTileOffset += b;
    cameraOffset->xTileOffset %= 32;  /* Wrap within 32x32 tilemap */
    cameraOffset->yTileOffset += c;
    cameraOffset->yTileOffset %= 32;
}

/**
 * FUNCTION: coords8_add
 *
 * PURPOSE: Add pixel-level displacement to the camera's sub-tile offset.
 *
 * HOW IT WORKS:
 * Accumulates sub-pixel movement for smooth scrolling between tile boundaries.
 *
 * PARAMETERS:
 * @param cameraOffset -- Camera offset to update
 * @param b -- Horizontal pixel displacement
 * @param c -- Vertical pixel displacement
 */
static void coords8_add(struct FieldCameraOffset *cameraOffset, u32 b, u32 c)
{
    cameraOffset->xPixelOffset += b;
    cameraOffset->yPixelOffset += c;
}

/**
 * FUNCTION: move_tilemap_camera_to_upper_left_corner
 *
 * PURPOSE: Public wrapper to reset the global camera offset.
 */
void move_tilemap_camera_to_upper_left_corner(void)
{
    move_tilemap_camera_to_upper_left_corner_(&sFieldCameraOffset);
}

/**
 * FUNCTION: FieldUpdateBgTilemapScroll
 *
 * PURPOSE: Write the camera's current scroll position to the BG scroll registers.
 *
 * HOW IT WORKS:
 * Combines the camera's pixel offset with the panning offset and writes the
 * result to the BG1, BG2, and BG3 horizontal and vertical offset registers.
 * All three BG layers scroll by the same amount (they show the same world
 * from the same viewpoint, just with different layering).
 *
 * The +8 on the vertical offset accounts for the 8-pixel top border of the
 * metatile system (half a metatile of padding).
 *
 * GBA CONTEXT:
 * BGxHOFS (Background X Horizontal Offset) and BGxVOFS (Background X Vertical
 * Offset) are 16-bit registers that control how many pixels the BG is scrolled.
 * The hardware applies this offset every scanline when rendering.
 */
void FieldUpdateBgTilemapScroll(void)
{
    u32 r4, r5;
    r5 = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    r4 = sVerticalCameraPan + sFieldCameraOffset.yPixelOffset + 8;

    SetGpuReg(REG_OFFSET_BG1HOFS, r5);
    SetGpuReg(REG_OFFSET_BG1VOFS, r4);
    SetGpuReg(REG_OFFSET_BG2HOFS, r5);
    SetGpuReg(REG_OFFSET_BG2VOFS, r4);
    SetGpuReg(REG_OFFSET_BG3HOFS, r5);
    SetGpuReg(REG_OFFSET_BG3VOFS, r4);
}

/**
 * FUNCTION: FieldCameraGetPixelOffsetAtGround
 *
 * PURPOSE: Get the camera's total pixel offset (scroll + pan) for ground-level rendering.
 *
 * PARAMETERS:
 * @param hofs_p -- Output: horizontal pixel offset
 * @param vofs_p -- Output: vertical pixel offset
 */
void FieldCameraGetPixelOffsetAtGround(s16 *hofs_p, s16 *vofs_p)
{
    *hofs_p = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    *vofs_p = sFieldCameraOffset.yPixelOffset + sVerticalCameraPan + 8;
}

/**
 * FUNCTION: DrawWholeMapView
 *
 * PURPOSE: Redraw the entire visible map area from scratch.
 *
 * HOW IT WORKS:
 * Called when loading a new map or after events that invalidate the tilemap
 * (like a door opening animation). Redraws all 16x16 metatiles visible in
 * the 32x32 hardware tilemap.
 */
void DrawWholeMapView(void)
{
    DrawWholeMapViewInternal(gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y, gMapHeader.mapLayout);
   // sFieldCameraOffset.copyBGToVRAM = TRUE;
}

/**
 * FUNCTION: DrawWholeMapViewInternal
 *
 * PURPOSE: Draw every metatile in the 32x32 hardware tilemap.
 *
 * HOW IT WORKS:
 * Iterates over the entire 32x32 tilemap in steps of 2 (because each metatile
 * occupies 2x2 hardware tiles). For each position, wraps the tilemap coordinate
 * to handle the circular nature of the 32x32 grid, then draws the metatile
 * from the map data at the corresponding world position.
 *
 * PARAMETERS:
 * @param x          -- Camera X position in the map (top-left metatile)
 * @param y          -- Camera Y position in the map (top-left metatile)
 * @param mapLayout  -- The current map's layout data (tilesets and metatile definitions)
 */
static void DrawWholeMapViewInternal(int x, int y, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 j;
    u32 r6;
    u8 temp;

    /* Iterate over the 32x32 tilemap in 2-tile (metatile) steps */
    for (i = 0; i < 32; i += 2)
    {
        /* Wrap Y coordinate within the 32-tile tilemap height */
        temp = sFieldCameraOffset.yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        r6 = temp * 32;  /* Row offset in the linear tilemap buffer */
        for (j = 0; j < 32; j += 2)
        {
            /* Wrap X coordinate within the 32-tile tilemap width */
            temp = sFieldCameraOffset.xTileOffset + j;
            if (temp >= 32)
                temp -= 32;
            /* Draw the metatile at this tilemap position from the corresponding map position */
            DrawMetatileAt(mapLayout, r6 + temp, x + j / 2, y + i / 2);
        }
    }
}

/**
 * FUNCTION: RedrawMapSlicesForCameraUpdate
 *
 * PURPOSE: Redraw the newly exposed edge of the tilemap after scrolling.
 *
 * HOW IT WORKS:
 * When the camera scrolls, new tiles come into view on one edge while old
 * tiles scroll off the opposite edge. This function redraws just the newly
 * exposed strip:
 *   - Scrolling right (x>0): redraw the west (left) edge becoming visible
 *   - Scrolling left (x<0): redraw the east (right) edge
 *   - Scrolling down (y>0): redraw the north (top) edge
 *   - Scrolling up (y<0): redraw the south (bottom) edge
 *
 * This is much more efficient than redrawing the entire map every frame.
 *
 * PARAMETERS:
 * @param cameraOffset -- Camera tilemap offset
 * @param x -- Horizontal tile displacement
 * @param y -- Vertical tile displacement
 */
static void RedrawMapSlicesForCameraUpdate(struct FieldCameraOffset *cameraOffset, int x, int y)
{
    const struct MapLayout *mapLayout = gMapHeader.mapLayout;

    if (x > 0)
        RedrawMapSliceWest(cameraOffset, mapLayout);
    if (x < 0)
        RedrawMapSliceEast(cameraOffset, mapLayout);
    if (y > 0)
        RedrawMapSliceNorth(cameraOffset, mapLayout);
    if (y < 0)
        RedrawMapSliceSouth(cameraOffset, mapLayout);
    cameraOffset->copyBGToVRAM = TRUE;
}

/**
 * FUNCTION: RedrawMapSliceNorth
 *
 * PURPOSE: Redraw the bottom row of metatiles (which just scrolled into view
 *          at the bottom of the screen as the camera moved south/down).
 *
 * HOW IT WORKS:
 * Calculates the tilemap Y position for the bottom edge (offset + 28, since
 * the visible area is about 14 metatiles = 28 tiles tall) and draws a full
 * row of metatiles at that position.
 *
 * The name "North" refers to the camera moving north (up), which reveals
 * new tiles at the south (bottom) edge of the screen.
 * NOTE: The naming convention is confusing -- it's named after the direction
 * the player moved, not the edge being drawn.
 */
static void RedrawMapSliceNorth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r7;

    temp = cameraOffset->yTileOffset + 28;
    if (temp >= 32)
        temp -= 32;
    r7 = temp * 32;
    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->xTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, r7 + temp, gSaveBlock1Ptr->pos.x + i / 2, gSaveBlock1Ptr->pos.y + 14);
    }
}

/**
 * FUNCTION: RedrawMapSliceSouth
 *
 * PURPOSE: Redraw the top row of metatiles (which just scrolled into view
 *          at the top of the screen as the camera moved north/up).
 */
static void RedrawMapSliceSouth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r7 = cameraOffset->yTileOffset * 32;

    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->xTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, r7 + temp, gSaveBlock1Ptr->pos.x + i / 2, gSaveBlock1Ptr->pos.y);
    }
}

/**
 * FUNCTION: RedrawMapSliceEast
 *
 * PURPOSE: Redraw the left column of metatiles (exposed when camera moves east/right).
 */
static void RedrawMapSliceEast(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r6 = cameraOffset->xTileOffset;

    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, temp * 32 + r6, gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y + i / 2);
    }
}

/**
 * FUNCTION: RedrawMapSliceWest
 *
 * PURPOSE: Redraw the right column of metatiles (exposed when camera moves west/left).
 */
static void RedrawMapSliceWest(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u8 r5 = cameraOffset->xTileOffset + 28;

    if (r5 >= 32)
        r5 -= 32;
    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, temp * 32 + r5, gSaveBlock1Ptr->pos.x + 14, gSaveBlock1Ptr->pos.y + i / 2);
    }
}

/**
 * FUNCTION: CurrentMapDrawMetatileAt
 *
 * PURPOSE: Redraw a single metatile on the current map at a specific position.
 *
 * HOW IT WORKS:
 * Converts the map position to a tilemap buffer offset, then draws the metatile
 * if the position is within the currently visible area. Used when a specific
 * tile changes (e.g., a door opening, a boulder being pushed, a cut tree).
 *
 * PARAMETERS:
 * @param x -- Map X coordinate (in metatiles)
 * @param y -- Map Y coordinate (in metatiles)
 */
void CurrentMapDrawMetatileAt(int x, int y)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        DrawMetatileAt(gMapHeader.mapLayout, offset, x, y);
       // sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

/**
 * FUNCTION: DrawDoorMetatileAt
 *
 * PURPOSE: Draw a door metatile at a specific position (for door open/close animations).
 *
 * HOW IT WORKS:
 * Similar to CurrentMapDrawMetatileAt but uses custom tile data instead of
 * reading from the map. The door animation system provides pre-defined tile
 * arrays for each frame of the door opening/closing animation.
 *
 * PARAMETERS:
 * @param x     -- Map X coordinate
 * @param y     -- Map Y coordinate
 * @param tiles -- Array of 8 tile entries for the door metatile
 */
void DrawDoorMetatileAt(int x, int y, const u16 *tiles)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        DrawMetatile(1, tiles, offset);
       // sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

/**
 * FUNCTION: DrawMetatileAt
 *
 * PURPOSE: Draw a single metatile from the map data to the tilemap buffers.
 *
 * HOW IT WORKS:
 * 1. Reads the metatile ID from the map grid at position (x, y)
 * 2. Determines which tileset contains this metatile:
 *    - IDs 0 to NUM_METATILES_IN_PRIMARY-1: primary tileset (common tiles)
 *    - IDs >= NUM_METATILES_IN_PRIMARY: secondary tileset (map-specific tiles)
 * 3. Looks up the metatile's layer type (split, covered, or normal)
 * 4. Calls DrawMetatile to write the 8 tile entries to the BG tilemap buffers
 *
 * GAME LOGIC:
 * Pokemon maps use a dual-tileset system. The primary tileset contains tiles
 * shared across many maps (grass, paths, generic buildings). The secondary
 * tileset contains tiles unique to a specific area (gym interiors, caves, etc.).
 * This saves ROM space by reusing common tiles.
 *
 * PARAMETERS:
 * @param mapLayout -- Map layout data with tileset pointers
 * @param offset    -- Position in the tilemap buffer (0-1023)
 * @param x         -- Map X coordinate (for reading the map grid)
 * @param y         -- Map Y coordinate
 */
static void DrawMetatileAt(const struct MapLayout *mapLayout, u16 offset, int x, int y)
{
    u16 metatileId = MapGridGetMetatileIdAt(x, y);
    const u16 *metatiles;

    if (metatileId > NUM_METATILES_TOTAL)
        metatileId = 0;  /* Invalid metatile: default to tile 0 */
    if (metatileId < NUM_METATILES_IN_PRIMARY)
        metatiles = mapLayout->primaryTileset->metatiles;
    else
    {
        metatiles = mapLayout->secondaryTileset->metatiles;
        metatileId -= NUM_METATILES_IN_PRIMARY;  /* Adjust to secondary tileset offset */
    }
    DrawMetatile(MapGridGetMetatileLayerTypeAt(x, y), metatiles + metatileId * NUM_TILES_PER_METATILE, offset);
}

/**
 * FUNCTION: DrawMetatile
 *
 * PURPOSE: Write a metatile's 8 tile entries to the three BG layer tilemap buffers.
 *
 * HOW IT WORKS:
 * Each metatile has 8 tile entries: 4 for the bottom layer and 4 for the top layer.
 * The bottom 4 tiles (indices 0-3) form a 2x2 grid: [0][1] / [2][3]
 * The top 4 tiles (indices 4-7) form another 2x2 grid: [4][5] / [6][7]
 *
 * These are distributed across the three BG layers based on the metatile's
 * layer type:
 *
 *   SPLIT: Bottom layer -> BG3, Top layer -> BG2, BG1 = transparent
 *     Used for normal ground tiles with optional overlays.
 *     Sprites appear between BG3 (ground) and BG2 (overlay).
 *
 *   COVERED: Bottom layer -> BG3, Top layer -> BG1, BG2 = transparent
 *     Used when the top layer should be behind sprites but above the ground.
 *
 *   NORMAL: Bottom layer -> BG1, Top layer -> BG2, BG3 = placeholder
 *     Used for tiles that cover sprites (rooftops, tree canopy).
 *     The top layer on BG2 draws over sprites.
 *     BG3 gets 0x3014 -- a placeholder tile value that renders as
 *     a solid border/edge tile.
 *
 * The offset 0x20 (32) between rows is because the tilemap is 32 tiles wide
 * (each row is 32 entries in the linear tilemap buffer).
 *
 * After writing, schedules all three BG layers for VRAM copy.
 *
 * PARAMETERS:
 * @param metatileLayerType -- METATILE_LAYER_TYPE_SPLIT/COVERED/NORMAL
 * @param tiles             -- Array of 8 tile entries (4 bottom + 4 top)
 * @param offset            -- Position in the tilemap buffer
 */
static void DrawMetatile(s32 metatileLayerType, const u16 *tiles, u16 offset)
{
    switch (metatileLayerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        /* Bottom layer -> BG3 (behind everything) */
        // Draw metatile's bottom layer to the bottom background layer.
        gBGTilemapBuffers3[offset] = tiles[0];
        gBGTilemapBuffers3[offset + 1] = tiles[1];
        gBGTilemapBuffers3[offset + 0x20] = tiles[2];
        gBGTilemapBuffers3[offset + 0x21] = tiles[3];

        /* BG1 = transparent (sprites show through) */
        // Draw transparent tiles to the middle background layer.
        gBGTilemapBuffers1[offset] = 0;
        gBGTilemapBuffers1[offset + 1] = 0;
        gBGTilemapBuffers1[offset + 0x20] = 0;
        gBGTilemapBuffers1[offset + 0x21] = 0;

        /* Top layer -> BG2 (above sprites) */
        // Draw metatile's top layer to the top background layer.
        gBGTilemapBuffers2[offset] = tiles[4];
        gBGTilemapBuffers2[offset + 1] = tiles[5];
        gBGTilemapBuffers2[offset + 0x20] = tiles[6];
        gBGTilemapBuffers2[offset + 0x21] = tiles[7];
        break;
    case METATILE_LAYER_TYPE_COVERED:
        /* Bottom layer -> BG3 */
        // Draw metatile's bottom layer to the bottom background layer.
        gBGTilemapBuffers3[offset] = tiles[0];
        gBGTilemapBuffers3[offset + 1] = tiles[1];
        gBGTilemapBuffers3[offset + 0x20] = tiles[2];
        gBGTilemapBuffers3[offset + 0x21] = tiles[3];

        /* Top layer -> BG1 (behind sprites but above ground) */
        // Draw metatile's top layer to the middle background layer.
        gBGTilemapBuffers1[offset] = tiles[4];
        gBGTilemapBuffers1[offset + 1] = tiles[5];
        gBGTilemapBuffers1[offset + 0x20] = tiles[6];
        gBGTilemapBuffers1[offset + 0x21] = tiles[7];

        /* BG2 = transparent */
        // Draw transparent tiles to the top background layer.
        gBGTilemapBuffers2[offset] = 0;
        gBGTilemapBuffers2[offset + 1] = 0;
        gBGTilemapBuffers2[offset + 0x20] = 0;
        gBGTilemapBuffers2[offset + 0x21] = 0;
        break;
    case METATILE_LAYER_TYPE_NORMAL:
        /*
         * BG3 = placeholder border tile (0x3014).
         * This value references a specific tile with palette 3 that
         * serves as a solid fill behind everything.
         */
        // Draw garbage to the bottom background layer.
        gBGTilemapBuffers3[offset] = 0x3014;
        gBGTilemapBuffers3[offset + 1] = 0x3014;
        gBGTilemapBuffers3[offset + 0x20] = 0x3014;
        gBGTilemapBuffers3[offset + 0x21] = 0x3014;

        /* Bottom layer -> BG1 */
        // Draw metatile's bottom layer to the middle background layer.
        gBGTilemapBuffers1[offset] = tiles[0];
        gBGTilemapBuffers1[offset + 1] = tiles[1];
        gBGTilemapBuffers1[offset + 0x20] = tiles[2];
        gBGTilemapBuffers1[offset + 0x21] = tiles[3];

        /* Top layer -> BG2 (covers sprites -- used for rooftops, etc.) */
        // Draw metatile's top layer to the top background layer, which covers object event sprites.
        gBGTilemapBuffers2[offset] = tiles[4];
        gBGTilemapBuffers2[offset + 1] = tiles[5];
        gBGTilemapBuffers2[offset + 0x20] = tiles[6];
        gBGTilemapBuffers2[offset + 0x21] = tiles[7];
        break;
    }
    /* Schedule all three BG layers for VRAM update */
    ScheduleBgCopyTilemapToVram(1);
    ScheduleBgCopyTilemapToVram(2);
    ScheduleBgCopyTilemapToVram(3);
}

/**
 * FUNCTION: MapPosToBgTilemapOffset
 *
 * PURPOSE: Convert a map position to a tilemap buffer index.
 *
 * HOW IT WORKS:
 * Converts a map (x, y) position to an index into the 32x32 tilemap buffer,
 * accounting for the camera's current tile offset. Returns -1 if the
 * position is outside the currently mapped region (offscreen).
 *
 * The multiplication by 2 converts metatile coordinates to hardware tile
 * coordinates (each metatile = 2x2 hardware tiles).
 *
 * PARAMETERS:
 * @param cameraOffset -- Current camera tilemap offset
 * @param x -- Map X position (metatile coordinates)
 * @param y -- Map Y position (metatile coordinates)
 *
 * RETURNS: Tilemap buffer index (0-1023), or -1 if out of range
 */
static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *cameraOffset, s32 x, s32 y)
{
    x -= gSaveBlock1Ptr->pos.x;
    x *= 2;
    if (x >= 32 || x < 0)
        return -1;
    x = x + cameraOffset->xTileOffset;
    if (x >= 32)
        x -= 32;

    y = (y - gSaveBlock1Ptr->pos.y) * 2;
    if (y >= 32 || y < 0)
        return -1;
    y = y + cameraOffset->yTileOffset;
    if (y >= 32)
        y -= 32;

    return y * 32 + x;
}

/**
 * FUNCTION: CameraUpdateCallback
 *
 * PURPOSE: Read the camera's movement speed from the tracked sprite.
 *
 * HOW IT WORKS:
 * The camera tracks a sprite (usually the player character). This callback
 * reads the sprite's movement speed from its data fields (data[2] = X speed,
 * data[3] = Y speed) and applies them to the camera object.
 *
 * PARAMETERS:
 * @param fieldCamera -- The camera object to update
 */
static void CameraUpdateCallback(struct CameraObject *fieldCamera)
{
    if (fieldCamera->spriteId != 0)
    {
        fieldCamera->movementSpeedX = gSprites[fieldCamera->spriteId].data[2];
        fieldCamera->movementSpeedY = gSprites[fieldCamera->spriteId].data[3];
    }
}

/**
 * FUNCTION: ResetCameraUpdateInfo
 *
 * PURPOSE: Reset all camera tracking state to defaults.
 */
void ResetCameraUpdateInfo(void)
{
    gFieldCamera.movementSpeedX = 0;
    gFieldCamera.movementSpeedY = 0;
    gFieldCamera.x = 0;
    gFieldCamera.y = 0;
    gFieldCamera.spriteId = 0;
    gFieldCamera.callback = NULL;
}

/**
 * FUNCTION: InitCameraUpdateCallback
 *
 * PURPOSE: Set up the camera to track a specific sprite.
 *
 * HOW IT WORKS:
 * Creates a "camera object" sprite that follows the tracked sprite and
 * provides smooth movement data. Sets the camera update callback to
 * CameraUpdateCallback which reads the sprite's position each frame.
 *
 * PARAMETERS:
 * @param trackedSpriteId -- ID of the sprite to follow (usually the player)
 *
 * RETURNS: Always 0
 */
u32 InitCameraUpdateCallback(u8 trackedSpriteId)
{
    if (gFieldCamera.spriteId != 0)
        DestroySprite(&gSprites[gFieldCamera.spriteId]);
    gFieldCamera.spriteId = AddCameraObject(trackedSpriteId);
    gFieldCamera.callback = CameraUpdateCallback;
    return 0;
}

/**
 * FUNCTION: CameraUpdate
 *
 * PURPOSE: Main per-frame camera update -- handles scrolling and tilemap updates.
 *
 * HOW IT WORKS:
 * 1. Calls the camera callback to get the current movement speed
 * 2. Determines if a new metatile boundary has been crossed (deltaX/deltaY)
 * 3. Accumulates sub-tile pixel offset (wrapping at 16 = one metatile width)
 * 4. If a boundary was crossed:
 *    - Moves the camera's map position (CameraMove)
 *    - Updates all object event positions relative to the camera
 *    - Shifts the tilemap offset and redraws newly exposed tiles
 * 5. Updates the pixel-level scroll offset for smooth movement
 * 6. Updates global camera pixel totals (used for sprite positioning)
 *
 * The boundary detection logic checks if the camera's sub-tile position (x, y)
 * just crossed 0 (wrapped around from 15 to 0), which means a new tile is
 * entering the screen edge.
 */
void CameraUpdate(void)
{
    int deltaX;
    int deltaY;
    int curMovementOffsetY;
    int curMovementOffsetX;
    int movementSpeedX;
    int movementSpeedY;

    if (gFieldCamera.callback != NULL)
        gFieldCamera.callback(&gFieldCamera);
    movementSpeedX = gFieldCamera.movementSpeedX;
    movementSpeedY = gFieldCamera.movementSpeedY;
    deltaX = 0;
    deltaY = 0;
    curMovementOffsetX = gFieldCamera.x;
    curMovementOffsetY = gFieldCamera.y;

    /* Detect metatile boundary crossings for horizontal movement */
    if (curMovementOffsetX == 0 && movementSpeedX != 0)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    /* Detect metatile boundary crossings for vertical movement */
    if (curMovementOffsetY == 0 && movementSpeedY != 0)
    {
        if (movementSpeedY > 0)
            deltaY = 1;
        else
            deltaY = -1;
    }
    /* Handle reverse direction boundary crossing */
    if (curMovementOffsetX != 0 && curMovementOffsetX == -movementSpeedX)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY != 0 && curMovementOffsetY == -movementSpeedY)
    {
        if (movementSpeedY > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }

    /* Accumulate sub-tile offset, wrapping at 16 (one metatile = 16 pixels) */
    gFieldCamera.x += movementSpeedX;
    gFieldCamera.x = gFieldCamera.x - 16 * (gFieldCamera.x / 16);
    gFieldCamera.y += movementSpeedY;
    gFieldCamera.y = gFieldCamera.y - 16 * (gFieldCamera.y / 16);

    /* If a metatile boundary was crossed, update the map and tilemap */
    if (deltaX != 0 || deltaY != 0)
    {
        CameraMove(deltaX, deltaY);
        UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
        // RotatingGatePuzzleCameraUpdate(deltaX, deltaY);
        // ResetBerryTreeSparkleFlags();
        /* Advance tilemap offset by 2 tiles (1 metatile = 2 hardware tiles) */
        tilemap_move_something(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        /* Redraw the newly exposed edge of the map */
        RedrawMapSlicesForCameraUpdate(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
    }

    /* Update pixel-level scroll offset for smooth sub-tile movement */
    coords8_add(&sFieldCameraOffset, movementSpeedX, movementSpeedY);
    /* Track total camera displacement (used for sprite world positioning) */
    gTotalCameraPixelOffsetX -= movementSpeedX;
    gTotalCameraPixelOffsetY -= movementSpeedY;
}

/**
 * FUNCTION: MoveCameraAndRedrawMap (unused)
 *
 * PURPOSE: Instantly move the camera by a tile delta and redraw the full map.
 *
 * PARAMETERS:
 * @param deltaX -- Tiles to move horizontally
 * @param deltaY -- Tiles to move vertically
 */
void MoveCameraAndRedrawMap(int deltaX, int deltaY) // unused
{
    CameraMove(deltaX, deltaY);
    UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
    DrawWholeMapView();
    gTotalCameraPixelOffsetX -= deltaX * 16;
    gTotalCameraPixelOffsetY -= deltaY * 16;
}

/**
 * FUNCTION: CameraUpdateNoObjectRefresh
 *
 * PURPOSE: Same as CameraUpdate but without updating object event positions.
 *
 * HOW IT WORKS:
 * Identical to CameraUpdate but skips UpdateObjectEventsForCameraUpdate and
 * the global pixel offset update. Used during special camera sequences where
 * objects shouldn't move with the camera (e.g., scripted camera pans).
 */
void CameraUpdateNoObjectRefresh(void)
{
    int deltaX;
    int deltaY;
    int curMovementOffsetY;
    int curMovementOffsetX;
    int movementSpeedX;
    int movementSpeedY;

    if (gFieldCamera.callback != NULL)
        gFieldCamera.callback(&gFieldCamera);
    movementSpeedX = gFieldCamera.movementSpeedX;
    movementSpeedY = gFieldCamera.movementSpeedY;
    deltaX = 0;
    deltaY = 0;
    curMovementOffsetX = gFieldCamera.x;
    curMovementOffsetY = gFieldCamera.y;


    if (curMovementOffsetX == 0 && movementSpeedX != 0)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY == 0 && movementSpeedY != 0)
    {
        if (movementSpeedY > 0)
            deltaY = 1;
        else
            deltaY = -1;
    }
    if (curMovementOffsetX != 0 && curMovementOffsetX == -movementSpeedX)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY != 0 && curMovementOffsetY == -movementSpeedY)
    {
        if (movementSpeedY > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }

    gFieldCamera.x += movementSpeedX;
    gFieldCamera.x = gFieldCamera.x - 16 * (gFieldCamera.x / 16);
    gFieldCamera.y += movementSpeedY;
    gFieldCamera.y = gFieldCamera.y - 16 * (gFieldCamera.y / 16);

    if (deltaX != 0 || deltaY != 0)
    {
        CameraMove(deltaX, deltaY);
        // UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
        // RotatingGatePuzzleCameraUpdate(deltaX, deltaY);
        // ResetBerryTreeSparkleFlags();
        tilemap_move_something(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        RedrawMapSlicesForCameraUpdate(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
    }

    coords8_add(&sFieldCameraOffset, movementSpeedX, movementSpeedY);
    // gTotalCameraPixelOffsetX -= movementSpeedX;
    // gTotalCameraPixelOffsetY -= movementSpeedY;
}

/**
 * FUNCTION: SetCameraPanningCallback
 *
 * PURPOSE: Set a custom camera panning callback function.
 *
 * PARAMETERS:
 * @param a -- Callback function (called every frame to update pan offsets)
 */
void SetCameraPanningCallback(void (*a)(void))
{
    sFieldCameraPanningCallback = a;
}

/**
 * FUNCTION: SetCameraPanning
 *
 * PURPOSE: Set the camera pan offset directly.
 *
 * HOW IT WORKS:
 * The +32 on the vertical pan is the default vertical offset that centers
 * the player on screen. Adding to this value pans the camera down, subtracting
 * pans it up.
 *
 * PARAMETERS:
 * @param a -- Horizontal pan in pixels (positive = pan right)
 * @param b -- Vertical pan in pixels (positive = pan down, added to base offset of 32)
 */
void SetCameraPanning(s16 a, s16 b)
{
    sHorizontalCameraPan = a;
    sVerticalCameraPan = b + 32;
}

/**
 * FUNCTION: InstallCameraPanAheadCallback
 *
 * PURPOSE: Set up the "pan ahead" camera mode used on the bicycle.
 *
 * HOW IT WORKS:
 * Installs CameraPanningCB_PanAhead as the panning callback and resets
 * the pan values to their defaults (horizontal center, vertical 32 = standard).
 *
 * GAME LOGIC:
 * When riding the bike, the camera pans slightly ahead in the direction
 * of travel so the player can see more of the path ahead.
 */
void InstallCameraPanAheadCallback(void)
{
    sFieldCameraPanningCallback = CameraPanningCB_PanAhead;
    sBikeCameraPanFlag = FALSE;
    sHorizontalCameraPan = 0;
    sVerticalCameraPan = 32;
}

/**
 * FUNCTION: UpdateCameraPanning
 *
 * PURPOSE: Run the panning callback and update the global sprite coordinate offset.
 *
 * HOW IT WORKS:
 * Calls the current panning callback (if any) to update pan values, then
 * calculates the sprite coordinate offset. This offset is applied to all
 * overworld sprites so they appear at the correct screen position relative
 * to the scrolled and panned camera.
 *
 * The formula: spriteOffset = totalCameraOffset - panOffset
 * ensures sprites stay aligned with the background layers.
 */
void UpdateCameraPanning(void)
{
    if (sFieldCameraPanningCallback != NULL)
        sFieldCameraPanningCallback();
    /* Update the global sprite coordinate offsets to match camera position */
    // Update sprite offset of overworld objects
    gSpriteCoordOffsetX = gTotalCameraPixelOffsetX - sHorizontalCameraPan;
    gSpriteCoordOffsetY = gTotalCameraPixelOffsetY - sVerticalCameraPan - 8;
}

/**
 * FUNCTION: CameraPanningCB_PanAhead
 *
 * PURPOSE: Gradually pan the camera ahead of the player when riding the bike.
 *
 * HOW IT WORKS:
 * If gBikeCameraAheadPanback is FALSE (default), resets panning to standard.
 * If TRUE (bike pan-ahead mode), gradually shifts the camera's vertical pan
 * based on the player's movement direction:
 *   - Moving north (dir 2): pan up (sVerticalCameraPan decreases toward -8)
 *   - Moving south (dir 1): pan down (sVerticalCameraPan increases toward 72)
 *   - Not moving vertically: return to center (32)
 *
 * The pan changes by 2 pixels per frame for smooth camera movement.
 * NOTE: The original comment indicates this code path is never reached
 * in the normal game (gBikeCameraAheadPanback is always FALSE in FR/LG).
 */
static void CameraPanningCB_PanAhead(void)
{
    u8 var;

    if (gBikeCameraAheadPanback == FALSE)
    {
        InstallCameraPanAheadCallback();
    }
    else
    {
        // this code is never reached.
        if (gPlayerAvatar.tileTransitionState == 1)
        {
            sBikeCameraPanFlag ^= 1;
            if (sBikeCameraPanFlag == FALSE)
                return;
        }
        else
        {
            sBikeCameraPanFlag = FALSE;
        }

        var = GetPlayerMovementDirection();
        if (var == 2)
        {
            if (sVerticalCameraPan > -8)
                sVerticalCameraPan -= 2;
        }
        else if (var == 1)
        {
            if (sVerticalCameraPan < 72)
                sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan < 32)
        {
            sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan > 32)
        {
            sVerticalCameraPan -= 2;
        }
    }
}
