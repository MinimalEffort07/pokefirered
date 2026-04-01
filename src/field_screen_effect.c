/**
 * @file field_screen_effect.c
 * @brief Overworld Screen Effects: Flash Circles and Barn Door Wipes
 *
 * FILE OVERVIEW:
 * This file implements two key visual effects used in the Pokemon FireRed overworld:
 *
 * 1. FLASH EFFECT: When the player uses the HM move Flash in dark caves (like Rock
 *    Tunnel), a circle of light expands or contracts on screen. The darkness is
 *    simulated by using the GBA's hardware windowing system — everything outside
 *    the light circle is blacked out. The circle is drawn using a variant of the
 *    Midpoint Circle Algorithm (Bresenham's circle).
 *
 * 2. BARN DOOR WIPE: A cinematic transition effect where two "doors" slide inward
 *    from the edges or outward from the center, like a barn door opening/closing.
 *    This uses the GBA's two hardware windows (WIN0 and WIN1) as the two "doors."
 *
 * 3. WHITEOUT RECOVERY: When all the player's Pokemon faint, a message is shown
 *    ("scurried to a Pokemon Center" or "scurried back home") and the player is
 *    warped to their last heal location.
 *
 * GBA HARDWARE CONTEXT — WINDOWING:
 * The GBA has a hardware windowing system with two rectangular windows (WIN0, WIN1)
 * and an "outside" region. Each window can independently show or hide background
 * layers, sprites, and color effects. By making everything outside the windows
 * black and the windows themselves transparent, you get a "spotlight" effect.
 *
 * The Flash effect goes further: it uses the Scanline Effect system (see
 * scanline_effect.c) to change the window boundaries on EVERY scanline, creating
 * a circular window shape instead of the default rectangle.
 */
#include "global.h"
#include "gflib.h"
#include "field_screen_effect.h"
#include "overworld.h"
#include "scanline_effect.h"
#include "script.h"
#include "task.h"
#include "strings.h"
#include "menu.h"
#include "heal_location.h"
#include "new_menu_helpers.h"
#include "event_object_movement.h"
#include "field_fadetransition.h"
#include "event_scripts.h"
#include "constants/heal_locations.h"
#include "constants/maps.h"

/*
 * Flash level to pixel radius mapping table.
 * Flash level 0 = full brightness (radius 200, larger than screen = no darkness).
 * Flash level 4 = maximum darkness (radius 24, tiny spotlight around player).
 * Each step reduces the visible circle, making the cave darker.
 *
 * The GBA screen is 240x160 pixels, so a radius of 200 covers the entire screen,
 * effectively making the cave fully lit (no visible darkness border).
 */
static const u16 sFlashLevelToRadius[] = { 200, 72, 56, 40, 24 };

/* Maximum valid flash level index (4 = darkest). Exported for use by other files. */
const s32 gMaxFlashLevel = ARRAY_COUNT(sFlashLevelToRadius) - 1;

/*
 * Window template for the whiteout recovery message box.
 * Positioned at the top-center of the screen (y=5 tiles down),
 * spanning nearly the full width (30 tiles = 240 pixels).
 * Uses palette 15 (standard text palette) and starts at tile base block 1.
 */
static const struct WindowTemplate sWindowTemplate_WhiteoutText =
{
    .bg = 0,
    .tilemapLeft = 0,
    .tilemapTop = 5,
    .width = 30,
    .height = 11,
    .paletteNum = 15,
    .baseBlock = 1,
};

/*
 * Text color triplet for whiteout message: {background, foreground, shadow}.
 * Transparent background so the black fade shows through, white text with dark gray shadow.
 */
static const u8 sWhiteoutTextColors[] = { TEXT_COLOR_TRANSPARENT, TEXT_COLOR_WHITE, TEXT_COLOR_DARK_GRAY };

static void Task_EnableScriptAfterMusicFade(u8 taskId);
static void Task_BarnDoorWipeChild(u8 taskId);

/**
 * FUNCTION: SetFlashScanlineEffectWindowBoundary
 *
 * PURPOSE: Sets the horizontal window boundaries for a single scanline, used to
 * build a circular spotlight effect line-by-line.
 *
 * HOW IT WORKS:
 * Each entry in the scanline effect buffer represents one horizontal line of the screen.
 * The value stored encodes a left and right boundary: the high byte is the left edge
 * and the low byte is the right edge. Everything between left and right is visible
 * (inside the "spotlight"); everything outside is darkened.
 *
 * GBA CONTEXT:
 * The GBA's WIN0H register (and WIN1H) uses this exact format: bits 8-15 are the
 * left coordinate, bits 0-7 are the right coordinate. The scanline effect system
 * writes these values to WIN0H on each scanline during HBlank (the brief pause
 * between drawing each horizontal line), creating a non-rectangular window shape.
 *
 * @param dest  — pointer to the scanline buffer (one u16 per scanline)
 * @param y     — the scanline (row) to set (0-160, the GBA screen height)
 * @param left  — left edge of the visible region (clamped to 0-255)
 * @param right — right edge of the visible region (clamped to 0-255)
 */
