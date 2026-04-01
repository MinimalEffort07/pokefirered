/*
 * scanline_effect.c - Per-Scanline GPU Register Manipulation (Raster Effects)
 *
 * ============================================================================
 * RASTER EFFECTS: CHANGING HARDWARE MID-FRAME
 * ============================================================================
 *
 * Normally, GPU registers are set once per frame during VBlank, and the GPU
 * uses those same values for all 160 scanlines. But what if you want
 * DIFFERENT values on different scanlines?
 *
 * This is called a "raster effect" (or "HBlank DMA effect"). It works by
 * using DMA Channel 0 to write a new value to a GPU register after EVERY
 * scanline (during HBlank - the brief pause between each scanline).
 *
 * EXAMPLES OF RASTER EFFECTS IN POKEMON:
 *   - Battle transition waves: The BG horizontal scroll (HOFS) is different
 *     on each scanline, creating a wavy distortion effect.
 *   - Water reflections: Different scroll values make the reflection ripple.
 *   - Circular screen wipe: A window register changes per scanline to
 *     create expanding/contracting circles.
 *
 * HOW IT WORKS:
 *
 * 1. Game code fills gScanlineEffectRegBuffers[N] with 160 values
 *    (one per visible scanline), representing the register value to use
 *    on each line.
 *
 * 2. ScanlineEffect_InitHBlankDmaTransfer() configures DMA0 to:
 *    - Source: gScanlineEffectRegBuffers[srcBuffer] (starting at entry 1)
 *    - Destination: A GPU register (e.g., REG_BG0HOFS)
 *    - Mode: DMA_START_HBLANK | DMA_REPEAT (trigger on each HBlank)
 *    - Count: 1 (transfer one value per HBlank)
 *
 * 3. The first scanline's value is set manually (because the first HBlank
 *    DMA fires AFTER scanline 0 has been drawn, so we need to set
 *    the value before scanline 0 starts).
 *
 * 4. For the remaining 159 scanlines, DMA0 automatically writes the
 *    next value from the buffer to the register during each HBlank.
 *
 * DOUBLE BUFFERING:
 *   Two buffers (gScanlineEffectRegBuffers[0] and [1]) are used.
 *   While DMA reads from one buffer, the game writes new values to the
 *   other. srcBuffer toggles (XOR 1) each frame to swap them.
 *   This prevents visual artifacts from partially-written buffers.
 *
 * WAVE EFFECT:
 *   The wave effect (ScanlineEffect_InitWave) pre-computes a sine wave
 *   table and uses a task to shift the wave pattern upward each frame,
 *   creating the classic "heat haze" / "underwater" distortion.
 *
 * ============================================================================
 */

#include "global.h"
#include "task.h"
#include "trig.h"
#include "scanline_effect.h"

/* Battle BG scroll positions - needed to add battle-specific offsets to wave values */
extern u16 gBattle_BG0_X;
extern u16 gBattle_BG0_Y;
extern u16 gBattle_BG1_X;
extern u16 gBattle_BG1_Y;
extern u16 gBattle_BG2_X;
extern u16 gBattle_BG2_Y;
extern u16 gBattle_BG3_X;
extern u16 gBattle_BG3_Y;

static void CopyValue16Bit(void);
static void CopyValue32Bit(void);

/*
 * Per-scanline register value buffers.
 * Two buffers of 960 (0x3C0) entries each:
 *   Entries 0-159:   Scanline values for the current frame
 *   Entries 160-319: Scanline values for the alternate buffer
 *   Entries 320-959: Scratch space for wave pre-computation
 *
 * DOUBLE BUFFERED: DMA reads from one while the CPU writes to the other.
 * Stored in EWRAM because they're too large for IWRAM (3840 bytes each).
 */
EWRAM_DATA u16 gScanlineEffectRegBuffers[2][0x3C0] = {0};

/* Main scanline effect state structure */
EWRAM_DATA struct ScanlineEffect gScanlineEffect = {0};

/* Flag to signal the wave task to self-destruct */
EWRAM_DATA static bool8 sShouldStopWaveTask = FALSE;

