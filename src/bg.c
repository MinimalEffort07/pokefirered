/*
 * bg.c - Background Layer Management
 *
 * ============================================================================
 * GBA BACKGROUND SYSTEM OVERVIEW
 * ============================================================================
 *
 * The GBA can display up to 4 background (BG) layers simultaneously.
 * In Mode 0 (used by Pokemon FireRed), all 4 are "text mode" layers.
 * Each layer is rendered independently and composited by the GPU based
 * on priority values.
 *
 * HOW A BACKGROUND LAYER WORKS:
 *
 *   A BG layer is like a virtual canvas made of tiles. The GPU reads two
 *   things from VRAM to render it:
 *
 *   1. TILESET (Character Base): A collection of 8x8 pixel tiles.
 *      Think of this as a "font" - each tile is one character.
 *      In 4bpp mode: each tile is 32 bytes (4 bits per pixel, 64 pixels).
 *      In 8bpp mode: each tile is 64 bytes (8 bits per pixel).
 *      Stored in VRAM at BG_CHAR_ADDR(n), where n=0..3 selects the
 *      16 KB "character base block".
 *
 *   2. TILEMAP (Screen Base): A grid of 16-bit entries, one per tile position.
 *      Each entry specifies WHICH tile to draw at that position:
 *        Bits 0-9:   Tile index (0-1023, selects which tile from the tileset)
 *        Bit 10:     Horizontal flip
 *        Bit 11:     Vertical flip
 *        Bits 12-15: Palette number (in 4bpp mode, selects one of 16 palettes)
 *      Stored in VRAM at BG_SCREEN_ADDR(n), where n=0..31 selects the
 *      2 KB "screen base block".
 *
 *   The GPU composites the visible portion by reading the tilemap,
 *   looking up each tile's graphics from the tileset, and rendering
 *   the 8x8 pixel blocks to the screen.
 *
 * SCREEN SIZE:
 *   The visible screen is 240x160 pixels = 30x20 tiles.
 *   But tilemaps can be larger (up to 64x64 tiles = 512x512 pixels).
 *   The BG scroll registers (HOFS/VOFS) control which portion is visible.
 *   This is how the overworld scrolls - the tilemap is bigger than the
 *   screen, and the camera moves the scroll position.
 *
 *   Text mode screen sizes (set in BGxCNT bits 14-15):
 *     0: 256x256  (32x32 tiles)  = 1 screen block  (2 KB)
 *     1: 512x256  (64x32 tiles)  = 2 screen blocks (4 KB)
 *     2: 256x512  (32x64 tiles)  = 2 screen blocks (4 KB)
 *     3: 512x512  (64x64 tiles)  = 4 screen blocks (8 KB)
 *
 * AFFINE BACKGROUNDS (Mode 1/2 only):
 *   BG2 and BG3 can be "affine" - supporting rotation and scaling.
 *   Affine BGs use 8-bit tilemap entries (just a tile index, no flip/palette).
 *   The GPU applies a 2x2 transformation matrix (PA,PB,PC,PD) to compute
 *   which source pixel maps to each screen pixel. This enables:
 *   - Rotation (spinning the background)
 *   - Scaling (zooming in/out)
 *   - Shearing
 *   Used in Pokemon for some battle transitions and the world map.
 *
 * PALETTE MODES:
 *   4bpp (16 colors): Each pixel is 4 bits. 16 palettes of 16 colors each.
 *     Tilemap entry bits 12-15 select which palette. Compact but limited colors.
 *     This is the standard mode for most game graphics.
 *   8bpp (256 colors): Each pixel is 8 bits. One palette of 256 colors.
 *     Uses twice the VRAM but allows more colors per tile.
 *     Used for Oak's speech trainer portraits in this game.
 *
 * HOW POKEMON FIRERED USES BG LAYERS:
 *
 *   Overworld:
 *     BG0: Text/UI layer (menus, textboxes, HUD)
 *     BG1: Upper map layer (treetops, building roofs - above player)
 *     BG2: Lower map layer (ground, paths, water - below player)
 *     BG3: Not typically used in overworld
 *
 *   Battle:
 *     BG0: Battle UI (HP bars, text)
 *     BG1: Battle platform / effects
 *     BG2: Battle background
 *     BG3: Additional effects
 *
 *   Sprites (OBJ layer) render between/above BG layers based on priority.
 *   The player character, NPCs, and Pokemon are all sprites, NOT BG tiles.
 *
 * ============================================================================
 * VRAM LAYOUT
 * ============================================================================
 *
 * BG VRAM (0x06000000 - 0x0600FFFF, 64 KB):
 *   Divided into 4 "character base blocks" of 16 KB each:
 *     Block 0: 0x06000000 (BG_CHAR_ADDR(0))
 *     Block 1: 0x06004000 (BG_CHAR_ADDR(1))
 *     Block 2: 0x06008000 (BG_CHAR_ADDR(2))
 *     Block 3: 0x0600C000 (BG_CHAR_ADDR(3))
 *
 *   Also divided into 32 "screen base blocks" of 2 KB each:
 *     Block 0:  0x06000000 (BG_SCREEN_ADDR(0))
 *     Block 1:  0x06000800 (BG_SCREEN_ADDR(1))
 *     ...
 *     Block 31: 0x0600F800 (BG_SCREEN_ADDR(31))
 *
 *   Character blocks and screen blocks OVERLAP in memory!
 *   The game must carefully choose base indices so tile graphics
 *   and tilemaps don't overwrite each other.
 *
 * OBJ VRAM (0x06010000 - 0x06017FFF, 32 KB):
 *   Sprite tile graphics. Separate from BG VRAM.
 *
 * ============================================================================
 * THIS MODULE'S ARCHITECTURE
 * ============================================================================
 *
 * This module provides a SOFTWARE abstraction layer over the BG hardware.
 * It maintains two sets of state:
 *
 *   sGpuBgConfigs (BgControl): Configuration for each BG layer.
 *     Mirrors the BGxCNT register fields (priority, char base, map base,
 *     screen size, palette mode, mosaic, wraparound).
 *     ShowBgInternal() packs these into the actual hardware register.
 *
 *   sGpuBgConfigs2 (BgConfig2): Additional per-BG state.
 *     - tilemap: Pointer to the RAM tilemap buffer. The game edits this
 *       buffer in RAM, then copies it to VRAM via DMA.
 *     - bg_x / bg_y: Scroll position (fixed-point, 8 fractional bits).
 *     - baseTile: Starting tile index for this BG's graphics.
 *     - basePalette: Starting palette for this BG.
 *
 * The double-buffer pattern:
 *   1. Game code modifies the RAM tilemap (sGpuBgConfigs2[bg].tilemap)
 *   2. CopyBgTilemapBufferToVram() queues a DMA copy to VRAM
 *   3. During VBlank, ProcessDma3Requests() executes the copy
 *   4. GPU renders from the updated VRAM next frame
 *
 * This prevents screen tearing (GPU reading tilemap while CPU writes it).
 * ============================================================================
 */

#include <limits.h>
#include "global.h"
#include "bg.h"
#include "dma3.h"
#include "gpu_regs.h"

/*
 * Mask for the BG enable bits (bits 8-11) and mode bits (bits 0-2)
 * in REG_DISPCNT. Used to modify only BG visibility/mode without
 * touching sprite enable, window enable, or other DISPCNT settings.
 */
#define DISPCNT_ALL_BG_AND_MODE_BITS    (DISPCNT_BG_ALL_ON | 0x7)

/*
 * BgControl: Software mirror of BGxCNT register for all 4 BG layers.
 *
 * struct BgConfig maps to the hardware BGxCNT register layout:
 *   visible: Whether this BG has been configured (not a hardware bit)
 *   screenSize: 0-3, maps to 256x256 through 512x512
 *   priority: 0-3, lower = drawn on top (0 = highest priority)
 *   mosaic: Enable pixel mosaic effect (blocky pixelation)
 *   wraparound: Affine mode only - wrap tilemap at edges
 *   charBaseIndex: 0-3, which 16 KB block in VRAM has tile graphics
 *   mapBaseIndex: 0-31, which 2 KB block in VRAM has the tilemap
 *   paletteMode: 0 = 4bpp (16 palettes × 16 colors), 1 = 8bpp (256 colors)
 *
 * bgVisibilityAndMode: Tracks which BGs are shown and the current BG mode.
 *   Low 3 bits = BG mode (0-2)
 *   Bits 8-11 = BG0-3 enable flags
 *   This mirrors the relevant bits of REG_DISPCNT.
 */