static void SetFlashScanlineEffectWindowBoundary(u16 *dest, u32 y, s32 left, s32 right)
{
    /* Only set boundaries for visible scanlines (GBA screen is 160 lines tall) */
    if (y <= 160)
    {
        /* Clamp left and right to valid pixel range (0-255, the max for an 8-bit field) */
        if (left < 0)
            left = 0;
        if (left > 255)
            left = 255;
        if (right < 0)
            right = 0;
        if (right > 255)
            right = 255;
        /*
         * Pack left and right into a single u16 in WIN0H format:
         * Bits 8-15 = left edge, Bits 0-7 = right edge.
         * The GPU will display pixels between left and right on this scanline.
         */
        dest[y] = (left << 8) | right;
    }
}

/*
 * Draws a circle by approximating xy^2 + yx^2 = radius^2.
 *
 * error is approximately xy^2 - yx^2. Negative values mean the circle is
 * slightly too large, and positive values mean the circle is slightly
 * too small. By decreasing xy whenever the error becomes negative the
 * code slightly under-approximates the size of the circle.
 *
 * The subtractive terms compute yx^2 - (yx - 1)^2, and therefore the sum
 * is yx^2 - 1:
 *   yx               |  0 |  1 |  2 |  3 |  4 |  5 |  6 |  7
 *   (yx * 2) - 1     | -1 |  1 |  3 |  5 |  7 |  9 | 11 | 13
 *   yx^2 - (yx-1)^2  | -1 |  1 |  3 |  5 |  7 |  9 | 11 | 13
 *   cumulative error | -1 |  0 |  3 |  8 | 15 | 24 | 35 | 48
 *   yx^2             |  0 |  1 |  4 |  9 | 16 | 25 | 36 | 49
 *
 * The additive terms compute xy^2 - (xy - 1)^2 - 1, and therefore the sum
 * (roughly) approximates the cumulative squared differences.
 *
 * The error is initialized to r, which corrects for rounding. The algorithm
 * exploits 4-way symmetry to compute boundaries in both directions out from
 * centerY (using yx for y), and also both directions *in* from
 * centerY +/- radius (using xy for y). Because xy doesn't change on every
 * iteration, we frequently overwrite boundaries set in the previous iteration.
 */

/**
 * FUNCTION: SetFlashScanlineEffectWindowBoundaries
 *
 * PURPOSE: Generates a circular spotlight shape by computing per-scanline window
 * boundaries using a Midpoint Circle Algorithm variant (Bresenham's circle).
 *
 * HOW IT WORKS:
 * Starting from the top of the circle, the algorithm walks around the first octant
 * and mirrors the result to all 8 octants via symmetry. For each computed point
 * (xy, yx), it sets the horizontal window boundaries on 4 scanlines:
 *   - centerY - yx (above center, wide span using xy)
 *   - centerY + yx (below center, wide span using xy)
 *   - centerY - xy (far above center, narrow span using yx)
 *   - centerY + xy (far below center, narrow span using yx)
 *
 * The result is a filled circle where each scanline's visible region corresponds
 * to the chord of the circle at that height.
 *
 * VISUAL EXAMPLE: For a circle centered at (120, 80) with radius 40:
 *   Scanline 40: visible from x=120 to x=120 (just 1 pixel — top of circle)
 *   Scanline 60: visible from x=85 to x=155 (wider chord)
 *   Scanline 80: visible from x=80 to x=160 (widest — the diameter)
 *   Scanline 100: visible from x=85 to x=155 (symmetric below)
 *   Scanline 120: visible from x=120 to x=120 (bottom of circle)
 *
 * @param dest    — scanline buffer to fill (one u16 per scanline)
 * @param centerX — X center of the circle (typically 120, screen center)
 * @param centerY — Y center of the circle (typically 80, screen center)
 * @param radius  — radius in pixels (from sFlashLevelToRadius[])
 */
void SetFlashScanlineEffectWindowBoundaries(u16 *dest, s32 centerX, s32 centerY, s32 radius)
{
    s32 xy = radius;       /* X coordinate (starts at radius, decreases toward center) */
    s32 error = radius;    /* Error accumulator for the Bresenham decision variable */
    s32 yx = 0;            /* Y coordinate (starts at 0, increases outward from center) */

    /* Walk from the top of the circle to the 45-degree line (where yx meets xy) */
    while (xy >= yx)
    {
        /* Set boundaries using 4-way symmetry: above/below center, wide/narrow spans */
        SetFlashScanlineEffectWindowBoundary(dest, centerY - yx, centerX - xy, centerX + xy);
        SetFlashScanlineEffectWindowBoundary(dest, centerY + yx, centerX - xy, centerX + xy);
        SetFlashScanlineEffectWindowBoundary(dest, centerY - xy, centerX - yx, centerX + yx);
        SetFlashScanlineEffectWindowBoundary(dest, centerY + xy, centerX - yx, centerX + yx);

        /* Update the Bresenham error term and advance yx */
        error -= (yx * 2) - 1;
        yx++;

        /* If error goes negative, the circle outline has moved inward — decrease xy */
        if (error < 0)
        {
            error += 2 * (xy - 1);
            xy--;
        }
    }
}

