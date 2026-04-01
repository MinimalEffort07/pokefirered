/*
 * gpu_regs.c - GPU Register Buffer Manager
 *
 * ============================================================================
 * GBA GPU REGISTER OVERVIEW
 * ============================================================================
 *
 * The GBA's GPU (PPU - Pixel Processing Unit) is controlled through
 * memory-mapped I/O registers at 0x04000000 - 0x04000056.
 *
 * KEY GPU REGISTERS:
 *
 *   REG_DISPCNT (0x04000000) - Display Control. THE master GPU register.
 *     Bits 0-2:  BG Mode (0-5). Mode 0 = 4 text BG layers (used by Pokemon)
 *     Bit 4:     Display frame select (bitmap modes only)
 *     Bit 5:     HBlank interval free (allow OAM access during HBlank)
 *     Bit 6:     OBJ character mapping (1D vs 2D tile layout)
 *     Bit 7:     Forced blank (screen is white, VRAM/OAM/Palette accessible)
 *     Bits 8-11: Enable BG0, BG1, BG2, BG3 layers
 *     Bit 12:    Enable OBJ (sprites) layer
 *     Bits 13-14: Enable Window 0, Window 1
 *     Bit 15:    Enable OBJ Window
 *
 *   REG_DISPSTAT (0x04000004) - Display Status/Control.
 *     Bit 0: Currently in VBlank? (read-only)
 *     Bit 1: Currently in HBlank? (read-only)
 *     Bit 2: VCount match flag (read-only)
 *     Bit 3: VBlank interrupt enable
 *     Bit 4: HBlank interrupt enable
 *     Bit 5: VCount match interrupt enable
 *     Bits 8-15: VCount trigger value
 *
 *   REG_VCOUNT (0x04000006) - Current scanline (0-227). Read-only.
 *     0-159: Visible scanlines (VDraw period)
 *     160-227: Vertical blank (VBlank period)
 *
 *   REG_BG0CNT - REG_BG3CNT (0x04000008-0x0400000E) - BG layer config.
 *     Bits 0-1:  Priority (0=highest, 3=lowest)
 *     Bits 2-3:  Character base block (which VRAM bank has tile graphics)
 *     Bit 6:     Mosaic enable
 *     Bit 7:     Color mode (0=4bpp/16 palettes, 1=8bpp/1 palette)
 *     Bits 8-12: Screen base block (which VRAM bank has the tilemap)
 *     Bits 14-15: Screen size (256x256, 512x256, 256x512, 512x512)
 *
 *   REG_BG0HOFS/VOFS (0x04000010-0x0400001E) - BG scroll positions.
 *     Write-only. 9 bits each. Controls which part of the tilemap is visible.
 *     The camera system writes these to scroll the overworld.
 *
 *   REG_BLDCNT (0x04000050) - Blend/alpha control.
 *   REG_BLDALPHA (0x04000052) - Blend coefficients.
 *   REG_BLDY (0x04000054) - Brightness fade level.
 *     These control color blending effects: alpha transparency between
 *     layers, and brightness fade to white/black (used for screen transitions).
 *
 *   REG_WIN0H/V, REG_WIN1H/V (0x04000040-0x04000046) - Window dimensions.
 *     Windows are rectangular regions that mask which layers are visible.
 *     Used for text boxes, battle UI, and other overlay effects.
 *
 * ============================================================================
 * THE PROBLEM: WRITING GPU REGISTERS MID-FRAME
 * ============================================================================
 *
 * GPU registers take effect IMMEDIATELY when written. If you write to
 * REG_BG0HOFS (scroll position) while the GPU is drawing scanline 80,
 * scanlines 0-79 use the old value and 80-159 use the new value.
 * Result: the top half of the screen is scrolled differently than the bottom.
 * This is called "screen tearing."
 *
 * Safe times to write GPU registers:
 *   - During VBlank (scanlines 160-227): GPU isn't drawing, writes are safe
 *   - During forced blank (DISPCNT bit 7): Screen is white, GPU is idle
 *   - During HBlank: Safe but very tight timing window (~68 cycles)
 *
 * SOLUTION: This module buffers GPU register writes.
 *
 * When game code calls SetGpuReg():
 *   - If we're in VBlank or forced blank: write directly (safe)
 *   - If we're mid-frame: save the value in sGpuRegBuffer and add the
 *     register offset to sGpuRegWaitingList
 *
 * During VBlank, CopyBufferedValuesToGpuRegs() walks the waiting list
 * and writes all pending values to the actual hardware registers.
 * This is called from VBlankIntr() in main.c.
 *
 * ============================================================================
 */

#include "global.h"

