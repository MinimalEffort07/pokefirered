/*
 * =Pokemon FireRed Text Rendering Engine=
 *
 * This file implements the complete text rendering system for the GBA.
 * It is responsible for:
 *   - Decompressing font glyph bitmaps from ROM into pixel buffers
 *   - Rendering characters one-by-one onto GBA background tile windows
 *   - Handling special control codes embedded in text strings (color changes,
 *     pauses, sound effects, music triggers, scrolling, etc.)
 *   - Supporting multiple fonts (Small, Normal, Male, Female, Bold, Braille)
 *   - Supporting both Latin and Japanese character sets
 *   - Drawing animated down-arrows and text cursors as sprites
 *   - Calculating string widths for centering/alignment
 *
 * GBA CONTEXT:
 * The GBA has no built-in text rendering hardware. All text must be drawn
 * as pixel art into background tile data (VRAM). Each character glyph is
 * stored as compressed 4bpp (4 bits-per-pixel) tile data in ROM, which
 * gets decompressed into a temporary buffer (gGlyphInfo.pixels), then
 * blitted (copied pixel-by-pixel) into a window's tile buffer. The window
 * system (see window.c) manages which region of the screen each text box
 * occupies and handles copying the pixel data to VRAM for display.
 *
 * FONT ARCHITECTURE:
 * Pokemon FireRed uses a variable-width font system. Each glyph has its
 * own pixel width (stored in width tables like sFontSmallLatinGlyphWidths),
 * unlike fixed-width fonts where every character is the same size. This
 * means the renderer must track the current X position and advance it by
 * each glyph's individual width after drawing.
 *
 * The game supports multiple font styles, some gender-specific:
 *   - FONT_SMALL: Compact font used in menus and UI elements
 *   - FONT_NORMAL: Standard dialogue font
 *   - FONT_MALE / FONT_FEMALE: Slightly different styles for gendered text
 *   - FONT_BOLD: Used for special displays (Japanese only)
 *   - FONT_BRAILLE: For the braille puzzles in the game
 *
 * TEXT STRING FORMAT:
 * Text strings are byte arrays where most bytes are glyph IDs. Special
 * byte values act as control codes:
 *   - 0xFF (EOS): End of string
 *   - 0xFE (CHAR_NEWLINE): Move to next line
 *   - 0xFD (PLACEHOLDER_BEGIN): Insert a dynamic string variable
 *   - 0xFC (EXT_CTRL_CODE_BEGIN): Extended control code follows
 *   - 0xFB (CHAR_PROMPT_CLEAR): Wait for input, then clear the window
 *   - 0xFA (CHAR_PROMPT_SCROLL): Wait for input, then scroll up one line
 *   - 0xF9 (CHAR_KEYPAD_ICON): Draw a button icon (A, B, D-pad, etc.)
 *   - 0xF8 (CHAR_EXTRA_SYMBOL): Access extended character set
 */
#include "global.h"
#include "gflib.h"
#include "m4a.h"
#include "quest_log.h"
#include "graphics.h"
#include "dynamic_placeholder_text_util.h"
#include "constants/songs.h"

/* Sprite tag used to identify text cursor sprites in the sprite system.
 * Tags allow the engine to find and free specific sprite graphics/palettes. */
#define TAG_CURSOR 0x8000

/* Number of frames to wait between each animation step of the down-arrow
 * or text cursor bounce. At 60fps, 8 frames = ~133ms per step. */
#define CURSOR_DELAY 8

/* Byte offset into the down-arrow tile sheet where the dark-colored variant
 * begins. The game uses two arrow styles: a light one for bright backgrounds
 * and a dark one for dark backgrounds. 256 bytes = 8 tiles of 4bpp data. */
#define DARK_DOWN_ARROW_OFFSET 256

extern const struct OamData gOamData_AffineOff_ObjNormal_16x16;

/* Forward declarations for glyph decompression functions (one per font) */
static void DecompressGlyph_NormalCopy1(u16 glyphId, bool32 isJapanese);
static void DecompressGlyph_NormalCopy2(u16 glyphId, bool32 isJapanese);
static void DecompressGlyph_Male(u16 glyphId, bool32 isJapanese);
static void DecompressGlyph_Bold(u16 glyphId);

/* Forward declarations for glyph width query functions (one per font).
 * These return how many pixels wide a particular character is, which is
 * needed for calculating string widths and advancing the cursor position. */
static s32 GetGlyphWidth_Small(u16 glyphId, bool32 isJapanese);
static s32 GetGlyphWidth_NormalCopy1(u16 glyphId, bool32 isJapanese);
static s32 GetGlyphWidth_Normal(u16 glyphId, bool32 isJapanese);
static s32 GetGlyphWidth_NormalCopy2(u16 glyphId, bool32 isJapanese);
static s32 GetGlyphWidth_Male(u16 glyphId, bool32 isJapanese);
static s32 GetGlyphWidth_Female(u16 glyphId, bool32 isJapanese);

/* Callback for the animated text cursor sprite (bounces up and down) */
static void SpriteCB_TextCursor(struct Sprite *sprite);

/* Global text rendering flags that control behavior across the entire text
 * system, such as whether auto-scroll is enabled or if the player can
 * speed up text by holding A/B. */
COMMON_DATA TextFlags gTextFlags = {0};

/* Pre-rendered 4bpp tile graphics for the "press A to continue" down-arrow
 * indicator. These are included directly from binary files at compile time
 * using the INCBIN_U8 macro, which embeds raw file data into the ROM. */
static const u8 sDownArrowTiles[]    = INCBIN_U8("graphics/fonts/down_arrows.4bpp");
static const u8 sDoubleArrowTiles1[] = INCBIN_U8("graphics/fonts/down_arrow_3.4bpp");
static const u8 sDoubleArrowTiles2[] = INCBIN_U8("graphics/fonts/down_arrow_4.4bpp");

/* Y-coordinate offsets for the down-arrow bounce animation. The arrow cycles
 * through positions 0 -> 16 -> 32 -> 16 -> 0... creating a smooth bounce
 * effect within the source tile sheet (not screen coordinates). */
static const u8 sDownArrowYCoords[]           = { 0, 16, 32, 16 };

/* How many pixels to scroll the text window per frame, based on the player's
 * text speed setting from the Options menu. Faster = bigger scroll steps. */
static const u8 sWindowVerticalScrollSpeeds[] = {
    [OPTIONS_TEXT_SPEED_SLOW] = 1,
    [OPTIONS_TEXT_SPEED_MID] = 2,
    [OPTIONS_TEXT_SPEED_FAST] = 4,
};

/* Lookup table mapping font IDs to their width-query functions.
 * When the engine needs to know how wide a character is (for string width
 * calculation or cursor advancement), it looks up the font ID here to find
 * the correct function. Each font has its own width table because different
 * fonts have different character proportions. */
static const struct GlyphWidthFunc sGlyphWidthFuncs[] = {
    { FONT_SMALL,         GetGlyphWidth_Small },
    { FONT_NORMAL_COPY_1, GetGlyphWidth_NormalCopy1 },
    { FONT_NORMAL,        GetGlyphWidth_Normal },
    { FONT_NORMAL_COPY_2, GetGlyphWidth_NormalCopy2 },
    { FONT_MALE,          GetGlyphWidth_Male },
    { FONT_FEMALE,        GetGlyphWidth_Female },
    { FONT_BRAILLE,       GetGlyphWidth_Braille }
};

/* Sprite sheet definitions for the text cursor (the bouncing double-arrow
 * shown during dialogue). Two variants are available; the caller picks
 * which one with a sheet index (0 or 1). */
static const struct SpriteSheet sSpriteSheets_TextCursor[] =
{
    {sDoubleArrowTiles1, sizeof(sDoubleArrowTiles1), TAG_CURSOR},
    {sDoubleArrowTiles2, sizeof(sDoubleArrowTiles2), TAG_CURSOR},
    {NULL}
};

static const struct SpritePalette sSpritePalettes_TextCursor[] =
{
    {gStandardMenuPalette, TAG_CURSOR},
    {NULL}
};

static const struct SpriteTemplate sSpriteTemplate_TextCursor =
{
    .tileTag = TAG_CURSOR,
    .paletteTag = TAG_CURSOR,
    .oam = &gOamData_AffineOff_ObjNormal_16x16,
    .anims = gDummySpriteAnimTable,
    .images = NULL,
    .affineAnims = gDummySpriteAffineAnimTable,
    .callback = SpriteCB_TextCursor,
};

/* Keypad icon metadata: maps button IDs (A, B, L, R, Start, etc.) to their
 * tile positions within the keypad icon tileset, along with pixel dimensions.
 * When text strings contain CHAR_KEYPAD_ICON, the next byte specifies which
 * button icon to draw inline with the text (e.g., "Press {A_BUTTON}"). */
