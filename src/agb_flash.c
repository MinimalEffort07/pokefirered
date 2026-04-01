/**
 * @file agb_flash.c
 * @brief Flash Memory Hardware Driver for Game Boy Advance
 *
 * FILE OVERVIEW:
 * This file implements low-level Flash ROM (non-volatile memory) operations for the GBA.
 * Flash memory is the physical chip on the game cartridge that stores save data — it
 * retains data even when the GBA is powered off, unlike RAM which is erased on shutdown.
 *
 * GBA HARDWARE CONTEXT:
 * The GBA cartridge can contain different types of save storage:
 * - SRAM (Static RAM): Simple, battery-backed, small (32KB). Byte-addressable.
 * - Flash ROM: No battery needed, larger (64KB or 128KB). Requires special command
 *   sequences to read/write/erase — you can't just write to it like regular memory.
 * - EEPROM: Serial interface, small (512B or 8KB). Used in some games.
 *
 * Flash memory is accessed through the address range starting at 0x0E000000 (FLASH_BASE).
 * To perform operations like reading the chip ID, erasing sectors, or programming bytes,
 * you must write specific "unlock" byte sequences to magic addresses (0x5555 and 0x2AAA)
 * within the flash address space. This is a hardware protocol defined by the flash chip
 * manufacturer (e.g., Atmel, SST, Macronix, Panasonic).
 *
 * CRITICAL TECHNIQUE — EXECUTING CODE FROM RAM:
 * Several functions in this file copy their own code into RAM buffers and execute from
 * there. This is necessary because:
 * 1. Flash memory and the Game Pak ROM share the same memory bus on the GBA
 * 2. While reading/writing flash, the ROM bus is occupied by flash command sequences
 * 3. If the CPU tried to fetch the next instruction from ROM during a flash operation,
 *    it would get garbage data instead of valid code
 * 4. By copying the critical read/verify function to RAM and executing it there,
 *    the CPU fetches instructions from RAM (a different bus) while the flash bus
 *    is busy with flash operations
 * This is one of the most advanced low-level techniques in GBA programming.
 *
 * ARM/THUMB POINTER NOTE:
 * The GBA's ARM7TDMI processor uses bit 0 of a function pointer to determine the
 * instruction set: bit 0 = 1 means THUMB mode (16-bit instructions), bit 0 = 0 means
 * ARM mode (32-bit instructions). The XOR with 1 (^ 1) and addition of 1 (+ 1) seen
 * throughout this file are used to strip/add this THUMB bit when converting between
 * raw data pointers and function pointers.
 */
#include "gba/gba.h"
#include "gba/flash_internal.h"

/*
 * Static variables for the flash timer subsystem.
 * Flash operations can hang if the chip doesn't respond — the timer
 * provides a timeout mechanism to detect and recover from failures.
 */
static u8 sTimerNum;       /* Which of the 4 GBA hardware timers (0-3) is used for flash timeouts */
static u16 sTimerCount;    /* Countdown value — when it reaches 0, the timeout flag is set */
static vu16 *sTimerReg;    /* Pointer to the timer's hardware register (volatile because hardware can change it) */
static u16 sSavedIme;      /* Saved state of the Interrupt Master Enable register, restored after flash ops */

/*
 * Global function pointers and state for the flash driver.
 * These are set by the flash type detection code (in agb_flash_1m.c, agb_flash_le.c, etc.)
 * to point to the correct implementation for the specific flash chip installed in the cartridge.
 * COMMON_DATA places these in the .bss section (zero-initialized global data).
 */
COMMON_DATA u8 gFlashTimeoutFlag = 0;                                            /* Set to 1 when a flash operation times out */
COMMON_DATA u8 (*PollFlashStatus)(u8 *) = NULL;                                  /* Function pointer: polls whether a flash write/erase completed */
COMMON_DATA u16 (*WaitForFlashWrite)(u8 phase, u8 *addr, u8 lastData) = NULL;    /* Function pointer: busy-waits for a flash write to finish */
COMMON_DATA u16 (*ProgramFlashSector)(u16 sectorNum, void *src) = NULL;          /* Function pointer: writes an entire sector of data to flash */
COMMON_DATA const struct FlashType *gFlash = NULL;                               /* Pointer to a struct describing this cartridge's flash chip properties */
COMMON_DATA u16 (*ProgramFlashByte)(u16 sectorNum, u32 offset, u8 data) = NULL;  /* Function pointer: writes a single byte to flash */
COMMON_DATA u16 gFlashNumRemainingBytes = 0;                                     /* Tracks how many bytes remain in the current write operation */
COMMON_DATA u16 (*EraseFlashChip)() = NULL;                                      /* Function pointer: erases the entire flash chip */
COMMON_DATA u16 (*EraseFlashSector)(u16 sectorNum) = NULL;                       /* Function pointer: erases one sector of flash */
COMMON_DATA const u16 *gFlashMaxTime = NULL;                                     /* Pointer to timeout values for different flash operation phases */

