/*
 * io_reg.h - GBA I/O Register Definitions
 *
 * ============================================================================
 * HOW GBA HARDWARE REGISTERS WORK
 * ============================================================================
 *
 * The GBA uses "memory-mapped I/O" (MMIO). This means hardware is controlled
 * by reading and writing to specific memory addresses, just like regular
 * variables. There are no "system calls" or "driver APIs" - you literally
 * write a number to a memory address and the hardware responds.
 *
 * All I/O registers live in the range 0x04000000 - 0x040003FF (1 KB).
 *
 * This file defines THREE things for each register:
 *
 * 1. OFFSET: The register's position relative to 0x04000000.
 *    Example: REG_OFFSET_DISPCNT = 0x0 (first register, at base + 0)
 *
 * 2. ADDRESS: The full memory address (REG_BASE + offset).
 *    Example: REG_ADDR_DISPCNT = 0x04000000
 *
 * 3. ACCESSOR: A C expression that lets you read/write the register
 *    like a regular variable, using a volatile pointer cast.
 *    Example: REG_DISPCNT = *(volatile u16 *)0x04000000
 *    Usage:   REG_DISPCNT = 0x0403;  // Write to display control
 *             u16 x = REG_VCOUNT;    // Read current scanline
 *
 * WHY THREE DEFINITIONS?
 *   - Offsets are used by the GPU register buffer system (gpu_regs.c)
 *     which stores register values in an array indexed by offset.
 *   - Addresses are used by DMA macros that need raw numeric addresses.
 *   - Accessors are used everywhere else for convenient read/write.
 *
 * IMPORTANT: Accessors use VOLATILE pointers (vu16*, vu32*) because
 * hardware registers can change at any time (the GPU updates VCOUNT
 * every scanline, button state changes when you press buttons, etc.)
 * Without volatile, the compiler might cache old values or skip writes.
 *
 * ============================================================================
 * REGISTER CATEGORIES
 * ============================================================================
 *
 * The registers are grouped by subsystem:
 *
 *   0x000-0x056: Display/Graphics (DISPCNT, DISPSTAT, BG control, blend, window)
 *   0x060-0x0A4: Sound (4 tone channels + 2 DMA channels)
 *   0x0B0-0x0DE: DMA (4 DMA channels, each with src/dest/control)
 *   0x100-0x10E: Timers (4 hardware timers)
 *   0x120-0x12A: Serial I/O (link cable communication)
 *   0x130-0x132: Keypad input (button states)
 *   0x134:       General I/O port control
 *   0x200-0x208: Interrupt control (IE, IF, IME)
 *   0x204:       Wait state control (ROM access timing)
 *
 * ============================================================================
 */

#ifndef GUARD_GBA_IO_REG_H
#define GUARD_GBA_IO_REG_H

/*
 * REG_BASE: The starting address of ALL I/O registers.
 * Every register address = REG_BASE + its offset.
 */
#define REG_BASE 0x4000000

/*
 * ============================================================================
 * SECTION 1: REGISTER OFFSETS (distance from REG_BASE)
 * ============================================================================
 */

/*
 * --- DISPLAY CONTROL REGISTERS (0x000-0x006) ---
 *
 * These control the overall display mode and provide status information.
 */
#define REG_OFFSET_DISPCNT     0x0   /* Display Control: master GPU config (mode, layers, etc.) */
#define REG_OFFSET_DISPSTAT    0x4   /* Display Status: VBlank/HBlank flags + interrupt enables */
#define REG_OFFSET_VCOUNT      0x6   /* Vertical Count: current scanline being drawn (0-227) */

/*
 * --- BACKGROUND CONTROL REGISTERS (0x008-0x01E) ---
 *
 * Each BG layer has a control register (priority, tile/map base, size)
 * and scroll registers (which part of the map to display).
 * HOFS = Horizontal Offset (how far scrolled right)
 * VOFS = Vertical Offset (how far scrolled down)
 * Scroll registers are WRITE-ONLY on the GBA (you cannot read them back).
 */
#define REG_OFFSET_BG0CNT      0x8   /* BG0 Control: priority, char/screen base, size */
#define REG_OFFSET_BG1CNT      0xa   /* BG1 Control */
#define REG_OFFSET_BG2CNT      0xc   /* BG2 Control */
#define REG_OFFSET_BG3CNT      0xe   /* BG3 Control */
#define REG_OFFSET_BG0HOFS     0x10  /* BG0 Horizontal scroll (write-only) */
#define REG_OFFSET_BG0VOFS     0x12  /* BG0 Vertical scroll (write-only) */
#define REG_OFFSET_BG1HOFS     0x14  /* BG1 Horizontal scroll */
#define REG_OFFSET_BG1VOFS     0x16  /* BG1 Vertical scroll */
#define REG_OFFSET_BG2HOFS     0x18  /* BG2 Horizontal scroll */
#define REG_OFFSET_BG2VOFS     0x1a  /* BG2 Vertical scroll */
#define REG_OFFSET_BG3HOFS     0x1c  /* BG3 Horizontal scroll */
#define REG_OFFSET_BG3VOFS     0x1e  /* BG3 Vertical scroll */
/*
 * --- AFFINE BACKGROUND PARAMETERS (0x020-0x03E) ---
 *
 * BG2 and BG3 support "affine" mode (rotation and scaling).
 * PA/PB/PC/PD form a 2x2 transformation matrix:
 *   | PA  PB |   Transforms source texture coordinates to screen coordinates.
 *   | PC  PD |   Identity (no transform): PA=PD=0x0100 (1.0 in 8.8 fixed-point)
 *
 * X/Y are the reference point (displacement). 32-bit values split into
 * _L (low 16 bits) and _H (high 16 bits) because the GBA bus is 16-bit.
 *
 * These are WRITE-ONLY registers.
 */
#define REG_OFFSET_BG2PA       0x20  /* BG2 Affine matrix PA (dx) */
#define REG_OFFSET_BG2PB       0x22  /* BG2 Affine matrix PB (dmx) */
#define REG_OFFSET_BG2PC       0x24  /* BG2 Affine matrix PC (dy) */
#define REG_OFFSET_BG2PD       0x26  /* BG2 Affine matrix PD (dmy) */
#define REG_OFFSET_BG2X        0x28  /* BG2 Reference point X (28-bit, 19.8 fixed) */
#define REG_OFFSET_BG2X_L      0x28  /* BG2 Ref X low 16 bits */
#define REG_OFFSET_BG2X_H      0x2a  /* BG2 Ref X high 12 bits */
#define REG_OFFSET_BG2Y        0x2c  /* BG2 Reference point Y (28-bit, 19.8 fixed) */
#define REG_OFFSET_BG2Y_L      0x2c
#define REG_OFFSET_BG2Y_H      0x2e
#define REG_OFFSET_BG3PA       0x30  /* BG3 Affine matrix PA */
#define REG_OFFSET_BG3PB       0x32  /* BG3 Affine matrix PB */
#define REG_OFFSET_BG3PC       0x34  /* BG3 Affine matrix PC */
#define REG_OFFSET_BG3PD       0x36  /* BG3 Affine matrix PD */
#define REG_OFFSET_BG3X        0x38  /* BG3 Reference point X */
#define REG_OFFSET_BG3X_L      0x38
#define REG_OFFSET_BG3X_H      0x3a
#define REG_OFFSET_BG3Y        0x3c  /* BG3 Reference point Y */
#define REG_OFFSET_BG3Y_L      0x3c
#define REG_OFFSET_BG3Y_H      0x3e

/*
 * --- WINDOW CONTROL REGISTERS (0x040-0x04A) ---
 *
 * Windows are rectangular regions on screen that control which layers
 * are visible inside/outside them. Used for:
 *   - Text boxes (show text BG inside window, hide outside)
 *   - Spotlight effects (show BG only in a circle using OBJ window)
 *   - HUD elements (keep a score display visible while BG scrolls)
 *
 * WIN0H/WIN1H: Horizontal bounds (left edge in bits 8-15, right in bits 0-7)
 * WIN0V/WIN1V: Vertical bounds (top edge in bits 8-15, bottom in bits 0-7)
 * WININ:  Which layers are visible INSIDE the windows
 * WINOUT: Which layers are visible OUTSIDE the windows
 */
#define REG_OFFSET_WIN0H       0x40  /* Window 0 horizontal range (write-only) */
#define REG_OFFSET_WIN1H       0x42  /* Window 1 horizontal range (write-only) */
#define REG_OFFSET_WIN0V       0x44  /* Window 0 vertical range (write-only) */
#define REG_OFFSET_WIN1V       0x46  /* Window 1 vertical range (write-only) */
#define REG_OFFSET_WININ       0x48  /* Layers visible inside windows 0 and 1 */
#define REG_OFFSET_WINOUT      0x4a  /* Layers visible outside windows + OBJ window */

/* Mosaic: applies a pixelation effect by grouping pixels into blocks */
#define REG_OFFSET_MOSAIC      0x4c  /* Mosaic size control (write-only) */

/*
 * --- BLEND/ALPHA CONTROL REGISTERS (0x050-0x054) ---
 *
 * These control color blending effects between layers:
 *   BLDCNT:   Select which layers to blend and the blend mode
 *             (alpha blend, brighten, darken, or none)
 *   BLDALPHA: Alpha blend coefficients (EVA for 1st target, EVB for 2nd)
 *   BLDY:     Brightness change coefficient (for brighten/darken modes)
 *
 * Used for: screen fades (to black/white), semi-transparent menus,
 * water reflections, day/night effects.
 */