struct BgControl
{
    struct BgConfig {
        u16 visible:1;
        u16 unknown_1:1;
        u16 screenSize:2;
        u16 priority:2;
        u16 mosaic:1;
        u16 wraparound:1;

        u16 charBaseIndex:2;
        u16 mapBaseIndex:5;
        u16 paletteMode:1;

        u8 unknown_2;
        u8 unknown_3;
    } configs[4];

    u16 bgVisibilityAndMode;
};

/*
 * BgConfig2: Extended per-BG state not represented in hardware registers.
 *
 * baseTile: Starting tile index within the character base block.
 *   Multiple BGs can share a character base block if they use
 *   different tile ranges. baseTile offsets where tile 0 starts.
 *
 * tilemap: Pointer to the RAM-based tilemap buffer.
 *   The game allocates a buffer in EWRAM and edits it freely.
 *   CopyBgTilemapBufferToVram() DMA-copies this to VRAM.
 *   NULL means no tilemap buffer is assigned.
 *
 * bg_x / bg_y: Scroll position in 8.8 fixed-point format.
 *   The upper 8 bits are the pixel offset written to BGxHOFS/VOFS.
 *   The lower 8 bits are sub-pixel fractional bits for smooth scrolling.
 *   Example: bg_x = 0x0A80 means X scroll = 0x0A pixels + 0x80/256 sub-pixel.
 */
struct BgConfig2
{
    u32 baseTile:10;
    u32 basePalette:4;
    u32 unk_3:18;

    void *tilemap;
    u32 bg_x;
    u32 bg_y;
};

static struct BgControl sGpuBgConfigs;
static struct BgConfig2 sGpuBgConfigs2[4];

/*
 * sDmaBusyBitfield: Tracks which DMA3 request slots are in use for BG copies.
 * When a BG tile/tilemap load is queued via RequestDma3Copy(), the request
 * index is recorded here. IsDma3ManagerBusyWithBgCopy() checks these bits
 * to determine if all pending BG copies are complete.
 * 4 u32 entries = 128 bits = tracks up to 128 concurrent DMA requests.
 */
static u32 sDmaBusyBitfield[4];

/*
 * gpu_tile_allocation_map_bg: Bitmap tracking which BG tile slots are in use.
 * Each bit represents one 32-byte tile slot (4bpp).
 * 0x100 bytes = 2048 bits = 2048 tile slots.
 * Used by the window tile auto-allocation system to find free tile space
 * for dynamically created UI elements (text windows, menus).
 *
 * This is essentially a memory allocator for VRAM tile space.
 */
static u8 gpu_tile_allocation_map_bg[0x100];

COMMON_DATA bool32 gWindowTileAutoAllocEnabled = 0;

static const struct BgConfig sZeroedBgControlStruct = { 0 };

/*
 * ResetBgs - Full reset of all BG state.
 * Zeros all BG configurations and hides all BG layers.
 * Called at startup and during major state transitions
 * (entering/exiting battle, opening menus, etc.)
 */
void ResetBgs(void)
{
    ResetBgControlStructs();
    sGpuBgConfigs.bgVisibilityAndMode = 0;
    SetTextModeAndHideBgs();
}

/*
 * SetBgModeInternal - Change the BG mode (0, 1, or 2).
 *
 * BG Mode determines which layers are text vs affine:
 *   Mode 0: BG0-3 all text (the standard mode for Pokemon)
 *   Mode 1: BG0-1 text, BG2 affine (rotation/scaling)
 *   Mode 2: BG2-3 affine (BG0-1 unavailable)
 *
 * Only modifies the low 3 bits of bgVisibilityAndMode,
 * preserving the BG enable flags in bits 8-11.
 */
void SetBgModeInternal(u8 bgMode)
{
    sGpuBgConfigs.bgVisibilityAndMode &= 0xFFF8;
    sGpuBgConfigs.bgVisibilityAndMode |= bgMode;
}

u8 GetBgMode(void)
{
    return sGpuBgConfigs.bgVisibilityAndMode & 0x7;
}

void ResetBgControlStructs(void)
{
    struct BgConfig* bgConfigs = &sGpuBgConfigs.configs[0];
    struct BgConfig zeroedConfig = sZeroedBgControlStruct;
    int i;

    for (i = 0; i < 4; i++)
    {
        bgConfigs[i] = zeroedConfig;
    }
}

void Unused_ResetBgControlStruct(u8 bg)
{
    if (IsInvalidBg(bg) == FALSE)
    {
        sGpuBgConfigs.configs[bg] = sZeroedBgControlStruct;
    }
}

/*
 * SetBgControlAttributes - Configure one BG layer's hardware attributes.
 *
 * Each parameter maps to a field in the BGxCNT register.
 * A value of 0xFF means "don't change this attribute" - this allows
 * modifying individual fields without affecting others.
 *
 * After setting attributes, visible is set to TRUE, indicating this
 * BG has been configured and can be shown.
 *
 * Note: This only updates the SOFTWARE state. The actual hardware register
 * is written by ShowBgInternal() when the BG is made visible.
 */
void SetBgControlAttributes(u8 bg, u8 charBaseIndex, u8 mapBaseIndex, u8 screenSize, u8 paletteMode, u8 priority, u8 mosaic, u8 wraparound)
{
    if (IsInvalidBg(bg) == FALSE)
    {
        if (charBaseIndex != 0xFF)
        {
            sGpuBgConfigs.configs[bg].charBaseIndex = charBaseIndex & 0x3;
        }

        if (mapBaseIndex != 0xFF)
        {
            sGpuBgConfigs.configs[bg].mapBaseIndex = mapBaseIndex & 0x1F;
        }

        if (screenSize != 0xFF)
        {
            sGpuBgConfigs.configs[bg].screenSize = screenSize & 0x3;
        }

        if (paletteMode != 0xFF)
        {
            sGpuBgConfigs.configs[bg].paletteMode = paletteMode;
        }

        if (priority != 0xFF)
        {
            sGpuBgConfigs.configs[bg].priority = priority & 0x3;
        }

        if (mosaic != 0xFF)
        {
            sGpuBgConfigs.configs[bg].mosaic = mosaic & 0x1;
        }

        if (wraparound != 0xFF)
        {
            sGpuBgConfigs.configs[bg].wraparound = wraparound;
        }

        sGpuBgConfigs.configs[bg].unknown_2 = 0;
        sGpuBgConfigs.configs[bg].unknown_3 = 0;

        sGpuBgConfigs.configs[bg].visible = 1;
    }
}

/*
 * GetBgControlAttribute - Read one attribute from the software BG config.
 * Returns 0xFF if the BG is invalid or not visible.
 */
u16 GetBgControlAttribute(u8 bg, u8 attributeId)
{
    if (IsInvalidBg(bg) == FALSE && sGpuBgConfigs.configs[bg].visible != FALSE)
    {
        switch (attributeId)
        {
            case BG_CTRL_ATTR_VISIBLE:
                return sGpuBgConfigs.configs[bg].visible;
            case BG_CTRL_ATTR_CHARBASEINDEX:
                return sGpuBgConfigs.configs[bg].charBaseIndex;
            case BG_CTRL_ATTR_MAPBASEINDEX:
                return sGpuBgConfigs.configs[bg].mapBaseIndex;
            case BG_CTRL_ATTR_SCREENSIZE:
                return sGpuBgConfigs.configs[bg].screenSize;
            case BG_CTRL_ATTR_PALETTEMODE:
                return sGpuBgConfigs.configs[bg].paletteMode;
            case BG_CTRL_ATTR_PRIORITY:
                return sGpuBgConfigs.configs[bg].priority;
            case BG_CTRL_ATTR_MOSAIC:
                return sGpuBgConfigs.configs[bg].mosaic;
            case BG_CTRL_ATTR_WRAPAROUND:
                return sGpuBgConfigs.configs[bg].wraparound;
        }
    }

    return 0xFF;
}

