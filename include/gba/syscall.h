/*
 * syscall.h - GBA BIOS System Call Declarations
 *
 * ============================================================================
 * BIOS SYSTEM CALLS (SWI - Software Interrupt)
 * ============================================================================
 *
 * The GBA BIOS (Basic Input/Output System) is a 16 KB ROM at address
 * 0x00000000 that contains about 40 utility functions. These functions
 * are called using the ARM "SWI" (Software Interrupt) instruction.
 *
 * When a SWI instruction executes:
 * 1. The CPU switches to Supervisor mode (privileged)
 * 2. The BIOS reads the SWI number from the instruction
 * 3. Jumps to the corresponding BIOS function
 * 4. The function executes and returns to the caller
 *
 * The actual SWI assembly is in separate .s (assembly) files.
 * These C declarations let you call them from C code like normal functions.
 *
 * IMPORTANT: The BIOS ROM is READ-PROTECTED. You can execute BIOS functions
 * via SWI, but you CANNOT read the BIOS ROM directly from game code
 * (reads return 0 or the last BIOS instruction fetched). This is a
 * security/copy-protection measure by Nintendo.
 *
 * COMMON BIOS CALLS USED IN THIS GAME:
 *   SoftReset (SWI 0x00):     Warm reboot the GBA
 *   RegisterRamReset (0x01):  Clear specified RAM regions
 *   VBlankIntrWait (0x05):    Halt CPU until VBlank interrupt
 *   Div (0x06):               Signed integer division (ARM has no division!)
 *   Sqrt (0x08):              Integer square root
 *   ArcTan2 (0x0A):           Two-argument arctangent
 *   CpuSet (0x0B):            Memory copy/fill (16 or 32 bit)
 *   CpuFastSet (0x0C):        Fast memory copy/fill (32-bit, block transfer)
 *   BgAffineSet (0x0E):       Calculate BG rotation/scale matrix
 *   ObjAffineSet (0x0F):      Calculate sprite rotation/scale matrix
 *   LZ77UnCompWram (0x11):    Decompress LZ77 data to WRAM
 *   LZ77UnCompVram (0x12):    Decompress LZ77 data to VRAM (16-bit writes)
 *   MultiBoot (0x25):         Transfer program to another GBA via link cable
 *
 * ============================================================================
 */

#ifndef GUARD_GBA_SYSCALL_H
#define GUARD_GBA_SYSCALL_H

/*
 * RegisterRamReset flags - specify which memory regions to zero out.
 * Used with the RegisterRamReset BIOS call at startup.
 * Multiple flags can be combined with OR:
 *   RegisterRamReset(RESET_EWRAM | RESET_PALETTE | RESET_VRAM);
 */
#define RESET_EWRAM      0x01  /* Clear External Work RAM (256 KB at 0x02000000) */
#define RESET_IWRAM      0x02  /* Clear Internal Work RAM (32 KB at 0x03000000)
                                * WARNING: This includes the stack! Don't use
                                * if you have important data on the stack. */
#define RESET_PALETTE    0x04  /* Clear Palette RAM (1 KB at 0x05000000) */
#define RESET_VRAM       0x08  /* Clear Video RAM (96 KB at 0x06000000) */
#define RESET_OAM        0x10  /* Clear OAM (1 KB at 0x07000000) - hides all sprites */
#define RESET_SIO_REGS   0x20  /* Reset Serial I/O registers (link cable state) */
#define RESET_SOUND_REGS 0x40  /* Reset all sound registers (silence audio) */
#define RESET_REGS       0x80  /* Reset all other I/O registers to defaults */
#define RESET_ALL        0xFF  /* Reset EVERYTHING (used at game startup) */

/**
 * FUNCTION: SoftReset
 *
 * PURPOSE: Perform a warm reboot of the GBA (BIOS SWI 0x00).
 *
 * HOW IT WORKS:
 * Resets the CPU state, clears the BIOS work area (0x03007E00-0x03007FFF),
 * and jumps to the ROM entry point (0x08000000). The effect is as if the
 * GBA was just powered on, but faster since it skips the BIOS boot logo.
 *
 * GBA CONTEXT:
 * The resetFlags parameter specifies what to clear before rebooting.
 * In this game, RESET_SIO_REGS is excluded to preserve link cable state.
 *
 * @param resetFlags — Bitmask of RESET_* flags (same as RegisterRamReset)
 */
void SoftReset(u32 resetFlags);

/**
 * FUNCTION: RegisterRamReset
 *
 * PURPOSE: Zero out specified memory regions (BIOS SWI 0x01).
 *
 * HOW IT WORKS:
 * The BIOS loops through each bit in resetFlags. For each set bit,
 * it fills the corresponding memory region with zeros.
 * Called once at game startup to ensure a clean state.
 *
 * @param resetFlags — Bitmask of RESET_* flags specifying what to clear
 */
void RegisterRamReset(u32 resetFlags);