#define REG_OFFSET_BLDCNT      0x50  /* Blend mode and target layer selection */
#define REG_OFFSET_BLDALPHA    0x52  /* Alpha blend coefficients */
#define REG_OFFSET_BLDY        0x54  /* Brightness coefficient (write-only) */

/*
 * --- SOUND REGISTERS (0x060-0x0A4) ---
 *
 * The GBA has 6 sound channels:
 *
 * Channels 1-4 are "CGB" channels (inherited from Game Boy Color):
 *   Channel 1 (SOUND1): Square wave with frequency sweep
 *   Channel 2 (SOUND2): Square wave (no sweep)
 *   Channel 3 (SOUND3): Programmable wave (user-defined waveform in WAVE_RAM)
 *   Channel 4 (SOUND4): Noise generator (for drums, explosions, etc.)
 *
 * Channels A-B are "Direct Sound" channels (GBA-exclusive):
 *   Channel A (FIFO_A): 8-bit PCM samples fed by DMA from RAM
 *   Channel B (FIFO_B): 8-bit PCM samples fed by DMA from RAM
 *   These are what the m4a sound engine uses for music and SFX.
 *   DMA channels 1 and 2 automatically refill the FIFOs.
 *
 * NRxx names are Game Boy legacy names (NR = Noise Register).
 * SOUNDxCNT names are the official GBA names.
 * Both refer to the same registers - this file defines both for compatibility.
 *
 * SOUNDCNT_L: Master volume for CGB channels
 * SOUNDCNT_H: Master volume for Direct Sound + DMA/timer config
 * SOUNDCNT_X: Master enable + channel status flags
 * SOUNDBIAS: Output bias level (affects audio quality/sample rate)
 * WAVE_RAM: 16 bytes of waveform data for Channel 3
 */
#define REG_OFFSET_SOUND1CNT_L 0x60
#define REG_OFFSET_NR10        0x60
#define REG_OFFSET_SOUND1CNT_H 0x62
#define REG_OFFSET_NR11        0x62
#define REG_OFFSET_NR12        0x63
#define REG_OFFSET_SOUND1CNT_X 0x64
#define REG_OFFSET_NR13        0x64
#define REG_OFFSET_NR14        0x65
#define REG_OFFSET_SOUND2CNT_L 0x68
#define REG_OFFSET_NR21        0x68
#define REG_OFFSET_NR22        0x69
#define REG_OFFSET_SOUND2CNT_H 0x6c
#define REG_OFFSET_NR23        0x6c
#define REG_OFFSET_NR24        0x6d
#define REG_OFFSET_SOUND3CNT_L 0x70
#define REG_OFFSET_NR30        0x70
#define REG_OFFSET_SOUND3CNT_H 0x72
#define REG_OFFSET_NR31        0x72
#define REG_OFFSET_NR32        0x73
#define REG_OFFSET_SOUND3CNT_X 0x74
#define REG_OFFSET_NR33        0x74
#define REG_OFFSET_NR34        0x75
#define REG_OFFSET_SOUND4CNT_L 0x78
#define REG_OFFSET_NR41        0x78
#define REG_OFFSET_NR42        0x79
#define REG_OFFSET_SOUND4CNT_H 0x7c
#define REG_OFFSET_NR43        0x7c
#define REG_OFFSET_NR44        0x7d
#define REG_OFFSET_SOUNDCNT_L  0x80
#define REG_OFFSET_NR50        0x80
#define REG_OFFSET_NR51        0x81
#define REG_OFFSET_SOUNDCNT_H  0x82
#define REG_OFFSET_SOUNDCNT_X  0x84
#define REG_OFFSET_NR52        0x84
#define REG_OFFSET_SOUNDBIAS   0x88
#define REG_OFFSET_SOUNDBIAS_L 0x88
#define REG_OFFSET_SOUNDBIAS_H 0x89
#define REG_OFFSET_WAVE_RAM0   0x90
#define REG_OFFSET_WAVE_RAM1   0x94
#define REG_OFFSET_WAVE_RAM2   0x98
#define REG_OFFSET_WAVE_RAM3   0x9c
#define REG_OFFSET_FIFO_A      0xa0
#define REG_OFFSET_FIFO_B      0xa4

/*
 * --- DMA REGISTERS (0x0B0-0x0DE) ---
 *
 * DMA (Direct Memory Access) copies memory WITHOUT using the CPU.
 * The CPU just writes source, destination, and size to these registers,
 * then the DMA controller handles the copy in the background.
 *
 * The GBA has 4 DMA channels (0-3), each with 3 registers:
 *   SAD (Source Address):      Where to copy FROM (32-bit address)
 *   DAD (Destination Address): Where to copy TO (32-bit address)
 *   CNT (Control):             How many units + control flags
 *     CNT_L (low 16 bits):  Number of transfer units (halfwords or words)
 *     CNT_H (high 16 bits): Control flags (enable, timing, width, repeat)
 *
 * DMA CHANNEL PRIORITY AND USAGE:
 *   DMA0: Highest priority. Used for HBlank effects (scanline DMA).
 *   DMA1: Used by sound for Direct Sound channel A (PCM audio).
 *   DMA2: Used by sound for Direct Sound channel B (PCM audio).
 *   DMA3: Lowest priority. General purpose - VRAM copies, OAM copies, etc.
 *
 * TIMING MODES (when the transfer starts):
 *   NOW:     Immediately (general purpose copies)
 *   VBLANK:  At start of VBlank (safe VRAM/OAM updates)
 *   HBLANK:  At each HBlank (scanline effects like wave distortion)
 *   SPECIAL: Channel-specific (DMA1/2: sound FIFO refill)
 *
 * _L and _H suffixes split 32-bit registers into two 16-bit halves
 * because the GBA's register bus is 16 bits wide.
 */
#define REG_OFFSET_DMA0        0xb0
#define REG_OFFSET_DMA0SAD     0xb0
#define REG_OFFSET_DMA0SAD_L   0xb0
#define REG_OFFSET_DMA0SAD_H   0xb2
#define REG_OFFSET_DMA0DAD     0xb4
#define REG_OFFSET_DMA0DAD_L   0xb4
#define REG_OFFSET_DMA0DAD_H   0xb6
#define REG_OFFSET_DMA0CNT     0xb8
#define REG_OFFSET_DMA0CNT_L   0xb8
#define REG_OFFSET_DMA0CNT_H   0xba
#define REG_OFFSET_DMA1        0xbc
#define REG_OFFSET_DMA1SAD     0xbc
#define REG_OFFSET_DMA1SAD_L   0xbc
#define REG_OFFSET_DMA1SAD_H   0xbe
#define REG_OFFSET_DMA1DAD     0xc0
#define REG_OFFSET_DMA1DAD_L   0xc0
#define REG_OFFSET_DMA1DAD_H   0xc2
#define REG_OFFSET_DMA1CNT     0xc4
#define REG_OFFSET_DMA1CNT_L   0xc4
#define REG_OFFSET_DMA1CNT_H   0xc6
#define REG_OFFSET_DMA2        0xc8
#define REG_OFFSET_DMA2SAD     0xc8
#define REG_OFFSET_DMA2SAD_L   0xc8
#define REG_OFFSET_DMA2SAD_H   0xca
#define REG_OFFSET_DMA2DAD     0xcc
#define REG_OFFSET_DMA2DAD_L   0xcc
#define REG_OFFSET_DMA2DAD_H   0xce
#define REG_OFFSET_DMA2CNT     0xd0
#define REG_OFFSET_DMA2CNT_L   0xd0
#define REG_OFFSET_DMA2CNT_H   0xd2
#define REG_OFFSET_DMA3        0xd4
#define REG_OFFSET_DMA3SAD     0xd4
#define REG_OFFSET_DMA3SAD_L   0xd4
#define REG_OFFSET_DMA3SAD_H   0xd6
#define REG_OFFSET_DMA3DAD     0xd8
#define REG_OFFSET_DMA3DAD_L   0xd8
#define REG_OFFSET_DMA3DAD_H   0xda
#define REG_OFFSET_DMA3CNT     0xdc
#define REG_OFFSET_DMA3CNT_L   0xdc
#define REG_OFFSET_DMA3CNT_H   0xde

/*
 * --- TIMER REGISTERS (0x100-0x10E) ---
 *
 * The GBA has 4 hardware timers (0-3). Each timer is a 16-bit counter
 * that increments at a configurable rate.
 *
 * Each timer has two registers:
 *   CNT_L: Counter value. Read = current count. Write = reload value
 *           (the value the counter resets to after overflow).
 *   CNT_H: Control register.
 *     Bits 0-1: Prescaler (clock divider):
 *       00 = 1 (16.78 MHz, one tick per CPU cycle)
 *       01 = 64 (262.2 KHz)
 *       10 = 256 (65.5 KHz)
 *       11 = 1024 (16.4 KHz)
 *     Bit 2: Cascade mode (count when PREVIOUS timer overflows)
 *     Bit 6: Interrupt on overflow
 *     Bit 7: Enable timer
 *
 * Timer usage in this game:
 *   Timer 0: Sound engine (m4a) mixing rate
 *   Timer 1: Random seed generation (free-running)
 *   Timer 2: Flash memory write timing
 *   Timer 3: Link cable transfer timing
 */
