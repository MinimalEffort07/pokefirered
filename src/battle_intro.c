/**
 * @file battle_intro.c
 * @brief Battle Introduction Slide-In Animations
 *
 * FILE OVERVIEW:
 * This file implements the dramatic slide-in effect that plays at the start of
 * every Pokemon battle. When a battle begins, the background terrain slides
 * horizontally while a vertical window "iris" opens to reveal the battle scene.
 *
 * There are 4 different slide animations, selected based on the terrain type:
 * - BattleIntroSlide1: Used for grass, long grass, pond, mountain, cave terrains
 * - BattleIntroSlide2: Used for sand, underwater, and water terrains (with alpha blending)
 * - BattleIntroSlide3: Used for building and plain terrains (with transparency)
 * - BattleIntroSlideLink: Special version for link battles (with VS letters)
 *
 * GBA HARDWARE CONTEXT — BATTLE INTRO TECHNIQUE:
 * The intro effect uses several GBA hardware features simultaneously:
 *
 * 1. HARDWARE WINDOWS: WIN0V controls a vertical "iris" that starts fully closed
 *    and opens from the center. Scanline effects make the two halves of the screen
 *    scroll in opposite directions (top half scrolls right, bottom half scrolls left).
 *
 * 2. SCANLINE EFFECT: Each scanline gets a different BG scroll offset. The top
 *    80 scanlines scroll one direction, the bottom 80 scroll the opposite way.
 *    This creates the "split screen" sliding effect.
 *
 * 3. ALPHA BLENDING: Some terrain types use the GBA's hardware alpha blending
 *    (BLDCNT/BLDALPHA registers) to gradually fade the terrain background in or out.
 *
 * 4. BG SCROLL: gBattle_BG1_X is continuously incremented to create the horizontal
 *    scrolling motion of the terrain background.
 */
#include "global.h"
#include "gflib.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_setup.h"
#include "scanline_effect.h"
#include "task.h"
#include "trig.h"

/* Stored BG control register value used by Set/GetAnimBgAttribute */
static EWRAM_DATA u16 sBgCnt = 0;

extern const u8 gBattleAnimRegOffsBgCnt[];
extern const u8 gBattleIntroRegOffsBgCnt[];

static void BattleIntroSlide1(u8 taskId);
static void BattleIntroSlide2(u8 taskId);
static void BattleIntroSlide3(u8 taskId);
static void BattleIntroSlideLink(u8 taskId);

/*
 * Maps each battle terrain type to its intro slide animation function.
 * The terrain is determined by where the battle was initiated (grass, cave, etc.)
 */
static const TaskFunc sBattleIntroSlideFuncs[] =
{
    BattleIntroSlide1, // BATTLE_TERRAIN_GRASS     — standard slide
    BattleIntroSlide1, // BATTLE_TERRAIN_LONG_GRASS — standard slide
    BattleIntroSlide2, // BATTLE_TERRAIN_SAND       — with alpha blend
    BattleIntroSlide2, // BATTLE_TERRAIN_UNDERWATER  — with alpha blend + wave
    BattleIntroSlide2, // BATTLE_TERRAIN_WATER       — with alpha blend
    BattleIntroSlide1, // BATTLE_TERRAIN_POND        — standard slide
    BattleIntroSlide1, // BATTLE_TERRAIN_MOUNTAIN    — standard slide
    BattleIntroSlide1, // BATTLE_TERRAIN_CAVE        — standard slide
    BattleIntroSlide3, // BATTLE_TERRAIN_BUILDING    — with transparency
    BattleIntroSlide3, // BATTLE_TERRAIN_PLAIN       — with transparency
};

/**
 * FUNCTION: SetAnimBgAttribute
 *
 * PURPOSE: Sets a specific attribute of a background layer's control register
 * during battle animations. This is a field-level setter for the BG control
 * register (BGxCNT), allowing individual attributes to be changed without
 * affecting others.
 *
 * GBA CONTEXT:
 * Each BG control register (BG0CNT-BG3CNT at 0x04000008-0x0400000E) is a 16-bit
 * register packed with multiple fields:
 * - Priority (bits 0-1): Drawing order (0 = highest priority/drawn last)
 * - Character base block (bits 2-3): Where tile graphics data starts in VRAM
 * - Mosaic (bit 6): Enable mosaic effect for this BG
 * - Palettes mode (bit 7): 0 = 16-color palettes, 1 = 256-color palette
 * - Screen base block (bits 8-12): Where the tilemap data starts in VRAM
 * - Area overflow mode (bit 13): For rotation/scaling BGs, wrap or clip
 * - Screen size (bits 14-15): Tilemap dimensions (256x256 to 512x512)
 *
 * @param bgId        — which background (0-3)
 * @param attributeId — which field of the BG control register to set
 * @param value       — the new value for that field
 */
