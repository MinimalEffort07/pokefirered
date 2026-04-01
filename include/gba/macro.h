/*
 * macro.h - CPU and DMA Memory Operation Macros
 *
 * ============================================================================
 * MEMORY COPY/FILL ON THE GBA
 * ============================================================================
 *
 * The GBA has TWO fast memory copy methods:
 *
 * 1. CPU-BASED (CpuSet / CpuFastSet):
 *    Uses BIOS system calls (SWI instructions) that execute optimized
 *    ARM assembly loops in the BIOS ROM. The CPU does the copying.
 *    Advantages: Works anytime, no register setup
 *    Disadvantages: Blocks the CPU during the copy
 *
 * 2. DMA-BASED (DMA channels 0-3):
 *    Hardware copies memory WITHOUT using the CPU. You write source,
 *    destination, and size to DMA registers, and the DMA controller
 *    handles the rest. The CPU is free to do other work.
 *    Advantages: CPU is free, very fast for large copies
 *    Disadvantages: More complex setup, can't be used during certain
 *    operations (DMA has priority over CPU and can block it)
 *
 * WHEN TO USE WHICH:
 *   - Small copies (< 256 bytes): CpuSet/CpuFastSet (simpler setup)
 *   - Large copies (VRAM, tilemap updates): DMA (faster, frees CPU)
 *   - During interrupts: CPU only (DMA can't be safely started in ISRs)
 *   - Audio streaming: DMA1/2 with special timing (automatic FIFO refill)
 *
 * ============================================================================
 * THE CpuSet BIOS CALL (SWI 0x0B)
 * ============================================================================
 *
 * CpuSet is a BIOS function for copying or filling memory:
 *   void CpuSet(const void *src, void *dest, u32 control);
 *
 * The 'control' parameter is a 32-bit value:
 *   Bits 0-20:  Number of units to transfer (halfwords or words)
 *   Bit 24:     Source fixed mode (0 = copy, 1 = fill)
 *               Fill mode: reads the same source address for every write
 *               Copy mode: source increments along with destination
 *   Bit 26:     Transfer size (0 = 16-bit halfwords, 1 = 32-bit words)
 *
 * CpuFastSet is similar (SWI 0x0C) but always uses 32-bit transfers
 * and requires both src and dest to be 4-byte aligned. It's faster
 * because it uses STMIA (Store Multiple) to write 8 words at once.
 *
 * ============================================================================
 */

#ifndef GUARD_GBA_MACRO_H
#define GUARD_GBA_MACRO_H

/*
 * CPU_FILL: Fill a memory region with a single value using CpuSet.
 *
 * HOW IT WORKS:
 * 1. Creates a volatile local variable 'tmp' with the fill value.
 *    Volatile prevents the compiler from optimizing it into a register
 *    (CpuSet needs a memory ADDRESS to read from, not a register value).
 * 2. Calls CpuSet with CPU_SET_SRC_FIXED flag, which means "read from
 *    the same source address for every write" = fill mode.
 * 3. The transfer count is calculated as: size_in_bytes / bytes_per_unit.
 *    For 16-bit: size / 2. For 32-bit: size / 4.
 *    Masked with 0x1FFFFF (21 bits = max 2 million units).
 *
 * @param value - The value to fill with
 * @param dest  - Destination address
 * @param size  - Size in BYTES to fill
 * @param bit   - Transfer width: 16 or 32
 */