void SetReadFlash1(u16 *dest);

/**
 * FUNCTION: SwitchFlashBank
 *
 * PURPOSE: Switches the active 64KB bank in 128KB (1 Megabit) flash chips.
 *
 * HOW IT WORKS:
 * 128KB flash chips are divided into two 64KB "banks" because the GBA's flash address
 * window at 0x0E000000 is only 64KB wide. To access the second 64KB, you send a
 * bank-switch command and then all subsequent reads/writes target the selected bank.
 *
 * GBA CONTEXT:
 * The FLASH_WRITE macro writes a byte to a specific offset within the flash address space
 * (base address 0x0E000000). The sequence 0xAA->0x5555, 0x55->0x2AAA, 0xB0->0x5555 is
 * the industry-standard unlock sequence for bank switching. After this, writing the bank
 * number (0 or 1) to address 0x0000 completes the switch.
 *
 * @param bankNum — The bank to switch to (0 = first 64KB, 1 = second 64KB)
 */
void SwitchFlashBank(u8 bankNum)
{
    /* Flash command unlock sequence for bank switching */
    FLASH_WRITE(0x5555, 0xAA);  /* Step 1: Write 0xAA to address 0x0E005555 */
    FLASH_WRITE(0x2AAA, 0x55);  /* Step 2: Write 0x55 to address 0x0E002AAA */
    FLASH_WRITE(0x5555, 0xB0);  /* Step 3: Write 0xB0 (bank switch command) to 0x0E005555 */
    FLASH_WRITE(0x0000, bankNum); /* Step 4: Write bank number to 0x0E000000 to select the bank */
}

/*
 * DELAY macro: Burns CPU cycles to give the flash chip time to process commands.
 * Flash chips have internal state machines that need microseconds to respond.
 * The volatile (vu16) qualifier prevents the compiler from optimizing away the loop,
 * since from the compiler's perspective the loop "does nothing useful."
 * 20000 iterations at ~60ns per loop iteration gives roughly 1.2ms of delay.
 */
#define DELAY()                  \
do {                             \
    vu16 i;                      \
    for (i = 20000; i != 0; i--) \
        ;                        \
} while (0)

/**
 * FUNCTION: ReadFlashId
 *
 * PURPOSE: Reads the manufacturer and device ID from the flash chip to identify
 *          which flash chip is installed in the cartridge.
 *
 * HOW IT WORKS:
 * 1. Copies a small flash-reading function into a RAM buffer (to avoid bus conflicts)
 * 2. Sends the "Enter ID Mode" command sequence to the flash chip
 * 3. Waits for the chip to enter ID mode
 * 4. Reads the device ID (address+1) and manufacturer ID (address+0)
 * 5. Sends the "Exit ID Mode" command sequence
 * 6. Returns the combined 16-bit ID (device << 8 | manufacturer)
 *
 * GBA CONTEXT:
 * Different GBA cartridges use different flash chips (Atmel, SST, Macronix, etc.).
 * The game must detect which chip is present at boot to use the correct
 * erase/program algorithms, since each chip has slightly different command
 * sequences and timing requirements.
 *
 * RETURNS: 16-bit flash ID — high byte is device ID, low byte is manufacturer ID
 */
