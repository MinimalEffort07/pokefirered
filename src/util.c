/**
 * util.c - General-Purpose Utility Functions
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * This file provides low-level utility functions used throughout the codebase:
 * - Bit manipulation helpers (bit table, trailing zeros)
 * - Word/halfword conversion for memory alignment constraints
 * - CRC-16 checksums for data integrity (especially save data)
 * - Sprite tile copying with flip support
 * - Background affine transformation setup
 *
 * Many of these exist because of GBA hardware constraints. For example, the
 * GBA's memory bus is 16-bit in many regions, so 32-bit values sometimes need
 * to be split into two 16-bit halves for storage.
 *
 * ============================================================================
 */

#include "global.h"

/**
 * gBitTable: Precomputed lookup table of single-bit masks.
 *
 * gBitTable[n] = (1 << n), giving a 32-bit value with only bit n set.
 * For example: gBitTable[0] = 0x00000001, gBitTable[7] = 0x00000080.
 *
 * WHY THIS EXISTS:
 * The ARM7TDMI CPU in the GBA doesn't have a barrel shifter in all instruction
 * forms, and variable bit shifts can be expensive. By precomputing all 32
 * possible single-bit masks, the game can use a simple array lookup instead of
 * a shift operation. This table is used heavily throughout the codebase for
 * flag/bitfield operations (e.g., event flags, Pokemon move flags, item flags).
 *
 * Usage example: to check if flag N is set: if (flags & gBitTable[N])
 */
const u32 gBitTable[] =
{
    1 << 0,
    1 << 1,
    1 << 2,
    1 << 3,
    1 << 4,
    1 << 5,
    1 << 6,
    1 << 7,
    1 << 8,
    1 << 9,
    1 << 10,
    1 << 11,
    1 << 12,
    1 << 13,
    1 << 14,
    1 << 15,
    1 << 16,
    1 << 17,
    1 << 18,
    1 << 19,
    1 << 20,
    1 << 21,
    1 << 22,
    1 << 23,
    1 << 24,
    1 << 25,
    1 << 26,
    1 << 27,
    1 << 28,
    1 << 29,
    1 << 30,
    1 << 31,
};

/**
 * gInvisibleSpriteTemplate: A sprite template for creating invisible sprites.
 *
 * These invisible sprites are used as "logic containers" -- they exist in the
 * sprite system not to display anything on screen, but to have their callback
 * function run every frame. This is a common GBA game dev pattern: create a
 * sprite just to use the sprite system's per-frame callback mechanism as a
 * lightweight task/coroutine system.
 *
 * All visual properties (tile, palette, animations) use dummy/empty values
 * since nothing will be drawn.
 */
static const struct SpriteTemplate gInvisibleSpriteTemplate =
{
    .tileTag = 0,
    .paletteTag = 0,
    .oam = &gDummyOamData,
    .anims = gDummySpriteAnimTable,
    .images = NULL,
    .affineAnims = gDummySpriteAffineAnimTable,
    .callback = SpriteCallbackDummy,
};

/**
 * sSpriteDimensions: Lookup table for GBA sprite sizes in tiles.
 *
 * GBA CONTEXT:
 * The GBA hardware supports sprites (called "objects" or "OBJs") in specific
 * sizes defined by two OAM (Object Attribute Memory) fields:
 *   - Shape: 0 = square, 1 = horizontal rectangle, 2 = vertical rectangle
 *   - Size: 0-3, meaning depends on shape
 *
 * Each entry is {width_in_tiles, height_in_tiles} where each tile is 8x8 pixels.
 *
 * The full size table:
 *   Shape 0 (square):     8x8,  16x16, 32x32, 64x64 pixels
 *   Shape 1 (wide):      16x8,  32x8,  32x16, 64x32 pixels
 *   Shape 2 (tall):       8x16,  8x32, 16x32, 32x64 pixels
 *
 * Dividing by 8 gives tiles: {1,1}, {2,2}, {4,4}, {8,8} for squares, etc.
 */
