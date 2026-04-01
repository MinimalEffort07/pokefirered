/*
 * sprite.c - Sprite (OBJ) System
 *
 * ============================================================================
 * GBA SPRITE HARDWARE (OAM - Object Attribute Memory)
 * ============================================================================
 *
 * The GBA can display up to 128 hardware sprites, called "objects" (OBJ).
 * Unlike backgrounds (which are tile grids), sprites are independently
 * positioned graphics that can be placed anywhere on screen.
 *
 * OAM (Object Attribute Memory) at 0x07000000 (1 KB):
 *   128 entries, each 8 bytes (6 attribute bytes + 2 affine parameter bytes).
 *   Each entry defines one sprite's position, size, tile, palette, and flags.
 *
 * OAM ENTRY FORMAT (6 bytes of attributes per sprite):
 *
 *   Attribute 0 (16 bits):
 *     Bits 0-7:   Y coordinate (0-255, wraps at 256)
 *     Bits 8-9:   OBJ Mode:
 *                   0 = Normal
 *                   1 = Semi-transparent (alpha blending)
 *                   2 = OBJ Window (masks other layers)
 *                   3 = Prohibited
 *     Bits 10-11: GFX Mode (affine):
 *                   0 = Off (no rotation/scaling)
 *                   1 = On (uses affine matrix)
 *                   2 = Off + hidden (sprite not rendered)
 *                   3 = On + double-size rendering area
 *     Bit 12:     Mosaic enable
 *     Bit 13:     Color mode (0 = 4bpp/16 palettes, 1 = 8bpp/256 colors)
 *     Bits 14-15: Shape (0 = square, 1 = horizontal rect, 2 = vertical rect)
 *
 *   Attribute 1 (16 bits):
 *     Bits 0-8:   X coordinate (0-511, wraps at 512)
 *     Bits 9-13:  Affine matrix index (if affine enabled) OR
 *                 Bit 12: Horizontal flip, Bit 13: Vertical flip (if affine off)
 *     Bits 14-15: Size (0-3, combined with shape determines pixel dimensions)
 *
 *   Attribute 2 (16 bits):
 *     Bits 0-9:   Tile index (which 8x8 tile from OBJ VRAM, 0-1023)
 *     Bits 10-11: Priority relative to BGs (0 = on top, 3 = behind all BGs)
 *     Bits 12-15: Palette number (in 4bpp mode)
 *
 * SPRITE SIZES (Shape × Size):
 *   Shape=Square:   8×8,  16×16, 32×32, 64×64
 *   Shape=H-Rect:   16×8, 32×8,  32×16, 64×32
 *   Shape=V-Rect:   8×16, 8×32,  16×32, 32×64
 *
 * OBJ VRAM (0x06010000, 32 KB):
 *   Sprite tile graphics, separate from BG VRAM.
 *   1024 tiles available (TOTAL_OBJ_TILE_COUNT).
 *   In 4bpp mode: each tile = 32 bytes.
 *   In 8bpp mode: each tile = 64 bytes (uses 2 tile slots).
 *
 * OBJ PALETTE (0x05000200, 256 bytes):
 *   16 palettes × 16 colors each (4bpp mode).
 *   The palette index in OAM selects which palette a sprite uses.
 *   OBJ palette is separate from BG palette (at 0x05000000).
 *
 * AFFINE SPRITES (rotation/scaling):
 *   32 affine matrices available. Each matrix is 4 × 16-bit parameters
 *   (PA, PB, PC, PD) that define a 2D transformation.
 *   The matrix data is INTERLEAVED with OAM attribute data:
 *     OAM[0].attr3 = Matrix[0].PA
 *     OAM[1].attr3 = Matrix[0].PB
 *     OAM[2].attr3 = Matrix[0].PC
 *     OAM[3].attr3 = Matrix[0].PD
 *     OAM[4].attr3 = Matrix[1].PA
 *     ...
 *   This means every 4th OAM entry shares space with affine params.
 *   CopyMatricesToOamBuffer() writes the matrix data into the correct
 *   interleaved positions.
 *
 * ============================================================================
 * SOFTWARE SPRITE SYSTEM
 * ============================================================================
 *
 * The game's sprite system adds significant functionality on top of
 * the raw OAM hardware:
 *
 * SPRITE STRUCT (gSprites[MAX_SPRITES]):
 *   Each software sprite (struct Sprite) contains:
 *   - oam: The OAM attribute data to write to hardware
 *   - x, y: Sprite position (world coordinates)
 *   - x2, y2: Position offsets (for animation/effects)
 *   - centerToCornerVecX/Y: Offset from center to top-left corner
 *   - callback: Per-frame update function (like a mini-game-loop per sprite)
 *   - anims: Pointer to animation command table
 *   - affineAnims: Pointer to affine animation command table
 *   - data[8]: General-purpose s16 storage (used by callbacks for state)
 *   - template: Original creation template (for reference)
 *   - subpriority: Fine-grained draw order within same OAM priority
 *   - inUse: Whether this sprite slot is allocated
 *   - invisible: Whether to skip rendering (but still update)
 *   - coordOffsetEnabled: Whether to apply camera scroll offset
 *
 * PER-FRAME PIPELINE (called from OverworldBasic/CB2):
 *   1. AnimateSprites():
 *      For each active sprite, call its callback function, then
 *      advance its animation state (frame animation + affine animation).
 *
 *   2. BuildOamBuffer():
 *      a. UpdateOamCoords() - Convert world coords to screen coords,
 *         applying camera offset (gSpriteCoordOffsetX/Y) if enabled.
 *      b. BuildSpritePriorities() - Compute sort key from priority+subpriority.
 *      c. SortSprites() - Insertion sort by priority, then by Y coordinate
 *         (lower Y = drawn first = behind higher Y sprites).
 *      d. AddSpritesToOamBuffer() - Copy sorted sprite OAM data to the
 *         output buffer (gMain.oamBuffer).
 *      e. CopyMatricesToOamBuffer() - Write affine matrices interleaved.
 *
 *   3. During VBlank (LoadOam()):
 *      DMA-copy gMain.oamBuffer to hardware OAM (0x07000000).
 *
 * TILE MANAGEMENT:
 *   Two modes for sprite tile graphics:
 *
 *   1. SHEET mode (usingSheet=TRUE):
 *      Tiles are pre-loaded to OBJ VRAM in a contiguous block ("sheet").
 *      Multiple sprites share the same sheet. sheetTileStart indexes into it.
 *      Used for: NPC sprites, player character, Pokemon sprites.
 *      Efficient: one DMA load, many sprites reference it.
 *
 *   2. IMAGE mode (usingSheet=FALSE):
 *      Each sprite allocates its own tile space via AllocSpriteTiles().
 *      Tile data is copied per-frame via sprite copy requests.
 *      Used for: animated sprites that change tile data frequently.
 *      Flexible but uses more VRAM and DMA bandwidth.
 *
 *   gSpriteTileAllocBitmap: Bitmap tracking allocated OBJ VRAM tiles.
 *   128 bytes = 1024 bits = 1024 tile slots.
 *
 * ANIMATION SYSTEM:
 *   Sprites can have frame-by-frame animations defined as command lists:
 *   - AnimCmd_frame: Display a specific tile frame for N VBlanks
 *   - AnimCmd_end: Stop animation
 *   - AnimCmd_jump: Jump to a different animation index
 *   - AnimCmd_loop: Repeat a section N times
 *
 *   Affine animations work similarly but modify the transformation matrix:
 *   - AffineAnimCmd_frame: Set/adjust rotation, scale, position
 *   - AffineAnimCmd_end/jump/loop: Flow control
 *
 * ============================================================================
 */
#include "global.h"
#include "gflib.h"

#define MAX_SPRITE_COPY_REQUESTS 64

#define OAM_MATRIX_COUNT 32

#define sAnchorX data[6]
#define sAnchorY data[7]

#define SET_SPRITE_TILE_RANGE(index, start, count) \
{                                                  \
    sSpriteTileRanges[index * 2] = start;          \
    (sSpriteTileRanges + 1)[index * 2] = count;    \
}

#define ALLOC_SPRITE_TILE(n)                              \
{                                                         \
    gSpriteTileAllocBitmap[(n) >> 3] |= (1 << ((n) & 7)); \
}

#define FREE_SPRITE_TILE(n)                                \
{                                                          \
    gSpriteTileAllocBitmap[(n) >> 3] &= ~(1 << ((n) & 7)); \
}

#define SPRITE_TILE_IS_ALLOCATED(n) ((gSpriteTileAllocBitmap[(n) >> 3] >> ((n) & 7)) & 1)


struct SpriteCopyRequest
{
    const u8 *src;
    u8 *dest;
    u16 size;
};

struct OamDimensions
{
    s8 width;
    s8 height;
};

