/*
 * decompress.c - Graphics Decompression and Pokemon Sprite Loading
 *
 * ============================================================================
 * WHY COMPRESSION?
 * ============================================================================
 *
 * The GBA ROM cartridge has 32 MB of space, but Pokemon FireRed has
 * THOUSANDS of graphics: 386+ Pokemon (front + back sprites), 100+ trainer
 * sprites, hundreds of tile sets, items, battle animations, etc.
 *
 * Most graphics data in the ROM is LZ77-COMPRESSED to save space.
 * LZ77 is a lossless compression algorithm that replaces repeated byte
 * sequences with back-references. Typical compression ratios are 40-60%
 * (the compressed data is 40-60% the size of the original).
 *
 * DECOMPRESSION FLOW:
 * 1. Compressed data sits in ROM (0x08000000 region, read-only).
 * 2. When needed, it's decompressed to a RAM buffer (gDecompressionBuffer
 *    in EWRAM, or a heap-allocated buffer).
 * 3. From the buffer, it's copied to VRAM (sprite tiles, BG tiles, etc.)
 *    or Palette RAM.
 *
 * WHY TWO STEPS (decompress to buffer, then copy)?
 *   - The LZ77 BIOS call writes 8 bits at a time (LZ77UnCompWram).
 *   - VRAM only accepts 16-bit or 32-bit writes. 8-bit writes to VRAM
 *     are silently ignored or write to the wrong address.
 *   - So we decompress to WRAM first (supports 8-bit writes), then
 *     DMA-copy to VRAM (using 16 or 32-bit transfers).
 *   - Exception: LZ77UnCompVram does 16-bit writes directly to VRAM.
 *
 * LZ77 DATA FORMAT:
 *   First 4 bytes: Header
 *     Byte 0: 0x10 (LZ77 identifier)
 *     Bytes 1-3: Decompressed size in bytes (little-endian, 24-bit)
 *   Remaining bytes: Compressed data stream
 *
 * COMPRESSED SPRITE SHEETS:
 *   A CompressedSpriteSheet combines the compressed tile data with
 *   metadata (size, tag). The tag is a unique ID used by the sprite
 *   system to identify which sprite sheet is loaded in VRAM.
 *
 * ============================================================================
 * POKEMON SPRITE SPECIAL CASES
 * ============================================================================
 *
 * Most Pokemon use a simple decompress-and-load flow. Two species have
 * special handling:
 *
 * UNOWN: Has 28 different letter forms (A-Z, ! and ?). The form is
 *   determined by the Pokemon's personality value using a formula that
 *   extracts bits from different positions in the 32-bit personality.
 *
 * DEOXYS: Has form-specific sprites. The sprite data is stored with the
 *   alternate form in the second half. DuplicateDeoxysTiles copies the
 *   correct half to fill the full sprite area.
 *
 * SPINDA: Has unique spot patterns. After decompression, DrawSpindaSpots
 *   applies random spots based on the personality value.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "decompress.h"
#include "pokemon.h"

extern const struct CompressedSpriteSheet gMonFrontPicTable[];
extern const struct CompressedSpriteSheet gMonBackPicTable[];

static void DuplicateDeoxysTiles(void *pointer, s32 species);

/**
 * FUNCTION: LZDecompressWram
 *
 * PURPOSE: Decompress LZ77 data to Work RAM (wrapper around BIOS call).
 *
 * GBA CONTEXT:
 * Calls the BIOS LZ77UnCompWram (SWI 0x11). Writes 8 bits at a time,
 * so it can decompress to any RAM address. Cannot write directly to VRAM.
 *
 * @param src  — Pointer to LZ77-compressed data in ROM
 * @param dest — Destination buffer in WRAM (EWRAM or IWRAM)
 */
void LZDecompressWram(const void *src, void *dest)
{
    LZ77UnCompWram(src, dest);
}

/**
 * FUNCTION: LZDecompressVram
 *
 * PURPOSE: Decompress LZ77 data directly to VRAM (wrapper around BIOS call).
 *
 * GBA CONTEXT:
 * Calls the BIOS LZ77UnCompVram (SWI 0x12). Writes 16 bits at a time,
 * which is compatible with VRAM's 16-bit bus. The destination MUST be
 * 2-byte aligned.
 *
 * @param src  — Pointer to LZ77-compressed data in ROM
 * @param dest — Destination in VRAM (must be 2-byte aligned)
 */
void LZDecompressVram(const void *src, void *dest)
{
    LZ77UnCompVram(src, dest);
}

