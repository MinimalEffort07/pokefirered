/*
 * defines.h - GBA Hardware Constants and Memory Map
 *
 * ============================================================================
 * GBA MEMORY MAP
 * ============================================================================
 *
 * The GBA uses a FLAT memory map where different address ranges correspond
 * to different physical memory chips and hardware. There is no virtual
 * memory, no MMU, no memory protection. Any code can read/write any address.
 *
 * Address Range        Size    Description
 * -------------------- ------- -------------------------------------------
 * 0x00000000-0x00003FFF  16 KB  BIOS ROM (system functions, read-protected)
 * 0x02000000-0x0203FFFF 256 KB  EWRAM (External Work RAM) - main data storage
 * 0x03000000-0x03007FFF  32 KB  IWRAM (Internal Work RAM) - fast, stack here
 * 0x04000000-0x040003FF   1 KB  I/O Registers (hardware control)
 * 0x05000000-0x050003FF   1 KB  Palette RAM (512 colors: 256 BG + 256 OBJ)
 * 0x06000000-0x06017FFF  96 KB  VRAM (tile data + tilemaps + bitmaps)
 * 0x07000000-0x070003FF   1 KB  OAM (128 sprite entries + 32 affine matrices)
 * 0x08000000-0x09FFFFFF  32 MB  ROM (game cartridge, read-only)
 * 0x0E000000-0x0E00FFFF  64 KB  SRAM/Flash (save data, battery-backed)
 *
 * ============================================================================
 * MEMORY-MAPPED I/O
 * ============================================================================
 *
 * The GBA has no "system calls" for hardware control (well, it has BIOS
 * calls, but they're limited). Instead, hardware is controlled by reading
 * and writing to specific memory addresses. This is called "memory-mapped I/O".
 *
 * For example:
 *   Writing 0x0403 to address 0x04000000 sets Mode 3 with BG2 enabled.
 *   Reading address 0x04000130 returns the current button state.
 *   Writing pixel data to 0x06000000 changes what's displayed on screen.
 *
 * This means a simple C pointer dereference like:
 *   *(volatile u16 *)0x04000000 = 0x0403;
 * ...is actually programming the GPU hardware.
 *
 * ============================================================================
 * SECTION ATTRIBUTES (GCC LINKER SECTIONS)
 * ============================================================================
 *
 * On a GBA, you can control WHERE in memory a variable is placed using
 * GCC's __attribute__((section(...))). The linker script maps section
 * names to physical memory addresses:
 *
 *   IWRAM_DATA -> 0x03000000 (32 KB, fast, 32-bit bus, 0 wait states)
 *   EWRAM_DATA -> 0x02000000 (256 KB, slower, 16-bit bus, 2 wait states)
 *   (no attribute) -> ROM at 0x08000000 (read-only, const data)
 *
 * This matters because:
 *   - Performance-critical code/data goes in IWRAM (fast but small)
 *   - Large data (save blocks, sprite buffers) goes in EWRAM
 *   - Constants (string tables, tile graphics) stay in ROM (saves RAM)
 *
 * ============================================================================
 */

#ifndef GUARD_GBA_DEFINES
#define GUARD_GBA_DEFINES

#include <stddef.h>

/* Boolean constants. The GBA has no native bool type. */
#define TRUE  1
#define FALSE 0

/*
 * Memory placement attributes.
 * These tell the GCC linker where to put variables in the GBA's memory.
 *
 * IWRAM_DATA: Place in Internal Work RAM (0x03000000).
 *   - 32 KB total, shared with the stack
 *   - 32-bit bus, 0 wait states (fastest RAM on the GBA)
 *   - Use for frequently accessed variables (interrupt handlers, hot loops)
 *
 * EWRAM_DATA: Place in External Work RAM (0x02000000).
 *   - 256 KB total
 *   - 16-bit bus, 2 wait states (slower but much larger)
 *   - Use for large data structures (save blocks, decompression buffers)
 *
 * COMMON_DATA: BSS-like section for zero-initialized globals.
 *   The linker places these after other data sections.
 *   Zero-initialized by RegisterRamReset at startup.
 *
 * macOS uses different section naming syntax (__DATA,section_name),
 * hence the #if defined(__APPLE__) guard. This allows cross-compilation
 * on Mac for development/testing.
 */
#if defined(__APPLE__)
#define IWRAM_DATA __attribute__((section("__DATA,iwram_data")))
#define EWRAM_DATA __attribute__((section("__DATA,ewram_data")))
#else
#define IWRAM_DATA __attribute__((section("iwram_data")))
#define EWRAM_DATA __attribute__((section("ewram_data")))
#endif
#define COMMON_DATA __attribute__((section("common_data")))