/* ========================================================================
 * FLASH LEVEL ANIMATION SYSTEM
 * ========================================================================
 * These task data fields control the flash circle animation.
 * Tasks use their data[] array as local variables (see task.c).
 */
#define tState               data[0]  /* Current state in the animation state machine */
#define tFlashCenterX        data[1]  /* X center of the flash circle (screen coords) */
#define tFlashCenterY        data[2]  /* Y center of the flash circle (screen coords) */
#define tCurFlashRadius      data[3]  /* Current radius — animates toward destination */
#define tDestFlashRadius     data[4]  /* Target radius to animate toward */
#define tFlashRadiusDelta    data[5]  /* How many pixels to grow/shrink per frame (+/-) */
#define tClearScanlineEffect data[6]  /* If TRUE, stop scanline effect when done (full brightness) */

/**
 * FUNCTION: UpdateFlashLevelEffect
 *
 * PURPOSE: Animates the flash circle radius from its current size to a target size,
 * expanding or contracting the spotlight each frame.
 *
 * HOW IT WORKS:
 * Uses a 3-state machine that alternates between two double-buffered frames:
 * - State 0: Draws the circle into one scanline buffer
 * - State 1: Draws into the other buffer, then advances the radius
 * The double-buffering prevents visual tearing — while the GPU reads from one
 * buffer to draw the current frame, we write the next frame's circle into the
 * other buffer. The scanline effect system swaps buffers during VBlank.
 *
 * When the radius reaches the destination:
 * - If transitioning to full brightness (clearScanlineEffect), stops the scanline
 *   effect entirely (no more per-scanline window manipulation = full bright screen)
 * - Otherwise, just destroys the task (leaving the current circle in place)
 *
 * GBA CONTEXT:
 * The double-buffering pattern (alternating gScanlineEffect.srcBuffer) is critical
 * because the HBlank DMA that creates the circle effect reads from the buffer
 * continuously. Writing to the same buffer the DMA is reading would cause tearing.
 *
 * @param taskId — task identifier for this animation
 */
static void UpdateFlashLevelEffect(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case 0:
        /* Draw circle into the current source buffer (frame A) */
        SetFlashScanlineEffectWindowBoundaries(gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer], tFlashCenterX, tFlashCenterY, tCurFlashRadius);
        tState = 1;
        break;
    case 1:
        /* Draw circle into the other buffer (frame B), then advance the radius */
        SetFlashScanlineEffectWindowBoundaries(gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer], tFlashCenterX, tFlashCenterY, tCurFlashRadius);
        tState = 0;
        tCurFlashRadius += tFlashRadiusDelta;  /* Grow or shrink the circle */

        /* Check if we've reached (or passed) the destination radius */
        if (tCurFlashRadius > tDestFlashRadius)
        {
            if (tClearScanlineEffect == TRUE)
            {
                /* Going to full brightness — stop the scanline effect hardware */
                ScanlineEffect_Stop();
                tState = 2;  /* Go to cleanup state */
            }
            else
            {
                /* Staying at a darkness level — leave scanline effect running */
                DestroyTask(taskId);
            }
        }
        break;
    case 2:
        /* Cleanup: clear scanline buffers and destroy task */
        ScanlineEffect_Clear();
        DestroyTask(taskId);
        break;
    }
}

/**
 * FUNCTION: Task_WaitForFlashUpdate
 *
 * PURPOSE: Waits for the flash animation to complete, then re-enables the script engine.
 *
 * GAME LOGIC:
 * Flash is triggered by a script command. This task blocks script execution until
 * the visual animation finishes, then resumes the script. This is a common pattern
 * in Pokemon: script starts effect -> creates waiting task -> task destroys itself
 * and re-enables scripts when effect is done.
 *
 * @param taskId — task identifier
 */