/**
 * FUNCTION: ScanlineEffect_Stop
 *
 * PURPOSE: Halt the scanline effect and stop DMA0.
 *
 * HOW IT WORKS:
 * Sets the state to 0 (inactive), stops DMA channel 0, and destroys
 * the wave task if one is running. DMA0 must be stopped explicitly
 * because it's set to REPEAT mode (it would keep firing every HBlank).
 *
 * GBA CONTEXT:
 * DMA0 is the highest-priority DMA channel. When it's running in HBlank
 * repeat mode, it fires 160 times per frame. If not stopped, it will
 * interfere with other DMA operations and waste bus bandwidth.
 */
void ScanlineEffect_Stop(void)
{
    gScanlineEffect.state = 0;
    DmaStop(0);
    if (gScanlineEffect.waveTaskId != 0xFF)
    {
        DestroyTask(gScanlineEffect.waveTaskId);
        gScanlineEffect.waveTaskId = 0xFF;
    }
}

/**
 * FUNCTION: ScanlineEffect_Clear
 *
 * PURPOSE: Zero out all scanline effect state and buffers.
 *
 * HOW IT WORKS:
 * Uses CpuFill16 to zero both scanline buffers (7680 bytes total),
 * then clears all fields of the ScanlineEffect struct.
 * waveTaskId is set to 0xFF (invalid task ID = no wave task).
 */
void ScanlineEffect_Clear(void)
{
    CpuFill16(0, gScanlineEffectRegBuffers, sizeof(gScanlineEffectRegBuffers));
    gScanlineEffect.dmaSrcBuffers[0] = NULL;
    gScanlineEffect.dmaSrcBuffers[1] = NULL;
    gScanlineEffect.dmaDest = NULL;
    gScanlineEffect.dmaControl = 0;
    gScanlineEffect.srcBuffer = 0;
    gScanlineEffect.state = 0;
    gScanlineEffect.unused16 = 0;
    gScanlineEffect.unused17 = 0;
    gScanlineEffect.waveTaskId = 0xFF;
}

/**
 * FUNCTION: ScanlineEffect_SetParams
 *
 * PURPOSE: Configure the scanline effect's DMA parameters.
 *
 * HOW IT WORKS:
 * Sets up the DMA source buffers, destination register, and control flags.
 * The source buffers point to entry [1] (not [0]) because the first HBlank
 * DMA fires AFTER scanline 0, so DMA starts transferring values for scanline 1.
 * Scanline 0's value is set manually via setFirstScanlineReg callback.
 *
 * Two transfer modes are supported:
 *   16-bit: For most GPU registers (HOFS, VOFS, etc.) which are 16-bit
 *   32-bit: For registers that need 32-bit writes (BG affine parameters)
 *
 * @param params — Structure containing dmaDest, dmaControl, initState, unused9
 */
void ScanlineEffect_SetParams(struct ScanlineEffectParams params)
{
    if (params.dmaControl == SCANLINE_EFFECT_DMACNT_16BIT)  // 16-bit
    {
        // Set the DMA src to the value for the second scanline because the
        // first DMA transfer occurs in HBlank *after* the first scanline is drawn
        gScanlineEffect.dmaSrcBuffers[0] = (u16 *)gScanlineEffectRegBuffers[0] + 1;
        gScanlineEffect.dmaSrcBuffers[1] = (u16 *)gScanlineEffectRegBuffers[1] + 1;
        gScanlineEffect.setFirstScanlineReg = CopyValue16Bit;
    }
    else  // assume 32-bit
    {
        // Set the DMA src to the value for the second scanline because the
        // first DMA transfer occurs in HBlank *after* the first scanline is drawn
        gScanlineEffect.dmaSrcBuffers[0] = (u32 *)gScanlineEffectRegBuffers[0] + 1;
        gScanlineEffect.dmaSrcBuffers[1] = (u32 *)gScanlineEffectRegBuffers[1] + 1;
        gScanlineEffect.setFirstScanlineReg = CopyValue32Bit;
    }

    gScanlineEffect.dmaControl = params.dmaControl;
    gScanlineEffect.dmaDest    = params.dmaDest;
    gScanlineEffect.state      = params.initState;
    gScanlineEffect.unused16   = params.unused9;
    gScanlineEffect.unused17   = params.unused9;
}