/**
 * FUNCTION: LoadCompressedSpriteSheet
 *
 * PURPOSE: Decompress a sprite sheet and load it into OBJ VRAM.
 *
 * HOW IT WORKS:
 * 1. Decompresses the LZ77 data from ROM into gDecompressionBuffer (16 KB EWRAM).
 * 2. Creates a SpriteSheet struct pointing to the decompressed data.
 * 3. Calls LoadSpriteSheet which copies the tile data into OBJ VRAM
 *    and registers the sheet's tag for later lookup.
 *
 * @param src — CompressedSpriteSheet with compressed data, size, and tag
 *
 * RETURNS: The tile start index in OBJ VRAM where the sheet was loaded
 */
u16 LoadCompressedSpriteSheet(const struct CompressedSpriteSheet *src)
{
    struct SpriteSheet dest;

    LZ77UnCompWram(src->data, gDecompressionBuffer);
    dest.data = gDecompressionBuffer;
    dest.size = src->size;
    dest.tag = src->tag;
    return LoadSpriteSheet(&dest);
}

void LoadCompressedSpriteSheetOverrideBuffer(const struct CompressedSpriteSheet *src, void *buffer)
{
    struct SpriteSheet dest;

    LZ77UnCompWram(src->data, buffer);
    dest.data = buffer;
    dest.size = src->size;
    dest.tag = src->tag;
    LoadSpriteSheet(&dest);
}

/**
 * FUNCTION: LoadCompressedSpritePalette
 *
 * PURPOSE: Decompress a sprite palette and load it into OBJ palette RAM.
 *
 * HOW IT WORKS:
 * Same pattern as LoadCompressedSpriteSheet: decompress to buffer, then
 * load into the palette system. The tag identifies which palette slot
 * to use in OBJ palette RAM (0x05000200-0x050003FF).
 *
 * @param src — CompressedSpritePalette with compressed palette data and tag
 */
void LoadCompressedSpritePalette(const struct CompressedSpritePalette *src)
{
    struct SpritePalette dest;

    LZ77UnCompWram(src->data, gDecompressionBuffer);
    dest.data = (void *) gDecompressionBuffer;
    dest.tag = src->tag;
    LoadSpritePalette(&dest);
}

void LoadCompressedSpritePaletteOverrideBuffer(const struct CompressedSpritePalette *a, void *buffer)
{
    struct SpritePalette dest;

    LZ77UnCompWram(a->data, buffer);
    dest.data = buffer;
    dest.tag = a->tag;
    LoadSpritePalette(&dest);
}

void DecompressPicFromTable(const struct CompressedSpriteSheet *src, void *buffer, s32 species)
{
    if (species > NUM_SPECIES)
        LZ77UnCompWram(gMonFrontPicTable[0].data, buffer);
    else
        LZ77UnCompWram(src->data, buffer);
    DuplicateDeoxysTiles(buffer, species);
}

void HandleLoadSpecialPokePic(const struct CompressedSpriteSheet *src, void *dest, s32 species, u32 personality)
{
    bool8 isFrontPic;

    if (src == &gMonFrontPicTable[species])
        isFrontPic = TRUE; // frontPic
    else
        isFrontPic = FALSE; // backPic
    LoadSpecialPokePic(src, dest, species, personality, isFrontPic);
}

void LoadSpecialPokePic(const struct CompressedSpriteSheet *src, void *dest, s32 species, u32 personality, bool8 isFrontPic)
{
    if (species == SPECIES_UNOWN)
    {
        u16 i = (((personality & 0x3000000) >> 18) | ((personality & 0x30000) >> 12) | ((personality & 0x300) >> 6) | (personality & 3)) % 0x1C;

        // The other Unowns are separate from Unown A.
        if (i == 0)
            i = SPECIES_UNOWN;
        else
            i += SPECIES_UNOWN_B - 1;
        if (!isFrontPic)
            LZ77UnCompWram(gMonBackPicTable[i].data, dest);
        else
            LZ77UnCompWram(gMonFrontPicTable[i].data, dest);
    }
    else if (species > NUM_SPECIES) // is species unknown? draw the ? icon
        LZ77UnCompWram(gMonFrontPicTable[0].data, dest);
    else
        LZ77UnCompWram(src->data, dest);

    DuplicateDeoxysTiles(dest, species);
    DrawSpindaSpots(species, personality, dest, isFrontPic);
}

static void DuplicateDeoxysTiles(void *pointer, s32 species)
{
    if (species == SPECIES_DEOXYS)
        CpuCopy32(pointer + 0x800, pointer, 0x800);
}

static void Unused_LZDecompressWramIndirect(const void **src, void *dest)
{
    LZ77UnCompWram(*src, dest);
}

