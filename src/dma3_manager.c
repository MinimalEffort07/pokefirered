/*
 * dma3_manager.c - DMA Channel 3 Request Queue System
 *
 * ============================================================================
 * WHY A DMA REQUEST QUEUE?
 * ============================================================================
 *
 * DMA (Direct Memory Access) copies memory without using the CPU. On the GBA,
 * DMA channel 3 is the general-purpose channel used for:
 *   - Copying tile data to VRAM (graphics updates)
 *   - Copying tilemap data to VRAM (map changes)
 *   - Copying sprite graphics to OBJ VRAM
 *   - Filling memory regions with a value (clearing screens, etc.)
 *
 * THE PROBLEM: DMA transfers to VRAM, OAM, and Palette RAM should ONLY
 * happen during VBlank (when the GPU isn't reading that memory). If you
 * write to VRAM while the GPU is drawing, you get visual artifacts (tearing,
 * flickering, corrupted graphics).
 *
 * THE SOLUTION: This request queue. Game code calls RequestDma3Copy() or
 * RequestDma3Fill() at any time during the frame. The requests are stored
 * in a 128-entry circular buffer. Then, during VBlank, ProcessDma3Requests()
 * runs through the queue and executes the actual DMA transfers when it's
 * safe.
 *
 * SAFEGUARDS:
 *   1. 40 KB transfer limit per frame: Prevents VBlank from being exceeded.
 *      VBlank lasts ~4.5ms. DMA3 transfers at ~4 bytes per cycle at 16 MHz,
 *      so 40 KB takes roughly 2.5ms - leaves room for other VBlank work.
 *   2. VCOUNT check: If scanline > 224, we're about to exit VBlank, so stop.
 *   3. Lock flag: Prevents ProcessDma3Requests from running while a new
 *      request is being added (avoids corruption from interrupt re-entry).
 *
 * ============================================================================
 */

#include "global.h"
#include "dma3.h"

#define MAX_DMA_REQUESTS 128

/*
 * The DMA request queue. Each entry describes one pending DMA operation.
 *
 * Fields:
 *   src:   Source address (for copy operations)
 *   dest:  Destination address
 *   size:  Number of bytes to transfer (0 = slot is empty/available)
 *   mode:  DMA_REQUEST_COPY32, DMA_REQUEST_FILL32, DMA_REQUEST_COPY16, or DMA_REQUEST_FILL16
 *   value: The fill value (only used for fill operations, ignored for copy)
 */
static struct {
    /* 0x00 */ const u8 *src;
    /* 0x04 */ u8 *dest;
    /* 0x08 */ u16 size;
    /* 0x0A */ u16 mode;
    /* 0x0C */ u32 value;
} gDma3Requests[128];

/*
 * gDma3ManagerLocked: Mutual exclusion flag.
 * Set to TRUE while adding a request or processing the queue.
 * Prevents ProcessDma3Requests (called from VBlank interrupt) from
 * running while RequestDma3Copy/Fill is in the middle of writing a request.
 * Volatile because it's accessed from both main code and interrupt context.
 */
static volatile bool8 gDma3ManagerLocked;

/*
 * gDma3RequestCursor: Index of the NEXT request to process.
 * ProcessDma3Requests starts here and advances through the circular buffer.
 * New requests are added starting from this position (searching forward for
 * an empty slot).
 */
static u8 gDma3RequestCursor;

/**
 * FUNCTION: ClearDma3Requests
 *
 * PURPOSE: Reset the DMA request queue to empty.
 *
 * HOW IT WORKS:
 * Sets the lock flag to prevent ProcessDma3Requests from running during
 * cleanup, then zeros out all 128 request slots (setting size=0 marks
 * them as empty). Resets the cursor to slot 0.
 *
 * GBA CONTEXT:
 * Called once during game startup (AgbMain) and when reinitializing
 * the graphics system. Any pending DMA transfers are discarded.
 */
void ClearDma3Requests(void)
{
    int i;

    gDma3ManagerLocked = TRUE;
    gDma3RequestCursor = 0;

    /* Mark all 128 slots as empty by zeroing their fields */
    for(i = 0; i < (u8)NELEMS(gDma3Requests); i++)
    {
        gDma3Requests[i].size = 0;
        gDma3Requests[i].src = 0;
        gDma3Requests[i].dest = 0;
    }

    gDma3ManagerLocked = FALSE;
}