/*
 * LoadBgVram - Queue a DMA copy of tile or tilemap data to BG VRAM.
 *
 * This doesn't copy immediately - it queues a DMA3 request that
 * executes during VBlank (via ProcessDma3Requests in VBlankIntr).
 *
 * mode parameter:
 *   1 = Copy to character (tile graphics) area.
 *       Destination = BG_CHAR_ADDR(charBaseIndex) + destOffset
 *   2 = Copy to screen (tilemap) area.
 *       Destination = BG_SCREEN_ADDR(mapBaseIndex) + destOffset
 *
 * Returns the DMA request slot index, or -1 on failure.
 *
 * The request goes through RequestDma3Copy() which adds it to a queue.
 * DMA3 is the general-purpose DMA channel used for VRAM transfers.
 * DMA3_16BIT means 16-bit transfer mode (matches VRAM's 16-bit bus).
 *
 * Why DMA instead of CPU copy? DMA is much faster for large transfers:
 *   CPU copy: ~8 cycles per halfword (read + write + loop overhead)
 *   DMA copy: ~2 cycles per halfword (hardware-driven, no CPU involvement)
 * For a 2 KB tilemap, DMA takes ~2048 cycles vs CPU's ~8192 cycles.
 */
u8 LoadBgVram(u8 bg, const void *src, u16 size, u16 destOffset, u8 mode)
{
    u16 offset;
    s8 cursor;

    if (IsInvalidBg(bg) == FALSE && sGpuBgConfigs.configs[bg].visible != FALSE)
    {
        switch (mode)
        {
            case 0x1:
                offset = sGpuBgConfigs.configs[bg].charBaseIndex * BG_CHAR_SIZE;
                break;
            case 0x2:
                offset = sGpuBgConfigs.configs[bg].mapBaseIndex * BG_SCREEN_SIZE;
                break;
            default:
                cursor = -1;
                goto end;
        }

        offset = destOffset + offset;

        cursor = RequestDma3Copy(src, (void *)(offset + BG_VRAM), size, DMA3_16BIT);

        if (cursor == -1)
        {
            return -1;
        }
    }
    else
    {
       return -1;
    }

end:
    return cursor;
}

/*
 * ShowBgInternal - Pack BG config into hardware register and enable the layer.
 *
 * This is where the software config becomes REAL hardware state.
 * It packs all the BgConfig fields into a single 16-bit value
 * matching the BGxCNT register format:
 *
 *   Bits 0-1:   Priority
 *   Bits 2-3:   Character base block
 *   Bit 6:      Mosaic
 *   Bit 7:      Palette mode (4bpp/8bpp)
 *   Bits 8-12:  Screen base block
 *   Bit 13:     Wraparound (affine only)
 *   Bits 14-15: Screen size
 *
 * The register offset is computed as (bg * 2) + 0x08:
 *   BG0CNT = 0x08, BG1CNT = 0x0A, BG2CNT = 0x0C, BG3CNT = 0x0E
 *
 * Also sets the BG's enable bit in bgVisibilityAndMode (mirroring DISPCNT).
 */
void ShowBgInternal(u8 bg)
{
    u16 value;
    if (IsInvalidBg(bg) == FALSE && sGpuBgConfigs.configs[bg].visible != FALSE)
    {
        value = sGpuBgConfigs.configs[bg].priority |
                (sGpuBgConfigs.configs[bg].charBaseIndex << 2) |
                (sGpuBgConfigs.configs[bg].mosaic << 6) |
                (sGpuBgConfigs.configs[bg].paletteMode << 7) |
                (sGpuBgConfigs.configs[bg].mapBaseIndex << 8) |
                (sGpuBgConfigs.configs[bg].wraparound << 13) |
                (sGpuBgConfigs.configs[bg].screenSize << 14);

        SetGpuReg((bg << 1) + 0x8, value);

        sGpuBgConfigs.bgVisibilityAndMode |= 1 << (bg + 8);
        sGpuBgConfigs.bgVisibilityAndMode &= DISPCNT_ALL_BG_AND_MODE_BITS;
    }
}

/*
 * HideBgInternal - Clear a BG's enable bit (but don't touch its config).
 * The BG data stays in VRAM - it's just not rendered anymore.
 */
static void HideBgInternal(u8 bg)
{
    if (IsInvalidBg(bg) == FALSE)
    {
        sGpuBgConfigs.bgVisibilityAndMode &= ~(1 << (bg + 8));
        sGpuBgConfigs.bgVisibilityAndMode &= DISPCNT_ALL_BG_AND_MODE_BITS;
    }
}

/*
 * SyncBgVisibilityAndMode - Write the BG mode and visibility bits to DISPCNT.
 *
 * REG_DISPCNT is the master display control register. This function
 * modifies only the BG-related bits (mode and BG0-3 enable),
 * preserving all other DISPCNT settings (sprite enable, windows, etc.)
 * by reading the current value with GetGpuReg and masking.
 */
static void SyncBgVisibilityAndMode(void)
{
    SetGpuReg(0, (GetGpuReg(0) & ~DISPCNT_ALL_BG_AND_MODE_BITS) | sGpuBgConfigs.bgVisibilityAndMode);
}

/*
 * SetTextModeAndHideBgs - Set BG mode 0 and disable all BG layers.
 * Clears the low 12 bits of DISPCNT (mode + BG enables).
 * Used during screen transitions to blank the background before
 * setting up new graphics.
 */
void SetTextModeAndHideBgs(void)
{
    SetGpuReg(0, GetGpuReg(0) & ~DISPCNT_ALL_BG_AND_MODE_BITS);
}

/*
 * SetBgAffineInternal - Configure affine (rotation/scaling) for a BG layer.
 *
 * Affine transformations use a 2x2 matrix (PA, PB, PC, PD) and a
 * reference point (DX, DY). For each screen pixel (x, y), the GPU
 * computes the source texture coordinate:
 *   srcX = PA * (x - dispCenterX) + PB * (y - dispCenterY) + DX
 *   srcY = PC * (x - dispCenterX) + PD * (y - dispCenterY) + DY
 *
 * BgAffineSet() is a BIOS function (SWI 0x0E) that computes PA-PD and
 * DX/DY from human-friendly parameters (scale, rotation angle, center point).
 *
 * Only works for BG2 in mode 1, or BG2/BG3 in mode 2.
 * In mode 0 (normal text mode), affine is not available.
 */
static void SetBgAffineInternal(u8 bg, u32 srcCenterX, u32 srcCenterY, s16 dispCenterX, s16 dispCenterY, s16 scaleX, s16 scaleY, u16 rotationAngle)
{
    struct BgAffineSrcData src;
    struct BgAffineDstData dest;

    switch (sGpuBgConfigs.bgVisibilityAndMode & 0x7)
    {
        case 1:
            if (bg != 2)
                return;
            break;
        case 2:
            if (bg < 2 || bg > 3)
                return;
            break;
        case 0:
        default:
            return;
    }

    src.texX = srcCenterX;
    src.texY = srcCenterY;
    src.scrX = dispCenterX;
    src.scrY = dispCenterY;
    src.sx = scaleX;
    src.sy = scaleY;
    src.alpha = rotationAngle;

    BgAffineSet(&src, &dest, 1);

    SetGpuReg(REG_OFFSET_BG2PA, dest.pa);
    SetGpuReg(REG_OFFSET_BG2PB, dest.pb);
    SetGpuReg(REG_OFFSET_BG2PC, dest.pc);
    SetGpuReg(REG_OFFSET_BG2PD, dest.pd);
    SetGpuReg(REG_OFFSET_BG2PA, dest.pa);
    SetGpuReg(REG_OFFSET_BG2X_L, (s16)(dest.dx));
    SetGpuReg(REG_OFFSET_BG2X_H, (s16)(dest.dx >> 16));
    SetGpuReg(REG_OFFSET_BG2Y_L, (s16)(dest.dy));
    SetGpuReg(REG_OFFSET_BG2Y_H, (s16)(dest.dy >> 16));
}

/*
 * IsInvalidBg - Validate BG index (must be 0-3).
 * The GBA only has 4 BG layers. Any index > 3 is invalid.
 */
bool8 IsInvalidBg(u8 bg)
{
    if (bg > 3)
        return TRUE;
    return FALSE;
}