/**
 * FUNCTION: ScanlineEffect_InitHBlankDmaTransfer
 *
 * PURPOSE: Start DMA0 for the current frame's scanline effect.
 *
 * HOW IT WORKS:
 * Called from the VBlank callback at the start of each frame.
 * 1. If state == 0: Effect is inactive, do nothing.
 * 2. If state == 3: Effect is being stopped. Stop DMA0 and signal the
 *    wave task to destroy itself.
 * 3. Otherwise: Set up DMA0 for HBlank-triggered transfers:
 *    a. Stop any previous DMA0 transfer.
 *    b. Configure DMA0 with source=buffer[srcBuffer], dest=GPU register,
 *       control=HBlank repeat mode.
 *    c. Manually write the first scanline's value (since DMA won't fire
 *       until AFTER scanline 0 has been drawn).
 *    d. Swap the source buffer index (0->1 or 1->0) for double buffering.
 *
 * GBA CONTEXT:
 * DMA_START_HBLANK mode causes DMA0 to automatically transfer one unit
 * of data after each scanline's visible pixels are drawn. Combined with
 * DMA_REPEAT, this repeats for all 160 visible scanlines.
 * The result is a different register value on every scanline.
 */
void ScanlineEffect_InitHBlankDmaTransfer(void)
{
    if (gScanlineEffect.state == 0)
    {
        return;
    }
    else if (gScanlineEffect.state == 3)
    {
        gScanlineEffect.state = 0;
        DmaStop(0);
        sShouldStopWaveTask = TRUE;
    }
    else
    {
        DmaStop(0);
        // Set DMA to copy to dest register on each HBlank for the next frame.
        // The HBlank DMA transfers do not occurr during VBlank, so the transfer
        // will begin on the HBlank after the first scanline
        DmaSet(0, gScanlineEffect.dmaSrcBuffers[gScanlineEffect.srcBuffer], gScanlineEffect.dmaDest, gScanlineEffect.dmaControl);
        // Manually set the reg for the first scanline
        gScanlineEffect.setFirstScanlineReg();
        // Swap current buffer
        gScanlineEffect.srcBuffer ^= 1;
    }
}

/**
 * FUNCTION: CopyValue16Bit / CopyValue32Bit
 *
 * PURPOSE: Manually write the scanline effect value for scanline 0.
 *
 * HOW IT WORKS:
 * DMA0 in HBlank mode fires AFTER a scanline is drawn. So for scanline 0,
 * the DMA hasn't fired yet. These functions manually copy entry [0] from
 * the current source buffer to the destination register, ensuring scanline 0
 * gets the correct value.
 *
 * Volatile pointers are used because we're writing to hardware GPU registers.
 */
static void CopyValue16Bit(void)
{
    vu16 *dest = (vu16 *)gScanlineEffect.dmaDest;
    vu16 *src = (vu16 *)&gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer];

    *dest = *src;
}

static void CopyValue32Bit(void)
{
    vu32 *dest = (vu32 *)gScanlineEffect.dmaDest;
    vu32 *src = (vu32 *)&gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer];

    *dest = *src;
}

#define tStartLine            data[0]
#define tEndLine              data[1]
#define tWaveLength           data[2]
#define tSrcBufferOffset      data[3]
#define tFramesUntilMove      data[4]
#define tDelayInterval        data[5]
#define tRegOffset            data[6]
#define tApplyBattleBgOffsets data[7]

static void TaskFunc_UpdateWavePerFrame(u8 taskId)
{
    int value = 0;
    int i;
    int offset;

    if (sShouldStopWaveTask)
    {
        DestroyTask(taskId);
        gScanlineEffect.waveTaskId = 0xFF;
    }
    else
    {
        if (gTasks[taskId].tApplyBattleBgOffsets)
        {
            switch (gTasks[taskId].tRegOffset)
            {
            case SCANLINE_EFFECT_REG_BG0HOFS:
                value = gBattle_BG0_X;
                break;
            case SCANLINE_EFFECT_REG_BG0VOFS:
                value = gBattle_BG0_Y;
                break;
            case SCANLINE_EFFECT_REG_BG1HOFS:
                value = gBattle_BG1_X;
                break;
            case SCANLINE_EFFECT_REG_BG1VOFS:
                value = gBattle_BG1_Y;
                break;
            case SCANLINE_EFFECT_REG_BG2HOFS:
                value = gBattle_BG2_X;
                break;
            case SCANLINE_EFFECT_REG_BG2VOFS:
                value = gBattle_BG2_Y;
                break;
            case SCANLINE_EFFECT_REG_BG3HOFS:
                value = gBattle_BG3_X;
                break;
            case SCANLINE_EFFECT_REG_BG3VOFS:
                value = gBattle_BG3_Y;
                break;
            }
        }
        if (gTasks[taskId].tFramesUntilMove != 0)
        {
            gTasks[taskId].tFramesUntilMove--;
            offset = gTasks[taskId].tSrcBufferOffset + 320;
            for (i = gTasks[taskId].tStartLine; i < gTasks[taskId].tEndLine; i++)
            {
                gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gScanlineEffectRegBuffers[0][offset] + value;
                offset++;
            }
        }
        else
        {
            gTasks[taskId].tFramesUntilMove = gTasks[taskId].tDelayInterval;
            offset = gTasks[taskId].tSrcBufferOffset + 320;
            for (i = gTasks[taskId].tStartLine; i < gTasks[taskId].tEndLine; i++)
            {
                gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gScanlineEffectRegBuffers[0][offset] + value;
                offset++;
            }

            // increment src buffer offset
            gTasks[taskId].tSrcBufferOffset++;
            if (gTasks[taskId].tSrcBufferOffset == gTasks[taskId].tWaveLength)
                gTasks[taskId].tSrcBufferOffset = 0;
        }
    }
}