static void UpdateOamCoords(void);
static void BuildSpritePriorities(void);
static void SortSprites(void);
static void CopyMatricesToOamBuffer(void);
static void AddSpritesToOamBuffer(void);
static u8 CreateSpriteAt(u8 index, const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority);
static void ResetOamMatrices(void);
static void ResetSprite(struct Sprite *sprite);
s16 AllocSpriteTiles(u16 tileCount);
static void RequestSpriteFrameImageCopy(u16 index, u16 tileNum, const struct SpriteFrameImage *images);
static void ResetAllSprites(void);
static void BeginAnim(struct Sprite *sprite);
static void ContinueAnim(struct Sprite *sprite);
static void AnimCmd_frame(struct Sprite *sprite);
static void AnimCmd_end(struct Sprite *sprite);
static void AnimCmd_jump(struct Sprite *sprite);
static void AnimCmd_loop(struct Sprite *sprite);
static void BeginAnimLoop(struct Sprite *sprite);
static void ContinueAnimLoop(struct Sprite *sprite);
static void JumpToTopOfAnimLoop(struct Sprite *sprite);
static void BeginAffineAnim(struct Sprite *sprite);
static void ContinueAffineAnim(struct Sprite *sprite);
static void AffineAnimDelay(u8 matrixNum, struct Sprite *sprite);
static void AffineAnimCmd_loop(u8 matrixNum, struct Sprite *sprite);
static void BeginAffineAnimLoop(u8 matrixNum, struct Sprite *sprite);
static void ContinueAffineAnimLoop(u8 matrixNum, struct Sprite *sprite);
static void JumpToTopOfAffineAnimLoop(u8 matrixNum, struct Sprite *sprite);
static void AffineAnimCmd_jump(u8 matrixNum, struct Sprite *sprite);
static void AffineAnimCmd_end(u8 matrixNum, struct Sprite *sprite);
static void AffineAnimCmd_frame(u8 matrixNum, struct Sprite *sprite);
static void CopyOamMatrix(u8 destMatrixIndex, struct OamMatrix *srcMatrix);
static u8 GetSpriteMatrixNum(struct Sprite *sprite);
static void SetSpriteOamFlipBits(struct Sprite *sprite, u8 hFlip, u8 vFlip);
static void AffineAnimStateRestartAnim(u8 matrixNum);
static void AffineAnimStateStartAnim(u8 matrixNum, u8 animNum);
static void AffineAnimStateReset(u8 matrixNum);
static void ApplyAffineAnimFrameAbsolute(u8 matrixNum, struct AffineAnimFrameCmd *frameCmd);
static void DecrementAnimDelayCounter(struct Sprite *sprite);
static bool8 DecrementAffineAnimDelayCounter(struct Sprite *sprite, u8 matrixNum);
static void ApplyAffineAnimFrameRelativeAndUpdateMatrix(u8 matrixNum, struct AffineAnimFrameCmd *frameCmd);
static s16 ConvertScaleParam(s16 scale);
static void GetAffineAnimFrame(u8 matrixNum, struct Sprite *sprite, struct AffineAnimFrameCmd *frameCmd);
static void ApplyAffineAnimFrame(u8 matrixNum, struct AffineAnimFrameCmd *frameCmd);
static u8 IndexOfSpriteTileTag(u16 tag);
static void AllocSpriteTileRange(u16 tag, u16 start, u16 count);
static void DoLoadSpritePalette(const u16 *src, u16 paletteOffset);
static void UpdateSpriteMatrixAnchorPos(struct Sprite* sprite, s32 a1, s32 a2);

typedef void (*AnimFunc)(struct Sprite *);
typedef void (*AnimCmdFunc)(struct Sprite *);
typedef void (*AffineAnimCmdFunc)(u8 matrixNum, struct Sprite *);

#define DUMMY_OAM_DATA        \
{                             \
    160, /* Y (off-screen) */ \
    ST_OAM_AFFINE_OFF,        \
    ST_OAM_OBJ_NORMAL,        \
    FALSE,                    \
    ST_OAM_4BPP,              \
    ST_OAM_SQUARE,            \
    304, /* X */              \
    0,                        \
    ST_OAM_SIZE_0,            \
    0x000,                    \
    3, /* lowest priority */  \
    0x0,                      \
    0                         \
}

#define ANIM_END        0xFFFF
#define AFFINE_ANIM_END 0x7FFF

// forward declarations
const union AnimCmd * const gDummySpriteAnimTable[];
const union AffineAnimCmd * const gDummySpriteAffineAnimTable[];
const struct SpriteTemplate gDummySpriteTemplate;

// Unreferenced error message.
// It means "The DMA transfer request table has exceeded its limit."
static const u8 sDmaOverErrorMsg[] =
    _(
    "DMA OVER\n"
    "DMAてんそう\n"
    "リクエストテーブルが\n"
    "オーバーしました"
);

// Unreferenced data. Also unreferenced in R/S.
static const u8 sUnknownData[24] =
{
    0x01, 0x04, 0x10, 0x40,
    0x02, 0x04, 0x08, 0x20,
    0x02, 0x04, 0x08, 0x20,
    0x01, 0x04, 0x10, 0x40,
    0x02, 0x04, 0x08, 0x20,
    0x02, 0x04, 0x08, 0x20,
};

static const u8 sCenterToCornerVecTable[3][4][2] =
{
    {   // square
        {  -4,  -4 },
        {  -8,  -8 },
        { -16, -16 },
        { -32, -32 },
    },
    {   // horizontal rectangle
        {  -8,  -4 },
        { -16,  -4 },
        { -16,  -8 },
        { -32, -16 },
    },
    {   // vertical rectangle
        {  -4,  -8 },
        {  -4, -16 },
        {  -8, -16 },
        { -16, -32 },
    },
};

static const struct Sprite sDummySprite =
{
    .oam = DUMMY_OAM_DATA,
    .anims = gDummySpriteAnimTable,
    .affineAnims = gDummySpriteAffineAnimTable,
    .template = &gDummySpriteTemplate,
    .callback = SpriteCallbackDummy,
    .x = DISPLAY_WIDTH + 64,
    .y = DISPLAY_HEIGHT,
    .subpriority = 0xFF
};

const struct OamData gDummyOamData = DUMMY_OAM_DATA;

static const union AnimCmd sDummyAnim = { ANIM_END };

const union AnimCmd * const gDummySpriteAnimTable[] = { &sDummyAnim };

static const union AffineAnimCmd sDummyAffineAnim = { AFFINE_ANIM_END };

const union AffineAnimCmd * const gDummySpriteAffineAnimTable[] = { &sDummyAffineAnim };

const struct SpriteTemplate gDummySpriteTemplate =
{
    .tileTag = 0,
    .paletteTag = TAG_NONE,
    .oam = &gDummyOamData,
    .anims = gDummySpriteAnimTable,
    .images = NULL,
    .affineAnims = gDummySpriteAffineAnimTable,
    .callback = SpriteCallbackDummy
};

static const AnimFunc sAnimFuncs[] =
{
    ContinueAnim,
    BeginAnim,
};

static const AnimFunc sAffineAnimFuncs[] =
{
    ContinueAffineAnim,
    BeginAffineAnim,
};

static const AnimCmdFunc sAnimCmdFuncs[] =
{
    AnimCmd_loop,
    AnimCmd_jump,
    AnimCmd_end,
    AnimCmd_frame,
};

static const AffineAnimCmdFunc sAffineAnimCmdFuncs[] =
{
    AffineAnimCmd_loop,
    AffineAnimCmd_jump,
    AffineAnimCmd_end,
    AffineAnimCmd_frame,
};

static const s32 sOamDimensionsCopy[3][4][2] =
{
    [ST_OAM_SQUARE] = {
        [ST_OAM_SIZE_0] = { 8,  8}, // SPRITE_SIZE_8x8
        [ST_OAM_SIZE_1] = {16, 16}, // SPRITE_SIZE_16x16
        [ST_OAM_SIZE_2] = {32, 32}, // SPRITE_SIZE_32x32
        [ST_OAM_SIZE_3] = {64, 64}, // SPRITE_SIZE_64x64
    },
    [ST_OAM_H_RECTANGLE] = {
        [ST_OAM_SIZE_0] = {16,  8}, // SPRITE_SIZE_16x8
        [ST_OAM_SIZE_1] = {32,  8}, // SPRITE_SIZE_32x8
        [ST_OAM_SIZE_2] = {32, 16}, // SPRITE_SIZE_32x16
        [ST_OAM_SIZE_3] = {64, 32}, // SPRITE_SIZE_64x32
    },
    [ST_OAM_V_RECTANGLE] = {
        [ST_OAM_SIZE_0] = { 8, 16}, // SPRITE_SIZE_8x16
        [ST_OAM_SIZE_1] = { 8, 32}, // SPRITE_SIZE_8x32
        [ST_OAM_SIZE_2] = {16, 32}, // SPRITE_SIZE_16x32
        [ST_OAM_SIZE_3] = {32, 64}, // SPRITE_SIZE_32x64
    },
};

static const struct OamDimensions sOamDimensions[3][4] =
{
    [ST_OAM_SQUARE] = {
        [ST_OAM_SIZE_0] = { 8,  8}, // SPRITE_SIZE_8x8
        [ST_OAM_SIZE_1] = {16, 16}, // SPRITE_SIZE_16x16
        [ST_OAM_SIZE_2] = {32, 32}, // SPRITE_SIZE_32x32
        [ST_OAM_SIZE_3] = {64, 64}, // SPRITE_SIZE_64x64
    },
    [ST_OAM_H_RECTANGLE] = {
        [ST_OAM_SIZE_0] = {16,  8}, // SPRITE_SIZE_16x8
        [ST_OAM_SIZE_1] = {32,  8}, // SPRITE_SIZE_32x8
        [ST_OAM_SIZE_2] = {32, 16}, // SPRITE_SIZE_32x16
        [ST_OAM_SIZE_3] = {64, 32}, // SPRITE_SIZE_64x32
    },
    [ST_OAM_V_RECTANGLE] = {
        [ST_OAM_SIZE_0] = { 8, 16}, // SPRITE_SIZE_8x16
        [ST_OAM_SIZE_1] = { 8, 32}, // SPRITE_SIZE_8x32
        [ST_OAM_SIZE_2] = {16, 32}, // SPRITE_SIZE_16x32
        [ST_OAM_SIZE_3] = {32, 64}, // SPRITE_SIZE_32x64
    },
};

