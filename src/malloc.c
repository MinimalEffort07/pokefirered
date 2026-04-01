/*
 * malloc.c - Custom Dynamic Memory Allocator (Heap Manager)
 *
 * ============================================================================
 * WHY A CUSTOM ALLOCATOR?
 * ============================================================================
 *
 * The GBA has NO operating system and NO standard C library heap.
 * There is no malloc() or free() provided by the system. This file
 * implements a custom allocator specifically designed for the GBA's
 * constraints:
 *
 *   - Limited memory (256 KB EWRAM for EVERYTHING)
 *   - No virtual memory (no swap, no page faults)
 *   - No memory protection (writing past your buffer corrupts other data)
 *   - Must be deterministic (no system calls that could vary in timing)
 *
 * HOW THE HEAP WORKS:
 *
 * The heap is a contiguous block of EWRAM (gHeap, HEAP_SIZE bytes).
 * It's managed as a doubly-linked list of "memory blocks".
 *
 *   +--------+------+--------+------+--------+------+--------+
 *   | Header | Data | Header | Data | Header | Data | Free   |
 *   | (16B)  | (var)| (16B)  | (var)| (16B)  | (var)| Space  |
 *   +--------+------+--------+------+--------+------+--------+
 *   ^                                                ^
 *   sHeapStart                                       End of heap
 *
 * Each memory block has a 16-byte header (struct MemBlock) followed
 * by the actual data. The header contains:
 *   - flag: Is this block allocated (TRUE) or free (FALSE)?
 *   - magic_number: 0xA3A3 to verify block integrity
 *   - size: Number of data bytes (NOT including the header)
 *   - prev/next: Pointers to adjacent blocks (doubly-linked list)
 *
 * The list is CIRCULAR: the last block's 'next' points back to the
 * first block (sHeapStart), and the first block's 'prev' points to
 * sHeapStart. This simplifies boundary checks.
 *
 * ALLOCATION (AllocInternal):
 *   Walks the block list looking for a free block big enough.
 *   If the found block is much larger than needed, it's SPLIT into
 *   two blocks: one allocated (exact size) and one free (remainder).
 *
 * FREEING (FreeInternal):
 *   Marks the block as free, then MERGES it with adjacent free blocks.
 *   This prevents fragmentation (many small free blocks that can't be
 *   used for larger allocations).
 *
 * ALIGNMENT:
 *   All allocations are rounded up to a multiple of 4 bytes.
 *   The ARM7TDMI CPU requires 32-bit aligned access for correctness
 *   and performance. Misaligned access gives wrong results.
 *
 * ============================================================================
 */

#include "global.h"

/* Pointers used by the allocator internally */
static void *sHeapStart;
static u32 sHeapSize;

static EWRAM_DATA struct MemBlock *head = NULL;
static EWRAM_DATA struct MemBlock *pos = NULL;
static EWRAM_DATA struct MemBlock *splitBlock = NULL;

/*
 * MALLOC_SYSTEM_ID: Magic number written into every block header.
 * If a block's magic_number doesn't match 0xA3A3, the heap is corrupted
 * (buffer overflow, double free, write-after-free, etc.)
 * The assertions in FreeInternal check this for debugging.
 */
#define MALLOC_SYSTEM_ID 0xA3A3

/*
 * MemBlock: Header structure prepended to every allocation.
 * Size: 16 bytes (flag=2, magic=2, size=4, prev=4, next=4).
 *
 * The data[0] field is a GCC extension (zero-length array).
 * It doesn't occupy space in the struct itself, but lets us write
 * block->data to get a pointer to the memory immediately after the header.
 * This is where the user's allocated memory starts.
 */
struct MemBlock {
    bool16 flag;              /* TRUE = allocated, FALSE = free */
    u16 magic_number;         /* Should be MALLOC_SYSTEM_ID (0xA3A3) */
    u32 size;                 /* Size of the DATA portion (excluding this header) */
    struct MemBlock *prev;    /* Previous block in the list (sHeapStart if first) */
    struct MemBlock *next;    /* Next block in the list (sHeapStart if last) */
    u8 data[0];              /* Start of the user's allocated memory (zero-length array) */
};