void SetAnimBgAttribute(u8 bgId, u8 attributeId, u8 value)
{
    if (bgId < 4)
    {
        /* Read the current register value through the battle anim register offset table */
        sBgCnt = GetGpuReg(gBattleAnimRegOffsBgCnt[bgId]);
        switch (attributeId)
        {
        case BG_ANIM_SCREEN_SIZE:
            ((struct BgCnt *)&sBgCnt)->screenSize = value;
            break;
        case BG_ANIM_AREA_OVERFLOW_MODE:
            ((struct BgCnt *)&sBgCnt)->areaOverflowMode = value;
            break;
        case BG_ANIM_MOSAIC:
            ((struct BgCnt *)&sBgCnt)->mosaic = value;
            break;
        case BG_ANIM_CHAR_BASE_BLOCK:
            ((struct BgCnt *)&sBgCnt)->charBaseBlock = value;
            break;
        case BG_ANIM_PRIORITY:
            ((struct BgCnt *)&sBgCnt)->priority = value;
            break;
        case BG_ANIM_PALETTES_MODE:
            ((struct BgCnt *)&sBgCnt)->palettes = value;
            break;
        case BG_ANIM_SCREEN_BASE_BLOCK:
            ((struct BgCnt *)&sBgCnt)->screenBaseBlock = value;
            break;
        }
        /* Write the modified value back to the GPU register */
        SetGpuReg(gBattleAnimRegOffsBgCnt[bgId], sBgCnt);
    }
}

/**
 * FUNCTION: GetAnimBgAttribute
 *
 * PURPOSE: Reads a specific attribute from a background's control register
 * during battle animations. Counterpart to SetAnimBgAttribute.
 *
 * NOTE: Uses gBattleIntroRegOffsBgCnt (different from the set function which uses
 * gBattleAnimRegOffsBgCnt). This is because intro slides and battle animations
 * may use different BG configurations.
 *
 * @param bgId        — which background (0-3)
 * @param attributeId — which field to read
 * @return The value of the requested attribute, or 0 if bgId is out of range
 */
s32 GetAnimBgAttribute(u8 bgId, u8 attributeId)
{
    u16 bgCnt;

    if (bgId < 4)
    {
        bgCnt = GetGpuReg(gBattleIntroRegOffsBgCnt[bgId]);
        switch (attributeId)
        {
        case BG_ANIM_SCREEN_SIZE:
            return ((struct BgCnt *)&bgCnt)->screenSize;
        case BG_ANIM_AREA_OVERFLOW_MODE:
            return ((struct BgCnt *)&bgCnt)->areaOverflowMode;
        case BG_ANIM_MOSAIC:
            return ((struct BgCnt *)&bgCnt)->mosaic;
        case BG_ANIM_CHAR_BASE_BLOCK:
            return ((struct BgCnt *)&bgCnt)->charBaseBlock;
        case BG_ANIM_PRIORITY:
            return ((struct BgCnt *)&bgCnt)->priority;
        case BG_ANIM_PALETTES_MODE:
            return ((struct BgCnt *)&bgCnt)->palettes;
        case BG_ANIM_SCREEN_BASE_BLOCK:
            return ((struct BgCnt *)&bgCnt)->screenBaseBlock;
        }
    }
    return 0;
}

/**
 * FUNCTION: HandleIntroSlide
 *
 * PURPOSE: Starts the battle intro slide animation. Called at the beginning of
 * every battle after the transition effect finishes.
 *
 * HOW IT WORKS:
 * Selects the appropriate slide function based on terrain and battle type:
 * - Link battles always use BattleIntroSlideLink (special VS letter animation)
 * - Kyogre/Groudon battles (non-Ruby) force underwater terrain
 * - All other battles use the terrain-indexed function from sBattleIntroSlideFuncs
 *
 * Task data fields:
 * - data[0]: State machine counter
 * - data[1]: Terrain type (used to adjust visual parameters)
 * - data[2]-data[6]: Various animation parameters (delay counters, scroll values)
 *
 * @param terrain — BATTLE_TERRAIN_* constant for the encounter location
 */