/*
 * NOINLINE: Prevent the compiler from inlining a function.
 * Inlining replaces a function call with the function's code at each
 * call site. This makes the code faster (no call overhead) but larger.
 * On GBA with limited ROM, sometimes you want to prevent this.
 * Only used in MODERN builds (newer GCC versions inline aggressively).
 */
#if MODERN
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

/*
 * ALIGNED(n): Force a variable to be placed at an address that's a
 * multiple of n bytes.
 *
 * ARM7TDMI requires certain alignments for correct operation:
 *   - 32-bit reads MUST be 4-byte aligned (address divisible by 4)
 *   - 16-bit reads MUST be 2-byte aligned (address divisible by 2)
 *   - DMA transfers MUST be aligned to the transfer width
 *
 * Misaligned access on ARM7TDMI doesn't crash - it silently reads
 * WRONG DATA (the address is rounded down). This causes subtle bugs
 * that are extremely hard to track down.
 */
#define ALIGNED(n) __attribute__((aligned(n)))

/*
 * BIOS WORK AREA POINTERS
 *
 * The top of IWRAM (0x03007Fxx) is reserved by the BIOS for system data.
 * The game can read/write these to interact with the BIOS:
 *
 * SOUND_INFO_PTR (0x03007FF0):
 *   Pointer to the SoundInfo struct used by the BIOS sound driver.
 *   The m4a sound engine writes here so the BIOS knows where to find
 *   the sound mixing buffer and configuration.
 *
 * INTR_CHECK (0x03007FF8):
 *   Bitmask of interrupts that have been acknowledged by handlers.
 *   The BIOS VBlankIntrWait() function reads this to know when
 *   to wake up from a halt state.
 *
 * INTR_VECTOR (0x03007FFC):
 *   Pointer to the game's interrupt dispatcher function.
 *   When ANY interrupt fires, the BIOS reads this address and jumps
 *   to the function stored here. In this game, it points to
 *   IntrMain_Buffer (a RAM copy of intr_main assembly code).
 */
#define SOUND_INFO_PTR (*(struct SoundInfo **)0x3007FF0)
#define INTR_CHECK     (*(u16 *)0x3007FF8)
#define INTR_VECTOR    (*(void **)0x3007FFC)

/*
 * WORK RAM ADDRESS RANGES
 *
 * EWRAM (External Work RAM): 256 KB at 0x02000000 - 0x0203FFFF
 *   The main "heap" memory for the game. Most game data lives here:
 *   save blocks, sprite animation data, decompression buffers, etc.
 *   Accessed over a 16-bit bus, so 32-bit reads take 2 bus cycles.
 *
 * IWRAM (Internal Work RAM): 32 KB at 0x03000000 - 0x03007FFF
 *   Fast memory on the same chip as the CPU. The stack grows downward
 *   from 0x03007F00. Interrupt handlers and performance-critical code
 *   are placed here. 32-bit bus with 0 wait states.
 */
#define EWRAM_START 0x02000000
#define EWRAM_END   (EWRAM_START + 0x40000)  /* 0x02040000 = 256 KB */
#define IWRAM_START 0x03000000
#define IWRAM_END   (IWRAM_START + 0x8000)   /* 0x03008000 = 32 KB */

/*
 * PALETTE RAM (PLTT) - 1 KB at 0x05000000
 *
 * The GBA stores ALL colors here. The GPU reads palette RAM directly
 * when rendering - there's no "set color" function, you just write
 * colors to these addresses.
 *
 * Layout (512 total 16-bit color entries):
 *
 *   BG_PLTT (0x05000000): Background palettes
 *     256 entries = 512 bytes (BG_PLTT_SIZE = 0x200)
 *     In 4bpp mode: 16 sub-palettes of 16 colors each
 *     In 8bpp mode: 1 palette of 256 colors
 *     Entry 0 of sub-palette 0 is the BACKDROP color (shown behind everything)
 *
 *   OBJ_PLTT (0x05000200): Sprite/Object palettes
 *     256 entries = 512 bytes (OBJ_PLTT_SIZE = 0x200)
 *     Same sub-palette structure as BG palettes
 *     Each sprite's OAM entry specifies which sub-palette it uses
 *
 * Colors are 15-bit BGR555: 0BBBBBGGGGGRRRRR (see PlttData in types.h)
 */
#define PLTT          0x5000000
#define BG_PLTT       PLTT
#define BG_PLTT_SIZE  0x200       /* 512 bytes = 256 colors x 2 bytes each */
#define OBJ_PLTT      (PLTT + BG_PLTT_SIZE)  /* 0x05000200 */
#define OBJ_PLTT_SIZE 0x200       /* 512 bytes = 256 colors x 2 bytes each */
#define PLTT_SIZE     (BG_PLTT_SIZE + OBJ_PLTT_SIZE)  /* 1024 bytes total */

