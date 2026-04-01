/*
 * bg_regs.c - Background Register Lookup Tables
 *
 * ============================================================================
 * PURPOSE
 * ============================================================================
 *
 * This file provides arrays that map background layer numbers (0-3) to
 * their corresponding hardware register addresses and offsets.
 *
 * Instead of writing switch statements or if-else chains every time code
 * needs to access a BG register, the code uses these arrays:
 *
 *   // Without lookup tables (repetitive, error-prone):
 *   if (bg == 0) REG_BG0CNT = value;
 *   else if (bg == 1) REG_BG1CNT = value;
 *   else if (bg == 2) REG_BG2CNT = value;
 *   else REG_BG3CNT = value;
 *
 *   // With lookup tables (clean, generic):
 *   *gBGControlRegs[bg] = value;
 *
 * This pattern is common in GBA development because the hardware has
 * many sets of numbered registers (4 BG layers, 4 DMA channels,
 * 4 timers) that are accessed by index.
 *
 * ============================================================================
 * GBA CONTEXT
 * ============================================================================
 *
 * Each BG layer has its own set of hardware registers:
 *   BG Control (BGxCNT):    Priority, tile/map base, color mode, size
 *   BG H Offset (BGxHOFS):  Horizontal scroll position (write-only)
 *   BG V Offset (BGxVOFS):  Vertical scroll position (write-only)
 *
 * These arrays provide both:
 *   - Direct register pointers (vu16*): For code that writes to hardware
 *     registers directly (rare, usually during init or special effects).
 *   - Register offsets (u8): For code that uses the GPU register buffer
 *     system (SetGpuReg/GetGpuReg in gpu_regs.c), which is the normal
 *     way to write to GPU registers in this codebase.
 *
 * The DISPCNT and BLDCNT flag arrays map BG numbers to the bit flags
 * that enable/configure those BGs in the master display register.
 *
 * ============================================================================
 */

#include "global.h"

/*
 * Direct hardware register pointers for BG control registers.
 * Volatile (vu16*) because they're memory-mapped I/O.
 * Used when code needs to write directly to hardware (bypassing the buffer).
 */
vu16 *const gBGControlRegs[] =
{
    &REG_BG0CNT,  /* 0x04000008 - BG0 control */
    &REG_BG1CNT,  /* 0x0400000A - BG1 control */
    &REG_BG2CNT,  /* 0x0400000C - BG2 control */
    &REG_BG3CNT,  /* 0x0400000E - BG3 control */
};

/*
 * Direct hardware register pointers for BG horizontal scroll.
 * Write-only on hardware - reading these addresses returns undefined values.
 */
vu16 *const gBGHOffsetRegs[] =
{
    &REG_BG0HOFS,  /* 0x04000010 */
    &REG_BG1HOFS,  /* 0x04000014 */
    &REG_BG2HOFS,  /* 0x04000018 */
    &REG_BG3HOFS,  /* 0x0400001C */
};

/*
 * Direct hardware register pointers for BG vertical scroll.
 */
vu16 *const gBGVOffsetRegs[] =
{
    &REG_BG0VOFS,  /* 0x04000012 */
    &REG_BG1VOFS,  /* 0x04000016 */
    &REG_BG2VOFS,  /* 0x0400001A */
    &REG_BG3VOFS,  /* 0x0400001E */
};

/*
 * DISPCNT flags to enable each BG layer.
 * OR these with REG_DISPCNT to make a BG layer visible.
 * Example: REG_DISPCNT |= gDISPCNTBGFlags[2]; // Enable BG2
 */
const u16 gDISPCNTBGFlags[] = { DISPCNT_BG0_ON, DISPCNT_BG1_ON, DISPCNT_BG2_ON, DISPCNT_BG3_ON };

/*
 * BLDCNT Target 2 flags for each BG layer.
 * Used when setting up alpha blending between the overworld BG layers.
 * "Target 2" is the layer that shows through behind the semi-transparent
 * "Target 1" layer.
 */
const u16 gOverworldBackgroundLayerFlags[] = { BLDCNT_TGT2_BG0, BLDCNT_TGT2_BG1, BLDCNT_TGT2_BG2, BLDCNT_TGT2_BG3 };

/*
 * BLDCNT Target 1 flags for each BG layer.
 * "Target 1" is the semi-transparent top layer in alpha blending.
 */
const u16 gBLDCNTTarget1BGFlags[] = { BLDCNT_TGT1_BG0, BLDCNT_TGT1_BG1, BLDCNT_TGT1_BG2, BLDCNT_TGT1_BG3 };

/*
 * Register OFFSETS for BG control registers (used with the GPU register buffer).
 * These are passed to SetGpuReg()/GetGpuReg() in gpu_regs.c.
 * Example: SetGpuReg(gBGControlRegOffsets[bgId], controlValue);
 */
const u8 gBGControlRegOffsets[] =
{
    REG_OFFSET_BG0CNT,  /* 0x08 */
    REG_OFFSET_BG1CNT,  /* 0x0A */
    REG_OFFSET_BG2CNT,  /* 0x0C */
    REG_OFFSET_BG3CNT,  /* 0x0E */
};

const u8 gBGHOffsetRegOffsets[] =
{
    REG_OFFSET_BG0HOFS,  /* 0x10 */
    REG_OFFSET_BG1HOFS,  /* 0x14 */
    REG_OFFSET_BG2HOFS,  /* 0x18 */
    REG_OFFSET_BG3HOFS,  /* 0x1C */
};

const u8 gBGVOffsetRegOffsets[] =
{
    REG_OFFSET_BG0VOFS,  /* 0x12 */
    REG_OFFSET_BG1VOFS,  /* 0x16 */
    REG_OFFSET_BG2VOFS,  /* 0x1A */
    REG_OFFSET_BG3VOFS,  /* 0x1E */
};