u16 ReadFlashId(void)
{
    u16 flashId;
    u16 readFlash1Buffer[0x20];  /* RAM buffer to hold a copy of the ReadFlash1 function code */
    u8 (*readFlash1)(u8 *);      /* Function pointer to the RAM-copied read function */

    /* Copy the ReadFlash1 function into our RAM buffer so we can call it safely */
    SetReadFlash1(readFlash1Buffer);
    /* Add 1 to set the THUMB bit — ARM7TDMI requires bit 0 = 1 for THUMB function calls */
    readFlash1 = (u8 (*)(u8 *))((s32)readFlash1Buffer + 1);

    /* Enter ID mode: standard flash command sequence */
    FLASH_WRITE(0x5555, 0xAA);  /* Unlock cycle 1 */
    FLASH_WRITE(0x2AAA, 0x55);  /* Unlock cycle 2 */
    FLASH_WRITE(0x5555, 0x90);  /* 0x90 = "Enter Software ID" command */
    DELAY();                     /* Wait for the flash chip to switch to ID mode */

    /* Read the device and manufacturer IDs from the flash chip */
    flashId = readFlash1(FLASH_BASE + 1) << 8;  /* Device ID at offset 1, placed in high byte */
    flashId |= readFlash1(FLASH_BASE);           /* Manufacturer ID at offset 0, placed in low byte */

    /* Leave ID mode: send the "Reset/Exit" command */
    FLASH_WRITE(0x5555, 0xAA);  /* Unlock cycle 1 */
    FLASH_WRITE(0x2AAA, 0x55);  /* Unlock cycle 2 */
    FLASH_WRITE(0x5555, 0xF0);  /* 0xF0 = "Reset" command — returns chip to normal read mode */
    FLASH_WRITE(0x5555, 0xF0);  /* Sent twice for reliability (some chips need it) */
    DELAY();                     /* Wait for the chip to fully exit ID mode */

    return flashId;
}

/**
 * FUNCTION: FlashTimerIntr
 *
 * PURPOSE: Interrupt handler called by a hardware timer to detect flash operation timeouts.
 *
 * HOW IT WORKS:
 * Each time the timer fires this interrupt, it decrements a countdown. When the countdown
 * reaches zero, it sets a global timeout flag that the flash write/erase code checks to
 * know that the operation took too long and should be aborted.
 *
 * GBA CONTEXT:
 * The GBA has 4 hardware timers (Timer 0-3) that count up and fire interrupts.
 * This function is registered as the interrupt handler for whichever timer is
 * assigned to flash operations via SetFlashTimerIntr().
 */
void FlashTimerIntr(void)
{
    if (sTimerCount != 0 && --sTimerCount == 0)
        gFlashTimeoutFlag = 1;  /* Time's up — signal that the flash operation has stalled */
}

/**
 * FUNCTION: SetFlashTimerIntr
 *
 * PURPOSE: Assigns one of the GBA's 4 hardware timers to be used for flash operation timeouts.
 *
 * HOW IT WORKS:
 * Validates the timer number (must be 0-3), then stores which timer to use and
 * sets the caller's interrupt function pointer to our FlashTimerIntr handler.
 *
 * GBA CONTEXT:
 * REG_TMCNT_L(n) is the Timer Counter/Reload register for timer n. On the GBA,
 * timer registers are at addresses 0x04000100, 0x04000104, 0x04000108, 0x0400010C
 * for timers 0-3. Each timer has two 16-bit registers: the counter/reload value
 * (TMCNT_L) and the control register (TMCNT_H).
 *
 * @param timerNum — Which hardware timer to use (0-3)
 * @param intrFunc — Pointer to the caller's interrupt handler variable, which will be
 *                   set to point to FlashTimerIntr
 * RETURNS: 0 on success, 1 if timerNum is invalid (>= 4)
 */
u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void))
{
    if (timerNum >= 4)
        return 1;  /* GBA only has timers 0-3 */

    sTimerNum = timerNum;
    sTimerReg = &REG_TMCNT_L(sTimerNum);  /* Point to this timer's counter register */
    *intrFunc = FlashTimerIntr;            /* Register our interrupt handler with the caller */
    return 0;
}

/**
 * FUNCTION: StartFlashTimer
 *
 * PURPOSE: Starts the hardware timer for a flash operation, with timeout values
 *          appropriate for the current operation phase.
 *
 * HOW IT WORKS:
 * 1. Looks up timeout parameters from a table (3 values per phase: count, reload, control)
 * 2. Saves and disables the Interrupt Master Enable to safely modify interrupt registers
 * 3. Configures the timer with the appropriate countdown values
 * 4. Enables the timer interrupt and re-enables the master interrupt
 *
 * GBA CONTEXT:
 * REG_IME (Interrupt Master Enable at 0x04000208) is a single bit that globally
 * enables/disables all interrupts. Setting it to 0 is essential when modifying
 * interrupt-related registers to prevent race conditions.
 * REG_IE (Interrupt Enable at 0x04000200) controls which individual interrupts are active.
 * REG_IF (Interrupt Flags at 0x04000202) is written to acknowledge/clear pending interrupts.
 * INTR_FLAG_TIMER0 is the bit for Timer 0 interrupts; shifting left by sTimerNum
 * selects the correct bit for the chosen timer.
 *
 * @param phase — The flash operation phase (e.g., erase, program), used to select
 *                appropriate timeout values from gFlashMaxTime table
 */
