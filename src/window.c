/**
 * window.c - GBA Text/UI Window Management System
 *
 * ============================================================================
 * GBA WINDOW SYSTEM OVERVIEW (Software Windows, not Hardware Windows)
 * ============================================================================
 *
 * IMPORTANT DISTINCTION: This file implements SOFTWARE windows -- rectangular
 * regions on background layers used for drawing text, menus, and UI elements.
 * These are NOT the same as the GBA's HARDWARE window feature (WIN0/WIN1),
 * which clips what layers are visible in certain screen regions.
 *
 * HOW SOFTWARE WINDOWS WORK:
 *
 * The GBA's text BG modes (Mode 0) use tile-based backgrounds. Each background
 * is a grid of 8x8 pixel tiles. A "window" in this system is a rectangular
 * region of tiles on one of the four background layers (BG0-BG3).
 *
 * Each window has:
 *   - A background layer (bg): Which of the 4 BG layers this window lives on
 *   - Position (tilemapLeft, tilemapTop): Where on the BG tilemap this window starts
 *   - Size (width, height): Dimensions in tiles (each tile = 8x8 pixels)
 *   - A palette number: Which of the 16 available 16-color palettes to use
 *   - A base tile block: The starting tile index in VRAM for this window's graphics
 *   - Tile data buffer: RAM buffer where pixel data is drawn before copying to VRAM
 *
 * THE TWO-BUFFER ARCHITECTURE:
 *
 * Each BG layer has two things that need updating:
 *   1. TILEMAP BUFFER: Says WHICH tile to display at each grid position.
 *      Writing sequential tile indices here makes the window's tiles appear.
 *   2. TILE DATA (character data): The actual pixel art for each tile (8x8 pixels).
 *      Text rendering and bitmap operations write into this buffer.
 *
 * To display a window, you must:
 *   1. Write tile indices into the BG's tilemap (PutWindowTilemap)
 *   2. Copy the tilemap to VRAM (CopyWindowToVram with COPYWIN_MAP)
 *   3. Load the tile pixel data to VRAM (CopyWindowToVram with COPYWIN_GFX)
 *
 * TILE DATA FORMAT:
 * In 4bpp (4 bits per pixel) mode, each pixel is a 4-bit palette index.
 * One 8x8 tile = 8 rows * 8 pixels * 4 bits = 256 bits = 32 bytes (0x20).
 * This is why you see 0x20 (32) multiplied everywhere -- it's the byte size
 * of a single 4bpp tile.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"

/*
 * gWindowClearTile: The tile index used when clearing a window's tilemap region.
 * Typically tile 0, which is usually a blank/transparent tile.
 */
COMMON_DATA u8 gWindowClearTile = {0};

/*
 * gWindowBgTilemapBuffers: Pointers to tilemap buffers for each of the 4 BG layers.
 * When a window is created on a BG layer that doesn't have a tilemap buffer yet,
 * one is allocated from the heap. The special value 'nullsub_8' (a function pointer
 * used as a sentinel) means "this BG already has a tilemap set by the BG system
 * itself, so we didn't allocate it and shouldn't free it."
 */
COMMON_DATA void *gWindowBgTilemapBuffers[4] = {0};

/*
 * gWindows: The master array of all active windows. Each entry tracks the
 * window's template (position, size, palette) and a pointer to its tile data
 * buffer in RAM. A window slot is "free" when its bg field is 0xFF.
 */
EWRAM_DATA struct Window gWindows[WINDOWS_MAX] = {0};

static u8 GetNumActiveWindowsOnBg(u8 bgId);

/*
 * sDummyWindowTemplate: Sentinel value representing an unused window slot.
 * The bg field is set to 0xFF, which is used throughout the code to check
 * whether a window slot is occupied or free.
 */
static const struct WindowTemplate sDummyWindowTemplate = {0xFF, 0, 0, 0, 0, 0, 0};

/**
 * FUNCTION: nullsub_8
 *
 * PURPOSE: A do-nothing function whose ADDRESS is used as a sentinel value.
 *
 * HOW IT WORKS:
 * This function does absolutely nothing. Its purpose is to provide a non-NULL
 * pointer that can be stored in gWindowBgTilemapBuffers[] to indicate "this
 * BG layer already has a tilemap buffer assigned by the BG system -- we didn't
 * allocate it, so we shouldn't free it." This distinguishes three states:
 *   - NULL: No tilemap buffer exists; one needs to be allocated
 *   - nullsub_8: A tilemap buffer exists (set externally); don't touch it
 *   - Any other pointer: We allocated this buffer; we're responsible for freeing it
 */
static void nullsub_8(void)
{

}

/**
 * FUNCTION: InitWindows
 *
 * PURPOSE: Initialize the entire window system from an array of window templates.
 *
 * HOW IT WORKS:
 * 1. Scans all 4 BG layers: if a BG already has a tilemap buffer (set by the BG
 *    system), marks it with the nullsub_8 sentinel so we know not to free it later.
 *    If no buffer exists, stores NULL so we know to allocate one when needed.
 * 2. Resets all window slots to the dummy template (bg = 0xFF = unused).
 * 3. Iterates through the provided template array (terminated by bg == 0xFF),
 *    creating each window:
 *    - If auto-allocation is enabled, finds free tile space in VRAM for the window
 *    - If the BG layer doesn't have a tilemap buffer yet, allocates one
 *    - Allocates a RAM buffer for the window's tile pixel data
 *      (size = 0x20 bytes per tile * width * height tiles)
 *    - Records the window in the gWindows[] array
 *
 * GBA CONTEXT:
 * The GBA has limited VRAM for tile data. Multiple windows can share a BG layer
 * but each needs its own region of tile indices. The auto-allocation system
 * (BgTileAllocOp) manages this tile space like a memory allocator manages RAM.
 *
 * PARAMETERS:
 * @param templates -- Array of WindowTemplate structs, terminated by one with bg == 0xFF
 *
 * RETURNS: TRUE if all windows were created successfully, FALSE if allocation failed
 */
