/**
 * @file text_window_graphics.c
 * @brief Text Window Frame Graphics and Palette Data
 *
 * FILE OVERVIEW:
 * This file defines all the graphical assets used to draw text window frames
 * (the decorative borders around dialogue boxes, signposts, and menus). The
 * player can choose from multiple frame styles in the Options menu — each
 * style has its own tile graphics and color palette.
 *
 * The file contains:
 *   - Signpost window graphics (used when reading signs in the overworld)
 *   - 10 user-selectable frame styles (Type1 through Type10)
 *   - Standard text window graphics and Quest Log window graphics
 *   - Standard text window palettes (5 palette variations)
 *   - A lookup function to retrieve frame graphics by index
 *
 * GBA CONTEXT:
 * INCBIN_U16() is a macro that includes raw binary file data directly into
 * the compiled ROM as an array of 16-bit values. The ".4bpp" extension means
 * the graphics use 4 bits per pixel (16 colors per palette), which is the
 * standard format for GBA tile-based backgrounds. The ".gbapal" files contain
 * 16-color palettes (16 entries x 2 bytes each = 32 bytes per palette) in
 * the GBA's native BGR555 color format.
 */
#include "global.h"
#include "text_window_graphics.h"

/* Signpost-style window frame tiles (used when the player reads a sign) */
const u16 gSignpostWindow_Gfx[] = INCBIN_U16("graphics/text_window/signpost.4bpp");

static const u16 sUserFrame_Type1_Gfx[] = INCBIN_U16("graphics/text_window/type1.4bpp");
static const u16 sUserFrame_Type2_Gfx[] = INCBIN_U16("graphics/text_window/type2.4bpp");
static const u16 sUserFrame_Empty1[16] = {0};
static const u16 sUserFrame_Type3_Gfx[] = INCBIN_U16("graphics/text_window/type3.4bpp");
static const u16 sUserFrame_Type4_Gfx[] = INCBIN_U16("graphics/text_window/type4.4bpp");
static const u16 sUserFrame_Type5_Gfx[] = INCBIN_U16("graphics/text_window/type5.4bpp");
static const u16 sUserFrame_Type6_Gfx[] = INCBIN_U16("graphics/text_window/type6.4bpp");
static const u16 sUserFrame_Type7_Gfx[] = INCBIN_U16("graphics/text_window/type7.4bpp");
static const u16 sUserFrame_Type8_Gfx[] = INCBIN_U16("graphics/text_window/type8.4bpp");
static const u16 sUserFrame_Empty2[16] = {0};
static const u16 sUserFrame_Type9_Gfx[] = INCBIN_U16("graphics/text_window/type9.4bpp");
static const u16 sUserFrame_Type10_Gfx[] = INCBIN_U16("graphics/text_window/type10.4bpp");
static const u16 sUserFrame_Empty3[16] = {0};

static const u16 sUserFrame_Type1_Pal[] = INCBIN_U16("graphics/text_window/type1.gbapal");
static const u16 sUserFrame_Type2_Pal[] = INCBIN_U16("graphics/text_window/type2.gbapal");
static const u16 sUserFrame_Type3_Pal[] = INCBIN_U16("graphics/text_window/type3.gbapal");
static const u16 sUserFrame_Type4_Pal[] = INCBIN_U16("graphics/text_window/type4.gbapal");
static const u16 sUserFrame_Type5_Pal[] = INCBIN_U16("graphics/text_window/type5.gbapal");
static const u16 sUserFrame_Type6_Pal[] = INCBIN_U16("graphics/text_window/type6.gbapal");
static const u16 sUserFrame_Type7_Pal[] = INCBIN_U16("graphics/text_window/type7.gbapal");
static const u16 sUserFrame_Type8_Pal[] = INCBIN_U16("graphics/text_window/type8.gbapal");
static const u16 sUserFrame_Type9_Pal[] = INCBIN_U16("graphics/text_window/type9.gbapal");
static const u16 sUserFrame_Type10_Pal[] = INCBIN_U16("graphics/text_window/type10.gbapal");

const u16 gStdTextWindow_Gfx[] = INCBIN_U16("graphics/text_window/std.4bpp");
const u16 gQuestLogWindow_Gfx[] = INCBIN_U16("graphics/text_window/quest_log.4bpp");

const u16 gTextWindowPalettes[][16] = {
    INCBIN_U16("graphics/text_window/stdpal_0.gbapal"),
    INCBIN_U16("graphics/text_window/stdpal_1.gbapal"),
    INCBIN_U16("graphics/text_window/stdpal_2.gbapal"),
    INCBIN_U16("graphics/text_window/stdpal_3.gbapal"),
    INCBIN_U16("graphics/text_window/stdpal_4.gbapal")
};

const struct TextWindowGraphics gUserFrames[] = {
    {sUserFrame_Type1_Gfx,  sUserFrame_Type1_Pal},
    {sUserFrame_Type2_Gfx,  sUserFrame_Type2_Pal},
    {sUserFrame_Type3_Gfx,  sUserFrame_Type3_Pal},
    {sUserFrame_Type4_Gfx,  sUserFrame_Type4_Pal},
    {sUserFrame_Type5_Gfx,  sUserFrame_Type5_Pal},
    {sUserFrame_Type6_Gfx,  sUserFrame_Type6_Pal},
    {sUserFrame_Type7_Gfx,  sUserFrame_Type7_Pal},
    {sUserFrame_Type8_Gfx,  sUserFrame_Type8_Pal},
    {sUserFrame_Type9_Gfx,  sUserFrame_Type9_Pal},
    {sUserFrame_Type10_Gfx, sUserFrame_Type10_Pal},
};

/**
 * FUNCTION: GetUserWindowGraphics
 *
 * PURPOSE: Returns a pointer to the graphics/palette pair for a given text
 *          window frame style, selected by the player in the Options menu.
 *
 * HOW IT WORKS:
 * Looks up the requested frame index in the gUserFrames table. If the index
 * is out of bounds, it falls back to the first frame style (Type 1) to prevent
 * reading garbage data from beyond the array.
 *
 * NOTE: The original code has a known bug — it compares against 20 (the number
 * of frame styles in Ruby/Sapphire/Emerald) instead of the actual array count
 * of 10 in FireRed. The BUGFIX ifdef corrects this to use ARRAY_COUNT, which
 * dynamically computes the real array size at compile time.
 *
 * PARAMETERS:
 * @param idx — Frame style index (0-9 for FireRed's 10 styles)
 *
 * RETURNS: Pointer to a TextWindowGraphics struct containing both tile and
 *          palette data pointers for the requested frame style.
 */
const struct TextWindowGraphics *GetUserWindowGraphics(u8 idx)
{
#ifdef BUGFIX
    if (idx >= ARRAY_COUNT(gUserFrames))
#else
    if (idx >= 20) // BUG: Uses the RSE count (20) instead of FRLG's actual count (10)
#endif
        return &gUserFrames[0]; /* Out of bounds — fall back to default frame */
    else
        return &gUserFrames[idx];
}