struct
{
    u16 tileOffset;  /* Tile index into gKeypadIconTiles (each tile = 32 bytes of 4bpp data) */
    u8 width;        /* Width of this icon in pixels */
    u8 height;       /* Height of this icon in pixels */
} static const sKeypadIcons[] =
{
    [CHAR_A_BUTTON]       = {  0x0,  8, 12 },
    [CHAR_B_BUTTON]       = {  0x1,  8, 12 },
    [CHAR_L_BUTTON]       = {  0x2, 16, 12 },
    [CHAR_R_BUTTON]       = {  0x4, 16, 12 },
    [CHAR_START_BUTTON]   = {  0x6, 24, 12 },
    [CHAR_SELECT_BUTTON]  = {  0x9, 24, 12 },
    [CHAR_DPAD_UP]        = {  0xC,  8, 12 },
    [CHAR_DPAD_DOWN]      = {  0xD,  8, 12 },
    [CHAR_DPAD_LEFT]      = {  0xE,  8, 12 },
    [CHAR_DPAD_RIGHT]     = {  0xF,  8, 12 },
    [CHAR_DPAD_UPDOWN]    = { 0x20,  8, 12 },
    [CHAR_DPAD_LEFTRIGHT] = { 0x21,  8, 12 },
    [CHAR_DPAD_NONE]      = { 0x22,  8, 12 },
};

const u8 gKeypadIconTiles[] = INCBIN_U8("graphics/fonts/keypad_icons.4bpp");

static const u16 sFontSmallLatinGlyphs[] = INCBIN_U16("graphics/fonts/latin_small.latfont");
static const u8 sFontSmallLatinGlyphWidths[] = 
{
     5,  5,  5,  5,  5,  5,  5,  5,  5,  4,  5,  4,  4,  5, 
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  4,  5,  4,  4,  5,  5,  5,  6,  5,  5,  5,  5,
     5,  5,  8,  7,  8,  5,  5,  5,  5,  5,  8,  8,  7,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  8,
     8,  8,  8,  8,  8,  8,  4,  7,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  8,  8,  8,  8,  5,
     5,  5,  5,  5,  5,  5,  5,  7,  7,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  8,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  4,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  8,  5,  8,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,  5,  5,  5,
     5,  4,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  8,  7,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  5
};
static const u16 sFontSmallJapaneseGlyphs[] = INCBIN_U16("graphics/fonts/japanese_small.fwjpnfont");

static const u16 sFontNormalCopy1LatinGlyphs[] = INCBIN_U16("graphics/fonts/latin_normal.latfont");
static const u8 sFontNormalCopy1LatinGlyphWidths[] =
{
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,
     6,  6,  9,  8,  8,  6,  6,  6,  6,  6, 10,  8,  5,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  8,
     8,  8,  8,  8,  8,  4,  6,  8,  5,  5,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6, 12, 12, 12, 12,  6,
     6,  6,  6,  6,  6,  6,  8,  8,  8,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  5,  6,  5,  6,  6,  6,  3,  3,  6,
     6,  8,  5,  9,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  4,  6,  5,
     5,  6,  5,  6,  6,  6,  5,  5,  5,  6,  6,  6,  6,  6,
     6,  8,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6, 12, 12, 12, 12,  8, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  6
};
static const u16 sFontTallJapaneseGlyphs[] = INCBIN_U16("graphics/fonts/japanese_tall.fwjpnfont");

static const u16 sFontNormalLatinGlyphs[] = INCBIN_U16("graphics/fonts/latin_normal.latfont");
static const u8 sFontNormalLatinGlyphWidths[] =
{
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,
     6,  6,  9,  8,  8,  6,  6,  6,  6,  6, 10,  8,  5,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  8,
     8,  8,  8,  8,  8,  4,  6,  8,  5,  5,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6, 12, 12, 12, 12,  6,
     6,  6,  6,  6,  6,  6,  8,  8,  8,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  5,  6,  5,  6,  6,  6,  3,  3,  6,
     6,  8,  5,  9,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  4,  6,  5,
     5,  6,  5,  6,  6,  6,  5,  5,  5,  6,  6,  6,  6,  6,
     6,  8,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6, 12, 12, 12, 12,  8, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  6
};
static const u16 sFontNormalJapaneseGlyphs[] = INCBIN_U16("graphics/fonts/japanese_normal.fwjpnfont");
static const u8 sFontNormalJapaneseGlyphWidths[] =
{
     0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10,  9,  9,  9,  9,  9,  9,  9,  9, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  9, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10,  9,  9,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10,  9,  8,  7,  8,  8,  8,  8,  8,
     8,  8,  8,  5,  9, 10, 10, 10,  8, 10, 10, 10, 10,  8,
     8,  8, 10, 10,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  2,  4,  6,
     3,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  5,  6,  6,  6,  6,  6,  6,  0,  0,  0,  0,  0,
     0,  0,  0,  0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  0
};

static const u16 sFontMaleLatinGlyphs[] = INCBIN_U16("graphics/fonts/latin_male.latfont");
static const u8 sFontMaleLatinGlyphWidths[] =
{
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,
     6,  6,  9,  8,  8,  6,  6,  6,  6,  6, 10,  8,  5,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  8,
     8,  8,  8,  8,  8,  4,  6,  8,  5,  5,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6, 12, 12, 12, 12,  6,
     6,  6,  6,  6,  6,  6,  8,  8,  8,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  5,  6,  5,  6,  6,  6,  3,  3,  6,
     6,  8,  5,  9,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  4,  6,  5,
     5,  6,  5,  6,  6,  6,  5,  5,  5,  6,  6,  6,  6,  6,
     6,  8,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6, 12, 12, 12, 12,  8, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  6
};
static const u16 sFontMaleJapaneseGlyphs[] = INCBIN_U16("graphics/fonts/japanese_male.fwjpnfont");
static const u8 sFontMaleJapaneseGlyphWidths[] = 
{
     0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10,  9,  9,  9,  9,  9,  9,  9, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  9, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10,  9,  9,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10,  9,  8,  7,  8,  8,  8,  8,  8,
     8,  8,  8,  5,  9, 10, 10, 10,  8, 10, 10, 10, 10,  8,
     8,  8, 10, 10,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  2,  4,  6,
     3,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  5,  6,  6,  6,  6,  6,  6,  0,  0,  0,  0,  0,
     0,  0,  0,  0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  0
};

static const u16 sFontFemaleLatinGlyphs[] = INCBIN_U16("graphics/fonts/latin_female.latfont");
static const u8 sFontFemaleLatinGlyphWidths[] =
{
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,
     6,  6,  9,  8,  8,  6,  6,  6,  6,  6, 10,  8,  5,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  8,
     8,  8,  8,  8,  8,  4,  6,  8,  5,  5,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6, 12, 12, 12, 12,  6,
     6,  6,  6,  6,  6,  6,  8,  8,  8,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  8,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  5,  6,  5,  6,  6,  6,  3,  3,  6,
     6,  8,  5,  9,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  4,  6,  5,
     5,  6,  5,  6,  6,  6,  5,  5,  5,  6,  6,  6,  6,  6,
     6,  8,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6, 12, 12, 12, 12,  8, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  8,  8,  8,  8,  6
};
static const u16 sFontFemaleJapaneseGlyphs[] = INCBIN_U16("graphics/fonts/japanese_female.fwjpnfont");
static const u8 sFontFemaleJapaneseGlyphWidths[] =
{
     0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10,  9,  9,  9,  9,  9,  9,  9,  9, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  9, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10,  9,  9,  9,  9,  9,  9,  9,  8, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10,  9,  8,  7,  8,  8,  8,  8,  8,
     8,  8,  8,  5,  9, 10, 10, 10,  8, 10, 10, 10, 10,  8,
     8,  8, 10, 10,  8,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  6,  6,  5,  6,  6,  2,  4,  6,
     3,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
     6,  6,  5,  6,  6,  6,  6,  6,  6,  0,  0,  0,  0,  0,
     0,  0,  0,  0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  0
};

static const u16 sFontBoldJapaneseGlyphs[] = INCBIN_U16("graphics/fonts/japanese_bold.fwjpnfont");

/**
 * FUNCTION: FontFunc_Small
 *
 * PURPOSE: Entry point for rendering text using the Small font.
 *
 * HOW IT WORKS:
 * Each font has a "FontFunc" that acts as its rendering entry point. On the
 * first call, it sets the glyphId to identify which font to use for glyph
 * decompression. Then it delegates to the shared RenderText() function which
 * handles the actual character-by-character rendering state machine.
 *
 * The hasGlyphIdBeenSet flag prevents re-initialization on subsequent calls,
 * since this function is called repeatedly (once per frame) as text prints
 * character by character.
 *
 * PARAMETERS:
 * @param textPrinter - The active text printer state (position, font, colors, etc.)
 *
 * RETURNS: A RENDER_* status code indicating whether rendering is still in progress.
 */
u16 FontFunc_Small(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (subStruct->hasGlyphIdBeenSet == 0)
    {
        textPrinter->subUnion.sub.glyphId = FONT_SMALL;
        subStruct->hasGlyphIdBeenSet = 1;
    }
    return RenderText(textPrinter);
}

/* FontFunc_NormalCopy1 through FontFunc_Female follow the same pattern as
 * FontFunc_Small above: set the font ID on first call, then delegate to
 * RenderText(). The "Copy1"/"Copy2" variants exist because the game uses
 * slightly different glyph sets for different contexts (e.g., dialogue vs.
 * menus), even though they share the same base Normal font in Latin mode. */