/**
 * FUNCTION: VBlankIntrWait
 *
 * PURPOSE: Halt the CPU until the next VBlank interrupt fires (BIOS SWI 0x05).
 *
 * HOW IT WORKS:
 * Puts the CPU into a low-power halt state. The CPU stops executing
 * instructions until an interrupt wakes it up. After waking, it checks
 * if the VBlank interrupt specifically has fired (via INTR_CHECK at
 * 0x03007FF8). If not, it halts again.
 *
 * GBA CONTEXT:
 * This is more power-efficient than busy-waiting (spinning in a loop).
 * However, this game uses a manual busy-wait loop instead (in WaitForVBlank)
 * because VBlankIntrWait has edge cases with DMA that can cause hangs.
 */
void VBlankIntrWait(void);

/**
 * FUNCTION: Sqrt
 *
 * PURPOSE: Calculate the integer square root (BIOS SWI 0x08).
 *
 * HOW IT WORKS:
 * Returns floor(sqrt(num)). Implemented in the BIOS using an optimized
 * iterative algorithm. Used instead of floating-point sqrt (which the
 * ARM7TDMI would need to emulate in software, taking hundreds of cycles).
 *
 * @param num — The number to take the square root of
 * @return The integer square root (rounded down)
 */
u16 Sqrt(u32 num);

/**
 * FUNCTION: ArcTan2
 *
 * PURPOSE: Calculate the angle from the origin to point (x, y) (BIOS SWI 0x0A).
 *
 * HOW IT WORKS:
 * Returns the angle in the range 0-0xFFFF (mapping to 0-360 degrees).
 * This is the GBA equivalent of the C math library's atan2() function,
 * but returns a fixed-point angle instead of radians.
 *
 * @param x — X coordinate (signed)
 * @param y — Y coordinate (signed)
 * @return Angle in range 0x0000-0xFFFF (0-360 degrees)
 */
u16 ArcTan2(s16 x, s16 y);

/*
 * CpuSet control flags.
 * Used in the 'control' parameter of CpuSet():
 *   Bit 24: Source fixed mode (fill vs copy)
 *   Bit 26: Transfer width (16 or 32 bit)
 *   Bits 0-20: Number of transfer units
 */
#define CPU_SET_SRC_FIXED 0x01000000  /* Fill mode: read same src for every write */
#define CPU_SET_16BIT     0x00000000  /* Transfer 16 bits (2 bytes) per unit */
#define CPU_SET_32BIT     0x04000000  /* Transfer 32 bits (4 bytes) per unit */

/**
 * FUNCTION: CpuSet
 *
 * PURPOSE: Copy or fill memory using optimized BIOS code (SWI 0x0B).
 *
 * HOW IT WORKS:
 * If CPU_SET_SRC_FIXED is set in control: fills dest with the value at src.
 * Otherwise: copies from src to dest.
 * Transfer width is either 16-bit or 32-bit based on the control flag.
 * Bits 0-20 of control specify the number of transfer units.
 *
 * GBA CONTEXT:
 * The ARM7TDMI has no hardware division and the data bus can be 16 or 32 bits
 * depending on the memory region. CpuSet handles these details efficiently.
 *
 * @param src     — Source address
 * @param dest    — Destination address
 * @param control — Transfer count (bits 0-20) | width flag | fill flag
 */
void CpuSet(const void *src, void *dest, u32 control);

/* CpuFastSet control flag */
#define CPU_FAST_SET_SRC_FIXED 0x01000000  /* Fill mode for CpuFastSet */

/**
 * FUNCTION: CpuFastSet
 *
 * PURPOSE: Fast memory copy/fill using block transfers (BIOS SWI 0x0C).
 *
 * HOW IT WORKS:
 * Similar to CpuSet but ALWAYS uses 32-bit transfers and processes
 * 8 words (32 bytes) at a time using LDMIA/STMIA instructions.
 * This makes it significantly faster for large, aligned transfers.
 *
 * REQUIREMENTS:
 * - Both src and dest MUST be 4-byte aligned
 * - Size MUST be a multiple of 32 bytes (8 words)
 * - If not a multiple of 32, the last 0-28 bytes are NOT transferred
 *
 * @param src     — Source address (must be 4-byte aligned)
 * @param dest    — Destination address (must be 4-byte aligned)
 * @param control — Transfer count in words (bits 0-20) | optional fill flag
 */
void CpuFastSet(const void *src, void *dest, u32 control);

/**
 * FUNCTION: BgAffineSet
 *
 * PURPOSE: Calculate background affine transformation matrix (BIOS SWI 0x0E).
 *
 * HOW IT WORKS:
 * Converts human-friendly rotation/scale parameters (angle, scale factor,
 * center point) into the raw 2x2 matrix + displacement that the GPU needs.
 * Can process multiple transformations in one call.
 *
 * GBA CONTEXT:
 * Affine BG layers (Mode 1/2) can be rotated and scaled. The GPU reads
 * PA/PB/PC/PD matrix registers every scanline to determine which source
 * pixel to display. This BIOS call does the trigonometry to compute those
 * matrix values from a rotation angle and scale factors.
 *
 * @param src   — Array of BgAffineSrcData (rotation, scale, center)
 * @param dest  — Array of BgAffineDstData (output matrices)
 * @param count — Number of transformations to calculate
 */