static u16 sSpriteTileRangeTags[MAX_SPRITES];
static u16 sSpriteTileRanges[MAX_SPRITES * 2];
static struct AffineAnimState sAffineAnimStates[OAM_MATRIX_COUNT];
static u16 sSpritePaletteTags[16];

COMMON_DATA u32 gOamMatrixAllocBitmap = 0;
COMMON_DATA u8 gReservedSpritePaletteCount = 0;

EWRAM_DATA struct Sprite gSprites[MAX_SPRITES + 1] = {0};
EWRAM_DATA u16 gSpritePriorities[MAX_SPRITES] = {0};
EWRAM_DATA u8 gSpriteOrder[MAX_SPRITES] = {0};
EWRAM_DATA bool8 gShouldProcessSpriteCopyRequests = 0;
EWRAM_DATA u8 gSpriteCopyRequestCount = 0;
EWRAM_DATA struct SpriteCopyRequest gSpriteCopyRequests[MAX_SPRITES] = {0};
EWRAM_DATA u8 gOamLimit = 0;
EWRAM_DATA u16 gReservedSpriteTileCount = 0;
EWRAM_DATA u8 gSpriteTileAllocBitmap[128] = {0};
EWRAM_DATA s16 gSpriteCoordOffsetX = 0;
EWRAM_DATA s16 gSpriteCoordOffsetY = 0;
EWRAM_DATA struct OamMatrix gOamMatrices[OAM_MATRIX_COUNT] = {0};
EWRAM_DATA bool8 gAffineAnimsDisabled = 0;

void ResetSpriteData(void)
{
    ResetOamRange(0, 128);
    ResetAllSprites();
    ClearSpriteCopyRequests();
    ResetAffineAnimData();
    FreeSpriteTileRanges();
    gOamLimit = 64;
    gReservedSpriteTileCount = 0;
    AllocSpriteTiles(0);
    gSpriteCoordOffsetX = 0;
    gSpriteCoordOffsetY = 0;
}

/*
 * AnimateSprites - Run all sprite callbacks and advance animations.
 *
 * Called every frame from OverworldBasic()/CB2.
 * For each active sprite:
 *   1. Call its callback function. This is the sprite's "brain" -
 *      it controls movement, behavior, and state. Examples:
 *      - SpriteCB_LinkPlayer: Updates link player position from network data
 *      - SpriteCB_RemotePlayer: Updates multiplayer remote player
 *      - SpriteCallbackDummy: Does nothing (static sprite)
 *   2. If still in use after callback, advance its animation.
 *      AnimateSprite() processes the current animation command list,
 *      advancing frames based on delay counters.
 *
 * A callback can destroy the sprite (set inUse=FALSE), which is why
 * we re-check inUse before calling AnimateSprite.
 */
void AnimateSprites(void)
{
    u8 i;
    for (i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *sprite = &gSprites[i];

        if (sprite->inUse)
        {
            sprite->callback(sprite);

            if (sprite->inUse)
                AnimateSprite(sprite);
        }
    }
}

/*
 * BuildOamBuffer - Convert software sprites into hardware OAM data.
 *
 * This is the sprite rendering pipeline. It reads all struct Sprite entries
 * and produces the 128-entry OAM buffer that gets DMA-copied to hardware
 * during VBlank.
 *
 * Pipeline:
 * 1. UpdateOamCoords: Convert each sprite's world position (x, y) to
 *    screen coordinates in oam.x and oam.y. Applies camera offset
 *    (gSpriteCoordOffsetX/Y) for sprites that scroll with the map.
 *
 * 2. BuildSpritePriorities: Compute a sort key for each sprite.
 *    High byte = OAM priority (0-3, from hardware priority field).
 *    Low byte = subpriority (0-255, software-defined fine ordering).
 *    Lower value = drawn on TOP (rendered last = appears in front).
 *
 * 3. SortSprites: Insertion sort by (priority, then Y coordinate).
 *    Sprites with equal priority are sorted by Y position so that
 *    sprites lower on screen (higher Y) overlap those above them.
 *    This creates the "depth" illusion in the overworld.
 *
 * 4. AddSpritesToOamBuffer: Copy sorted sprite OAM data to gMain.oamBuffer.
 *    Invisible sprites are skipped. Unused OAM slots are filled with
 *    the dummy OAM data (Y=160, offscreen) to hide them.
 *
 * 5. CopyMatricesToOamBuffer: Write the 32 affine matrices into the
 *    interleaved OAM positions (every 4th entry's affineParam field).
 *
 * gMain.oamLoadDisabled is set TRUE during buffer construction to prevent
 * the VBlank handler from DMA-copying a half-built buffer to hardware.
 * gShouldProcessSpriteCopyRequests signals that sprite tile copy requests
 * can be processed during the next VBlank.
 */
void BuildOamBuffer(void)
{
    u8 temp;
    UpdateOamCoords();
    BuildSpritePriorities();
    SortSprites();
    temp = gMain.oamLoadDisabled;
    gMain.oamLoadDisabled = TRUE;
    AddSpritesToOamBuffer();
    CopyMatricesToOamBuffer();
    gMain.oamLoadDisabled = temp;
    gShouldProcessSpriteCopyRequests = TRUE;
}

/*
 * UpdateOamCoords - Convert world coordinates to screen coordinates.
 *
 * Sprites store their position as world coordinates (sprite->x, sprite->y).
 * The OAM hardware needs SCREEN coordinates (0,0 = top-left of display).
 *
 * The conversion:
 *   screen_x = world_x + offset_x + center_to_corner_x + camera_x
 *   screen_y = world_y + offset_y + center_to_corner_y + camera_y
 *
 * Components:
 *   x, y:       Base world position (set by game logic)
 *   x2, y2:     Secondary offset (used for bobbing, shaking, etc.)
 *   centerToCornerVecX/Y: Converts center-origin to top-left-origin.
 *     OAM coordinates are the sprite's TOP-LEFT corner, but the game
 *     uses CENTER coordinates. This vector is negative half-dimensions
 *     (e.g., for a 16x32 sprite: centerToCornerVecX=-8, vecY=-16).
 *   gSpriteCoordOffsetX/Y: Camera scroll offset. Applied to sprites
 *     that move with the map (NPCs, items) but NOT to UI sprites
 *     (HP bars, text) which stay fixed on screen.
 *     coordOffsetEnabled controls whether this is applied.
 *
 * The camera offset is computed each frame based on the player's position.
 * When the player walks right, gSpriteCoordOffsetX decreases, making all
 * map sprites shift left on screen while the player stays centered.
 */
void UpdateOamCoords(void)
{
    u8 i;
    for (i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *sprite = &gSprites[i];
        if (sprite->inUse && !sprite->invisible)
        {
            if (sprite->coordOffsetEnabled)
            {
                sprite->oam.x = sprite->x + sprite->x2 + sprite->centerToCornerVecX + gSpriteCoordOffsetX;
                sprite->oam.y = sprite->y + sprite->y2 + sprite->centerToCornerVecY + gSpriteCoordOffsetY;
            }
            else
            {
                sprite->oam.x = sprite->x + sprite->x2 + sprite->centerToCornerVecX;
                sprite->oam.y = sprite->y + sprite->y2 + sprite->centerToCornerVecY;
            }
        }
    }
}

void BuildSpritePriorities(void)
{
    u16 i;
    for (i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *sprite = &gSprites[i];
        u16 priority = sprite->subpriority | (sprite->oam.priority << 8);
        gSpritePriorities[i] = priority;
    }
}