u16 FontFunc_NormalCopy1(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (subStruct->hasGlyphIdBeenSet == 0)
    {
        textPrinter->subUnion.sub.glyphId = FONT_NORMAL_COPY_1;
        subStruct->hasGlyphIdBeenSet = 1;
    }
    return RenderText(textPrinter);
}

u16 FontFunc_Normal(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (subStruct->hasGlyphIdBeenSet == 0)
    {
        textPrinter->subUnion.sub.glyphId = FONT_NORMAL;
        subStruct->hasGlyphIdBeenSet = 1;
    }
    return RenderText(textPrinter);
}

u16 FontFunc_NormalCopy2(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (subStruct->hasGlyphIdBeenSet == 0)
    {
        textPrinter->subUnion.sub.glyphId = FONT_NORMAL_COPY_2;
        subStruct->hasGlyphIdBeenSet = 1;
    }
    return RenderText(textPrinter);
}

u16 FontFunc_Male(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (subStruct->hasGlyphIdBeenSet == 0)
    {
        textPrinter->subUnion.sub.glyphId = FONT_MALE;
        subStruct->hasGlyphIdBeenSet = 1;
    }
    return RenderText(textPrinter);
}

u16 FontFunc_Female(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (subStruct->hasGlyphIdBeenSet == 0)
    {
        textPrinter->subUnion.sub.glyphId = FONT_FEMALE;
        subStruct->hasGlyphIdBeenSet = 1;
    }
    return RenderText(textPrinter);
}

/**
 * FUNCTION: TextPrinterInitDownArrowCounters
 *
 * PURPOSE: Initialize the animation counters for the "press A to continue"
 *          down-arrow indicator or the auto-scroll delay timer.
 *
 * HOW IT WORKS:
 * If auto-scroll mode is active (used during quest log playback), the delay
 * counter is set to 0 so it will start counting up toward the auto-advance
 * threshold. Otherwise, the bounce animation index and frame delay for the
 * down-arrow are both reset to 0 to start the animation fresh.
 */
void TextPrinterInitDownArrowCounters(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;

    if (gTextFlags.autoScroll == 1)
        subStruct->autoScrollDelay = 0;
    else
    {
        subStruct->downArrowYPosIdx = 0;
        subStruct->downArrowDelay = 0;
    }
}

/**
 * FUNCTION: TextPrinterDrawDownArrow
 *
 * PURPOSE: Animate and draw the bouncing down-arrow that tells the player
 *          to press A/B to advance the dialogue.
 *
 * HOW IT WORKS:
 * Uses a frame delay counter to control animation speed. When the counter
 * reaches 0, it clears the previous arrow frame, selects the appropriate
 * arrow style (light or dark), then blits the current animation frame from
 * the arrow tile sheet to the window at the text printer's current position.
 * The sDownArrowYCoords array cycles the source Y offset to create the
 * bounce: 0->16->32->16->0... The arrow is 10x12 pixels in size.
 *
 * GBA CONTEXT:
 * BlitBitmapRectToWindow copies a rectangular region from a source bitmap
 * into a window's pixel buffer. The window system then handles copying
 * this to VRAM during the next CopyWindowToVram call. The 0x80 and 0x10
 * values are the source bitmap's full width and height in pixels.
 */
void TextPrinterDrawDownArrow(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;
    const u8 *arrowTiles;

    if (gTextFlags.autoScroll == 0)
    {
        if (subStruct->downArrowDelay != 0)
        {
            subStruct->downArrowDelay = ((*(u32 *)&textPrinter->subUnion.sub) << 19 >> 27) - 1;    // convoluted way of getting field_1, necessary to match
        }
        else
        {
            FillWindowPixelRect(
                textPrinter->printerTemplate.windowId,
                textPrinter->printerTemplate.bgColor << 4 | textPrinter->printerTemplate.bgColor,
                textPrinter->printerTemplate.currentX,
                textPrinter->printerTemplate.currentY,
                10,
                12);

            switch (gTextFlags.useAlternateDownArrow)
            {
                case 0:
                default:
                    arrowTiles = sDownArrowTiles;
                    break;
                case 1:
                    arrowTiles = &sDownArrowTiles[DARK_DOWN_ARROW_OFFSET];
                    break;
            }

            BlitBitmapRectToWindow(
                textPrinter->printerTemplate.windowId,
                arrowTiles,
                sDownArrowYCoords[subStruct->downArrowYPosIdx],
                0,
                0x80,
                0x10,
                textPrinter->printerTemplate.currentX,
                textPrinter->printerTemplate.currentY,
                10,
                12);
            CopyWindowToVram(textPrinter->printerTemplate.windowId, 0x2);

            subStruct->downArrowDelay = CURSOR_DELAY;
            subStruct->downArrowYPosIdx = (*(u32 *)subStruct << 17 >> 30) + 1;
        }
    }
}

/**
 * FUNCTION: TextPrinterClearDownArrow
 *
 * PURPOSE: Erase the down-arrow indicator from the text window. Called when
 *          the player presses A/B and the text starts scrolling.
 */
void TextPrinterClearDownArrow(struct TextPrinter *textPrinter)
{
    FillWindowPixelRect(
        textPrinter->printerTemplate.windowId,
        textPrinter->printerTemplate.bgColor << 4 | textPrinter->printerTemplate.bgColor,
        textPrinter->printerTemplate.currentX,
        textPrinter->printerTemplate.currentY,
        10,
        12);
    CopyWindowToVram(textPrinter->printerTemplate.windowId, 0x2);
}

/**
 * FUNCTION: TextPrinterWaitAutoMode
 *
 * PURPOSE: Wait for auto-scroll delay to expire (used during quest log playback
 *          where the player doesn't manually press buttons to advance text).
 *
 * HOW IT WORKS:
 * Increments a delay counter each frame. Returns TRUE when the counter reaches
 * the threshold (50 frames during quest log playback, 120 frames otherwise).
 * At 60fps, that's roughly 0.83 or 2 seconds of pause before auto-advancing.
 */
bool8 TextPrinterWaitAutoMode(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;
    u8 delay = (gQuestLogState == QL_STATE_PLAYBACK) ? 50 : 120;

    if (subStruct->autoScrollDelay == delay)
    {
        return TRUE;
    }
    else
    {
        subStruct->autoScrollDelay++;
        return FALSE;
    }
}

/**
 * FUNCTION: TextPrinterWaitWithDownArrow
 *
 * PURPOSE: Wait for the player to press A or B while showing the animated
 *          down-arrow. Used at CHAR_PROMPT_SCROLL and CHAR_PROMPT_CLEAR points.
 *
 * RETURNS: TRUE when the player presses A or B (or auto-scroll finishes).
 */
bool16 TextPrinterWaitWithDownArrow(struct TextPrinter *textPrinter)
{
    bool8 result = FALSE;
    if (gTextFlags.autoScroll != 0)
    {
        result = TextPrinterWaitAutoMode(textPrinter);
    }
    else
    {
        TextPrinterDrawDownArrow(textPrinter);
        if (JOY_NEW(A_BUTTON | B_BUTTON))
        {
            result = TRUE;
            PlaySE(SE_SELECT);
        }
    }
    return result;
}

/**
 * FUNCTION: TextPrinterWait
 *
 * PURPOSE: Wait for the player to press A or B without showing a down-arrow.
 *          Used for EXT_CTRL_CODE_PAUSE_UNTIL_PRESS.
 */
bool16 TextPrinterWait(struct TextPrinter *textPrinter)
{
    bool16 result = FALSE;
    if (gTextFlags.autoScroll != 0)
    {
        result = TextPrinterWaitAutoMode(textPrinter);
    }
    else
    {
        if (JOY_NEW(A_BUTTON | B_BUTTON))
        {
            result = TRUE;
            PlaySE(SE_SELECT);
        }
    }
    return result;
}

/**
 * FUNCTION: DrawDownArrow
 *
 * PURPOSE: Standalone version of the down-arrow drawing that can be used
 *          outside of the TextPrinter system (e.g., in scrollable lists).
 *
 * PARAMETERS:
 * @param windowId    - Which window to draw in
 * @param x, y        - Pixel position within the window
 * @param bgColor     - Background color index to clear with before drawing
 * @param drawArrow   - If 0, draw the arrow; if nonzero, skip drawing (just clear)
 * @param counter     - Pointer to a frame delay counter (caller-managed)
 * @param yCoordIndex - Pointer to the animation frame index (caller-managed)
 */
void DrawDownArrow(u8 windowId, u16 x, u16 y, u8 bgColor, bool8 drawArrow, u8 *counter, u8 *yCoordIndex)
{
    const u8 *arrowTiles;

    if (*counter != 0)
    {
        --*counter;
    }
    else
    {
        FillWindowPixelRect(windowId, (bgColor << 4) | bgColor, x, y, 10, 12);
        if (drawArrow == 0)
        {
            switch (gTextFlags.useAlternateDownArrow)
            {
                case 0:
                default:
                    arrowTiles = sDownArrowTiles;
                    break;
                case 1:
                    arrowTiles = &sDownArrowTiles[DARK_DOWN_ARROW_OFFSET];
                    break;
            }

            BlitBitmapRectToWindow(
                windowId,
                arrowTiles,
                sDownArrowYCoords[*yCoordIndex & 3],
                0,
                0x80,
                0x10,
                x,
                y,
                10,
                12);
            CopyWindowToVram(windowId, 0x2);
            *counter = CURSOR_DELAY;
            ++*yCoordIndex;
        }
    }
}