/*
 * GPU register addresses 0x04000000-0x04000056 map to buffer offsets 0x00-0x56.
 * The buffer is 0x60 (96) bytes, covering all GPU registers up to REG_BLDY.
 * Each register is 16 bits (2 bytes), so this covers 48 registers.
 */
#define GPU_REG_BUF_SIZE 0x60

/*
 * GPU_REG_BUF: Read/write the BUFFERED value for a register.
 *   sGpuRegBuffer is a byte array; this macro casts it to u16* for
 *   16-bit register access. The offset is the same as the hardware offset
 *   (e.g., REG_OFFSET_DISPCNT = 0x00, REG_OFFSET_BG0CNT = 0x08).
 *
 * GPU_REG: Read/write the ACTUAL hardware register.
 *   REG_BASE (0x04000000) + offset gives the memory-mapped I/O address.
 *   The 'volatile' qualifier (vu16) tells the compiler this address can
 *   change at any time (hardware can modify it), preventing optimizations
 *   that might cache the value in a CPU register.
 */
#define GPU_REG_BUF(offset) (*(u16 *)(&sGpuRegBuffer[offset]))
#define GPU_REG(offset) (*(vu16 *)(REG_BASE + offset))

/* Sentinel value indicating an empty slot in the waiting list. */
#define EMPTY_SLOT 0xFF

/*
 * sGpuRegBuffer: Shadow copy of all GPU register values.
 *   The game always reads from this buffer (via GetGpuReg), never from
 *   hardware. This ensures consistent reads even if the hardware value
 *   hasn't been updated yet. Think of it as a "write-back cache."
 *
 * sGpuRegWaitingList: Queue of register offsets that need to be flushed.
 *   When SetGpuReg() can't write directly (mid-frame), it appends the
 *   register offset here. CopyBufferedValuesToGpuRegs() processes this
 *   list during VBlank. Entries are EMPTY_SLOT when unused.
 *
 * sGpuRegBufferLocked: Spinlock to prevent race conditions.
 *   SetGpuReg() (main thread) and CopyBufferedValuesToGpuRegs() (VBlank
 *   interrupt) both access the waiting list. This flag prevents the
 *   interrupt from processing the list while the main thread is modifying it.
 *   This is a simple but effective mutex for the single-CPU GBA.
 *
 * sShouldSyncRegIE / sRegIE: Buffered interrupt enable register.
 *   REG_IE (0x04000200) controls which interrupts are enabled.
 *   Like GPU regs, modifying it requires care (disable IME first).
 */
static u8 sGpuRegBuffer[GPU_REG_BUF_SIZE];
static u8 sGpuRegWaitingList[GPU_REG_BUF_SIZE];
static volatile bool8 sGpuRegBufferLocked;
static volatile bool8 sShouldSyncRegIE;
static vu16 sRegIE;

static void CopyBufferedValueToGpuReg(u8 regOffset);
static void SyncRegIE(void);
static void UpdateRegDispstatIntrBits(u16 regIE);

/*
 * InitGpuRegManager - Zero all buffers and reset state.
 * Called once at startup from AgbMain(). Every register starts at 0
 * and the waiting list is empty.
 */
void InitGpuRegManager(void)
{
	s32 i;

	for (i = 0; i < GPU_REG_BUF_SIZE; i++)
    {
		sGpuRegBuffer[i] = 0;
		sGpuRegWaitingList[i] = EMPTY_SLOT;
	}

	sGpuRegBufferLocked = FALSE;
	sShouldSyncRegIE = FALSE;
	sRegIE = 0;
}

/*
 * CopyBufferedValueToGpuReg - Write one buffered value to hardware.
 *
 * Special case for REG_DISPSTAT: We must NOT overwrite the read-only
 * status bits (bits 0-2: VBlank flag, HBlank flag, VCount match).
 * Instead, we only modify the interrupt enable bits (3-4) by:
 *   1. Clear the interrupt bits in the hardware register
 *   2. OR in the buffered interrupt bits
 * This preserves the read-only status bits while updating the writable ones.
 *
 * For all other registers: simple direct write from buffer to hardware.
 */
static void CopyBufferedValueToGpuReg(u8 regOffset)
{
	if (regOffset == REG_OFFSET_DISPSTAT)
    {
		REG_DISPSTAT &= ~(DISPSTAT_HBLANK_INTR | DISPSTAT_VBLANK_INTR);
		REG_DISPSTAT |= GPU_REG_BUF(REG_OFFSET_DISPSTAT);
	}
	else
    {
		GPU_REG(regOffset) = GPU_REG_BUF(regOffset);
	}
}