void StartFlashTimer(u8 phase)
{
    const u16 *maxTime = &gFlashMaxTime[phase * 3];  /* Each phase has 3 values: count, reload, control */
    sSavedIme = REG_IME;        /* Save the current interrupt state so we can restore it later */
    REG_IME = 0;                /* Disable all interrupts while we modify timer/interrupt registers */
    sTimerReg[1] = 0;           /* Stop the timer by clearing its control register (TMCNT_H) */
    REG_IE |= (INTR_FLAG_TIMER0 << sTimerNum);  /* Enable the interrupt for our chosen timer */
    gFlashTimeoutFlag = 0;      /* Clear any previous timeout flag */
    sTimerCount = *maxTime++;   /* Set the countdown value (how many timer interrupts before timeout) */
    *sTimerReg++ = *maxTime++;  /* Set the timer reload value (TMCNT_L) — determines timer frequency */
    *sTimerReg-- = *maxTime++;  /* Set the timer control value (TMCNT_H) — starts the timer */
    REG_IF = (INTR_FLAG_TIMER0 << sTimerNum);  /* Clear any pending interrupt for this timer */
    REG_IME = 1;                /* Re-enable interrupts — the timer is now running */
}

/**
 * FUNCTION: StopFlashTimer
 *
 * PURPOSE: Stops the hardware timer used for flash operation timeouts and restores
 *          the previous interrupt state.
 *
 * HOW IT WORKS:
 * Disables interrupts, clears the timer registers to stop it, disables the timer's
 * interrupt in REG_IE, then restores the original interrupt master enable state.
 */
void StopFlashTimer(void)
{
    REG_IME = 0;                /* Disable all interrupts while modifying registers */
    *sTimerReg++ = 0;           /* Clear timer reload value (TMCNT_L) */
    *sTimerReg-- = 0;           /* Clear timer control value (TMCNT_H) — stops the timer */
    REG_IE &= ~(INTR_FLAG_TIMER0 << sTimerNum);  /* Disable this timer's interrupt */
    REG_IME = sSavedIme;        /* Restore the original interrupt master enable state */
}

/**
 * FUNCTION: ReadFlash1
 *
 * PURPOSE: Reads a single byte from a given address. This is the function that gets
 *          copied to RAM for safe flash reading.
 *
 * HOW IT WORKS:
 * Simply dereferences the pointer. This function is trivially simple on purpose —
 * its entire machine code will be copied to a RAM buffer by SetReadFlash1, and then
 * executed from RAM to avoid ROM/flash bus conflicts.
 *
 * @param addr — The memory address to read from (typically a flash address)
 * RETURNS: The byte at that address
 */
u8 ReadFlash1(u8 *addr)
{
    return *addr;
}

/**
 * FUNCTION: SetReadFlash1
 *
 * PURPOSE: Copies the machine code of ReadFlash1 into a RAM buffer so it can be
 *          executed from RAM during flash operations.
 *
 * HOW IT WORKS:
 * 1. Calculates the size of ReadFlash1's machine code by subtracting its address
 *    from the next function's address (SetReadFlash1 itself, which follows in memory)
 * 2. Copies the raw bytes (as 16-bit halfwords, since THUMB instructions are 16 bits)
 *    from ROM to the provided RAM buffer
 * 3. Sets the global PollFlashStatus function pointer to point to the RAM copy
 *
 * GBA CONTEXT:
 * The XOR with 1 ((s32)src ^ 1) strips the THUMB bit from the function pointer
 * to get the true memory address of the code. THUMB function pointers on ARM7TDMI
 * always have bit 0 set to 1, but we need the actual byte address for copying.
 * The + 1 when setting PollFlashStatus adds the THUMB bit back for calling.
 *
 * @param dest — RAM buffer to copy the function code into (must be large enough)
 */