/**
 * FUNCTION: RenderText
 *
 * PURPOSE: The core text rendering state machine. This is the most important
 *          function in the text system -- it processes one character at a time
 *          per call and handles all control codes, scrolling, and waiting.
 *
 * HOW IT WORKS:
 * This function is called once per frame by the text printer system. It operates
 * as a state machine with these states:
 *   - RENDER_STATE_HANDLE_CHAR: Read the next character from the string and process it.
 *     If it's a normal character, decompress its glyph and blit it to the window.
 *     If it's a control code, execute the appropriate action (change color, pause, etc.)
 *   - RENDER_STATE_WAIT: Pause until the player presses A/B.
 *   - RENDER_STATE_CLEAR: Show down-arrow, wait for input, then clear the whole window.
 *   - RENDER_STATE_SCROLL_START: Show down-arrow, wait for input, then begin scrolling.
 *   - RENDER_STATE_SCROLL: Smoothly scroll the window contents upward pixel by pixel.
 *   - RENDER_STATE_WAIT_SE: Wait for a sound effect to finish playing.
 *   - RENDER_STATE_PAUSE: Count down a delay timer before resuming.
 *
 * RETURNS:
 *   RENDER_UPDATE  - Still working, call again next frame
 *   RENDER_REPEAT  - Processed a control code, call again immediately this frame
 *   RENDER_PRINT   - Drew a character, needs VRAM update
 *   RENDER_FINISH  - Reached end of string, rendering complete
 *
 * GBA CONTEXT:
 * Text rendering speed is controlled by the textSpeed and delayCounter fields.
 * The player can hold A/B to speed up text if canABSpeedUpPrint is set. During
 * auto-scroll mode (quest log playback), the delay is always 1 frame per char.
 */
u16 RenderText(struct TextPrinter *textPrinter)
{
    struct TextPrinterSubStruct *subStruct = &textPrinter->subUnion.sub;
    u16 currChar;
    s32 width;
    s32 widthHelper;

    switch (textPrinter->state)
    {
    case RENDER_STATE_HANDLE_CHAR:
        if (JOY_HELD(A_BUTTON | B_BUTTON) && subStruct->hasPrintBeenSpedUp)
            textPrinter->delayCounter = 0;

        if (textPrinter->delayCounter && textPrinter->textSpeed)
        {
            textPrinter->delayCounter--;
            if (gTextFlags.canABSpeedUpPrint && JOY_NEW(A_BUTTON | B_BUTTON))
            {
                subStruct->hasPrintBeenSpedUp = TRUE;
                textPrinter->delayCounter = 0;
            }
            return RENDER_UPDATE;
        }

        if (gTextFlags.autoScroll)
            textPrinter->delayCounter = 1;
        else
            textPrinter->delayCounter = textPrinter->textSpeed;

        currChar = *textPrinter->printerTemplate.currentChar;
        textPrinter->printerTemplate.currentChar++;

        switch (currChar)
        {
        case CHAR_NEWLINE:
            textPrinter->printerTemplate.currentX = textPrinter->printerTemplate.x;
            textPrinter->printerTemplate.currentY += gFonts[textPrinter->printerTemplate.fontId].maxLetterHeight + textPrinter->printerTemplate.lineSpacing;
            return RENDER_REPEAT;
        case PLACEHOLDER_BEGIN:
            textPrinter->printerTemplate.currentChar++;
            return RENDER_REPEAT;
        case EXT_CTRL_CODE_BEGIN:
            currChar = *textPrinter->printerTemplate.currentChar;
            textPrinter->printerTemplate.currentChar++;
            switch (currChar)
            {
            case EXT_CTRL_CODE_COLOR:
                textPrinter->printerTemplate.fgColor = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                GenerateFontHalfRowLookupTable(textPrinter->printerTemplate.fgColor, textPrinter->printerTemplate.bgColor, textPrinter->printerTemplate.shadowColor);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_HIGHLIGHT:
                textPrinter->printerTemplate.bgColor = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                GenerateFontHalfRowLookupTable(textPrinter->printerTemplate.fgColor, textPrinter->printerTemplate.bgColor, textPrinter->printerTemplate.shadowColor);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_SHADOW:
                textPrinter->printerTemplate.shadowColor = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                GenerateFontHalfRowLookupTable(textPrinter->printerTemplate.fgColor, textPrinter->printerTemplate.bgColor, textPrinter->printerTemplate.shadowColor);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW:
                textPrinter->printerTemplate.fgColor = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                textPrinter->printerTemplate.bgColor = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                textPrinter->printerTemplate.shadowColor = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                GenerateFontHalfRowLookupTable(textPrinter->printerTemplate.fgColor, textPrinter->printerTemplate.bgColor, textPrinter->printerTemplate.shadowColor);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_PALETTE:
                textPrinter->printerTemplate.currentChar++;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_FONT:
                subStruct->glyphId = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_RESET_FONT:
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_PAUSE:
                textPrinter->delayCounter = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                textPrinter->state = RENDER_STATE_PAUSE;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_PAUSE_UNTIL_PRESS:
                textPrinter->state = RENDER_STATE_WAIT;
                if (gTextFlags.autoScroll)
                    subStruct->autoScrollDelay = 0;
                return RENDER_UPDATE;
            case EXT_CTRL_CODE_WAIT_SE:
                textPrinter->state = RENDER_STATE_WAIT_SE;
                return RENDER_UPDATE;
            case EXT_CTRL_CODE_PLAY_BGM:
                currChar = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                currChar |= *textPrinter->printerTemplate.currentChar << 8;
                textPrinter->printerTemplate.currentChar++;
                if (!QL_IS_PLAYBACK_STATE)
                    PlayBGM(currChar);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_PLAY_SE:
                currChar = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                currChar |= (*textPrinter->printerTemplate.currentChar << 8);
                textPrinter->printerTemplate.currentChar++;
                PlaySE(currChar);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_ESCAPE:
                textPrinter->printerTemplate.currentChar++;
                currChar = *textPrinter->printerTemplate.currentChar;
                break;
            case EXT_CTRL_CODE_SHIFT_RIGHT:
                textPrinter->printerTemplate.currentX = textPrinter->printerTemplate.x + *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_SHIFT_DOWN:
                textPrinter->printerTemplate.currentY = textPrinter->printerTemplate.y + *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_FILL_WINDOW:
                FillWindowPixelBuffer(textPrinter->printerTemplate.windowId, PIXEL_FILL(textPrinter->printerTemplate.bgColor));
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_PAUSE_MUSIC:
                m4aMPlayStop(&gMPlayInfo_BGM);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_RESUME_MUSIC:
                m4aMPlayContinue(&gMPlayInfo_BGM);
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_CLEAR:
                width = *textPrinter->printerTemplate.currentChar;
                textPrinter->printerTemplate.currentChar++;
                if (width > 0)
                {
                    ClearTextSpan(textPrinter, width);
                    textPrinter->printerTemplate.currentX += width;
                    return RENDER_PRINT;
                }
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_SKIP:
                textPrinter->printerTemplate.currentX = *textPrinter->printerTemplate.currentChar + textPrinter->printerTemplate.x;
                textPrinter->printerTemplate.currentChar++;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_CLEAR_TO:
                {
                    widthHelper = *textPrinter->printerTemplate.currentChar;
                    widthHelper += textPrinter->printerTemplate.x;
                    textPrinter->printerTemplate.currentChar++;
                    width = widthHelper - textPrinter->printerTemplate.currentX;
                    if (width > 0)
                    {
                        ClearTextSpan(textPrinter, width);
                        textPrinter->printerTemplate.currentX += width;
                        return RENDER_PRINT;
                    }
                }
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_MIN_LETTER_SPACING:
                textPrinter->minLetterSpacing = *textPrinter->printerTemplate.currentChar++;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_JPN:
                textPrinter->japanese = TRUE;
                return RENDER_REPEAT;
            case EXT_CTRL_CODE_ENG:
                textPrinter->japanese = FALSE;
                return RENDER_REPEAT;
            }
            break;
        case CHAR_PROMPT_CLEAR:
            textPrinter->state = RENDER_STATE_CLEAR;
            TextPrinterInitDownArrowCounters(textPrinter);
            return RENDER_UPDATE;
        case CHAR_PROMPT_SCROLL:
            textPrinter->state = RENDER_STATE_SCROLL_START;
            TextPrinterInitDownArrowCounters(textPrinter);
            return RENDER_UPDATE;
        case CHAR_EXTRA_SYMBOL:
            currChar = *textPrinter->printerTemplate.currentChar | 0x100;
            textPrinter->printerTemplate.currentChar++;
            break;
        case CHAR_KEYPAD_ICON:
            currChar = *textPrinter->printerTemplate.currentChar++;
            gGlyphInfo.width = DrawKeypadIcon(textPrinter->printerTemplate.windowId, currChar, textPrinter->printerTemplate.currentX, textPrinter->printerTemplate.currentY);
            textPrinter->printerTemplate.currentX += gGlyphInfo.width + textPrinter->printerTemplate.letterSpacing;
            return RENDER_PRINT;
        case EOS:
            return RENDER_FINISH;
        }

        switch (subStruct->glyphId)
        {
        case FONT_SMALL:
            DecompressGlyph_Small(currChar, textPrinter->japanese);
            break;
        case FONT_NORMAL_COPY_1:
            DecompressGlyph_NormalCopy1(currChar, textPrinter->japanese);
            break;
        case FONT_NORMAL:
            DecompressGlyph_Normal(currChar, textPrinter->japanese);
            break;
        case FONT_NORMAL_COPY_2:
            DecompressGlyph_NormalCopy2(currChar, textPrinter->japanese);
            break;
        case FONT_MALE:
            DecompressGlyph_Male(currChar, textPrinter->japanese);
            break;
        case FONT_FEMALE:
            DecompressGlyph_Female(currChar, textPrinter->japanese);
            break;
        }

        CopyGlyphToWindow(textPrinter);

        if (textPrinter->minLetterSpacing)
        {
            textPrinter->printerTemplate.currentX += gGlyphInfo.width;
            width = textPrinter->minLetterSpacing - gGlyphInfo.width;
            if (width > 0)
            {
                ClearTextSpan(textPrinter, width);
                textPrinter->printerTemplate.currentX += width;
            }
        }
        else
        {
            if (textPrinter->japanese)
                textPrinter->printerTemplate.currentX += (gGlyphInfo.width + textPrinter->printerTemplate.letterSpacing);
            else
                textPrinter->printerTemplate.currentX += gGlyphInfo.width;
        }
        return RENDER_PRINT;
    case RENDER_STATE_WAIT:
        if (TextPrinterWait(textPrinter))
            textPrinter->state = RENDER_STATE_HANDLE_CHAR;
        return RENDER_UPDATE;
    case RENDER_STATE_CLEAR:
        if (TextPrinterWaitWithDownArrow(textPrinter))
        {
            FillWindowPixelBuffer(textPrinter->printerTemplate.windowId, PIXEL_FILL(textPrinter->printerTemplate.bgColor));
            textPrinter->printerTemplate.currentX = textPrinter->printerTemplate.x;
            textPrinter->printerTemplate.currentY = textPrinter->printerTemplate.y;
            textPrinter->state = RENDER_STATE_HANDLE_CHAR;
        }
        return RENDER_UPDATE;
    case RENDER_STATE_SCROLL_START:
        if (TextPrinterWaitWithDownArrow(textPrinter))
        {
            TextPrinterClearDownArrow(textPrinter);
            textPrinter->scrollDistance = gFonts[textPrinter->printerTemplate.fontId].maxLetterHeight + textPrinter->printerTemplate.lineSpacing;
            textPrinter->printerTemplate.currentX = textPrinter->printerTemplate.x;
            textPrinter->state = RENDER_STATE_SCROLL;
        }
        return RENDER_UPDATE;
    case RENDER_STATE_SCROLL:
        if (textPrinter->scrollDistance)
        {
    
            if (textPrinter->scrollDistance < sWindowVerticalScrollSpeeds[gSaveBlock2Ptr->optionsTextSpeed])
            {
                ScrollWindow(textPrinter->printerTemplate.windowId, 0, textPrinter->scrollDistance, PIXEL_FILL(textPrinter->printerTemplate.bgColor));
                textPrinter->scrollDistance = 0;
            }
            else
            {
                ScrollWindow(textPrinter->printerTemplate.windowId, 0, sWindowVerticalScrollSpeeds[gSaveBlock2Ptr->optionsTextSpeed], PIXEL_FILL(textPrinter->printerTemplate.bgColor));
                textPrinter->scrollDistance -= sWindowVerticalScrollSpeeds[gSaveBlock2Ptr->optionsTextSpeed];
            }
            CopyWindowToVram(textPrinter->printerTemplate.windowId, COPYWIN_GFX);
        }
        else
        {
            textPrinter->state = RENDER_STATE_HANDLE_CHAR;
        }
        return RENDER_UPDATE;
    case RENDER_STATE_WAIT_SE:
        if (!IsSEPlaying())
            textPrinter->state = RENDER_STATE_HANDLE_CHAR;
        return RENDER_UPDATE;
    case RENDER_STATE_PAUSE:
        if (textPrinter->delayCounter != 0)
            textPrinter->delayCounter--;
        else
            textPrinter->state = RENDER_STATE_HANDLE_CHAR;
        return RENDER_UPDATE;
    }

    return RENDER_FINISH;
}