#define REG_OFFSET_TMCNT       0x100
#define REG_OFFSET_TMCNT_L     0x100
#define REG_OFFSET_TMCNT_H     0x102
#define REG_OFFSET_TM0CNT      0x100
#define REG_OFFSET_TM0CNT_L    0x100
#define REG_OFFSET_TM0CNT_H    0x102
#define REG_OFFSET_TM1CNT      0x104
#define REG_OFFSET_TM1CNT_L    0x104
#define REG_OFFSET_TM1CNT_H    0x106
#define REG_OFFSET_TM2CNT      0x108
#define REG_OFFSET_TM2CNT_L    0x108
#define REG_OFFSET_TM2CNT_H    0x10a
#define REG_OFFSET_TM3CNT      0x10c
#define REG_OFFSET_TM3CNT_L    0x10c
#define REG_OFFSET_TM3CNT_H    0x10e

/*
 * --- SERIAL I/O REGISTERS (0x120-0x12A) ---
 *
 * Control the link cable port for multiplayer communication.
 *
 * In Multi-Player mode (the mode Pokemon uses):
 *   SIOMULTI0-3: Read the 16-bit value received from each player (0-3).
 *                After a transfer completes, all 4 GBAs have the same
 *                4 values in these registers.
 *   SIOMLT_SEND: Write the 16-bit value to send in the next transfer.
 *   SIOCNT:      Control register (baud rate, mode, start, interrupt).
 *
 * In Normal mode (2-player):
 *   SIODATA32: 32-bit data register for bidirectional transfer.
 *   SIODATA8:  8-bit data register.
 */
#define REG_OFFSET_SIOCNT      0x128
#define REG_OFFSET_SIODATA8    0x12a
#define REG_OFFSET_SIODATA32   0x120
#define REG_OFFSET_SIOMLT_SEND 0x12a
#define REG_OFFSET_SIOMLT_RECV 0x120
#define REG_OFFSET_SIOMULTI0   0x120
#define REG_OFFSET_SIOMULTI1   0x122
#define REG_OFFSET_SIOMULTI2   0x124
#define REG_OFFSET_SIOMULTI3   0x126

/*
 * --- KEYPAD INPUT REGISTERS (0x130-0x132) ---
 *
 * KEYINPUT: Button state register (READ-ONLY).
 *   10 bits, one per button. ACTIVE LOW: 0 = pressed, 1 = released.
 *   The game XORs with 0x3FF to invert this to 1 = pressed.
 *
 * KEYCNT: Keypad interrupt control.
 *   Can trigger an interrupt when specific button combinations are pressed.
 *   Used for the A+B+Start+Select soft reset check in some implementations.
 */
#define REG_OFFSET_KEYINPUT    0x130
#define REG_OFFSET_KEYCNT      0x132

#define REG_OFFSET_RCNT        0x134

#define REG_OFFSET_JOYCNT      0x140
#define REG_OFFSET_JOYSTAT     0x158
#define REG_OFFSET_JOY_RECV    0x150
#define REG_OFFSET_JOY_RECV_L  0x150
#define REG_OFFSET_JOY_RECV_H  0x152
#define REG_OFFSET_JOY_TRANS   0x154
#define REG_OFFSET_JOY_TRANS_L 0x154
#define REG_OFFSET_JOY_TRANS_H 0x156

/*
 * --- INTERRUPT CONTROL REGISTERS (0x200-0x208) ---
 *
 * IME (Interrupt Master Enable): Global on/off switch for ALL interrupts.
 *   0 = all interrupts disabled, 1 = enabled (subject to IE mask).
 *   Set to 0 before modifying IE to prevent race conditions.
 *
 * IE (Interrupt Enable): Bitmask of WHICH interrupts are allowed.
 *   Each bit corresponds to one interrupt source (see INTR_FLAG_* below).
 *   An interrupt only fires if BOTH IME=1 AND its IE bit is set.
 *
 * IF (Interrupt Flags): Which interrupts have FIRED (read) / acknowledge (write).
 *   READ: 1 = this interrupt is pending (has fired but not acknowledged).
 *   WRITE: Writing 1 to a bit CLEARS it (acknowledges the interrupt).
 *   This is "write-1-to-clear" behavior - writing 0 does nothing.
 *   You MUST acknowledge interrupts or they'll keep firing.
 *
 * NOTE: IE is at offset 0x200 but IME is at 0x208 - they're not in order!
 */
#define REG_OFFSET_IME         0x208
#define REG_OFFSET_IE          0x200
#define REG_OFFSET_IF          0x202

/*
 * --- WAIT STATE CONTROL (0x204) ---
 *
 * WAITCNT controls how many CPU cycles to wait when accessing ROM.
 * The ROM bus is 16-bit and slower than the CPU, so wait states are needed.
 *
 * The GBA ROM space has 3 "wait state regions" (WS0, WS1, WS2):
 *   0x08000000-0x09FFFFFF = Wait State 0 (most ROM code lives here)
 *   0x0A000000-0x0BFFFFFF = Wait State 1 (mirror, different timing)
 *   0x0C000000-0x0DFFFFFF = Wait State 2 (mirror, different timing)
 *
 * Each region has:
 *   N (Non-sequential): Wait for random/first access (2-8 cycles)
 *   S (Sequential): Wait for consecutive access (1-8 cycles)
 *
 * Sequential access is faster because the ROM chip doesn't need to
 * seek to a new address - it just reads the next byte.
 *
 * PREFETCH BUFFER: When enabled, the GBA pre-reads ROM instructions
 * while the CPU is busy with calculations. Huge performance win.
 */
#define REG_OFFSET_WAITCNT     0x204

// I/O register addresses

#define REG_ADDR_DISPCNT     (REG_BASE + REG_OFFSET_DISPCNT)
#define REG_ADDR_DISPSTAT    (REG_BASE + REG_OFFSET_DISPSTAT)
#define REG_ADDR_VCOUNT      (REG_BASE + REG_OFFSET_VCOUNT)
#define REG_ADDR_BG0CNT      (REG_BASE + REG_OFFSET_BG0CNT)
#define REG_ADDR_BG1CNT      (REG_BASE + REG_OFFSET_BG1CNT)
#define REG_ADDR_BG2CNT      (REG_BASE + REG_OFFSET_BG2CNT)
#define REG_ADDR_BG3CNT      (REG_BASE + REG_OFFSET_BG3CNT)
#define REG_ADDR_BG0HOFS     (REG_BASE + REG_OFFSET_BG0HOFS)
#define REG_ADDR_BG0VOFS     (REG_BASE + REG_OFFSET_BG0VOFS)
#define REG_ADDR_BG1HOFS     (REG_BASE + REG_OFFSET_BG1HOFS)
#define REG_ADDR_BG1VOFS     (REG_BASE + REG_OFFSET_BG1VOFS)
#define REG_ADDR_BG2HOFS     (REG_BASE + REG_OFFSET_BG2HOFS)
#define REG_ADDR_BG2VOFS     (REG_BASE + REG_OFFSET_BG2VOFS)
#define REG_ADDR_BG3HOFS     (REG_BASE + REG_OFFSET_BG3HOFS)
#define REG_ADDR_BG3VOFS     (REG_BASE + REG_OFFSET_BG3VOFS)
#define REG_ADDR_BG2PA       (REG_BASE + REG_OFFSET_BG2PA)
#define REG_ADDR_BG2PB       (REG_BASE + REG_OFFSET_BG2PB)
#define REG_ADDR_BG2PC       (REG_BASE + REG_OFFSET_BG2PC)
#define REG_ADDR_BG2PD       (REG_BASE + REG_OFFSET_BG2PD)
#define REG_ADDR_BG2X        (REG_BASE + REG_OFFSET_BG2X)
#define REG_ADDR_BG2X_L      (REG_BASE + REG_OFFSET_BG2X_L)
#define REG_ADDR_BG2X_H      (REG_BASE + REG_OFFSET_BG2X_H)
#define REG_ADDR_BG2Y        (REG_BASE + REG_OFFSET_BG2Y)
#define REG_ADDR_BG2Y_L      (REG_BASE + REG_OFFSET_BG2Y_L)
#define REG_ADDR_BG2Y_H      (REG_BASE + REG_OFFSET_BG2Y_H)
#define REG_ADDR_BG3PA       (REG_BASE + REG_OFFSET_BG3PA)
#define REG_ADDR_BG3PB       (REG_BASE + REG_OFFSET_BG3PB)
#define REG_ADDR_BG3PC       (REG_BASE + REG_OFFSET_BG3PC)
#define REG_ADDR_BG3PD       (REG_BASE + REG_OFFSET_BG3PD)
#define REG_ADDR_BG3X        (REG_BASE + REG_OFFSET_BG3X)
#define REG_ADDR_BG3X_L      (REG_BASE + REG_OFFSET_BG3X_L)
#define REG_ADDR_BG3X_H      (REG_BASE + REG_OFFSET_BG3X_H)
#define REG_ADDR_BG3Y        (REG_BASE + REG_OFFSET_BG3Y)
#define REG_ADDR_BG3Y_L      (REG_BASE + REG_OFFSET_BG3Y_L)
#define REG_ADDR_BG3Y_H      (REG_BASE + REG_OFFSET_BG3Y_H)
#define REG_ADDR_WIN0H       (REG_BASE + REG_OFFSET_WIN0H)
#define REG_ADDR_WIN1H       (REG_BASE + REG_OFFSET_WIN1H)
#define REG_ADDR_WIN0V       (REG_BASE + REG_OFFSET_WIN0V)
#define REG_ADDR_WIN1V       (REG_BASE + REG_OFFSET_WIN1V)
#define REG_ADDR_WININ       (REG_BASE + REG_OFFSET_WININ)
#define REG_ADDR_WINOUT      (REG_BASE + REG_OFFSET_WINOUT)
#define REG_ADDR_MOSAIC      (REG_BASE + REG_OFFSET_MOSAIC)
#define REG_ADDR_BLDCNT      (REG_BASE + REG_OFFSET_BLDCNT)
#define REG_ADDR_BLDALPHA    (REG_BASE + REG_OFFSET_BLDALPHA)
#define REG_ADDR_BLDY        (REG_BASE + REG_OFFSET_BLDY)

