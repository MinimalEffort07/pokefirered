/**
 * text_printer.c - Text Rendering Pipeline and Glyph Composition
 *
 * ============================================================================
 * TEXT RENDERING SYSTEM OVERVIEW
 * ============================================================================
 *
 * This file implements the core text rendering pipeline that converts game
 * strings into visible pixels on screen. It manages "text printers" -- state
 * machines that render text character-by-character with configurable speed.
 *
 * HOW TEXT GETS FROM STRING TO SCREEN:
 *
 * 1. INITIATION: Game code calls AddTextPrinter with a string, window ID,
 *    position, font, and speed. A TextPrinter is created and stored.
 *
 * 2. RENDERING LOOP: Each frame, RunTextPrinters iterates all active printers.
 *    For each one, it calls RenderFont, which:
 *    a. Reads the next character from the string
 *    b. Looks up the glyph (character image) in the font data
 *    c. Decompresses the glyph using the half-row lookup table
 *    d. Copies the glyph pixels into the window's tile data buffer
 *    e. Advances the cursor position
 *
 * 3. DISPLAY: After rendering, the window's tile data is DMA'd to VRAM
 *    where the GBA's PPU displays it on the next frame.
 *
 * TEXT SPEED:
 *   speed 0: Instant -- all text rendered in one frame
 *   speed TEXT_SKIP_DRAW: Render all text but don't copy to VRAM
 *   speed 1-N: Render one character, then wait N-1 frames before the next
 *
 * FONT RENDERING DETAILS:
 *
 * Fonts are stored as 1bpp (1 bit per pixel) glyph data -- each pixel is
 * either "on" or "off." The actual colors (foreground, background, shadow)
 * are applied during decompression using a lookup table.
 *
 * The "half-row" optimization: Instead of processing pixels one at a time,
 * the renderer processes 4 pixels at once (a "half row" = half of an 8-pixel
 * tile row). The sFontHalfRowLookupTable precomputes the 4bpp output for
 * every possible combination of 4 pixels in 3 colors (3^4 = 81 entries).
 * This trades memory for speed -- important on the GBA's slow CPU.
 *
 * ============================================================================
 */

#include "global.h"
#include "window.h"
#include "text.h"

/*
 * sTempTextPrinter: Temporary printer used during initialization. Text printer
 * state is built up here, then copied to the appropriate slot in sTextPrinters.
 *
 * sTextPrinters: Array of all text printer slots. Each window can have one
 * active text printer. NUM_TEXT_PRINTERS typically equals the max number of
 * windows.
 */
static EWRAM_DATA struct TextPrinter sTempTextPrinter = {0};
static EWRAM_DATA struct TextPrinter sTextPrinters[NUM_TEXT_PRINTERS] = {0};

/*
 * sFontHalfRowLookupTable: Precomputed lookup table that maps 4-pixel
 * combinations to their 16-bit (4 pixels * 4bpp) output value.
 * 0x51 = 81 entries = 3^4 (3 colors, 4 pixels per half-row).
 *
 * sLastText*Color: Cache of the most recently used text colors, so the
 * lookup table can be regenerated if colors change.
 */
static u16 sFontHalfRowLookupTable[0x51];
static u16 sLastTextBgColor;
static u16 sLastTextFgColor;
static u16 sLastTextShadowColor;

/*
 * gFonts: Pointer to the font info array. Each font has a rendering function,
 * default colors, letter/line spacing, and glyph data pointers.
 *
 * gGlyphInfo: Temporary storage for the currently decompressed glyph.
 * Contains the pixel data and dimensions (width, height) of the character
 * being rendered. Fonts write their decompressed glyphs here, then
 * CopyGlyphToWindow copies them into the target window.
 */
COMMON_DATA const struct FontInfo *gFonts = NULL;
COMMON_DATA struct GlyphInfo gGlyphInfo = {0};

/**
 * sFontHalfRowOffsets: Maps 8-bit source glyph data (4 pixels, 2 bits each)
 * to indices into sFontHalfRowLookupTable.
 *
 * HOW IT WORKS:
 * Font glyph data uses 2 bits per pixel: 0 = background, 1 = foreground,
 * 2 = shadow. Four pixels = 8 bits = 256 possible combinations. But since
 * each pixel is only 0-2 (not 0-3), only 81 combinations are valid.
 * This table maps the 256 possible byte values to the 81 valid lookup
 * table indices, effectively ignoring the invalid combinations (value 3).
 */