bool16 InitWindows(const struct WindowTemplate *templates)
{
    int i;
    void *bgTilemapBuffer;
    int j;
    u8 bgLayer;
    u16 bgSize;
    u8 *allocatedTilemapBuffer;
    int allocatedBaseBlock;

    /*
     * Phase 1: Check each BG layer's tilemap buffer status.
     * If a buffer already exists (set by SetBgTilemapBuffer elsewhere),
     * store nullsub_8 as a sentinel meaning "don't free this."
     * If no buffer exists, store NULL meaning "needs allocation."
     */
    for (i = 0; i < 4; ++i)
    {
        bgTilemapBuffer = GetBgTilemapBuffer(i);
        if (bgTilemapBuffer != NULL)
            gWindowBgTilemapBuffers[i] = nullsub_8;
        else
            gWindowBgTilemapBuffers[i] = bgTilemapBuffer;
    }

    /* Phase 2: Mark all window slots as unused (bg = 0xFF) and clear tile data */
    for (i = 0; i < WINDOWS_MAX; ++i)
    {
        gWindows[i].window = sDummyWindowTemplate;
        gWindows[i].tileData = NULL;
    }

    /*
     * Phase 3: Create each window from the template array.
     * The loop terminates when it hits a template with bg == 0xFF (the end sentinel)
     * or when we've filled all available window slots.
     */
    for (i = 0, allocatedBaseBlock = 0, bgLayer = templates[i].bg; bgLayer != 0xFF && i < WINDOWS_MAX; ++i, bgLayer = templates[i].bg)
    {
        /*
         * If tile auto-allocation is enabled, find a contiguous block of free
         * tile indices in the BG's tile space. The number of tiles needed is
         * width * height (the window's dimensions in tiles).
         */
        if (gWindowTileAutoAllocEnabled == TRUE)
        {
            allocatedBaseBlock = BgTileAllocOp(bgLayer, 0, templates[i].width * templates[i].height, BG_TILE_FIND_FREE_SPACE);
            if (allocatedBaseBlock == -1)
                return FALSE;
        }

        /*
         * If this BG layer doesn't have a tilemap buffer yet (NULL),
         * allocate one. The tilemap size depends on the BG's configured
         * map size (can be 256x256, 256x512, 512x256, or 512x512 pixels).
         */
        if (gWindowBgTilemapBuffers[bgLayer] == NULL)
        {
            bgSize = GetBgAttribute(bgLayer, BG_ATTR_MAPSIZE);

            if (bgSize != 0xFFFF)
            {
                allocatedTilemapBuffer = Alloc(bgSize);

                if (allocatedTilemapBuffer == NULL)
                {
                    FreeAllWindowBuffers();
                    return FALSE;
                }

                /* Zero out the tilemap -- all positions start showing tile 0 (blank) */
                for (j = 0; j < bgSize; ++j)
                    allocatedTilemapBuffer[j] = 0;

                gWindowBgTilemapBuffers[bgLayer] = allocatedTilemapBuffer;
                SetBgTilemapBuffer(bgLayer, allocatedTilemapBuffer);
            }
        }

        /*
         * Allocate the tile data buffer for this window's pixel content.
         * Size = 0x20 (32) bytes per tile * number of tiles.
         * 0x20 bytes = one 8x8 tile in 4bpp format (4 bits per pixel).
         * This buffer is where text rendering and drawing operations write pixels
         * before they get copied to VRAM.
         */
        allocatedTilemapBuffer = Alloc((u16)(0x20 * (templates[i].width * templates[i].height)));

        if (allocatedTilemapBuffer == NULL)
        {
            /*
             * Allocation failed. If this was the only window on this BG layer
             * and we allocated the tilemap buffer ourselves (not nullsub_8 sentinel),
             * free the tilemap buffer too since no windows use it.
             */
            if ((GetNumActiveWindowsOnBg(bgLayer) == 0) && (gWindowBgTilemapBuffers[bgLayer] != nullsub_8))
            {
                Free(gWindowBgTilemapBuffers[bgLayer]);
                gWindowBgTilemapBuffers[bgLayer] = allocatedTilemapBuffer;
            }

            return FALSE;
        }

        /* Store the tile data buffer and template in the window slot */
        gWindows[i].tileData = allocatedTilemapBuffer;
        gWindows[i].window = templates[i];

        /*
         * If auto-allocation is on, record which tile index block was assigned
         * to this window, and mark those tiles as "in use" in the allocator.
         */
        if (gWindowTileAutoAllocEnabled == TRUE)
        {
            gWindows[i].window.baseBlock = allocatedBaseBlock;
            BgTileAllocOp(bgLayer, allocatedBaseBlock, templates[i].width * templates[i].height, BG_TILE_ALLOC);
        }
    }

    gWindowClearTile = 0;
    return TRUE;
}

/**
 * FUNCTION: AddWindow
 *
 * PURPOSE: Add a single new window at runtime (after InitWindows has been called).
 *
 * HOW IT WORKS:
 * 1. Scans the gWindows[] array for the first free slot (bg == 0xFF)
 * 2. If auto-allocation is on, finds free tile space for the new window
 * 3. If the target BG layer has no tilemap buffer, allocates one
 * 4. Allocates a tile data buffer for the window's pixel content
 * 5. Stores the window template and returns the window ID (slot index)
 *
 * This is used for dynamically created UI elements like popup menus,
 * dialog boxes, and context-sensitive UI that appears during gameplay.
 *
 * PARAMETERS:
 * @param template -- Pointer to a WindowTemplate describing the new window
 *
 * RETURNS: The window ID (index into gWindows[]), or 0xFF if creation failed
 */