static void StitchObjectsOn8x8Canvas(s32 object_size, s32 object_count, u8 *src_tiles, u8 *dest_tiles)
{
    /*
      This function appears to emulate behaviour found in the GB(C) versions regarding how the Pokemon images
      are stitched together to be displayed on the battle screen.
      Given "compacted" tiles, an object count and a bounding box/object size, place the tiles in such a way
      that the result will have each object centered in a 8x8 tile canvas.
    */
    s32 i, j, k, l;
    u8 *src = src_tiles, *dest = dest_tiles;
    u8 bottom_off;

    if (object_size & 1)
    {
        // Object size is odd
        bottom_off = (object_size >> 1) + 4;
        for (l = 0; l < object_count; l++)
        {
            // Clear all unused rows of tiles plus the half-tile required due to centering
            for (j = 0; j < 8-object_size; j++)
            {
                for (k = 0; k < 8; k++)
                {
                    for (i = 0; i < 16; i++)
                    {
                        if (j % 2 == 0)
                        {
                            // Clear top half of top tile and bottom half of bottom tile when on even j
                            ((dest+i) + (k << 5))[((j >> 1) << 8)] = 0;
                            ((bottom_off << 8) + (dest+i) + (k << 5) + 16)[((j >> 1) << 8)] = 0;
                        }
                        else
                        {
                            // Clear bottom half of top tile and top half of tile following bottom tile when on odd j
                            ((dest+i) + (k << 5) + 16)[((j >> 1) << 8)] = 0;
                            ((bottom_off << 8) + (dest+i) + (k << 5) + 256)[((j >> 1) << 8)] = 0;
                        }
                    }
                }
            }

            // Clear the columns to the left and right that wont be used completely
            // Unlike the previous loops, this will clear the later used space as well
            for (j = 0; j < 2; j++)
            {
                for (i = 0; i < 8; i++)
                {
                    for (k = 0; k < 32; k++)
                    {
                        // Left side
                        ((dest+k) + (i << 8))[(j << 5)] = 0;
                        // Right side
                        ((dest+k) + (i << 8))[(j << 5)+192] = 0;
                    }
                }
            }

            // Skip the top row and first tile on the second row for objects of size 5
            if (object_size == 5) dest += 0x120;

            // Copy tile data
            for (j = 0; j < object_size; j++)
            {
                for (k = 0; k < object_size; k++)
                {
                    for (i = 0; i < 4; i++)
                    {
                        // Offset the tile by +4px in both x and y directions
                        (dest + (i << 2))[18] = (src + (i << 2))[0];
                        (dest + (i << 2))[19] = (src + (i << 2))[1];
                        (dest + (i << 2))[48] = (src + (i << 2))[2];
                        (dest + (i << 2))[49] = (src + (i << 2))[3];

                        (dest + (i << 2))[258] = (src + (i << 2))[16];
                        (dest + (i << 2))[259] = (src + (i << 2))[17];
                        (dest + (i << 2))[288] = (src + (i << 2))[18];
                        (dest + (i << 2))[289] = (src + (i << 2))[19];
                    }
                    src += 32;
                    dest += 32;
                }

                // At the end of a row, skip enough tiles to get to the beginning of the next row
                if (object_size == 7) dest += 0x20;
                else if (object_size == 5) dest += 0x60;
            }

            // Skip remaining unused space to go to the beginning of the next object
            if (object_size == 7) dest += 0x100;
            else if (object_size == 5) dest += 0x1e0;
        }
    }
    else
    {
        // Object size is even
        for (i = 0; i < object_count; i++)
        {
            // For objects of size 6, the first and last row and column will be cleared
            // While the remaining space will be filled with actual data
            if (object_size == 6)
            {
                for (k = 0; k < 256; k++) {
                    *dest = 0;
                    dest++;
                }
            }

            for (j = 0; j < object_size; j++)
            {
                if (object_size == 6)
                {
                    for (k = 0; k < 32; k++) {
                        *dest = 0;
                        dest++;
                    }
                }

                // Copy tile data
                for (k = 0; k < 32 * object_size; k++) {
                    *dest = *src;
                    src++;
                    dest++;
                }

                if (object_size == 6)
                {
                    for (k = 0; k < 32; k++) {
                        *dest = 0;
                        dest++;
                    }
                }
            }

            if (object_size == 6)
            {
                for (k = 0; k < 256; k++) {
                    *dest = 0;
                    dest++;
                }
            }
        }
    }
}