static const u8 sSpriteDimensions[3][4][2] =
{
    // square
    {
        {1, 1},
        {2, 2},
        {4, 4},
        {8, 8},
    },

    // horizontal rectangle
    {
        {2, 1},
        {4, 1},
        {4, 2},
        {8, 4},
    },

    // vertical rectangle
    {
        {1, 2},
        {1, 4},
        {2, 4},
        {4, 8},
    },
};

/**
 * gCrc16Table: Precomputed lookup table for fast CRC-16 calculation.
 *
 * CRC (Cyclic Redundancy Check) is a checksum algorithm used to detect
 * corruption in data. This table allows computing CRC-16 one byte at a time
 * using a table lookup instead of processing bit-by-bit, which is 8x faster.
 *
 * The polynomial used is 0x8408 (bit-reversed form of the standard CCITT
 * polynomial 0x1021). The initial value 0x1121 is a non-standard seed.
 *
 * Used primarily for save data integrity verification -- if the CRC stored
 * in the save doesn't match the CRC of the save data, the save is considered
 * corrupt.
 */
static const u16 gCrc16Table[] =
{
    0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
    0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
    0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
    0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
    0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
    0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
    0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
    0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
    0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
    0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
    0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
    0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
    0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
    0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
    0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
    0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
    0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
    0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
    0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
    0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
    0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
    0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
    0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
    0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
    0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
    0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
    0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
    0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
    0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
    0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
    0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78,
};

/**
 * gMiscBlank_Gfx: A blank/empty 4bpp tile graphic loaded from a binary file.
 *
 * INCBIN_U8 is a macro that includes a binary file directly into the ROM data
 * as a byte array. This blank graphic is used as a placeholder or for clearing
 * tile regions.
 */
const u8 gMiscBlank_Gfx[] = INCBIN_U8("graphics/interface/blank.4bpp");

/**
 * FUNCTION: CreateInvisibleSpriteWithCallback
 *
 * PURPOSE: Create a sprite that is invisible but runs a callback every frame.
 *
 * HOW IT WORKS:
 * Creates a sprite using the invisible template (no visible graphics), places
 * it off-screen at position (248, 168) -- just past the visible 240x160 screen --
 * sets it as invisible, and assigns the provided callback function.
 *
 * GBA CONTEXT:
 * The sprite system calls each sprite's callback function once per frame.
 * By creating an "invisible sprite," game code can use this as a lightweight
 * task system -- the callback runs every frame just like a regular sprite
 * animation, but nothing is drawn. This is used for things like managing
 * screen fade effects, delayed events, or state machines that need per-frame
 * processing without being tied to a visible object.
 *
 * The priority of 14 places it in the middle of the sprite priority range,
 * which doesn't matter visually since it's invisible, but affects callback
 * execution order.
 *
 * PARAMETERS:
 * @param callback -- Function to call every frame (receives the sprite pointer)
 *
 * RETURNS: The sprite ID (index into the gSprites[] array)
 */
u8 CreateInvisibleSpriteWithCallback(void (*callback)(struct Sprite *))
{
    u8 sprite = CreateSprite(&gInvisibleSpriteTemplate, 248, 168, 14);
    gSprites[sprite].invisible = TRUE;
    gSprites[sprite].callback = callback;
    return sprite;
}

/**
 * FUNCTION: StoreWordInTwoHalfwords
 *
 * PURPOSE: Split a 32-bit word into two 16-bit halfwords for storage.
 *
 * HOW IT WORKS:
 * Stores the lower 16 bits in h[0] and the upper 16 bits in h[1].
 *
 * GBA CONTEXT:
 * Many GBA memory regions (EWRAM, save memory) have a 16-bit bus, meaning
 * 32-bit writes are split into two 16-bit writes by the hardware. Some data
 * structures use halfword arrays for alignment or compatibility reasons.
 * This function explicitly splits a 32-bit value for storage in such contexts.
 * Often used to store pointers or large values in save data.
 *
 * PARAMETERS:
 * @param h -- Pointer to an array of at least 2 u16 values
 * @param w -- The 32-bit word to store
 */