/**
 * FUNCTION: ProcessDma3Requests
 *
 * PURPOSE: Execute pending DMA transfers from the request queue.
 *
 * HOW IT WORKS:
 * Called during VBlank interrupt (from VBlankIntr in main.c).
 * Iterates through pending requests starting at gDma3RequestCursor,
 * executing each DMA transfer until:
 *   - All pending requests are processed, OR
 *   - Total bytes transferred exceeds 40 KB (VBlank time budget), OR
 *   - Scanline counter (VCOUNT) exceeds 224 (approaching end of VBlank)
 *
 * After processing each request, the slot is cleared (size=0) and the
 * cursor advances. The cursor wraps around at 128 (circular buffer).
 *
 * GBA CONTEXT:
 * VBlank is the safe window for modifying VRAM/OAM/Palette.
 * VBlank spans scanlines 160-227. If VCOUNT > 224, we're about to
 * leave VBlank, so we stop to avoid writing to VRAM while the GPU
 * is reading it (which causes visual glitches).
 *
 * The 40 KB limit is a conservative budget. At DMA3's transfer rate
 * (~4 bytes/cycle at 16.78 MHz), 40 KB takes about 2.5 ms. VBlank
 * is ~4.5 ms total, leaving room for sound processing and other
 * VBlank tasks.
 */
void ProcessDma3Requests(void)
{
    u16 bytesTransferred;

    /* Don't process if someone is currently adding a request */
    if (gDma3ManagerLocked)
        return;

    bytesTransferred = 0;

    /* Process requests until we hit an empty slot (size == 0) */
    while (gDma3Requests[gDma3RequestCursor].size != 0)
    {
        bytesTransferred += gDma3Requests[gDma3RequestCursor].size;

        /*
         * Safety check 1: Don't transfer more than 40 KB per VBlank frame.
         * 40 * 1024 = 40,960 bytes. This prevents the DMA from running
         * past the end of VBlank and corrupting the display.
         */
        if (bytesTransferred > 40 * 1024)
            return;

        /*
         * Safety check 2: If VCOUNT > 224, we're near the end of VBlank.
         * REG_ADDR_VCOUNT (0x04000006) holds the current scanline.
         * VBlank spans lines 160-227. Line 224 gives us a ~3 scanline
         * safety margin before the GPU starts drawing the next frame.
         */
        if (*(u8 *)REG_ADDR_VCOUNT > 224)
            return;

        /* Execute the DMA transfer based on the request type */
        switch (gDma3Requests[gDma3RequestCursor].mode)
        {
        case DMA_REQUEST_COPY32:
            /*
             * 32-bit DMA copy. Copies in 4 KB chunks (MAX_DMA_BLOCK_SIZE)
             * because a single DMA transfer is limited to 16-bit count.
             * Dma3CopyLarge32_ handles chunking internally.
             */
            Dma3CopyLarge32_(gDma3Requests[gDma3RequestCursor].src,
                             gDma3Requests[gDma3RequestCursor].dest,
                             gDma3Requests[gDma3RequestCursor].size);
            break;
        case DMA_REQUEST_FILL32:
            /*
             * 32-bit DMA fill. Repeats a single 32-bit value across
             * the destination region. Uses DMA_SRC_FIXED flag internally.
             */
            Dma3FillLarge32_(gDma3Requests[gDma3RequestCursor].value,
                             gDma3Requests[gDma3RequestCursor].dest,
                             gDma3Requests[gDma3RequestCursor].size);
            break;
        case DMA_REQUEST_COPY16:
            /* 16-bit DMA copy. Used when destination requires 16-bit writes. */
            Dma3CopyLarge16_(gDma3Requests[gDma3RequestCursor].src,
                             gDma3Requests[gDma3RequestCursor].dest,
                             gDma3Requests[gDma3RequestCursor].size);
            break;
        case DMA_REQUEST_FILL16:
            /* 16-bit DMA fill. Required for Palette RAM (16-bit bus). */
            Dma3FillLarge16_(gDma3Requests[gDma3RequestCursor].value,
                             gDma3Requests[gDma3RequestCursor].dest,
                             gDma3Requests[gDma3RequestCursor].size);
            break;
        }

        /* Clear the processed request slot (mark as available) */
        gDma3Requests[gDma3RequestCursor].src = NULL;
        gDma3Requests[gDma3RequestCursor].dest = NULL;
        gDma3Requests[gDma3RequestCursor].size = 0;
        gDma3Requests[gDma3RequestCursor].mode = 0;
        gDma3Requests[gDma3RequestCursor].value = 0;

        /* Advance cursor, wrapping around at 128 (circular buffer) */
        gDma3RequestCursor++;
        if (gDma3RequestCursor >= MAX_DMA_REQUESTS)
            gDma3RequestCursor = 0;
    }
}

