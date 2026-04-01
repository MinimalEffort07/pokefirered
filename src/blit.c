/*
 * blit.c - Tile-Based Bitmap Blitting (Block Image Transfer)
 *
 * ============================================================================
 * WHAT IS BLITTING?
 * ============================================================================
 *
 * "Blit" (Block Image Transfer) means copying a rectangular region of pixels
 * from one bitmap to another. This is how the game draws text characters,
 * menu elements, and other graphics onto background layers.
 *
 * ============================================================================
 * GBA TILE PIXEL LAYOUT (WHY THIS CODE IS COMPLEX)
 * ============================================================================
 *
 * On the GBA, graphics are NOT stored as simple rows of pixels (like a .bmp
 * file). They're stored as 8x8 TILES. Understanding the tile layout is
 * critical to understanding this code.
 *
 * In a normal bitmap, pixels are stored left-to-right, top-to-bottom:
 *   Pixel (0,0), (1,0), (2,0), ... (W-1,0), (0,1), (1,1), ...
 *
 * On the GBA, pixels within a tile are stored left-to-right, top-to-bottom
 * within the 8x8 tile, but TILES THEMSELVES are stored left-to-right:
 *
 *   [Tile 0,0] [Tile 1,0] [Tile 2,0] ...
 *   [Tile 0,1] [Tile 1,1] [Tile 2,1] ...
 *
 *   Where each tile is 8x8 pixels stored sequentially (row-major within tile).
 *
 * 4BPP FORMAT (4 Bits Per Pixel):
 *   Each pixel is 4 bits (one nibble), so TWO pixels fit in one byte.
 *   The LOW nibble (bits 0-3) is the LEFT pixel.
 *   The HIGH nibble (bits 4-7) is the RIGHT pixel.
 *   Each 8x8 tile = 8 rows * 4 bytes/row = 32 bytes.
 *
 * 8BPP FORMAT (8 Bits Per Pixel):
 *   Each pixel is 8 bits (one byte), one pixel per byte.
 *   Each 8x8 tile = 8 rows * 8 bytes/row = 64 bytes.
 *
 * THE PIXEL ADDRESS FORMULA:
 *   For a 4bpp tile-based surface at pixel (x, y):
 *
 *   address = base
 *           + ((x >> 1) & 3)            // Byte within the tile row (0-3)
 *           + ((x >> 3) << 5)           // Tile column offset (each tile = 32 bytes)
 *           + (((y >> 3) * tilesPerRow) << 5)  // Tile row offset
 *           + ((y & 7) << 2)            // Row within the tile (0-7, each row = 4 bytes)
 *
 *   The expressions like (u32)(y << 0x1d) >> 0x1B are optimized forms of
 *   ((y & 7) << 2) that the original compiler generated. They extract the
 *   lower 3 bits of y and shift left by 2 using rotate instructions.
 *
 * COLOR KEY:
 *   A color key is a "transparent" color index. When blitting, pixels
 *   matching the color key are skipped (not drawn), allowing the
 *   destination to show through. Color key 0xFF means "no transparency"
 *   (draw all pixels). Color key 0 is commonly used for transparency.
 *
 * ============================================================================
 */

#include "global.h"
#include "blit.h"

/**
 * FUNCTION: BlitBitmapRect4BitWithoutColorKey
 *
 * PURPOSE: Copy a rectangular region from one 4bpp tilemap surface to another,
 *          drawing ALL pixels (no transparency).
 *
 * HOW IT WORKS:
 * Simply calls BlitBitmapRect4Bit with colorKey=0xFF, which is the
 * "no color key" sentinel value.
 *
 * @param src    — Source bitmap structure (contains pixels, width, height)
 * @param dst    — Destination bitmap structure
 * @param srcX   — X coordinate to start reading from in source
 * @param srcY   — Y coordinate to start reading from in source
 * @param dstX   — X coordinate to start writing to in destination
 * @param dstY   — Y coordinate to start writing to in destination
 * @param width  — Width of the rectangle to copy (in pixels)
 * @param height — Height of the rectangle to copy (in pixels)
 */