void StoreWordInTwoHalfwords(u16 *h, u32 w)
{
    h[0] = (u16)(w);         /* Lower 16 bits: bits 0-15 */
    h[1] = (u16)(w >> 16);   /* Upper 16 bits: bits 16-31 */
}

/**
 * FUNCTION: LoadWordFromTwoHalfwords
 *
 * PURPOSE: Reconstruct a 32-bit word from two 16-bit halfwords.
 *
 * HOW IT WORKS:
 * Takes the low halfword from h[0] and the high halfword from h[1], combines
 * them with a left shift and OR to produce the original 32-bit value.
 *
 * Note: h[1] is cast to s16 (signed) before the shift, which means if bit 15
 * of h[1] is set, the upper bits of the result will be sign-extended. This
 * correctly reconstructs negative 32-bit values.
 *
 * PARAMETERS:
 * @param h -- Pointer to array of two u16 halfwords
 * @param w -- Pointer to u32 where the reconstructed word is stored
 */
void LoadWordFromTwoHalfwords(u16 *h, u32 *w)
{
    *w = h[0] | (s16)h[1] << 16;
}

/**
 * FUNCTION: SetBgAffineStruct
 *
 * PURPOSE: Fill in a BgAffineSrcData struct with the given transformation parameters.
 *
 * GBA CONTEXT:
 * Affine transformations allow BG layers (in modes 1 and 2) to be rotated,
 * scaled, and scrolled. The GBA's BIOS function BgAffineSet() computes the
 * 2x2 rotation/scale matrix from human-readable parameters:
 *
 *   texX, texY: The center of rotation in the texture/source image (in pixels,
 *               with 8 bits of fractional precision -- fixed-point 24.8 format)
 *   scrX, scrY: The screen position that maps to the texture center
 *   sx, sy:     Scale factors (0x100 = 1.0x, 0x200 = 2.0x, 0x80 = 0.5x)
 *               Higher values = smaller/zoomed out, lower = bigger/zoomed in
 *   alpha:      Rotation angle (0-0xFFFF maps to 0-360 degrees)
 *
 * PARAMETERS:
 * @param src   -- The struct to fill in
 * @param texX  -- Texture center X (fixed-point 24.8)
 * @param texY  -- Texture center Y (fixed-point 24.8)
 * @param scrX  -- Screen center X
 * @param scrY  -- Screen center Y
 * @param sx    -- Horizontal scale (0x100 = 1:1)
 * @param sy    -- Vertical scale (0x100 = 1:1)
 * @param alpha -- Rotation angle (0-0xFFFF = 0-360 degrees)
 */
void SetBgAffineStruct(struct BgAffineSrcData *src, u32 texX, u32 texY, s16 scrX, s16 scrY, s16 sx, s16 sy, u16 alpha)
{
    src->texX = texX;
    src->texY = texY;
    src->scrX = scrX;
    src->scrY = scrY;
    src->sx = sx;
    src->sy = sy;
    src->alpha = alpha;
}

