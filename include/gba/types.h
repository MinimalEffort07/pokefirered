/*
 * types.h - GBA Fundamental Type Definitions
 *
 * ============================================================================
 * WHY CUSTOM TYPES?
 * ============================================================================
 *
 * The GBA CPU (ARM7TDMI) is a 32-bit processor, but communicates with
 * its memory and peripherals over buses of different widths:
 *   - IWRAM: 32-bit bus (fast, can read/write 32 bits at once)
 *   - EWRAM: 16-bit bus (needs 2 cycles for a 32-bit read)
 *   - VRAM/Palette/OAM: 16-bit bus
 *   - ROM: 16-bit bus
 *   - I/O Registers: mostly 16-bit or 32-bit
 *
 * Because hardware registers are specific widths (some 16-bit, some 32-bit),
 * we need types with EXACT known sizes. Standard C types like "int" can vary
 * between compilers and platforms. These typedefs guarantee exact sizes.
 *
 * The "u" prefix means unsigned (no negative numbers, full range for positive).
 * The "s" prefix means signed (can be negative, half the positive range).
 * The number is the bit width.
 *
 * ============================================================================
 * VOLATILE TYPES
 * ============================================================================
 *
 * The "v" prefix types (vu8, vu16, etc.) are VOLATILE versions.
 *
 * "volatile" is a C keyword that tells the compiler: "This variable can
 * change at ANY TIME, even if my code doesn't modify it." This prevents
 * the compiler from optimizing away reads/writes to it.
 *
 * WHY IS THIS CRITICAL FOR GBA?
 *   Memory-mapped I/O registers are just memory addresses that are connected
 *   to hardware circuits. Reading/writing these addresses controls the hardware.
 *
 *   Without volatile:
 *     // Compiler might optimize this to just "x = *(u16*)0x04000130"
 *     // because it thinks the value can't change between reads.
 *     x = *(u16*)0x04000130;  // Read button state
 *     y = *(u16*)0x04000130;  // Compiler might reuse x instead of re-reading!
 *
 *   With volatile:
 *     x = *(vu16*)0x04000130;  // Read button state - ALWAYS reads hardware
 *     y = *(vu16*)0x04000130;  // ALWAYS reads hardware again (buttons may change)
 *
 *   Similarly for writes:
 *     *(vu16*)0x04000000 = 0x0403;  // Write to DISPCNT - ALWAYS writes
 *     // Without volatile, compiler might skip this if it "knows" the
 *     // value hasn't changed since last write.
 *
 * RULE: Always use volatile types when accessing hardware registers.
 *       Use regular types for normal variables in RAM.
 *
 * ============================================================================
 * BOOL TYPES
 * ============================================================================
 *
 * The GBA has no native boolean type (pre-C99). These are custom:
 *   bool8  = u8  (1 byte) - most memory-efficient for storing TRUE/FALSE
 *   bool16 = u16 (2 bytes) - matches GBA's natural 16-bit bus width
 *   bool32 = u32 (4 bytes) - matches ARM's natural 32-bit register width
 *
 * Using different sizes matters for struct packing and alignment.
 * ARM7TDMI is most efficient with 32-bit aligned data.
 *
 * ============================================================================
 */

#ifndef GUARD_GBA_TYPES_H
#define GUARD_GBA_TYPES_H

#include <stdint.h>

/*
 * Unsigned integer types - can only hold non-negative values.
 *   u8:  0 to 255         (1 byte  = 8 bits)
 *   u16: 0 to 65,535      (2 bytes = 16 bits) - matches GBA register width
 *   u32: 0 to 4,294,967,295 (4 bytes = 32 bits) - matches ARM register width
 *   u64: 0 to ~18.4 quintillion (8 bytes = 64 bits) - rarely used on GBA
 */
typedef uint8_t   u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/*
 * Signed integer types - can hold negative and positive values.
 *   s8:  -128 to 127
 *   s16: -32,768 to 32,767 - used for coordinates, offsets
 *   s32: -2,147,483,648 to 2,147,483,647
 *   s64: very large range (rarely used on GBA)
 */
typedef int8_t    s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/*
 * Volatile versions of all integer types.
 * Used EXCLUSIVELY for hardware register access (memory-mapped I/O).
 * See the "VOLATILE TYPES" section above for why this matters.
 */
typedef volatile u8   vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8   vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

/*
 * Floating point types.
 * The ARM7TDMI has NO floating-point hardware (no FPU).
 * Any float/double operations are done in SOFTWARE, which is extremely slow.
 * GBA games almost never use floats - they use fixed-point math instead.
 * (Fixed-point: treat an integer as having an implicit decimal point,
 *  e.g., store 1.5 as 384 with 8 fractional bits: 384/256 = 1.5)
 */