/**
 * FUNCTION: RequestDma3Copy
 *
 * PURPOSE: Queue a DMA copy operation to be executed during VBlank.
 *
 * HOW IT WORKS:
 * Searches the request queue for an empty slot (one with size==0),
 * starting from the current cursor position. If found, fills in
 * the slot with the copy parameters and returns the slot index.
 * If all 128 slots are full, returns -1 (request dropped).
 *
 * The search wraps around the circular buffer. The lock flag is
 * set during the search to prevent ProcessDma3Requests (which runs
 * in the VBlank interrupt) from modifying the queue simultaneously.
 *
 * PARAMETERS:
 * @param src  — Source address to copy from
 * @param dest — Destination address to copy to
 * @param size — Number of bytes to copy (max 65535)
 * @param mode — DMA3_16BIT (0) or DMA3_32BIT (1)
 *
 * RETURNS: Slot index (0-127) on success, -1 if queue is full
 */
s16 RequestDma3Copy(const void *src, void *dest, u16 size, u8 mode)
{
    int cursor;
    int var = 0;

    /* Lock the queue to prevent the VBlank handler from processing
     * during our search and write */
    gDma3ManagerLocked = 1;

    cursor = gDma3RequestCursor;
    while(1)
    {
        if(!gDma3Requests[cursor].size) /* Found an empty slot */
        {
            gDma3Requests[cursor].src = src;
            gDma3Requests[cursor].dest = dest;
            gDma3Requests[cursor].size = size;

            /* Convert user-friendly mode (0=16bit, 1=32bit) to
             * internal request type */
            if(mode == DMA3_32BIT)
                gDma3Requests[cursor].mode = DMA_REQUEST_COPY32;
            else
                gDma3Requests[cursor].mode = DMA_REQUEST_COPY16;

            gDma3ManagerLocked = FALSE;
            return (s16)cursor;
        }

        /* Wrap around at the end of the buffer */
        if(++cursor >= 0x80)
        {
            cursor = 0;
        }

        /* If we've checked all 128 slots without finding an empty one,
         * the queue is full. Return -1 to indicate failure. */
        if(++var >= 0x80)
        {
            break;
        }
    }
    gDma3ManagerLocked = FALSE;
    return -1;
}

/**
 * FUNCTION: RequestDma3Fill
 *
 * PURPOSE: Queue a DMA fill operation to be executed during VBlank.
 *
 * HOW IT WORKS:
 * Same search algorithm as RequestDma3Copy, but stores a fill VALUE
 * instead of a source address. During processing, DMA_SRC_FIXED
 * mode is used so the DMA reads the same value for every write.
 *
 * PARAMETERS:
 * @param value — The 32-bit value to fill with (e.g., 0 to clear memory)
 * @param dest  — Destination address to fill
 * @param size  — Number of bytes to fill (max 65535)
 * @param mode  — DMA3_16BIT (0) or DMA3_32BIT (1)
 *
 * RETURNS: Slot index (0-127) on success, -1 if queue is full
 */
s16 RequestDma3Fill(s32 value, void *dest, u16 size, u8 mode)
{
    int cursor;
    int var = 0;

    cursor = gDma3RequestCursor;
    gDma3ManagerLocked = 1;

    while(1)
    {
        if(!gDma3Requests[cursor].size)
        {
            gDma3Requests[cursor].dest = dest;
            gDma3Requests[cursor].size = size;
            gDma3Requests[cursor].mode = mode;
            gDma3Requests[cursor].value = value;

            if(mode == DMA3_32BIT)
                gDma3Requests[cursor].mode = DMA_REQUEST_FILL32;
            else
                gDma3Requests[cursor].mode = DMA_REQUEST_FILL16;

            gDma3ManagerLocked = FALSE;
            return (s16)cursor;
        }
        if(++cursor >= 0x80)
        {
            cursor = 0;
        }
        if(++var >= 0x80)
        {
            break;
        }
    }
    gDma3ManagerLocked = FALSE;
    return -1;
}

/**
 * FUNCTION: WaitDma3Request
 *
 * PURPOSE: Check if a specific DMA request has been processed (or if any are pending).
 *
 * HOW IT WORKS:
 * If index == -1: Checks ALL 128 slots. Returns -1 if ANY request is pending,
 * 0 if all are complete.
 * If index >= 0: Checks the specific slot. Returns -1 if that request is still
 * pending (size != 0), 0 if it's been processed.
 *
 * GAME LOGIC:
 * Some operations need to ensure their DMA has completed before proceeding
 * (e.g., a tilemap update must finish before the game changes the scroll
 * position). They call WaitDma3Request in a loop or check it once per frame.
 *
 * PARAMETERS:
 * @param index — Slot index from RequestDma3Copy/Fill, or -1 for "check all"
 *
 * RETURNS: -1 if the request is still pending, 0 if complete
 */
s16 WaitDma3Request(s16 index)
{
    int current = 0;

    if (index == -1)
    {
        /* Check all slots - if ANY has a non-zero size, something is pending */
        for (; current < 0x80; current ++)
            if (gDma3Requests[current].size)
                return -1;

        return 0;
    }

    /* Check the specific slot */
    if (gDma3Requests[index].size)
        return -1;

    return 0;
}