void SetReadFlash1(u16 *dest)
{
    u16 *src;
    u16 i;

    /* Set PollFlashStatus to point to the RAM copy (+1 for THUMB bit) */
    PollFlashStatus = (u8 (*)(u8 *))((s32)dest + 1);

    /* Get the raw address of ReadFlash1's machine code (strip THUMB bit) */
    src = (u16 *)ReadFlash1;
    src = (u16 *)((s32)src ^ 1);

    /* Calculate how many 16-bit halfwords to copy:
     * (address of SetReadFlash1 - address of ReadFlash1) / 2
     * This works because the linker places these functions contiguously in memory */
    i = ((s32)SetReadFlash1 - (s32)ReadFlash1) >> 1;

    /* Copy the function's machine code, halfword by halfword, to the RAM buffer */
    while (i != 0)
    {
        *dest++ = *src++;
        i--;
    }
}


/**
 * FUNCTION: ReadFlash_Core
 *
 * PURPOSE: Copies data from flash memory to a destination buffer, one byte at a time.
 *          This is the "inner loop" function that gets copied to RAM for execution.
 *
 * HOW IT WORKS:
 * Simple byte-by-byte copy loop. The source pointer is volatile (vu8*) to ensure
 * the compiler reads each byte individually from flash rather than trying to
 * optimize with word-sized reads, which could cause incorrect data on flash hardware.
 *
 * GBA CONTEXT:
 * Flash memory on the GBA can only be reliably read as individual bytes (8-bit).
 * The volatile qualifier (vu8*) prevents the compiler from using 16-bit or 32-bit
 * load instructions (LDRH/LDR), which would read multiple bytes at once and could
 * return incorrect data from flash hardware that only supports byte-width access.
 *
 * @param src — Source address in flash memory (volatile to force byte reads)
 * @param dest — Destination buffer in RAM
 * @param size — Number of bytes to copy
 */
// Using volatile here to make sure the flash memory will ONLY be read as bytes, to prevent any compiler optimizations.
void ReadFlash_Core(vu8 *src, u8 *dest, u32 size)
{
    while (size-- != 0)
    {
        *dest++ = *src++;
    }
}

/**
 * FUNCTION: ReadFlash
 *
 * PURPOSE: High-level function to read data from a specified flash sector into RAM.
 *
 * HOW IT WORKS:
 * 1. Configures the wait state controller for SRAM-speed access (flash uses SRAM bus)
 * 2. For 128KB flash chips, switches to the correct 64KB bank
 * 3. Copies the ReadFlash_Core function to a RAM buffer (to avoid bus conflicts)
 * 4. Calculates the source address within flash based on sector number and offset
 * 5. Calls the RAM-resident copy of ReadFlash_Core to perform the actual read
 *
 * GBA CONTEXT:
 * REG_WAITCNT (0x04000204) controls the timing of memory access. The SRAM wait
 * state bits control how many CPU cycles the bus waits when accessing SRAM/flash.
 * WAITCNT_SRAM_8 sets 8 wait cycles, which is the safest (slowest) setting that
 * works with all flash chips. Using fewer wait cycles with a slow flash chip would
 * cause read errors.
 *
 * Flash sectors are subdivisions of the flash chip (typically 4KB each). The sector
 * address is calculated by shifting the sector number left by gFlash->sector.shift
 * bits, which multiplies by the sector size.
 *
 * @param sectorNum — Which flash sector to read from (0-based)
 * @param offset — Byte offset within the sector to start reading from
 * @param dest — Destination buffer in RAM
 * @param size — Number of bytes to read
 */