void HandleIntroSlide(u8 terrain)
{
    u8 taskId;

    if (gBattleTypeFlags & BATTLE_TYPE_LINK)
    {
        /* Link battles: always use the special VS animation */
        taskId = CreateTask(BattleIntroSlideLink, 0);
    }
    else if ((gBattleTypeFlags & BATTLE_TYPE_KYOGRE_GROUDON) && gGameVersion != VERSION_RUBY)
    {
        /* Kyogre battle (Sapphire/Emerald): use underwater slide with wave effect */
        terrain = BATTLE_TERRAIN_UNDERWATER;
        taskId = CreateTask(BattleIntroSlide2, 0);
    }
    else
    {
        /* Standard battle: select animation based on terrain type */
        taskId = CreateTask(sBattleIntroSlideFuncs[terrain], 0);
    }

    /* Initialize all task data to zero */
    gTasks[taskId].data[0] = 0;
    gTasks[taskId].data[1] = terrain;
    gTasks[taskId].data[2] = 0;
    gTasks[taskId].data[3] = 0;
    gTasks[taskId].data[4] = 0;
    gTasks[taskId].data[5] = 0;
    gTasks[taskId].data[6] = 0;
}

/**
 * FUNCTION: BattleIntroSlideEnd
 *
 * PURPOSE: Cleans up after the intro slide animation completes. Resets all
 * modified GPU registers to their neutral battle state.
 *
 * GBA CONTEXT:
 * Resets BG scroll positions to 0, disables all blending effects, and sets
 * the window registers so that everything is visible both inside and outside
 * all windows (effectively disabling windowing for normal battle rendering).
 *
 * WININ/WINOUT bit flags:
 * - BG_ALL: Show all 4 background layers
 * - OBJ: Show sprites (object layer)
 * - CLR: Allow color special effects (blending/brightness)
 *
 * @param taskId — task to destroy
 */
void BattleIntroSlideEnd(u8 taskId)
{
    DestroyTask(taskId);
    gBattle_BG1_X = 0;
    gBattle_BG1_Y = 0;
    gBattle_BG2_X = 0;
    gBattle_BG2_Y = 0;
    /* Disable all blending effects */
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    /* Make everything visible in all window regions */
    SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL | WININ_WIN0_OBJ | WININ_WIN0_CLR | WININ_WIN1_BG_ALL | WININ_WIN1_OBJ | WININ_WIN1_CLR);
    SetGpuReg(REG_OFFSET_WINOUT, WINOUT_WIN01_BG_ALL | WINOUT_WIN01_OBJ | WINOUT_WIN01_CLR | WINOUT_WINOBJ_BG_ALL | WINOUT_WINOBJ_OBJ | WINOUT_WINOBJ_CLR);
}

/**
 * FUNCTION: BattleIntroSlide1
 *
 * PURPOSE: Standard intro slide for grass, long grass, pond, mountain, and cave
 * terrains. The background scrolls horizontally while a vertical window opens.
 *
 * HOW IT WORKS:
 * State 0: Initialize delay (1 frame for normal battles, 16 for link battles)
 * State 1: Wait for delay, then enable WIN0 contents
 * State 2: Open the vertical window (WIN0V top edge moves toward center row 0x30)
 *          and clear the intro slide flag. This reveals the battle scene.
 * State 3: After the window is fully open, use scanline effects to slide the
 *          two halves of the screen in opposite directions. The top half scrolls
 *          right and the bottom half scrolls left, creating a "split and slide"
 *          effect. Also scrolls BG1_Y to position the terrain background.
 *          Once the slide reaches zero, cleans up BG tilemaps.
 * State 4: Cleanup — call BattleIntroSlideEnd
 *
 * GBA CONTEXT — SCANLINE SCROLL TRICK:
 * The scanline buffer stores a per-scanline BG scroll offset. Lines 0-79 (top half)
 * get a positive offset (scroll right), lines 80-159 (bottom half) get a negative
 * offset (scroll left). This creates the visual effect of the screen "splitting"
 * in the middle with each half sliding away to reveal the battle background.
 *
 * @param taskId — task identifier
 */