typedef float  f32;
typedef double f64;

/*
 * Boolean types of various sizes.
 * TRUE = 1, FALSE = 0 (defined in defines.h).
 * Different sizes exist for struct alignment and bus-width matching.
 */
typedef u8  bool8;
typedef u16 bool16;
typedef u32 bool32;
typedef vu8  vbool8;
typedef vu16 vbool16;
typedef vu32 vbool32;

/*
 * BgCnt - Background Control Register Structure
 *
 * This is a bitfield struct that maps directly onto a BGxCNT register
 * (REG_BG0CNT through REG_BG3CNT, at 0x04000008-0x0400000E).
 *
 * Each BGxCNT register is 16 bits and controls one background layer.
 * Using a bitfield struct lets us access individual fields by name
 * instead of manual bit shifting:
 *
 *   bgCnt.priority = 2;     // instead of: reg = (reg & ~0x3) | 2;
 *   bgCnt.mosaic = 1;       // instead of: reg |= (1 << 6);
 *
 * BIT LAYOUT (16 bits total):
 *   Bits 0-1:   priority       - Drawing order (0=front, 3=back).
 *                                 Lower priority layers are drawn on top.
 *   Bits 2-3:   charBaseBlock  - Which 16KB block of VRAM holds the tileset.
 *                                 Block 0 = 0x06000000, Block 1 = 0x06004000, etc.
 *                                 The tileset is the collection of 8x8 pixel tile graphics.
 *   Bits 4-5:   dummy          - Unused padding bits.
 *   Bit 6:      mosaic         - Enable mosaic effect (pixelation/blockiness).
 *                                 Mosaic size is set in REG_MOSAIC (0x0400004C).
 *   Bit 7:      palettes       - Color depth mode:
 *                                 0 = 4bpp (4 bits per pixel), 16 palettes of 16 colors each
 *                                 1 = 8bpp (8 bits per pixel), 1 palette of 256 colors
 *                                 4bpp is more common - it uses half the VRAM but limits
 *                                 each tile to 16 colors (from one of 16 sub-palettes).
 *   Bits 8-12:  screenBaseBlock - Which 2KB block of VRAM holds the tilemap.
 *                                  The tilemap is the grid that says which tile goes where.
 *                                  Block 0 = 0x06000000, Block 31 = 0x0600F800.
 *   Bit 13:     areaOverflowMode - Wrap mode (affine BGs only):
 *                                   0 = transparent outside map area
 *                                   1 = wrap around (seamless tiling)
 *   Bits 14-15: screenSize     - Map dimensions:
 *                                 Text mode: 0=256x256, 1=512x256, 2=256x512, 3=512x512
 *                                 Affine mode: 0=128x128, 1=256x256, 2=512x512, 3=1024x1024
 */
struct BgCnt
{
    u16 priority:2;
    u16 charBaseBlock:2;
    u16 dummy:2;
    u16 mosaic:1;
    u16 palettes:1;
    u16 screenBaseBlock:5;
    u16 areaOverflowMode:1;
    u16 screenSize:2;
};

/*
 * vBgCnt - Volatile version for direct register access.
 * Use this when reading/writing BGxCNT registers directly:
 *   vBgCnt *reg = (vBgCnt *)REG_ADDR_BG0CNT;
 *   reg->priority = 1;  // This write goes directly to hardware
 */
typedef volatile struct BgCnt vBgCnt;

/*
 * PlttData - Palette Color Entry Structure
 *
 * The GBA uses 15-bit BGR555 color format (NOT the RGB you might expect).
 * Each color is 16 bits but only 15 bits are used:
 *
 *   Bit layout: 0BBBBBGGGGGRRRRR
 *     Bits 0-4:   Red   (0-31, where 31 is maximum red)
 *     Bits 5-9:   Green (0-31)
 *     Bits 10-14: Blue  (0-31)
 *     Bit 15:     Unused (always 0)
 *
 * So pure red = 0x001F, pure green = 0x03E0, pure blue = 0x7C00.
 * White = 0x7FFF (all channels max), Black = 0x0000.
 *
 * The GBA palette RAM (0x05000000) stores these entries:
 *   First 256 entries: Background palettes (16 sub-palettes of 16 colors in 4bpp mode)
 *   Next 256 entries: Sprite/OBJ palettes (same layout)
 */
struct PlttData
{
    u16 r:5; // red   (0-31)
    u16 g:5; // green (0-31)
    u16 b:5; // blue  (0-31)
    u16 unused_15:1;
};