#define REG_ADDR_SOUND1CNT_L (REG_BASE + REG_OFFSET_SOUND1CNT_L)
#define REG_ADDR_NR10        (REG_BASE + REG_OFFSET_NR10)
#define REG_ADDR_SOUND1CNT_H (REG_BASE + REG_OFFSET_SOUND1CNT_H)
#define REG_ADDR_NR11        (REG_BASE + REG_OFFSET_NR11)
#define REG_ADDR_NR12        (REG_BASE + REG_OFFSET_NR12)
#define REG_ADDR_SOUND1CNT_X (REG_BASE + REG_OFFSET_SOUND1CNT_X)
#define REG_ADDR_NR13        (REG_BASE + REG_OFFSET_NR13)
#define REG_ADDR_NR14        (REG_BASE + REG_OFFSET_NR14)
#define REG_ADDR_SOUND2CNT_L (REG_BASE + REG_OFFSET_SOUND2CNT_L)
#define REG_ADDR_NR21        (REG_BASE + REG_OFFSET_NR21)
#define REG_ADDR_NR22        (REG_BASE + REG_OFFSET_NR22)
#define REG_ADDR_SOUND2CNT_H (REG_BASE + REG_OFFSET_SOUND2CNT_H)
#define REG_ADDR_NR23        (REG_BASE + REG_OFFSET_NR23)
#define REG_ADDR_NR24        (REG_BASE + REG_OFFSET_NR24)
#define REG_ADDR_SOUND3CNT_L (REG_BASE + REG_OFFSET_SOUND3CNT_L)
#define REG_ADDR_NR30        (REG_BASE + REG_OFFSET_NR30)
#define REG_ADDR_SOUND3CNT_H (REG_BASE + REG_OFFSET_SOUND3CNT_H)
#define REG_ADDR_NR31        (REG_BASE + REG_OFFSET_NR31)
#define REG_ADDR_NR32        (REG_BASE + REG_OFFSET_NR32)
#define REG_ADDR_SOUND3CNT_X (REG_BASE + REG_OFFSET_SOUND3CNT_X)
#define REG_ADDR_NR33        (REG_BASE + REG_OFFSET_NR33)
#define REG_ADDR_NR34        (REG_BASE + REG_OFFSET_NR34)
#define REG_ADDR_SOUND4CNT_L (REG_BASE + REG_OFFSET_SOUND4CNT_L)
#define REG_ADDR_NR41        (REG_BASE + REG_OFFSET_NR41)
#define REG_ADDR_NR42        (REG_BASE + REG_OFFSET_NR42)
#define REG_ADDR_SOUND4CNT_H (REG_BASE + REG_OFFSET_SOUND4CNT_H)
#define REG_ADDR_NR43        (REG_BASE + REG_OFFSET_NR43)
#define REG_ADDR_NR44        (REG_BASE + REG_OFFSET_NR44)
#define REG_ADDR_SOUNDCNT_L  (REG_BASE + REG_OFFSET_SOUNDCNT_L)
#define REG_ADDR_NR50        (REG_BASE + REG_OFFSET_NR50)
#define REG_ADDR_NR51        (REG_BASE + REG_OFFSET_NR51)
#define REG_ADDR_SOUNDCNT_H  (REG_BASE + REG_OFFSET_SOUNDCNT_H)
#define REG_ADDR_SOUNDCNT_X  (REG_BASE + REG_OFFSET_SOUNDCNT_X)
#define REG_ADDR_NR52        (REG_BASE + REG_OFFSET_NR52)
#define REG_ADDR_SOUNDBIAS   (REG_BASE + REG_OFFSET_SOUNDBIAS)
#define REG_ADDR_SOUNDBIAS_L (REG_BASE + REG_OFFSET_SOUNDBIAS_L)
#define REG_ADDR_SOUNDBIAS_H (REG_BASE + REG_OFFSET_SOUNDBIAS_H)
#define REG_ADDR_WAVE_RAM0   (REG_BASE + REG_OFFSET_WAVE_RAM0)
#define REG_ADDR_WAVE_RAM1   (REG_BASE + REG_OFFSET_WAVE_RAM1)
#define REG_ADDR_WAVE_RAM2   (REG_BASE + REG_OFFSET_WAVE_RAM2)
#define REG_ADDR_WAVE_RAM3   (REG_BASE + REG_OFFSET_WAVE_RAM3)
#define REG_ADDR_FIFO_A      (REG_BASE + REG_OFFSET_FIFO_A)
#define REG_ADDR_FIFO_B      (REG_BASE + REG_OFFSET_FIFO_B)

#define REG_ADDR_DMA0        (REG_BASE + REG_OFFSET_DMA0)
#define REG_ADDR_DMA0SAD     (REG_BASE + REG_OFFSET_DMA0SAD)
#define REG_ADDR_DMA0DAD     (REG_BASE + REG_OFFSET_DMA0DAD)
#define REG_ADDR_DMA0CNT     (REG_BASE + REG_OFFSET_DMA0CNT)
#define REG_ADDR_DMA0CNT_L   (REG_BASE + REG_OFFSET_DMA0CNT_L)
#define REG_ADDR_DMA0CNT_H   (REG_BASE + REG_OFFSET_DMA0CNT_H)
#define REG_ADDR_DMA1        (REG_BASE + REG_OFFSET_DMA1)
#define REG_ADDR_DMA1SAD     (REG_BASE + REG_OFFSET_DMA1SAD)
#define REG_ADDR_DMA1DAD     (REG_BASE + REG_OFFSET_DMA1DAD)
#define REG_ADDR_DMA1CNT     (REG_BASE + REG_OFFSET_DMA1CNT)
#define REG_ADDR_DMA1CNT_L   (REG_BASE + REG_OFFSET_DMA1CNT_L)
#define REG_ADDR_DMA1CNT_H   (REG_BASE + REG_OFFSET_DMA1CNT_H)
#define REG_ADDR_DMA2        (REG_BASE + REG_OFFSET_DMA2)
#define REG_ADDR_DMA2SAD     (REG_BASE + REG_OFFSET_DMA2SAD)
#define REG_ADDR_DMA2DAD     (REG_BASE + REG_OFFSET_DMA2DAD)
#define REG_ADDR_DMA2CNT     (REG_BASE + REG_OFFSET_DMA2CNT)
#define REG_ADDR_DMA2CNT_L   (REG_BASE + REG_OFFSET_DMA2CNT_L)
#define REG_ADDR_DMA2CNT_H   (REG_BASE + REG_OFFSET_DMA2CNT_H)
#define REG_ADDR_DMA3        (REG_BASE + REG_OFFSET_DMA3)
#define REG_ADDR_DMA3SAD     (REG_BASE + REG_OFFSET_DMA3SAD)
#define REG_ADDR_DMA3DAD     (REG_BASE + REG_OFFSET_DMA3DAD)
#define REG_ADDR_DMA3CNT     (REG_BASE + REG_OFFSET_DMA3CNT)
#define REG_ADDR_DMA3CNT_L   (REG_BASE + REG_OFFSET_DMA3CNT_L)
#define REG_ADDR_DMA3CNT_H   (REG_BASE + REG_OFFSET_DMA3CNT_H)

#define REG_ADDR_TMCNT       (REG_BASE + REG_OFFSET_TMCNT)
#define REG_ADDR_TMCNT_L     (REG_BASE + REG_OFFSET_TMCNT_L)
#define REG_ADDR_TMCNT_H     (REG_BASE + REG_OFFSET_TMCNT_H)
#define REG_ADDR_TM0CNT      (REG_BASE + REG_OFFSET_TM0CNT)
#define REG_ADDR_TM0CNT_L    (REG_BASE + REG_OFFSET_TM0CNT_L)
#define REG_ADDR_TM0CNT_H    (REG_BASE + REG_OFFSET_TM0CNT_H)
#define REG_ADDR_TM1CNT      (REG_BASE + REG_OFFSET_TM1CNT)
#define REG_ADDR_TM1CNT_L    (REG_BASE + REG_OFFSET_TM1CNT_L)
#define REG_ADDR_TM1CNT_H    (REG_BASE + REG_OFFSET_TM1CNT_H)
#define REG_ADDR_TM2CNT      (REG_BASE + REG_OFFSET_TM2CNT)
#define REG_ADDR_TM2CNT_L    (REG_BASE + REG_OFFSET_TM2CNT_L)
#define REG_ADDR_TM2CNT_H    (REG_BASE + REG_OFFSET_TM2CNT_H)
#define REG_ADDR_TM3CNT      (REG_BASE + REG_OFFSET_TM3CNT)
#define REG_ADDR_TM3CNT_L    (REG_BASE + REG_OFFSET_TM3CNT_L)
#define REG_ADDR_TM3CNT_H    (REG_BASE + REG_OFFSET_TM3CNT_H)