static void Task_WaitForFlashUpdate(u8 taskId)
{
    if (!FuncIsActiveTask(UpdateFlashLevelEffect))
    {
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
}

/**
 * FUNCTION: StartWaitForFlashUpdate
 *
 * PURPOSE: Creates the waiting task if it doesn't already exist.
 * Guards against duplicate creation (idempotent).
 */
static void StartWaitForFlashUpdate(void)
{
    if (!FuncIsActiveTask(Task_WaitForFlashUpdate))
        CreateTask(Task_WaitForFlashUpdate, 80);
}

/**
 * FUNCTION: StartUpdateFlashLevelEffect
 *
 * PURPOSE: Creates and configures a flash circle animation task.
 *
 * HOW IT WORKS:
 * Sets up all the parameters for the animation: center position, start/end radii,
 * and whether to clear the scanline effect when done. The delta direction (positive
 * or negative) is automatically determined — if the destination radius is larger,
 * delta is positive (circle grows); if smaller, delta is negative (circle shrinks).
 *
 * @param centerX            — X center of the flash circle
 * @param centerY            — Y center of the flash circle
 * @param initialFlashRadius — starting radius (from current flash level)
 * @param destFlashRadius    — target radius (from new flash level)
 * @param clearScanlineEffect — TRUE if going to full brightness (flash level 0)
 * @param delta              — magnitude of radius change per frame (sign is auto-set)
 * @return task ID of the created animation task
 */
static u8 StartUpdateFlashLevelEffect(s32 centerX, s32 centerY, s32 initialFlashRadius, s32 destFlashRadius, bool32 clearScanlineEffect, u8 delta)
{
    u8 taskId = CreateTask(UpdateFlashLevelEffect, 80);
    s16 *data = gTasks[taskId].data;

    tCurFlashRadius = initialFlashRadius;
    tDestFlashRadius = destFlashRadius;
    tFlashCenterX = centerX;
    tFlashCenterY = centerY;
    tClearScanlineEffect = clearScanlineEffect;

    /* Automatically set direction based on whether circle is growing or shrinking */
    if (initialFlashRadius < destFlashRadius)
        tFlashRadiusDelta = delta;   /* Growing (getting brighter) = positive delta */
    else
        tFlashRadiusDelta = -delta;  /* Shrinking (getting darker) = negative delta */

    return taskId;
}

#undef tState
#undef tCurFlashRadius
#undef tDestFlashRadius
#undef tFlashRadiusDelta
#undef tClearScanlineEffect

/**
 * FUNCTION: AnimateFlash
 *
 * PURPOSE: Plays the flash level transition animation when the player uses Flash
 * or enters/exits a dark cave area.
 *
 * HOW IT WORKS:
 * Looks up the current and new flash level radii, then starts the circle animation
 * between them. If the new level is 0 (full brightness), flags the scanline effect
 * for removal. Locks player controls during the animation so the player can't walk
 * while the circle is expanding/contracting.
 *
 * GAME LOGIC:
 * A higher flash level = smaller radius = more darkness. Flash level 0 means
 * no darkness at all (radius 200 covers entire screen). When entering Rock Tunnel
 * without Flash, the level might be 4 (radius 24 = tiny spotlight).
 *
 * @param newFlashLevel — target flash level (0 = bright, 4 = very dark)
 */
void AnimateFlash(u8 newFlashLevel)
{
    u8 curFlashLevel = Overworld_GetFlashLevel();
    bool32 fullBrightness = FALSE;
    if (newFlashLevel == 0)
        fullBrightness = TRUE;  /* Will clear scanline effect when animation completes */

    /* Start animation from current radius to new radius, advancing 2 pixels per frame */
    StartUpdateFlashLevelEffect(DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, sFlashLevelToRadius[curFlashLevel], sFlashLevelToRadius[newFlashLevel], fullBrightness, 2);
    StartWaitForFlashUpdate();
    LockPlayerFieldControls();  /* Prevent player movement during animation */
}

/**
 * FUNCTION: WriteFlashScanlineEffectBuffer
 *
 * PURPOSE: Draws the static flash circle for the initial cave entry (no animation,
 * just sets up the circle at the given flash level).
 *
 * HOW IT WORKS:
 * Draws the circle into buffer 0, then copies it to buffer 1. Both buffers now
 * have the same circle, so the double-buffered scanline effect can start immediately
 * without one frame of garbage data.
 *
 * GBA CONTEXT:
 * CpuFastCopy is a fast memory copy using 32-bit transfers. The buffer size is
 * 240 * 8 bytes = 240 entries * 4 bytes each (240 scanlines * 2 buffers worth,
 * but actually this copies the full buffer pair).
 *
 * @param flashLevel — current flash/darkness level (0 = no effect, 1-4 = circle)
 */
void WriteFlashScanlineEffectBuffer(u8 flashLevel)
{
    if (flashLevel)
    {
        /* Draw circle centered at screen center (120, 80) with appropriate radius */
        SetFlashScanlineEffectWindowBoundaries(&gScanlineEffectRegBuffers[0][0], 120, 80, sFlashLevelToRadius[flashLevel]);
        /* Copy buffer 0 to buffer 1 so both buffers match (no tearing on first frame) */
        CpuFastCopy(&gScanlineEffectRegBuffers[0], &gScanlineEffectRegBuffers[1], 240 * 8);
    }
}

/* ========================================================================
 * MUSIC FADE SCRIPT INTEGRATION
 * ======================================================================== */

/**
 * FUNCTION: Script_FadeOutMapMusic
 *
 * PURPOSE: Fades out the current map's background music and pauses the script
 * engine until the fade completes.
 *
 * GAME LOGIC:
 * Used in scripted events that need silence before a dramatic moment (e.g.,
 * encountering a legendary Pokemon, entering a cutscene).
 */
void Script_FadeOutMapMusic(void)
{
    Overworld_FadeOutMapMusic();
    CreateTask(Task_EnableScriptAfterMusicFade, 80);
}

/**
 * FUNCTION: Task_EnableScriptAfterMusicFade
 *
 * PURPOSE: Polls each frame to check if background music has stopped. Once it has,
 * re-enables the script engine and destroys itself.
 *
 * @param taskId — task identifier
 */
static void Task_EnableScriptAfterMusicFade(u8 taskId)
{
    if (BGMusicStopped() == TRUE)
    {
        DestroyTask(taskId);
        ScriptContext_Enable();
    }
}

/* ========================================================================
 * BARN DOOR WIPE TRANSITION EFFECT
 * ========================================================================
 * A cinematic wipe effect using two hardware windows that slide symmetrically:
 * - WIPE IN: Two black bars slide from the edges toward the center (closing)
 * - WIPE OUT: Two black bars slide from the center toward the edges (opening)
 *
 * GBA CONTEXT:
 * The GBA has two hardware windows (WIN0, WIN1). Each window defines a rectangular
 * region of the screen. WININ controls what's visible INSIDE the windows, WINOUT
 * controls what's visible OUTSIDE. By setting WININ to show nothing and WINOUT to
 * show everything, the windows become "black bars" that obscure the screen.
 * Sliding the window boundaries creates the barn door effect.
 */

#define tState data[9]           /* State machine index for the parent wipe task */
#define tDirection data[10]      /* Wipe direction: 0 = inward, 1 = outward */
#define DIR_WIPE_IN 0            /* Black bars close from edges to center */
#define DIR_WIPE_OUT 1           /* Black bars open from center to edges */
#define tChildOffset data[0]     /* Current pixel offset of the sliding bars (child task) */

/**
 * FUNCTION: DoInwardBarnDoorFade
 *
 * PURPOSE: Starts a barn door wipe that closes inward (edges to center).
 * Used for dramatic transitions like entering a new scene.
 */
static void DoInwardBarnDoorFade(void)
{
    u8 taskId = CreateTask(Task_BarnDoorWipe, 80);
    gTasks[taskId].tDirection = DIR_WIPE_IN;
}

/**
 * FUNCTION: DoOutwardBarnDoorWipe
 *
 * PURPOSE: Starts a barn door wipe that opens outward (center to edges).
 * Used for revealing a new scene after a transition.
 */
void DoOutwardBarnDoorWipe(void)
{
    u8 taskId = CreateTask(Task_BarnDoorWipe, 80);
    gTasks[taskId].tDirection = DIR_WIPE_OUT;
}

/**
 * FUNCTION: BarnDoorWipeSaveGpuRegs
 *
 * PURPOSE: Saves the current GPU register state before the wipe modifies them.
 *
 * HOW IT WORKS:
 * The barn door effect needs to reconfigure the display, window, and blending
 * registers. Before doing so, it saves the current values into the task's data
 * array so they can be restored after the wipe completes. This prevents the
 * wipe from permanently corrupting the display settings.
 *
 * GBA CONTEXT:
 * The registers saved are:
 * - DISPCNT: Master display control (which layers/windows are enabled)
 * - WININ/WINOUT: What's visible inside/outside the hardware windows
 * - BLDCNT/BLDALPHA: Color blending/transparency settings
 * - WIN0H/WIN0V/WIN1H/WIN1V: Window 0 and 1 boundary rectangles
 *
 * @param taskId — task whose data array stores the saved register values
 */
static void BarnDoorWipeSaveGpuRegs(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    data[0] = GetGpuReg(REG_OFFSET_DISPCNT);
    data[1] = GetGpuReg(REG_OFFSET_WININ);
    data[2] = GetGpuReg(REG_OFFSET_WINOUT);
    data[3] = GetGpuReg(REG_OFFSET_BLDCNT);
    data[4] = GetGpuReg(REG_OFFSET_BLDALPHA);
    data[5] = GetGpuReg(REG_OFFSET_WIN0H);
    data[6] = GetGpuReg(REG_OFFSET_WIN0V);
    data[7] = GetGpuReg(REG_OFFSET_WIN1H);
    data[8] = GetGpuReg(REG_OFFSET_WIN1V);
}

/**
 * FUNCTION: BarnDoorWipeLoadGpuRegs
 *
 * PURPOSE: Restores GPU registers to their pre-wipe state.
 * Called after the wipe animation completes to undo all window/blend changes.
 *
 * @param taskId — task containing the saved register values
 */
static void BarnDoorWipeLoadGpuRegs(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    SetGpuReg(REG_OFFSET_DISPCNT, data[0]);
    SetGpuReg(REG_OFFSET_WININ, data[1]);
    SetGpuReg(REG_OFFSET_WINOUT, data[2]);
    SetGpuReg(REG_OFFSET_BLDCNT, data[3]);
    SetGpuReg(REG_OFFSET_BLDALPHA, data[4]);
    SetGpuReg(REG_OFFSET_WIN0H, data[5]);
    SetGpuReg(REG_OFFSET_WIN0V, data[6]);
    SetGpuReg(REG_OFFSET_WIN1H, data[7]);
    SetGpuReg(REG_OFFSET_WIN1V, data[8]);
}

/**
 * FUNCTION: Task_BarnDoorWipe
 *
 * PURPOSE: Parent task that manages the barn door wipe effect lifecycle:
 * save registers, configure windows, run animation, restore registers.
 *
 * HOW IT WORKS:
 * State 0 (Setup): Saves current GPU state, enables both hardware windows, and
 *   configures them based on wipe direction:
 *   - WIPE IN: Windows start at the edges (WIN0 at left edge, WIN1 at right edge)
 *     with nothing visible inside — black bars at the edges, scene visible in center
 *   - WIPE OUT: Windows start at center (both covering the middle), scene hidden
 * State 1: Spawns the child animation task that actually slides the windows
 * State 2: Waits for the child task to finish
 * State 3: Restores all GPU registers and destroys itself
 *
 * GBA CONTEXT:
 * WIN_RANGE(left, right) packs two 8-bit values into a 16-bit register.
 * WININ = 0 means nothing is visible inside the windows (they are opaque black).
 * WINOUT shows everything outside: all BG layers, OBJ (sprites), and color effects.
 * So the windows act as "erasers" — wherever they are, the screen is black.
 *
 * @param taskId — task identifier
 */
void Task_BarnDoorWipe(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    switch (tState)
    {
        case 0:
            /* Save current display settings so we can restore them later */
            BarnDoorWipeSaveGpuRegs(taskId);
            /* Enable both hardware windows in the display control register */
            SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_WIN0_ON);
            SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_WIN1_ON);

            if (data[10] == 0)  /* DIR_WIPE_IN: bars start at edges */
            {
                /* WIN0 covers left edge (x: 0 to 0 = zero width, will grow) */
                SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, 0));
                /* WIN1 covers right edge (x: 240 to 255 = off-screen, will grow leftward) */
                SetGpuReg(REG_OFFSET_WIN1H, WIN_RANGE(DISPLAY_WIDTH, 255));
                /* Both windows span full vertical height */
                SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(0, 255));
                SetGpuReg(REG_OFFSET_WIN1V, WIN_RANGE(0, 255));
            }
            else  /* DIR_WIPE_OUT: bars start at center */
            {
                /* WIN0 covers left half (x: 0 to 120) */
                SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, DISPLAY_WIDTH / 2));
                SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(0, 255));
                /* WIN1 covers right half (x: 120 to 255) */
                SetGpuReg(REG_OFFSET_WIN1H, WIN_RANGE(DISPLAY_WIDTH / 2, 255));
                SetGpuReg(REG_OFFSET_WIN1V, WIN_RANGE(0, 255));
            }
            /* Windows are opaque (nothing visible inside them = black) */
            SetGpuReg(REG_OFFSET_WININ, 0);
            /* Everything OUTSIDE windows is visible (all BGs, sprites, color effects) */
            SetGpuReg(REG_OFFSET_WINOUT, WINOUT_WIN01_BG_ALL | WINOUT_WIN01_OBJ | WINOUT_WIN01_CLR);
            tState = 1;
            break;
        case 1:
            /* Spawn the child task that performs the per-frame window sliding */
            CreateTask(Task_BarnDoorWipeChild, 80);
            tState = 2;
            break;
        case 2:
            /* Wait for the sliding animation child task to finish */
            if (!FuncIsActiveTask(Task_BarnDoorWipeChild))
            {
                tState = 3;
            }
            break;
        case 3:
            /* Restore all GPU registers to pre-wipe state and clean up */
            BarnDoorWipeLoadGpuRegs(taskId);
            DestroyTask(taskId);
            break;
    }
}