/**
 * FUNCTION: LoadCompressedSpriteSheetUsingHeap
 *
 * PURPOSE: Decompress and load a sprite sheet using heap-allocated memory.
 *
 * HOW IT WORKS:
 * Unlike LoadCompressedSpriteSheet (which uses the shared gDecompressionBuffer),
 * this function allocates its own temporary buffer on the heap. This is needed
 * when gDecompressionBuffer is already in use or when multiple decompressions
 * need to happen before the data is consumed.
 *
 * The decompressed size is read from the LZ77 header (bytes 1-3 of the
 * compressed data, shifted right by 8 to skip the 0x10 identifier byte).
 *
 * The buffer is freed after the sprite sheet is loaded to VRAM.
 *
 * @param src — CompressedSpriteSheet with compressed data, size, and tag
 *
 * RETURNS: FALSE on success, TRUE on failure (out of memory)
 */
bool8 LoadCompressedSpriteSheetUsingHeap(const struct CompressedSpriteSheet* src)
{
    struct SpriteSheet dest;
    void *buffer;

    buffer = AllocZeroed(*((u32 *)src->data) >> 8);
    if (!buffer)
        return TRUE;
    LZ77UnCompWram(src->data, buffer);
    dest.data = buffer;
    dest.size = src->size;
    dest.tag = src->tag;
    LoadSpriteSheet(&dest);
    Free(buffer);
    return FALSE;
}

bool8 LoadCompressedSpritePaletteUsingHeap(const struct CompressedSpritePalette *src)
{
    struct SpritePalette dest;
    void *buffer;

    buffer = AllocZeroed(*((u32 *)src->data) >> 8);
    if (!buffer)
        return TRUE;
    LZ77UnCompWram(src->data, buffer);
    dest.data = buffer;
    dest.tag = src->tag;
    LoadSpritePalette(&dest);
    Free(buffer);
    return FALSE;
}

/**
 * FUNCTION: GetDecompressedDataSize
 *
 * PURPOSE: Read the decompressed size from an LZ77 data header.
 *
 * HOW IT WORKS:
 * LZ77 compressed data has a 4-byte header:
 *   Byte 0: 0x10 (compression type identifier)
 *   Bytes 1-3: Decompressed size in little-endian byte order
 *
 * This function extracts bytes 1-3 and interprets them as a 24-bit
 * little-endian integer (the decompressed data size in bytes).
 *
 * @param ptr — Pointer to the start of LZ77 compressed data
 *
 * RETURNS: Size of the data after decompression, in bytes
 */
u32 GetDecompressedDataSize(const u8 *ptr)
{
    u32 ptr32[1];
    u8 *ptr8 = (u8 *)ptr32;

    ptr8[0] = ptr[1];
    ptr8[1] = ptr[2];
    ptr8[2] = ptr[3];
    ptr8[3] = 0;
    return ptr32[0];
}

void DecompressPicFromTable_DontHandleDeoxys(const struct CompressedSpriteSheet *src, void *buffer, s32 species)
{
    if (species > NUM_SPECIES)
        LZ77UnCompWram(gMonFrontPicTable[0].data, buffer);
    else
        LZ77UnCompWram(src->data, buffer);
}

void HandleLoadSpecialPokePic_DontHandleDeoxys(const struct CompressedSpriteSheet *src, void *dest, s32 species, u32 personality)
{
    bool8 isFrontPic;

    if (src == &gMonFrontPicTable[species])
        isFrontPic = TRUE; // frontPic
    else
        isFrontPic = FALSE; // backPic
    LoadSpecialPokePic_DontHandleDeoxys(src, dest, species, personality, isFrontPic);
}

void LoadSpecialPokePic_DontHandleDeoxys(const struct CompressedSpriteSheet *src, void *dest, s32 species, u32 personality, bool8 isFrontPic)
{
    if (species == SPECIES_UNOWN)
    {
        u16 i = (((personality & 0x3000000) >> 18) | ((personality & 0x30000) >> 12) | ((personality & 0x300) >> 6) | (personality & 3)) % 0x1C;

        // The other Unowns are separate from Unown A.
        if (i == 0)
            i = SPECIES_UNOWN;
        else
            i += SPECIES_UNOWN_B - 1;
        if (!isFrontPic)
            LZ77UnCompWram(gMonBackPicTable[i].data, dest);
        else
            LZ77UnCompWram(gMonFrontPicTable[i].data, dest);
    }
    else if (species > NUM_SPECIES) // is species unknown? draw the ? icon
    {
        LZ77UnCompWram(gMonFrontPicTable[0].data, dest);
    }
    else
    {
        LZ77UnCompWram(src->data, dest);
    }
    DrawSpindaSpots(species, personality, dest, isFrontPic);
}