/*
 * BgTileAllocOp - BG tile VRAM allocator.
 *
 * Manages a bitmap (gpu_tile_allocation_map_bg) that tracks which
 * 32-byte tile slots in VRAM are in use. Used by the window system
 * to dynamically allocate VRAM space for text box tiles.
 *
 * Operations:
 *   BG_TILE_FIND_FREE_SPACE: Scan for 'count' consecutive free tiles.
 *     Returns the offset of the first tile, or -1 if not enough space.
 *     This is a first-fit allocator scanning left to right.
 *
 *   BG_TILE_ALLOC: Mark 'count' tiles starting at 'offset' as used.
 *
 *   BG_TILE_FREE: Mark 'count' tiles starting at 'offset' as free.
 *
 * Each BG's character base block determines the search range.
 * With 4 char base blocks of 16 KB each, and 32 bytes per 4bpp tile,
 * each block has 512 tiles. Total: 2048 tile slots.
 */
int BgTileAllocOp(int bg, int offset, int count, int mode)
{
    int start, end;
    int blockSize;
    int blockStart;
    int i;

    switch (mode)
    {
    case BG_TILE_FIND_FREE_SPACE:
        start = GetBgControlAttribute(bg, BG_CTRL_ATTR_CHARBASEINDEX) * (BG_CHAR_SIZE / TILE_SIZE_4BPP);
        end = start + 0x400;
        if (end > 0x800)
            end = 0x800;
        blockSize = 0;
        blockStart = 0;
        for (i = start, offset = 0; i < end; i++, offset++)
        {
            if (!((gpu_tile_allocation_map_bg[i / 8] >> (i % 8)) & 1))
            {
                if (blockSize)
                {
                    blockSize++;
                    if (blockSize == count)
                        return blockStart;
                }
                else
                {
                    blockStart = offset;
                    blockSize = 1;
                }
            }
            else
            {
                blockSize = 0;
            }
        }
        return -1;
    case BG_TILE_ALLOC:
        start = GetBgControlAttribute(bg, BG_CTRL_ATTR_CHARBASEINDEX) * (BG_CHAR_SIZE / TILE_SIZE_4BPP) + offset;
        end = start + count;
        for (i = start; i < end; i++)
            gpu_tile_allocation_map_bg[i / 8] |= 1 << (i % 8);
        break;
    case BG_TILE_FREE:
        start = GetBgControlAttribute(bg, BG_CTRL_ATTR_CHARBASEINDEX) * (BG_CHAR_SIZE / TILE_SIZE_4BPP) + offset;
        end = start + count;
        for (i = start; i < end; i++)
            gpu_tile_allocation_map_bg[i / 8] &= ~(1 << (i % 8));
        break;
    }

    return 0;
}

/*
 * ResetBgsAndClearDma3BusyFlags - Full reset for major state transitions.
 * Clears BG state, DMA tracking, and tile allocation map.
 * Called at the start of battle, menus, and other screen changes.
 */
void ResetBgsAndClearDma3BusyFlags(bool32 enableWindowTileAutoAlloc)
{
    int i;
    ResetBgs();

    for (i = 0; i < 4; i++)
    {
        sDmaBusyBitfield[i] = 0;
    }

    gWindowTileAutoAllocEnabled = enableWindowTileAutoAlloc;

    for (i = 0; i < 0x100; i++)
    {
        gpu_tile_allocation_map_bg[i] = 0;
    }
}

/*
 * InitBgsFromTemplates - Set up multiple BG layers from a template array.
 *
 * BgTemplate is a convenient struct for specifying all BG attributes at once.
 * This function iterates the template array, configuring each BG layer.
 * Used at the start of each game screen (overworld, battle, menu) to
 * define which BG layers are active and how they're configured.
 *
 * The first tile of each character base block is marked as allocated
 * in the tile allocation map (it's typically reserved for the "blank" tile).
 */
void InitBgsFromTemplates(u8 bgMode, const struct BgTemplate *templates, u8 numTemplates)
{
    int i;
    u8 bg;

    SetBgModeInternal(bgMode);
    ResetBgControlStructs();

    for (i = 0; i < numTemplates; i++)
    {
        bg = templates[i].bg;
        if (bg < 4) {
            SetBgControlAttributes(bg,
                                   templates[i].charBaseIndex,
                                   templates[i].mapBaseIndex,
                                   templates[i].screenSize,
                                   templates[i].paletteMode,
                                   templates[i].priority,
                                   0,
                                   0);

            sGpuBgConfigs2[bg].baseTile = templates[i].baseTile;
            sGpuBgConfigs2[bg].basePalette = 0;
            sGpuBgConfigs2[bg].unk_3 = 0;

            sGpuBgConfigs2[bg].tilemap = NULL;
            sGpuBgConfigs2[bg].bg_x = 0;
            sGpuBgConfigs2[bg].bg_y = 0;

            gpu_tile_allocation_map_bg[(templates[i].charBaseIndex * (BG_CHAR_SIZE / TILE_SIZE_4BPP)) / 8] = 1;
        }
    }
}

void InitBgFromTemplate(const struct BgTemplate *template)
{
    u8 bg = template->bg;

    if (bg < 4)
    {
        SetBgControlAttributes(bg,
                               template->charBaseIndex,
                               template->mapBaseIndex,
                               template->screenSize,
                               template->paletteMode,
                               template->priority,
                               0,
                               0);

        sGpuBgConfigs2[bg].baseTile = template->baseTile;
        sGpuBgConfigs2[bg].basePalette = 0;
        sGpuBgConfigs2[bg].unk_3 = 0;

        sGpuBgConfigs2[bg].tilemap = NULL;
        sGpuBgConfigs2[bg].bg_x = 0;
        sGpuBgConfigs2[bg].bg_y = 0;

        gpu_tile_allocation_map_bg[(template->charBaseIndex * (BG_CHAR_SIZE / TILE_SIZE_4BPP)) / 8] = 1;
    }
}

/*
 * LoadBgTiles - Load tile graphics (character data) to VRAM via DMA.
 *
 * Computes the destination offset based on baseTile and palette mode:
 *   4bpp: each tile = 0x20 (32) bytes
 *   8bpp: each tile = 0x40 (64) bytes
 *
 * Queues the data for DMA3 transfer during VBlank.
 * Also marks the tile range as allocated in the tile allocation map.
 */
u16 LoadBgTiles(u8 bg, const void *src, u16 size, u16 destOffset)
{
    u16 tileOffset;
    u8 cursor;

    if (GetBgControlAttribute(bg, BG_CTRL_ATTR_PALETTEMODE) == 0)
    {
        tileOffset = (sGpuBgConfigs2[bg].baseTile + destOffset) * 0x20;
    }
    else
    {
        tileOffset = (sGpuBgConfigs2[bg].baseTile + destOffset) * 0x40;
    }

    cursor = LoadBgVram(bg, src, size, tileOffset, DISPCNT_MODE_1);

    if (cursor == 0xFF)
    {
        return -1;
    }

    sDmaBusyBitfield[cursor / 0x20] |= (1 << (cursor % 0x20));

    if (gWindowTileAutoAllocEnabled == TRUE)
    {
        BgTileAllocOp(bg, tileOffset / 0x20, size / 0x20, BG_TILE_ALLOC);
    }

    return cursor;
}

/*
 * LoadBgTilemap - Load tilemap (screen) data to VRAM via DMA.
 * destOffset is in units of 32 bytes (one tilemap row = 32 entries × 2 bytes = 64 bytes,
 * but the offset unit here is screen blocks).
 */
u16 LoadBgTilemap(u8 bg, const void *src, u16 size, u16 destOffset)
{
    u8 cursor;

    cursor = LoadBgVram(bg, src, size, destOffset * 32, DISPCNT_MODE_2);

    if (cursor == 0xFF)
    {
        return -1;
    }

    sDmaBusyBitfield[cursor / 0x20] |= (1 << (cursor % 0x20));

    return cursor;
}

u16 Unused_LoadBgPalette(u8 bg, const void *src, u16 size, u16 destOffset)
{
    u16 paletteOffset;
    s8 cursor;

    if (IsInvalidBg32(bg) == FALSE)
    {
        paletteOffset = (sGpuBgConfigs2[bg].basePalette * 0x20) + (destOffset * 2);
        cursor = RequestDma3Copy(src, (void *)(paletteOffset + BG_PLTT), size, DMA3_16BIT);

        if (cursor == -1)
        {
            return -1;
        }
    }
    else
    {
        return -1;
    }

    sDmaBusyBitfield[cursor / 0x20] |= (1 << (cursor % 0x20));

    return (u8)cursor;
}