void ReadFlash(u16 sectorNum, u32 offset, void *dest, u32 size)
{
    u8 *src;
    u16 i;
    vu16 readFlash_Core_Buffer[0x40];  /* RAM buffer for the ReadFlash_Core function code */
    vu16 *funcSrc;
    vu16 *funcDest;
    void (*readFlash_Core)(vu8 *, u8 *, u32);  /* Function pointer for the RAM-copied function */

    /* Configure wait states for SRAM/flash access — 8 wait cycles is safest */
    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    /* For 128KB (1 Megabit) flash, handle bank switching */
    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        /* Switch to the correct 64KB bank (each bank holds SECTORS_PER_BANK sectors) */
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;  /* Adjust sector number to be within the current bank */
    }

    /* Copy ReadFlash_Core's machine code to the RAM buffer */
    funcSrc = (vu16 *)ReadFlash_Core;
    funcSrc = (vu16 *)((s32)funcSrc ^ 1);  /* Strip THUMB bit to get raw code address */
    funcDest = readFlash_Core_Buffer;

    /* Calculate function size: (ReadFlash - ReadFlash_Core) / 2 halfwords */
    i = ((s32)ReadFlash - (s32)ReadFlash_Core) >> 1;

    while (i != 0)
    {
        *funcDest++ = *funcSrc++;
        i--;
    }

    /* Create a callable function pointer from the RAM buffer (+1 for THUMB bit) */
    readFlash_Core = (void (*)(vu8 *, u8 *, u32))((s32)readFlash_Core_Buffer + 1);

    /* Calculate the source address in flash:
     * FLASH_BASE (0x0E000000) + (sector * sector_size) + offset */
    src = FLASH_BASE + (sectorNum << gFlash->sector.shift) + offset;

    /* Execute the read function from RAM — this avoids ROM/flash bus conflicts */
    readFlash_Core(src, dest, size);
}

/**
 * FUNCTION: VerifyFlashSector_Core
 *
 * PURPOSE: Compares data in flash against a source buffer to verify a write operation
 *          succeeded. This is the inner function copied to RAM for execution.
 *
 * HOW IT WORKS:
 * Byte-by-byte comparison loop. If any byte doesn't match, returns the address of
 * the first mismatch (useful for debugging). Returns 0 if all bytes match.
 *
 * @param src — Source data buffer in RAM (what we expected to write)
 * @param tgt — Target address in flash (what was actually written)
 * @param size — Number of bytes to compare
 * RETURNS: 0 if all bytes match; address of first mismatch otherwise
 */
u32 VerifyFlashSector_Core(u8 *src, u8 *tgt, u32 size)
{
    while (size-- != 0)
    {
        if (*tgt++ != *src++)
            return (u32)(tgt - 1);  /* Return address of the byte that didn't match */
    }

    return 0;  /* All bytes verified successfully */
}

/**
 * FUNCTION: VerifyFlashSector
 *
 * PURPOSE: Verifies that an entire flash sector contains the expected data by
 *          comparing it against a source buffer. Handles bank switching and
 *          executes the comparison from RAM.
 *
 * HOW IT WORKS:
 * Same pattern as ReadFlash: configures wait states, handles bank switching for
 * 128KB chips, copies VerifyFlashSector_Core to RAM, then calls the RAM copy
 * to compare the flash contents against the expected source data.
 *
 * @param sectorNum — Which flash sector to verify
 * @param src — Source data buffer to compare against
 * RETURNS: 0 if the sector data matches src; address of first mismatch otherwise
 */
u32 VerifyFlashSector(u16 sectorNum, u8 *src)
{
    u16 i;
    vu16 verifyFlashSector_Core_Buffer[0x80];  /* RAM buffer for the verify function */
    vu16 *funcSrc;
    vu16 *funcDest;
    u8 *tgt;
    u16 size;
    u32 (*verifyFlashSector_Core)(u8 *, u8 *, u32);

    /* Set SRAM wait state for safe flash access */
    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    /* Handle 128KB flash bank switching */
    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    /* Copy VerifyFlashSector_Core to RAM */
    funcSrc = (vu16 *)VerifyFlashSector_Core;
    funcSrc = (vu16 *)((s32)funcSrc ^ 1);  /* Strip THUMB bit */
    funcDest = verifyFlashSector_Core_Buffer;

    i = ((s32)VerifyFlashSector - (s32)VerifyFlashSector_Core) >> 1;

    while (i != 0)
    {
        *funcDest++ = *funcSrc++;
        i--;
    }

    /* Create callable function pointer from RAM copy */
    verifyFlashSector_Core = (u32 (*)(u8 *, u8 *, u32))((s32)verifyFlashSector_Core_Buffer + 1);

    /* Calculate flash sector address and get sector size from the flash descriptor */
    tgt = FLASH_BASE + (sectorNum << gFlash->sector.shift);
    size = gFlash->sector.size;

    return verifyFlashSector_Core(src, tgt, size); // return 0 if verified.
}