/**
 * FUNCTION: Task_BarnDoorWipeChild
 *
 * PURPOSE: Performs the per-frame sliding animation of the barn door wipe.
 *
 * HOW IT WORKS:
 * Each frame, advances the wipe offset and recalculates window boundaries:
 * - WIPE IN: Left bar grows rightward, right bar grows leftward. When they
 *   meet at the center (offset > DISPLAY_WIDTH/2 = 120), animation is done.
 * - WIPE OUT: Bars start at center and shrink outward. When the left edge
 *   goes below 0, animation is done.
 *
 * The speed is variable: moves 4 pixels/frame until offset reaches 90,
 * then slows to 2 pixels/frame. This creates a "fast start, slow finish"
 * easing effect that looks more natural than constant speed.
 *
 * @param taskId — task identifier
 */
static void Task_BarnDoorWipeChild(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
	u8 parentTaskId = FindTaskIdByFunc(Task_BarnDoorWipe);
    s16 lhs, rhs;

    if (gTasks[parentTaskId].tDirection == DIR_WIPE_IN)
    {
        /* Closing: left edge advances right, right edge advances left */
        lhs = tChildOffset;
        rhs = DISPLAY_WIDTH - tChildOffset;
        if (lhs > DISPLAY_WIDTH / 2)  /* Bars have met in the center */
        {
            DestroyTask(taskId);
            return;
        }
    }
    else
    {
        /* Opening: both edges retreat from center toward screen edges */
        lhs = DISPLAY_WIDTH / 2 - tChildOffset;
        rhs = DISPLAY_WIDTH / 2 + tChildOffset;
        if (lhs < 0)  /* Bars have moved past the screen edges */
        {
            DestroyTask(taskId);
            return;
        }
    }

    /* Update window boundaries: WIN0 covers 0..lhs, WIN1 covers rhs..240 */
    SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, lhs));
    SetGpuReg(REG_OFFSET_WIN1H, WIN_RANGE(rhs, DISPLAY_WIDTH));

    /* Speed easing: fast (4px/frame) until offset 90, then slow (2px/frame) */
    if (lhs < 90)
        tChildOffset += 4;
    else
        tChildOffset += 2;
}