/*
 * IsDma3ManagerBusyWithBgCopy - Check if all pending BG DMA copies are done.
 *
 * Iterates through all DMA request slots tracked in sDmaBusyBitfield.
 * For each in-use slot, calls WaitDma3Request() to check if complete.
 * Returns TRUE if any request is still pending.
 *
 * Used by the game to wait for VRAM updates before proceeding:
 *   while (IsDma3ManagerBusyWithBgCopy()) {}
 * This is a "sync point" - ensures tiles/tilemaps are in VRAM
 * before the GPU tries to render them.
 */
bool8 IsDma3ManagerBusyWithBgCopy(void)
{
    int i;

    for (i = 0; i < 0x80; i++)
    {
        u8 div = i / 0x20;
        u8 mod = i % 0x20;

        if ((sDmaBusyBitfield[div] & (1 << mod)))
        {
            s8 reqSpace = WaitDma3Request(i);
            if (reqSpace == -1)
                return TRUE;
            sDmaBusyBitfield[div] &= ~(1 << mod);
        }
    }
    return FALSE;
}

/*
 * ShowBg / HideBg - Public API to enable/disable a BG layer.
 * ShowBg writes the BGxCNT register and sets the DISPCNT enable bit.
 * HideBg clears the DISPCNT enable bit (config stays in memory).
 */
void ShowBg(u8 bg)
{
    ShowBgInternal(bg);
    SyncBgVisibilityAndMode();
}

void HideBg(u8 bg)
{
    HideBgInternal(bg);
    SyncBgVisibilityAndMode();
}

/*
 * SetBgAttribute / GetBgAttribute - Modify/read individual BG config fields.
 * Higher-level API that wraps SetBgControlAttributes with the 0xFF
 * "don't change" pattern for all other fields.
 */
void SetBgAttribute(u8 bg, u8 attributeId, u8 value)
{
    switch (attributeId)
    {
        case 1:
            SetBgControlAttributes(bg, value, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
            break;
        case 2:
            SetBgControlAttributes(bg, 0xFF, value, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
            break;
        case 3:
            SetBgControlAttributes(bg, 0xFF, 0xFF, value, 0xFF, 0xFF, 0xFF, 0xFF);
            break;
        case 4:
            SetBgControlAttributes(bg, 0xFF, 0xFF, 0xFF, value, 0xFF, 0xFF, 0xFF);
            break;
        case 7:
            SetBgControlAttributes(bg, 0xFF, 0xFF, 0xFF, 0xFF, value, 0xFF, 0xFF);
            break;
        case 5:
            SetBgControlAttributes(bg, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, value, 0xFF);
            break;
        case 6:
            SetBgControlAttributes(bg, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, value);
            break;
    }
}

u16 GetBgAttribute(u8 bg, u8 attributeId)
{
    switch (attributeId)
    {
        case BG_ATTR_CHARBASEINDEX:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_CHARBASEINDEX);
        case BG_ATTR_MAPBASEINDEX:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_MAPBASEINDEX);
        case BG_ATTR_SCREENSIZE:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_SCREENSIZE);
        case BG_ATTR_PALETTEMODE:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_PALETTEMODE);
        case BG_ATTR_PRIORITY:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_PRIORITY);
        case BG_ATTR_MOSAIC:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_MOSAIC);
        case BG_ATTR_WRAPAROUND:
            return GetBgControlAttribute(bg, BG_CTRL_ATTR_WRAPAROUND);
        case BG_ATTR_MAPSIZE:
            switch (GetBgType(bg))
            {
                case 0:
                    return GetBgMetricTextMode(bg, 0) * 0x800;
                case 1:
                    return GetBgMetricAffineMode(bg, 0) * 0x100;
                default:
                    return 0;
            }
        case BG_ATTR_BGTYPE:
            return GetBgType(bg);
        case BG_ATTR_BASETILE:
            return sGpuBgConfigs2[bg].baseTile;
        default:
            return -1;
    }
}

/*
 * ChangeBgX / ChangeBgY - Set or modify a BG layer's scroll position.
 *
 * The scroll value is in 8.8 FIXED-POINT format:
 *   High byte (bits 8-15) = integer pixel offset
 *   Low byte (bits 0-7) = fractional sub-pixel
 *
 * The hardware scroll registers (BGxHOFS, BGxVOFS) only accept the
 * integer part, so we shift right by 8 before writing.
 *
 * Fixed-point math is used because the camera might move at non-integer
 * speeds (e.g., 1.5 pixels per frame for smooth diagonal scrolling).
 *
 * Operations:
 *   BG_COORD_SET: Set to absolute value
 *   BG_COORD_ADD: Add to current position (scroll right/down)
 *   BG_COORD_SUB: Subtract from current position (scroll left/up)
 *
 * For affine backgrounds (mode 1/2), the scroll uses the 32-bit
 * BG2X/BG2Y reference point registers instead, which are split into
 * high and low 16-bit halves.
 *
 * This is the function that makes the overworld scroll when you walk.
 * The camera system calls ChangeBgX/Y every frame with the player's
 * position to keep the viewport centered on the player.
 */
u32 ChangeBgX(u8 bg, u32 value, u8 op)
{
    u8 mode;
    u16 temp1;
    u16 temp2;

    if (IsInvalidBg32(bg) != FALSE || GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) == 0)
    {
        return -1;
    }

    switch (op)
    {
    case BG_COORD_SET:
    default:
        sGpuBgConfigs2[bg].bg_x = value;
        break;
    case BG_COORD_ADD:
        sGpuBgConfigs2[bg].bg_x += value;
        break;
    case BG_COORD_SUB:
        sGpuBgConfigs2[bg].bg_x -= value;
        break;
    }

    mode = GetBgMode();

    switch (bg)
    {
    case 0:
        temp1 = sGpuBgConfigs2[0].bg_x >> 0x8;
        SetGpuReg(REG_OFFSET_BG0HOFS, temp1);
        break;
    case 1:
        temp1 = sGpuBgConfigs2[1].bg_x >> 0x8;
        SetGpuReg(REG_OFFSET_BG1HOFS, temp1);
        break;
    case 2:
        if (mode == 0)
        {
            temp1 = sGpuBgConfigs2[2].bg_x >> 0x8;
            SetGpuReg(REG_OFFSET_BG2HOFS, temp1);
        }
        else
        {
            temp1 = sGpuBgConfigs2[2].bg_x >> 0x10;
            temp2 = sGpuBgConfigs2[2].bg_x & 0xFFFF;
            SetGpuReg(REG_OFFSET_BG2X_H, temp1);
            SetGpuReg(REG_OFFSET_BG2X_L, temp2);
        }
        break;
    case 3:
        if (mode == 0)
        {
            temp1 = sGpuBgConfigs2[3].bg_x >> 0x8;
            SetGpuReg(REG_OFFSET_BG3HOFS, temp1);
        }
        else if (mode == 2)
        {
            temp1 = sGpuBgConfigs2[3].bg_x >> 0x10;
            temp2 = sGpuBgConfigs2[3].bg_x & 0xFFFF;
            SetGpuReg(REG_OFFSET_BG3X_H, temp1);
            SetGpuReg(REG_OFFSET_BG3X_L, temp2);
        }
        break;
    }

    return sGpuBgConfigs2[bg].bg_x;
}

u32 GetBgX(u8 bg)
{
    if (IsInvalidBg32(bg) != FALSE)
        return -1;
    if (GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) == 0)
        return -1;
    return sGpuBgConfigs2[bg].bg_x;
}