static const u8 sFontHalfRowOffsets[] =
{
    0x00, 0x01, 0x02, 0x00, 0x03, 0x04, 0x05, 0x03, 0x06, 0x07, 0x08, 0x06, 0x00, 0x01, 0x02, 0x00,
    0x09, 0x0A, 0x0B, 0x09, 0x0C, 0x0D, 0x0E, 0x0C, 0x0F, 0x10, 0x11, 0x0F, 0x09, 0x0A, 0x0B, 0x09,
    0x12, 0x13, 0x14, 0x12, 0x15, 0x16, 0x17, 0x15, 0x18, 0x19, 0x1A, 0x18, 0x12, 0x13, 0x14, 0x12,
    0x00, 0x01, 0x02, 0x00, 0x03, 0x04, 0x05, 0x03, 0x06, 0x07, 0x08, 0x06, 0x00, 0x01, 0x02, 0x00,
    0x1B, 0x1C, 0x1D, 0x1B, 0x1E, 0x1F, 0x20, 0x1E, 0x21, 0x22, 0x23, 0x21, 0x1B, 0x1C, 0x1D, 0x1B,
    0x24, 0x25, 0x26, 0x24, 0x27, 0x28, 0x29, 0x27, 0x2A, 0x2B, 0x2C, 0x2A, 0x24, 0x25, 0x26, 0x24,
    0x2D, 0x2E, 0x2F, 0x2D, 0x30, 0x31, 0x32, 0x30, 0x33, 0x34, 0x35, 0x33, 0x2D, 0x2E, 0x2F, 0x2D,
    0x1B, 0x1C, 0x1D, 0x1B, 0x1E, 0x1F, 0x20, 0x1E, 0x21, 0x22, 0x23, 0x21, 0x1B, 0x1C, 0x1D, 0x1B,
    0x36, 0x37, 0x38, 0x36, 0x39, 0x3A, 0x3B, 0x39, 0x3C, 0x3D, 0x3E, 0x3C, 0x36, 0x37, 0x38, 0x36,
    0x3F, 0x40, 0x41, 0x3F, 0x42, 0x43, 0x44, 0x42, 0x45, 0x46, 0x47, 0x45, 0x3F, 0x40, 0x41, 0x3F,
    0x48, 0x49, 0x4A, 0x48, 0x4B, 0x4C, 0x4D, 0x4B, 0x4E, 0x4F, 0x50, 0x4E, 0x48, 0x49, 0x4A, 0x48,
    0x36, 0x37, 0x38, 0x36, 0x39, 0x3A, 0x3B, 0x39, 0x3C, 0x3D, 0x3E, 0x3C, 0x36, 0x37, 0x38, 0x36,
    0x00, 0x01, 0x02, 0x00, 0x03, 0x04, 0x05, 0x03, 0x06, 0x07, 0x08, 0x06, 0x00, 0x01, 0x02, 0x00,
    0x09, 0x0A, 0x0B, 0x09, 0x0C, 0x0D, 0x0E, 0x0C, 0x0F, 0x10, 0x11, 0x0F, 0x09, 0x0A, 0x0B, 0x09,
    0x12, 0x13, 0x14, 0x12, 0x15, 0x16, 0x17, 0x15, 0x18, 0x19, 0x1A, 0x18, 0x12, 0x13, 0x14, 0x12,
    0x00, 0x01, 0x02, 0x00, 0x03, 0x04, 0x05, 0x03, 0x06, 0x07, 0x08, 0x06, 0x00, 0x01, 0x02, 0x00
};

/**
 * FUNCTION: SetFontsPointer
 *
 * PURPOSE: Set the global fonts pointer to the game's font info array.
 *
 * HOW IT WORKS:
 * The font data is stored as a constant array in ROM. This function sets
 * the global pointer so all text rendering functions can access it.
 * Must be called before any text can be rendered.
 *
 * PARAMETERS:
 * @param fonts -- Pointer to the FontInfo array (one entry per font)
 */