u16 AddWindow(const struct WindowTemplate *template)
{
    u16 win;
    u8 bgLayer;
    int allocatedBaseBlock;
    u16 bgSize;
    u8 *allocatedTilemapBuffer;
    int i;

    /* Find the first unused window slot (bg == 0xFF means the slot is free) */
    for (win = 0; win < WINDOWS_MAX; ++win)
    {
        if ((bgLayer = gWindows[win].window.bg) == 0xFF)
            break;
    }

    /* No free slots available */
    if (win == WINDOWS_MAX)
        return 0xFF;

    bgLayer = template->bg;
    allocatedBaseBlock = 0;

    /* Try to auto-allocate tile space if the feature is enabled */
    if (gWindowTileAutoAllocEnabled == TRUE)
    {
        allocatedBaseBlock = BgTileAllocOp(bgLayer, 0, template->width * template->height, BG_TILE_FIND_FREE_SPACE);

        if (allocatedBaseBlock == -1)
            return 0xFF;
    }

    /* If this BG doesn't have a tilemap buffer yet, allocate one */
    if (gWindowBgTilemapBuffers[bgLayer] == NULL)
    {
        bgSize = GetBgAttribute(bgLayer, BG_ATTR_MAPSIZE);

        if (bgSize != 0xFFFF)
        {
            allocatedTilemapBuffer = Alloc(bgSize);

            if (allocatedTilemapBuffer == NULL)
                return 0xFF;

            /* Zero the tilemap buffer */
            for (i = 0; i < bgSize; ++i)
                allocatedTilemapBuffer[i] = 0;

            gWindowBgTilemapBuffers[bgLayer] = allocatedTilemapBuffer;
            SetBgTilemapBuffer(bgLayer, allocatedTilemapBuffer);
        }
    }

    /*
     * Allocate the tile data buffer: 0x20 (32) bytes per 8x8 tile in 4bpp mode.
     * Total size = 32 * width_in_tiles * height_in_tiles.
     */
    allocatedTilemapBuffer = Alloc((u16)(0x20 * (template->width * template->height)));

    if (allocatedTilemapBuffer == NULL)
    {
        /* Clean up: free the tilemap buffer if we were the only user */
        if ((GetNumActiveWindowsOnBg(bgLayer) == 0) && (gWindowBgTilemapBuffers[bgLayer] != nullsub_8))
        {
            Free(gWindowBgTilemapBuffers[bgLayer]);
            gWindowBgTilemapBuffers[bgLayer] = allocatedTilemapBuffer;
        }
        return 0xFF;
    }

    /* Register the new window */
    gWindows[win].tileData = allocatedTilemapBuffer;
    gWindows[win].window = *template;

    /* Mark the tile space as occupied in the allocator */
    if (gWindowTileAutoAllocEnabled == TRUE)
    {
        gWindows[win].window.baseBlock = allocatedBaseBlock;
        BgTileAllocOp(bgLayer, allocatedBaseBlock, gWindows[win].window.width * gWindows[win].window.height, BG_TILE_ALLOC);
    }

    return win;
}

/**
 * FUNCTION: RemoveWindow
 *
 * PURPOSE: Destroy a window and free all its associated memory.
 *
 * HOW IT WORKS:
 * 1. If auto-allocation is on, frees the tile space this window was using
 * 2. Marks the window slot as unused (sets template to dummy)
 * 3. If this was the last window on its BG layer and we allocated the
 *    tilemap buffer (not the external sentinel), frees that too
 * 4. Frees the window's tile data buffer
 *
 * PARAMETERS:
 * @param windowId -- Index into gWindows[] identifying which window to remove
 */
void RemoveWindow(u8 windowId)
{
    u8 bgLayer = gWindows[windowId].window.bg;

    /* Free the tile indices this window was using */
    if (gWindowTileAutoAllocEnabled == TRUE)
    {
        BgTileAllocOp(bgLayer, gWindows[windowId].window.baseBlock, gWindows[windowId].window.width * gWindows[windowId].window.height, BG_TILE_FREE);
    }

    /* Mark the slot as unused */
    gWindows[windowId].window = sDummyWindowTemplate;

    /*
     * If no more windows use this BG layer and we own the tilemap buffer
     * (i.e., it's not the nullsub_8 sentinel meaning "externally managed"),
     * free the tilemap buffer.
     */
    if (GetNumActiveWindowsOnBg(bgLayer) == 0)
    {
        if (gWindowBgTilemapBuffers[bgLayer] != nullsub_8)
        {
            Free(gWindowBgTilemapBuffers[bgLayer]);
            gWindowBgTilemapBuffers[bgLayer] = 0;
        }
    }

    /* Free the tile pixel data buffer */
    if (gWindows[windowId].tileData != NULL)
    {
        Free(gWindows[windowId].tileData);
        gWindows[windowId].tileData = NULL;
    }
}

/**
 * FUNCTION: FreeAllWindowBuffers
 *
 * PURPOSE: Free all memory used by the window system (full teardown).
 *
 * HOW IT WORKS:
 * Iterates through all 4 BG tilemap buffers and all window tile data buffers,
 * freeing any that were allocated by this system (skipping nullsub_8 sentinels).
 * Called during screen transitions or when reinitializing the window system.
 */
void FreeAllWindowBuffers(void)
{
    int i;

    /* Free tilemap buffers for all 4 BG layers (skip externally-managed ones) */
    for (i = 0; i < 4; ++i)
    {
        if (gWindowBgTilemapBuffers[i] != NULL && gWindowBgTilemapBuffers[i] != nullsub_8)
        {
            Free(gWindowBgTilemapBuffers[i]);
            gWindowBgTilemapBuffers[i] = NULL;
        }
    }

    /* Free tile data buffers for all window slots */
    for (i = 0; i < WINDOWS_MAX; ++i)
    {
        if (gWindows[i].tileData != NULL)
        {
            Free(gWindows[i].tileData);
            gWindows[i].tileData = NULL;
        }
    }
}