void BlitBitmapRect4BitWithoutColorKey(const struct Bitmap *src, struct Bitmap *dst, u16 srcX, u16 srcY, u16 dstX, u16 dstY, u16 width, u16 height)
{
    BlitBitmapRect4Bit(src, dst, srcX, srcY, dstX, dstY, width, height, 0xFF);
}

/**
 * FUNCTION: BlitBitmapRect4Bit
 *
 * PURPOSE: Copy a rectangular region between two 4bpp tile-based surfaces,
 *          with optional color key transparency.
 *
 * HOW IT WORKS:
 * 1. Clips the copy region to the destination surface bounds.
 * 2. Calculates the "tiles per row" for both source and destination
 *    surfaces (multiplierSrcY, multiplierDstY). Width is rounded up to
 *    the next multiple of 8 (tile width), then divided by 8.
 * 3. Loops through every pixel in the rectangle:
 *    a. Calculates the source byte address using the tile pixel formula.
 *    b. Extracts the 4-bit pixel value from the correct nibble.
 *    c. If colorKey != 0xFF: skips the pixel if it matches the color key.
 *    d. Writes the pixel to the correct nibble of the destination byte,
 *       preserving the other nibble.
 *
 * GBA CONTEXT:
 * The complex address calculations navigate the tile-based pixel layout.
 * Each pixel must be located within its 8x8 tile, and then within the
 * correct row and column of that tile.
 *
 * @param colorKey — Pixel value to treat as transparent (0xFF = no transparency)
 */
void BlitBitmapRect4Bit(const struct Bitmap *src, struct Bitmap *dst, u16 srcX, u16 srcY, u16 dstX, u16 dstY, u16 width, u16 height, u8 colorKey)
{
    s32 xEnd;
    s32 yEnd;
    s32 multiplierSrcY;
    s32 multiplierDstY;
    s32 loopSrcY, loopDstY;
    s32 loopSrcX, loopDstX;
    const u8 *pixelsSrc;
    u8 *pixelsDst;
    s32 toOrr;
    s32 toAnd;
    s32 toShift;

    /* Clip: if the copy region extends past the destination, shrink it */
    if (dst->width - dstX < width)
        xEnd = (dst->width - dstX) + srcX;
    else
        xEnd = srcX + width;

    if (dst->height - dstY < height)
        yEnd = (dst->height - dstY) + srcY;
    else
        yEnd = height + srcY;

    /*
     * Calculate tiles per row for each surface.
     * (width + (width & 7)) >> 3 rounds up width to next multiple of 8, then divides by 8.
     * This gives the number of 8-pixel-wide tiles per row.
     */
    multiplierSrcY = (src->width + (src->width & 7)) >> 3;
    multiplierDstY = (dst->width + (dst->width & 7)) >> 3;

    if (colorKey == 0xFF)
    {
        /* NO TRANSPARENCY: Copy every pixel */
        for (loopSrcY = srcY, loopDstY = dstY; loopSrcY < yEnd; loopSrcY++, loopDstY++)
        {
            for (loopSrcX = srcX, loopDstX = dstX; loopSrcX < xEnd; loopSrcX++, loopDstX++)
            {
                /*
                 * Calculate source and destination byte addresses.
                 * See tile pixel layout explanation in the file header.
                 */
                pixelsSrc = src->pixels + ((loopSrcX >> 1) & 3) + ((loopSrcX >> 3) << 5) + (((loopSrcY >> 3) * multiplierSrcY) << 5) + ((u32)(loopSrcY << 0x1d) >> 0x1B);
                pixelsDst = dst->pixels + ((loopDstX >> 1) & 3) + ((loopDstX >> 3) << 5) + (((loopDstY >> 3) * multiplierDstY) << 5) + ((u32)(loopDstY << 0x1d) >> 0x1B);

                /*
                 * Extract the 4-bit pixel from the source.
                 * If x is even: pixel is in the LOW nibble (bits 0-3) -> shift right by 0
                 * If x is odd:  pixel is in the HIGH nibble (bits 4-7) -> shift right by 4
                 * (loopSrcX & 1) << 2 gives 0 or 4.
                 */
                toOrr = ((*pixelsSrc >> ((loopSrcX & 1) << 2)) & 0xF);

                /*
                 * Write the pixel to the destination nibble.
                 * toShift: 0 for even x (low nibble), 4 for odd x (high nibble)
                 * toAnd: mask to PRESERVE the OTHER nibble:
                 *   Even x: toAnd = 0xF0 (preserve high nibble, clear low)
                 *   Odd x:  toAnd = 0x0F (preserve low nibble, clear high)
                 */
                toShift = ((loopDstX & 1) << 2);
                toOrr <<= toShift;
                toAnd = 0xF0 >> (toShift);
                *pixelsDst = toOrr | (*pixelsDst & toAnd);
            }
        }
    }
    else
    {
        /* WITH TRANSPARENCY: Skip pixels matching the color key */
        for (loopSrcY = srcY, loopDstY = dstY; loopSrcY < yEnd; loopSrcY++, loopDstY++)
        {
            for (loopSrcX = srcX, loopDstX = dstX; loopSrcX < xEnd; loopSrcX++, loopDstX++)
            {
                pixelsSrc = src->pixels + ((loopSrcX >> 1) & 3) + ((loopSrcX >> 3) << 5) + (((loopSrcY >> 3) * multiplierSrcY) << 5) + ((u32)(loopSrcY << 0x1d) >> 0x1B);
                pixelsDst = dst->pixels + ((loopDstX >> 1) & 3) + ((loopDstX >> 3) << 5) + (((loopDstY >> 3) * multiplierDstY) << 5) + ((u32)(loopDstY << 0x1d) >> 0x1B);
                toOrr = ((*pixelsSrc >> ((loopSrcX & 1) << 2)) & 0xF);

                /* Only draw if the pixel doesn't match the transparent color */
                if (toOrr != colorKey)
                {
                    toShift = ((loopDstX & 1) << 2);
                    toOrr <<= toShift;
                    toAnd = 0xF0 >> (toShift);
                    *pixelsDst = toOrr | (*pixelsDst & toAnd);
                }
            }
        }
    }
}