void SortSprites(void)
{
    u8 i;
    for (i = 1; i < MAX_SPRITES; i++)
    {
        u8 j = i;
        struct Sprite *sprite1 = &gSprites[gSpriteOrder[i - 1]];
        struct Sprite *sprite2 = &gSprites[gSpriteOrder[i]];
        u16 sprite1Priority = gSpritePriorities[gSpriteOrder[i - 1]];
        u16 sprite2Priority = gSpritePriorities[gSpriteOrder[i]];
        s16 sprite1Y = sprite1->oam.y;
        s16 sprite2Y = sprite2->oam.y;

        if (sprite1Y >= DISPLAY_HEIGHT)
            sprite1Y = sprite1Y - 256;

        if (sprite2Y >= DISPLAY_HEIGHT)
            sprite2Y = sprite2Y - 256;

        if (sprite1->oam.affineMode == ST_OAM_AFFINE_DOUBLE
         && sprite1->oam.size == 3)
        {
            u32 shape = sprite1->oam.shape;
            if (shape == ST_OAM_SQUARE || shape == 2)
            {
                if (sprite1Y > 128)
                    sprite1Y = sprite1Y - 256;
            }
        }

        if (sprite2->oam.affineMode == ST_OAM_AFFINE_DOUBLE
         && sprite2->oam.size == 3)
        {
            u32 shape = sprite2->oam.shape;
            if (shape == ST_OAM_SQUARE || shape == ST_OAM_V_RECTANGLE)
            {
                if (sprite2Y > 128)
                    sprite2Y = sprite2Y - 256;
            }
        }

        while (j > 0
            && ((sprite1Priority > sprite2Priority)
             || (sprite1Priority == sprite2Priority && sprite1Y < sprite2Y)))
        {
            u8 temp = gSpriteOrder[j];
            gSpriteOrder[j] = gSpriteOrder[j - 1];
            gSpriteOrder[j - 1] = temp;

            // UB: If j equals 1, then j-- makes j equal 0.
            // Then, gSpriteOrder[-1] gets accessed below.
            // Although this doesn't result in a bug in the ROM,
            // the behavior is undefined.
            j--;

            sprite1 = &gSprites[gSpriteOrder[j - 1]];
            sprite2 = &gSprites[gSpriteOrder[j]];
            sprite1Priority = gSpritePriorities[gSpriteOrder[j - 1]];
            sprite2Priority = gSpritePriorities[gSpriteOrder[j]];
            sprite1Y = sprite1->oam.y;
            sprite2Y = sprite2->oam.y;

            if (sprite1Y >= DISPLAY_HEIGHT)
                sprite1Y = sprite1Y - 256;

            if (sprite2Y >= DISPLAY_HEIGHT)
                sprite2Y = sprite2Y - 256;

            if (sprite1->oam.affineMode == ST_OAM_AFFINE_DOUBLE
             && sprite1->oam.size == 3)
            {
                u32 shape = sprite1->oam.shape;
                if (shape == ST_OAM_SQUARE || shape == ST_OAM_V_RECTANGLE)
                {
                    if (sprite1Y > 128)
                        sprite1Y = sprite1Y - 256;
                }
            }

            if (sprite2->oam.affineMode == ST_OAM_AFFINE_DOUBLE
             && sprite2->oam.size == 3)
            {
                u32 shape = sprite2->oam.shape;
                if (shape == ST_OAM_SQUARE || shape == ST_OAM_V_RECTANGLE)
                {
                    if (sprite2Y > 128)
                        sprite2Y = sprite2Y - 256;
                }
            }
        }
    }
}

/*
 * CopyMatricesToOamBuffer - Write affine matrices into the OAM buffer.
 *
 * GBA hardware quirk: affine matrix data is INTERLEAVED with OAM entries.
 * The 32 matrices share space with the 128 OAM entries.
 *
 * Each OAM entry is 8 bytes. Bytes 6-7 hold either sprite attribute 3
 * (for non-affine sprites) OR one affine parameter (for affine sprites).
 *
 * Matrix N uses the affineParam field of OAM entries 4N, 4N+1, 4N+2, 4N+3:
 *   OAM[4N+0].affineParam = PA (horizontal scale)
 *   OAM[4N+1].affineParam = PB (horizontal shear/rotation)
 *   OAM[4N+2].affineParam = PC (vertical shear/rotation)
 *   OAM[4N+3].affineParam = PD (vertical scale)
 *
 * For an identity matrix (no transformation): PA=PD=0x100, PB=PC=0.
 * For 2x zoom: PA=PD=0x80. For 90° rotation: PA=PD=0, PB=0x100, PC=-0x100.
 *
 * This interleaved layout is a hardware design choice by Nintendo to
 * save memory (no separate matrix memory needed). Software must write
 * to the correct interleaved positions.
 */
void CopyMatricesToOamBuffer(void)
{
    u8 i;
    for (i = 0; i < OAM_MATRIX_COUNT; i++)
    {
        u32 base = 4 * i;
        gMain.oamBuffer[base + 0].affineParam = gOamMatrices[i].a;
        gMain.oamBuffer[base + 1].affineParam = gOamMatrices[i].b;
        gMain.oamBuffer[base + 2].affineParam = gOamMatrices[i].c;
        gMain.oamBuffer[base + 3].affineParam = gOamMatrices[i].d;
    }
}

void AddSpritesToOamBuffer(void)
{
    int i = 0;
    u8 oamIndex = 0;

    while (i < MAX_SPRITES)
    {
        struct Sprite *sprite = &gSprites[gSpriteOrder[i]];
        if (sprite->inUse && !sprite->invisible && AddSpriteToOamBuffer(sprite, &oamIndex))
            return;
        i++;
    }

    while (oamIndex < gOamLimit)
    {
        gMain.oamBuffer[oamIndex] = gDummyOamData;
        oamIndex++;
    }
}

/*
 * CreateSprite - Allocate and initialize a new sprite.
 *
 * Finds the first free slot in gSprites[] and sets it up from the template.
 * Returns the sprite index, or MAX_SPRITES (64) if no free slot.
 *
 * SpriteTemplate contains:
 *   tileTag: Tag to look up pre-loaded tile data, or TAG_NONE for per-sprite tiles
 *   paletteTag: Tag to look up the palette slot, or TAG_NONE for manual palette
 *   oam: Default OAM attribute data (shape, size, priority)
 *   anims: Animation command table
 *   images: Tile data for per-sprite (non-sheet) tiles
 *   affineAnims: Affine animation command table
 *   callback: Per-frame update function
 *
 * The tag system: tile and palette data can be pre-loaded to VRAM with a
 * "tag" (a 16-bit identifier). CreateSprite looks up the tag to find where
 * in VRAM the tiles/palette were loaded. This avoids redundant VRAM loads
 * when multiple sprites share the same graphics.
 */
u8 CreateSprite(const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
        if (!gSprites[i].inUse)
            return CreateSpriteAt(i, template, x, y, subpriority);

    return MAX_SPRITES;
}

u8 CreateSpriteAtEnd(const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority)
{
    s16 i;

    for (i = MAX_SPRITES - 1; i > -1; i--)
        if (!gSprites[i].inUse)
            return CreateSpriteAt(i, template, x, y, subpriority);

    return MAX_SPRITES;
}

u8 CreateInvisibleSprite(void (*callback)(struct Sprite *))
{
    u8 index = CreateSprite(&gDummySpriteTemplate, 0, 0, 31);

    if (index == MAX_SPRITES)
    {
        return MAX_SPRITES;
    }
    else
    {
        gSprites[index].invisible = TRUE;
        gSprites[index].callback = callback;
        return index;
    }
}

u8 CreateSpriteAt(u8 index, const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority)
{
    struct Sprite *sprite = &gSprites[index];

    ResetSprite(sprite);

    sprite->inUse = TRUE;
    sprite->animBeginning = TRUE;
    sprite->affineAnimBeginning = TRUE;
    sprite->usingSheet = TRUE;

    sprite->subpriority = subpriority;
    sprite->oam = *template->oam;
    sprite->anims = template->anims;
    sprite->affineAnims = template->affineAnims;
    sprite->template = template;
    sprite->callback = template->callback;
    sprite->x = x;
    sprite->y = y;

    CalcCenterToCornerVec(sprite, sprite->oam.shape, sprite->oam.size, sprite->oam.affineMode);

    if (template->tileTag == TAG_NONE)
    {
        s16 tileNum;
        sprite->images = template->images;
        tileNum = AllocSpriteTiles((u8)(sprite->images->size / TILE_SIZE_4BPP));
        if (tileNum == -1)
        {
            ResetSprite(sprite);
            return MAX_SPRITES;
        }
        sprite->oam.tileNum = tileNum;
        sprite->usingSheet = FALSE;
        sprite->sheetTileStart = 0;
    }
    else
    {
        sprite->sheetTileStart = GetSpriteTileStartByTag(template->tileTag);
        SetSpriteSheetFrameTileNum(sprite);
    }

    if (sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK)
        InitSpriteAffineAnim(sprite);

    if (template->paletteTag != TAG_NONE)
        sprite->oam.paletteNum = IndexOfSpritePaletteTag(template->paletteTag);

    return index;
}

u8 CreateSpriteAndAnimate(const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *sprite = &gSprites[i];

        if (!gSprites[i].inUse)
        {
            u8 index = CreateSpriteAt(i, template, x, y, subpriority);

            if (index == MAX_SPRITES)
                return MAX_SPRITES;

            gSprites[i].callback(sprite);

            if (gSprites[i].inUse)
                AnimateSprite(sprite);

            return index;
        }
    }

    return MAX_SPRITES;
}

/*
 * DestroySprite - Free a sprite slot and its allocated VRAM tiles.
 *
 * If the sprite was using per-sprite tile allocation (usingSheet=FALSE),
 * its OBJ VRAM tiles are freed via the tile allocation bitmap.
 * Sheet-mode sprites don't free tiles here because the sheet is
 * shared with other sprites.
 *
 * ResetSprite() overwrites the struct with sDummySprite, which sets:
 *   inUse=FALSE, position off-screen, callback=SpriteCallbackDummy.
 * This effectively removes the sprite from all processing.
 */
void DestroySprite(struct Sprite *sprite)
{
    if (sprite->inUse)
    {
        if (!sprite->usingSheet)
        {
            u16 i;
            u16 tileEnd = (sprite->images->size / TILE_SIZE_4BPP) + sprite->oam.tileNum;
            for (i = sprite->oam.tileNum; i < tileEnd; i++)
                FREE_SPRITE_TILE(i);
        }
        ResetSprite(sprite);
    }
}

void ResetOamRange(u8 a, u8 b)
{
    u8 i;

    for (i = a; i < b; i++)
    {
        struct OamData *oamBuffer = gMain.oamBuffer;
        oamBuffer[i] = *(struct OamData *)&gDummyOamData;
    }
}