/**
 * FUNCTION: DoBgAffineSet
 *
 * PURPOSE: Compute a BG affine transformation matrix from human-readable parameters.
 *
 * HOW IT WORKS:
 * Fills in a BgAffineSrcData struct, then calls the BIOS function BgAffineSet()
 * to compute the actual 2x2 transformation matrix. The result (stored in 'dest')
 * contains the pa, pb, pc, pd matrix values and dx, dy displacement values that
 * get written to the GBA's BG affine registers.
 *
 * GBA CONTEXT:
 * BgAffineSet is BIOS call 0x0E (SWI 0x0E). It converts rotation angle + scale
 * into the four 16-bit fixed-point matrix values the hardware needs:
 *   pa = sx * cos(alpha)    pb = -sx * sin(alpha)
 *   pc = sy * sin(alpha)    pd = sy * cos(alpha)
 * These values are written to REG_BGxPA through REG_BGxPD.
 *
 * PARAMETERS:
 * @param dest  -- Output: the computed affine matrix and displacement
 * @param texX  -- Texture center X (fixed-point 24.8)
 * @param texY  -- Texture center Y (fixed-point 24.8)
 * @param scrX  -- Screen center X
 * @param scrY  -- Screen center Y
 * @param sx    -- Horizontal scale
 * @param sy    -- Vertical scale
 * @param alpha -- Rotation angle (0-0xFFFF)
 */
void DoBgAffineSet(struct BgAffineDstData *dest, u32 texX, u32 texY, s16 scrX, s16 scrY, s16 sx, s16 sy, u16 alpha)
{
    struct BgAffineSrcData src;

    SetBgAffineStruct(&src, texX, texY, scrX, scrY, sx, sy, alpha);
    BgAffineSet(&src, dest, 1);  /* 1 = compute one transformation */
}

/**
 * FUNCTION: CopySpriteTiles
 *
 * PURPOSE: Copy sprite tile data from a tileset, applying flip transformations
 *          based on tilemap attributes.
 *
 * HOW IT WORKS:
 * Reads a tilemap (array of tile references with flip attributes) and copies
 * the corresponding tile data from a tileset into the output buffer. Each
 * tilemap entry contains:
 *   - Bits 0-9: Tile index (which tile in the tileset, multiplied by 32 for byte offset)
 *   - Bit 10 (0x400): X-flip flag (mirror horizontally)
 *   - Bit 11 (0x800): Y-flip flag (mirror vertically)
 *
 * When flipping is needed, the function manually reverses pixel data:
 *   - Y-flip: Copies rows in reverse order (row 7 becomes row 0, etc.)
 *   - X-flip: Swaps pixels within each row AND swaps the two nybbles in each byte
 *     (since 4bpp stores two pixels per byte, swapping nybbles mirrors them)
 *   - Both flips: Applies X-flip first, then Y-flip on the result
 *
 * GBA CONTEXT:
 * Each 4bpp tile is 32 bytes (8 rows * 4 bytes per row). Each byte holds two
 * pixels: the low nybble (bits 0-3) is the left pixel, and the high nybble
 * (bits 4-7) is the right pixel. Flipping pixels horizontally requires
 * swapping the nybbles: (byte & 0xF) << 4 gives the low nybble shifted to
 * the high position, and byte >> 4 gives the high nybble shifted to the low.
 *
 * DmaCopy32Defvars uses DMA channel 3 for fast 32-bit memory copies.
 *
 * PARAMETERS:
 * @param shape   -- Sprite shape (0=square, 1=horizontal, 2=vertical)
 * @param size    -- Sprite size index (0-3, see sSpriteDimensions)
 * @param tiles   -- Source tileset data (raw 4bpp tile graphics)
 * @param tilemap -- Array of tile references with flip attributes
 * @param output  -- Destination buffer for the composed sprite tile data
 */