/**
 * FUNCTION: PutMemBlockHeader
 *
 * PURPOSE: Initialize a memory block header with given values.
 *
 * HOW IT WORKS:
 * Writes the header fields into the memory at 'block'. The block starts
 * as free (flag=FALSE). The magic_number is set to MALLOC_SYSTEM_ID
 * so we can later verify the block hasn't been corrupted.
 *
 * @param block — Address where the block header should be written
 * @param prev  — Pointer to the previous block in the list
 * @param next  — Pointer to the next block in the list
 * @param size  — Size of the DATA area (bytes after the header)
 */
void PutMemBlockHeader(void *block, struct MemBlock *prev, struct MemBlock *next, u32 size)
{
    struct MemBlock *header = (struct MemBlock *)block;

    header->flag = FALSE;
    header->magic_number = MALLOC_SYSTEM_ID;
    header->size = size;
    header->prev = prev;
    header->next = next;
}

/**
 * FUNCTION: PutFirstMemBlockHeader
 *
 * PURPOSE: Initialize the heap with a single free block spanning the entire space.
 *
 * HOW IT WORKS:
 * Creates one block that covers the whole heap. Both prev and next point
 * to itself (circular list with one element). The data size is the total
 * heap size minus the header size (16 bytes), since the header lives
 * inside the heap space.
 *
 * @param block — Start of the heap memory
 * @param size  — Total heap size in bytes
 */
void PutFirstMemBlockHeader(void *block, u32 size)
{
    PutMemBlockHeader(block, (struct MemBlock *)block, (struct MemBlock *)block, size - sizeof(struct MemBlock));
}

/**
 * FUNCTION: AllocInternal
 *
 * PURPOSE: Allocate a block of memory from the heap.
 *
 * HOW IT WORKS:
 * 1. Round up the requested size to a multiple of 4 bytes (ARM alignment).
 * 2. Walk the block list looking for a free block that's large enough.
 * 3. If the free block is MUCH bigger than needed (by more than 2 headers):
 *    Split it into an allocated block of the exact size and a free block
 *    containing the remainder.
 * 4. If the free block is only slightly bigger: just use the whole thing
 *    (wasting a few bytes is better than a tiny unusable free block).
 * 5. If no suitable block is found: assert failure (out of memory).
 *
 * @param heapStart — Start of the heap (for circular list boundary)
 * @param size      — Number of bytes to allocate
 *
 * RETURNS: Pointer to the allocated memory (NOT the header), or NULL if OOM
 */
void *AllocInternal(void *heapStart, u32 size)
{
    u32 foundBlockSize;

    head = (struct MemBlock *)heapStart;
    pos = head;

    /*
     * Round size up to the next multiple of 4 bytes.
     * ARM7TDMI requires 4-byte alignment for 32-bit operations.
     * Example: size=5 -> size=8, size=12 stays 12.
     */
    if (size & 3)
        size = 4 * ((size / 4) + 1);

    for (;;) {
        if (!pos->flag) {  /* Is this block free? */
            foundBlockSize = pos->size;

            if (foundBlockSize >= size) {
                if (foundBlockSize - size < 2 * sizeof(struct MemBlock)) {
                    /*
                     * The block is big enough but NOT big enough to split.
                     * (Splitting would leave a remainder smaller than 2 headers,
                     *  which would be unusable.) Just allocate the whole block.
                     * The wasted space is called "internal fragmentation".
                     */
                    pos->flag = TRUE;
                    return pos->data;
                } else {
                    /*
                     * The block is significantly bigger than needed. SPLIT it.
                     *
                     * Before:  [Header(pos) | -------- foundBlockSize --------]
                     * After:   [Header(pos) | size | Header(split) | remainder]
                     *
                     * The split block is placed at pos->data + size
                     * (right after the allocated data).
                     */
                    int splitBlockSize = foundBlockSize;
                    splitBlockSize -= sizeof(struct MemBlock); /* Room for the new header */
                    splitBlockSize -= size;                     /* Remainder data size */

                    splitBlock = (struct MemBlock *)(pos->data + size);

                    pos->flag = TRUE;   /* Mark this block as allocated */
                    pos->size = size;   /* Shrink to exact requested size */

                    /* Create a new free block for the remainder */
                    PutMemBlockHeader(splitBlock, pos, pos->next, splitBlockSize);

                    /* Update the linked list to include the new split block */
                    pos->next = splitBlock;
                    if (splitBlock->next != head)
                        splitBlock->next->prev = splitBlock;

                    return pos->data;
                }
            }
        }

        /* If we've looped back to the start, the heap is full */
        if (pos->next == head)
        {
            AGB_ASSERT_EX(0, ABSPATH("gflib/malloc.c"), 174);
            return NULL;
        }

        pos = pos->next;
    }
}