/*
 * LoadOam - Copy the software OAM buffer to hardware OAM.
 *
 * Called during VBlank from VBlankCB_Field (or equivalent).
 * CpuCopy32 uses a BIOS call (SWI 0x0C) for fast 32-bit block copy.
 *
 * gMain.oamBuffer (1 KB in RAM) → OAM hardware (0x07000000, 1 KB).
 * This is the ONLY time OAM hardware is written. All sprite position/
 * attribute changes during the frame modify the RAM buffer only.
 *
 * OAM can only be safely written during VBlank or HBlank. Writing
 * during active display causes sprite corruption (sprites may flicker,
 * display garbage tiles, or appear at wrong positions).
 *
 * oamLoadDisabled prevents copying during BuildOamBuffer() to avoid
 * the VBlank handler reading a half-built buffer.
 */
void LoadOam(void)
{
    if (!gMain.oamLoadDisabled)
        CpuCopy32(gMain.oamBuffer, (void *)OAM, sizeof(gMain.oamBuffer));
}

void ClearSpriteCopyRequests(void)
{
    u8 i;

    gShouldProcessSpriteCopyRequests = FALSE;
    gSpriteCopyRequestCount = 0;

    for (i = 0; i < MAX_SPRITE_COPY_REQUESTS; i++)
    {
        gSpriteCopyRequests[i].src = 0;
        gSpriteCopyRequests[i].dest = 0;
        gSpriteCopyRequests[i].size = 0;
    }
}

/*
 * ResetOamMatrices - Set all 32 affine matrices to identity (no transform).
 *
 * The identity matrix [0x100, 0, 0, 0x100] means:
 *   PA = 0x100 (1.0 in 8.8 fixed-point) = no horizontal scaling
 *   PB = 0 = no horizontal shear/rotation
 *   PC = 0 = no vertical shear/rotation
 *   PD = 0x100 (1.0) = no vertical scaling
 *
 * Affine parameters use 8.8 fixed-point: 0x100 = 1.0, 0x80 = 0.5, 0x200 = 2.0.
 * Confusingly, LARGER values mean SMALLER sprites (it's an inverse scale).
 */
void ResetOamMatrices(void)
{
    u8 i;
    for (i = 0; i < OAM_MATRIX_COUNT; i++)
    {
        // set to identity matrix
        gOamMatrices[i].a = 0x0100;
        gOamMatrices[i].b = 0x0000;
        gOamMatrices[i].c = 0x0000;
        gOamMatrices[i].d = 0x0100;
    }
}

void SetOamMatrix(u8 matrixNum, u16 a, u16 b, u16 c, u16 d)
{
    gOamMatrices[matrixNum].a = a;
    gOamMatrices[matrixNum].b = b;
    gOamMatrices[matrixNum].c = c;
    gOamMatrices[matrixNum].d = d;
}

void ResetSprite(struct Sprite *sprite)
{
    *sprite = sDummySprite;
}

void CalcCenterToCornerVec(struct Sprite *sprite, u8 shape, u8 size, u8 affineMode)
{
    u8 x = sCenterToCornerVecTable[shape][size][0];
    u8 y = sCenterToCornerVecTable[shape][size][1];

    if (affineMode & ST_OAM_AFFINE_DOUBLE_MASK)
    {
        x *= 2;
        y *= 2;
    }

    sprite->centerToCornerVecX = x;
    sprite->centerToCornerVecY = y;
}

/*
 * AllocSpriteTiles - Allocate contiguous tile slots in OBJ VRAM.
 *
 * OBJ VRAM (32 KB) is divided into 1024 tile slots (32 bytes each in 4bpp).
 * This function finds a contiguous block of 'tileCount' free slots.
 * Uses a first-fit algorithm scanning gSpriteTileAllocBitmap.
 *
 * gReservedSpriteTileCount: tiles 0..N-1 are reserved (never allocated).
 * Reserved tiles are typically used for fixed graphics that persist
 * across screen changes (player sprite sheet, etc.)
 *
 * Special case: tileCount=0 frees ALL non-reserved tiles.
 * This is called during ResetSpriteData() to wipe OBJ VRAM clean.
 *
 * Returns the starting tile index, or -1 if not enough contiguous space.
 * The caller stores this in sprite->oam.tileNum.
 */
s16 AllocSpriteTiles(u16 tileCount)
{
    u16 i;
    s16 start;
    u16 numTilesFound;

    if (tileCount == 0)
    {
        // Free all unreserved tiles if the tile count is 0.
        for (i = gReservedSpriteTileCount; i < TOTAL_OBJ_TILE_COUNT; i++)
            FREE_SPRITE_TILE(i);

        return 0;
    }

    i = gReservedSpriteTileCount;

    for (;;)
    {
        while (SPRITE_TILE_IS_ALLOCATED(i))
        {
            i++;

            if (i == TOTAL_OBJ_TILE_COUNT)
                return -1;
        }

        start = i;
        numTilesFound = 1;

        while (numTilesFound != tileCount)
        {
            i++;

            if (i == TOTAL_OBJ_TILE_COUNT)
                return -1;

            if (!SPRITE_TILE_IS_ALLOCATED(i))
                numTilesFound++;
            else
                break;
        }

        if (numTilesFound == tileCount)
            break;
    }

    for (i = start; i < tileCount + start; i++)
        ALLOC_SPRITE_TILE(i);

    return start;
}

u8 SpriteTileAllocBitmapOp(u16 bit, u8 op)
{
    u8 index = bit / 8;
    u8 shift = bit % 8;
    u8 val = bit % 8;
    u8 retVal = 0;

    if (op == 0) // clear
    {
        val = ~(1 << val);
        gSpriteTileAllocBitmap[index] &= val;
    }
    else if (op == 1) // set
    {
        val = (1 << val);
        gSpriteTileAllocBitmap[index] |= val;
    }
    else // check
    {
        retVal = 1 << shift;
        retVal &= gSpriteTileAllocBitmap[index];
    }

    return retVal;
}

void FreeSpriteTilesIfNotUsingSheet(struct Sprite *sprite)
{
    if (!sprite->usingSheet)
    {
        int i;
        int end = (sprite->images[0].size / TILE_SIZE_4BPP) + sprite->oam.tileNum;

        for (i = sprite->oam.tileNum; i < end; i++)
            FREE_SPRITE_TILE(i);
    }
}

void SpriteCallbackDummy(struct Sprite *sprite)
{
}

void ProcessSpriteCopyRequests(void)
{
    if (gShouldProcessSpriteCopyRequests)
    {
        u8 i = 0;

        while (gSpriteCopyRequestCount > 0)
        {
            CpuCopy16(gSpriteCopyRequests[i].src, gSpriteCopyRequests[i].dest, gSpriteCopyRequests[i].size);
            gSpriteCopyRequestCount--;
            i++;
        }

        gShouldProcessSpriteCopyRequests = FALSE;
    }
}

void RequestSpriteFrameImageCopy(u16 index, u16 tileNum, const struct SpriteFrameImage *images)
{
    if (gSpriteCopyRequestCount < MAX_SPRITE_COPY_REQUESTS)
    {
        gSpriteCopyRequests[gSpriteCopyRequestCount].src = images[index].data;
        gSpriteCopyRequests[gSpriteCopyRequestCount].dest = (u8 *)OBJ_VRAM0 + TILE_SIZE_4BPP * tileNum;
        gSpriteCopyRequests[gSpriteCopyRequestCount].size = images[index].size;
        gSpriteCopyRequestCount++;
    }
}

void RequestSpriteCopy(const u8 *src, u8 *dest, u16 size)
{
    if (gSpriteCopyRequestCount < MAX_SPRITE_COPY_REQUESTS)
    {
        gSpriteCopyRequests[gSpriteCopyRequestCount].src = src;
        gSpriteCopyRequests[gSpriteCopyRequestCount].dest = dest;
        gSpriteCopyRequests[gSpriteCopyRequestCount].size = size;
        gSpriteCopyRequestCount++;
    }
}

void CopyFromSprites(u8 *dest)
{
    u32 i;
    u8 *src = (u8 *)gSprites;
    for (i = 0; i < sizeof(struct Sprite) * MAX_SPRITES; i++)
    {
        *dest = *src;
        dest++;
        src++;
    }
}

void CopyToSprites(u8 *src)
{
    u32 i;
    u8 *dest = (u8 *)gSprites;
    for (i = 0; i < sizeof(struct Sprite) * MAX_SPRITES; i++)
    {
        *dest = *src;
        src++;
        dest++;
    }
}

void ResetAllSprites(void)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        ResetSprite(&gSprites[i]);
        gSpriteOrder[i] = i;
    }

    ResetSprite(&gSprites[i]);
}

void FreeSpriteTiles(struct Sprite *sprite)
{
    if (sprite->template->tileTag != TAG_NONE)
        FreeSpriteTilesByTag(sprite->template->tileTag);
}

void FreeSpritePalette(struct Sprite *sprite)
{
    FreeSpritePaletteByTag(sprite->template->paletteTag);
}

void FreeSpriteOamMatrix(struct Sprite *sprite)
{
    if (sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK)
    {
        FreeOamMatrix(sprite->oam.matrixNum);
        sprite->oam.affineMode = ST_OAM_AFFINE_OFF;
    }
}

void DestroySpriteAndFreeResources(struct Sprite *sprite)
{
    FreeSpriteTiles(sprite);
    FreeSpritePalette(sprite);
    FreeSpriteOamMatrix(sprite);
    DestroySprite(sprite);
}