/*
 * OamData - Object Attribute Memory Entry Structure
 *
 * This is THE fundamental sprite data structure. Each of the 128 hardware
 * sprites has one OamData entry in OAM (0x07000000).
 *
 * The structure maps directly to the hardware's 8-byte OAM entry format.
 * The fields are documented in detail in the header comments of sprite.c.
 *
 * OAM is organized as 128 entries of 8 bytes each (1024 bytes total).
 * However, the last 2 bytes of each entry (affineParam) are actually
 * part of a SEPARATE system: the 32 affine matrices.
 *
 * AFFINE MATRICES: The GBA can rotate/scale sprites using 2x2 transformation
 * matrices. There are 32 such matrices, and their parameters are interleaved
 * into the OAM entries at every 4th entry's affineParam field.
 * Matrix N uses: OAM[N*4].affineParam (pa), OAM[N*4+1].affineParam (pb),
 *                OAM[N*4+2].affineParam (pc), OAM[N*4+3].affineParam (pd).
 */
struct OamData
{
    /*
     * Attribute 0 (first 16 bits)
     * Contains: Y position, rendering mode, shape
     */
    /*0x00*/ u32 y:8;          /* Y coordinate on screen (0-255, wraps) */
    /*0x01*/ u32 affineMode:2; /* 0=no affine, 1=affine, 2=hidden, 3=affine+double area */
             u32 objMode:2;    /* 0=normal, 1=semi-transparent, 2=obj window */
             u32 mosaic:1;     /* 1=enable mosaic effect */
             u32 bpp:1;        /* 0=4bpp (16 colors), 1=8bpp (256 colors) */
             u32 shape:2;      /* 0=square, 1=horizontal rect, 2=vertical rect */

    /*
     * Attribute 1 (next 16 bits)
     * Contains: X position, flip flags or affine matrix index, size
     */
    /*0x02*/ u32 x:9;          /* X coordinate on screen (0-511, wraps) */
             u32 matrixNum:5;  /* If affine: which of 32 affine matrices to use.
                                * If NOT affine: bits 3-4 are H-flip and V-flip flags */
             u32 size:2;       /* Size selector (0-3), combined with shape for pixel dims */

    /*
     * Attribute 2 (next 16 bits)
     * Contains: tile index, priority, palette
     */
    /*0x04*/ u16 tileNum:10;   /* Which 8x8 tile from OBJ VRAM (0-1023) */
             u16 priority:2;   /* Priority vs BG layers (0=on top, 3=behind all BGs) */
             u16 paletteNum:4; /* Which of 16 OBJ palettes (in 4bpp mode) */

    /*
     * Affine parameter (last 16 bits)
     * This is NOT a sprite attribute - it's part of the affine matrix system.
     * Every 4 OAM entries contribute one parameter (pa, pb, pc, pd) to form
     * a 2x2 rotation/scaling matrix.
     */
    /*0x06*/ u16 affineParam;
};

/*
 * OAM attribute constants for the matrixNum field when affine is OFF.
 * When affineMode is 0 (no affine), the matrixNum field is repurposed:
 *   Bit 3 = horizontal flip
 *   Bit 4 = vertical flip
 */
#define ST_OAM_HFLIP     0x08  /* Flip sprite horizontally */
#define ST_OAM_VFLIP     0x10  /* Flip sprite vertically */
#define ST_OAM_MNUM_FLIP_MASK 0x18  /* Mask for both flip bits */

/* OBJ rendering modes (objMode field) */
#define ST_OAM_OBJ_NORMAL 0  /* Normal rendering */
#define ST_OAM_OBJ_BLEND  1  /* Semi-transparent (alpha blending with layers behind) */
#define ST_OAM_OBJ_WINDOW 2  /* OBJ window (sprite shape masks other layers) */

/* Affine mode values (affineMode field) */
#define ST_OAM_AFFINE_OFF    0  /* No rotation/scaling, sprite drawn normally */
#define ST_OAM_AFFINE_NORMAL 1  /* Affine transformation active */
#define ST_OAM_AFFINE_ERASE  2  /* Sprite is hidden (not rendered at all) */
#define ST_OAM_AFFINE_DOUBLE 3  /* Affine active with double-size render area
                                 * (prevents clipping when sprite rotates/scales) */

#define ST_OAM_AFFINE_ON_MASK     1  /* Bit 0: is affine on? */
#define ST_OAM_AFFINE_DOUBLE_MASK 2  /* Bit 1: is double-size on? */

/* Color depth modes (bpp field) */
#define ST_OAM_4BPP 0  /* 4 bits per pixel = 16 colors per palette, uses less VRAM */
#define ST_OAM_8BPP 1  /* 8 bits per pixel = 256 colors, uses more VRAM */