/**
 * FUNCTION: CopyWindowToVram
 *
 * PURPOSE: Transfer a window's data from RAM to VRAM so the GBA can display it.
 *
 * HOW IT WORKS:
 * The GBA can only display graphics that are in VRAM (Video RAM, at 0x06000000).
 * Window contents are first drawn into RAM buffers, then this function copies
 * them to VRAM. There are three transfer modes:
 *
 *   COPYWIN_MAP: Copy the BG's tilemap buffer to VRAM. This updates WHICH tiles
 *     appear at each position but doesn't update the tile graphics themselves.
 *     Used after PutWindowTilemap or ClearWindowTilemap.
 *
 *   COPYWIN_GFX: Copy the window's tile pixel data to VRAM. This updates the
 *     actual graphics of each tile. Used after rendering text or drawing bitmaps
 *     into the window.
 *
 *   COPYWIN_FULL: Copy both tilemap AND tile data. Used when both have changed.
 *
 * GBA CONTEXT:
 * VRAM access on the GBA has timing constraints -- it can only be safely written
 * during VBlank or HBlank. These copy functions typically queue DMA transfers
 * that execute during the next VBlank.
 *
 * PARAMETERS:
 * @param windowId -- Index into gWindows[]
 * @param mode     -- COPYWIN_MAP, COPYWIN_GFX, or COPYWIN_FULL
 */
void CopyWindowToVram(u8 windowId, u8 mode)
{
    struct Window windowLocal = gWindows[windowId];
    /*
     * Calculate total byte size of the window's tile data:
     * 32 bytes per tile * width * height tiles
     */
    u16 windowSize = 32 * (windowLocal.window.width * windowLocal.window.height);

    switch (mode)
    {
        case COPYWIN_MAP:
            /* Copy only the tilemap (tile index map) to VRAM */
            CopyBgTilemapBufferToVram(windowLocal.window.bg);
            break;
        case COPYWIN_GFX:
            /* Copy only the tile pixel data to VRAM, starting at this window's base tile */
            LoadBgTiles(windowLocal.window.bg, windowLocal.tileData, windowSize, windowLocal.window.baseBlock);
            break;
        case COPYWIN_FULL:
            /* Copy both tile data and tilemap to VRAM */
            LoadBgTiles(windowLocal.window.bg, windowLocal.tileData, windowSize, windowLocal.window.baseBlock);
            CopyBgTilemapBufferToVram(windowLocal.window.bg);
            break;
    }
}

/**
 * FUNCTION: PutWindowTilemap
 *
 * PURPOSE: Write sequential tile indices into the BG tilemap so this window's
 *          tiles will be displayed at the correct screen position.
 *
 * HOW IT WORKS:
 * The BG tilemap is a grid where each cell says "display tile number N here."
 * This function fills the rectangular region occupied by this window with
 * sequential tile indices starting from the window's baseBlock. After calling
 * this, the BG hardware will display the window's tiles in order at the
 * correct screen position.
 *
 * For example, a 3x2 window with baseBlock=100 would write:
 *   100, 101, 102    (top row)
 *   103, 104, 105    (bottom row)
 * into the tilemap at the window's (tilemapLeft, tilemapTop) position.
 *
 * GBA CONTEXT:
 * GetBgAttribute with BG_ATTR_BASETILE retrieves the BG layer's tile base
 * offset, which is added to the window's baseBlock. This allows multiple
 * BG layers to share the same tile character data memory by using different
 * base offsets.
 *
 * PARAMETERS:
 * @param windowId -- Index into gWindows[]
 */
void PutWindowTilemap(u8 windowId)
{
    struct Window windowLocal = gWindows[windowId];

    WriteSequenceToBgTilemapBuffer(
        windowLocal.window.bg,
        GetBgAttribute(windowLocal.window.bg, BG_ATTR_BASETILE) + windowLocal.window.baseBlock,
        windowLocal.window.tilemapLeft,
        windowLocal.window.tilemapTop,
        windowLocal.window.width,
        windowLocal.window.height,
        windowLocal.window.paletteNum,
        1);  /* increment = 1: each successive tile gets the next tile index */
}

/**
 * FUNCTION: PutWindowRectTilemapOverridePalette
 *
 * PURPOSE: Write tile indices for a sub-rectangle of a window, using a
 *          different palette than the window's default.
 *
 * HOW IT WORKS:
 * Similar to PutWindowTilemap but operates on a rectangular sub-region and
 * allows overriding the palette. Calculates the starting tile index based on
 * the sub-rectangle's position within the window, then writes sequential
 * tile indices row by row.
 *
 * This is useful for highlighting specific parts of a window (e.g., a
 * selected menu item) with a different color palette.
 *
 * PARAMETERS:
 * @param windowId -- Index into gWindows[]
 * @param x        -- Left edge of the sub-rectangle (in tiles, relative to window)
 * @param y        -- Top edge of the sub-rectangle (in tiles, relative to window)
 * @param width    -- Width of the sub-rectangle in tiles
 * @param height   -- Height of the sub-rectangle in tiles
 * @param palette  -- Palette number to use (overrides the window's default)
 */