void AnimateSprite(struct Sprite *sprite)
{
    sAnimFuncs[sprite->animBeginning](sprite);

    if (!gAffineAnimsDisabled)
        sAffineAnimFuncs[sprite->affineAnimBeginning](sprite);
}

void BeginAnim(struct Sprite *sprite)
{
    s16 imageValue;
    u8 duration;
    u8 hFlip;
    u8 vFlip;

    sprite->animCmdIndex = 0;
    sprite->animEnded = FALSE;
    sprite->animLoopCounter = 0;
    imageValue = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.imageValue;

    if (imageValue != -1)
    {
        sprite->animBeginning = FALSE;
        duration = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.duration;
        hFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.hFlip;
        vFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.vFlip;

        if (duration)
            duration--;

        sprite->animDelayCounter = duration;

        if (!(sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK))
            SetSpriteOamFlipBits(sprite, hFlip, vFlip);

        if (sprite->usingSheet)
            sprite->oam.tileNum = sprite->sheetTileStart + imageValue;
        else
            RequestSpriteFrameImageCopy(imageValue, sprite->oam.tileNum, sprite->images);
    }
}

void ContinueAnim(struct Sprite *sprite)
{
    if (sprite->animDelayCounter)
    {
        u8 hFlip;
        u8 vFlip;
        DecrementAnimDelayCounter(sprite);
        hFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.hFlip;
        vFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.vFlip;
        if (!(sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK))
            SetSpriteOamFlipBits(sprite, hFlip, vFlip);
    }
    else if (!sprite->animPaused)
    {
        s16 type;
        s16 funcIndex;
        sprite->animCmdIndex++;
        type = sprite->anims[sprite->animNum][sprite->animCmdIndex].type;
        funcIndex = 3;
        if (type < 0)
            funcIndex = type + 3;
        sAnimCmdFuncs[funcIndex](sprite);
    }
}

void AnimCmd_frame(struct Sprite *sprite)
{
    s16 imageValue;
    u8 duration;
    u8 hFlip;
    u8 vFlip;

    imageValue = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.imageValue;
    duration = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.duration;
    hFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.hFlip;
    vFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.vFlip;

    if (duration)
        duration--;

    sprite->animDelayCounter = duration;

    if (!(sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK))
        SetSpriteOamFlipBits(sprite, hFlip, vFlip);

    if (sprite->usingSheet)
        sprite->oam.tileNum = sprite->sheetTileStart + imageValue;
    else
        RequestSpriteFrameImageCopy(imageValue, sprite->oam.tileNum, sprite->images);
}

void AnimCmd_end(struct Sprite *sprite)
{
    sprite->animCmdIndex--;
    sprite->animEnded = TRUE;
}

void AnimCmd_jump(struct Sprite *sprite)
{
    s16 imageValue;
    u8 duration;
    u8 hFlip;
    u8 vFlip;

    sprite->animCmdIndex = sprite->anims[sprite->animNum][sprite->animCmdIndex].jump.target;

    imageValue = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.imageValue;
    duration = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.duration;
    hFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.hFlip;
    vFlip = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.vFlip;

    if (duration)
        duration--;

    sprite->animDelayCounter = duration;

    if (!(sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK))
        SetSpriteOamFlipBits(sprite, hFlip, vFlip);

    if (sprite->usingSheet)
        sprite->oam.tileNum = sprite->sheetTileStart + imageValue;
    else
        RequestSpriteFrameImageCopy(imageValue, sprite->oam.tileNum, sprite->images);
}

void AnimCmd_loop(struct Sprite *sprite)
{
    if (sprite->animLoopCounter)
        ContinueAnimLoop(sprite);
    else
        BeginAnimLoop(sprite);
}

void BeginAnimLoop(struct Sprite *sprite)
{
    sprite->animLoopCounter = sprite->anims[sprite->animNum][sprite->animCmdIndex].loop.count;
    JumpToTopOfAnimLoop(sprite);
    ContinueAnim(sprite);
}

void ContinueAnimLoop(struct Sprite *sprite)
{
    sprite->animLoopCounter--;
    JumpToTopOfAnimLoop(sprite);
    ContinueAnim(sprite);
}

void JumpToTopOfAnimLoop(struct Sprite *sprite)
{
    if (sprite->animLoopCounter)
    {
        sprite->animCmdIndex--;

        while (sprite->anims[sprite->animNum][sprite->animCmdIndex - 1].type != -3)
        {
            if (sprite->animCmdIndex == 0)
                break;
            sprite->animCmdIndex--;
        }

        sprite->animCmdIndex--;
    }
}

void BeginAffineAnim(struct Sprite *sprite)
{
    if ((sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK) && sprite->affineAnims[0][0].type != 32767)
    {
        struct AffineAnimFrameCmd frameCmd;
        u8 matrixNum = GetSpriteMatrixNum(sprite);
        AffineAnimStateRestartAnim(matrixNum);
        GetAffineAnimFrame(matrixNum, sprite, &frameCmd);
        sprite->affineAnimBeginning = FALSE;
        sprite->affineAnimEnded = FALSE;
        ApplyAffineAnimFrame(matrixNum, &frameCmd);
        sAffineAnimStates[matrixNum].delayCounter = frameCmd.duration;
        if (sprite->anchored)
            UpdateSpriteMatrixAnchorPos(sprite, sprite->sAnchorX, sprite->sAnchorY);
    }
}

void ContinueAffineAnim(struct Sprite *sprite)
{
    if (sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK)
    {
        u8 matrixNum = GetSpriteMatrixNum(sprite);

        if (sAffineAnimStates[matrixNum].delayCounter)
            AffineAnimDelay(matrixNum, sprite);
        else if (sprite->affineAnimPaused)
            return;
        else
        {
            s16 type;
            s16 funcIndex;
            sAffineAnimStates[matrixNum].animCmdIndex++;
            type = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].type;
            funcIndex = 3;
            if (type >= 32765)
                funcIndex = type - 32765;
            sAffineAnimCmdFuncs[funcIndex](matrixNum, sprite);
        }
        if (sprite->anchored)
            UpdateSpriteMatrixAnchorPos(sprite, sprite->sAnchorX, sprite->sAnchorY);
    }
}

void AffineAnimDelay(u8 matrixNum, struct Sprite *sprite)
{
    if (!DecrementAffineAnimDelayCounter(sprite, matrixNum))
    {
        struct AffineAnimFrameCmd frameCmd;
        GetAffineAnimFrame(matrixNum, sprite, &frameCmd);
        ApplyAffineAnimFrameRelativeAndUpdateMatrix(matrixNum, &frameCmd);
    }
}

void AffineAnimCmd_loop(u8 matrixNum, struct Sprite *sprite)
{
    if (sAffineAnimStates[matrixNum].loopCounter)
        ContinueAffineAnimLoop(matrixNum, sprite);
    else
        BeginAffineAnimLoop(matrixNum, sprite);
}

void BeginAffineAnimLoop(u8 matrixNum, struct Sprite *sprite)
{
    sAffineAnimStates[matrixNum].loopCounter = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].loop.count;
    JumpToTopOfAffineAnimLoop(matrixNum, sprite);
    ContinueAffineAnim(sprite);
}

void ContinueAffineAnimLoop(u8 matrixNum, struct Sprite *sprite)
{
    sAffineAnimStates[matrixNum].loopCounter--;
    JumpToTopOfAffineAnimLoop(matrixNum, sprite);
    ContinueAffineAnim(sprite);
}

void JumpToTopOfAffineAnimLoop(u8 matrixNum, struct Sprite *sprite)
{
    if (sAffineAnimStates[matrixNum].loopCounter)
    {
        sAffineAnimStates[matrixNum].animCmdIndex--;

        while (sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex - 1].type != 32765)
        {
            if (sAffineAnimStates[matrixNum].animCmdIndex == 0)
                break;
            sAffineAnimStates[matrixNum].animCmdIndex--;
        }

        sAffineAnimStates[matrixNum].animCmdIndex--;
    }
}

void AffineAnimCmd_jump(u8 matrixNum, struct Sprite *sprite)
{
    struct AffineAnimFrameCmd frameCmd;
    sAffineAnimStates[matrixNum].animCmdIndex = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].jump.target;
    GetAffineAnimFrame(matrixNum, sprite, &frameCmd);
    ApplyAffineAnimFrame(matrixNum, &frameCmd);
    sAffineAnimStates[matrixNum].delayCounter = frameCmd.duration;
}

void AffineAnimCmd_end(u8 matrixNum, struct Sprite *sprite)
{
    struct AffineAnimFrameCmd dummyFrameCmd = {0};
    sprite->affineAnimEnded = TRUE;
    sAffineAnimStates[matrixNum].animCmdIndex--;
    ApplyAffineAnimFrameRelativeAndUpdateMatrix(matrixNum, &dummyFrameCmd);
}

void AffineAnimCmd_frame(u8 matrixNum, struct Sprite *sprite)
{
    struct AffineAnimFrameCmd frameCmd;
    GetAffineAnimFrame(matrixNum, sprite, &frameCmd);
    ApplyAffineAnimFrame(matrixNum, &frameCmd);
    sAffineAnimStates[matrixNum].delayCounter = frameCmd.duration;
}

void CopyOamMatrix(u8 destMatrixIndex, struct OamMatrix *srcMatrix)
{
    gOamMatrices[destMatrixIndex].a = srcMatrix->a;
    gOamMatrices[destMatrixIndex].b = srcMatrix->b;
    gOamMatrices[destMatrixIndex].c = srcMatrix->c;
    gOamMatrices[destMatrixIndex].d = srcMatrix->d;
}