/**
 * FUNCTION: VerifyFlashSectorNBytes
 *
 * PURPOSE: Like VerifyFlashSector, but verifies only the first N bytes of a sector
 *          instead of the entire sector. Used when a sector is only partially written.
 *
 * @param sectorNum — Which flash sector to verify
 * @param src — Source data buffer to compare against
 * @param n — Number of bytes to verify (may be less than full sector size)
 * RETURNS: 0 if the first n bytes match; address of first mismatch otherwise
 */
u32 VerifyFlashSectorNBytes(u16 sectorNum, u8 *src, u32 n)
{
    u16 i;
    vu16 verifyFlashSector_Core_Buffer[0x80];
    vu16 *funcSrc;
    vu16 *funcDest;
    u8 *tgt;
    u32 (*verifyFlashSector_Core)(u8 *, u8 *, u32);

    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    /* Copy verify function to RAM — same pattern as above */
    funcSrc = (vu16 *)VerifyFlashSector_Core;
    funcSrc = (vu16 *)((s32)funcSrc ^ 1);
    funcDest = verifyFlashSector_Core_Buffer;

    i = ((s32)VerifyFlashSector - (s32)VerifyFlashSector_Core) >> 1;

    while (i != 0)
    {
        *funcDest++ = *funcSrc++;
        i--;
    }

    verifyFlashSector_Core = (u32 (*)(u8 *, u8 *, u32))((s32)verifyFlashSector_Core_Buffer + 1);

    tgt = FLASH_BASE + (sectorNum << gFlash->sector.shift);

    return verifyFlashSector_Core(src, tgt, n);  /* Verify only n bytes instead of full sector */
}

/**
 * FUNCTION: ProgramFlashSectorAndVerify
 *
 * PURPOSE: Writes a full sector of data to flash memory and verifies the write succeeded.
 *          Retries up to 3 times on failure.
 *
 * HOW IT WORKS:
 * Flash memory is inherently unreliable — writes can fail due to wear, electrical
 * noise, or timing issues. This function implements a robust write-and-verify loop:
 * 1. Attempt to program (write) the sector
 * 2. If programming itself fails, retry
 * 3. If programming succeeds, verify by reading back and comparing
 * 4. If verification fails, retry the entire program+verify cycle
 * 5. Give up after 3 total attempts
 *
 * GBA CONTEXT:
 * Flash memory has a limited number of write cycles (typically 10,000-100,000 per sector).
 * Each write must first erase the sector (sets all bytes to 0xFF), then program the
 * new data. The ProgramFlashSector function pointer handles both steps internally.
 *
 * @param sectorNum — Which flash sector to write to
 * @param src — Source data buffer to write
 * RETURNS: 0 on success (verified), non-zero on failure (address of mismatch or error code)
 */
u32 ProgramFlashSectorAndVerify(u16 sectorNum, u8 *src)
{
    u8 i;
    u32 result;

    for (i = 0; i < 3; i++) // 3 attempts
    {
        /* Try to write the data to flash */
        result = ProgramFlashSector(sectorNum, src);
        if (result != 0)
            continue;  /* Programming failed — try again */

        /* Programming succeeded — now verify by reading back and comparing */
        result = VerifyFlashSector(sectorNum, src);
        if (result == 0)
            break;  /* Verification passed — data was written correctly */
    }

    return result; // return 0 if verified and programmed.
}

/**
 * FUNCTION: ProgramFlashSectorAndVerifyNBytes
 *
 * PURPOSE: Like ProgramFlashSectorAndVerify, but only verifies the first N bytes.
 *          Used when only a portion of a sector contains meaningful data.
 *
 * HOW IT WORKS:
 * Same retry logic as ProgramFlashSectorAndVerify, but uses VerifyFlashSectorNBytes
 * instead of VerifyFlashSector to only check the bytes that were intentionally written.
 * Note: The entire sector is still programmed (flash writes are sector-granular),
 * but only n bytes of the verification matter.
 *
 * @param sectorNum — Which flash sector to write to
 * @param dataSrc — Source data buffer to write
 * @param n — Number of bytes to verify after writing
 * RETURNS: 0 on success, non-zero on failure
 */
u32 ProgramFlashSectorAndVerifyNBytes(u16 sectorNum, void *dataSrc, u32 n)
{
    u8 i;
    u32 result;

    for (i = 0; i < 3; i++)
    {
        result = ProgramFlashSector(sectorNum, dataSrc);
        if (result != 0)
            continue;

        result = VerifyFlashSectorNBytes(sectorNum, dataSrc, n);
        if (result == 0)
            break;
    }

    return result;
}