void PutWindowRectTilemapOverridePalette(u8 windowId, u8 x, u8 y, u8 width, u8 height, u8 palette)
{
    struct Window windowLocal = gWindows[windowId];
    /*
     * Calculate the starting tile index for this sub-rectangle.
     * baseBlock + (y * window.width) + x gives the tile index at position (x,y)
     * within the window's tile allocation. BG_ATTR_BASETILE adds the layer offset.
     */
    u16 currentRow = windowLocal.window.baseBlock + (y * windowLocal.window.width) + x + GetBgAttribute(windowLocal.window.bg, BG_ATTR_BASETILE);
    int i;

    /* Write one row at a time, advancing the tile index by the full window width */
    for (i = 0; i < height; ++i)
    {
        WriteSequenceToBgTilemapBuffer(
            windowLocal.window.bg,
            currentRow,
            windowLocal.window.tilemapLeft + x,
            windowLocal.window.tilemapTop + y + i,
            width,
            1,       /* height = 1 row at a time */
            palette,
            1);      /* increment = 1 */

        /*
         * Move to the next row in the window's tile allocation.
         * We advance by the full window width (not the sub-rect width)
         * because the tiles are laid out in window-width-sized rows.
         */
        currentRow += windowLocal.window.width;
    }
}

/**
 * FUNCTION: ClearWindowTilemap
 *
 * PURPOSE: Remove a window from the screen by filling its tilemap region
 *          with the clear tile (usually tile 0 = transparent/blank).
 *
 * HOW IT WORKS:
 * Fills the window's rectangular region in the BG tilemap with gWindowClearTile.
 * After copying the tilemap to VRAM, the window's area will show the clear tile
 * (typically blank), effectively hiding the window.
 *
 * Note: This does NOT free the window's memory -- it just hides it on screen.
 * Use RemoveWindow to actually destroy the window.
 *
 * PARAMETERS:
 * @param windowId -- Index into gWindows[]
 */
void ClearWindowTilemap(u8 windowId)
{
    struct Window windowLocal = gWindows[windowId];

    FillBgTilemapBufferRect(
        windowLocal.window.bg,
        gWindowClearTile,
        windowLocal.window.tilemapLeft,
        windowLocal.window.tilemapTop,
        windowLocal.window.width,
        windowLocal.window.height,
        windowLocal.window.paletteNum);
}

/**
 * FUNCTION: PutWindowRectTilemap
 *
 * PURPOSE: Write tile indices for a sub-rectangle of a window using the
 *          window's default palette.
 *
 * HOW IT WORKS:
 * Same as PutWindowRectTilemapOverridePalette but uses the window's own
 * paletteNum instead of an override. Used to partially show or refresh
 * a portion of a window on screen.
 *
 * PARAMETERS:
 * @param windowId -- Index into gWindows[]
 * @param x        -- Left edge (tiles, relative to window)
 * @param y        -- Top edge (tiles, relative to window)
 * @param width    -- Width in tiles
 * @param height   -- Height in tiles
 */
void PutWindowRectTilemap(u8 windowId, u8 x, u8 y, u8 width, u8 height)
{
    struct Window windowLocal = gWindows[windowId];
    u16 currentRow = windowLocal.window.baseBlock + (y * windowLocal.window.width) + x + GetBgAttribute(windowLocal.window.bg, BG_ATTR_BASETILE);
    int i;

    for (i = 0; i < height; ++i)
    {
        WriteSequenceToBgTilemapBuffer(
            windowLocal.window.bg,
            currentRow,
            windowLocal.window.tilemapLeft + x,
            windowLocal.window.tilemapTop + y + i,
            width,
            1,
            windowLocal.window.paletteNum,
            1);

        currentRow += windowLocal.window.width;
    }
}

/**
 * FUNCTION: BlitBitmapToWindow
 *
 * PURPOSE: Copy a bitmap image into a window's tile data buffer.
 *
 * HOW IT WORKS:
 * A convenience wrapper around BlitBitmapRectToWindow that copies the entire
 * source bitmap. "Blit" stands for "Block Image Transfer" -- a term from
 * computer graphics meaning to copy a rectangular block of pixels.
 *
 * The source bitmap must be in 4bpp (4 bits per pixel) format to match the
 * GBA's tile format.
 *
 * PARAMETERS:
 * @param windowId -- Target window
 * @param pixels   -- Source bitmap pixel data (4bpp format)
 * @param x        -- X position in the window to place the bitmap (in pixels)
 * @param y        -- Y position in the window to place the bitmap (in pixels)
 * @param width    -- Width of the source bitmap in pixels
 * @param height   -- Height of the source bitmap in pixels
 */
void BlitBitmapToWindow(u8 windowId, const u8 *pixels, u16 x, u16 y, u16 width, u16 height)
{
    BlitBitmapRectToWindow(windowId, pixels, 0, 0, width, height, x, y, width, height);
}

/**
 * FUNCTION: BlitBitmapRectToWindow
 *
 * PURPOSE: Copy a rectangular sub-region of a source bitmap into a window.
 *
 * HOW IT WORKS:
 * Sets up source and destination Bitmap structs, then calls BlitBitmapRect4Bit
 * to perform the actual pixel copying. The destination bitmap is the window's
 * tile data buffer, with dimensions calculated from the window's tile size
 * (each tile = 8x8 pixels, so total pixels = 8 * width_in_tiles).
 *
 * The colorKey of 0 means no transparency -- all pixels are copied.
 *
 * PARAMETERS:
 * @param windowId   -- Target window
 * @param pixels     -- Source bitmap pixel data
 * @param srcX       -- X offset into the source bitmap
 * @param srcY       -- Y offset into the source bitmap
 * @param srcWidth   -- Total width of the source bitmap in pixels
 * @param srcHeight  -- Total height of the source bitmap in pixels
 * @param destX      -- X position in the window (in pixels)
 * @param destY      -- Y position in the window (in pixels)
 * @param rectWidth  -- Width of the rectangle to copy (in pixels)
 * @param rectHeight -- Height of the rectangle to copy (in pixels)
 */