/**
 * FUNCTION: FillBitmapRect4Bit
 *
 * PURPOSE: Fill a rectangular region of a 4bpp tile-based surface with a single color.
 *
 * HOW IT WORKS:
 * Iterates through each pixel in the rectangle, calculates its byte address
 * in the tile-based layout, and writes the fill value to the correct nibble.
 *
 * For even X: writes to the LOW nibble (bits 0-3), preserves HIGH nibble
 * For odd X:  writes to the HIGH nibble (bits 4-7), preserves LOW nibble
 *
 * @param surface   — The bitmap to fill
 * @param x, y      — Top-left corner of the rectangle
 * @param width     — Width of the rectangle in pixels
 * @param height    — Height of the rectangle in pixels
 * @param fillValue — 4-bit color index to fill with (0-15)
 */
void FillBitmapRect4Bit(struct Bitmap *surface, u16 x, u16 y, u16 width, u16 height, u8 fillValue)
{
    s32 xEnd;
    s32 yEnd;
    s32 multiplierY;
    s32 loopX, loopY;

    /* Clip to surface bounds */
    xEnd = x + width;
    if (xEnd > surface->width)
        xEnd = surface->width;

    yEnd = y + height;
    if (yEnd > surface->height)
        yEnd = surface->height;

    multiplierY = (surface->width + (surface->width & 7)) >> 3;

    for (loopY = y; loopY < yEnd; loopY++)
    {
        for (loopX = x; loopX < xEnd; loopX++)
        {
            u8 *pixels = surface->pixels + ((loopX >> 1) & 3) + ((loopX >> 3) << 5) + (((loopY >> 3) * multiplierY) << 5) + ((u32)(loopY << 0x1d) >> 0x1B);

            if ((loopX & 1) != 0)
            {
                /* Odd X: write to HIGH nibble, preserve LOW nibble */
                *pixels &= 0xF;            /* Clear high nibble */
                *pixels |= fillValue << 4; /* Set high nibble to fill value */
            }
            else
            {
                /* Even X: write to LOW nibble, preserve HIGH nibble */
                *pixels &= 0xF0;      /* Clear low nibble */
                *pixels |= fillValue;  /* Set low nibble to fill value */
            }
        }
    }
}