static void BattleIntroSlide1(u8 taskId)
{
    s32 i;

    /* Continuously scroll the terrain background to the right at 6 pixels/frame */
    gBattle_BG1_X += 6;
    switch (gTasks[taskId].data[0])
    {
    case 0:
        if (gBattleTypeFlags & BATTLE_TYPE_LINK)
        {
            gTasks[taskId].data[2] = 16;  /* Link battle: longer delay for sync */
            ++gTasks[taskId].data[0];
        }
        else
        {
            gTasks[taskId].data[2] = 1;   /* Normal battle: minimal delay */
            ++gTasks[taskId].data[0];
        }
        break;
    case 1:
        if (--gTasks[taskId].data[2] == 0)
        {
            ++gTasks[taskId].data[0];
            /* Enable WIN0 to show all layers, sprites, and color effects inside */
            SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL | WININ_WIN0_OBJ | WININ_WIN0_CLR);
        }
        break;
    case 2:
        /*
         * Open the vertical window: subtract from the top byte of WIN0V.
         * WIN0V format: top byte = top edge, bottom byte = bottom edge.
         * Subtracting 0xFF from the 16-bit value decrements the top byte by 1
         * (and wraps the bottom byte, which is handled by the mask check).
         * When the top edge reaches 0x30 (scanline 48), the window is open enough.
         */
        gBattle_WIN0V -= 0xFF;
        if ((gBattle_WIN0V & 0xFF00) == 0x3000)
        {
            ++gTasks[taskId].data[0];
            gTasks[taskId].data[2] = 240;  /* Start scroll offset at 240 (full screen width) */
            gTasks[taskId].data[3] = 32;   /* Delay before BG_Y scrolling starts */
            gIntroSlideFlags &= ~1;        /* Signal that the intro slide opening is done */
        }
        break;
    case 3:
        /* Delay before starting vertical scroll */
        if (gTasks[taskId].data[3])
        {
            --gTasks[taskId].data[3];
        }
        else
        {
            /* Scroll the terrain background vertically based on terrain type */
            if (gTasks[taskId].data[1] == 1)  /* Long grass: scroll more */
            {
                if (gBattle_BG1_Y != 0xFFB0)
                    gBattle_BG1_Y -= 2;
            }
            else if (gBattle_BG1_Y != 0xFFC8)
            {
                    gBattle_BG1_Y -= 1;
            }
        }
        /* Continue opening the vertical window if not fully open */
        if (gBattle_WIN0V & 0xFF00)
            gBattle_WIN0V -= 0x3FC;

        /* Decrease the horizontal split-scroll offset by 2 per frame */
        if (gTasks[taskId].data[2])
            gTasks[taskId].data[2] -= 2;

        /*
         * Write per-scanline scroll offsets to the scanline effect buffer:
         * Top half (scanlines 0-79): positive offset = scroll right
         * Bottom half (scanlines 80-159): negative offset = scroll left
         */
        for (i = 0; i < 80; ++i)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gTasks[taskId].data[2];
        while (i < 160)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i++] = -gTasks[taskId].data[2];

        /* When scroll reaches zero, the slide-in is complete */
        if (!gTasks[taskId].data[2])
        {
            gScanlineEffect.state = 3;  /* Signal scanline effect to stop */
            ++gTasks[taskId].data[0];
            /* Clear the terrain tilemap from screen base block 28 */
            CpuFill32(0, (void *)BG_SCREEN_ADDR(28), BG_SCREEN_SIZE);
            /* Reset BG character/screen base blocks for normal battle rendering */
            SetBgAttribute(1, BG_ATTR_CHARBASEINDEX, 0);
            SetBgAttribute(2, BG_ATTR_CHARBASEINDEX, 0);
            SetGpuReg(REG_OFFSET_BG1CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(28) | BGCNT_TXT256x512);
            SetGpuReg(REG_OFFSET_BG2CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(30) | BGCNT_TXT512x256);
        }
        break;
    case 4:
        BattleIntroSlideEnd(taskId);
        break;
    }
}

/**
 * FUNCTION: BattleIntroSlide2
 *
 * PURPOSE: Intro slide for sand, underwater, and water terrains. Similar to
 * Slide1 but adds alpha blending that gradually fades the terrain layer.
 *
 * HOW IT WORKS:
 * Same vertical window opening and scanline split-scroll as Slide1, but
 * additionally uses the GBA's alpha blending hardware:
 * - BLDCNT targets BG1 as the semi-transparent layer, blended against BG3 and OBJ
 * - BLDALPHA controls the blend coefficient, gradually decreasing from 16 to 0
 * - For underwater terrain, BG1_Y follows a cosine wave for a bobbing water effect
 *
 * GBA CONTEXT — ALPHA BLENDING:
 * The GBA can blend two layers together using the BLDCNT and BLDALPHA registers:
 * - BLDCNT selects which layer is "first target" (semi-transparent) and which
 *   layers are "second targets" (what shows through)
 * - BLDALPHA sets the blend ratio: BLDALPHA_BLEND(A, B) means
 *   output = (first * A + second * B) / 16
 * - This creates transparency effects like seeing the battle field through
 *   the terrain background
 *
 * @param taskId — task identifier
 */