void SetFontsPointer(const struct FontInfo *fonts)
{
    gFonts = fonts;
}

/**
 * FUNCTION: DeactivateAllTextPrinters
 *
 * PURPOSE: Mark all text printer slots as inactive.
 *
 * HOW IT WORKS:
 * Sets the 'active' flag to 0 for every printer slot. This stops all
 * text rendering. Called during screen transitions to prevent stale
 * text printers from running after their windows are destroyed.
 */
void DeactivateAllTextPrinters (void)
{
    int printer;
    for (printer = 0; printer < NUM_TEXT_PRINTERS; ++printer)
        sTextPrinters[printer].active = 0;
}

/**
 * FUNCTION: AddTextPrinterParameterized
 *
 * PURPOSE: Convenience function to create a text printer with common parameters.
 *
 * HOW IT WORKS:
 * Builds a TextPrinterTemplate struct from the provided parameters, filling
 * in letter spacing, line spacing, and colors from the font's defaults.
 * Then delegates to AddTextPrinter for the actual setup.
 *
 * PARAMETERS:
 * @param windowId -- Window to render text into
 * @param fontId   -- Which font to use
 * @param str      -- The text string to render (Pokemon encoding)
 * @param x        -- X pixel position within the window
 * @param y        -- Y pixel position within the window
 * @param speed    -- Text speed (0=instant, 1+=chars per N frames)
 * @param callback -- Optional callback invoked after each character/event
 *
 * RETURNS: Return value from AddTextPrinter
 */
u16 AddTextPrinterParameterized(u8 windowId, u8 fontId, const u8 *str, u8 x, u8 y, u8 speed, void (*callback)(struct TextPrinterTemplate *, u16))
{
    struct TextPrinterTemplate printerTemplate;

    printerTemplate.currentChar = str;
    printerTemplate.windowId = windowId;
    printerTemplate.fontId = fontId;
    printerTemplate.x = x;            /* Starting X (also used for line wrapping) */
    printerTemplate.y = y;            /* Starting Y */
    printerTemplate.currentX = x;     /* Current X cursor position */
    printerTemplate.currentY = y;     /* Current Y cursor position */
    printerTemplate.letterSpacing = gFonts[fontId].letterSpacing;
    printerTemplate.lineSpacing = gFonts[fontId].lineSpacing;
    printerTemplate.unk = gFonts[fontId].unk;
    printerTemplate.fgColor = gFonts[fontId].fgColor;
    printerTemplate.bgColor = gFonts[fontId].bgColor;
    printerTemplate.shadowColor = gFonts[fontId].shadowColor;
    return AddTextPrinter(&printerTemplate, speed, callback);
}

/**
 * FUNCTION: AddTextPrinter
 *
 * PURPOSE: Create and initialize a text printer, optionally rendering all
 *          text immediately.
 *
 * HOW IT WORKS:
 * Sets up the text printer state machine with the given template and speed.
 *
 * If speed is non-zero (and not TEXT_SKIP_DRAW):
 *   - Stores the printer in sTextPrinters for per-frame rendering
 *   - Each frame, RunTextPrinters will render one character
 *   - textSpeed is decremented by 1 (speed 1 = render every frame,
 *     speed 2 = render every other frame, etc.)
 *
 * If speed is 0 (instant):
 *   - Renders all text immediately in a loop (up to 0x400 = 1024 chars)
 *   - Copies the result to VRAM right away
 *   - Marks the printer as inactive (no per-frame work needed)
 *
 * If speed is TEXT_SKIP_DRAW:
 *   - Renders all text but does NOT copy to VRAM
 *   - Used to pre-calculate text layout without displaying it
 *
 * PARAMETERS:
 * @param textSubPrinter -- Template with string, position, font, colors
 * @param speed          -- Text speed (0=instant, TEXT_SKIP_DRAW=render-only)
 * @param callback       -- Optional per-character callback
 *
 * RETURNS: TRUE if successful, FALSE if fonts not initialized
 */