// Unused
static s32 GetStringWidthFixedWidthFont(const u8 *str, u8 fontId, u8 letterSpacing)
{
    int i;
    u8 width;
    int temp;
    int temp2;
    u8 line;
    int strPos;
    u8 lineWidths[8];
    const u8 *strLocal;

    for (i = 0; i < (int)ARRAY_COUNT(lineWidths); i++)
        lineWidths[i] = 0;

    width = 0;
    line = 0;
    strLocal = str;
    strPos = 0;

    do
    {
        temp = strLocal[strPos++];
        switch (temp)
        {
        case CHAR_NEWLINE:
        case EOS:
            lineWidths[line] = width;
            width = 0;
            line++;
            break;
        case EXT_CTRL_CODE_BEGIN:
            temp2 = strLocal[strPos++];
            switch (temp2)
            {
            case EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW:
                ++strPos;
            case EXT_CTRL_CODE_PLAY_BGM:
            case EXT_CTRL_CODE_PLAY_SE:
                ++strPos;
            case EXT_CTRL_CODE_COLOR:
            case EXT_CTRL_CODE_HIGHLIGHT:
            case EXT_CTRL_CODE_SHADOW:
            case EXT_CTRL_CODE_PALETTE:
            case EXT_CTRL_CODE_FONT:
            case EXT_CTRL_CODE_PAUSE:
            case EXT_CTRL_CODE_ESCAPE:
            case EXT_CTRL_CODE_SHIFT_RIGHT:
            case EXT_CTRL_CODE_SHIFT_DOWN:
            case EXT_CTRL_CODE_CLEAR:
            case EXT_CTRL_CODE_SKIP:
            case EXT_CTRL_CODE_CLEAR_TO:
            case EXT_CTRL_CODE_MIN_LETTER_SPACING:
                ++strPos;
                break;
            case EXT_CTRL_CODE_RESET_FONT:
            case EXT_CTRL_CODE_PAUSE_UNTIL_PRESS:
            case EXT_CTRL_CODE_WAIT_SE:
            case EXT_CTRL_CODE_FILL_WINDOW:
            case EXT_CTRL_CODE_JPN:
            case EXT_CTRL_CODE_ENG:
            default:
                break;
            }
            break;
        case CHAR_DYNAMIC:
        case PLACEHOLDER_BEGIN:
            ++strPos;
            break;
        case CHAR_PROMPT_SCROLL:
        case CHAR_PROMPT_CLEAR:
            break;
        case CHAR_KEYPAD_ICON:
        case CHAR_EXTRA_SYMBOL:
            ++strPos;
        default:
            ++width;
            break;
        }
    } while (temp != EOS);

    for (width = 0, strPos = 0; strPos < (int)ARRAY_COUNT(lineWidths); ++strPos)
    {
        if (width < lineWidths[strPos])
            width = lineWidths[strPos];
    }

    return (u8)(GetFontAttribute(fontId, FONTATTR_MAX_LETTER_WIDTH) + letterSpacing) * width;
}

/**
 * FUNCTION: GetFontWidthFunc
 *
 * PURPOSE: Look up the glyph-width-query function for a given font ID.
 *
 * HOW IT WORKS:
 * Searches the sGlyphWidthFuncs table for a matching font ID and returns
 * a function pointer that, given a glyph ID, returns that glyph's pixel width.
 * Returns NULL if the font ID is not found.
 *
 * PARAMETERS:
 * @param glyphId - The font ID (FONT_SMALL, FONT_NORMAL, etc.)
 *
 * RETURNS: Function pointer of type s32(*)(u16, bool32), or NULL.
 */
s32 (*GetFontWidthFunc(u8 glyphId))(u16 _glyphId, bool32 _isJapanese)
{
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sGlyphWidthFuncs); ++i)
    {
        if (glyphId == sGlyphWidthFuncs[i].fontId)
            return *sGlyphWidthFuncs[i].func;
    }

    return NULL;
}

/**
 * FUNCTION: GetStringWidth
 *
 * PURPOSE: Calculate the total pixel width of a text string without actually
 *          rendering it. Essential for centering text or sizing windows.
 *
 * HOW IT WORKS:
 * Walks through the entire string, accumulating glyph widths. Handles all the
 * same control codes as RenderText (color changes, font switches, clear/skip
 * commands, placeholder string insertions, etc.) but only tracks width, never
 * draws anything. For multi-line strings, returns the width of the widest line.
 *
 * PARAMETERS:
 * @param fontId        - Which font to measure with
 * @param str           - The text string to measure
 * @param letterSpacing - Extra pixels between characters (-1 = use font default)
 *
 * RETURNS: The pixel width of the widest line in the string.
 */