static void BattleIntroSlide2(u8 taskId)
{
    s32 i;

    /* Scroll speed varies by terrain type */
    switch (gTasks[taskId].data[1])
    {
    case 2:  /* Sand */
    case 4:  /* Water */
        gBattle_BG1_X += 8;
        break;
    case 3:  /* Underwater */
        gBattle_BG1_X += 6;
        break;
    }

    /* Underwater: bobbing wave effect using cosine curve */
    if (gTasks[taskId].data[1] == 4)
    {
        gBattle_BG1_Y = Cos2(gTasks[taskId].data[6]) / 512 - 8;
        if (gTasks[taskId].data[6] < 180)
            gTasks[taskId].data[6] += 4;   /* Slower in first half of wave */
        else
            gTasks[taskId].data[6] += 6;   /* Faster in second half */
        if (gTasks[taskId].data[6] == 360)
            gTasks[taskId].data[6] = 0;    /* Wrap angle */
    }

    switch (gTasks[taskId].data[0])
    {
    case 0:
        gTasks[taskId].data[4] = 16;  /* Alpha blend starts at full opacity */
        if (gBattleTypeFlags & BATTLE_TYPE_LINK)
        {
            gTasks[taskId].data[2] = 16;
            ++gTasks[taskId].data[0];
        }
        else
        {
            gTasks[taskId].data[2] = 1;
            ++gTasks[taskId].data[0];
        }
        break;
    case 1:
        if (--gTasks[taskId].data[2] == 0)
        {
            ++gTasks[taskId].data[0];
            SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL | WININ_WIN0_OBJ | WININ_WIN0_CLR);
        }
        break;
    case 2:
        gBattle_WIN0V -= 0xFF;
        if ((gBattle_WIN0V & 0xFF00) == 0x3000)
        {
            ++gTasks[taskId].data[0];
            gTasks[taskId].data[2] = 240;
            gTasks[taskId].data[3] = 32;
            gTasks[taskId].data[5] = 1;  /* Timer for alpha blend steps */
            gIntroSlideFlags &= ~1;
        }
        break;
    case 3:
        if (gTasks[taskId].data[3])
        {
            if (--gTasks[taskId].data[3] == 0)
            {
                /* Start alpha blending: BG1 blends against BG3 and OBJ */
                SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ);
                SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(15, 0));
                SetGpuReg(REG_OFFSET_BLDY, 0);
            }
        }
        else if ((gTasks[taskId].data[4] & 0x1F) && --gTasks[taskId].data[5] == 0)
        {
                /* Gradually decrease alpha (fade terrain out) every 4 frames */
                gTasks[taskId].data[4] += 0xFF;  /* Decrement the blend coefficient */
                gTasks[taskId].data[5] = 4;
        }
        if (gBattle_WIN0V & 0xFF00)
            gBattle_WIN0V -= 0x3FC;

        if (gTasks[taskId].data[2])
            gTasks[taskId].data[2] -= 2;

        /* Same scanline split-scroll effect as Slide1 */
        for (i = 0; i < 80; ++i)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gTasks[taskId].data[2];
        while (i < 160)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i++] = -gTasks[taskId].data[2];
        if (!gTasks[taskId].data[2])
        {
            gScanlineEffect.state = 3;
            ++gTasks[taskId].data[0];
            CpuFill32(0, (void *)BG_SCREEN_ADDR(28), BG_SCREEN_SIZE);
            SetBgAttribute(1, BG_ATTR_CHARBASEINDEX, 0);
            SetBgAttribute(2, BG_ATTR_CHARBASEINDEX, 0);
            SetGpuReg(REG_OFFSET_BG1CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(28) | BGCNT_TXT256x512);
            SetGpuReg(REG_OFFSET_BG2CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(30) | BGCNT_TXT512x256);
        }
        break;
    case 4:
        BattleIntroSlideEnd(taskId);
        break;
    }
    /* Update alpha blend register every frame (except on the final cleanup state) */
    if (gTasks[taskId].data[0] != 4)
        SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(gTasks[taskId].data[4], 0));
}