u8 GetSpriteMatrixNum(struct Sprite *sprite)
{
    u8 matrixNum = 0;
    if (sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK)
        matrixNum = sprite->oam.matrixNum;
    return matrixNum;
}

void SetSpriteMatrixAnchor(struct Sprite* sprite, s16 x, s16 y)
{
    sprite->sAnchorX = x;
    sprite->sAnchorY = y;
    sprite->anchored = TRUE;
}

static s32 GetAnchorCoord(s32 baseDim, s32 xformed, s32 modifier)
{
    s32 subResult, shiftResult;

    subResult = xformed - baseDim;
    if (subResult < 0)
        shiftResult = -(subResult) >> 9;
    else
        shiftResult = -(subResult >> 9);
    return modifier - ((u32)(modifier * xformed) / (u32)(baseDim) + shiftResult);
}

static void UpdateSpriteMatrixAnchorPos(struct Sprite *sprite, s32 x, s32 y)
{
    s32 dim, baseDim, xFormed;

    u32 matrixNum = sprite->oam.matrixNum;
    if (x != NO_ANCHOR)
    {
        dim = sOamDimensionsCopy[sprite->oam.shape][sprite->oam.size][0];
        baseDim = dim << 8;
        xFormed = (dim << 16) / gOamMatrices[matrixNum].a;
        sprite->x2 = GetAnchorCoord(baseDim, xFormed, x);
    }
    if (y != NO_ANCHOR)
    {
        dim = sOamDimensionsCopy[sprite->oam.shape][sprite->oam.size][1];
        baseDim = dim << 8;
        xFormed = (dim << 16) / gOamMatrices[matrixNum].d;
        sprite->y2 = GetAnchorCoord(baseDim, xFormed, y);
    }
}

void SetSpriteOamFlipBits(struct Sprite *sprite, u8 hFlip, u8 vFlip)
{
    sprite->oam.matrixNum &= 0x7;
    sprite->oam.matrixNum |= (((hFlip ^ sprite->hFlip) & 1) << 3);
    sprite->oam.matrixNum |= (((vFlip ^ sprite->vFlip) & 1) << 4);
}

void AffineAnimStateRestartAnim(u8 matrixNum)
{
    sAffineAnimStates[matrixNum].animCmdIndex = 0;
    sAffineAnimStates[matrixNum].delayCounter = 0;
    sAffineAnimStates[matrixNum].loopCounter = 0;
}

void AffineAnimStateStartAnim(u8 matrixNum, u8 animNum)
{
    sAffineAnimStates[matrixNum].animNum = animNum;
    sAffineAnimStates[matrixNum].animCmdIndex = 0;
    sAffineAnimStates[matrixNum].delayCounter = 0;
    sAffineAnimStates[matrixNum].loopCounter = 0;
    sAffineAnimStates[matrixNum].xScale = 0x0100;
    sAffineAnimStates[matrixNum].yScale = 0x0100;
    sAffineAnimStates[matrixNum].rotation = 0;
}

void AffineAnimStateReset(u8 matrixNum)
{
    sAffineAnimStates[matrixNum].animNum = 0;
    sAffineAnimStates[matrixNum].animCmdIndex = 0;
    sAffineAnimStates[matrixNum].delayCounter = 0;
    sAffineAnimStates[matrixNum].loopCounter = 0;
    sAffineAnimStates[matrixNum].xScale = 0x0100;
    sAffineAnimStates[matrixNum].yScale = 0x0100;
    sAffineAnimStates[matrixNum].rotation = 0;
}

void ApplyAffineAnimFrameAbsolute(u8 matrixNum, struct AffineAnimFrameCmd *frameCmd)
{
    sAffineAnimStates[matrixNum].xScale = frameCmd->xScale;
    sAffineAnimStates[matrixNum].yScale = frameCmd->yScale;
    sAffineAnimStates[matrixNum].rotation = frameCmd->rotation << 8;
}

void DecrementAnimDelayCounter(struct Sprite *sprite)
{
    if (!sprite->animPaused)
        sprite->animDelayCounter--;
}

bool8 DecrementAffineAnimDelayCounter(struct Sprite *sprite, u8 matrixNum)
{
    if (!sprite->affineAnimPaused)
        --sAffineAnimStates[matrixNum].delayCounter;
    return sprite->affineAnimPaused;
}

void ApplyAffineAnimFrameRelativeAndUpdateMatrix(u8 matrixNum, struct AffineAnimFrameCmd *frameCmd)
{
    struct ObjAffineSrcData srcData;
    struct OamMatrix matrix;
    sAffineAnimStates[matrixNum].xScale += frameCmd->xScale;
    sAffineAnimStates[matrixNum].yScale += frameCmd->yScale;
    sAffineAnimStates[matrixNum].rotation = (sAffineAnimStates[matrixNum].rotation + (frameCmd->rotation << 8)) & ~0xFF;
    srcData.xScale = ConvertScaleParam(sAffineAnimStates[matrixNum].xScale);
    srcData.yScale = ConvertScaleParam(sAffineAnimStates[matrixNum].yScale);
    srcData.rotation = sAffineAnimStates[matrixNum].rotation;
    ObjAffineSet(&srcData, &matrix, 1, 2);
    CopyOamMatrix(matrixNum, &matrix);
}

s16 ConvertScaleParam(s16 scale)
{
    s32 val = 0x10000;
    return SAFE_DIV(val, scale);
}

void GetAffineAnimFrame(u8 matrixNum, struct Sprite *sprite, struct AffineAnimFrameCmd *frameCmd)
{
    frameCmd->xScale = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].frame.xScale;
    frameCmd->yScale = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].frame.yScale;
    frameCmd->rotation = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].frame.rotation;
    frameCmd->duration = sprite->affineAnims[sAffineAnimStates[matrixNum].animNum][sAffineAnimStates[matrixNum].animCmdIndex].frame.duration;
}

void ApplyAffineAnimFrame(u8 matrixNum, struct AffineAnimFrameCmd *frameCmd)
{
    struct AffineAnimFrameCmd dummyFrameCmd = {0};

    if (frameCmd->duration)
    {
        frameCmd->duration--;
        ApplyAffineAnimFrameRelativeAndUpdateMatrix(matrixNum, frameCmd);
    }
    else
    {
        ApplyAffineAnimFrameAbsolute(matrixNum, frameCmd);
        ApplyAffineAnimFrameRelativeAndUpdateMatrix(matrixNum, &dummyFrameCmd);
    }
}

void StartSpriteAnim(struct Sprite *sprite, u8 animNum)
{
    sprite->animNum = animNum;
    sprite->animBeginning = TRUE;
    sprite->animEnded = FALSE;
}

void StartSpriteAnimIfDifferent(struct Sprite *sprite, u8 animNum)
{
    if (sprite->animNum != animNum)
        StartSpriteAnim(sprite, animNum);
}

void SeekSpriteAnim(struct Sprite *sprite, u8 animCmdIndex)
{
    u8 temp = sprite->animPaused;
    sprite->animCmdIndex = animCmdIndex - 1;
    sprite->animDelayCounter = 0;
    sprite->animBeginning = FALSE;
    sprite->animEnded = FALSE;
    sprite->animPaused = FALSE;
    ContinueAnim(sprite);
    if (sprite->animDelayCounter)
        sprite->animDelayCounter++;
    sprite->animPaused = temp;
}

void StartSpriteAffineAnim(struct Sprite *sprite, u8 animNum)
{
    u8 matrixNum = GetSpriteMatrixNum(sprite);
    AffineAnimStateStartAnim(matrixNum, animNum);
    sprite->affineAnimBeginning = TRUE;
    sprite->affineAnimEnded = FALSE;
}

void StartSpriteAffineAnimIfDifferent(struct Sprite *sprite, u8 animNum)
{
    u8 matrixNum = GetSpriteMatrixNum(sprite);
    if (sAffineAnimStates[matrixNum].animNum != animNum)
        StartSpriteAffineAnim(sprite, animNum);
}

void ChangeSpriteAffineAnim(struct Sprite *sprite, u8 animNum)
{
    u8 matrixNum = GetSpriteMatrixNum(sprite);
    sAffineAnimStates[matrixNum].animNum = animNum;
    sprite->affineAnimBeginning = TRUE;
    sprite->affineAnimEnded = FALSE;
}

void ChangeSpriteAffineAnimIfDifferent(struct Sprite *sprite, u8 animNum)
{
    u8 matrixNum = GetSpriteMatrixNum(sprite);
    if (sAffineAnimStates[matrixNum].animNum != animNum)
        ChangeSpriteAffineAnim(sprite, animNum);
}

void SetSpriteSheetFrameTileNum(struct Sprite *sprite)
{
    if (sprite->usingSheet)
    {
        s16 tileOffset = sprite->anims[sprite->animNum][sprite->animCmdIndex].frame.imageValue;
        if (tileOffset < 0)
            tileOffset = 0;
        sprite->oam.tileNum = sprite->sheetTileStart + tileOffset;
    }
}

void ResetAffineAnimData(void)
{
    u8 i;

    gAffineAnimsDisabled = 0;
    gOamMatrixAllocBitmap = 0;

    ResetOamMatrices();

    for (i = 0; i < OAM_MATRIX_COUNT; i++)
        AffineAnimStateReset(i);
}