void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count);

/**
 * FUNCTION: ObjAffineSet
 *
 * PURPOSE: Calculate sprite affine transformation matrix (BIOS SWI 0x0F).
 *
 * HOW IT WORKS:
 * Similar to BgAffineSet but for sprites. Calculates the 2x2 matrix
 * (pa, pb, pc, pd) from rotation angle and scale factors.
 *
 * The 'offset' parameter controls the stride between output entries.
 * For OAM: offset=8 because affine parameters are interleaved in OAM
 * (every 4th OAM entry contributes one matrix parameter).
 *
 * @param src    — Array of ObjAffineSrcData (scale + rotation)
 * @param dest   — Output buffer for matrix parameters
 * @param count  — Number of matrices to calculate
 * @param offset — Byte stride between consecutive output parameters
 */
void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset);

/**
 * FUNCTION: LZ77UnCompWram
 *
 * PURPOSE: Decompress LZ77-compressed data to Work RAM (BIOS SWI 0x11).
 *
 * HOW IT WORKS:
 * LZ77 is a compression algorithm that replaces repeated byte sequences
 * with back-references ("copy 12 bytes from 50 bytes ago"). Most graphics
 * data in the ROM is LZ77-compressed to save cartridge space.
 *
 * This version writes 8 bits at a time, so it can write to any RAM address.
 * Use this for decompressing to EWRAM or IWRAM.
 *
 * @param src  — Pointer to LZ77-compressed data (starts with 4-byte header)
 * @param dest — Destination buffer (must be large enough for decompressed data)
 */
void LZ77UnCompWram(const void *src, void *dest);

/**
 * FUNCTION: LZ77UnCompVram
 *
 * PURPOSE: Decompress LZ77-compressed data to VRAM (BIOS SWI 0x12).
 *
 * HOW IT WORKS:
 * Same as LZ77UnCompWram but writes 16 bits at a time. This is REQUIRED
 * for writing to VRAM because VRAM only supports 16-bit and 32-bit writes.
 * 8-bit writes to VRAM are silently ignored (or write to wrong locations),
 * which is a common source of bugs for GBA beginners.
 *
 * @param src  — Pointer to LZ77-compressed data
 * @param dest — Destination in VRAM (must be 2-byte aligned)
 */
void LZ77UnCompVram(const void *src, void *dest);

/**
 * FUNCTION: RLUnCompWram
 *
 * PURPOSE: Decompress Run-Length Encoded data to Work RAM (BIOS SWI 0x14).
 *
 * HOW IT WORKS:
 * RLE compression stores repeated bytes as "repeat N times" + byte value.
 * Simpler than LZ77 but less effective for complex data.
 *
 * @param src  — Pointer to RLE-compressed data
 * @param dest — Destination buffer
 */
void RLUnCompWram(const void *src, void *dest);

/**
 * FUNCTION: RLUnCompVram
 *
 * PURPOSE: Decompress Run-Length Encoded data to VRAM (BIOS SWI 0x15).
 *
 * HOW IT WORKS:
 * Same as RLUnCompWram but uses 16-bit writes (required for VRAM access).
 *
 * @param src  — Pointer to RLE-compressed data
 * @param dest — Destination in VRAM (must be 2-byte aligned)
 */
void RLUnCompVram(const void *src, void *dest);

/**
 * FUNCTION: MultiBoot
 *
 * PURPOSE: Transfer a program to another GBA via link cable (BIOS SWI 0x25).
 *
 * HOW IT WORKS:
 * Sends up to 256 KB of program data to connected GBAs that are in
 * "multiboot" mode (no cartridge inserted, or holding Start during boot).
 * Used for multiplayer games where only one player has the cartridge.
 *
 * @param mp — MultiBoot parameter structure (addresses, sizes, mode)
 * @return 0 on success, non-zero on error
 */
int MultiBoot(struct MultiBootParam *mp);

/**
 * FUNCTION: Div
 *
 * PURPOSE: Signed integer division (BIOS SWI 0x06).
 *
 * HOW IT WORKS:
 * Returns num / denom. The ARM7TDMI CPU has NO hardware division instruction!
 * Division must be done in software. The BIOS provides this optimized
 * implementation. Without it, a C compiler would generate a much slower
 * software division routine.
 *
 * GBA CONTEXT:
 * Modern CPUs have division built into hardware (1-40 cycles).
 * The ARM7TDMI lacks this, so division is a BIOS call taking ~50+ cycles.
 * GBA games minimize division by using bit shifts (divide by powers of 2),
 * multiplication by reciprocals, or lookup tables where possible.
 *
 * @param num   — Numerator (dividend)
 * @param denom — Denominator (divisor, must not be 0)
 * @return num / denom (integer division, truncated toward zero)
 */
s32 Div(s32 num, s32 denom);

#endif // GUARD_GBA_SYSCALL_H