/*
 * VIDEO RAM (VRAM) - 96 KB at 0x06000000
 *
 * VRAM stores ALL graphical data: tile pixel data, tilemaps, and bitmaps.
 * The GPU reads VRAM directly when rendering each scanline.
 *
 * VRAM is divided into regions depending on display mode:
 *
 * TEXT MODE (Mode 0, used by Pokemon):
 *   BG_VRAM (0x06000000 - 0x0600FFFF): 64 KB for background tiles + tilemaps
 *     Character Base Blocks: 4 blocks of 16 KB each for tile pixel data
 *     Screen Base Blocks: 32 blocks of 2 KB each for tilemaps
 *     These OVERLAP in the same 64 KB space - careful not to put tilemap
 *     data on top of tile pixel data!
 *
 *   OBJ_VRAM0 (0x06010000 - 0x06017FFF): 32 KB for sprite tile pixel data
 *     Sprite tiles are stored here. Each sprite's OAM tileNum field
 *     indexes into this area (tile N starts at OBJ_VRAM0 + N*32 in 4bpp).
 *
 * BITMAP MODE (Modes 3-5):
 *   OBJ_VRAM1 (0x06014000): Only 16 KB available for sprites
 *     (because the bitmap frame buffer takes up most of VRAM)
 */
#define VRAM      0x6000000
#define VRAM_SIZE 0x18000         /* 96 KB total */

#define BG_VRAM           VRAM
#define BG_VRAM_SIZE      0x10000  /* 64 KB for BG data */
#define BG_CHAR_SIZE      0x4000   /* 16 KB per character base block (tile graphics) */
#define BG_SCREEN_SIZE    0x800    /* 2 KB per screen base block (tilemap) */

/*
 * BG_CHAR_ADDR(n): Get the address of character base block N (0-3).
 *   Block 0 = 0x06000000, Block 1 = 0x06004000, etc.
 *   This is where tile PIXEL DATA lives (the "font" of 8x8 tiles).
 *
 * BG_SCREEN_ADDR(n): Get the address of screen base block N (0-31).
 *   Block 0 = 0x06000000, Block 31 = 0x0600F800.
 *   This is where the TILEMAP lives (which tile goes where on the grid).
 *
 * BG_TILE_ADDR(n): Get the address of tile N within VRAM.
 *   Each tile is 0x80 bytes apart in this addressing scheme (used for
 *   8bpp tiles where each tile = 64 bytes, rounded up to 0x80 alignment).
 */
#define BG_CHAR_ADDR(n)   (void *)(BG_VRAM + (BG_CHAR_SIZE * (n)))
#define BG_SCREEN_ADDR(n) (void *)(BG_VRAM + (BG_SCREEN_SIZE * (n)))
#define BG_TILE_ADDR(n)   (void *)(BG_VRAM + (0x80 * (n)))

/*
 * TILEMAP ENTRY FLAGS
 *
 * Each entry in a text-mode tilemap is 16 bits:
 *   Bits 0-9:   Tile index (which 8x8 tile to display, 0-1023)
 *   Bit 10:     Horizontal flip (mirror the tile left-right)
 *   Bit 11:     Vertical flip (mirror the tile top-bottom)
 *   Bits 12-15: Palette number (which of 16 sub-palettes, in 4bpp mode)
 *
 * These macros add the flip flags to a tile index:
 *   BG_TILE_H_FLIP(5)     -> tile 5, flipped horizontally  (0x0405)
 *   BG_TILE_V_FLIP(5)     -> tile 5, flipped vertically    (0x0805)
 *   BG_TILE_H_V_FLIP(5)   -> tile 5, flipped both ways     (0x0C05)
 */
#define BG_TILE_H_FLIP(n)   (0x400 + (n))   /* Bit 10 set = horizontal flip */
#define BG_TILE_V_FLIP(n)   (0x800 + (n))   /* Bit 11 set = vertical flip */
#define BG_TILE_H_V_FLIP(n) (0xC00 + (n))   /* Bits 10+11 set = both flips */

/*
 * OBJ (Sprite) VRAM regions.
 * Sprite tile data is stored separately from BG tile data, at the
 * end of the 96 KB VRAM space.
 */
/* text-mode BG: sprites get 32 KB starting at 0x06010000 */
#define OBJ_VRAM0      (void *)(VRAM + 0x10000)
#define OBJ_VRAM0_SIZE 0x8000  /* 32 KB */

/* bitmap-mode BG: sprites only get 16 KB (bitmap eats the rest) */
#define OBJ_VRAM1      (void *)(VRAM + 0x14000)
#define OBJ_VRAM1_SIZE 0x4000  /* 16 KB */