#undef tState
#undef tDirection
#undef DIR_WIPE_IN
#undef DIR_WIPE_OUT
#undef tChildOffset

/* ========================================================================
 * WHITEOUT (ALL POKEMON FAINTED) RECOVERY SYSTEM
 * ========================================================================
 * When all the player's Pokemon faint, they "white out" and are transported
 * to their last heal location (Pokemon Center or home in Pallet Town).
 * This section handles displaying the recovery message and triggering the
 * appropriate post-whiteout event script.
 */

#define tState      data[0]   /* State machine index */
#define tWindowId   data[1]   /* Window ID for the message text */
#define tPrintState data[2]   /* Sub-state for text printing (0=start, 1=wait) */

/**
 * FUNCTION: PrintWhiteOutRecoveryMessage
 *
 * PURPOSE: Prints the whiteout recovery message character by character with a
 * typewriter effect, returning TRUE when printing is complete.
 *
 * HOW IT WORKS:
 * State 0: Clears the window, expands placeholder text (e.g., inserts player name),
 *   and starts the text printer with white-on-transparent colors.
 * State 1: Pumps the text printer each frame until all characters are printed.
 *
 * @param taskId — parent task ID (used to access shared state)
 * @param text   — the message string (with placeholders like {PLAYER})
 * @param x      — X position in pixels within the window
 * @param y      — Y position in pixels within the window
 * @return TRUE when the message has finished printing, FALSE while still in progress
 */
