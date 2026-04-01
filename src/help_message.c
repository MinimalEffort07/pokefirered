/**
 * @file help_message.c
 * @brief Help Message Window — Bottom-Screen Help Text Bar
 *
 * FILE OVERVIEW:
 * This file manages the help message window that appears at the bottom of the
 * screen to display contextual help text. For example, when the player opens
 * the Start Menu, a help bar at the bottom explains what each menu option does.
 *
 * The window uses custom tile graphics (a special decorative frame) rather than
 * the standard text window border. The system handles creating, drawing the
 * background tiles, printing text, and cleaning up the window.
 *
 * GBA CONTEXT:
 * On the GBA, text windows are drawn as rectangular regions of background tiles.
 * Each window has a unique ID (slot) in the window system. EWRAM_DATA places the
 * variable in external work RAM (256 KB), which is slower than internal WRAM but
 * much larger. WINDOW_NONE (0xFF) is a sentinel value meaning "no window allocated."
 */
#include "global.h"
#include "malloc.h"
#include "menu.h"
#include "malloc.h"

/* The ID of the currently active help message window, or WINDOW_NONE if none exists */
static EWRAM_DATA u8 sHelpMessageWindowId = 0;

const u8 gHelpMessageWindow_Gfx[] = INCBIN_U8("graphics/help_system/msg_window.4bpp");

static const struct WindowTemplate sHelpMessageWindowTemplate = {
    .bg = 0,
    .tilemapLeft = 0,
    .tilemapTop = 15,
    .width = 30,
    .height = 5,
    .paletteNum = 15,
    .baseBlock = 0x08F
};

/**
 * FUNCTION: MapNamePopupWindowIdSetDummy
 *
 * PURPOSE: Resets the help message window ID to the "no window" sentinel value.
 *          Despite the misleading name (leftover from shared code with the map
 *          name popup system), this initializes the help message window state.
 */
void MapNamePopupWindowIdSetDummy(void)
{
    sHelpMessageWindowId = WINDOW_NONE;
}

/**
 * FUNCTION: CreateHelpMessageWindow
 *
 * PURPOSE: Creates the help message window if it doesn't already exist.
 *
 * HOW IT WORKS:
 * Checks if a window is already allocated (not WINDOW_NONE). If not, allocates
 * a new window using the predefined template and writes its tilemap entry so
 * it becomes visible on the background layer.
 *
 * RETURNS: The window ID of the help message window.
 */
u8 CreateHelpMessageWindow(void)
{
    if (sHelpMessageWindowId == WINDOW_NONE)
    {
        sHelpMessageWindowId = AddWindow(&sHelpMessageWindowTemplate);
        PutWindowTilemap(sHelpMessageWindowId); /* Make the window visible on the BG tilemap */
    }
    return sHelpMessageWindowId;
}

/**
 * FUNCTION: DestroyHelpMessageWindow
 *
 * PURPOSE: Removes the help message window and optionally copies the cleared
 *          state to VRAM so the screen updates.
 *
 * PARAMETERS:
 * @param a0 — VRAM copy mode: 0 = don't copy, nonzero = copy mode flag
 *             (controls whether tile data, tilemap, or both are transferred)
 */
void DestroyHelpMessageWindow(u8 a0)
{
    if (sHelpMessageWindowId != WINDOW_NONE)
    {
        FillWindowPixelBuffer(sHelpMessageWindowId, PIXEL_FILL(0)); /* Clear all pixels to transparent */
        ClearWindowTilemap(sHelpMessageWindowId); /* Remove from the BG tilemap */

        if (a0)
            CopyWindowToVram(sHelpMessageWindowId, a0); /* Push changes to VRAM */

        RemoveWindow(sHelpMessageWindowId); /* Free the window slot */
        sHelpMessageWindowId = WINDOW_NONE;
    }
}