#define REG_ADDR_SIOCNT      (REG_BASE + REG_OFFSET_SIOCNT)
#define REG_ADDR_SIODATA8    (REG_BASE + REG_OFFSET_SIODATA8)
#define REG_ADDR_SIODATA32   (REG_BASE + REG_OFFSET_SIODATA32)
#define REG_ADDR_SIOMLT_SEND (REG_BASE + REG_OFFSET_SIOMLT_SEND)
#define REG_ADDR_SIOMLT_RECV (REG_BASE + REG_OFFSET_SIOMLT_RECV)
#define REG_ADDR_SIOMULTI0   (REG_BASE + REG_OFFSET_SIOMULTI0)
#define REG_ADDR_SIOMULTI1   (REG_BASE + REG_OFFSET_SIOMULTI1)
#define REG_ADDR_SIOMULTI2   (REG_BASE + REG_OFFSET_SIOMULTI2)
#define REG_ADDR_SIOMULTI3   (REG_BASE + REG_OFFSET_SIOMULTI3)

#define REG_ADDR_KEYINPUT    (REG_BASE + REG_OFFSET_KEYINPUT)
#define REG_ADDR_KEYCNT      (REG_BASE + REG_OFFSET_KEYCNT)

#define REG_ADDR_RCNT        (REG_BASE + REG_OFFSET_RCNT)

#define REG_ADDR_JOYCNT      (REG_BASE + REG_OFFSET_JOYCNT)
#define REG_ADDR_JOYSTAT     (REG_BASE + REG_OFFSET_JOYSTAT)
#define REG_ADDR_JOY_RECV    (REG_BASE + REG_OFFSET_JOY_RECV)
#define REG_ADDR_JOY_RECV_L  (REG_BASE + REG_OFFSET_JOY_RECV_L)
#define REG_ADDR_JOY_RECV_H  (REG_BASE + REG_OFFSET_JOY_RECV_H)
#define REG_ADDR_JOY_TRANS   (REG_BASE + REG_OFFSET_JOY_TRANS)
#define REG_ADDR_JOY_TRANS_L (REG_BASE + REG_OFFSET_JOY_TRANS_L)
#define REG_ADDR_JOY_TRANS_H (REG_BASE + REG_OFFSET_JOY_TRANS_H)

#define REG_ADDR_IME         (REG_BASE + REG_OFFSET_IME)
#define REG_ADDR_IE          (REG_BASE + REG_OFFSET_IE)
#define REG_ADDR_IF          (REG_BASE + REG_OFFSET_IF)

#define REG_ADDR_WAITCNT     (REG_BASE + REG_OFFSET_WAITCNT)

// I/O registers

#define REG_DISPCNT     (*(vu16 *)REG_ADDR_DISPCNT)
#define REG_DISPSTAT    (*(vu16 *)REG_ADDR_DISPSTAT)
#define REG_VCOUNT      (*(vu16 *)REG_ADDR_VCOUNT)
#define REG_BG0CNT      (*(vu16 *)REG_ADDR_BG0CNT)
#define REG_BG1CNT      (*(vu16 *)REG_ADDR_BG1CNT)
#define REG_BG2CNT      (*(vu16 *)REG_ADDR_BG2CNT)
#define REG_BG3CNT      (*(vu16 *)REG_ADDR_BG3CNT)
#define REG_BG0HOFS     (*(vu16 *)REG_ADDR_BG0HOFS)
#define REG_BG0VOFS     (*(vu16 *)REG_ADDR_BG0VOFS)
#define REG_BG1HOFS     (*(vu16 *)REG_ADDR_BG1HOFS)
#define REG_BG1VOFS     (*(vu16 *)REG_ADDR_BG1VOFS)
#define REG_BG2HOFS     (*(vu16 *)REG_ADDR_BG2HOFS)
#define REG_BG2VOFS     (*(vu16 *)REG_ADDR_BG2VOFS)
#define REG_BG3HOFS     (*(vu16 *)REG_ADDR_BG3HOFS)
#define REG_BG3VOFS     (*(vu16 *)REG_ADDR_BG3VOFS)
#define REG_BG2PA       (*(vu16 *)REG_ADDR_BG2PA)
#define REG_BG2PB       (*(vu16 *)REG_ADDR_BG2PB)
#define REG_BG2PC       (*(vu16 *)REG_ADDR_BG2PC)
#define REG_BG2PD       (*(vu16 *)REG_ADDR_BG2PD)
#define REG_BG2X        (*(vu32 *)REG_ADDR_BG2X)
#define REG_BG2X_L      (*(vu16 *)REG_ADDR_BG2X_L)
#define REG_BG2X_H      (*(vu16 *)REG_ADDR_BG2X_H)
#define REG_BG2Y        (*(vu32 *)REG_ADDR_BG2Y)
#define REG_BG2Y_L      (*(vu16 *)REG_ADDR_BG2Y_L)
#define REG_BG2Y_H      (*(vu16 *)REG_ADDR_BG2Y_H)
#define REG_BG3PA       (*(vu16 *)REG_ADDR_BG3PA)
#define REG_BG3PB       (*(vu16 *)REG_ADDR_BG3PB)
#define REG_BG3PC       (*(vu16 *)REG_ADDR_BG3PC)
#define REG_BG3PD       (*(vu16 *)REG_ADDR_BG3PD)
#define REG_BG3X        (*(vu32 *)REG_ADDR_BG3X)
#define REG_BG3X_L      (*(vu16 *)REG_ADDR_BG3X_L)
#define REG_BG3X_H      (*(vu16 *)REG_ADDR_BG3X_H)
#define REG_BG3Y        (*(vu32 *)REG_ADDR_BG3Y)
#define REG_BG3Y_L      (*(vu16 *)REG_ADDR_BG3Y_L)
#define REG_BG3Y_H      (*(vu16 *)REG_ADDR_BG3Y_H)
#define REG_WIN0H       (*(vu16 *)REG_ADDR_WIN0H)
#define REG_WIN1H       (*(vu16 *)REG_ADDR_WIN1H)
#define REG_WIN0V       (*(vu16 *)REG_ADDR_WIN0V)
#define REG_WIN1V       (*(vu16 *)REG_ADDR_WIN1V)
#define REG_WININ       (*(vu16 *)REG_ADDR_WININ)
#define REG_WINOUT      (*(vu16 *)REG_ADDR_WINOUT)
#define REG_MOSAIC      (*(vu16 *)REG_ADDR_MOSAIC)
#define REG_BLDCNT      (*(vu16 *)REG_ADDR_BLDCNT)
#define REG_BLDALPHA    (*(vu16 *)REG_ADDR_BLDALPHA)
#define REG_BLDY        (*(vu16 *)REG_ADDR_BLDY)

#define REG_SOUND1CNT_L (*(vu16 *)REG_ADDR_SOUND1CNT_L)
#define REG_NR10        (*(vu8  *)REG_ADDR_NR10)
#define REG_SOUND1CNT_H (*(vu16 *)REG_ADDR_SOUND1CNT_H)
#define REG_NR11        (*(vu8  *)REG_ADDR_NR11)
#define REG_NR12        (*(vu8  *)REG_ADDR_NR12)
#define REG_SOUND1CNT_X (*(vu16 *)REG_ADDR_SOUND1CNT_X)
#define REG_NR13        (*(vu8  *)REG_ADDR_NR13)
#define REG_NR14        (*(vu8  *)REG_ADDR_NR14)
#define REG_SOUND2CNT_L (*(vu16 *)REG_ADDR_SOUND2CNT_L)
#define REG_NR21        (*(vu8  *)REG_ADDR_NR21)
#define REG_NR22        (*(vu8  *)REG_ADDR_NR22)
#define REG_SOUND2CNT_H (*(vu16 *)REG_ADDR_SOUND2CNT_H)
#define REG_NR23        (*(vu8  *)REG_ADDR_NR23)
#define REG_NR24        (*(vu8  *)REG_ADDR_NR24)
#define REG_SOUND3CNT_L (*(vu16 *)REG_ADDR_SOUND3CNT_L)
#define REG_NR30        (*(vu8  *)REG_ADDR_NR30)
#define REG_SOUND3CNT_H (*(vu16 *)REG_ADDR_SOUND3CNT_H)
#define REG_NR31        (*(vu8  *)REG_ADDR_NR31)
#define REG_NR32        (*(vu8  *)REG_ADDR_NR32)
#define REG_SOUND3CNT_X (*(vu16 *)REG_ADDR_SOUND3CNT_X)
#define REG_NR33        (*(vu8  *)REG_ADDR_NR33)
#define REG_NR34        (*(vu8  *)REG_ADDR_NR34)
#define REG_SOUND4CNT_L (*(vu16 *)REG_ADDR_SOUND4CNT_L)
#define REG_NR41        (*(vu8  *)REG_ADDR_NR41)
#define REG_NR42        (*(vu8  *)REG_ADDR_NR42)
#define REG_SOUND4CNT_H (*(vu16 *)REG_ADDR_SOUND4CNT_H)
#define REG_NR43        (*(vu8  *)REG_ADDR_NR43)
#define REG_NR44        (*(vu8  *)REG_ADDR_NR44)
#define REG_SOUNDCNT_L  (*(vu16 *)REG_ADDR_SOUNDCNT_L)
#define REG_NR50        (*(vu8  *)REG_ADDR_NR50)
#define REG_NR51        (*(vu8  *)REG_ADDR_NR51)
#define REG_SOUNDCNT_H  (*(vu16 *)REG_ADDR_SOUNDCNT_H)
#define REG_SOUNDCNT_X  (*(vu16 *)REG_ADDR_SOUNDCNT_X)
#define REG_NR52        (*(vu8  *)REG_ADDR_NR52)
#define REG_SOUNDBIAS   (*(vu16 *)REG_ADDR_SOUNDBIAS)
#define REG_SOUNDBIAS_L (*(vu8  *)REG_ADDR_SOUNDBIAS_L)
#define REG_SOUNDBIAS_H (*(vu8  *)REG_ADDR_SOUNDBIAS_H)
#define REG_WAVE_RAM0   (*(vu32 *)REG_ADDR_WAVE_RAM0)
#define REG_WAVE_RAM1   (*(vu32 *)REG_ADDR_WAVE_RAM1)
#define REG_WAVE_RAM2   (*(vu32 *)REG_ADDR_WAVE_RAM2)
#define REG_WAVE_RAM3   (*(vu32 *)REG_ADDR_WAVE_RAM3)
#define REG_FIFO_A      (*(vu32 *)REG_ADDR_FIFO_A)
#define REG_FIFO_B      (*(vu32 *)REG_ADDR_FIFO_B)