static bool8 PrintWhiteOutRecoveryMessage(u8 taskId, const u8 *text, u8 x, u8 y)
{
    u8 windowId = gTasks[taskId].tWindowId;

    switch (gTasks[taskId].tPrintState)
    {
    case 0:
        /* Clear the window to transparent, expand placeholders, and start printing */
        FillWindowPixelBuffer(windowId, PIXEL_FILL(0));
        StringExpandPlaceholders(gStringVar4, text);
        AddTextPrinterParameterized4(windowId, FONT_NORMAL, x, y, 1, 0, sWhiteoutTextColors, 1, gStringVar4);
        gTextFlags.canABSpeedUpPrint = FALSE;  /* Player can't skip through this text */
        gTasks[taskId].tPrintState = 1;
        break;
    case 1:
        /* Pump the text printer until it finishes */
        RunTextPrinters();
        if (!IsTextPrinterActive(windowId))
        {
            gTasks[taskId].tPrintState = 0;
            return TRUE;  /* Printing complete */
        }
        break;
    }
    return FALSE;  /* Still printing */
}

/**
 * FUNCTION: Task_RushInjuredPokemonToCenter
 *
 * PURPOSE: Handles the full whiteout recovery sequence: shows a message about
 * rushing to a Pokemon Center (or home), then fades to black and runs the
 * appropriate recovery event script.
 *
 * GAME LOGIC:
 * The message and post-whiteout script differ based on the player's last heal
 * location. If it's Pallet Town (the starting house), the message says "scurried
 * back home" and uses EventScript_AfterWhiteOutMomHeal. Otherwise, it says
 * "scurried to a Pokemon Center" and uses EventScript_AfterWhiteOutHeal.
 *
 * State flow (Pokemon Center path): 0 -> 1 -> 2 -> 3
 * State flow (Pallet Town path):    0 -> 4 -> 5 -> 6
 *
 * @param taskId — task identifier
 */