bool16 AddTextPrinter(struct TextPrinterTemplate *textSubPrinter, u8 speed, void (*callback)(struct TextPrinterTemplate *, u16))
{
    int i;
    u16 j;

    if (!gFonts)
        return FALSE;

    /* Initialize the temporary printer */
    sTempTextPrinter.active = TRUE;
    sTempTextPrinter.state = RENDER_STATE_HANDLE_CHAR;
    sTempTextPrinter.textSpeed = speed;
    sTempTextPrinter.delayCounter = 0;
    sTempTextPrinter.scrollDistance = 0;

    /* Clear the sub-union fields (used for font-specific state) */
    for (i = 0; i < (int)ARRAY_COUNT(sTempTextPrinter.subUnion.fields); ++i)
        sTempTextPrinter.subUnion.fields[i] = 0;

    sTempTextPrinter.printerTemplate = *textSubPrinter;
    sTempTextPrinter.callback = callback;
    sTempTextPrinter.minLetterSpacing = 0;
    sTempTextPrinter.japanese = 0;

    /* Build the color lookup table for this text's colors */
    GenerateFontHalfRowLookupTable(textSubPrinter->fgColor, textSubPrinter->bgColor, textSubPrinter->shadowColor);
    if (speed != TEXT_SKIP_DRAW && speed != 0)
    {
        /*
         * Animated text: store printer for per-frame rendering.
         * Speed is decremented by 1 because the delay counter starts at 0
         * and the first character is rendered immediately.
         */
        --sTempTextPrinter.textSpeed;
        sTextPrinters[textSubPrinter->windowId] = sTempTextPrinter;
    }
    else
    {
        sTempTextPrinter.textSpeed = 0;

        /*
         * Instant text: render everything in one go.
         * The 0x400 (1024) limit prevents infinite loops if the string
         * is malformed (no EOS terminator).
         */
        // Render all text (up to limit) at once
        for (j = 0; j < 0x400; ++j)
        {
            if (RenderFont(&sTempTextPrinter) == RENDER_FINISH)
                break;
        }

        // All the text is rendered to the window but don't draw it yet.
        if (speed != TEXT_SKIP_DRAW)
          CopyWindowToVram(sTempTextPrinter.printerTemplate.windowId, COPYWIN_GFX);
        sTextPrinters[textSubPrinter->windowId].active = FALSE;
    }
    return TRUE;
}

/**
 * FUNCTION: RunTextPrinters
 *
 * PURPOSE: Main per-frame update for all active text printers.
 *
 * HOW IT WORKS:
 * Iterates through all text printer slots. For each active printer:
 *   1. Calls RenderFont to process the next character(s)
 *   2. Based on the return value:
 *      RENDER_PRINT: A character was rendered -- copy window to VRAM and
 *        invoke the callback. (Falls through to RENDER_UPDATE for callback.)
 *      RENDER_UPDATE: No new character rendered but state updated -- invoke
 *        the callback only (no VRAM copy needed).
 *      RENDER_FINISH: Text is complete -- deactivate the printer.
 *
 * This function is called once per frame from the main game loop.
 * The text speed of each printer determines how many frames pass between
 * rendering each character.
 */
void RunTextPrinters(void)
{
    int i;

    for (i = 0; i < NUM_TEXT_PRINTERS; ++i)
    {
        if (sTextPrinters[i].active)
        {
            u16 renderCmd = RenderFont(&sTextPrinters[i]);
            switch (renderCmd)
            {
            case RENDER_PRINT:
                /* New glyph rendered: copy pixel data to VRAM */
                CopyWindowToVram(sTextPrinters[i].printerTemplate.windowId, COPYWIN_GFX);
            case RENDER_UPDATE:
                /* State changed: notify callback (if any) */
                if (sTextPrinters[i].callback != NULL)
                    sTextPrinters[i].callback(&sTextPrinters[i].printerTemplate, renderCmd);
                break;
            case RENDER_FINISH:
                /* String is fully rendered: deactivate this printer */
                sTextPrinters[i].active = FALSE;
                break;
            }
        }
    }
}

/**
 * FUNCTION: IsTextPrinterActive
 *
 * PURPOSE: Check if a specific text printer is still rendering.
 *
 * GAME LOGIC:
 * Used to wait for text to finish before proceeding with game logic.
 * For example, dialog scripts wait for IsTextPrinterActive to return FALSE
 * before showing the next message or advancing the plot.
 *
 * PARAMETERS:
 * @param id -- Text printer slot (usually the window ID)
 *
 * RETURNS: TRUE if still rendering, FALSE if finished or inactive
 */