/**
 * FUNCTION: FreeInternal
 *
 * PURPOSE: Free a previously allocated block of memory.
 *
 * HOW IT WORKS:
 * 1. Gets the MemBlock header (located just BEFORE the user's data pointer).
 * 2. Marks it as free (flag = FALSE).
 * 3. Tries to MERGE with adjacent free blocks:
 *    - If the NEXT block is free, absorb it (combine sizes, remove from list).
 *    - If the PREVIOUS block is free, let it absorb us.
 *    Merging prevents fragmentation (many small free blocks that together
 *    would be big enough, but individually are too small).
 *
 * @param heapStart — Start of the heap
 * @param p         — Pointer to the memory to free (NOT the header)
 */
void FreeInternal(void *heapStart, void *p)
{
    AGB_ASSERT_EX(p != NULL, ABSPATH("gflib/malloc.c"), 195);

    if (p) {
        struct MemBlock *head = (struct MemBlock *)heapStart;

        /*
         * Get the header: it's sizeof(MemBlock) bytes BEFORE the data pointer.
         * When AllocInternal returned pos->data, the header is at pos.
         * So: header = data_ptr - sizeof(MemBlock)
         */
        struct MemBlock *pos = (struct MemBlock *)((u8 *)p - sizeof(struct MemBlock));

        /* Sanity checks (debug only) */
        AGB_ASSERT_EX(pos->magic_number == MALLOC_SYSTEM_ID, ABSPATH("gflib/malloc.c"), 204);
        AGB_ASSERT_EX(pos->flag == TRUE, ABSPATH("gflib/malloc.c"), 205);

        pos->flag = FALSE;  /* Mark as free */

        /* Try to merge with the NEXT block if it's also free */
        if (pos->next != head) {
            if (!pos->next->flag) {
                AGB_ASSERT_EX(pos->next->magic_number == MALLOC_SYSTEM_ID, ABSPATH("gflib/malloc.c"), 211);
                /* Absorb the next block: add its header size + data size to ours */
                pos->size += sizeof(struct MemBlock) + pos->next->size;
                pos->next->magic_number = 0;  /* Invalidate the absorbed header */
                pos->next = pos->next->next;   /* Skip over it in the list */
                if (pos->next != head)
                    pos->next->prev = pos;
            }
        }

        /* Try to merge with the PREVIOUS block if it's also free */
        if (pos != head) {
            if (!pos->prev->flag) {
                AGB_ASSERT_EX(pos->prev->magic_number == MALLOC_SYSTEM_ID, ABSPATH("gflib/malloc.c"), 228);
                /* Let the previous block absorb us */
                pos->prev->next = pos->next;
                if (pos->next != head)
                    pos->next->prev = pos->prev;
                pos->magic_number = 0;
                pos->prev->size += sizeof(struct MemBlock) + pos->size;
            }
        }
    }
}

/**
 * FUNCTION: AllocZeroedInternal
 *
 * PURPOSE: Allocate memory and fill it with zeros.
 *
 * HOW IT WORKS:
 * Calls AllocInternal to get the memory, then uses CpuFill32 (a BIOS call)
 * to zero it out. This is the GBA equivalent of calloc().
 *
 * @param heapStart — Start of the heap
 * @param size      — Number of bytes to allocate and zero
 *
 * RETURNS: Pointer to zeroed memory, or NULL if allocation failed
 */
void *AllocZeroedInternal(void *heapStart, u32 size)
{
    void *mem = AllocInternal(heapStart, size);

    if (mem != NULL) {
        /* Round up to multiple of 4 (CpuFill32 requires aligned size) */
        if (size & 3)
            size = 4 * ((size / 4) + 1);

        CpuFill32(0, mem, size);
    }

    return mem;
}

/**
 * FUNCTION: CheckMemBlockInternal
 *
 * PURPOSE: Verify that a heap allocation is not corrupted.
 *
 * HOW IT WORKS:
 * Checks several invariants that should be true for a valid memory block:
 * 1. Magic number is correct (header hasn't been overwritten)
 * 2. Next block's magic number is correct
 * 3. Next block's prev pointer points back to us (list consistency)
 * 4. Previous block's magic number is correct
 * 5. Previous block's next pointer points to us
 * 6. Next block starts exactly where our data ends (no gaps/overlaps)
 *
 * If ANY check fails, the heap is corrupted (likely buffer overflow).
 *
 * @param heapStart — Start of the heap
 * @param pointer   — Pointer to check (the data pointer, not the header)
 *
 * RETURNS: TRUE if the block appears valid, FALSE if corrupted
 */