void BlitBitmapRectToWindow(u8 windowId, const u8 *pixels, u16 srcX, u16 srcY, u16 srcWidth, int srcHeight, u16 destX, u16 destY, u16 rectWidth, u16 rectHeight)
{
    struct Bitmap sourceRect;
    struct Bitmap destRect;

    sourceRect.pixels = (u8 *)pixels;
    sourceRect.width = srcWidth;
    sourceRect.height = srcHeight;

    destRect.pixels = gWindows[windowId].tileData;
    destRect.width = 8 * gWindows[windowId].window.width;    /* Convert tile width to pixel width */
    destRect.height = 8 * gWindows[windowId].window.height;  /* Convert tile height to pixel height */

    BlitBitmapRect4Bit(&sourceRect, &destRect, srcX, srcY, destX, destY, rectWidth, rectHeight, 0);
}

/**
 * FUNCTION: BlitBitmapRectToWindowWithColorKey
 *
 * PURPOSE: Copy a rectangular bitmap region into a window with transparency.
 *
 * HOW IT WORKS:
 * Same as BlitBitmapRectToWindow but with a colorKey parameter. Any pixel in
 * the source bitmap matching the colorKey value will NOT be copied, leaving
 * the destination pixel unchanged. This implements transparent sprites/icons.
 *
 * GBA CONTEXT:
 * In 4bpp mode, each pixel is a value 0-15 (palette index). Color index 0 is
 * conventionally transparent, so colorKey is often 0.
 *
 * PARAMETERS:
 * @param windowId   -- Target window
 * @param pixels     -- Source bitmap pixel data
 * @param srcX, srcY -- Source rectangle offset
 * @param srcWidth, srcHeight -- Source bitmap dimensions
 * @param destX, destY -- Destination position in window (pixels)
 * @param rectWidth, rectHeight -- Rectangle size to copy (pixels)
 * @param colorKey   -- Pixel value to treat as transparent (skip copying)
 */
void BlitBitmapRectToWindowWithColorKey(u8 windowId, const u8 *pixels, u16 srcX, u16 srcY, u16 srcWidth, int srcHeight, u16 destX, u16 destY, u16 rectWidth, u16 rectHeight, u8 colorKey)
{
    struct Bitmap sourceRect;
    struct Bitmap destRect;

    sourceRect.pixels = (u8 *)pixels;
    sourceRect.width = srcWidth;
    sourceRect.height = srcHeight;

    destRect.pixels = gWindows[windowId].tileData;
    destRect.width = 8 * gWindows[windowId].window.width;
    destRect.height = 8 * gWindows[windowId].window.height;

    BlitBitmapRect4Bit(&sourceRect, &destRect, srcX, srcY, destX, destY, rectWidth, rectHeight, colorKey);
}

/**
 * FUNCTION: FillWindowPixelRect
 *
 * PURPOSE: Fill a rectangular region within a window with a solid color.
 *
 * HOW IT WORKS:
 * Creates a Bitmap struct pointing to the window's tile data, then calls
 * FillBitmapRect4Bit to fill the specified rectangle with the given pixel value.
 * Used for drawing solid-color backgrounds, clearing text areas, etc.
 *
 * PARAMETERS:
 * @param windowId  -- Target window
 * @param fillValue -- 4-bit pixel value to fill with (0-15, palette index)
 * @param x         -- Left edge in pixels
 * @param y         -- Top edge in pixels
 * @param width     -- Width in pixels
 * @param height    -- Height in pixels
 */
void FillWindowPixelRect(u8 windowId, u8 fillValue, u16 x, u16 y, u16 width, u16 height)
{
    struct Bitmap pixelRect;

    pixelRect.pixels = gWindows[windowId].tileData;
    pixelRect.width = 8 * gWindows[windowId].window.width;
    pixelRect.height = 8 * gWindows[windowId].window.height;

    FillBitmapRect4Bit(&pixelRect, x, y, width, height, fillValue);
}

/**
 * FUNCTION: CopyToWindowPixelBuffer
 *
 * PURPOSE: Copy raw tile data or LZ77-compressed tile data into a window's
 *          pixel buffer at a specific tile offset.
 *
 * HOW IT WORKS:
 * If size is non-zero, performs a direct memory copy of 'size' bytes.
 * If size is zero, treats 'src' as LZ77-compressed data and decompresses it.
 * The tileOffset parameter allows writing to a specific tile within the window
 * (multiplied by 0x20 = 32 bytes per tile to get the byte offset).
 *
 * GBA CONTEXT:
 * LZ77 is a compression algorithm supported natively by the GBA's BIOS.
 * Game assets (graphics, tilemaps) are often LZ77-compressed in the ROM to
 * save space, then decompressed into RAM/VRAM when needed.
 * CpuCopy16 is a 16-bit-aligned memory copy (the GBA's memory bus is 16-bit
 * for most regions, so 16-bit copies are efficient).
 *
 * PARAMETERS:
 * @param windowId   -- Target window
 * @param src        -- Source data (raw or LZ77-compressed)
 * @param size       -- Number of bytes to copy (0 = decompress instead)
 * @param tileOffset -- Starting tile index within the window's buffer
 */
void CopyToWindowPixelBuffer(u8 windowId, const void *src, u16 size, u16 tileOffset)
{
    if (size != 0)
        CpuCopy16(src, gWindows[windowId].tileData + (0x20 * tileOffset), size);
    else
        LZ77UnCompWram(src, gWindows[windowId].tileData + (0x20 * tileOffset));
}