/* Sprite shape values (shape field) */
#define ST_OAM_SQUARE      0  /* Square sprite (8x8, 16x16, 32x32, or 64x64) */
#define ST_OAM_H_RECTANGLE 1  /* Wider than tall (16x8, 32x8, 32x16, or 64x32) */
#define ST_OAM_V_RECTANGLE 2  /* Taller than wide (8x16, 8x32, 16x32, or 32x64) */

/* Sprite size values (size field) - combined with shape to determine pixel dimensions */
#define ST_OAM_SIZE_0   0  /* Smallest size for each shape */
#define ST_OAM_SIZE_1   1  /* Second smallest */
#define ST_OAM_SIZE_2   2  /* Second largest */
#define ST_OAM_SIZE_3   3  /* Largest size for each shape */

/*
 * SPRITE SIZE LOOKUP TABLE
 *
 * The GBA determines sprite pixel dimensions from the combination of
 * shape (2 bits) and size (2 bits). This gives 12 possible combinations:
 *
 *              Size 0    Size 1    Size 2    Size 3
 * Square:      8x8       16x16     32x32     64x64
 * H-Rect:      16x8      32x8      32x16     64x32
 * V-Rect:      8x16      8x32      16x32     32x64
 *
 * These macros encode both size and shape into a single value:
 *   Upper 2 bits = size, Lower 2 bits = shape
 * This lets functions accept a single "sprite size" parameter.
 */
#define SPRITE_SIZE_8x8     ((ST_OAM_SIZE_0 << 2) | (ST_OAM_SQUARE))
#define SPRITE_SIZE_16x16   ((ST_OAM_SIZE_1 << 2) | (ST_OAM_SQUARE))
#define SPRITE_SIZE_32x32   ((ST_OAM_SIZE_2 << 2) | (ST_OAM_SQUARE))
#define SPRITE_SIZE_64x64   ((ST_OAM_SIZE_3 << 2) | (ST_OAM_SQUARE))

#define SPRITE_SIZE_16x8    ((ST_OAM_SIZE_0 << 2) | (ST_OAM_H_RECTANGLE))
#define SPRITE_SIZE_32x8    ((ST_OAM_SIZE_1 << 2) | (ST_OAM_H_RECTANGLE))
#define SPRITE_SIZE_32x16   ((ST_OAM_SIZE_2 << 2) | (ST_OAM_H_RECTANGLE))
#define SPRITE_SIZE_64x32   ((ST_OAM_SIZE_3 << 2) | (ST_OAM_H_RECTANGLE))

#define SPRITE_SIZE_8x16    ((ST_OAM_SIZE_0 << 2) | (ST_OAM_V_RECTANGLE))
#define SPRITE_SIZE_8x32    ((ST_OAM_SIZE_1 << 2) | (ST_OAM_V_RECTANGLE))
#define SPRITE_SIZE_16x32   ((ST_OAM_SIZE_2 << 2) | (ST_OAM_V_RECTANGLE))
#define SPRITE_SIZE_32x64   ((ST_OAM_SIZE_3 << 2) | (ST_OAM_V_RECTANGLE))

/*
 * Helper macros to extract size and shape from a combined SPRITE_SIZE_* value.
 *   SPRITE_SIZE(16x16)  -> 1 (ST_OAM_SIZE_1)
 *   SPRITE_SHAPE(16x16) -> 0 (ST_OAM_SQUARE)
 */
#define SPRITE_SIZE(dim)  ((SPRITE_SIZE_##dim >> 2) & 0x03)
#define SPRITE_SHAPE(dim) (SPRITE_SIZE_##dim & 0x03)

/*
 * BgAffineSrcData - Input parameters for background affine transformation.
 *
 * AFFINE BACKGROUNDS (Mode 1 and 2) can be rotated and scaled.
 * Unlike text-mode BGs which are just scrollable tile grids, affine BGs
 * can be freely rotated, scaled, and skewed using a 2x2 matrix.
 *
 * This structure provides the HUMAN-FRIENDLY parameters that get
 * converted into the raw matrix values by the BIOS call BgAffineSet().
 *
 * texX/texY: The "center of rotation" point on the background texture.
 *            In 8.8 fixed-point format (8 integer bits, 8 fractional bits).
 * scrX/scrY: Where that center point appears on the screen.
 * sx/sy:     Scale factors. 0x100 = 1.0 (no scaling). 0x80 = 2x zoom in.
 * alpha:     Rotation angle (0-65535 maps to 0-360 degrees).
 */