#define REG_DMA0SAD     (*(vu32 *)REG_ADDR_DMA0SAD)
#define REG_DMA0DAD     (*(vu32 *)REG_ADDR_DMA0DAD)
#define REG_DMA0CNT     (*(vu32 *)REG_ADDR_DMA0CNT)
#define REG_DMA0CNT_L   (*(vu16 *)REG_ADDR_DMA0CNT_L)
#define REG_DMA0CNT_H   (*(vu16 *)REG_ADDR_DMA0CNT_H)

#define REG_DMA1SAD     (*(vu32 *)REG_ADDR_DMA1SAD)
#define REG_DMA1DAD     (*(vu32 *)REG_ADDR_DMA1DAD)
#define REG_DMA1CNT     (*(vu32 *)REG_ADDR_DMA1CNT)
#define REG_DMA1CNT_L   (*(vu16 *)REG_ADDR_DMA1CNT_L)
#define REG_DMA1CNT_H   (*(vu16 *)REG_ADDR_DMA1CNT_H)

#define REG_DMA2SAD     (*(vu32 *)REG_ADDR_DMA2SAD)
#define REG_DMA2DAD     (*(vu32 *)REG_ADDR_DMA2DAD)
#define REG_DMA2CNT     (*(vu32 *)REG_ADDR_DMA2CNT)
#define REG_DMA2CNT_L   (*(vu16 *)REG_ADDR_DMA2CNT_L)
#define REG_DMA2CNT_H   (*(vu16 *)REG_ADDR_DMA2CNT_H)

#define REG_DMA3SAD     (*(vu32 *)REG_ADDR_DMA3SAD)
#define REG_DMA3DAD     (*(vu32 *)REG_ADDR_DMA3DAD)
#define REG_DMA3CNT     (*(vu32 *)REG_ADDR_DMA3CNT)
#define REG_DMA3CNT_L   (*(vu16 *)REG_ADDR_DMA3CNT_L)
#define REG_DMA3CNT_H   (*(vu16 *)REG_ADDR_DMA3CNT_H)

#define REG_TMCNT(n)    (*(vu32 *)(REG_ADDR_TMCNT + ((n) * 4)))
#define REG_TMCNT_L(n)  (*(vu16 *)(REG_ADDR_TMCNT_L + ((n) * 4)))
#define REG_TMCNT_H(n)  (*(vu16 *)(REG_ADDR_TMCNT_H + ((n) * 4)))
#define REG_TM0CNT      (*(vu32 *)REG_ADDR_TM0CNT)
#define REG_TM0CNT_L    (*(vu16 *)REG_ADDR_TM0CNT_L)
#define REG_TM0CNT_H    (*(vu16 *)REG_ADDR_TM0CNT_H)
#define REG_TM1CNT      (*(vu32 *)REG_ADDR_TM1CNT)
#define REG_TM1CNT_L    (*(vu16 *)REG_ADDR_TM1CNT_L)
#define REG_TM1CNT_H    (*(vu16 *)REG_ADDR_TM1CNT_H)
#define REG_TM2CNT      (*(vu32 *)REG_ADDR_TM2CNT)
#define REG_TM2CNT_L    (*(vu16 *)REG_ADDR_TM2CNT_L)
#define REG_TM2CNT_H    (*(vu16 *)REG_ADDR_TM2CNT_H)
#define REG_TM3CNT      (*(vu32 *)REG_ADDR_TM3CNT)
#define REG_TM3CNT_L    (*(vu16 *)REG_ADDR_TM3CNT_L)
#define REG_TM3CNT_H    (*(vu16 *)REG_ADDR_TM3CNT_H)

#define REG_SIOCNT      (*(vu16 *)REG_ADDR_SIOCNT)
#define REG_SIODATA8    (*(vu16 *)REG_ADDR_SIODATA8)
#define REG_SIODATA32   (*(vu32 *)REG_ADDR_SIODATA32)
#define REG_SIOMLT_SEND (*(vu16 *)REG_ADDR_SIOMLT_SEND)
#define REG_SIOMLT_RECV (*(vu64 *)REG_ADDR_SIOMLT_RECV)
#define REG_SIOMULTI0   (*(vu16 *)REG_ADDR_SIOMULTI0)
#define REG_SIOMULTI1   (*(vu16 *)REG_ADDR_SIOMULTI1)
#define REG_SIOMULTI2   (*(vu16 *)REG_ADDR_SIOMULTI2)
#define REG_SIOMULTI3   (*(vu16 *)REG_ADDR_SIOMULTI3)

#define REG_KEYINPUT    (*(vu16 *)REG_ADDR_KEYINPUT)
#define REG_KEYCNT      (*(vu16 *)REG_ADDR_KEYCNT)

#define REG_RCNT        (*(vu16 *)REG_ADDR_RCNT)

#define REG_IME         (*(vu16 *)REG_ADDR_IME)
#define REG_IE          (*(vu16 *)REG_ADDR_IE)
#define REG_IF          (*(vu16 *)REG_ADDR_IF)

#define REG_WAITCNT     (*(vu16 *)REG_ADDR_WAITCNT)

/*
 * ============================================================================
 * SECTION 4: REGISTER BIT FIELD CONSTANTS
 * ============================================================================
 *
 * These constants represent individual bits and bit fields within
 * I/O registers. They're used with bitwise OR (|) to combine settings:
 *
 *   REG_DISPCNT = DISPCNT_MODE_0 | DISPCNT_OBJ_ON | DISPCNT_BG0_ON;
 *   //            ^mode 0 (tiled)  ^sprites visible  ^BG layer 0 visible
 */

/*
 * --- DISPCNT BIT FIELDS (Display Control Register) ---
 *
 * This is THE most important GPU register. It controls:
 * - Which display mode is active (tiled vs bitmap)
 * - Which layers are visible
 * - Sprite mapping mode
 * - Window enables
 */
#define DISPCNT_MODE_0       0x0000 /* Mode 0: 4 tiled BG layers (Pokemon uses this) */
#define DISPCNT_MODE_1       0x0001 /* Mode 1: 2 tiled + 1 affine BG */
#define DISPCNT_MODE_2       0x0002 /* Mode 2: 2 affine BG layers */
#define DISPCNT_MODE_3       0x0003 /* Mode 3: Full-screen 240x160 bitmap, 15-bit color */
#define DISPCNT_MODE_4       0x0004 /* Mode 4: Full-screen 240x160 bitmap, 256 colors */
#define DISPCNT_MODE_5       0x0005 /* Mode 5: Smaller 160x128 bitmap, 15-bit color */
#define DISPCNT_OBJ_1D_MAP   0x0040 /* Sprite tile mapping: 1D (tiles in sequence) vs
                                     * 2D (tiles in a grid). 1D is simpler and used by
                                     * almost all games including Pokemon. */
#define DISPCNT_FORCED_BLANK 0x0080 /* Force screen to white. Used during VRAM setup
                                     * to prevent garbage on screen while loading graphics. */
#define DISPCNT_BG0_ON       0x0100 /* Enable BG layer 0 (bit 8) */
#define DISPCNT_BG1_ON       0x0200 /* Enable BG layer 1 (bit 9) */
#define DISPCNT_BG2_ON       0x0400 /* Enable BG layer 2 (bit 10) */
#define DISPCNT_BG3_ON       0x0800 /* Enable BG layer 3 (bit 11) */
#define DISPCNT_BG_ALL_ON    0x0F00 /* Enable all 4 BG layers */
#define DISPCNT_OBJ_ON       0x1000 /* Enable sprites/objects (bit 12) */
#define DISPCNT_WIN0_ON      0x2000 /* Enable hardware window 0 (bit 13) */
#define DISPCNT_WIN1_ON      0x4000 /* Enable hardware window 1 (bit 14) */
#define DISPCNT_OBJWIN_ON    0x8000 /* Enable OBJ window (sprite-shaped mask, bit 15) */