/**
 * FUNCTION: FillWindowPixelBuffer
 *
 * PURPOSE: Fill an entire window's tile data buffer with a single pixel value.
 *
 * HOW IT WORKS:
 * Calculates the total number of tiles (width * height), multiplies by 0x20
 * (32 bytes per tile) to get the buffer size, then fills the entire buffer
 * using CpuFastFill8. This is the fastest way to clear a window to a solid
 * color -- commonly used to erase all text before redrawing.
 *
 * GBA CONTEXT:
 * CpuFastFill8 is an optimized fill function that uses 32-bit writes internally
 * (by replicating the 8-bit value across all 4 bytes of a word) for maximum
 * speed on the GBA's 32-bit ARM CPU.
 *
 * PARAMETERS:
 * @param windowId  -- Target window
 * @param fillValue -- 8-bit fill value. In 4bpp mode, this fills two pixels at once
 *                     (each byte holds two 4-bit pixel values, high and low nybble).
 *                     A value of 0x00 = two transparent pixels; 0x11 = two pixels of color 1.
 */
void FillWindowPixelBuffer(u8 windowId, u8 fillValue)
{
    int fillSize = gWindows[windowId].window.width * gWindows[windowId].window.height;
    CpuFastFill8(fillValue, gWindows[windowId].tileData, 0x20 * fillSize);
}

/*
 * MOVE_TILES_DOWN macro: Used by ScrollWindow to shift tile rows upward
 * (scroll content down / new content appears at bottom).
 *
 * This macro operates on 4 bytes at a time (one u32 word). For each row of
 * pixels within a tile, it calculates where that pixel data should come from
 * after scrolling, and copies it. If the source offset is beyond the buffer
 * (i.e., we've scrolled past the content), it fills with the fill color instead.
 *
 * The expression ((width * (distanceLoop & ~7)) | (distanceLoop & 7)) is
 * a bit-manipulation trick for navigating the GBA's tile memory layout:
 *   - Tiles are 8 rows tall, so bits 0-2 of distanceLoop select the row
 *     within a tile (distanceLoop & 7)
 *   - Bits 3+ select which tile row we're in (distanceLoop & ~7), multiplied
 *     by the window width to skip across tile columns
 *   - The | combines these to get the correct byte offset in the interleaved
 *     tile layout
 * Multiplied by 4 because each row-within-a-tile is 4 bytes (8 pixels * 4bpp / 8).
 */
#define MOVE_TILES_DOWN(a)                                                      \
{                                                                               \
    destOffset = i + (a);                                                       \
    srcOffset = i + (((width * (distanceLoop & ~7)) | (distanceLoop & 7)) * 4); \
    if (srcOffset < size)                                                       \
        *(u32 *)(tileData + destOffset) = *(u32 *)(tileData + srcOffset);         \
    else                                                                        \
        *(u32 *)(tileData + destOffset) = fillValue32;                           \
    distanceLoop++;                                                             \
}

/*
 * MOVE_TILES_UP macro: Same concept as MOVE_TILES_DOWN but scrolls in the
 * opposite direction (content moves up, new content appears at top).
 * Notice the subtraction (tileData - destOffset/srcOffset) instead of addition.
 */
#define MOVE_TILES_UP(a)                                                        \
{                                                                               \
    destOffset = i + (a);                                                       \
    srcOffset = i + (((width * (distanceLoop & ~7)) | (distanceLoop & 7)) * 4); \
    if (srcOffset < size)                                                       \
        *(u32 *)(tileData - destOffset) = *(u32 *)(tileData - srcOffset);         \
    else                                                                        \
        *(u32 *)(tileData - destOffset) = fillValue32;                           \
    distanceLoop++;                                                             \
}

/**
 * FUNCTION: ScrollWindow
 *
 * PURPOSE: Scroll the pixel contents of a window vertically by a given distance.
 *
 * HOW IT WORKS:
 * This function shifts the pixel data within a window's tile buffer, creating a
 * scrolling effect. It operates directly on the tile data, moving pixel rows
 * by the specified distance. Rows that scroll off the edge are lost, and newly
 * exposed rows are filled with the fill color.
 *
 * Direction 0 = scroll content UP (text moves up, new space at bottom)
 *   - Processes from the beginning of the buffer forward
 * Direction 1 = scroll content DOWN (text moves down, new space at top)
 *   - Processes from the end of the buffer backward
 * Direction 2 = horizontal scroll (currently unimplemented)
 *
 * Each tile is 32 bytes (8 rows * 4 bytes per row). The macros process
 * 8 rows per tile (the 8 MOVE_TILES calls per iteration), moving 4 bytes
 * (one row of 8 pixels) at a time.
 *
 * GAME CONTEXT:
 * This is used for scrolling text in dialog boxes -- as the player presses
 * buttons, old text scrolls up and new text appears at the bottom.
 *
 * PARAMETERS:
 * @param windowId  -- Target window
 * @param direction -- 0 = up, 1 = down, 2 = horizontal (unimplemented)
 * @param distance  -- How many pixel rows to scroll
 * @param fillValue -- Pixel value for newly exposed area (usually 0 for transparent)
 */