u32 ChangeBgY(u8 bg, u32 value, u8 op)
{
    u8 mode;
    u16 temp1;
    u16 temp2;

    if (IsInvalidBg32(bg) != FALSE || GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) == 0)
    {
        return -1;
    }

    switch (op)
    {
    case BG_COORD_SET:
    default:
        sGpuBgConfigs2[bg].bg_y = value;
        break;
    case BG_COORD_ADD:
        sGpuBgConfigs2[bg].bg_y += value;
        break;
    case BG_COORD_SUB:
        sGpuBgConfigs2[bg].bg_y -= value;
        break;
    }

    mode = GetBgMode();

    switch (bg)
    {
    case 0:
        temp1 = sGpuBgConfigs2[0].bg_y >> 0x8;
        SetGpuReg(REG_OFFSET_BG0VOFS, temp1);
        break;
    case 1:
        temp1 = sGpuBgConfigs2[1].bg_y >> 0x8;
        SetGpuReg(REG_OFFSET_BG1VOFS, temp1);
        break;
    case 2:
        if (mode == 0)
        {
            temp1 = sGpuBgConfigs2[2].bg_y >> 0x8;
            SetGpuReg(REG_OFFSET_BG2VOFS, temp1);
        }
        else
        {
            temp1 = sGpuBgConfigs2[2].bg_y >> 0x10;
            temp2 = sGpuBgConfigs2[2].bg_y & 0xFFFF;
            SetGpuReg(REG_OFFSET_BG2Y_H, temp1);
            SetGpuReg(REG_OFFSET_BG2Y_L, temp2);
        }
        break;
    case 3:
        if (mode == 0)
        {
            temp1 = sGpuBgConfigs2[3].bg_y >> 0x8;
            SetGpuReg(REG_OFFSET_BG3VOFS, temp1);
        }
        else if (mode == 2)
        {
            temp1 = sGpuBgConfigs2[3].bg_y >> 0x10;
            temp2 = sGpuBgConfigs2[3].bg_y & 0xFFFF;
            SetGpuReg(REG_OFFSET_BG3Y_H, temp1);
            SetGpuReg(REG_OFFSET_BG3Y_L, temp2);
        }
        break;
    }

    return sGpuBgConfigs2[bg].bg_y;
}

u32 GetBgY(u8 bg)
{
    if (IsInvalidBg32(bg) != FALSE)
        return -1;
    if (GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) == 0)
        return -1;
    return sGpuBgConfigs2[bg].bg_y;
}

void SetBgAffine(u8 bg, u32 srcCenterX, u32 srcCenterY, s16 dispCenterX, s16 dispCenterY, s16 scaleX, s16 scaleY, u16 rotationAngle)
{
    SetBgAffineInternal(bg, srcCenterX, srcCenterY, dispCenterX, dispCenterY, scaleX, scaleY, rotationAngle);
}

/*
 * AdjustBgMosaic - Modify the mosaic effect intensity.
 *
 * REG_MOSAIC (0x0400004C) controls the mosaic pixelation effect.
 * Mosaic makes groups of pixels display the same color, creating
 * a "blocky" low-resolution look. Used for screen transitions.
 *
 *   Bits 0-3:  BG mosaic horizontal size (0-15)
 *   Bits 4-7:  BG mosaic vertical size (0-15)
 *   Bits 8-11: OBJ mosaic horizontal size
 *   Bits 12-15: OBJ mosaic vertical size
 *
 * Size 0 = no effect (1:1). Size 15 = maximum (16x16 pixel blocks).
 *
 * Operations: set both, set H only, set V only, increment/decrement H/V.
 */
u8 AdjustBgMosaic(u8 value, u8 mode)
{
    u16 mosaicSize;
    s16 bgMosaicH;
    s16 bgMosaicV;
    mosaicSize = GetGpuReg(REG_OFFSET_MOSAIC);
    bgMosaicH = mosaicSize & 0xF;
    bgMosaicV = (mosaicSize >> 4) & 0xF;
    mosaicSize &= 0xFF00;

    switch (mode)
    {
    case BG_MOSAIC_SET:
    default:
        bgMosaicH = value & 0xF;
        bgMosaicV = value >> 0x4;
        break;
    case BG_MOSAIC_SET_H:
        bgMosaicH = value & 0xF;
        break;
    case BG_MOSAIC_INC_H:
        if ((bgMosaicH + value) > 0xF)
            bgMosaicH = 0xF;
        else
            bgMosaicH += value;
        break;
    case BG_MOSAIC_DEC_H:
        if ((bgMosaicH - value) < 0)
            bgMosaicH = 0x0;
        else
            bgMosaicH -= value;
        break;
    case BG_MOSAIC_SET_V:
        bgMosaicV = value & 0xF;
        break;
    case BG_MOSAIC_INC_V:
        if ((bgMosaicV + value) > 0xF)
            bgMosaicV = 0xF;
        else
            bgMosaicV += value;
        break;
    case BG_MOSAIC_DEC_V:
        if ((bgMosaicV - value) < 0)
            bgMosaicV = 0x0;
        else
            bgMosaicV -= value;
        break;
    }
    mosaicSize |= ((bgMosaicV << 0x4) & 0xF0);
    mosaicSize |= (bgMosaicH & 0xF);
    SetGpuReg(REG_OFFSET_MOSAIC, mosaicSize);
    return mosaicSize;
}

/*
 * TILEMAP BUFFER MANAGEMENT
 *
 * The game maintains tilemap data in RAM buffers (EWRAM) rather than
 * editing VRAM directly. This allows:
 *   1. Safe editing at any time (not just VBlank)
 *   2. Partial updates (change a few tiles, copy the whole buffer later)
 *   3. LZ77 decompression to RAM (can't decompress directly to VRAM)
 *
 * Workflow:
 *   SetBgTilemapBuffer(bg, buffer)  → assign a RAM buffer
 *   FillBgTilemapBufferRect(...)    → edit tiles in the RAM buffer
 *   CopyBgTilemapBufferToVram(bg)   → queue DMA copy to VRAM
 *   (VBlank DMA executes the copy)
 */

void SetBgTilemapBuffer(u8 bg, void *tilemap)
{
    if (IsInvalidBg32(bg) == FALSE && GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) != 0x0)
    {
        sGpuBgConfigs2[bg].tilemap = tilemap;
    }
}

void UnsetBgTilemapBuffer(u8 bg)
{
    if (IsInvalidBg32(bg) == FALSE && GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) != 0x0)
    {
        sGpuBgConfigs2[bg].tilemap = NULL;
    }
}

void *GetBgTilemapBuffer(u8 bg)
{
    if (IsInvalidBg32(bg) != FALSE)
        return NULL;
    if (GetBgControlAttribute(bg, BG_CTRL_ATTR_VISIBLE) == 0)
        return NULL;
    return sGpuBgConfigs2[bg].tilemap;
}

/*
 * CopyToBgTilemapBuffer - Copy tilemap data to the RAM buffer.
 *
 * If mode != 0: plain copy of 'mode' bytes (CpuCopy16).
 * If mode == 0: LZ77 decompress from 'src' into the buffer.
 *
 * LZ77UnCompWram is a BIOS call (SWI 0x11) that decompresses
 * LZ77-encoded data. Most tilemap data in the ROM is compressed
 * to save cartridge space. The BIOS handles decompression in hardware,
 * which is faster than a software implementation.
 */
void CopyToBgTilemapBuffer(u8 bg, const void *src, u16 mode, u16 destOffset)
{
    if (IsInvalidBg32(bg) == FALSE && IsTileMapOutsideWram(bg) == FALSE)
    {
        if (mode != 0)
        {
            CpuCopy16(src, (void *)(sGpuBgConfigs2[bg].tilemap + (destOffset * 32)), mode);
        }
        else
        {
            LZ77UnCompWram(src, (void *)(sGpuBgConfigs2[bg].tilemap + (destOffset * 32)));
        }
    }
}

/*
 * CopyBgTilemapBufferToVram - Queue the entire RAM tilemap for DMA to VRAM.
 *
 * Computes the tilemap size based on BG type and screen size:
 *   Text mode: size in screen blocks × 0x800 (2 KB per block)
 *   Affine mode: size in map pages × 0x100 (256 bytes per page)
 *
 * The DMA copy happens during VBlank (see LoadBgVram → RequestDma3Copy).
 * Mode 2 = copy to screen base (tilemap area of VRAM).
 */
void CopyBgTilemapBufferToVram(u8 bg)
{
    u16 sizeToLoad;

    if (IsInvalidBg32(bg) == FALSE && IsTileMapOutsideWram(bg) == FALSE)
    {
        switch (GetBgType(bg))
        {
            case 0:
                sizeToLoad = GetBgMetricTextMode(bg, 0) * 0x800;
                break;
            case 1:
                sizeToLoad = GetBgMetricAffineMode(bg, 0) * 0x100;
                break;
            default:
                sizeToLoad = 0;
                break;
        }
        LoadBgVram(bg, sGpuBgConfigs2[bg].tilemap, sizeToLoad, 0, 2);
    }
}

