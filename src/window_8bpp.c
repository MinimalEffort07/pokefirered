/**
 * =8-BIT (256-COLOR) WINDOW SYSTEM=
 *
 * FILE OVERVIEW:
 * This file provides an alternative window system that uses 8-bit (256-color)
 * graphics instead of the standard 4-bit (16-color) mode. This is used for
 * screens that need more colors per tile, such as Pokemon summary screens
 * or other graphically rich displays.
 *
 * GBA CONTEXT:
 * The GBA supports two tile color modes:
 *   4bpp (4 bits per pixel): Each pixel uses 4 bits, selecting from a
 *     palette of 16 colors. Tiles are 32 bytes each (8x8 pixels * 4 bits).
 *   8bpp (8 bits per pixel): Each pixel uses 8 bits, selecting from a
 *     palette of 256 colors. Tiles are 64 bytes each (8x8 pixels * 8 bits).
 *
 * The 8bpp mode uses twice the VRAM per tile but provides much richer
 * color variety. The constant 0x40 (64) used throughout this file is
 * the size of a single 8bpp tile in bytes.
 *
 * This file parallels the standard window functions in window.c but
 * uses 8bpp tile sizes (0x40 bytes/tile vs 0x20 bytes/tile for 4bpp).
 */
#include "global.h"
#include "gflib.h"

EWRAM_DATA static struct Window* sWindowPtr = NULL;
EWRAM_DATA static u16 sWindowSize = 0;

static u8 GetNumActiveWindowsOnBg8Bit(u8 bgId);

static void nullsub_9(void)
{
}

/**
 * FUNCTION: AddWindow8Bit
 *
 * PURPOSE: Allocates and initializes an 8bpp window, including its tilemap
 * buffer and tile data buffer.
 *
 * HOW IT WORKS:
 * 1. Finds an unused window slot (bg == 0xFF means unused)
 * 2. If the BG layer doesn't have a tilemap buffer yet, allocates one
 * 3. Allocates tile data: width * height tiles * 0x40 bytes per 8bpp tile
 * 4. If any allocation fails, cleans up and returns 0xFF (failure)
 *
 * @param template — window dimensions and position
 * RETURNS: Window ID (0-31) on success, 0xFF on failure
 */
u16 AddWindow8Bit(const struct WindowTemplate *template)
{
    u16 windowId;
    u8 *memAddress;
    u8 bgLayer;

    for (windowId = 0; windowId < WINDOWS_MAX; windowId++)
    {
        if (gWindows[windowId].window.bg == 0xFF)
            break;
    }
    if (windowId == WINDOWS_MAX)
        return 0xFF;
    bgLayer = template->bg;
    if (gWindowBgTilemapBuffers[bgLayer] == NULL)
    {
        u16 attribute = GetBgAttribute(bgLayer, BG_ATTR_MAPSIZE);
        if (attribute != 0xFFFF)
        {
            s32 i;
            memAddress = Alloc(attribute);
            if (memAddress == NULL)
                return 0xFF;
            for (i = 0; i < attribute; i++) // if we're going to zero out the memory anyway, why not call AllocZeroed?
                memAddress[i] = 0;
            gWindowBgTilemapBuffers[bgLayer] = memAddress;
            SetBgTilemapBuffer(bgLayer, memAddress);
        }
    }
    memAddress = Alloc((u16)(0x40 * (template->width * template->height)));
    if (memAddress == NULL)
    {
        if (GetNumActiveWindowsOnBg8Bit(bgLayer) == 0 && gWindowBgTilemapBuffers[bgLayer] != nullsub_9)
        {
            Free(gWindowBgTilemapBuffers[bgLayer]);
            gWindowBgTilemapBuffers[bgLayer] = NULL;
        }
        return 0xFF;
    }
    else
    {
        gWindows[windowId].tileData = memAddress;
        gWindows[windowId].window = *template;
        return windowId;
    }
}

void FillWindowPixelBuffer8Bit(u8 windowId, u8 fillValue)
{
    s32 i;
    s32 size;

    size = (u16)(0x40 * (gWindows[windowId].window.width * gWindows[windowId].window.height));
    for (i = 0; i < size; i++)
        gWindows[windowId].tileData[i] = fillValue;
}

void FillWindowPixelRect8Bit(u8 windowId, u8 fillValue, u16 x, u16 y, u16 width, u16 height)
{
    struct Bitmap pixelRect;

    pixelRect.pixels = gWindows[windowId].tileData;
    pixelRect.width = 8 * gWindows[windowId].window.width;
    pixelRect.height = 8 * gWindows[windowId].window.height;

    FillBitmapRect8Bit(&pixelRect, x, y, width, height, fillValue);
}

void BlitBitmapRectToWindow4BitTo8Bit(u8 windowId, const u8 *pixels, u16 srcX, u16 srcY, u16 srcWidth, int srcHeight, u16 destX, u16 destY, u16 rectWidth, u16 rectHeight, u8 paletteNum)
{
    struct Bitmap sourceRect;
    struct Bitmap destRect;

    sourceRect.pixels = (u8 *)pixels;
    sourceRect.width = srcWidth;
    sourceRect.height = srcHeight;

    destRect.pixels = gWindows[windowId].tileData;
    destRect.width = 8 * gWindows[windowId].window.width;
    destRect.height = 8 * gWindows[windowId].window.height;

    BlitBitmapRect4BitTo8Bit(&sourceRect, &destRect, srcX, srcY, destX, destY, rectWidth, rectHeight, 0, paletteNum);
}

void CopyWindowToVram8Bit(u8 windowId, u8 mode)
{
    sWindowPtr = &gWindows[windowId];
    sWindowSize = 0x40 * (sWindowPtr->window.width * sWindowPtr->window.height);

    switch (mode)
    {
        case COPYWIN_MAP:
            CopyBgTilemapBufferToVram(sWindowPtr->window.bg);
            break;
        case COPYWIN_GFX:
            LoadBgTiles(sWindowPtr->window.bg, sWindowPtr->tileData, sWindowSize, sWindowPtr->window.baseBlock);
            break;
        case COPYWIN_FULL:
            LoadBgTiles(sWindowPtr->window.bg, sWindowPtr->tileData, sWindowSize, sWindowPtr->window.baseBlock);
            CopyBgTilemapBufferToVram(sWindowPtr->window.bg);
            break;
    }
}

static u8 GetNumActiveWindowsOnBg8Bit(u8 bgId)
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