void CopySpriteTiles(u8 shape, u8 size, u8 *tiles, u16 *tilemap, u8 *output)
{
    u8 x, y;
    s8 i, j;
    u8 xflip[32];  /* Temporary buffer for one horizontally-flipped tile (32 bytes) */
    u8 h = sSpriteDimensions[shape][size][1];  /* Height in tiles */
    u8 w = sSpriteDimensions[shape][size][0];  /* Width in tiles */

    for (y = 0; y < h; y++)
    {
        /*
         * The tilemap is 32 tiles wide (standard BG tilemap width).
         * After processing 'w' tiles for this row, skip the remaining
         * (32 - w) entries to reach the next row in the tilemap.
         */
        int filler = 32 - w;

        for (x = 0; x < w; x++)
        {
            /* Extract tile index (bits 0-9) and multiply by 32 to get byte offset */
            u16 tile = (*tilemap & 0x3ff) * 32;
            /* Extract flip attributes (bits 10-11) */
            int attr = *tilemap & 0xc00;

            if (attr == 0)
            {
                /* No flip: straight DMA copy of 32 bytes (one full tile) */
                DmaCopy32Defvars(3, tiles + tile, output, 32);
            }
            else if (attr == 0x800)  // yflip only
            {
                /* Y-flip: copy rows in reverse order (row 7->0, 6->1, etc.) */
                for (i = 0; i < 8; i++)
                {
                    /*
                     * These increment/decrement operations exist only to match
                     * the original compiled binary. They have no functional effect.
                     */
                    u8 requiredForMatching = 0;

                    ++requiredForMatching;
                    --requiredForMatching;
                    /* Copy row (7-i) from source to row i in destination */
                    /* Each row = 4 bytes (8 pixels * 4bpp / 8 bits) */
                    DmaCopy32Defvars(3, tile + (7 - i) * 4 + tiles, output + i * 4, 4);
                }
            }
            else  // xflip (with possible yflip)
            {
                /*
                 * X-flip: for each row, reverse the pixel order.
                 * In 4bpp format, each byte holds 2 pixels (low nybble = left pixel,
                 * high nybble = right pixel). To mirror horizontally:
                 * 1. Reverse byte order within each row (byte 3->0, 2->1, etc.)
                 * 2. Swap the two nybbles in each byte (left/right pixel swap)
                 */
                for (i = 0; i < 8; i++)
                {
                    for (j = 0; j < 4; j++)
                    {
                        u8 i2 = i * 4;
                        /* Reverse byte order AND swap nybbles:
                         * (byte & 0xF) << 4 moves low nybble to high position
                         * byte >> 4 moves high nybble to low position
                         */
                        xflip[i2 + (3 - j)] = (tiles[tile + i2 + j] & 0xf) << 4;
                        xflip[i2 + (3 - j)] |= tiles[tile + i2 + j] >> 4;
                    }
                }
                if (*tilemap & 0x800)  // yflip as well (both X and Y flip)
                {
                    /* Apply Y-flip on top of the X-flipped data */
                    for (i = 0; i < 8; i++)
                    {
                        ++tile;
                        --tile;
                        DmaCopy32Defvars(3, (7 - i) * 4 + xflip, output + i * 4, 4);
                    }
                }
                else
                {
                    /* X-flip only: copy the flipped tile data directly */
                    DmaCopy32Defvars(3, xflip, output, 32);
                }
            }
            tilemap++;
            output += 32;  /* Advance output by one tile (32 bytes) */
        }
        tilemap += filler;  /* Skip to the next row in the 32-wide tilemap */
    }
}

/**
 * FUNCTION: CountTrailingZeroBits
 *
 * PURPOSE: Count the number of trailing zero bits in a 32-bit integer.
 *
 * HOW IT WORKS:
 * Checks each bit starting from bit 0 (least significant). Shifts the value
 * right by one each iteration until a set bit (1) is found, then returns
 * the count of zeros found.
 *
 * For example: value = 0b...1000 -> returns 3 (three trailing zeros).
 * If value = 0, returns 0 (all 32 bits checked, none set).
 *
 * Modern CPUs have a dedicated instruction for this (CTZ/TZCNT), but the
 * ARM7TDMI doesn't, so this manual loop is needed.
 *
 * PARAMETERS:
 * @param value -- The 32-bit value to analyze
 *
 * RETURNS: Number of trailing zero bits (0-31), or 0 if all bits are zero
 */
int CountTrailingZeroBits(u32 value)
{
    u8 i;

    for (i = 0; i < 32; i++)
    {
        if ((value & 1) == 0)
            value >>= 1;
        else
            return i;
    }
    return 0;
}