bool16 IsTextPrinterActive(u8 id)
{
    return sTextPrinters[id].active;
}

/**
 * FUNCTION: RenderFont
 *
 * PURPOSE: Render the next character(s) using the current font's render function.
 *
 * HOW IT WORKS:
 * Calls the font's fontFunction (a function pointer stored in the FontInfo
 * struct) to render the next character. Different fonts have different
 * rendering logic (fixed-width vs proportional, different glyph formats).
 *
 * If the font function returns 2 (RENDER_REPEAT), it means "I processed
 * a control code or intermediate state; call me again immediately for the
 * actual character." This loop handles that by re-calling until a final
 * result (RENDER_PRINT, RENDER_UPDATE, or RENDER_FINISH) is returned.
 *
 * PARAMETERS:
 * @param textPrinter -- The text printer to advance
 *
 * RETURNS: RENDER_PRINT, RENDER_UPDATE, or RENDER_FINISH
 */
u32 RenderFont(struct TextPrinter *textPrinter)
{
    u32 ret;
    while (TRUE)
    {
        ret = gFonts[textPrinter->printerTemplate.fontId].fontFunction(textPrinter);
        if (ret != 2)  /* 2 = RENDER_REPEAT: process another character immediately */
            return ret;
    }
}

/**
 * FUNCTION: GenerateFontHalfRowLookupTable
 *
 * PURPOSE: Build the lookup table that maps 2bpp glyph data to 4bpp colored pixels.
 *
 * HOW IT WORKS:
 * Font glyphs use 2 bits per pixel (3 states: bg=0, fg=1, shadow=2).
 * The GBA display uses 4 bits per pixel (16-color palette indices).
 * This function precomputes every possible combination of 4 pixels
 * (4 pixels * 2 bits = 8 bits, but only values 0-2 are valid per pixel,
 * so there are 3^4 = 81 valid combinations).
 *
 * For each combination of 4 pixels, it builds a 16-bit value where each
 * nybble is the actual palette color index for that pixel:
 *   Bits 0-3:   pixel 0 color (leftmost)
 *   Bits 4-7:   pixel 1 color
 *   Bits 8-11:  pixel 2 color
 *   Bits 12-15: pixel 3 color (rightmost)
 *
 * The four nested loops iterate over each pixel's possible values (0-2),
 * and the result is stored sequentially in sFontHalfRowLookupTable.
 *
 * PARAMETERS:
 * @param fgColor     -- Palette index for foreground (text) pixels
 * @param bgColor     -- Palette index for background pixels
 * @param shadowColor -- Palette index for shadow pixels
 */
void GenerateFontHalfRowLookupTable(u8 fgColor, u8 bgColor, u8 shadowColor)
{
    int lutIndex;
    int i, j, k, l;
    const u32 colors[] = {bgColor, fgColor, shadowColor};

    /* Cache colors for later retrieval */
    sLastTextBgColor = bgColor;
    sLastTextFgColor = fgColor;
    sLastTextShadowColor = shadowColor;

    lutIndex = 0;

    /*
     * Generate all 3^4 = 81 combinations of 4 pixels, each of which
     * can be background (0), foreground (1), or shadow (2).
     * The result is a 16-bit value with 4 nybbles of color indices.
     */
    for (i = 0; i < 3; i++)       /* pixel 0 (leftmost) */
        for (j = 0; j < 3; j++)   /* pixel 1 */
            for (k = 0; k < 3; k++)   /* pixel 2 */
                for (l = 0; l < 3; l++)   /* pixel 3 (rightmost) */
                    sFontHalfRowLookupTable[lutIndex++] = (colors[l] << 12) | (colors[k] << 8) | (colors[j] << 4) | colors[i];
}

/**
 * FUNCTION: SaveTextColors
 *
 * PURPOSE: Save the current text colors for later restoration.
 *
 * PARAMETERS:
 * @param fgColor     -- Output: current foreground color
 * @param bgColor     -- Output: current background color
 * @param shadowColor -- Output: current shadow color
 */