/**
 * FUNCTION: BattleIntroSlide3
 *
 * PURPOSE: Intro slide for building and plain terrains. Uses alpha blending
 * that starts at 50% transparency (BLDALPHA_BLEND(8,8)) and gradually fades.
 *
 * HOW IT WORKS:
 * Similar to Slide2 but initializes blending from the start (state 0) rather
 * than waiting for the window to open. The blend ratio starts at 8:8 (50/50)
 * and the first target coefficient decreases every 6 frames, creating a
 * slower, more subtle fade compared to Slide2's faster 4-frame steps.
 *
 * @param taskId — task identifier
 */
static void BattleIntroSlide3(u8 taskId)
{
    s32 i;

    gBattle_BG1_X += 8;
    switch (gTasks[taskId].data[0])
    {
    case 0:
        /* Set up blending from the start with 50% transparency */
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ);
        SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(8, 8));
        SetGpuReg(REG_OFFSET_BLDY, 0);
        gTasks[taskId].data[4] = BLDALPHA_BLEND(8, 8);
        if (gBattleTypeFlags & BATTLE_TYPE_LINK)
        {
            gTasks[taskId].data[2] = 16;
            ++gTasks[taskId].data[0];
        }
        else
        {
            gTasks[taskId].data[2] = 1;
            ++gTasks[taskId].data[0];
        }
        break;
    case 1:
        if (--gTasks[taskId].data[2] == 0)
        {
            ++gTasks[taskId].data[0];
            SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL | WININ_WIN0_OBJ | WININ_WIN0_CLR);
        }
        break;
    case 2:
        gBattle_WIN0V -= 0xFF;
        if ((gBattle_WIN0V & 0xFF00) == 0x3000)
        {
            ++gTasks[taskId].data[0];
            gTasks[taskId].data[2] = 240;
            gTasks[taskId].data[3] = 32;
            gTasks[taskId].data[5] = 1;
            gIntroSlideFlags &= ~1;
        }
        break;
    case 3:
        if (gTasks[taskId].data[3])
        {
            --gTasks[taskId].data[3];
        }
        else if ((gTasks[taskId].data[4] & 0xF) && --gTasks[taskId].data[5] == 0)
        {
            /* Decrease alpha every 6 frames (slower than Slide2's every 4 frames) */
            gTasks[taskId].data[4] += 0xFF;
            gTasks[taskId].data[5] = 6;
        }
        if (gBattle_WIN0V & 0xFF00)
            gBattle_WIN0V -= 0x3FC;
        if (gTasks[taskId].data[2])
            gTasks[taskId].data[2] -= 2;
        for (i = 0; i < 80; ++i)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gTasks[taskId].data[2];
        while (i < 160)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i++] = -gTasks[taskId].data[2];
        if (!gTasks[taskId].data[2])
        {
            gScanlineEffect.state = 3;
            ++gTasks[taskId].data[0];
            CpuFill32(0, (void *)BG_SCREEN_ADDR(28), BG_SCREEN_SIZE);
            SetBgAttribute(1, BG_ATTR_CHARBASEINDEX, 0);
            SetBgAttribute(2, BG_ATTR_CHARBASEINDEX, 0);
            SetGpuReg(REG_OFFSET_BG1CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(28) | BGCNT_TXT256x512);
            SetGpuReg(REG_OFFSET_BG2CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(30) | BGCNT_TXT512x256);
        }
        break;
    case 4:
        BattleIntroSlideEnd(taskId);
        break;
    }
    if (gTasks[taskId].data[0] != 4)
        SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(gTasks[taskId].data[4], 0));
}