void ScrollWindow(u8 windowId, u8 direction, u8 distance, u8 fillValue)
{
    struct WindowTemplate window = gWindows[windowId].window;
    u8 *tileData = gWindows[windowId].tileData;
    /*
     * Replicate the 8-bit fill value across all 4 bytes of a 32-bit word.
     * This allows filling 4 bytes at a time with the same pixel pattern.
     */
    u32 fillValue32 = (fillValue << 24) | (fillValue << 16) | (fillValue << 8) | fillValue;
    s32 size = window.height * window.width * 32;  /* Total bytes in the tile buffer */
    u32 width = window.width;
    s32 i;
    s32 srcOffset, destOffset;
    u32 distanceLoop;

    switch (direction)
    {
    case 0:
        /* Scroll UP: process forward, copying each row from a later position */
        for (i = 0; i < size; i += 32)
        {
            distanceLoop = distance;
            /* Process all 8 rows of this tile (each 4 bytes = one row) */
            MOVE_TILES_DOWN(0)
            MOVE_TILES_DOWN(4)
            MOVE_TILES_DOWN(8)
            MOVE_TILES_DOWN(12)
            MOVE_TILES_DOWN(16)
            MOVE_TILES_DOWN(20)
            MOVE_TILES_DOWN(24)
            MOVE_TILES_DOWN(28)
        }
        break;
    case 1:
        /* Scroll DOWN: process backward from the end of the buffer */
        tileData += size - 4;  /* Point to the last 4-byte word */
        for (i = 0; i < size; i += 32)
        {
            distanceLoop = distance;
            MOVE_TILES_UP(0)
            MOVE_TILES_UP(4)
            MOVE_TILES_UP(8)
            MOVE_TILES_UP(12)
            MOVE_TILES_UP(16)
            MOVE_TILES_UP(20)
            MOVE_TILES_UP(24)
            MOVE_TILES_UP(28)
        }
        break;
    case 2:
        /* Horizontal scrolling: not implemented */
        break;
    }
}

/**
 * FUNCTION: CallWindowFunction
 *
 * PURPOSE: Invoke a callback function, passing the window's template fields
 *          as individual arguments.
 *
 * HOW IT WORKS:
 * Unpacks the window's template into separate arguments and calls the provided
 * function pointer. This is a callback pattern used for custom window drawing
 * operations (like drawing decorative borders/frames).
 *
 * PARAMETERS:
 * @param windowId -- Window whose properties are passed to the callback
 * @param func     -- Function pointer receiving (bg, left, top, width, height, paletteNum)
 */
void CallWindowFunction(u8 windowId, WindowFunc func)
{
    struct WindowTemplate window = gWindows[windowId].window;
    func(window.bg, window.tilemapLeft, window.tilemapTop, window.width, window.height, window.paletteNum);
}

/**
 * FUNCTION: SetWindowAttribute
 *
 * PURPOSE: Modify a single attribute of an existing window.
 *
 * HOW IT WORKS:
 * Uses a switch statement to set the requested attribute. Only certain
 * attributes can be changed after creation (position, palette, baseBlock).
 * Attempting to change bg, width, height, or tileData returns TRUE (error)
 * because changing those would require reallocating tile data buffers.
 *
 * PARAMETERS:
 * @param windowId    -- Target window
 * @param attributeId -- Which attribute to change (WINDOW_TILEMAP_LEFT, etc.)
 * @param value       -- New value for the attribute
 *
 * RETURNS: FALSE on success, TRUE on failure (immutable attribute)
 */
bool8 SetWindowAttribute(u8 windowId, u8 attributeId, u32 value)
{
    switch (attributeId)
    {
    case WINDOW_TILEMAP_LEFT:
        gWindows[windowId].window.tilemapLeft = value;
        return FALSE;
    case WINDOW_TILEMAP_TOP:
        gWindows[windowId].window.tilemapTop = value;
        return FALSE;
    case WINDOW_PALETTE_NUM:
        gWindows[windowId].window.paletteNum = value;
        return FALSE;
    case WINDOW_BASE_BLOCK:
        gWindows[windowId].window.baseBlock = value;
        return FALSE;
    case WINDOW_TILE_DATA:
    case WINDOW_BG:
    case WINDOW_WIDTH:
    case WINDOW_HEIGHT:
    default:
        /* These attributes cannot be changed after creation */
        return TRUE;
    }
}

/**
 * FUNCTION: GetWindowAttribute
 *
 * PURPOSE: Read a single attribute from a window.
 *
 * HOW IT WORKS:
 * Simple accessor that returns the requested field from the window's template
 * or the tileData pointer (cast to u32). All window properties are readable.
 *
 * PARAMETERS:
 * @param windowId    -- Target window
 * @param attributeId -- Which attribute to read (WINDOW_BG, WINDOW_WIDTH, etc.)
 *
 * RETURNS: The attribute value, or 0 for unknown attribute IDs
 */
u32 GetWindowAttribute(u8 windowId, u8 attributeId)
{
    switch (attributeId)
    {
    case WINDOW_BG:
        return gWindows[windowId].window.bg;
    case WINDOW_TILEMAP_LEFT:
        return gWindows[windowId].window.tilemapLeft;
    case WINDOW_TILEMAP_TOP:
        return gWindows[windowId].window.tilemapTop;
    case WINDOW_WIDTH:
        return gWindows[windowId].window.width;
    case WINDOW_HEIGHT:
        return gWindows[windowId].window.height;
    case WINDOW_PALETTE_NUM:
        return gWindows[windowId].window.paletteNum;
    case WINDOW_BASE_BLOCK:
        return gWindows[windowId].window.baseBlock;
    case WINDOW_TILE_DATA:
        return (u32)(gWindows[windowId].tileData);
    default:
        return 0;
    }
}

/**
 * FUNCTION: GetNumActiveWindowsOnBg
 *
 * PURPOSE: Count how many active windows exist on a specific BG layer.
 *
 * HOW IT WORKS:
 * Scans all window slots and counts how many have their bg field set to bgId.
 * Used when removing windows to determine if a BG layer's tilemap buffer
 * can be freed (when no more windows use that layer).
 *
 * PARAMETERS:
 * @param bgId -- BG layer to check (0-3)
 *
 * RETURNS: Number of active windows on that BG layer
 */
static u8 GetNumActiveWindowsOnBg(u8 bgId)
{
    u8 windowsNum = 0;
    s32 i;
    for (i = 0; i < WINDOWS_MAX; i++)
    {
        if (gWindows[i].window.bg == bgId)
            windowsNum++;
    }
    return windowsNum;
}