s32 GetStringWidth(u8 fontId, const u8 *str, s16 letterSpacing)
{
    bool8 isJapanese;
    int minGlyphWidth;
    s32 (*func)(u16 glyphId, bool32 isJapanese);
    int localLetterSpacing;
    u32 lineWidth;
    const u8 *bufferPointer;
    int glyphWidth;
    u32 width;

    isJapanese = FALSE;
    minGlyphWidth = 0;

    func = GetFontWidthFunc(fontId);
    if (func == NULL)
        return 0;

    if (letterSpacing == -1)
        localLetterSpacing = GetFontAttribute(fontId, FONTATTR_LETTER_SPACING);
    else
        localLetterSpacing = letterSpacing;

    width = 0;
    lineWidth = 0;
    bufferPointer = NULL;

    while (*str != EOS)
    {
        switch (*str)
        {
        case CHAR_NEWLINE:
            if (lineWidth > width)
                width = lineWidth;
            lineWidth = 0;
            break;
        case PLACEHOLDER_BEGIN:
            switch (*++str)
            {
                case PLACEHOLDER_ID_STRING_VAR_1:
                    bufferPointer = gStringVar1;
                    break;
                case PLACEHOLDER_ID_STRING_VAR_2:
                    bufferPointer = gStringVar2;
                    break;
                case PLACEHOLDER_ID_STRING_VAR_3:
                    bufferPointer = gStringVar3;
                    break;
                default:
                    return 0;
            }
        case CHAR_DYNAMIC:
            if (bufferPointer == NULL)
                bufferPointer = DynamicPlaceholderTextUtil_GetPlaceholderPtr(*++str);
            while (*bufferPointer != EOS)
            {
                glyphWidth = func(*bufferPointer++, isJapanese);
                if (minGlyphWidth > 0)
                    lineWidth += minGlyphWidth > glyphWidth ? minGlyphWidth : glyphWidth;
                else
                    lineWidth += isJapanese ? glyphWidth + localLetterSpacing : glyphWidth;
            }
            bufferPointer = NULL;
            break;
        case CHAR_PROMPT_SCROLL:
        case CHAR_PROMPT_CLEAR:
            break;
        case EXT_CTRL_CODE_BEGIN:
            switch (*++str)
            {
            case EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW:
                ++str;
            case EXT_CTRL_CODE_PLAY_BGM:
            case EXT_CTRL_CODE_PLAY_SE:
                ++str;
            case EXT_CTRL_CODE_COLOR:
            case EXT_CTRL_CODE_HIGHLIGHT:
            case EXT_CTRL_CODE_SHADOW:
            case EXT_CTRL_CODE_PALETTE:
            case EXT_CTRL_CODE_PAUSE:
            case EXT_CTRL_CODE_ESCAPE:
            case EXT_CTRL_CODE_SHIFT_RIGHT:
            case EXT_CTRL_CODE_SHIFT_DOWN:
                ++str;
            case EXT_CTRL_CODE_RESET_FONT:
            case EXT_CTRL_CODE_PAUSE_UNTIL_PRESS:
            case EXT_CTRL_CODE_WAIT_SE:
            case EXT_CTRL_CODE_FILL_WINDOW:
                break;
            case EXT_CTRL_CODE_FONT:
                func = GetFontWidthFunc(*++str);
                if (func == NULL)
                    return 0;
                if (letterSpacing == -1)
                    localLetterSpacing = GetFontAttribute(*str, FONTATTR_LETTER_SPACING);
                break;
            case EXT_CTRL_CODE_CLEAR:
                glyphWidth = *++str;
                lineWidth += glyphWidth;
                break;
            case EXT_CTRL_CODE_SKIP:
                lineWidth = *++str;
                break;
            case EXT_CTRL_CODE_CLEAR_TO:
                if (*++str > lineWidth)
                    lineWidth = *str;
                break;
            case EXT_CTRL_CODE_MIN_LETTER_SPACING:
                minGlyphWidth = *++str;
                break;
            case EXT_CTRL_CODE_JPN:
                isJapanese = TRUE;
                break;
            case EXT_CTRL_CODE_ENG:
                isJapanese = FALSE;
            default:
                break;
            }
            break;
        case CHAR_KEYPAD_ICON:
        case CHAR_EXTRA_SYMBOL:
            if (*str == CHAR_EXTRA_SYMBOL)
                glyphWidth = func(*++str | 0x100, isJapanese);
            else
                glyphWidth = GetKeypadIconWidth(*++str);

            if (minGlyphWidth > 0)
            {
                if (glyphWidth < minGlyphWidth)
                    glyphWidth = minGlyphWidth;
            }
            else if (isJapanese)
            {
                glyphWidth += localLetterSpacing;
            }
            lineWidth += glyphWidth;
            break;
        default:
            glyphWidth = func(*str, isJapanese);
            if (minGlyphWidth > 0)
            {
                if (glyphWidth < minGlyphWidth)
                    glyphWidth = minGlyphWidth;
                lineWidth += glyphWidth;
            }
            else
            {
                if (fontId != FONT_BRAILLE && isJapanese)
                    glyphWidth += localLetterSpacing;
                lineWidth += glyphWidth;
            }
            break;
        }
        ++str;
    }

    if (lineWidth > width)
        return lineWidth;
    return width;
}

/**
 * FUNCTION: RenderTextHandleBold
 *
 * PURPOSE: Render a string using the Bold font directly into a pixel buffer
 *          (not into a window). Used for special rendering contexts like the
 *          quest log or help system where text needs to be pre-rendered.
 *
 * HOW IT WORKS:
 * Unlike RenderText which draws to windows, this function decompresses each
 * glyph and copies the raw pixel data into a caller-provided buffer. It handles
 * color control codes but ignores positioning/scrolling codes. Each glyph
 * occupies 0x40 bytes (two 8x8 tiles of 4bpp data = 32 bytes each, arranged
 * as top tile + bottom tile). The output buffer advances by 0x40 per character.
 *
 * PARAMETERS:
 * @param pixels - Destination buffer for rendered glyph pixel data
 * @param fontId - Font to use (may be changed mid-string by control codes)
 * @param str    - The text string to render
 * @param a3-a7  - Unused parameters (kept for calling convention compatibility)
 *
 * RETURNS: Always returns 1.
 */
u8 RenderTextHandleBold(u8 *pixels, u8 fontId, u8 *str, int a3, int a4, int a5, int a6, int a7)
{
    u8 shadowColor;
    u8 *strLocal;
    int strPos;
    int temp;
    int temp2;
    u8 colorBackup[3];
    u8 fgColor;
    u8 bgColor;

    SaveTextColors(&colorBackup[0], &colorBackup[1], &colorBackup[2]);

    fgColor = 1;
    bgColor = 0;
    shadowColor = 3;

    GenerateFontHalfRowLookupTable(TEXT_COLOR_WHITE, TEXT_COLOR_TRANSPARENT, TEXT_COLOR_LIGHT_GRAY);
    strLocal = str;
    strPos = 0;

    do
    {
        temp = strLocal[strPos++];
        switch (temp)
        {
        case EXT_CTRL_CODE_BEGIN:
            temp2 = strLocal[strPos++];
            switch (temp2)
            {
            case EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW:
                fgColor = strLocal[strPos++];
                bgColor = strLocal[strPos++];
                shadowColor = strLocal[strPos++];
                GenerateFontHalfRowLookupTable(fgColor, bgColor, shadowColor);
                continue;
            case EXT_CTRL_CODE_COLOR:
                fgColor = strLocal[strPos++];
                GenerateFontHalfRowLookupTable(fgColor, bgColor, shadowColor);
                continue;
            case EXT_CTRL_CODE_HIGHLIGHT:
                bgColor = strLocal[strPos++];
                GenerateFontHalfRowLookupTable(fgColor, bgColor, shadowColor);
                continue;
            case EXT_CTRL_CODE_SHADOW:
                shadowColor = strLocal[strPos++];
                GenerateFontHalfRowLookupTable(fgColor, bgColor, shadowColor);
                continue;
            case EXT_CTRL_CODE_FONT:
                fontId = strLocal[strPos++];
                break;
            case EXT_CTRL_CODE_PLAY_BGM:
            case EXT_CTRL_CODE_PLAY_SE:
                ++strPos;
            case EXT_CTRL_CODE_PALETTE:
            case EXT_CTRL_CODE_PAUSE:
            case EXT_CTRL_CODE_ESCAPE:
            case EXT_CTRL_CODE_SHIFT_RIGHT:
            case EXT_CTRL_CODE_SHIFT_DOWN:
            case EXT_CTRL_CODE_CLEAR:
            case EXT_CTRL_CODE_SKIP:
            case EXT_CTRL_CODE_CLEAR_TO:
            case EXT_CTRL_CODE_MIN_LETTER_SPACING:
                ++strPos;
                break;
            case EXT_CTRL_CODE_RESET_FONT:
            case EXT_CTRL_CODE_PAUSE_UNTIL_PRESS:
            case EXT_CTRL_CODE_WAIT_SE:
            case EXT_CTRL_CODE_FILL_WINDOW:
            case EXT_CTRL_CODE_JPN:
            case EXT_CTRL_CODE_ENG:
            default:
                continue;
            }
            break;
        case CHAR_DYNAMIC:
        case CHAR_KEYPAD_ICON:
        case CHAR_EXTRA_SYMBOL:
        case PLACEHOLDER_BEGIN:
            ++strPos;
            break;
        case CHAR_PROMPT_SCROLL:
        case CHAR_PROMPT_CLEAR:
        case CHAR_NEWLINE:
        case EOS:
            break;
        default:
            DecompressGlyph_Bold(temp);
            CpuCopy32(gGlyphInfo.pixels, pixels, 0x20);
            CpuCopy32(gGlyphInfo.pixels + 0x40, pixels + 0x20, 0x20);
            pixels += 0x40;
            break;
        }
    }
    while (temp != EOS);

    RestoreTextColors(&colorBackup[0], &colorBackup[1], &colorBackup[2]);
    return 1;
}