/*
 * OAM (Object Attribute Memory) - 1 KB at 0x07000000
 *
 * OAM holds the attribute data for all 128 hardware sprites.
 * Each sprite = 8 bytes (see OamData in types.h), so 128 * 8 = 1024 bytes.
 *
 * The game maintains a BUFFER of OAM data in regular RAM and copies it
 * to hardware OAM during VBlank (via LoadOam/DmaCopy). This prevents
 * mid-frame modifications that would cause sprite tearing.
 */
#define OAM      0x7000000
#define OAM_SIZE 0x400  /* 1024 bytes = 128 sprites * 8 bytes each */

/*
 * ROM HEADER
 * The first 0xC0 (192) bytes of the ROM cartridge contain the ROM header:
 * game title, game code, maker code, checksums, etc.
 * The BIOS verifies the header checksum before running the game.
 */
#define ROM_HEADER_SIZE   0xC0

/*
 * DISPLAY DIMENSIONS
 * The GBA's LCD is 240 pixels wide by 160 pixels tall.
 * These constants are used throughout the codebase for screen math.
 */
#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 160

/*
 * TILE SIZE CONSTANTS
 *
 * An 8x8 pixel tile occupies different amounts of memory depending
 * on the color depth:
 *
 *   4bpp (4 bits per pixel): 8 * 8 * 4 / 8 = 32 bytes per tile
 *     Each byte stores 2 pixels (lower nibble = left pixel, upper = right)
 *     Can only use 16 colors per tile (from one of 16 sub-palettes)
 *     More memory-efficient, used for most graphics
 *
 *   8bpp (8 bits per pixel): 8 * 8 * 8 / 8 = 64 bytes per tile
 *     Each byte stores 1 pixel
 *     Can use all 256 palette colors
 *     Used when you need more colors (detailed character portraits, etc.)
 */
#define TILE_SIZE_4BPP 32   /* 32 bytes per 8x8 tile at 4 bits/pixel */
#define TILE_SIZE_8BPP 64   /* 64 bytes per 8x8 tile at 8 bits/pixel */

/*
 * BG_TILE_ADDR_4BPP(n): Get the byte address of the Nth 4bpp tile.
 * Used to calculate where in VRAM a specific tile's pixel data starts.
 */
#define BG_TILE_ADDR_4BPP(n)   (void *)(BG_VRAM + (TILE_SIZE_4BPP * (n)))

/*
 * TILE_OFFSET_*BPP(n): Calculate the byte offset for N tiles.
 * Useful for computing how far into a tileset a particular tile is.
 */
#define TILE_OFFSET_4BPP(n) ((n) * TILE_SIZE_4BPP)
#define TILE_OFFSET_8BPP(n) ((n) * TILE_SIZE_8BPP)

/*
 * Maximum number of 8x8 tiles that can be stored in OBJ VRAM.
 * OBJ VRAM = 32 KB = 32768 bytes. At 32 bytes per 4bpp tile: 1024 tiles.
 * OAM tileNum field is 10 bits (0-1023), which matches exactly.
 */
#define TOTAL_OBJ_TILE_COUNT 1024

/*
 * PALETTE SIZE HELPERS
 *
 * PLTT_SIZEOF(n): How many bytes N palette entries occupy (each is u16 = 2 bytes).
 * PLTT_SIZE_4BPP: One 4bpp sub-palette = 16 colors = 32 bytes.
 * PLTT_SIZE_8BPP: One 8bpp palette = 256 colors = 512 bytes.
 * PLTT_OFFSET_4BPP(n): Byte offset to the Nth 4bpp sub-palette.
 */
#define PLTT_SIZEOF(n) ((n) * sizeof(u16))
#define PLTT_SIZE_4BPP PLTT_SIZEOF(16)    /* 32 bytes = 16 colors */
#define PLTT_SIZE_8BPP PLTT_SIZEOF(256)   /* 512 bytes = 256 colors */

#define PLTT_OFFSET_4BPP(n) ((n) * PLTT_SIZE_4BPP)  /* Offset to sub-palette N */

/*
 * FUNCTION ATTRIBUTES
 *
 * NAKED: Tells GCC to emit NO function prologue/epilogue.
 *   Normal functions save/restore registers and set up the stack frame.
 *   NAKED functions skip all of that - the function body is JUST the
 *   inline assembly you write. Used for interrupt handlers and other
 *   code that needs precise control over the generated assembly.
 *
 * UNUSED: Suppresses "unused variable/function" compiler warnings.
 *   Some variables exist only to force the compiler to generate specific
 *   code patterns needed for matching the original ROM's binary output.
 */
#define NAKED __attribute__((naked))
#define UNUSED __attribute__((unused))

#endif // GUARD_GBA_DEFINES