/**
 * FUNCTION: DrawHelpMessageWindowTilesById
 *
 * PURPOSE: Draws the decorative background tiles for the help message window.
 *          This creates the visual frame/border that appears behind the help text.
 *
 * HOW IT WORKS:
 * The help message window graphic strip contains tiles for three row types:
 *   - Tile 0: top border row
 *   - Tile 5: middle/body row
 *   - Tile 14: bottom border row
 * This function fills the entire window pixel buffer by selecting the appropriate
 * tile for each row and repeating it across all columns.
 *
 * GBA CONTEXT:
 * Each 8x8 tile uses 32 bytes in 4bpp mode (4 bits per pixel, 8x8 = 64 pixels,
 * 64 * 4 / 8 = 32 bytes). The function builds the complete window image in a
 * temporary buffer, then copies it all at once to the window's pixel buffer.
 *
 * PARAMETERS:
 * @param windowId — The window to draw tiles into
 */
void DrawHelpMessageWindowTilesById(u8 windowId)
{
    const u8 *ptr = gHelpMessageWindow_Gfx;
    u8 *buffer;
    u8 i, j;
    u8 width, height;
    u8 tileId;

    width = (u8)GetWindowAttribute(windowId, WINDOW_WIDTH);
    height = (u8)GetWindowAttribute(windowId, WINDOW_HEIGHT);

    /* Allocate temporary buffer: 32 bytes per tile * width * height tiles */
    buffer = (u8 *)Alloc(32 * width * height);

    if (buffer != NULL)
    {
        for (i = 0; i < height; i++)
        {
            for (j = 0; j < width; j++)
            {
                /* Select the appropriate tile graphic based on row position */
                if (i == 0)                /* Top row: use border top tile */
                    tileId = 0;
                else if (i == height - 1)  /* Bottom row: use border bottom tile */
                    tileId = 14;
                else                       /* Middle rows: use body fill tile */
                    tileId = 5;
                /* Copy one tile (32 bytes) from the source graphic to the buffer.
                 * tileId * 32 selects which tile from the graphic strip.
                 * (i * width + j) * 32 computes the destination offset in the buffer. */
                CpuCopy32(
                    &ptr[tileId * 32],
                    &buffer[(i * width + j) * 32],
                    32
                );
            }
        }
        /* Transfer the assembled tile image into the window's pixel buffer */
        CopyToWindowPixelBuffer(windowId, buffer, width * height * 32, 0);
        Free(buffer);
    }
}

/**
 * FUNCTION: DrawHelpMessageWindowTiles
 *
 * PURPOSE: Convenience wrapper that draws tiles for the current help message window.
 */
static void DrawHelpMessageWindowTiles(void)
{
    DrawHelpMessageWindowTilesById(sHelpMessageWindowId);
}

/* Text color triple: [background, foreground, shadow] used for help message text */
static const u8 sHelpMessageTextColors[3] = {TEXT_COLOR_TRANSPARENT, TEXT_DYNAMIC_COLOR_1, TEXT_COLOR_DARK_GRAY};

/**
 * FUNCTION: PrintHelpMessageText
 *
 * PURPOSE: Prints text onto the help message window using the standard help text
 *          colors and positioning (2 pixels from left edge, 5 pixels from top).
 */
static void PrintHelpMessageText(const u8 *text)
{
    AddTextPrinterParameterized4(sHelpMessageWindowId, FONT_NORMAL, 2, 5, 1, 1, sHelpMessageTextColors, -1, text);
}

/**
 * FUNCTION: PrintTextOnHelpMessageWindow
 *
 * PURPOSE: High-level function that draws the window background tiles, prints
 *          text on top of them, and optionally pushes the result to VRAM.
 *
 * PARAMETERS:
 * @param text — The text string to display
 * @param mode — VRAM copy mode (0 = don't copy, nonzero = copy to VRAM)
 */
void PrintTextOnHelpMessageWindow(const u8 *text, u8 mode)
{
    DrawHelpMessageWindowTiles();
    PrintHelpMessageText(text);
    if (mode)
        CopyWindowToVram(sHelpMessageWindowId, mode);
}