/* Sprite data field aliases for the text cursor sprite.
 * GBA sprites have a data[] array for storing custom state. */
#define sDelay data[0]
#define sState data[1]

/**
 * FUNCTION: SpriteCB_TextCursor
 *
 * PURPOSE: Animate the text cursor sprite with a gentle bounce effect.
 *
 * HOW IT WORKS:
 * Uses a frame delay counter (sDelay) that counts down each frame. When it
 * hits 0, the cursor moves to the next position in its 4-step bounce cycle:
 *   State 0: y offset = 0  (top position)
 *   State 1: y offset = 1  (slightly down)
 *   State 2: y offset = 2  (bottom position)
 *   State 3: y offset = 1  (back up, then reset to state 0)
 *
 * GBA CONTEXT:
 * sprite->y2 is a secondary Y offset that gets added to the sprite's base Y
 * position. By modifying y2 rather than the base y, the bounce animation is
 * independent of the sprite's actual screen position.
 */
static void SpriteCB_TextCursor(struct Sprite *sprite)
{
    if (sprite->sDelay)
    {
        sprite->sDelay--;
    }
    else
    {
        sprite->sDelay = CURSOR_DELAY;
        switch(sprite->sState)
        {
        case 0:
            sprite->y2 = 0;
            break;
        case 1:
            sprite->y2 = 1;
            break;
        case 2:
            sprite->y2 = 2;
            break;
        case 3:
            sprite->y2 = 1;
            sprite->sState = 0;
            return;
        }
        sprite->sState++;
    }
}

/**
 * FUNCTION: CreateTextCursorSprite
 *
 * PURPOSE: Create an animated cursor sprite at the specified position, typically
 *          shown next to dialogue text to indicate more text is available.
 *
 * GBA CONTEXT:
 * Sprites on the GBA are hardware objects (OBJ layer) that can be positioned
 * independently of backgrounds. This is useful for UI elements like cursors
 * because they can overlay any background without disturbing the tile data.
 * The priority field (0-3) controls which background layers the sprite appears
 * in front of or behind.
 *
 * PARAMETERS:
 * @param sheetId     - 0 or 1, selects which arrow tile sheet to use
 * @param x, y        - Screen position for the cursor
 * @param priority    - OBJ priority (0 = in front of all BGs, 3 = behind most BGs)
 * @param subpriority - Ordering relative to other sprites at the same priority
 *
 * RETURNS: The sprite ID for later reference (e.g., to destroy it).
 */
u8 CreateTextCursorSprite(u8 sheetId, u16 x, u16 y, u8 priority, u8 subpriority)
{
    u8 spriteId;
    LoadSpriteSheet(&sSpriteSheets_TextCursor[sheetId & 1]);
    LoadSpritePalette(&sSpritePalettes_TextCursor[0]);
    spriteId = CreateSprite(&sSpriteTemplate_TextCursor, x + 3, y + 4, subpriority);
    gSprites[spriteId].oam.priority = (priority & 3);
    gSprites[spriteId].oam.matrixNum = 0;
    gSprites[spriteId].sDelay = CURSOR_DELAY;
    return spriteId;
}

/**
 * FUNCTION: DestroyTextCursorSprite
 *
 * PURPOSE: Remove the text cursor sprite and free its graphics/palette memory.
 */
void DestroyTextCursorSprite(u8 spriteId)
{
    DestroySprite(&gSprites[spriteId]);
    FreeSpriteTilesByTag(TAG_CURSOR);
    FreeSpritePaletteByTag(TAG_CURSOR);
}

#undef sDelay
#undef sState

/**
 * FUNCTION: DrawKeypadIcon
 *
 * PURPOSE: Draw a GBA button icon (A, B, L, R, Start, D-pad, etc.) inline
 *          with text. Used when text strings contain {A_BUTTON} or similar.
 *
 * HOW IT WORKS:
 * Looks up the icon's tile offset, width, and height from sKeypadIcons[],
 * then blits that region from the gKeypadIconTiles tileset into the window.
 * The tile offset is multiplied by 0x20 (32 bytes per 4bpp 8x8 tile) to get
 * the byte offset into the tile data. Source bitmap is 0x80 pixels wide
 * (128 pixels = 16 tiles wide) and 0x80 pixels tall.
 *
 * PARAMETERS:
 * @param windowId     - Which window to draw into
 * @param keypadIconId - Which button icon (CHAR_A_BUTTON, CHAR_B_BUTTON, etc.)
 * @param x, y         - Pixel position within the window
 *
 * RETURNS: The pixel width of the drawn icon (for advancing the text cursor).
 */
u8 DrawKeypadIcon(u8 windowId, u8 keypadIconId, u16 x, u16 y)
{
    BlitBitmapRectToWindow(
        windowId,
        gKeypadIconTiles + (sKeypadIcons[keypadIconId].tileOffset * 0x20),
        0,
        0,
        0x80,
        0x80,
        x,
        y,
        sKeypadIcons[keypadIconId].width,
        sKeypadIcons[keypadIconId].height);
    return sKeypadIcons[keypadIconId].width;
}

u8 GetKeypadIconTileOffset(u8 keypadIconId)
{
    return sKeypadIcons[keypadIconId].tileOffset;
}

u8 GetKeypadIconWidth(u8 keypadIconId)
{
    return sKeypadIcons[keypadIconId].width;
}

u8 GetKeypadIconHeight(u8 keypadIconId)
{
    return sKeypadIcons[keypadIconId].height;
}

/**
 * FUNCTION: DecompressGlyph_Small
 *
 * PURPOSE: Decompress a character glyph from the Small font into the global
 *          gGlyphInfo pixel buffer for subsequent blitting to a window.
 *
 * HOW IT WORKS:
 * Font glyphs are stored as compressed 4bpp tile data in ROM. Each glyph
 * occupies either one or two 8x8 tiles depending on the font's height.
 *
 * For Japanese glyphs (8x12 fixed-width):
 *   The glyph sheet is organized in a grid. The formula to find a glyph's
 *   address is: base + (0x100 * row) + (0x8 * column), where:
 *     - row = glyphId >> 4 (divide by 16 to get the row)
 *     - column = glyphId & 0xF (mod 16 to get the column)
 *     - 0x100 = bytes per row of 16 glyphs (16 * 16 bytes each)
 *     - 0x8 = half-width of each glyph entry in u16 units
 *   The second tile (bottom half) is at offset 0x80 from the first.
 *
 * For Latin glyphs (variable-width, up to 8 pixels wide, 13 pixels tall):
 *   Simpler layout: base + (0x10 * glyphId), since Latin glyphs are stored
 *   sequentially with 0x10 (16) u16 entries per glyph (two 8x8 tiles).
 *   Width is looked up from sFontSmallLatinGlyphWidths[].
 *
 * GBA CONTEXT:
 * DecompressGlyphTile() takes compressed tile data and expands it into the
 * gGlyphInfo.pixels buffer. The pixel buffer uses 4bpp format where each
 * byte holds two pixels (one in the low nibble, one in the high nibble).
 * The buffer is organized as: pixels[0x00..0x1F] = top-left tile,
 * pixels[0x20..0x3F] = top-right tile, pixels[0x40..0x5F] = bottom-left,
 * pixels[0x60..0x7F] = bottom-right.
 */
void DecompressGlyph_Small(u16 glyphId, bool32 isJapanese)
{
    const u16 *glyphs;

    if (isJapanese == TRUE)
    {
        glyphs = sFontSmallJapaneseGlyphs + (0x100 * (glyphId >> 0x4)) + (0x8 * (glyphId & 0xF));
        DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
        DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
        gGlyphInfo.width = 8;
        gGlyphInfo.height = 12;
    }
    else
    {
        glyphs = sFontSmallLatinGlyphs + (0x10 * glyphId);
        DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
        DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x40));
        gGlyphInfo.width = sFontSmallLatinGlyphWidths[glyphId];
        gGlyphInfo.height = 13;
    }
}