/*
 * CopyToBgTilemapBufferRect - Copy a rectangular region of tilemap data
 * from a source array to the RAM tilemap buffer.
 *
 * In text mode (type 0): each entry is 16 bits (u16).
 * In affine mode (type 1): each entry is 8 bits (u8).
 *
 * The tilemap buffer is a flat array; the address for position (x, y) is:
 *   Text mode: buffer[y * 32 + x] (32 tiles per row in a screen block)
 *   Affine mode: buffer[y * mapWidth + x]
 */
void CopyToBgTilemapBufferRect(u8 bg, const void *src, u8 destX, u8 destY, u8 width, u8 height)
{
    u16 destX16;
    u16 destY16;
    u16 mode;

    if (IsInvalidBg32(bg) == FALSE && IsTileMapOutsideWram(bg) == FALSE)
    {
        switch (GetBgType(bg))
        {
            case 0:
            {
                const u16 * srcCopy = src;
                for (destY16 = destY; destY16 < (destY + height); destY16++)
                {
                    for (destX16 = destX; destX16 < (destX + width); destX16++)
                    {
                        ((u16 *)sGpuBgConfigs2[bg].tilemap)[((destY16 * 0x20) + destX16)] = *(srcCopy)++;
                    }
                }
                break;
            }
            case 1:
            {
                const u8 * srcCopy = src;
                mode = GetBgMetricAffineMode(bg, 0x1);
                for (destY16 = destY; destY16 < (destY + height); destY16++)
                {
                    for (destX16 = destX; destX16 < (destX + width); destX16++)
                    {
                        ((u8 *)sGpuBgConfigs2[bg].tilemap)[((destY16 * mode) + destX16)] = *(srcCopy)++;
                    }
                }
                break;
            }
        }
    }
}

void CopyToBgTilemapBufferRect_ChangePalette(u8 bg, const void *src, u8 destX, u8 destY, u8 rectWidth, u8 rectHeight, u8 palette)
{
    CopyRectToBgTilemapBufferRect(bg, src, 0, 0, rectWidth, rectHeight, destX, destY, rectWidth, rectHeight, palette, 0, 0);
}

/*
 * CopyRectToBgTilemapBufferRect - Advanced tilemap copy with palette override.
 *
 * Copies a rectangular region from a source tilemap to the buffer,
 * with per-entry palette and tile offset adjustments via CopyTileMapEntry().
 *
 * This handles the complex screen block layout for tilemaps larger than
 * 32x32 tiles (screen sizes 1-3) via GetTileMapIndexFromCoords().
 */
void CopyRectToBgTilemapBufferRect(u8 bg, const void *src, u8 srcX, u8 srcY, u8 srcWidth, u8 srcHeight, u8 destX, u8 destY, u8 rectWidth, u8 rectHeight, u8 palette1, s16 tileOffset, s16 palette2)
{
    u16 screenWidth, screenHeight, screenSize;
    u16 var;
    const void *srcPtr;
    u16 i, j;

    if (!IsInvalidBg32(bg) && !IsTileMapOutsideWram(bg))
    {
        screenSize = GetBgControlAttribute(bg, BG_CTRL_ATTR_SCREENSIZE);
        screenWidth = GetBgMetricTextMode(bg, 0x1) * 0x20;
        screenHeight = GetBgMetricTextMode(bg, 0x2) * 0x20;
        switch (GetBgType(bg))
        {
        case 0:
            srcPtr = src + ((srcY * srcWidth) + srcX) * 2;
            for (i = destY; i < (destY + rectHeight); i++)
            {
                for (j = destX; j < (destX + rectWidth); j++)
                {
                    u16 index = GetTileMapIndexFromCoords(j, i, screenSize, screenWidth, screenHeight);
                    CopyTileMapEntry(srcPtr, sGpuBgConfigs2[bg].tilemap + (index * 2), palette1, tileOffset, palette2);
                    srcPtr += 2;
                }
                srcPtr += (srcWidth - rectWidth) * 2;
            }
            break;
        case 1:
            srcPtr = src + ((srcY * srcWidth) + srcX);
            var = GetBgMetricAffineMode(bg, 0x1);
            for (i = destY; i < (destY + rectHeight); i++)
            {
                for (j = destX; j < (destX + rectWidth); j++)
                {
                    *(u8 *)(sGpuBgConfigs2[bg].tilemap + ((var * i) + j)) = *(u8 *)(srcPtr) + tileOffset;
                    srcPtr++;
                }
                srcPtr += (srcWidth - rectWidth);
            }
            break;
        }
    }
}

/*
 * FillBgTilemapBufferRect_Palette0 - Fill a rectangle with a single tile.
 * Writes the same tileNum to every position in the rectangle.
 * Used to clear regions of the tilemap (e.g., fill with tile 0 = transparent).
 */
void FillBgTilemapBufferRect_Palette0(u8 bg, u16 tileNum, u8 x, u8 y, u8 width, u8 height)
{
    u16 x16;
    u16 y16;
    u16 mode;

    if (IsInvalidBg32(bg) == FALSE && IsTileMapOutsideWram(bg) == FALSE)
    {
        switch (GetBgType(bg))
        {
            case 0:
                for (y16 = y; y16 < (y + height); y16++)
                {
                    for (x16 = x; x16 < (x + width); x16++)
                    {
                        ((u16 *)sGpuBgConfigs2[bg].tilemap)[((y16 * 0x20) + x16)] = tileNum;
                    }
                }
                break;
            case 1:
                mode = GetBgMetricAffineMode(bg, 0x1);
                for (y16 = y; y16 < (y + height); y16++)
                {
                    for (x16 = x; x16 < (x + width); x16++)
                    {
                        ((u8 *)sGpuBgConfigs2[bg].tilemap)[((y16 * mode) + x16)] = tileNum;
                    }
                }
                break;
        }
    }
}

/*
 * FillBgTilemapBufferRect - Fill a rectangle with a tile, specifying palette.
 * Delegates to WriteSequenceToBgTilemapBuffer with tileNumDelta=0
 * (all tiles in the rectangle are the same).
 */
void FillBgTilemapBufferRect(u8 bg, u16 tileNum, u8 x, u8 y, u8 width, u8 height, u8 palette)
{
    WriteSequenceToBgTilemapBuffer(bg, tileNum, x, y, width, height, palette, 0);
}

/*
 * WriteSequenceToBgTilemapBuffer - Fill a rectangle with incrementing tiles.
 *
 * Writes firstTileNum to position (x,y), firstTileNum+tileNumDelta to (x+1,y),
 * and so on. Used for drawing frames/borders where each tile is sequential.
 *
 * CopyTileMapEntry() handles the palette bits of each tilemap entry.
 * The tile index is in the lower 10 bits (& 0x3FF), palette in upper bits.
 *
 * GetTileMapIndexFromCoords handles the multi-screen-block layout
 * for tilemaps larger than 32x32 tiles.
 */
void WriteSequenceToBgTilemapBuffer(u8 bg, u16 firstTileNum, u8 x, u8 y, u8 width, u8 height, u8 paletteSlot, s16 tileNumDelta)
{
    u16 mode;
    u16 mode2;
    u16 attribute;
    u16 mode3;

    u16 x16;
    u16 y16;

    if (IsInvalidBg32(bg) == FALSE && IsTileMapOutsideWram(bg) == FALSE)
    {
        attribute = GetBgControlAttribute(bg, BG_CTRL_ATTR_SCREENSIZE);
        mode = GetBgMetricTextMode(bg, 0x1) * 0x20;
        mode2 = GetBgMetricTextMode(bg, 0x2) * 0x20;
        switch (GetBgType(bg))
        {
            case 0:
                for (y16 = y; y16 < (y + height); y16++)
                {
                    for (x16 = x; x16 < (x + width); x16++)
                    {
                        CopyTileMapEntry(&firstTileNum, &((u16 *)sGpuBgConfigs2[bg].tilemap)[(u16)GetTileMapIndexFromCoords(x16, y16, attribute, mode, mode2)], paletteSlot, 0, 0);
                        firstTileNum = (firstTileNum & 0xFC00) + ((firstTileNum + tileNumDelta) & 0x3FF);
                    }
                }
                break;
            case 1:
                mode3 = GetBgMetricAffineMode(bg, 0x1);
                for (y16 = y; y16 < (y + height); y16++)
                {
                    for (x16 = x; x16 < (x + width); x16++)
                    {
                        ((u8 *)sGpuBgConfigs2[bg].tilemap)[(y16 * mode3) + x16] = firstTileNum;
                        firstTileNum = (firstTileNum & 0xFC00) + ((firstTileNum + tileNumDelta) & 0x3FF);
                    }
                }
                break;
        }
    }
}