// DISPSTAT
#define DISPSTAT_VBLANK      0x0001 // in V-Blank
#define DISPSTAT_HBLANK      0x0002 // in H-Blank
#define DISPSTAT_VCOUNT      0x0004 // V-Count match
#define DISPSTAT_VBLANK_INTR 0x0008 // V-Blank interrupt enabled
#define DISPSTAT_HBLANK_INTR 0x0010 // H-Blank interrupt enabled
#define DISPSTAT_VCOUNT_INTR 0x0020 // V-Count interrupt enabled

// BGCNT
#define BGCNT_PRIORITY(n)          (n) // Values 0 - 3. Lower priority BGs will be drawn on top of higher priority BGs.
#define BGCNT_CHARBASE(n)   ((n) << 2) // Values 0 - 3. Base block for tile pixel data.
#define BGCNT_MOSAIC            0x0040
#define BGCNT_16COLOR           0x0000 // 4 bits per pixel
#define BGCNT_256COLOR          0x0080 // 8 bits per pixel
#define BGCNT_SCREENBASE(n) ((n) << 8) // Values 0 - 31. Base block for tile map.
#define BGCNT_WRAP              0x2000 // Only affects affine BGs. Text BGs wrap by default.
#define BGCNT_TXT256x256        0x0000 // Internal screen size size of text mode BG in pixels.
#define BGCNT_TXT512x256        0x4000
#define BGCNT_TXT256x512        0x8000
#define BGCNT_TXT512x512        0xC000
#define BGCNT_AFF128x128        0x0000 // Internal screen size size of affine mode BG in pixels.
#define BGCNT_AFF256x256        0x4000
#define BGCNT_AFF512x512        0x8000
#define BGCNT_AFF1024x1024      0xC000

// WININ/OUT
#define WININ_WIN0_BG0      (1 << 0)
#define WININ_WIN0_BG1      (1 << 1)
#define WININ_WIN0_BG2      (1 << 2)
#define WININ_WIN0_BG3      (1 << 3)
#define WININ_WIN0_BG_ALL   (WININ_WIN0_BG0 | WININ_WIN0_BG1 | WININ_WIN0_BG2 | WININ_WIN0_BG3)
#define WININ_WIN0_OBJ      (1 << 4)
#define WININ_WIN0_CLR      (1 << 5)
#define WININ_WIN0_ALL      (WININ_WIN0_BG_ALL | WININ_WIN0_OBJ | WININ_WIN0_CLR)
#define WININ_WIN1_BG0      (1 << 8)
#define WININ_WIN1_BG1      (1 << 9)
#define WININ_WIN1_BG2      (1 << 10)
#define WININ_WIN1_BG3      (1 << 11)
#define WININ_WIN1_BG_ALL   (WININ_WIN1_BG0 | WININ_WIN1_BG1 | WININ_WIN1_BG2 | WININ_WIN1_BG3)
#define WININ_WIN1_OBJ      (1 << 12)
#define WININ_WIN1_CLR      (1 << 13)
#define WININ_WIN1_ALL      (WININ_WIN1_BG_ALL | WININ_WIN1_OBJ | WININ_WIN1_CLR)

#define WINOUT_WIN01_BG0    (1 << 0)
#define WINOUT_WIN01_BG1    (1 << 1)
#define WINOUT_WIN01_BG2    (1 << 2)
#define WINOUT_WIN01_BG3    (1 << 3)
#define WINOUT_WIN01_BG_ALL (WINOUT_WIN01_BG0 | WINOUT_WIN01_BG1 | WINOUT_WIN01_BG2 | WINOUT_WIN01_BG3)
#define WINOUT_WIN01_OBJ    (1 << 4)
#define WINOUT_WIN01_CLR    (1 << 5)
#define WINOUT_WIN01_ALL    (WINOUT_WIN01_BG_ALL | WINOUT_WIN01_OBJ | WINOUT_WIN01_CLR)
#define WINOUT_WINOBJ_BG0   (1 << 8)
#define WINOUT_WINOBJ_BG1   (1 << 9)
#define WINOUT_WINOBJ_BG2   (1 << 10)
#define WINOUT_WINOBJ_BG3   (1 << 11)
#define WINOUT_WINOBJ_BG_ALL (WINOUT_WINOBJ_BG0 | WINOUT_WINOBJ_BG1 | WINOUT_WINOBJ_BG2 | WINOUT_WINOBJ_BG3)
#define WINOUT_WINOBJ_OBJ   (1 << 12)
#define WINOUT_WINOBJ_CLR   (1 << 13)
#define WINOUT_WINOBJ_ALL   (WINOUT_WINOBJ_BG_ALL | WINOUT_WINOBJ_OBJ | WINOUT_WINOBJ_CLR)

#define WIN_RANGE(a, b) (((a) << 8) | (b))
#define WIN_RANGE2(a, b) ((b) | ((a) << 8))

// BLDCNT
// Bits 0-5 select layers for the 1st target
#define BLDCNT_TGT1_BG0      (1 << 0)
#define BLDCNT_TGT1_BG1      (1 << 1)
#define BLDCNT_TGT1_BG2      (1 << 2)
#define BLDCNT_TGT1_BG3      (1 << 3)
#define BLDCNT_TGT1_BG_ALL   (BLDCNT_TGT1_BG0 | BLDCNT_TGT1_BG1 | BLDCNT_TGT1_BG2 | BLDCNT_TGT1_BG3)
#define BLDCNT_TGT1_OBJ      (1 << 4)
#define BLDCNT_TGT1_BD       (1 << 5)
#define BLDCNT_TGT1_ALL      (BLDCNT_TGT1_BG_ALL | BLDCNT_TGT1_OBJ | BLDCNT_TGT1_BD)
// Bits 6-7 select the special effect
#define BLDCNT_EFFECT_NONE      (0 << 6)   // no special effect
#define BLDCNT_EFFECT_BLEND     (1 << 6)   // 1st+2nd targets mixed (controlled by BLDALPHA)
#define BLDCNT_EFFECT_LIGHTEN   (2 << 6)   // 1st target becomes whiter (controlled by BLDY)
#define BLDCNT_EFFECT_DARKEN    (3 << 6)   // 1st target becomes blacker (controlled by BLDY)
// Bits 8-13 select layers for the 2nd target
#define BLDCNT_TGT2_BG0      (1 << 8)
#define BLDCNT_TGT2_BG1      (1 << 9)
#define BLDCNT_TGT2_BG2      (1 << 10)
#define BLDCNT_TGT2_BG3      (1 << 11)
#define BLDCNT_TGT2_BG_ALL   (BLDCNT_TGT2_BG0 | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3)
#define BLDCNT_TGT2_OBJ      (1 << 12)
#define BLDCNT_TGT2_BD       (1 << 13)
#define BLDCNT_TGT2_ALL      (BLDCNT_TGT2_BG_ALL | BLDCNT_TGT2_OBJ | BLDCNT_TGT2_BD)

// BLDALPHA
#define BLDALPHA_BLEND(target1, target2) (((target2) << 8) | (target1))
#define BLDALPHA_BLEND2(target1, target2) ((target1) | ((target2) << 8))

// SOUNDCNT_H
#define SOUND_CGB_MIX_QUARTER 0x0000
#define SOUND_CGB_MIX_HALF    0x0001
#define SOUND_CGB_MIX_FULL    0x0002
#define SOUND_A_MIX_HALF      0x0000
#define SOUND_A_MIX_FULL      0x0004
#define SOUND_B_MIX_HALF      0x0000
#define SOUND_B_MIX_FULL      0x0008
#define SOUND_ALL_MIX_FULL    0x000E
#define SOUND_A_RIGHT_OUTPUT  0x0100
#define SOUND_A_LEFT_OUTPUT   0x0200
#define SOUND_A_TIMER_0       0x0000
#define SOUND_A_TIMER_1       0x0400
#define SOUND_A_FIFO_RESET    0x0800
#define SOUND_B_RIGHT_OUTPUT  0x1000
#define SOUND_B_LEFT_OUTPUT   0x2000
#define SOUND_B_TIMER_0       0x0000
#define SOUND_B_TIMER_1       0x4000
#define SOUND_B_FIFO_RESET    0x8000

// SOUNDCNT_X
#define SOUND_1_ON          0x0001
#define SOUND_2_ON          0x0002
#define SOUND_3_ON          0x0004
#define SOUND_4_ON          0x0008
#define SOUND_MASTER_ENABLE 0x0080

/*
 * --- DMA CONTROL BIT FIELDS (DMAx_CNT_H upper 16 bits) ---
 *
 * These control how a DMA transfer operates.
 * Typical usage: DMA_ENABLE | DMA_START_NOW | DMA_32BIT | DMA_SRC_INC | DMA_DEST_INC
 */
