/*
 * blend_palette.c - Software Palette Color Blending
 *
 * ============================================================================
 * COLOR BLENDING ON THE GBA
 * ============================================================================
 *
 * The GBA has HARDWARE alpha blending (via REG_BLDCNT/BLDALPHA/BLDY), but
 * it only works between LAYERS (e.g., blend BG0 with BG1). It cannot blend
 * individual colors within a palette.
 *
 * This file implements SOFTWARE color blending: manually calculating new
 * colors by interpolating between an original color and a target color.
 * This is used for effects like:
 *   - Screen fades (blend all colors toward black or white)
 *   - Tinting (blend toward a color, like red for damage flash)
 *   - Day/night color shifts
 *   - Pokemon color changes (shiny palette, status effects)
 *
 * HOW COLOR BLENDING WORKS (Linear Interpolation):
 *
 *   result = original + ((target - original) * coefficient / 16)
 *
 * The coefficient ranges from 0 to 16:
 *   0  = 100% original color (no blend)
 *   8  = 50% mix of both colors
 *   16 = 100% target color (fully blended)
 *
 * This is done SEPARATELY for each of the R, G, B channels.
 * The division by 16 (>> 4) is used instead of a true percentage because
 * division is expensive on ARM7TDMI, but bit-shifting is free.
 *
 * DUAL PALETTE BUFFER SYSTEM:
 *   gPlttBufferUnfaded[]: The "original" colors (as loaded from ROM)
 *   gPlttBufferFaded[]:   The "current" colors (with blending applied)
 *
 *   TransferPlttBuffer() copies gPlttBufferFaded to hardware palette RAM
 *   during VBlank. This double-buffering lets us apply effects without
 *   losing the original colors.
 *
 * ============================================================================
 */

#include "global.h"
#include "blend_palette.h"
#include "palette.h"

/**
 * FUNCTION: BlendPalette
 *
 * PURPOSE: Blend a range of palette entries from their unfaded colors
 *          toward a target blend color.
 *
 * HOW IT WORKS:
 * For each palette entry in the range [palOffset, palOffset + numEntries):
 * 1. Read the original R, G, B values from gPlttBufferUnfaded
 * 2. Read the target R, G, B values from blendColor
 * 3. Interpolate each channel: result = original + ((target - original) * coeff / 16)
 * 4. Pack the result back into a 15-bit BGR555 color
 * 5. Store in gPlttBufferFaded (will be copied to hardware palette during VBlank)
 *
 * GBA CONTEXT:
 * Colors are stored in BGR555 format: 0BBBBBGGGGGRRRRR (15 bits per color).
 * Each channel (R, G, B) is 5 bits (0-31).
 * The PlttData struct provides named access to individual channels via bitfields.
 *
 * The final color is assembled with bit shifts:
 *   R in bits 0-4   (shift << 0)
 *   G in bits 5-9   (shift << 5)
 *   B in bits 10-14  (shift << 10)
 *
 * @param palOffset  — Starting palette index (0-511, where 0-255 = BG, 256-511 = OBJ)
 * @param numEntries — Number of consecutive palette entries to blend
 * @param coeff      — Blend strength (0 = no blend, 16 = fully blended to blendColor)
 * @param blendColor — Target color to blend toward (BGR555 format)
 */
void BlendPalette(u16 palOffset, u16 numEntries, u8 coeff, u16 blendColor)
{
    u16 i;
    for (i = 0; i < numEntries; i++)
    {
        u16 index = i + palOffset;

        /* Extract R, G, B from the ORIGINAL (unfaded) palette entry */
        struct PlttData *data1 = (struct PlttData *)&gPlttBufferUnfaded[index];
        s8 r = data1->r;  /* Red channel (0-31) */
        s8 g = data1->g;  /* Green channel (0-31) */
        s8 b = data1->b;  /* Blue channel (0-31) */

        /* Extract R, G, B from the TARGET blend color */
        struct PlttData *data2 = (struct PlttData *)&blendColor;

        /*
         * Interpolate each channel and pack into BGR555:
         *   channel_result = original + ((target - original) * coeff) / 16
         *
         * The >> 4 is division by 16 (bit shift is much faster than division).
         * When coeff=0:  result = original (no change)
         * When coeff=16: result = original + (target - original) = target
         */
        gPlttBufferFaded[index] = ((r + (((data2->r - r) * coeff) >> 4)) << 0)   /* Red:   bits 0-4 */
                                | ((g + (((data2->g - g) * coeff) >> 4)) << 5)   /* Green: bits 5-9 */
                                | ((b + (((data2->b - b) * coeff) >> 4)) << 10); /* Blue:  bits 10-14 */
    }
}

/**
 * FUNCTION: BlendPalettesAt
 *
 * PURPOSE: Blend a palette buffer in-place toward a target color.
 *
 * HOW IT WORKS:
 * Unlike BlendPalette which reads from gPlttBufferUnfaded and writes to
 * gPlttBufferFaded, this function modifies a palette buffer IN PLACE.
 * The caller provides the buffer pointer directly.
 *
 * Special case: If coefficient == 16 (100% blend), skips the math and
 * just fills every entry with the blend color directly. This is faster
 * than computing the interpolation when the result would be the target anyway.
 *
 * The blending math is identical to BlendPalette but operates on raw u16
 * values with manual bit extraction instead of using PlttData structs:
 *   R = (color >> 0)  & 0x1F  (bits 0-4)
 *   G = (color >> 5)  & 0x1F  (bits 5-9)
 *   B = (color >> 10) & 0x1F  (bits 10-14)
 *
 * @param palbuff     — Pointer to the palette buffer to modify
 * @param blend_pal   — Target blend color (BGR555)
 * @param coefficient — Blend strength (0-16)
 * @param size        — Number of palette entries to process
 */
void BlendPalettesAt(u16 * palbuff, u16 blend_pal, u32 coefficient, s32 size)
{
    if (coefficient == 16)
    {
        /* Full blend: just fill everything with the target color */
        while (--size != -1)
        {
            *palbuff++ = blend_pal;
        }
    }
    else
    {
        /* Extract target color channels once (they're the same for every entry) */
        u16 r = (blend_pal >>  0) & 0x1F;  /* Target red (0-31) */
        u16 g = (blend_pal >>  5) & 0x1F;  /* Target green (0-31) */
        u16 b = (blend_pal >> 10) & 0x1F;  /* Target blue (0-31) */

        while (--size != -1)
        {
            /* Extract current entry's channels */
            u16 r2 = (*palbuff >>  0) & 0x1F;
            u16 g2 = (*palbuff >>  5) & 0x1F;
            u16 b2 = (*palbuff >> 10) & 0x1F;

            /* Interpolate each channel and pack back into BGR555 */
            *palbuff++ = ((r2 + (((r - r2) * coefficient) >> 4)) <<  0)
                       | ((g2 + (((g - g2) * coefficient) >> 4)) <<  5)
                       | ((b2 + (((b - b2) * coefficient) >> 4)) << 10);
        }
    }
}