void SaveTextColors(u8 *fgColor, u8 *bgColor, u8 *shadowColor)
{
    *bgColor = sLastTextBgColor;
    *fgColor = sLastTextFgColor;
    *shadowColor = sLastTextShadowColor;
}

/**
 * FUNCTION: RestoreTextColors
 *
 * PURPOSE: Restore previously saved text colors and rebuild the lookup table.
 *
 * PARAMETERS:
 * @param fgColor     -- Foreground color to restore
 * @param bgColor     -- Background color to restore
 * @param shadowColor -- Shadow color to restore
 */
void RestoreTextColors(u8 *fgColor, u8 *bgColor, u8 *shadowColor)
{
    GenerateFontHalfRowLookupTable(*fgColor, *bgColor, *shadowColor);
}

/**
 * FUNCTION: DecompressGlyphTile
 *
 * PURPOSE: Convert one 8x8 tile of font glyph data from 2bpp to colored 4bpp.
 *
 * HOW IT WORKS:
 * Processes 16 half-rows (8 rows * 2 halves per row = 16 iterations).
 * For each half-row:
 *   1. Extract 8 bits of source glyph data (4 pixels at 2bpp)
 *   2. Look up the offset in sFontHalfRowOffsets to get the LUT index
 *   3. Read the precomputed 16-bit colored value from sFontHalfRowLookupTable
 *   4. Store the result in the destination
 *
 * The odd/even check (i << 31) alternates between reading the low byte and
 * high byte of each source u16. On odd iterations, it reads *src++ (low byte
 * and advances); on even iterations, it reads *src >> 8 (high byte).
 *
 * PARAMETERS:
 * @param src  -- Source glyph data (2bpp, 16 bytes for one 8x8 tile)
 * @param dest -- Destination buffer (4bpp, 32 bytes for one 8x8 tile as u16 array)
 */
void DecompressGlyphTile(const u16 *src, u16 *dest)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        int offsetIndex = (i << 31) ? (u8)*src++ : (*src >> 8);
        dest[i] = sFontHalfRowLookupTable[sFontHalfRowOffsets[offsetIndex]];
    }
}

/**
 * FUNCTION: GetLastTextColor
 *
 * PURPOSE: Retrieve one of the cached text color values.
 *
 * PARAMETERS:
 * @param colorType -- 0 = foreground, 1 = shadow, 2 = background
 *
 * RETURNS: The palette index for the requested color type
 */
u8 GetLastTextColor(u8 colorType)
{
    switch (colorType)
    {
        case 0:
            return sLastTextFgColor;
        case 2:
            return sLastTextBgColor;
        case 1:
            return sLastTextShadowColor;
        default:
            return 0;
    }
}

/**
 * GLYPH_COPY macro: Copy a rectangular region of glyph pixels into a
 * window's tile data buffer at the current cursor position.
 *
 * This is the lowest-level text rendering primitive. It reads pixels from
 * gGlyphInfo.pixels (the decompressed glyph) and writes them into the
 * window's tile data buffer at the correct position.
 *
 * THE TILE COORDINATE MATH:
 * GBA tile memory is NOT laid out as a simple 2D pixel grid. Instead,
 * pixels are organized in 8x8 tiles, and the tiles are arranged in rows.
 * Converting (x, y) pixel coordinates to a byte offset in tile memory:
 *
 *   Byte offset = (x/2 & 3)           -- byte within a tile row (4bpp: 2px per byte)
 *               + (x/8 * 32)          -- which tile column (32 bytes per tile)
 *               + (y/8 * sizeX * 32)  -- which tile row (sizeX tiles per row)
 *               + (y%8 * 4)           -- which pixel row within the tile
 *
 * The nybble write:
 * Since 4bpp packs 2 pixels per byte, we need to write to either the
 * low or high nybble based on whether x is even or odd:
 *   bits = (x & 1) * 4  -> 0 for even x (low nybble), 4 for odd x (high nybble)
 *   *dst = (pixel << bits) | (*dst & (0xF0 >> bits))
 * This preserves the other pixel in the byte while writing our pixel.
 *
 * PARAMETERS:
 *   widthOffset, heightOffset: offset into the glyph (for splitting large glyphs)
 *   width, height: size of the region to copy
 *   tilesDest: pointer to the window's tile data buffer
 *   left, top: destination pixel coordinates in the window
 *   sizeX: window width in tiles (for row stride calculation)
 */