u8 AllocOamMatrix(void)
{
    u8 i = 0;
    u32 bit = 1;
    u32 bitmap = gOamMatrixAllocBitmap;

    while (i < OAM_MATRIX_COUNT)
    {
        if (!(bitmap & bit))
        {
            gOamMatrixAllocBitmap |= bit;
            return i;
        }

        i++;
        bit <<= 1;
    }

    return 0xFF;
}

void FreeOamMatrix(u8 matrixNum)
{
    u8 i = 0;
    u32 bit = 1;

    while (i < matrixNum)
    {
        i++;
        bit <<= 1;
    }

    gOamMatrixAllocBitmap &= ~bit;
    SetOamMatrix(matrixNum, 0x100, 0, 0, 0x100);
}

void InitSpriteAffineAnim(struct Sprite *sprite)
{
    u8 matrixNum = AllocOamMatrix();
    if (matrixNum != 0xFF)
    {
        CalcCenterToCornerVec(sprite, sprite->oam.shape, sprite->oam.size, sprite->oam.affineMode);
        sprite->oam.matrixNum = matrixNum;
        sprite->affineAnimBeginning = TRUE;
        AffineAnimStateReset(matrixNum);
    }
}

void SetOamMatrixRotationScaling(u8 matrixNum, s16 xScale, s16 yScale, u16 rotation)
{
    struct ObjAffineSrcData srcData;
    struct OamMatrix matrix;
    srcData.xScale = ConvertScaleParam(xScale);
    srcData.yScale = ConvertScaleParam(yScale);
    srcData.rotation = rotation;
    ObjAffineSet(&srcData, &matrix, 1, 2);
    CopyOamMatrix(matrixNum, &matrix);
}

u16 LoadSpriteSheet(const struct SpriteSheet *sheet)
{
    s16 tileStart = AllocSpriteTiles(sheet->size / TILE_SIZE_4BPP);

    if (tileStart < 0)
    {
        return 0;
    }
    else
    {
        AllocSpriteTileRange(sheet->tag, (u16)tileStart, sheet->size / TILE_SIZE_4BPP);
        CpuCopy16(sheet->data, (u8 *)OBJ_VRAM0 + TILE_SIZE_4BPP * tileStart, sheet->size);
        return (u16)tileStart;
    }
}

void LoadSpriteSheets(const struct SpriteSheet *sheets)
{
    u8 i;
    for (i = 0; sheets[i].data != NULL; i++)
        LoadSpriteSheet(&sheets[i]);
}

void FreeSpriteTilesByTag(u16 tag)
{
    u8 index = IndexOfSpriteTileTag(tag);
    if (index != 0xFF)
    {
        u16 i;
        u16 *rangeStarts;
        u16 *rangeCounts;
        u16 start;
        u16 count;
        rangeStarts = sSpriteTileRanges;
        start = rangeStarts[index * 2];
        rangeCounts = sSpriteTileRanges + 1;
        count = rangeCounts[index * 2];

        for (i = start; i < start + count; i++)
            FREE_SPRITE_TILE(i);

        sSpriteTileRangeTags[index] = TAG_NONE;
    }
}

void FreeSpriteTileRanges(void)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        sSpriteTileRangeTags[i] = TAG_NONE;
        SET_SPRITE_TILE_RANGE(i, 0, 0);
    }
}

u16 GetSpriteTileStartByTag(u16 tag)
{
    u8 index = IndexOfSpriteTileTag(tag);
    if (index == 0xFF)
        return TAG_NONE;
    return sSpriteTileRanges[index * 2];
}

u8 IndexOfSpriteTileTag(u16 tag)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
        if (sSpriteTileRangeTags[i] == tag)
            return i;

    return 0xFF;
}

u16 GetSpriteTileTagByTileStart(u16 start)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        if (sSpriteTileRangeTags[i] != TAG_NONE && sSpriteTileRanges[i * 2] == start)
            return sSpriteTileRangeTags[i];
    }

    return TAG_NONE;
}

void AllocSpriteTileRange(u16 tag, u16 start, u16 count)
{
    u8 freeIndex = IndexOfSpriteTileTag(TAG_NONE);
    sSpriteTileRangeTags[freeIndex] = tag;
    SET_SPRITE_TILE_RANGE(freeIndex, start, count);
}

void FreeAllSpritePalettes(void)
{
    u8 i;
    gReservedSpritePaletteCount = 0;
    for (i = 0; i < ARRAY_COUNT(sSpritePaletteTags); i++)
        sSpritePaletteTags[i] = TAG_NONE;
}

u8 LoadSpritePalette(const struct SpritePalette *palette)
{
    u8 index = IndexOfSpritePaletteTag(palette->tag);

    if (index != 0xFF)
        return index;

    index = IndexOfSpritePaletteTag(TAG_NONE);

    if (index == 0xFF)
    {
        return 0xFF;
    }
    else
    {
        sSpritePaletteTags[index] = palette->tag;
        DoLoadSpritePalette(palette->data, PLTT_ID(index));
        return index;
    }
}

void LoadSpritePalettes(const struct SpritePalette *palettes)
{
    u8 i;
    for (i = 0; palettes[i].data != NULL; i++)
        if (LoadSpritePalette(&palettes[i]) == 0xFF)
            break;
}

static void DoLoadSpritePalette(const u16 *src, u16 paletteOffset)
{
    LoadPalette(src, paletteOffset + OBJ_PLTT_OFFSET, PLTT_SIZE_4BPP);
}

u8 AllocSpritePalette(u16 tag)
{
    u8 index = IndexOfSpritePaletteTag(TAG_NONE);
    if (index == 0xFF)
    {
        return 0xFF;
    }
    else
    {
        sSpritePaletteTags[index] = tag;
        return index;
    }
}

u8 IndexOfSpritePaletteTag(u16 tag)
{
    u8 i;
    for (i = gReservedSpritePaletteCount; i < 16; i++)
        if (sSpritePaletteTags[i] == tag)
            return i;

    return 0xFF;
}

u16 GetSpritePaletteTagByPaletteNum(u8 paletteNum)
{
    return sSpritePaletteTags[paletteNum];
}

void FreeSpritePaletteByTag(u16 tag)
{
    u8 index = IndexOfSpritePaletteTag(tag);
    if (index != 0xFF)
        sSpritePaletteTags[index] = TAG_NONE;
}

void SetSubspriteTables(struct Sprite *sprite, const struct SubspriteTable *subspriteTables)
{
    sprite->subspriteTables = subspriteTables;
    sprite->subspriteTableNum = 0;
    sprite->subspriteMode = SUBSPRITES_ON;
}

bool8 AddSpriteToOamBuffer(struct Sprite *sprite, u8 *oamIndex)
{
    if (*oamIndex >= gOamLimit)
        return 1;

    if (!sprite->subspriteTables || sprite->subspriteMode == SUBSPRITES_OFF)
    {
        gMain.oamBuffer[*oamIndex] = sprite->oam;
        (*oamIndex)++;
        return 0;
    }
    else
    {
        return AddSubspritesToOamBuffer(sprite, &gMain.oamBuffer[*oamIndex], oamIndex);
    }
}

bool8 AddSubspritesToOamBuffer(struct Sprite *sprite, struct OamData *destOam, u8 *oamIndex)
{
    const struct SubspriteTable *subspriteTable;
    struct OamData *oam;

    if (*oamIndex >= gOamLimit)
        return 1;

    subspriteTable = &sprite->subspriteTables[sprite->subspriteTableNum];
    oam = &sprite->oam;

    if (!subspriteTable || !subspriteTable->subsprites)
    {
        *destOam = *oam;
        (*oamIndex)++;
        return 0;
    }
    else
    {
        u16 tileNum;
        u16 baseX;
        u16 baseY;
        u8 subspriteCount;
        u8 hFlip;
        u8 vFlip;
        u8 i;

        tileNum = oam->tileNum;
        subspriteCount = subspriteTable->subspriteCount;
        hFlip = ((s32)oam->matrixNum >> 3) & 1;
        vFlip = ((s32)oam->matrixNum >> 4) & 1;
        baseX = oam->x - sprite->centerToCornerVecX;
        baseY = oam->y - sprite->centerToCornerVecY;

        for (i = 0; i < subspriteCount; i++, (*oamIndex)++)
        {
            u16 x;
            u16 y;

            if (*oamIndex >= gOamLimit)
                return 1;

            x = subspriteTable->subsprites[i].x;
            y = subspriteTable->subsprites[i].y;

            if (hFlip)
            {
                s8 width = sOamDimensions[subspriteTable->subsprites[i].shape][subspriteTable->subsprites[i].size].width;
                s16 right = x;
                right += width;
                x = right;
                x = ~x + 1;
            }

            if (vFlip)
            {
                s8 height = sOamDimensions[subspriteTable->subsprites[i].shape][subspriteTable->subsprites[i].size].height;
                s16 bottom = y;
                bottom += height;
                y = bottom;
                y = ~y + 1;
            }

            destOam[i] = *oam;
            destOam[i].shape = subspriteTable->subsprites[i].shape;
            destOam[i].size = subspriteTable->subsprites[i].size;
            destOam[i].x = (s16)baseX + (s16)x;
            destOam[i].y = baseY + y;
            destOam[i].tileNum = tileNum + subspriteTable->subsprites[i].tileOffset;

            if (sprite->subspriteMode != SUBSPRITES_IGNORE_PRIORITY)
                destOam[i].priority = subspriteTable->subsprites[i].priority;
        }
    }

    return 0;
}