/**
 * FUNCTION: BattleIntroSlideLink
 *
 * PURPOSE: Special intro slide for link (multiplayer) battles. Features the "VS"
 * letters that appear using the Object Window (OBJ Window) system.
 *
 * HOW IT WORKS:
 * State 0: Wait 32 frames for link sync
 * State 1: After delay, activate the VS letter sprites using the OBJ window mode.
 *   OBJ_WINDOW mode means the sprite itself is invisible, but it defines a window
 *   region — areas where the sprite pixels are non-transparent become part of the
 *   "object window" region, which can have different layer visibility settings.
 *   This creates the effect of the VS letters acting as "cut-outs" in the scene.
 * State 2-3: Same window opening and scanline split-scroll as other slides
 *   But BG1 and BG2 scroll in opposite directions (not split-screen scanline).
 * State 4: Cleanup
 *
 * GBA CONTEXT — OBJECT WINDOW:
 * The GBA has a special window type called the "OBJ Window." Sprites set to
 * ST_OAM_OBJ_WINDOW mode don't render normally — instead, their non-transparent
 * pixels define a window region. The WINOUT register's WINOBJ bits control what
 * layers are visible in this sprite-shaped window. This allows arbitrarily-shaped
 * window regions (unlike WIN0/WIN1 which are always rectangles).
 *
 * @param taskId — task identifier
 */
static void BattleIntroSlideLink(u8 taskId)
{
    s32 i;

    /* After state 1, slide BG1 and BG2 in opposite directions */
    if (gTasks[taskId].data[0] > 1 && !gTasks[taskId].data[4])
    {
        u16 var0 = gBattle_BG1_X & 0x8000;

        if (var0 || gBattle_BG1_X < 80)
        {
            gBattle_BG1_X += 3;   /* BG1 slides right */
            gBattle_BG2_X -= 3;   /* BG2 slides left */
        }
        else
        {
            /* Slides complete — clear both terrain tilemaps */
            CpuFill32(0, (void *)BG_SCREEN_ADDR(28), BG_SCREEN_SIZE);
            CpuFill32(0, (void *)BG_SCREEN_ADDR(30), BG_SCREEN_SIZE);
            gTasks[taskId].data[4] = 1;  /* Signal slides are done */
        }
    }
    switch (gTasks[taskId].data[0])
    {
    case 0:
        gTasks[taskId].data[2] = 32;  /* 32-frame delay for link sync */
        ++gTasks[taskId].data[0];
        break;
    case 1:
        if (--gTasks[taskId].data[2] == 0)
        {
            ++gTasks[taskId].data[0];
            /*
             * Set VS letter sprites to OBJ_WINDOW mode — their pixels define
             * a window region instead of rendering visually. Start their animation.
             */
            gSprites[gBattleStruct->linkBattleVsSpriteId_V].oam.objMode = ST_OAM_OBJ_WINDOW;
            gSprites[gBattleStruct->linkBattleVsSpriteId_V].callback = SpriteCB_VsLetterInit;
            gSprites[gBattleStruct->linkBattleVsSpriteId_S].oam.objMode = ST_OAM_OBJ_WINDOW;
            gSprites[gBattleStruct->linkBattleVsSpriteId_S].callback = SpriteCB_VsLetterInit;
            /* Configure window visibility:
             * Inside WIN0: show everything (the battle scene)
             * Outside all windows: only show BG1 and BG2 (the terrain) + OBJ window contents
             */
            SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL | WININ_WIN0_OBJ | WININ_WIN0_CLR);
            SetGpuReg(REG_OFFSET_WINOUT, WINOUT_WINOBJ_BG_ALL | WINOUT_WINOBJ_OBJ | WINOUT_WINOBJ_CLR | WINOUT_WIN01_BG1 | WINOUT_WIN01_BG2);
        }
        break;
    case 2:
        gBattle_WIN0V -= 0xFF;
        if ((gBattle_WIN0V & 0xFF00) == 0x3000)
        {
            ++gTasks[taskId].data[0];
            gTasks[taskId].data[2] = 240;
            gTasks[taskId].data[3] = 32;
            gIntroSlideFlags &= ~1;
        }
        break;
    case 3:
        if (gBattle_WIN0V & 0xFF00)
            gBattle_WIN0V -= 0x3FC;
        if (gTasks[taskId].data[2])
            gTasks[taskId].data[2] -= 2;
        for (i = 0; i < 80; ++i)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gTasks[taskId].data[2];
        while (i < 160)
            gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i++] = -gTasks[taskId].data[2];
        if (!gTasks[taskId].data[2])
        {
            gScanlineEffect.state = 3;
            ++gTasks[taskId].data[0];
            SetBgAttribute(1, BG_ATTR_CHARBASEINDEX, 0);
            SetBgAttribute(2, BG_ATTR_CHARBASEINDEX, 0);
            SetGpuReg(REG_OFFSET_BG1CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(28) | BGCNT_TXT256x512);
            SetGpuReg(REG_OFFSET_BG2CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(30) | BGCNT_TXT512x256);
        }
        break;
    case 4:
        BattleIntroSlideEnd(taskId);
        break;
    }
}