#define GLYPH_COPY(widthOffset, heightOffset, width, height, tilesDest, left, top, sizeX)                                                    \
{                                                                                                                                            \
    int xAdd, xpos, yAdd, ypos, toOrr, bits;                                                                                                 \
    u8 * src, * dst;                                                                                                                         \
    u32 _8pixbuf;                                                                                                                            \
                                                                                                                                             \
    src = gGlyphInfo.pixels + (heightOffset / 8 * 0x40) + (widthOffset / 8 * 0x20);                                                          \
    for (yAdd = 0, ypos = top + heightOffset; yAdd < height; yAdd++, ypos++)                                                                 \
    {                                                                                                                                        \
        _8pixbuf = *(u32 *)src;                                                                                                              \
        for (xAdd = 0, xpos = left + widthOffset; xAdd < width; xAdd++, xpos++)                                                              \
        {                                                                                                                                    \
            dst = (u8 *)((tilesDest) + ((xpos >> 1) & 3) + ((xpos >> 3) << 5) + (((ypos >> 3) * (sizeX)) << 5) + ((u32)(ypos << 29) >> 27)); \
            toOrr = (_8pixbuf >> (xAdd * 4)) & 0xF;                                                                                          \
            if (toOrr != 0)                                                                                                                  \
            {                                                                                                                                \
                bits = (xpos & 1) * 4;                                                                                                       \
                *dst = (toOrr << bits) | (*dst & (0xF0 >> bits));                                                                            \
            }                                                                                                                                \
        }                                                                                                                                    \
        src += 4;                                                                                                                            \
    }                                                                                                                                        \
}

/**
 * FUNCTION: CopyGlyphToWindow
 *
 * PURPOSE: Copy the current glyph (in gGlyphInfo) to the text printer's window.
 *
 * HOW IT WORKS:
 * Glyphs can be up to 16x16 pixels (2x2 tiles). Since the GLYPH_COPY macro
 * works on tile-aligned chunks, large glyphs must be split into up to 4 pieces:
 *   - Top-left 8x8
 *   - Top-right 8x8 (if glyph is wider than 8px)
 *   - Bottom-left 8x8 (if glyph is taller than 8px)
 *   - Bottom-right 8x8 (if both wider and taller than 8px)
 *
 * The function first clips the glyph to the window boundaries, then determines
 * which of the 4 cases applies (using sizeType as a 2-bit flag: bit 0 = wide,
 * bit 1 = tall) and calls GLYPH_COPY for each needed piece.
 *
 * PARAMETERS:
 * @param textPrinter -- The active text printer (provides window ID and position)
 */