/*
 * GetBgMetricTextMode - Get tilemap dimensions for text-mode BGs.
 *
 * Returns the number of screen blocks or the width/height in tiles
 * based on the screen size setting (0-3).
 *
 * whichMetric:
 *   0: Total screen blocks (1, 2, 2, or 4)
 *   1: Width in 32-tile units (1 or 2)
 *   2: Height in 32-tile units (1 or 2)
 *
 * Screen size layout in VRAM (each box = one 32x32 screen block):
 *   Size 0: [A]           (32x32 = 256x256 pixels)
 *   Size 1: [A][B]        (64x32 = 512x256 pixels)
 *   Size 2: [A]           (32x64 = 256x512 pixels)
 *           [B]
 *   Size 3: [A][B]        (64x64 = 512x512 pixels)
 *           [C][D]
 */
u16 GetBgMetricTextMode(u8 bg, u8 whichMetric)
{
    u8 attribute;

    attribute = GetBgControlAttribute(bg, BG_CTRL_ATTR_SCREENSIZE);

    switch (whichMetric)
    {
        case 0:
            switch (attribute)
            {
                case 0:
                    return 1;
                case 1:
                case 2:
                    return 2;
                case 3:
                    return 4;
            }
            break;
        case 1:
            switch (attribute)
            {
                case 0:
                    return 1;
                case 1:
                    return 2;
                case 2:
                    return 1;
                case 3:
                    return 2;
            }
            break;
        case 2:
            switch (attribute)
            {
                case 0:
                case 1:
                    return 1;
                case 2:
                case 3:
                    return 2;
            }
            break;
    }
    return 0;
}

/*
 * GetBgMetricAffineMode - Get tilemap dimensions for affine-mode BGs.
 *
 * Affine BGs use different size progression than text mode:
 *   Size 0: 16x16 tiles   (128x128 pixels)   = 1 page
 *   Size 1: 32x32 tiles   (256x256 pixels)   = 4 pages
 *   Size 2: 64x64 tiles   (512x512 pixels)   = 16 pages
 *   Size 3: 128x128 tiles (1024x1024 pixels)  = 64 pages
 *
 * Each "page" is 256 bytes (16x16 8-bit entries).
 * Width/height = 16 << screenSize tiles.
 */
u32 GetBgMetricAffineMode(u8 bg, u8 whichMetric)
{
    u8 attribute;

    attribute = GetBgControlAttribute(bg, BG_CTRL_ATTR_SCREENSIZE);

    switch (whichMetric)
    {
        case 0:
            switch (attribute)
            {
                case 0:
                    return 0x1;
                case 1:
                    return 0x4;
                case 2:
                    return 0x10;
                case 3:
                    return 0x40;
            }
            break;
        case 1:
        case 2:
            return 0x10 << attribute;
    }
    return 0;
}

/*
 * GetTileMapIndexFromCoords - Convert (x, y) tile coords to a flat array index.
 *
 * This is needed because tilemaps larger than 32x32 use MULTIPLE screen blocks
 * arranged in a specific layout. A 64x64 tilemap uses 4 screen blocks:
 *
 *   Screen block layout for size 3 (512x512):
 *     Block 0: rows 0-31, cols 0-31
 *     Block 1: rows 0-31, cols 32-63
 *     Block 2: rows 32-63, cols 0-31
 *     Block 3: rows 32-63, cols 32-63
 *
 * Each screen block is 32 rows × 32 columns × 2 bytes = 2048 bytes.
 * When x >= 32, we're in the right half (block 1 or 3).
 * When y >= 32, we're in the bottom half (block 2 or 3).
 *
 * The index computation accounts for this layout by adding offsets
 * of 0x20 (32) per screen block boundary crossed.
 *
 * The coordinate wrapping (& screenWidth-1, & screenHeight-1) makes
 * the tilemap repeat/wrap at the edges.
 */
u32 GetTileMapIndexFromCoords(s32 x, s32 y, s32 screenSize, u32 screenWidth, u32 screenHeight)
{
    x = x & (screenWidth - 1);
    y = y & (screenHeight - 1);

    switch (screenSize)
    {
        case 0:
        case 2:
            break;
        case 3:
            if (y >= 0x20)
                y += 0x20;
        case 1:
            if (x >= 0x20)
            {
                x -= 0x20;
                y += 0x20;
            }
    }
    return (y * 0x20) + x;
}

/*
 * CopyTileMapEntry - Copy one tilemap entry with palette/offset adjustments.
 *
 * A text-mode tilemap entry is 16 bits:
 *   Bits 0-9:   Tile index (which 8x8 tile from the tileset)
 *   Bit 10:     Horizontal flip
 *   Bit 11:     Vertical flip
 *   Bits 12-15: Palette number (in 4bpp mode)
 *
 * This function can:
 *   palette1 = 0-15: Set a specific palette number (replaces bits 12-15)
 *   palette1 = 16: Keep existing palette, just update tile index
 *   palette1 = 17+: Copy raw value with offset adjustments
 *
 * tileOffset adds to the tile index (for tile base adjustment).
 * palette2 adds to the palette number (for palette base adjustment).
 */
void CopyTileMapEntry(const u16 *src, u16 *dest, s32 palette1, s32 tileOffset, s32 palette2)
{
    u16 var;

    switch (palette1)
    {
    case 0 ... 15:
        var = ((*src + tileOffset) & 0xFFF) + ((palette1 + palette2) << 12);
        break;
    case 16:
        var = *dest;
        var &= 0xFC00;
        var += palette2 << 12;
        var |= (*src + tileOffset) & 0x3FF;
        break;
    default:
    case 17 ... INT_MAX:
        var = *src + tileOffset + (palette2 << 12);
        break;
    }
    *dest = var;
}

/*
 * GetBgType - Determine if a BG layer is text mode (0) or affine mode (1).
 *
 * Depends on both the BG index and the current BG mode:
 *   Mode 0: All 4 BGs are text (type 0)
 *   Mode 1: BG0-1 = text, BG2 = affine (type 1)
 *   Mode 2: BG2-3 = affine, BG0-1 = unavailable
 *
 * Returns 0xFFFF for invalid combinations (e.g., BG3 in mode 1).
 */
u32 GetBgType(u8 bg)
{
    u8 mode;

    mode = GetBgMode();


    switch (bg)
    {
        case 0:
        case 1:
            switch (mode)
            {
                case 0:
                case 1:
                    return 0;
            }
            break;
        case 2:
            switch (mode)
            {
                case 0:
                    return 0;
                case 1:
                case 2:
                    return 1;
            }
            break;
        case 3:
            switch (mode)
            {
                case 0:
                    return 0;
                case 2:
                    return 1;
            }
            break;
    }

    return 0xFFFF;
}

bool32 IsInvalidBg32(u8 bg)
{
    if (bg > 3)
        return TRUE;
    return FALSE;
}

/*
 * IsTileMapOutsideWram - Check if the tilemap buffer pointer is valid.
 *
 * The tilemap buffer must be in EWRAM (0x02000000-0x0203FFFF) or
 * IWRAM (0x03000000-0x03007FFF). If it points beyond IWRAM_END or
 * is NULL, the buffer is invalid and tilemap operations are skipped.
 *
 * This prevents crashes from writing to ROM, hardware registers,
 * or unmapped memory if a BG was configured without a tilemap buffer.
 */
bool32 IsTileMapOutsideWram(u8 bg)
{
    if (sGpuBgConfigs2[bg].tilemap > (void *)IWRAM_END)
        return TRUE;
    if (sGpuBgConfigs2[bg].tilemap == 0x0)
        return TRUE;
    return FALSE;
}