/*
 * CopyBufferedValuesToGpuRegs - Flush all pending register writes.
 *
 * Called from VBlankIntr() in main.c, during the safe VBlank window.
 * Walks the waiting list and writes each pending register to hardware.
 * Stops at the first EMPTY_SLOT (list is compact, no gaps).
 *
 * Skips processing if sGpuRegBufferLocked is TRUE, meaning the main
 * thread is currently modifying the waiting list. This prevents the
 * interrupt handler from reading a half-updated list. The pending
 * writes will be processed on the next VBlank instead.
 */
void CopyBufferedValuesToGpuRegs(void)
{
	if (!sGpuRegBufferLocked)
    {
		s32 i;

		for (i = 0; i < GPU_REG_BUF_SIZE; i++)
        {
			u8 regOffset = sGpuRegWaitingList[i];
			if (regOffset == EMPTY_SLOT)
				return;
			CopyBufferedValueToGpuReg(regOffset);
			sGpuRegWaitingList[i] = EMPTY_SLOT;
		}
	}
}

/*
 * SetGpuReg - Write a value to a GPU register (buffered).
 *
 * This is THE function all game code uses to modify GPU state.
 * It NEVER writes directly to hardware during the visible frame.
 *
 * Algorithm:
 * 1. Store the value in sGpuRegBuffer (always, for GetGpuReg reads)
 * 2. Check REG_VCOUNT to determine if we're in VBlank:
 *    - Scanlines 161-225: We're in VBlank → write directly to hardware
 *    - DISPCNT forced blank: Screen is off → write directly
 *    - Otherwise: We're mid-frame → queue for next VBlank
 * 3. If queuing: lock the buffer, scan the waiting list to avoid duplicates
 *    (same register already pending), append if not found, unlock.
 *
 * Why check 161-225 instead of 160-227?
 *   Line 160 is the FIRST VBlank line but the interrupt may not have fired
 *   yet. Lines 226-227 are the last VBlank lines where the GPU starts
 *   preparing for the next frame. The safe window is 161-225.
 *
 * The duplicate check prevents the waiting list from filling up if
 * SetGpuReg is called multiple times for the same register in one frame.
 * Only the LAST value written is used (it overwrites in the buffer).
 */
void SetGpuReg(u8 regOffset, u16 value)
{
	if (regOffset < GPU_REG_BUF_SIZE)
	{
		u16 vcount;

		GPU_REG_BUF(regOffset) = value;
		vcount = REG_VCOUNT & 0xFF;

		if ((vcount >= 161 && vcount <= 225)
		 || (REG_DISPCNT & DISPCNT_FORCED_BLANK)) {
			/* Safe to write directly - we're in VBlank or screen is blanked */
			CopyBufferedValueToGpuReg(regOffset);
		} else {
			s32 i;

			/*
			 * Mid-frame: queue the write for next VBlank.
			 * Lock the buffer to prevent VBlankIntr from processing
			 * a half-updated list.
			 */
			sGpuRegBufferLocked = TRUE;

			/* Check if this register is already in the queue */
			for (i = 0; i < GPU_REG_BUF_SIZE && sGpuRegWaitingList[i] != EMPTY_SLOT; i++) {
				if (sGpuRegWaitingList[i] == regOffset) {
					/* Already queued - buffer has the new value, done */
					sGpuRegBufferLocked = FALSE;
					return;
				}
			}

			/* Append to queue */
			sGpuRegWaitingList[i] = regOffset;
			sGpuRegBufferLocked = FALSE;
		}
	}
}

/*
 * GetGpuReg - Read a GPU register value.
 *
 * For most registers: returns the BUFFERED value, not the hardware value.
 * This is important because the hardware value might be from a previous
 * frame if there are pending writes in the queue.
 *
 * Exceptions:
 *   REG_DISPSTAT: Read from hardware because it has read-only status bits
 *     (VBlank flag, HBlank flag) that change every scanline.
 *   REG_VCOUNT: Read from hardware because it's the current scanline counter,
 *     which changes 228 times per frame.
 *
 * These are the only two GPU registers where the hardware value matters
 * more than the buffered value.
 */
u16 GetGpuReg(u8 regOffset)
{
	if (regOffset == REG_OFFSET_DISPSTAT)
		return REG_DISPSTAT;

	if (regOffset == REG_OFFSET_VCOUNT)
		return REG_VCOUNT;

	return GPU_REG_BUF(regOffset);
}

/*
 * SetGpuRegBits / ClearGpuRegBits - Modify individual bits.
 *
 * Read-modify-write pattern using the buffer. These are convenience
 * wrappers that avoid clobbering other bits in a register.
 *
 * Example: SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_BG0_ON)
 *   → Enables BG0 without touching other DISPCNT settings.
 */