/**
 * FUNCTION: BlitBitmapRect4BitTo8Bit
 *
 * PURPOSE: Copy a rectangular region from a 4bpp surface to an 8bpp surface.
 *
 * HOW IT WORKS:
 * Reads 4-bit pixels from the source and writes them as 8-bit pixels to
 * the destination. The paletteOffset is added to each pixel value to shift
 * it into the correct palette range in the 256-color palette.
 *
 * The source uses 4bpp tile layout (32 bytes per tile).
 * The destination uses 8bpp tile layout (64 bytes per tile).
 *
 * This is used when rendering 4bpp text/graphics onto an 8bpp background
 * layer, allowing multiple 16-color sub-palettes to coexist in the
 * 256-color space.
 *
 * @param paletteOffset — Added to each pixel value (selects sub-palette in 8bpp mode)
 * @param colorKey      — Pixel value to treat as transparent (0xFF = no transparency)
 */
void BlitBitmapRect4BitTo8Bit(const struct Bitmap *src, struct Bitmap *dst, u16 srcX, u16 srcY, u16 dstX, u16 dstY, u16 width, u16 height, u8 colorKey, u8 paletteOffset)
{
    s32 palOffsetBits;
    s32 xEnd;
    s32 yEnd;
    s32 multiplierSrcY;
    s32 multiplierDstY;
    s32 loopSrcY, loopDstY;
    s32 loopSrcX, loopDstX;
    const u8 *pixelsSrc;
    u8 *pixelsDst;
    s32 colorKeyBits;

    /*
     * Convert paletteOffset to its position in the 8bpp color space.
     * The shift-left-then-right-unsigned extracts the low 4 bits and
     * shifts them to the correct position (multiply by 16).
     * This maps sub-palette N to colors N*16 through N*16+15.
     */
    palOffsetBits = (u32)(paletteOffset << 0x1C) >> 0x18;
    colorKeyBits = (u32)(colorKey << 0x1C) >> 0x18;

    /* Clip to destination bounds */
    if (dst->width - dstX < width)
        xEnd = (dst->width - dstX) + srcX;
    else
        xEnd = width + srcX;

    if (dst->height - dstY < height)
        yEnd = (srcY + dst->height) - dstY;
    else
        yEnd = srcY + height;

    multiplierSrcY = (src->width + (src->width & 7)) >> 3;
    multiplierDstY = (dst->width + (dst->width & 7)) >> 3;

    if (colorKey == 0xFF)
    {
        /* NO TRANSPARENCY */
        for (loopSrcY = srcY, loopDstY = dstY; loopSrcY < yEnd; loopSrcY++, loopDstY++)
        {
            pixelsSrc = src->pixels + ((srcX >> 1) & 3) + ((srcX >> 3) << 5) + (((loopSrcY >> 3) * multiplierSrcY) << 5) + ((u32)(loopSrcY << 0x1d) >> 0x1b);
            for (loopSrcX = srcX, loopDstX = dstX; loopSrcX < xEnd; loopSrcX++, loopDstX++)
            {
                /* 8bpp destination: 1 byte per pixel, 64 bytes per tile */
                pixelsDst = dst->pixels + (loopDstX & 7) + ((loopDstX >> 3) << 6) + (((loopDstY >> 3) * multiplierDstY) << 6) + ((u32)(loopDstY << 0x1d) >> 0x1a);
                if (loopSrcX & 1)
                {
                    /* Odd source X: read from HIGH nibble */
                    *pixelsDst = palOffsetBits + (*pixelsSrc >> 4);
                }
                else
                {
                    /* Even source X: recalculate address and read from LOW nibble */
                    pixelsSrc = src->pixels + ((loopSrcX >> 1) & 3) + ((loopSrcX >> 3) << 5) + (((loopSrcY >> 3) * multiplierSrcY) << 5) + ((u32)(loopSrcY << 0x1d) >> 0x1b);
                    *pixelsDst = palOffsetBits + (*pixelsSrc & 0xF);
                }
            }
        }
    }
    else
    {
        /* WITH TRANSPARENCY */
        for (loopSrcY = srcY, loopDstY = dstY; loopSrcY < yEnd; loopSrcY++, loopDstY++)
        {
            pixelsSrc = src->pixels + ((srcX >> 1) & 3) + ((srcX >> 3) << 5) + (((loopSrcY >> 3) * multiplierSrcY) << 5) + ((u32)(loopSrcY << 0x1d) >> 0x1b);
            for (loopSrcX = srcX, loopDstX = dstX; loopSrcX < xEnd; loopSrcX++, loopDstX++)
            {
                if (loopSrcX & 1)
                {
                    if ((*pixelsSrc & 0xF0) != colorKeyBits)
                    {
                        pixelsDst = dst->pixels + (loopDstX & 7) + ((loopDstX >> 3) << 6) + (((loopDstY >> 3) * multiplierDstY) << 6) + ((u32)(loopDstY << 0x1d) >> 0x1a);
                        *pixelsDst = palOffsetBits + (*pixelsSrc >> 4);
                    }
                }
                else
                {
                    pixelsSrc = src->pixels + ((loopSrcX >> 1) & 3) + ((loopSrcX >> 3) << 5) + (((loopSrcY >> 3) * multiplierSrcY) << 5) + ((u32)(loopSrcY << 0x1d) >> 0x1b);
                    if ((*pixelsSrc & 0xF) != colorKey)
                    {
                        pixelsDst = dst->pixels + (loopDstX & 7) + ((loopDstX >> 3) << 6) + (((loopDstY >> 3) * multiplierDstY) << 6) + ((u32)(loopDstY << 0x1d) >> 0x1a);
                        *pixelsDst = palOffsetBits + (*pixelsSrc & 0xF);
                    }
                }
            }
        }
    }
}