#define DMA_DEST_INC      0x0000 /* Destination address increments after each transfer */
#define DMA_DEST_DEC      0x0020 /* Destination address decrements (copy backwards) */
#define DMA_DEST_FIXED    0x0040 /* Destination stays the same (write same address repeatedly) */
#define DMA_DEST_RELOAD   0x0060 /* Destination resets to initial value each repeat */
#define DMA_SRC_INC       0x0000 /* Source address increments (normal copy) */
#define DMA_SRC_DEC       0x0080 /* Source address decrements */
#define DMA_SRC_FIXED     0x0100 /* Source stays the same (fill: copy one value everywhere) */
#define DMA_REPEAT        0x0200 /* Repeat transfer (used with HBLANK/VBLANK timing) */
#define DMA_16BIT         0x0000 /* Transfer 16 bits (2 bytes) at a time */
#define DMA_32BIT         0x0400 /* Transfer 32 bits (4 bytes) at a time (faster for aligned data) */
#define DMA_DREQ_ON       0x0800 /* Game Pak DMA request (rarely used) */
#define DMA_START_NOW     0x0000 /* Start transfer immediately when DMA is enabled */
#define DMA_START_VBLANK  0x1000 /* Start transfer at next VBlank (safe for VRAM/OAM) */
#define DMA_START_HBLANK  0x2000 /* Start transfer at each HBlank (scanline effects) */
#define DMA_START_SPECIAL 0x3000 /* Special: DMA1/2 = sound FIFO, DMA3 = video capture */
#define DMA_START_MASK    0x3000 /* Mask for the start timing bits */
#define DMA_INTR_ENABLE   0x4000 /* Fire an interrupt when transfer completes */
#define DMA_ENABLE        0x8000 /* Master enable. Setting this bit starts the DMA. */

// timer
#define TIMER_1CLK        0x00
#define TIMER_64CLK       0x01
#define TIMER_256CLK      0x02
#define TIMER_1024CLK     0x03
#define TIMER_INTR_ENABLE 0x40
#define TIMER_ENABLE      0x80

// serial
#define SIO_ID             0x0030 // Communication ID

#define SIO_8BIT_MODE      0x0000 // Normal 8-bit communication mode
#define SIO_32BIT_MODE     0x1000 // Normal 32-bit communication mode
#define SIO_MULTI_MODE     0x2000 // Multi-player communication mode
#define SIO_UART_MODE      0x3000 // UART communication mode

#define SIO_9600_BPS       0x0000 // baud rate   9600 bps
#define SIO_38400_BPS      0x0001 //            38400 bps
#define SIO_57600_BPS      0x0002 //            57600 bps
#define SIO_115200_BPS     0x0003 //           115200 bps

#define SIO_MULTI_SI       0x0004 // Multi-player communication SI terminal
#define SIO_MULTI_SD       0x0008 //                            SD terminal
#define SIO_MULTI_BUSY     0x0080

#define SIO_ERROR          0x0040 // Detect error
#define SIO_START          0x0080 // Start transfer
#define SIO_ENABLE         0x0080 // Enable SIO

#define SIO_INTR_ENABLE    0x4000

#define SIO_MULTI_SI_SHIFT 2
#define SIO_MULTI_SI_MASK  0x1
#define SIO_MULTI_DI_SHIFT 3
#define SIO_MULTI_DI_MASK  0x1

/*
 * --- KEY INPUT BIT FIELDS (REG_KEYINPUT / KEYCNT) ---
 *
 * Each bit represents one button. In REG_KEYINPUT:
 *   0 = button is PRESSED, 1 = button is RELEASED (active-low)
 * The game XORs with KEYS_MASK (0x3FF) to invert to active-high.
 *
 * KEYS_MASK covers all 10 buttons (bits 0-9).
 * DPAD_ANY covers the 4 directional buttons.
 */
#define A_BUTTON        0x0001
#define B_BUTTON        0x0002
#define SELECT_BUTTON   0x0004
#define START_BUTTON    0x0008
#define DPAD_RIGHT      0x0010
#define DPAD_LEFT       0x0020
#define DPAD_UP         0x0040
#define DPAD_DOWN       0x0080
#define R_BUTTON        0x0100
#define L_BUTTON        0x0200
#define KEYS_MASK       0x03FF
#define KEY_INTR_ENABLE 0x4000
#define KEY_OR_INTR     0x0000
#define KEY_AND_INTR    0x8000
#define DPAD_ANY        0x00F0
#define JOY_EXCL_DPAD   0x030F

/*
 * --- INTERRUPT FLAG BIT FIELDS (REG_IE and REG_IF) ---
 *
 * These bits are used in BOTH the Interrupt Enable (IE) register
 * and the Interrupt Flags (IF) register:
 *
 *   IE: Set a bit to 1 to ALLOW that interrupt to fire.
 *   IF: Read 1 = interrupt has fired. Write 1 = acknowledge (clear).
 *
 * To enable VBlank interrupts:
 *   REG_IE |= INTR_FLAG_VBLANK;
 *
 * To acknowledge a VBlank interrupt in a handler:
 *   REG_IF = INTR_FLAG_VBLANK;  // Write 1 to clear (NOT |=, just =)
 */
#define INTR_FLAG_VBLANK  (1 <<  0) /* VBlank: start of vertical blank (scanline 160) */
#define INTR_FLAG_HBLANK  (1 <<  1) /* HBlank: end of each visible scanline */
#define INTR_FLAG_VCOUNT  (1 <<  2) /* VCount: scanline matches DISPSTAT target */
#define INTR_FLAG_TIMER0  (1 <<  3) /* Timer 0 overflow */
#define INTR_FLAG_TIMER1  (1 <<  4) /* Timer 1 overflow */
#define INTR_FLAG_TIMER2  (1 <<  5) /* Timer 2 overflow */
#define INTR_FLAG_TIMER3  (1 <<  6) /* Timer 3 overflow */
#define INTR_FLAG_SERIAL  (1 <<  7) /* Serial (link cable) transfer complete */
#define INTR_FLAG_DMA0    (1 <<  8) /* DMA channel 0 transfer complete */
#define INTR_FLAG_DMA1    (1 <<  9) /* DMA channel 1 transfer complete */
#define INTR_FLAG_DMA2    (1 << 10) /* DMA channel 2 transfer complete */
#define INTR_FLAG_DMA3    (1 << 11) /* DMA channel 3 transfer complete */
#define INTR_FLAG_KEYPAD  (1 << 12) /* Keypad: button combination pressed */
#define INTR_FLAG_GAMEPAK (1 << 13) /* Game Pak: cartridge removed (rarely used) */

/*
 * --- WAITCNT BIT FIELDS (Wait State Control) ---
 *
 * Configure ROM and SRAM access timing.
 * Lower wait states = faster, but may cause errors on some cartridges.
 * The values used by Pokemon (WS0_N_3 + WS0_S_1 + PREFETCH) are standard
 * for most commercial GBA games.
 *
 * SRAM: Save data access timing (0x0E000000)
 * WS0/WS1/WS2: ROM access timing for each of the 3 ROM mirrors
 * N = Non-sequential (first/random access), S = Sequential (consecutive)
 * PREFETCH: Enables the instruction prefetch buffer (always use this)
 */
#define WAITCNT_SRAM_4          (0 << 0)
#define WAITCNT_SRAM_3          (1 << 0)
#define WAITCNT_SRAM_2          (2 << 0)
#define WAITCNT_SRAM_8          (3 << 0)
#define WAITCNT_SRAM_MASK       (3 << 0)

#define WAITCNT_WS0_N_4         (0 << 2)
#define WAITCNT_WS0_N_3         (1 << 2)
#define WAITCNT_WS0_N_2         (2 << 2)
#define WAITCNT_WS0_N_8         (3 << 2)
#define WAITCNT_WS0_N_MASK      (3 << 2)

#define WAITCNT_WS0_S_2         (0 << 4)
#define WAITCNT_WS0_S_1         (1 << 4)

#define WAITCNT_WS1_N_4         (0 << 5)
#define WAITCNT_WS1_N_3         (1 << 5)
#define WAITCNT_WS1_N_2         (2 << 5)
#define WAITCNT_WS1_N_8         (3 << 5)
#define WAITCNT_WS1_N_MASK      (3 << 5)

#define WAITCNT_WS1_S_4         (0 << 7)
#define WAITCNT_WS1_S_1         (1 << 7)

#define WAITCNT_WS2_N_4         (0 << 8)
#define WAITCNT_WS2_N_3         (1 << 8)
#define WAITCNT_WS2_N_2         (2 << 8)
#define WAITCNT_WS2_N_8         (3 << 8)
#define WAITCNT_WS2_N_MASK      (3 << 8)

#define WAITCNT_WS2_S_8         (0 << 10)
#define WAITCNT_WS2_S_1         (1 << 10)

#define WAITCNT_PHI_OUT_NONE    (0 << 11)
#define WAITCNT_PHI_OUT_4MHZ    (1 << 11)
#define WAITCNT_PHI_OUT_8MHZ    (2 << 11)
#define WAITCNT_PHI_OUT_16MHZ   (3 << 11)
#define WAITCNT_PHI_OUT_MASK    (3 << 11)

#define WAITCNT_PREFETCH_ENABLE (1 << 14)

#define WAITCNT_AGB (0 << 15)
#define WAITCNT_CGB (1 << 15)

#endif // GUARD_GBA_IO_REG_H