bool32 CheckMemBlockInternal(void *heapStart, void *pointer)
{
    struct MemBlock *head = (struct MemBlock *)heapStart;
    struct MemBlock *block = (struct MemBlock *)((u8 *)pointer - sizeof(struct MemBlock));

    if (block->magic_number != MALLOC_SYSTEM_ID)
        return FALSE;

    if (block->next->magic_number != MALLOC_SYSTEM_ID)
        return FALSE;

    if (block->next != head && block->next->prev != block)
        return FALSE;

    if (block->prev->magic_number != MALLOC_SYSTEM_ID)
        return FALSE;

    if (block->prev != head && block->prev->next != block)
        return FALSE;

    /* The next block should start immediately after our data */
    if (block->next != head && block->next != (struct MemBlock *)(block->data + block->size))
        return FALSE;

    return TRUE;
}

/**
 * FUNCTION: InitHeap
 *
 * PURPOSE: Initialize the dynamic memory allocator.
 *
 * HOW IT WORKS:
 * Records the heap start address and size, then creates a single free
 * block spanning the entire heap. All subsequent Alloc/Free calls
 * operate on this heap.
 *
 * GBA CONTEXT:
 * Called from AgbMain at startup. The heap lives in EWRAM (gHeap).
 * HEAP_SIZE is defined elsewhere and determines how much EWRAM is
 * available for dynamic allocation vs. static EWRAM_DATA variables.
 *
 * @param heapStart — Start address of the heap memory
 * @param heapSize  — Total size of the heap in bytes
 */
void InitHeap(void *heapStart, u32 heapSize)
{
    sHeapStart = heapStart;
    sHeapSize = heapSize;
    PutFirstMemBlockHeader(heapStart, heapSize);
}

/**
 * FUNCTION: Alloc
 *
 * PURPOSE: Allocate memory from the global heap (game's malloc).
 *
 * @param size — Number of bytes to allocate
 * RETURNS: Pointer to allocated memory, or NULL if out of memory
 */
void *Alloc(u32 size)
{
    return AllocInternal(sHeapStart, size);
}

/**
 * FUNCTION: AllocZeroed
 *
 * PURPOSE: Allocate zero-initialized memory from the global heap (game's calloc).
 *
 * @param size — Number of bytes to allocate
 * RETURNS: Pointer to zeroed memory, or NULL if out of memory
 */
void *AllocZeroed(u32 size)
{
    return AllocZeroedInternal(sHeapStart, size);
}

/**
 * FUNCTION: Free
 *
 * PURPOSE: Return allocated memory to the global heap (game's free).
 *
 * @param pointer — Pointer previously returned by Alloc/AllocZeroed
 */
void Free(void *pointer)
{
    FreeInternal(sHeapStart, pointer);
}

/**
 * FUNCTION: CheckMemBlock
 *
 * PURPOSE: Check if a heap allocation is valid (not corrupted).
 *
 * @param pointer — Pointer to check
 * RETURNS: TRUE if valid, FALSE if corrupted
 */
bool32 CheckMemBlock(void *pointer)
{
    return CheckMemBlockInternal(sHeapStart, pointer);
}

/**
 * FUNCTION: CheckHeap
 *
 * PURPOSE: Validate the entire heap for corruption.
 *
 * HOW IT WORKS:
 * Walks through every block in the heap's linked list and checks each
 * one with CheckMemBlockInternal. If ANY block fails the check, the
 * heap is corrupted.
 *
 * GAME LOGIC:
 * This is a diagnostic function, likely used during development to
 * detect heap corruption caused by buffer overflows.
 *
 * RETURNS: TRUE if all blocks are valid, FALSE if any corruption detected
 */
bool32 CheckHeap()
{
    struct MemBlock *pos = (struct MemBlock *)sHeapStart;

    do {
        if (!CheckMemBlockInternal(sHeapStart, pos->data))
            return FALSE;
        pos = pos->next;
    } while (pos != (struct MemBlock *)sHeapStart);

    return TRUE;
}