static void Task_RushInjuredPokemonToCenter(u8 taskId)
{
    u8 windowId;
    const struct HealLocation *loc;

    switch (gTasks[taskId].tState)
    {
    case 0:
        /* Set up the text window for the whiteout message */
        windowId = AddWindow(&sWindowTemplate_WhiteoutText);
        gTasks[taskId].tWindowId = windowId;
        Menu_LoadStdPalAt(BG_PLTT_ID(15));  /* Load text palette into slot 15 */
        FillWindowPixelBuffer(windowId, PIXEL_FILL(0));
        PutWindowTilemap(windowId);
        CopyWindowToVram(windowId, COPYWIN_FULL);

        /*
         * Check if the player's last heal location is their house in Pallet Town.
         * If so, use the "scurried back home" message path (states 4-6).
         * Otherwise, use the "scurried to a Pokemon Center" path (states 1-3).
         */
        loc = GetHealLocation(HEAL_LOCATION_PALLET_TOWN);
        if (gSaveBlock1Ptr->lastHealLocation.mapGroup == loc->mapGroup
         && gSaveBlock1Ptr->lastHealLocation.mapNum == loc->mapNum
         && gSaveBlock1Ptr->lastHealLocation.warpId == WARP_ID_NONE
         && gSaveBlock1Ptr->lastHealLocation.x == loc->x
         && gSaveBlock1Ptr->lastHealLocation.y == loc->y)
            gTasks[taskId].tState = 4;   /* Home path */
        else
            gTasks[taskId].tState = 1;   /* Pokemon Center path */
        break;

    case 1:
        /* Pokemon Center path: print "scurried to a Pokemon Center" message */
        if (PrintWhiteOutRecoveryMessage(taskId, gText_PlayerScurriedToCenter, 2, 8))
        {
            /* Turn player sprite to face up (north) as if rushing somewhere */
            ObjectEventTurn(&gObjectEvents[gPlayerAvatar.objectEventId], DIR_NORTH);
            gTasks[taskId].tState++;
        }
        break;
    case 4:
        /* Home path: print "scurried back home" message */
        if (PrintWhiteOutRecoveryMessage(taskId, gText_PlayerScurriedBackHome, 2, 8))
        {
            ObjectEventTurn(&gObjectEvents[gPlayerAvatar.objectEventId], DIR_NORTH);
            gTasks[taskId].tState++;
        }
        break;

    case 2:
    case 5:
        /* Shared cleanup: remove text window and start fade to black */
        windowId = gTasks[taskId].tWindowId;
        ClearWindowTilemap(windowId);
        CopyWindowToVram(windowId, COPYWIN_MAP);
        RemoveWindow(windowId);
        palette_bg_faded_fill_black();  /* Fill the faded palette buffer with black */
        FadeInFromBlack();              /* Start a fade-in transition (from black) */
        gTasks[taskId].tState++;
        break;

    case 3:
        /* Pokemon Center: wait for fade, then run the heal event script */
        if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
        {
            DestroyTask(taskId);
            ScriptContext_SetupScript(EventScript_AfterWhiteOutHeal);
        }
        break;
    case 6:
        /* Home: wait for fade, then run Mom's heal event script */
        if (FieldFadeTransitionBackgroundEffectIsFinished() == TRUE)
        {
            DestroyTask(taskId);
            ScriptContext_SetupScript(EventScript_AfterWhiteOutMomHeal);
        }
        break;
    }
}

/**
 * FUNCTION: FieldCB_RushInjuredPokemonToCenter
 *
 * PURPOSE: Entry point for the whiteout recovery sequence. Called when the overworld
 * loads after a battle where all Pokemon fainted.
 *
 * HOW IT WORKS:
 * Locks player controls (can't move during recovery), fills the screen with black
 * (so the warp destination isn't briefly visible), and starts the recovery task
 * that shows the message and triggers the appropriate script.
 *
 * GAME LOGIC:
 * This is set as the field callback (FieldCB) before warping to the heal location.
 * The overworld system calls this callback after the map has loaded, giving it a
 * chance to play the recovery message before the player gains control.
 */
void FieldCB_RushInjuredPokemonToCenter(void)
{
    u8 taskId;

    LockPlayerFieldControls();      /* Prevent player input during recovery */
    palette_bg_faded_fill_black();  /* Start with a black screen */
    taskId = CreateTask(Task_RushInjuredPokemonToCenter, 10);
    gTasks[taskId].tState = 0;
}