void CopyGlyphToWindow(struct TextPrinter *textPrinter)
{
    int glyphWidth, glyphHeight;
    u8 sizeType;

    /* Clip glyph width to window boundary */
    if (gWindows[textPrinter->printerTemplate.windowId].window.width * 8 - textPrinter->printerTemplate.currentX < gGlyphInfo.width)
        glyphWidth = gWindows[textPrinter->printerTemplate.windowId].window.width * 8 - textPrinter->printerTemplate.currentX;
    else
        glyphWidth = gGlyphInfo.width;
    /* Clip glyph height to window boundary */
    if (gWindows[textPrinter->printerTemplate.windowId].window.height * 8 - textPrinter->printerTemplate.currentY < gGlyphInfo.height)
        glyphHeight = gWindows[textPrinter->printerTemplate.windowId].window.height * 8 - textPrinter->printerTemplate.currentY;
    else
        glyphHeight = gGlyphInfo.height;

    /* Determine glyph size category (0=small, 1=wide, 2=tall, 3=both) */
    sizeType = 0;
    if (glyphWidth > 8)
        sizeType |= 1;   /* Wider than one tile */
    if (glyphHeight > 8)
        sizeType |= 2;   /* Taller than one tile */

    switch (sizeType)
    {
        case 0: // 8x8 or smaller: single tile copy
            GLYPH_COPY(0, 0, glyphWidth, glyphHeight, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            return;
        case 1: // wider than 8px: two horizontal tiles
            GLYPH_COPY(0, 0, 8, glyphHeight, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            GLYPH_COPY(8, 0, glyphWidth - 8, glyphHeight, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            return;
        case 2: // taller than 8px: two vertical tiles
            GLYPH_COPY(0, 0, glyphWidth, 8, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            GLYPH_COPY(0, 8, glyphWidth, glyphHeight - 8, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            return;
        case 3: // both wide and tall: four tile copies (2x2 grid)
            GLYPH_COPY(0, 0, 8, 8, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            GLYPH_COPY(8, 0, glyphWidth - 8, 8, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            GLYPH_COPY(0, 8, 8, glyphHeight - 8, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            GLYPH_COPY(8, 8, glyphWidth - 8, glyphHeight - 8, gWindows[textPrinter->printerTemplate.windowId].tileData, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY, ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8 + ((gWindows[textPrinter->printerTemplate.windowId].window.width * 8) & 7)) >> 3));
            return;
    }
}

/**
 * FUNCTION: CopyGlyphToWindow_Parameterized (Unused)
 *
 * PURPOSE: Same as CopyGlyphToWindow but with explicit parameters instead
 *          of reading from a TextPrinter struct.
 *
 * HOW IT WORKS:
 * Identical algorithm to CopyGlyphToWindow but takes tile data, position,
 * and window size as direct parameters. Useful for rendering glyphs outside
 * the normal text printer system.
 *
 * PARAMETERS:
 * @param tileData -- Destination tile data buffer
 * @param currentX -- X position to draw at
 * @param currentY -- Y position to draw at
 * @param width    -- Total window width in pixels
 * @param height   -- Total window height in pixels
 */
// Unused
static void CopyGlyphToWindow_Parameterized(void *tileData, u16 currentX, u16 currentY, u16 width, u16 height)
{
    int glyphWidth, glyphHeight;
    u8 sizeType;
    u16 sizeX;

    if (width - currentX < gGlyphInfo.width)
        glyphWidth = width - currentX;
    else
        glyphWidth = gGlyphInfo.width;
    if (height - currentY < gGlyphInfo.height)
        glyphHeight = height - currentY;
    else
        glyphHeight = gGlyphInfo.height;

    sizeType = 0;
    sizeX  = (width + (width & 7)) >> 3;  /* Round up to nearest tile width */
    if (glyphWidth > 8)
        sizeType |= 1;
    if (glyphHeight > 8)
        sizeType |= 2;

    switch (sizeType)
    {
        case 0:
            GLYPH_COPY(0, 0, glyphWidth, glyphHeight, tileData, currentX, currentY, sizeX);
            return;
        case 1:
            GLYPH_COPY(0, 0, 8, glyphHeight, tileData, currentX, currentY, sizeX);
            GLYPH_COPY(8, 0, glyphWidth - 8, glyphHeight, tileData, currentX, currentY, sizeX);
            return;
        case 2:
            GLYPH_COPY(0, 0, glyphWidth, 8, tileData, currentX, currentY, sizeX);
            GLYPH_COPY(0, 8, glyphWidth, glyphHeight - 8, tileData, currentX, currentY, sizeX);
            return;
        case 3:
            GLYPH_COPY(0, 0, 8, 8, tileData, currentX, currentY, sizeX);
            GLYPH_COPY(8, 0, glyphWidth - 8, 8, tileData, currentX, currentY, sizeX);
            GLYPH_COPY(0, 8, 8, glyphHeight - 8, tileData, currentX, currentY, sizeX);
            GLYPH_COPY(8, 8, glyphWidth - 8, glyphHeight - 8, tileData, currentX, currentY, sizeX);
            return;
    }
}

/**
 * FUNCTION: ClearTextSpan
 *
 * PURPOSE: Clear a horizontal span of text pixels (stub -- not implemented).
 *
 * HOW IT WORKS:
 * This function is empty. It appears to be a placeholder that was never
 * implemented, or whose implementation was removed. It would have been used
 * to clear a horizontal region of pixels in the window (e.g., for erasing
 * text when overwriting or for cursor blinking).
 *
 * PARAMETERS:
 * @param textPrinter -- The active text printer
 * @param width       -- Width in pixels to clear
 */
void ClearTextSpan(struct TextPrinter *textPrinter, u32 width)
{
}