struct BgAffineSrcData
{
    s32 texX;   /* X center on texture (8.8 fixed-point) */
    s32 texY;   /* Y center on texture (8.8 fixed-point) */
    s16 scrX;   /* X center on screen */
    s16 scrY;   /* Y center on screen */
    s16 sx;     /* X scale (0x100 = 1.0, 0x200 = 0.5x, 0x80 = 2x) */
    s16 sy;     /* Y scale */
    u16 alpha;  /* Rotation angle (0-0xFFFF = 0-360 degrees) */
};

/*
 * BgAffineDstData - Output matrix for background affine transformation.
 *
 * This is the RAW 2x2 transformation matrix + displacement vector
 * that the GPU actually uses. Generated by BgAffineSet() from BgAffineSrcData.
 *
 * pa/pb/pc/pd form a 2x2 rotation+scale matrix:
 *   | pa  pb |   For no transformation (identity): pa=0x100, pb=0, pc=0, pd=0x100
 *   | pc  pd |   For 2x zoom: pa=0x80, pb=0, pc=0, pd=0x80
 *
 * dx/dy are the displacement (where pixel 0,0 of the BG maps to on screen).
 * These are 19.8 fixed-point values.
 *
 * Written to REG_BG2PA-PD + REG_BG2X/Y (or BG3 equivalents).
 */
struct BgAffineDstData
{
    s16 pa;  /* Rotation/scale matrix element (cos * sx) */
    s16 pb;  /* Rotation/scale matrix element (-sin * sx) */
    s16 pc;  /* Rotation/scale matrix element (sin * sy) */
    s16 pd;  /* Rotation/scale matrix element (cos * sy) */
    s32 dx;  /* X displacement (19.8 fixed-point) */
    s32 dy;  /* Y displacement (19.8 fixed-point) */
};

/*
 * ObjAffineSrcData - Input parameters for sprite affine transformation.
 *
 * Similar to BgAffineSrcData but simpler (sprites don't need displacement).
 * Used with the BIOS call ObjAffineSet() to generate the 2x2 matrix
 * that gets written into the OAM affine parameter slots.
 */
struct ObjAffineSrcData
{
    s16 xScale;   /* X scale factor (0x100 = 1.0) */
    s16 yScale;   /* Y scale factor (0x100 = 1.0) */
    u16 rotation; /* Rotation angle (0-0xFFFF = 0-360 degrees) */
};

/*
 * SioMultiCnt - Serial I/O Multi-Player Control Register Structure
 *
 * Maps to REG_SIOCNT (0x04000128) when in Multi-Player mode.
 * This register controls the GBA's link cable serial communication.
 *
 * Multi-Player mode allows up to 4 GBAs to exchange data simultaneously.
 * One GBA is the "master" (ID=0), others are "slaves" (ID=1,2,3).
 * The master initiates all transfers by setting the enable bit.
 *
 * Each transfer exchanges one 16-bit value from each connected GBA.
 * After transfer, each GBA can read all 4 values from REG_SIOMULTI0-3.
 */
struct SioMultiCnt
{
    u16 baudRate:2;    /* Communication speed:
                        *   0 = 9600 bps, 1 = 38400 bps,
                        *   2 = 57600 bps, 3 = 115200 bps
                        * Higher = faster but more error-prone */
    u16 si:1;          /* SI terminal state (read-only) - indicates connection status */
    u16 sd:1;          /* SD terminal state (read-only) */
    u16 id:2;          /* This GBA's player ID (0-3, read-only).
                        * 0 = master (controls transfer timing) */
    u16 error:1;       /* Error flag - set if transfer failed (read-only) */
    u16 enable:1;      /* Start transfer. Master sets this to 1 to begin.
                        * Automatically cleared when transfer completes. */
    u16 unused_11_8:4; /* Unused bits */
    u16 mode:2;        /* Communication mode. Must be 2 for multi-player mode. */
    u16 intrEnable:1;  /* If 1, fire a Serial interrupt when transfer completes */
    u16 unused_15:1;   /* Unused */
    u16 data;          /* Data to send in next transfer (write) / received data (read) */
};

#define ST_SIO_MULTI_MODE 2 /* Multi-player communication mode identifier */

/* Serial I/O baud rate settings */
#define ST_SIO_9600_BPS   0 /*   9,600 bits per second - slowest, most reliable */
#define ST_SIO_38400_BPS  1 /*  38,400 bps */
#define ST_SIO_57600_BPS  2 /*  57,600 bps */
#define ST_SIO_115200_BPS 3 /* 115,200 bps - fastest, used by Pokemon link */

#endif // GUARD_GBA_TYPES_H