#define CPU_FILL(value, dest, size, bit)                                          \
{                                                                                 \
    vu##bit tmp = (vu##bit)(value);                                               \
    CpuSet((void *)&tmp,                                                          \
           dest,                                                                  \
           CPU_SET_##bit##BIT | CPU_SET_SRC_FIXED | ((size)/(bit/8) & 0x1FFFFF)); \
}

/* Convenience wrappers for 16-bit and 32-bit fills */
#define CpuFill16(value, dest, size) CPU_FILL(value, dest, size, 16)
#define CpuFill32(value, dest, size) CPU_FILL(value, dest, size, 32)

/*
 * CPU_COPY: Copy memory from src to dest using CpuSet (no SRC_FIXED flag).
 * Both source and destination increment with each transfer unit.
 *
 * @param src  - Source address (data to copy from)
 * @param dest - Destination address (where to copy to)
 * @param size - Size in BYTES to copy
 * @param bit  - Transfer width: 16 or 32
 */
#define CPU_COPY(src, dest, size, bit) CpuSet(src, dest, CPU_SET_##bit##BIT | ((size)/(bit/8) & 0x1FFFFF))

#define CpuCopy16(src, dest, size) CPU_COPY(src, dest, size, 16)
#define CpuCopy32(src, dest, size) CPU_COPY(src, dest, size, 32)

/*
 * CpuFastFill: Fill memory using CpuFastSet (SWI 0x0C).
 * CpuFastSet is faster than CpuSet because it uses block transfers
 * (LDMIA/STMIA: load/store 8 registers = 32 bytes at once).
 * REQUIRES: dest must be 4-byte aligned, size must be multiple of 32 bytes.
 *
 * CpuFastFill16: Fill with a 16-bit value. The value is duplicated into
 * both halves of a 32-bit word (e.g., 0xABCD becomes 0xABCDABCD).
 *
 * CpuFastFill8: Fill with an 8-bit value. The value is replicated into
 * all 4 bytes of a 32-bit word (e.g., 0x42 becomes 0x42424242).
 */
#define CpuFastFill(value, dest, size)                               \
{                                                                    \
    vu32 tmp = (vu32)(value);                                        \
    CpuFastSet((void *)&tmp,                                         \
               dest,                                                 \
               CPU_FAST_SET_SRC_FIXED | ((size)/(32/8) & 0x1FFFFF)); \
}

#define CpuFastFill16(value, dest, size) CpuFastFill(((value) << 16) | (value), (dest), (size))

#define CpuFastFill8(value, dest, size) CpuFastFill(((value) << 24) | ((value) << 16) | ((value) << 8) | (value), (dest), (size))

/*
 * CpuFastCopy: Copy memory using CpuFastSet (no SRC_FIXED flag).
 * Faster than CpuCopy32 for large, aligned transfers.
 */
#define CpuFastCopy(src, dest, size) CpuFastSet(src, dest, ((size)/(32/8) & 0x1FFFFF))

/*
 * ============================================================================
 * DMA MACROS
 * ============================================================================
 *
 * These macros set up DMA transfers by writing to the DMA hardware registers.
 * Each DMA channel has 3 registers:
 *   dmaRegs[0] = Source Address (DMA_SAD)
 *   dmaRegs[1] = Destination Address (DMA_DAD)
 *   dmaRegs[2] = Control (DMA_CNT: lower 16 = count, upper 16 = flags)
 *
 * The final "dmaRegs[2];" line (with no assignment) is a READ-BACK.
 * Reading the control register after writing forces the CPU to wait until
 * the DMA write has actually reached the hardware. Without this, the CPU
 * might continue executing before the DMA is truly configured (pipeline
 * hazard on ARM7TDMI with memory-mapped I/O).
 *
 * ============================================================================
 */

/*
 * DmaSet: Low-level macro that writes src, dest, and control to DMA registers.
 * The 'control' parameter encodes both the transfer count (low 16 bits)
 * and control flags (high 16 bits) in a single 32-bit write.
 */
#define DmaSet(dmaNum, src, dest, control)        \
{                                                 \
    vu32 *dmaRegs = (vu32 *)REG_ADDR_DMA##dmaNum; \
    dmaRegs[0] = (vu32)(src);                     \
    dmaRegs[1] = (vu32)(dest);                    \
    dmaRegs[2] = (vu32)(control);                 \
    dmaRegs[2];  /* Read-back: ensures write completes before continuing */ \
}

/*
 * DMA_FILL: Fill a memory region with a value using DMA.
 * Similar to CPU_FILL but uses DMA hardware instead of the CPU.
 * DMA_SRC_FIXED means the DMA reads the same source address repeatedly.
 *
 * The control value packs flags into the upper 16 bits and count into lower 16:
 *   (flags << 16) | count
 * Flags: DMA_ENABLE + DMA_START_NOW + bit width + DMA_SRC_FIXED + DMA_DEST_INC
 */
#define DMA_FILL(dmaNum, value, dest, size, bit)                                              \
{                                                                                             \
    vu##bit tmp = (vu##bit)(value);                                                           \
    DmaSet(dmaNum,                                                                            \
           &tmp,                                                                              \
           dest,                                                                              \
           (DMA_ENABLE | DMA_START_NOW | DMA_##bit##BIT | DMA_SRC_FIXED | DMA_DEST_INC) << 16 \
         | ((size)/(bit/8)));                                                                 \
}

#define DmaFill16(dmaNum, value, dest, size) DMA_FILL(dmaNum, value, dest, size, 16)
#define DmaFill32(dmaNum, value, dest, size) DMA_FILL(dmaNum, value, dest, size, 32)

/*
 * DMA_CLEAR: Fill memory with zero using DMA. Convenience wrapper around DMA_FILL.
 * The extra local variables force the compiler to generate specific code patterns
 * that match the original ROM binary (needed for decompilation accuracy).
 */
#define DMA_CLEAR(dmaNum, dest, size, bit)  \
{                                           \
    vu##bit *_dest = (vu##bit *)(dest);     \
    u32 _size = size;                       \
    DmaFill##bit(dmaNum, 0, _dest, _size);  \
}

#define DmaClear16(dmaNum, dest, size) DMA_CLEAR(dmaNum, dest, size, 16)
#define DmaClear32(dmaNum, dest, size) DMA_CLEAR(dmaNum, dest, size, 32)

/*
 * DMA_COPY: Copy memory from src to dest using DMA.
 * Both source and destination addresses increment (normal copy behavior).
 */
#define DMA_COPY(dmaNum, src, dest, size, bit)                                              \
    DmaSet(dmaNum,                                                                          \
           src,                                                                             \
           dest,                                                                            \
           (DMA_ENABLE | DMA_START_NOW | DMA_##bit##BIT | DMA_SRC_INC | DMA_DEST_INC) << 16 \
         | ((size)/(bit/8)))

#define DmaCopy16(dmaNum, src, dest, size) DMA_COPY(dmaNum, src, dest, size, 16)
#define DmaCopy32(dmaNum, src, dest, size) DMA_COPY(dmaNum, src, dest, size, 32)

/*
 * DmaStop: Halt an active DMA channel.
 *
 * Stopping DMA requires a specific sequence to avoid hardware glitches:
 * 1. First, clear the start timing and repeat flags (but keep DMA enabled).
 *    This prevents the DMA from restarting on the next trigger.
 * 2. Then, clear the DMA_ENABLE bit to actually stop the channel.
 * 3. Read-back to ensure the stop takes effect.
 *
 * Note: Uses 16-bit register access (vu16*) because we only need to
 * modify the control register's upper half (CNT_H at offset 5 in
 * halfword addressing = offset 10 in byte addressing = 0xBA-0xBB).
 */
#define DmaStop(dmaNum)                                         \
{                                                               \
    vu16 *dmaRegs = (vu16 *)REG_ADDR_DMA##dmaNum;               \
    dmaRegs[5] &= ~(DMA_START_MASK | DMA_DREQ_ON | DMA_REPEAT); \
    dmaRegs[5] &= ~DMA_ENABLE;                                  \
    dmaRegs[5];  /* Read-back */                                \
}

/*
 * DmaCopyLarge: Copy more than 64 KB using DMA.
 *
 * A single DMA transfer is limited to 16-bit count = max 65,536 units
 * (64 KB in 16-bit mode, 256 KB in 32-bit mode). For larger copies,
 * this macro breaks the transfer into chunks of 'block' bytes.
 *
 * It copies 'block' bytes at a time, advancing src and dest pointers,
 * until the remaining size fits in one final transfer.
 */
#define DmaCopyLarge(dmaNum, src, dest, size, block, bit) \
{                                                         \
    const void *_src = src;                               \
    void *_dest = dest;                                   \
    u32 _size = size;                                     \
    while (1)                                             \
    {                                                     \
        DmaCopy##bit(dmaNum, _src, _dest, (block));       \
        _src += (block);                                  \
        _dest += (block);                                 \
        _size -= (block);                                 \
        if (_size <= (block))                             \
        {                                                 \
            DmaCopy##bit(dmaNum, _src, _dest, _size);     \
            break;                                        \
        }                                                 \
    }                                                     \
}

/* DmaClearLarge: Zero-fill more than 64 KB using DMA (chunked). */
#define DmaClearLarge(dmaNum, dest, size, block, bit) \
{                                                           \
    void *_dest = dest;                                     \
    u32 _size = size;                                       \
    while (1)                                               \
    {                                                       \
        DmaFill##bit(dmaNum, 0, _dest, (block));       \
        _dest += (block);                                   \
        _size -= (block);                                   \
        if (_size <= (block))                               \
        {                                                   \
            DmaFill##bit(dmaNum, 0, _dest, _size);     \
            break;                                          \
        }                                                   \
    }                                                       \
}

/* DmaFillLarge: Value-fill more than 64 KB using DMA (chunked). */
#define DmaFillLarge(dmaNum, value, dest, size, block, bit) \
{                                                           \
    void *_dest = (void *)dest;                             \
    u32 _size = size;                                       \
    while (1)                                               \
    {                                                       \
        DmaFill##bit(dmaNum, value, _dest, (block));        \
        _dest += (block);                                   \
        _size -= (block);                                   \
        if (_size <= (block))                               \
        {                                                   \
            DmaFill##bit(dmaNum, value, _dest, _size);      \
            break;                                          \
        }                                                   \
    }                                                       \
}

/* Convenience wrappers specifying 16-bit or 32-bit transfer width */
#define DmaCopyLarge16(dmaNum, src, dest, size, block) DmaCopyLarge(dmaNum, src, dest, size, block, 16)
#define DmaCopyLarge32(dmaNum, src, dest, size, block) DmaCopyLarge(dmaNum, src, dest, size, block, 32)
#define DmaFillLarge16(dmaNum, value, dest, size, block) DmaFillLarge(dmaNum, value, dest, size, block, 16)
#define DmaFillLarge32(dmaNum, value, dest, size, block) DmaFillLarge(dmaNum, value, dest, size, block, 32)
#define DmaClearLarge16(dmaNum, dest, size, block) DmaClearLarge(dmaNum, dest, size, block, 16)
#define DmaClearLarge32(dmaNum, dest, size, block) DmaClearLarge(dmaNum, dest, size, block, 32)

/*
 * "Defvars" variants: These create local variable copies of the arguments
 * before passing them to the DMA macros. This is a decompilation artifact -
 * the original compiled code used local variables, so these macros reproduce
 * that pattern to match the ROM binary exactly.
 */
#define DmaCopyDefvars(dmaNum, src, dest, size, bit) \
{                                                    \
    const void *_src = src;                          \
    void *_dest = dest;                              \
    u32 _size = size;                                \
    DmaCopy##bit(dmaNum, _src, _dest, _size);        \
}

#define DmaCopy16Defvars(dmaNum, src, dest, size) DmaCopyDefvars(dmaNum, src, dest, size, 16)
#define DmaCopy32Defvars(dmaNum, src, dest, size) DmaCopyDefvars(dmaNum, src, dest, size, 32)

#define DmaFillDefvars(dmaNum, value, dest, size, bit) \
{                                                      \
    void *_dest = (void *)dest;                        \
    u32 _size = size;                                  \
    DmaFill##bit(dmaNum, value, _dest, _size);         \
}

#define DmaFill16Defvars(dmaNum, value, dest, size) DmaFillDefvars(dmaNum, value, dest, size, 16)
#define DmaFill32Defvars(dmaNum, value, dest, size) DmaFillDefvars(dmaNum, value, dest, size, 32)

#define DmaClearDefvars(dmaNum, dest, size, bit) \
{                                                \
    void *_dest = dest;                          \
    u32 _size = size;                            \
    DmaClear##bit(dmaNum, _dest, _size);         \
}

#define DmaClear16Defvars(dmaNum, dest, size) DmaClearDefvars(dmaNum, dest, size, 16)
#define DmaClear32Defvars(dmaNum, dest, size) DmaClearDefvars(dmaNum, dest, size, 32)

/*
 * IntrEnable: Safely enable specific interrupts.
 *
 * Modifying REG_IE while interrupts are enabled could cause a race condition
 * (an interrupt could fire while we're mid-write, seeing a half-updated IE).
 * So we:
 * 1. Save current IME state
 * 2. Disable ALL interrupts (IME = 0)
 * 3. Set the desired IE bits
 * 4. Restore IME to its previous state
 *
 * @param flags - Bitmask of INTR_FLAG_* values to enable
 */
#define IntrEnable(flags)                                       \
{                                                               \
    u16 imeTemp;                                                \
                                                                \
    imeTemp = REG_IME;                                          \
    REG_IME = 0;                                                \
    REG_IE |= flags;                                            \
    REG_IME = imeTemp;                                          \
}

#endif // GUARD_GBA_MACRO_H