/**
 * FUNCTION: CopyBattlerSpriteToBg
 *
 * PURPOSE: Copies a battler's (Pokemon or trainer) sprite from its sprite sheet
 * buffer to a background layer. Used during move animations that need to display
 * a battler's image on a BG layer instead of as a sprite (for special effects
 * like the substitute doll or transform animation).
 *
 * HOW IT WORKS:
 * 1. Copies the sprite's pixel data from gMonSpritesGfxPtr to the tile buffer
 * 2. Loads these tiles into the specified BG's character data area
 * 3. Builds a tilemap that arranges the tiles in an 8x8 grid at position (x,y)
 * 4. Each tilemap entry includes the tile offset and palette number
 *
 * GBA CONTEXT:
 * Sprites and backgrounds use different rendering systems on the GBA. Sometimes
 * a sprite needs to be rendered as a BG (e.g., for rotation/scaling effects that
 * work differently on BGs vs sprites). This function bridges that gap by copying
 * sprite data into BG format.
 *
 * The tilemap format: each u16 entry is (paletteNum << 12) | tileOffset.
 * This tells the GPU which tile to draw and which palette to color it with.
 *
 * @param bgId           — target background layer (0-3)
 * @param x              — X position in tile coordinates (0-31)
 * @param y              — Y position in tile coordinates (0-31)
 * @param battlerPosition — which battler's sprite to copy
 * @param palno          — palette number to use (0-15)
 * @param tilesDest      — buffer for tile data
 * @param tilemapDest    — buffer for tilemap data
 * @param tilesOffset    — starting tile index offset in VRAM
 */
void CopyBattlerSpriteToBg(s32 bgId, u8 x, u8 y, u8 battlerPosition, u8 palno, u8 *tilesDest, u16 *tilemapDest, u16 tilesOffset)
{
    s32 i, j;
    u8 battler = GetBattlerAtPosition(battlerPosition);
    s32 offset = tilesOffset;

    /*
     * Copy sprite pixel data. BG_SCREEN_SIZE (0x800) bytes per frame.
     * gBattleMonForms[battler] selects which animation frame to use.
     */
    CpuCopy16(gMonSpritesGfxPtr->sprites[battlerPosition] + BG_SCREEN_SIZE * gBattleMonForms[battler], tilesDest, BG_SCREEN_SIZE);
    /* Load the tile data into the BG's VRAM character area */
    LoadBgTiles(bgId, tilesDest, 0x1000, tilesOffset);

    /* Build the tilemap: an 8x8 grid of tiles starting at (x, y) */
    for (i = y; i < y + 8; ++i)
        for (j = x; j < x + 8; ++j)
            tilemapDest[i * 32 + j] = offset++ | (palno << 12);
    LoadBgTilemap(bgId, tilemapDest, BG_SCREEN_SIZE, 0);
}

/**
 * FUNCTION: DrawBattlerOnBgDMA
 *
 * PURPOSE: (Unused) Copies a battler sprite directly to BG VRAM using DMA.
 * Similar to CopyBattlerSpriteToBg but uses DMA transfers and writes directly
 * to VRAM addresses rather than using the BG management functions.
 *
 * GBA CONTEXT:
 * DmaCopy16(3, ...) uses DMA channel 3 for a 16-bit copy. This is faster than
 * CPU copy but writes directly to hardware-mapped memory addresses.
 * BG_SCREEN_ADDR(0) is 0x06000000 (start of BG VRAM).
 * BG_VRAM is also 0x06000000. The tilemap is written directly to VRAM.
 */
static void DrawBattlerOnBgDMA(u8 arg0, u8 arg1, u8 battlerPosition, u8 arg3, u8 arg4, u16 arg5, u8 arg6, u8 arg7)
{
    s32 i, j, offset;

    DmaCopy16(3, gMonSpritesGfxPtr->sprites[battlerPosition] + BG_SCREEN_SIZE * arg3, (void *)BG_SCREEN_ADDR(0) + arg5, BG_SCREEN_SIZE);
    offset = (arg5 >> 5) - (arg7 << 9);
    for (i = arg1; i < arg1 + 8; ++i)
        for (j = arg0; j < arg0 + 8; ++j)
            *((u16 *)(BG_VRAM) + (i * 32) + (j + (arg6 << 10))) = offset++ | (arg4 << 12);
}