static s32 GetGlyphWidth_Small(u16 glyphId, bool32 isJapanese)
{
    if (isJapanese == TRUE)
        return 8;
    else
        return sFontSmallLatinGlyphWidths[glyphId];
}

static void DecompressGlyph_NormalCopy1(u16 glyphId, bool32 isJapanese)
{
    const u16 *glyphs;

    if (isJapanese == TRUE)
    {
        // This font only differs from the Normal font in Japanese
        int eff;
        glyphs = sFontTallJapaneseGlyphs + (0x100 * (glyphId >> 0x4)) + (0x8 * (glyphId & (eff = 0xF)));  // shh, no questions, only matching now
        DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
        DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
        gGlyphInfo.width = 8;
        gGlyphInfo.height = 16;
    }
    else
    {
        glyphs = sFontNormalCopy1LatinGlyphs + (0x20 * glyphId);
        DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
        DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
        DecompressGlyphTile(glyphs + 0x10, (u16 *)(gGlyphInfo.pixels + 0x40));
        DecompressGlyphTile(glyphs + 0x18, (u16 *)(gGlyphInfo.pixels + 0x60));
        gGlyphInfo.width = sFontNormalCopy1LatinGlyphWidths[glyphId];
        gGlyphInfo.height = 14;
    }
}

static s32 GetGlyphWidth_NormalCopy1(u16 glyphId, bool32 isJapanese)
{
    if (isJapanese == TRUE)
        return 8;
    else
        return sFontNormalCopy1LatinGlyphWidths[glyphId];
}

/**
 * FUNCTION: DecompressGlyph_Normal
 *
 * PURPOSE: Decompress a character glyph from the Normal font. This is the
 *          main dialogue font used throughout the game.
 *
 * HOW IT WORKS:
 * Similar to DecompressGlyph_Small but handles wider glyphs. Japanese glyphs
 * are 10 pixels wide (requiring two tiles horizontally), while Latin glyphs
 * are up to 12 pixels wide and 14 pixels tall (requiring four tiles: 2x2 grid).
 *
 * Special case: glyphId 0 represents a "space" character. Instead of
 * decompressing tile data, it fills the pixel buffer with the shadow color
 * (GetLastTextColor(2) returns the current shadow color index). The width/height
 * are still set inside the loop body (a known Game Freak code quality issue where
 * constant assignments are needlessly repeated each iteration).
 *
 * For Japanese Normal font, the glyph grid uses:
 *   - 8 glyphs per row (>> 3 for row, & 0x7 for column)
 *   - 0x100 bytes per row, 0x10 per glyph entry
 *   - Each glyph spans 2 tiles wide (0x8 apart) and 2 tiles tall (0x80 apart)
 */
void DecompressGlyph_Normal(u16 glyphId, bool32 isJapanese)
{
    const u16 *glyphs;
    int i;
    u8 lastColor;

    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // Game Freak, please. writing the same values over and over...
                gGlyphInfo.width = 10;
                gGlyphInfo.height = 12;
            }
        }
        else
        {
            glyphs = sFontNormalJapaneseGlyphs + (0x100 * (glyphId >> 0x3)) + (0x10 * (glyphId & 0x7));
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x88, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = sFontNormalJapaneseGlyphWidths[glyphId];
            gGlyphInfo.height = 12;
        }
    }
    else
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // but why
                gGlyphInfo.width = sFontNormalLatinGlyphWidths[0];
                gGlyphInfo.height = 14;
            }
        }
        else
        {
            glyphs = sFontNormalLatinGlyphs + (0x20 * glyphId);
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x10, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x18, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = sFontNormalLatinGlyphWidths[glyphId];
            gGlyphInfo.height = 14;
        }
    }
}

static s32 GetGlyphWidth_Normal(u16 glyphId, bool32 isJapanese)
{
    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
            return 10;

        return sFontNormalJapaneseGlyphWidths[glyphId];
    }
    else
    {
        return sFontNormalLatinGlyphWidths[glyphId];
    }
}

static void DecompressGlyph_NormalCopy2(u16 glyphId, bool32 isJapanese)
{
    const u16 *glyphs;
    int i;
    u8 lastColor;

    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // Game Freak, please. writing the same values over and over...
                gGlyphInfo.width = 10;
                gGlyphInfo.height = 12;
            }
        }
        else
        {
            glyphs = sFontNormalJapaneseGlyphs + (0x100 * (glyphId >> 0x3)) + (0x10 * (glyphId & 0x7));
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x88, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = 10;
            gGlyphInfo.height = 12;
        }
    }
    else
        DecompressGlyph_Normal(glyphId, isJapanese);
}

static s32 GetGlyphWidth_NormalCopy2(u16 glyphId, bool32 isJapanese)
{
    if (isJapanese == TRUE)
        return 10;
    else
        return sFontNormalLatinGlyphWidths[glyphId];
}

static void DecompressGlyph_Male(u16 glyphId, bool32 isJapanese)
{
    const u16 *glyphs;
    int i;
    u8 lastColor;

    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // Game Freak, please. writing the same values over and over...
                gGlyphInfo.width = 10;
                gGlyphInfo.height = 12;
            }
        }
        else
        {
            glyphs = sFontMaleJapaneseGlyphs + (0x100 * (glyphId >> 0x3)) + (0x10 * (glyphId & 0x7));
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x88, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = sFontMaleJapaneseGlyphWidths[glyphId];
            gGlyphInfo.height = 12;
        }
    }
    else
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // but why
                gGlyphInfo.width = sFontMaleLatinGlyphWidths[0];
                gGlyphInfo.height = 14;
            }
        }
        else
        {
            glyphs = sFontMaleLatinGlyphs + (0x20 * glyphId);
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x10, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x18, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = sFontMaleLatinGlyphWidths[glyphId];
            gGlyphInfo.height = 14;
        }
    }
}

static s32 GetGlyphWidth_Male(u16 glyphId, bool32 isJapanese)
{
    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
            return 10;

        return sFontMaleJapaneseGlyphWidths[glyphId];
    }
    else
        return sFontMaleLatinGlyphWidths[glyphId];
}

/**
 * FUNCTION: DecompressGlyph_Female
 *
 * PURPOSE: Decompress a glyph from the Female font variant. Same structure as
 *          DecompressGlyph_Male but uses the female-specific glyph/width tables.
 *          The Male and Female fonts have subtly different character shapes to
 *          match the gender aesthetic of the player character's text style.
 */
void DecompressGlyph_Female(u16 glyphId, bool32 isJapanese)
{
    const u16 *glyphs;
    int i;
    u8 lastColor;

    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // Game Freak, please. writing the same values over and over...
                gGlyphInfo.width = 10;
                gGlyphInfo.height = 12;
            }
        }
        else
        {
            glyphs = sFontFemaleJapaneseGlyphs + (0x100 * (glyphId >> 0x3)) + (0x10 * (glyphId & 0x7));
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x88, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = sFontFemaleJapaneseGlyphWidths[glyphId];
            gGlyphInfo.height = 12;
        }
    }
    else
    {
        if (glyphId == 0)
        {
            lastColor = GetLastTextColor(2);

            for(i = 0; i < 0x80; i++)
            {
                gGlyphInfo.pixels[i] = lastColor | lastColor << 4;
                // but why
                gGlyphInfo.width = sFontFemaleLatinGlyphWidths[0];
                gGlyphInfo.height = 14;
            }
        }
        else
        {
            glyphs = sFontFemaleLatinGlyphs + (0x20 * glyphId);
            DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
            DecompressGlyphTile(glyphs + 0x8, (u16 *)(gGlyphInfo.pixels + 0x20));
            DecompressGlyphTile(glyphs + 0x10, (u16 *)(gGlyphInfo.pixels + 0x40));
            DecompressGlyphTile(glyphs + 0x18, (u16 *)(gGlyphInfo.pixels + 0x60));
            gGlyphInfo.width = sFontFemaleLatinGlyphWidths[glyphId];
            gGlyphInfo.height = 14;
        }
    }
}

static s32 GetGlyphWidth_Female(u16 glyphId, bool32 isJapanese)
{
    if (isJapanese == TRUE)
    {
        if (glyphId == 0)
            return 10;
        
        return sFontFemaleJapaneseGlyphWidths[glyphId];
    }
    else
        return sFontFemaleLatinGlyphWidths[glyphId];
}

/**
 * FUNCTION: DecompressGlyph_Bold
 *
 * PURPOSE: Decompress a glyph from the Bold Japanese-only font. Uses the same
 *          8x12 tile layout as the Small font (16 glyphs per row in the sheet).
 *          This font has no Latin variant and is used for special display contexts.
 */
static void DecompressGlyph_Bold(u16 glyphId)
{
    const u16 *glyphs = sFontBoldJapaneseGlyphs + (0x100 * (glyphId >> 0x4)) + (0x8 * (glyphId & 0xF));
    DecompressGlyphTile(glyphs, (u16 *)gGlyphInfo.pixels);
    DecompressGlyphTile(glyphs + 0x80, (u16 *)(gGlyphInfo.pixels + 0x40));
    gGlyphInfo.width = 8;
    gGlyphInfo.height = 12;
}