void SetGpuRegBits(u8 regOffset, u16 mask)
{
	u16 regValue = GPU_REG_BUF(regOffset);
	SetGpuReg(regOffset, regValue | mask);
}

void ClearGpuRegBits(u8 regOffset, u16 mask)
{
	u16 regValue = GPU_REG_BUF(regOffset);
	SetGpuReg(regOffset, regValue & ~mask);
}

/*
 * SyncRegIE - Apply the buffered interrupt enable value to hardware.
 *
 * REG_IE (0x04000200) controls which interrupts are enabled.
 * Modifying it requires a critical section:
 *   1. Save REG_IME (master interrupt enable)
 *   2. Set REG_IME = 0 (disable ALL interrupts)
 *   3. Write the new REG_IE value
 *   4. Restore REG_IME
 *
 * Without disabling IME first, an interrupt could fire between reading
 * and writing REG_IE, potentially causing a lost interrupt or enabling
 * an interrupt whose handler isn't ready yet.
 *
 * This pattern (disable-modify-restore) is the standard way to
 * perform atomic operations on shared hardware registers on systems
 * without hardware atomic instructions.
 */
static void SyncRegIE(void)
{
	if (sShouldSyncRegIE) {
		u16 temp = REG_IME;
		REG_IME = 0;
		REG_IE = sRegIE;
		REG_IME = temp;
		sShouldSyncRegIE = FALSE;
	}
}

/*
 * EnableInterrupts / DisableInterrupts - Toggle interrupt sources.
 *
 * These modify the buffered sRegIE value, sync it to hardware via
 * SyncRegIE(), then update DISPSTAT's interrupt enable bits.
 *
 * The mask parameter uses INTR_FLAG_* constants:
 *   INTR_FLAG_VBLANK  = 0x0001
 *   INTR_FLAG_HBLANK  = 0x0002
 *   INTR_FLAG_VCOUNT  = 0x0004
 *   INTR_FLAG_TIMER0  = 0x0008
 *   INTR_FLAG_TIMER1  = 0x0010
 *   INTR_FLAG_TIMER2  = 0x0020
 *   INTR_FLAG_TIMER3  = 0x0040
 *   INTR_FLAG_SERIAL  = 0x0080
 *   INTR_FLAG_DMA0-3  = 0x0100-0x0800
 *   INTR_FLAG_KEYPAD  = 0x1000
 *   INTR_FLAG_GAMEPAK = 0x2000
 *
 * Note: VBlank and HBlank interrupts have TWO enable bits each:
 *   1. In REG_IE (bit 0 for VBlank, bit 1 for HBlank)
 *   2. In REG_DISPSTAT (bit 3 for VBlank, bit 4 for HBlank)
 * BOTH must be set for the interrupt to actually fire.
 * UpdateRegDispstatIntrBits() keeps them in sync.
 */
void EnableInterrupts(u16 mask)
{
	sRegIE |= mask;
	sShouldSyncRegIE = TRUE;
	SyncRegIE();
	UpdateRegDispstatIntrBits(sRegIE);
}

void DisableInterrupts(u16 mask)
{
	sRegIE &= ~mask;
	sShouldSyncRegIE = TRUE;
	SyncRegIE();
	UpdateRegDispstatIntrBits(sRegIE);
}

/*
 * UpdateRegDispstatIntrBits - Sync DISPSTAT interrupt enables with REG_IE.
 *
 * VBlank and HBlank interrupts are special: they need to be enabled in
 * BOTH REG_IE (the main interrupt enable register) AND REG_DISPSTAT
 * (the display status/control register). This function reads the
 * VBlank/HBlank enable bits from regIE and mirrors them into DISPSTAT.
 *
 * The comparison with oldValue prevents unnecessary GPU register writes,
 * which would add entries to the waiting list.
 *
 * This is a quirk of GBA hardware design - most interrupts only need
 * REG_IE, but the GPU interrupts need both because REG_DISPSTAT existed
 * before the interrupt controller and has its own enable bits.
 */
static void UpdateRegDispstatIntrBits(u16 regIE)
{
	u16 oldValue = GetGpuReg(REG_OFFSET_DISPSTAT) & (DISPSTAT_HBLANK_INTR | DISPSTAT_VBLANK_INTR);
	u16 newValue = 0;

	if (regIE & INTR_FLAG_VBLANK)
		newValue |= DISPSTAT_VBLANK_INTR;

	if (regIE & INTR_FLAG_HBLANK)
		newValue |= DISPSTAT_HBLANK_INTR;

	if (oldValue != newValue)
		SetGpuReg(REG_OFFSET_DISPSTAT, newValue);
}
