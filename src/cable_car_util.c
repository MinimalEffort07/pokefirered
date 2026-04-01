/**
 * @file cable_car_util.c
 * @brief Cable Car Tilemap Utilities (Ruby/Sapphire Leftover)
 *
 * FILE OVERVIEW:
 * These utility functions are from Ruby/Sapphire's cable car scene (the ride
 * between Route 112 and Mt. Chimney). They perform wrapped tilemap operations —
 * writing tiles to a tilemap buffer with coordinate wrapping at the edges.
 *
 * In FireRed/LeafGreen, there is no cable car scene, but these functions remain
 * in the codebase from the shared engine.
 *
 * GBA CONTEXT — TILEMAP WRAPPING:
 * GBA background tilemaps are typically 32x32 tiles. The wrapping (% 32) ensures
 * that writes past the edge of the tilemap wrap around to the other side, which
 * is useful for scrolling effects where the background appears to repeat infinitely.
 *
 * Each tilemap entry is 2 bytes (u16), so a 32-tile-wide row is 64 bytes.
 * The formula (y * 64 + x * 2) converts tile coordinates to byte offsets.
 */
#include "global.h"

/**
 * FUNCTION: CableCarUtil_FillWrapped
 *
 * PURPOSE: Fills a rectangular region of a tilemap with a single tile value,
 * wrapping coordinates at 32-tile boundaries.
 *
 * @param dest   — pointer to the tilemap buffer
 * @param value  — tile value to fill with (u16: palette + tile index)
 * @param left   — left column of the rectangle (in tiles)
 * @param top    — top row of the rectangle (in tiles)
 * @param width  — width of the rectangle (in tiles)
 * @param height — height of the rectangle (in tiles)
 */
static void CableCarUtil_FillWrapped(void *dest, u16 value, u8 left, u8 top, u8 width, u8 height)
{
    u8 i;
    u8 j;
    u8 x;
    u8 y;

    for (i = 0, y = top; i < height; i++)
    {
        for (x = left, j = 0; j < width; j++)
        {
            *(u16 *)&((u8 *)dest)[y * 64 + x * 2] = value;
            x = (x + 1) % 32;  /* Wrap horizontally at 32 tiles */
        }
        y = (y + 1) % 32;  /* Wrap vertically at 32 tiles */
    }
}

/**
 * FUNCTION: CableCarUtil_CopyWrapped
 *
 * PURPOSE: Copies a rectangular block of tiles from a source array into a tilemap
 * buffer, wrapping coordinates at 32-tile boundaries.
 *
 * @param dest   — pointer to the destination tilemap buffer
 * @param src    — pointer to the source tile data (sequential u16 values)
 * @param left   — left column of the destination rectangle
 * @param top    — top row of the destination rectangle
 * @param width  — width of the rectangle (in tiles)
 * @param height — height of the rectangle (in tiles)
 */
static void CableCarUtil_CopyWrapped(void *dest, const u16 *src, u8 left, u8 top, u8 width, u8 height)
{
    u8 i;
    u8 j;
    u8 x;
    u8 y;
    const u16 *_src;

    for (i = 0, _src = src, y = top; i < height; i++)
    {
        for (x = left, j = 0; j < width; j++)
        {
            *(u16 *)&((u8 *)dest)[y * 64 + x * 2] = *_src++;
            x = (x + 1) % 32;
        }
        y = (y + 1) % 32;
    }
}