/**
 * FUNCTION: FillBitmapRect8Bit
 *
 * PURPOSE: Fill a rectangular region of an 8bpp tile-based surface with a single color.
 *
 * HOW IT WORKS:
 * Same logic as FillBitmapRect4Bit but for 8bpp tiles (64 bytes per tile,
 * 1 byte per pixel). Simpler because each pixel is a full byte - no nibble
 * manipulation needed.
 *
 * The address formula for 8bpp:
 *   base + (x & 7)                           // Byte within tile row
 *        + ((x >> 3) << 6)                   // Tile column (64 bytes per tile)
 *        + (((y >> 3) * tilesPerRow) << 6)   // Tile row
 *        + ((y & 7) << 3)                    // Row within tile (8 bytes per row)
 *
 * @param surface   — The bitmap to fill
 * @param x, y      — Top-left corner of the rectangle
 * @param width     — Width in pixels
 * @param height    — Height in pixels
 * @param fillValue — 8-bit color index to fill with (0-255)
 */
void FillBitmapRect8Bit(struct Bitmap *surface, u16 x, u16 y, u16 width, u16 height, u8 fillValue)
{
    s32 xEnd;
    s32 yEnd;
    s32 multiplierY;
    s32 loopX, loopY;

    xEnd = x + width;
    if (xEnd > surface->width)
        xEnd = surface->width;

    yEnd = y + height;
    if (yEnd > surface->height)
        yEnd = surface->height;

    multiplierY = (surface->width + (surface->width & 7)) >> 3;

    for (loopY = y; loopY < yEnd; loopY++)
    {
        for (loopX = x; loopX < xEnd; loopX++)
        {
            u8 *pixels = surface->pixels + (loopX & 7) + ((loopX >> 3) << 6) + (((loopY >> 3) * multiplierY) << 6) + ((u32)(loopY << 0x1d) >> 0x1a);
            *pixels = fillValue;
        }
    }
}
