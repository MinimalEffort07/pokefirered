/**
 * @file fldeff_poison.c
 * @brief Overworld Poison Visual Effect — Screen Mosaic Pulse
 *
 * FILE OVERVIEW:
 * When a poisoned Pokemon takes damage from walking on the overworld (see
 * field_poison.c), this file creates the visual feedback — a brief "mosaic"
 * pulse effect on the screen. The screen momentarily becomes pixelated and
 * then returns to normal, visually indicating that something bad happened.
 *
 * GBA CONTEXT — MOSAIC EFFECT:
 * The GBA hardware has a built-in mosaic effect (REG_MOSAIC at 0x0400004C)
 * that pixelates background layers by grouping NxN pixel blocks into single
 * colors. The mosaic size can be set independently for X and Y.
 *
 * The mosaic register format: bits 0-3 = BG horizontal size, bits 4-7 = BG
 * vertical size. A value of 0 means no mosaic (normal), while higher values
 * mean larger pixel blocks (more pixelated). Maximum is 15 (16x16 blocks).
 *
 * This effect ramps the mosaic from 0 to 4 and back to 0, creating a brief
 * "pulse" of pixelation that lasts about 10 frames total.
 */
#include "global.h"
#include "gflib.h"
#include "task.h"
#include "constants/songs.h"

/**
 * FUNCTION: Task_FieldPoisonEffect
 *
 * PURPOSE: Animates the mosaic pulse effect: increases mosaic level from 0 to 4,
 * then decreases it back to 0, then destroys itself.
 *
 * HOW IT WORKS:
 * State 0: Increment mosaic level each frame until it reaches 5 (increasing pixelation)
 * State 1: Decrement mosaic level each frame until it reaches 0 (clearing pixelation)
 * State 2: Cleanup — destroy the task
 *
 * The mosaic value is packed as (level << 4 | level) to set both horizontal and
 * vertical mosaic to the same size. BG_MOSAIC_SET directly writes to the mosaic
 * register, applying the effect to background layers.
 *
 * @param taskId — task identifier
 */
static void Task_FieldPoisonEffect(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (data[0])
    {
    case 0:
        /* Phase 1: Increase mosaic (more pixelated) */
        data[1] += 1;
        if (data[1] > 4)
            data[0]++;
        break;
    case 1:
        /* Phase 2: Decrease mosaic (less pixelated, returning to normal) */
        data[1] -= 1;
        if (data[1] == 0)
            data[0]++;
        break;
    case 2:
        /* Done — remove the task */
        DestroyTask(taskId);
        return;
    }
    /*
     * Apply the mosaic effect. The value (data[1] << 4 | data[1]) sets both
     * the horizontal and vertical mosaic sizes to the same level.
     * For example, data[1]=3 -> 0x33 -> 3x3 pixel blocks in both directions.
     */
    AdjustBgMosaic((u8)(((u8)data[1] << 4) | (u8)data[1]), BG_MOSAIC_SET);
}

/**
 * FUNCTION: FldEffPoison_Start
 *
 * PURPOSE: Starts the poison visual effect — plays the poison damage sound
 * and creates the mosaic pulse task.
 *
 * GAME LOGIC:
 * Called by the field poison system (field_poison.c) every time a poisoned
 * Pokemon takes a step's worth of damage on the overworld.
 */
void FldEffPoison_Start(void)
{
    PlaySE(SE_FIELD_POISON);
    CreateTask(Task_FieldPoisonEffect, 80);
}

/**
 * FUNCTION: FldEffPoison_IsActive
 *
 * PURPOSE: Returns TRUE if the poison mosaic effect is currently playing.
 * Used by the field poison system to wait for the effect to finish before
 * continuing to process the next poison step.
 *
 * @return TRUE if the effect is still active, FALSE when complete
 */
bool32 FldEffPoison_IsActive(void)
{
    return FuncIsActiveTask(Task_FieldPoisonEffect);
}