/**
 * FUNCTION: GenerateWave
 *
 * PURPOSE: Pre-compute a sine wave lookup table for the wave effect.
 *
 * HOW IT WORKS:
 * Fills a 256-entry buffer with sine values scaled by the given amplitude.
 * Uses gSineTable (a precomputed 256-entry sine table from trig.c).
 * The frequency parameter controls how fast the angle advances per entry.
 * Higher frequency = more wave cycles across the scanline range.
 *
 * @param buffer    — Output buffer for wave values
 * @param frequency — How fast to advance through the sine table (higher = more cycles)
 * @param amplitude — Maximum displacement in pixels
 * @param unused    — Not used
 */
static void GenerateWave(u16 *buffer, u8 frequency, u8 amplitude, u8 unused)
{
    u16 i = 0;
    u8 theta = 0;

    while (i < 256)
    {
        buffer[i] = (gSineTable[theta] * amplitude) / 256;
        theta += frequency;
        i++;
    }
}

// Initializes a background "wave" effect that affects scanlines startLine (inclusive) to endLine (exclusive).
// 'frequency' and 'amplitude' control the frequency and amplitude of the wave.
// 'delayInterval' controls how fast the wave travels up the screen. The wave will shift upwards one scanline every 'delayInterval'+1 frames.
// 'regOffset' is the offset of the video register to modify.
u8 ScanlineEffect_InitWave(u8 startLine, u8 endLine, u8 frequency, u8 amplitude, u8 delayInterval, u8 regOffset, bool8 applyBattleBgOffsets)
{
    int i;
    int offset;
    struct ScanlineEffectParams params;
    u8 taskId;

    ScanlineEffect_Clear();

    params.dmaDest = (void *)(REG_ADDR_BG0HOFS + regOffset);
    params.dmaControl = SCANLINE_EFFECT_DMACNT_16BIT;
    params.initState = 1;
    params.unused9 = 0;
    ScanlineEffect_SetParams(params);

    taskId = CreateTask(TaskFunc_UpdateWavePerFrame, 0);

    gTasks[taskId].tStartLine            = startLine;
    gTasks[taskId].tEndLine              = endLine;
    gTasks[taskId].tWaveLength           = 256 / frequency;
    gTasks[taskId].tSrcBufferOffset      = 0;
    gTasks[taskId].tFramesUntilMove      = delayInterval;
    gTasks[taskId].tDelayInterval        = delayInterval;
    gTasks[taskId].tRegOffset            = regOffset;
    gTasks[taskId].tApplyBattleBgOffsets = applyBattleBgOffsets;

    gScanlineEffect.waveTaskId = taskId;
    sShouldStopWaveTask = FALSE;

    GenerateWave(&gScanlineEffectRegBuffers[0][320], frequency, amplitude, endLine - startLine);

    offset = 320;
    for (i = startLine; i < endLine; i++)
    {
        gScanlineEffectRegBuffers[0][i] = gScanlineEffectRegBuffers[0][offset];
        gScanlineEffectRegBuffers[1][i] = gScanlineEffectRegBuffers[0][offset];
        offset++;
    }

    return taskId;
}