/**
 * FUNCTION: CalcCRC16
 *
 * PURPOSE: Calculate a CRC-16 checksum using bit-by-bit processing.
 *
 * HOW IT WORKS:
 * Processes data one bit at a time using the standard CRC division algorithm:
 * 1. XOR the current data byte into the CRC accumulator
 * 2. For each of the 8 bits: if the low bit is 1, shift right and XOR with
 *    the polynomial (0x8408); otherwise, just shift right
 * 3. After all bytes are processed, invert (bitwise NOT) the final CRC
 *
 * This is the "slow" version -- it processes 8 bits per byte with a loop.
 * CalcCRC16WithTable is the fast version using the precomputed table.
 *
 * The polynomial 0x8408 is the bit-reversed form of CCITT CRC-16 polynomial
 * 0x1021. The initial seed 0x1121 is non-standard (CCITT standard uses 0xFFFF).
 *
 * PARAMETERS:
 * @param data   -- Pointer to the data to checksum
 * @param length -- Number of bytes to process
 *
 * RETURNS: 16-bit CRC checksum (inverted)
 */
u16 CalcCRC16(const u8 *data, u32 length)
{
    u16 i, j;
    u16 crc = 0x1121;  /* Non-standard initial seed value */

    for (i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)  /* Process each bit */
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0x8408;  /* Polynomial: bit-reversed CCITT */
            else
                crc >>= 1;
        }
    }
    return ~crc;  /* Invert all bits for the final checksum */
}

/**
 * FUNCTION: CalcCRC16WithTable
 *
 * PURPOSE: Calculate a CRC-16 checksum using the precomputed lookup table.
 *
 * HOW IT WORKS:
 * The table-driven approach processes one full byte at a time instead of
 * bit-by-bit, making it ~8x faster than CalcCRC16. The algorithm:
 * 1. Take the upper byte of the current CRC
 * 2. XOR the data byte into the lower byte of the CRC
 * 3. Look up the lower byte in gCrc16Table to get the new CRC contribution
 * 4. XOR the saved upper byte with the table result
 *
 * GAME LOGIC:
 * Used for validating save data integrity. When saving, the game computes
 * the CRC of the save block and stores it. When loading, it recomputes the
 * CRC and compares -- if they differ, the save data is corrupt.
 *
 * PARAMETERS:
 * @param data   -- Pointer to the data to checksum
 * @param length -- Number of bytes to process
 *
 * RETURNS: 16-bit CRC checksum (inverted)
 */
u16 CalcCRC16WithTable(const u8 *data, u32 length)
{
    u16 i;
    u16 crc = 0x1121;  /* Same initial seed as CalcCRC16 */
    u8 byte;

    for (i = 0; i < length; i++)
    {
        byte = crc >> 8;                      /* Save upper byte of CRC */
        crc ^= data[i];                       /* XOR data byte into CRC */
        crc = byte ^ gCrc16Table[(u8)crc];    /* Table lookup on low byte, XOR with saved high byte */
    }
    return ~crc;
}

/**
 * FUNCTION: CalcByteArraySum
 *
 * PURPOSE: Calculate a simple additive checksum of a byte array.
 *
 * HOW IT WORKS:
 * Sums all bytes in the array. This is a simple checksum -- less robust than
 * CRC-16 (can miss certain types of errors like byte swaps) but faster to
 * compute. Used where speed matters more than error detection quality.
 *
 * PARAMETERS:
 * @param array -- Pointer to the byte array
 * @param size  -- Number of bytes to sum
 *
 * RETURNS: Sum of all bytes (may overflow, wrapping at 32 bits)
 */
u32 CalcByteArraySum(const u8 * array, u32 size)
{
    s32 i;
    u32 result = 0;

    for (i = 0; i < size; i++)
    {
        result += array[i];
    }

    return result;
}
