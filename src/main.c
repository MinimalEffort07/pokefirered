/*
 * main.c - GBA Game Entry Point and Core System Loop
 *
 * ============================================================================
 * GBA HARDWARE OVERVIEW FOR C PROGRAMMERS
 * ============================================================================
 *
 * The Game Boy Advance is an ARM7TDMI-based embedded system with:
 *
 * MEMORY MAP:
 *   0x00000000 - BIOS ROM (16 KB) - System functions, not readable by game
 *   0x02000000 - EWRAM (256 KB) - External Work RAM. Main heap/data storage.
 *                Slower than IWRAM (2 wait states). Accessed via 16-bit bus.
 *   0x03000000 - IWRAM (32 KB) - Internal Work RAM. Stack lives here.
 *                Fast (0 wait states), 32-bit bus. Used for perf-critical code/data.
 *   0x04000000 - I/O Registers - Hardware control (GPU, sound, DMA, timers, serial)
 *   0x05000000 - Palette RAM (1 KB) - 256 BG colors + 256 sprite colors (15-bit RGB)
 *   0x06000000 - VRAM (96 KB) - Video RAM for tile data and tilemaps
 *   0x07000000 - OAM (1 KB) - Object Attribute Memory (sprite positions/attributes)
 *   0x08000000 - ROM (up to 32 MB) - Game cartridge, read-only
 *   0x0E000000 - SRAM/Flash (up to 128 KB) - Save data
 *
 * DISPLAY:
 *   240x160 pixels, 15-bit color (5 bits per R/G/B channel)
 *   60 Hz refresh rate (59.73 Hz precisely)
 *   Each frame: 160 visible scanlines (VDraw) + 68 blank scanlines (VBlank)
 *   Each scanline: 240 visible pixels (HDraw) + 68 blank pixels (HBlank)
 *
 * GRAPHICS MODES:
 *   Mode 0: 4 tiled BG layers (text mode) - used by Pokemon FireRed
 *   Mode 1: 2 tiled + 1 affine (rotatable) BG layer
 *   Mode 2: 2 affine BG layers
 *   Modes 3-5: Bitmap modes (not used in Pokemon)
 *
 * SPRITES (Objects):
 *   Up to 128 sprites via OAM (Object Attribute Memory)
 *   Each sprite: position, size (8x8 to 64x64), palette, tile, flip, priority
 *   Sprites are rendered on top of BG layers, sorted by priority
 *
 * INTERRUPTS:
 *   VBlank - Fires at start of vertical blank period (line 160). Main sync point.
 *   HBlank - Fires at end of each visible scanline. Used for raster effects.
 *   VCount - Fires when scanline counter matches a preset value.
 *   Timer  - Hardware timer overflow interrupts.
 *   Serial - SIO transfer complete (link cable communication).
 *   DMA    - DMA transfer complete.
 *
 * The GBA has NO operating system. The game IS the operating system. This file
 * sets up the hardware, installs interrupt handlers, and runs the main loop.
 *
 * ============================================================================
 * SECTION ATTRIBUTES (MEMORY PLACEMENT)
 * ============================================================================
 *
 * The GBA linker script places variables in different memory regions:
 *
 *   EWRAM_DATA - External Work RAM (0x02000000). 256 KB, slow but large.
 *                Used for most game data (save blocks, sprite data, etc.)
 *
 *   IWRAM_DATA - Internal Work RAM (0x03000000). 32 KB, fast.
 *                Used for performance-critical data accessed in tight loops.
 *
 *   COMMON_DATA - BSS-like section. Zero-initialized globals.
 *                 Lives in IWRAM or EWRAM depending on linker config.
 *
 *   No attribute = ROM. Placed in the cartridge at 0x08000000. Read-only.
 *                  Used for const data (strings, graphics, tables).
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"
#include "link.h"
#include "link_rfu.h"
#include "load_save.h"
#include "m4a.h"
#include "random.h"
#include "gba/flash_internal.h"
#include "help_system.h"
#include "new_menu_helpers.h"
#include "overworld.h"
#include "play_time.h"
#include "intro.h"
#include "battle_controllers.h"
#include "scanline_effect.h"
#include "save_failed_screen.h"
#include "quest_log.h"

/*
 * intr_main is the low-level ARM assembly interrupt dispatcher.
 * When ANY interrupt fires, the CPU jumps to the address at 0x03007FFC
 * (INTR_VECTOR). That points to IntrMain_Buffer, which is a RAM copy of
 * intr_main. The dispatcher reads REG_IE (enabled interrupts) and REG_IF
 * (fired interrupts), finds the highest-priority one, and calls the
 * corresponding function from gIntrTable[].
 *
 * It's copied to RAM because ROM access is slow (2-3 wait states).
 * Interrupt handlers need to execute FAST - every CPU cycle spent in an
 * interrupt is a cycle stolen from the main game loop.
 */
extern u32 intr_main[];

static void VBlankIntr(void);
static void HBlankIntr(void);
static void VCountIntr(void);
static void SerialIntr(void);
static void IntrDummy(void);

/*
 * Game version and language constants.
 * Burned into ROM as const data. Used by the link system to verify
 * compatibility between connected cartridges (e.g., FireRed can't link
 * with Ruby for certain features).
 */
const u8 gGameVersion = GAME_VERSION;
const u8 gGameLanguage = GAME_LANGUAGE;

#if MODERN
const char BuildDateTime[] = __DATE__ " " __TIME__;
#else
#if REVISION == 0
const char BuildDateTime[] = "2004 04 26 11:20";
#else
const char BuildDateTime[] = "2004 07 20 09:30";
#endif //REVISION
#endif //MODERN

/*
 * GBA INTERRUPT PRIORITY TABLE
 *
 * The GBA supports 14 interrupt sources. This table maps each interrupt
 * to its handler function. The ORDER matters - interrupts earlier in the
 * table have HIGHER priority. If two interrupts fire simultaneously,
 * the one with the lower index is serviced first.
 *
 * Priority order (highest to lowest):
 *  0. VCount  - Scanline counter match. Used for sound timing (line 150).
 *  1. Serial  - Link cable transfer complete. Critical for multiplayer sync.
 *  2. Timer3  - Hardware timer. Used by the link system for transfer timing.
 *  3. HBlank  - End of each scanline. Used for screen effects (wave, fade).
 *  4. VBlank  - Start of vertical blank. THE main game sync point.
 *  5-13. Unused (DMA, Key, GamePak, Timers 0-2) - set to IntrDummy.
 *
 * VBlank is priority 4 (not 0) because sound timing (VCount) and
 * link communication (Serial, Timer3) are more time-sensitive.
 * Missing a sound update causes audible glitches; missing a link
 * transfer causes desync. VBlank has a large window (~4.5ms) so
 * it's fine to service it after the others.
 */
const IntrFunc gIntrTableTemplate[] =
{
    VCountIntr, // V-count interrupt
    SerialIntr, // Serial interrupt
    Timer3Intr, // Timer 3 interrupt
    HBlankIntr, // H-blank interrupt
    VBlankIntr, // V-blank interrupt
    IntrDummy,  // Timer 0 interrupt
    IntrDummy,  // Timer 1 interrupt
    IntrDummy,  // Timer 2 interrupt
    IntrDummy,  // DMA 0 interrupt
    IntrDummy,  // DMA 1 interrupt
    IntrDummy,  // DMA 2 interrupt
    IntrDummy,  // DMA 3 interrupt
    IntrDummy,  // Key interrupt
    IntrDummy,  // Game Pak interrupt
};

#define INTR_COUNT ((int)(sizeof(gIntrTableTemplate)/sizeof(IntrFunc)))

/*
 * Key repeat configuration.
 * When a button is held down continuously:
 *  - After gKeyRepeatStartDelay frames (40 = ~0.67s), start repeating
 *  - Then repeat every gKeyRepeatContinueDelay frames (5 = ~12 times/sec)
 * This is used for menu scrolling (holding UP/DOWN in item lists, etc.)
 */
COMMON_DATA u16 gKeyRepeatStartDelay = 0;
COMMON_DATA u8 gLinkTransferringData = 0;

/*
 * THE central game state structure. Contains:
 *  - callback1/callback2: The two main game callbacks (see CallCallbacks)
 *  - state: Generic state counter, reset when callback2 changes
 *  - vblankCallback: Called during VBlank interrupt
 *  - hblankCallback: Called during HBlank interrupt
 *  - serialCallback: Called during Serial interrupt
 *  - Key state: newKeys, heldKeys, newAndRepeatedKeys, etc.
 *  - vblankCounter1/2: Frame counters incremented each VBlank
 *  - intrCheck: Bitmask of which interrupts have fired (for polling)
 */
COMMON_DATA struct Main gMain = {0};
COMMON_DATA u16 gKeyRepeatContinueDelay = 0;
COMMON_DATA u8 gSoftResetDisabled = 0;

/*
 * The RUNTIME interrupt table (in IWRAM for speed).
 * This is a copy of gIntrTableTemplate that can be modified at runtime
 * (e.g., RestoreSerialTimer3IntrHandlers replaces entries).
 * Lives in IWRAM because interrupt dispatch must be fast.
 */
COMMON_DATA IntrFunc gIntrTable[INTR_COUNT] = {0};
COMMON_DATA u8 sVcountAfterSound = 0;
COMMON_DATA bool8 gLinkVSyncDisabled = 0;

/*
 * RAM copy of the interrupt dispatcher code.
 * The ARM assembly from intr_main[] is DMA-copied here at startup.
 * INTR_VECTOR (0x03007FFC) points to this buffer.
 * This is in IWRAM (COMMON_DATA) so interrupt dispatch is fast.
 */
COMMON_DATA u32 IntrMain_Buffer[0x200] = {0};
COMMON_DATA u8 sVcountAtIntr = 0;
COMMON_DATA u8 sVcountBeforeSound = 0;

/*
 * PCM DMA counter for the sound engine (m4a).
 * The GBA's sound hardware uses DMA channels 1 and 2 to stream PCM audio.
 * The sound engine (m4a, aka MusicPlayer2000) manages this, and this
 * counter tracks DMA transfers for synchronization.
 */
COMMON_DATA u8 gPcmDmaCounter = 0;

static IntrFunc * const sTimerIntrFunc = gIntrTable + 0x7;

/*
 * General-purpose decompression buffer in EWRAM.
 * Many graphics assets in the ROM are LZ77-compressed to save space.
 * They're decompressed into this 16 KB buffer before being copied to VRAM.
 * This is a shared scratch buffer - only one decompression at a time.
 */
EWRAM_DATA u8 gDecompressionBuffer[0x4000] = {0};
EWRAM_DATA u16 gTrainerId = 0;

static void UpdateLinkAndCallCallbacks(void);
static void InitMainCallbacks(void);
static void CallCallbacks(void);
static void ReadKeys(void);
void InitIntrHandlers(void);
static void WaitForVBlank(void);
void EnableVCountIntrAtLine150(void);

#define B_START_SELECT (B_BUTTON | START_BUTTON | SELECT_BUTTON)

/*
 * AgbMain - THE entry point. Called by the CRT0 startup code after the
 * CPU is initialized and the C runtime is set up.
 *
 * This function:
 * 1. Resets all RAM to zero (RegisterRamReset)
 * 2. Sets the screen to white (writes to palette RAM directly)
 * 3. Configures ROM access waitstates (REG_WAITCNT)
 * 4. Initializes all hardware subsystems
 * 5. Enters the infinite main loop
 *
 * THE MAIN LOOP runs once per frame (~60 Hz). Each iteration:
 *  a. ReadKeys() - Sample the physical button state from REG_KEYINPUT
 *  b. Soft reset check - A+B+Start+Select triggers a warm reboot
 *  c. HandleLinkConnection() + CallCallbacks() - The game logic
 *  d. PlayTimeCounter_Update() - Increment play time
 *  e. MapMusicMain() - Process music commands
 *  f. WaitForVBlank() - Spin until VBlank interrupt fires
 *
 * After WaitForVBlank returns, the VBlank interrupt handler has already:
 *  - Synced link cable transfers
 *  - Copied OAM buffer to hardware OAM (sprite positions)
 *  - Copied palette buffer to hardware palette RAM
 *  - Written buffered GPU register values
 *  - Processed DMA requests
 *  - Run the sound engine (m4a)
 *
 * This "do work, then wait for VBlank" pattern is the standard GBA
 * game loop. All visible changes to VRAM/OAM/palettes MUST happen
 * during VBlank (or HBlank) to avoid screen tearing.
 */
void AgbMain()
{
#if MODERN
    // Modern compilers are liberal with the stack on entry to this function,
    // so RegisterRamReset may crash if it resets IWRAM.
    RegisterRamReset(RESET_ALL & ~RESET_IWRAM);
    asm("mov\tr1, #0xC0\n"
        "\tlsl\tr1, r1, #0x12\n"
        "\tmov\tr2, #0xFC\n"
        "\tlsl\tr2, r2, #0x7\n"
        "\tadd\tr2, r1, r2\n"
        "\tmov\tr0, #0\n"
        "\tmov\tr3, r0\n"
        "\tmov\tr4, r0\n"
        "\tmov\tr5, r0\n"
        ".LCU%=:\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tcmp\tr1, r2\n"
        "\tbcc\t.LCU%=\n"
        :
        :
        : "r0", "r1", "r2", "r3", "r4", "r5", "memory"
    );
#else
    /*
     * RegisterRamReset is a BIOS call (SWI 0x01) that zeros memory regions.
     * RESET_ALL clears EWRAM, IWRAM, Palette, VRAM, OAM, and I/O registers.
     * This gives us a clean slate - all RAM is zero, all hardware is default.
     *
     * BIOS calls (SWI = Software Interrupt) are the GBA's "system calls".
     * The BIOS ROM at 0x00000000 provides ~40 utility functions accessed
     * via the ARM SWI instruction. They execute in privileged mode.
     */
    RegisterRamReset(RESET_ALL);
#endif //MODERN

    /*
     * Set the first palette entry (index 0) to white.
     * BG_PLTT (0x05000000) is palette RAM. The first entry is the
     * "backdrop color" - shown wherever no BG or sprite is drawn.
     * Writing directly to palette RAM (memory-mapped I/O) is instant.
     * RGB_WHITE = 0x7FFF = white in 15-bit BGR format (5 bits each).
     *
     * Note: GBA uses BGR555 format: bit layout is 0BBBBBGGGGGRRRRR
     */
    *(vu16 *)BG_PLTT = RGB_WHITE;

    /*
     * Initialize the GPU register buffer system.
     * The GBA's GPU registers (REG_DISPCNT, REG_BGxCNT, etc.) are at
     * 0x04000000. Writing to them mid-frame can cause visual glitches.
     * The register manager buffers writes and applies them during VBlank.
     */
    InitGpuRegManager();

    /*
     * Configure ROM access timing.
     * REG_WAITCNT (0x04000204) controls how many CPU cycles the bus
     * waits when reading from ROM. The GBA ROM bus is 16-bit, but the
     * CPU is 32-bit, so each 32-bit read requires two 16-bit accesses.
     *
     * WAITCNT_PREFETCH_ENABLE: Enables the prefetch buffer. The GBA
     *   has a small buffer that pre-fetches the next ROM instruction
     *   while the CPU processes the current one. Big performance win.
     * WAITCNT_WS0_S_1: Sequential ROM access = 1 wait state
     * WAITCNT_WS0_N_3: Non-sequential ROM access = 3 wait states
     *
     * Sequential = consecutive addresses (common in code execution).
     * Non-sequential = random access (jumps, table lookups).
     */
    REG_WAITCNT = WAITCNT_PREFETCH_ENABLE | WAITCNT_WS0_S_1 | WAITCNT_WS0_N_3;

    InitKeys();
    InitIntrHandlers();

    /*
     * m4aSoundInit() initializes the sound engine (MusicPlayer2000, aka m4a).
     * The GBA has 4 tone channels (2 square wave, 1 wave, 1 noise) plus
     * 2 Direct Sound channels that stream 8-bit PCM audio via DMA.
     * m4a is a software mixer that combines multiple instrument samples
     * into a single PCM stream, outputting through Direct Sound.
     * This is the standard sound engine used by nearly all GBA games.
     */
    m4aSoundInit();

    /*
     * Set up a VCount interrupt at scanline 150.
     * The VCount interrupt fires when REG_VCOUNT matches a preset value.
     * Scanline 150 is near the end of the visible area (line 159 is last).
     * This is used to call m4aSoundVSync() which adjusts the sound
     * engine's mixing rate to stay synchronized with the display refresh.
     * Sound timing is critical - drift causes audible pitch changes.
     */
    EnableVCountIntrAtLine150();

    /*
     * Initialize the RF Unit (wireless adapter) interface.
     * The GBA Wireless Adapter is an external peripheral that plugs into
     * the link port. It uses a proprietary protocol (not standard SIO).
     * InitRFU sets up the communication state machine but doesn't
     * actually establish any wireless connections.
     */
    InitRFU();

    /*
     * Detect what type of save storage the cartridge has.
     * GBA cartridges can use SRAM (battery-backed static RAM),
     * Flash (NOR flash memory, 64KB or 128KB), or EEPROM.
     * Pokemon FireRed uses 128KB Flash (two 64KB banks).
     * This function probes the flash chip to identify its manufacturer
     * and capacity, which determines the correct write commands.
     */
    CheckForFlashMemory();

    InitMainCallbacks();

    /*
     * InitMapMusic() sets up the music system's state.
     * The game has a music manager that handles BGM transitions
     * (fade out old song, fade in new song when changing maps).
     */
    InitMapMusic();

    /*
     * DMA (Direct Memory Access) allows hardware-driven memory copies
     * without CPU involvement. The GBA has 4 DMA channels (0-3).
     * DMA3 is the general-purpose channel used by the game for
     * bulk memory operations. ClearDma3Requests empties the request queue.
     *
     * DMA0-1 are reserved for sound (PCM streaming to the DAC).
     * DMA2 is used for HBlank effects (scanline-based screen tricks).
     * DMA3 is used for VRAM copies, OAM updates, and general bulk transfers.
     */
    ClearDma3Requests();

    /*
     * Reset all 4 background layers to default state.
     * Each BG layer has: scroll position, tile base, map base,
     * size, color mode (4bpp/8bpp), and priority.
     */
    ResetBgs();

    /*
     * Initialize the dynamic memory allocator (heap).
     * gHeap is a block of EWRAM used for malloc/free style allocations.
     * HEAP_SIZE determines how much EWRAM is available for dynamic allocation.
     * The heap is used for temporary data (decompressed graphics, menus, etc.)
     * that doesn't need to persist across major state changes.
     *
     * Note: The GBA has no OS, so this is a custom allocator, not libc malloc.
     */
    InitHeap(gHeap, HEAP_SIZE);
    SetDefaultFontsPointer();

    gSoftResetDisabled = FALSE;
    gHelpSystemEnabled = FALSE;

    SetNotInSaveFailedScreen();

#ifndef NDEBUG
#if (LOG_HANDLER == LOG_HANDLER_MGBA_PRINT)
    /*
     * mGBA-specific debug logging.
     * mGBA (the emulator) provides a custom I/O register at 0x04FFF780
     * that lets games print debug messages to the emulator's console.
     * This is invaluable for debugging - like printf but for GBA.
     * Only compiled in debug builds (NDEBUG not defined).
     */
    (void) MgbaOpen();
#elif (LOG_HANDLER == LOG_HANDLER_AGB_PRINT)
    AGBPrintInit();
#endif
#endif

#if REVISION == 1
    if (gFlashMemoryPresent != TRUE)
        SetMainCallback2(NULL);
#endif

    gLinkTransferringData = FALSE;

    /*
     * ========================================================================
     * THE MAIN GAME LOOP
     * ========================================================================
     *
     * This infinite loop executes once per frame (~16.7ms at 59.73 Hz).
     * It's the heartbeat of the entire game.
     *
     * Frame timeline:
     * 1. ReadKeys() - Sample button state from hardware register
     * 2. Check for soft reset (A+B+Start+Select)
     * 3. Process link cable data + run game callbacks
     * 4. Update play time counter
     * 5. Process music state machine
     * 6. WaitForVBlank() - Spin until VBlank interrupt fires
     *    └── VBlank handler runs (OAM copy, palette copy, DMA, sound)
     * 7. Loop back to step 1
     *
     * The link data check (steps 170-189) has 3 paths:
     *  A. Sending keys over link: Process link first, then callbacks
     *  B. Normal: Process callbacks, then check if receiving link data
     *  C. Receiving keys from link: Run callbacks twice (once for link,
     *     once for normal processing) to prevent input lag
     */
    for (;;)
    {
        /*
         * Sample the physical button register.
         * REG_KEYINPUT (0x04000130) is a 10-bit register where each bit
         * represents a button. A bit is 0 when pressed, 1 when released.
         * ReadKeys() inverts this (XOR with 0x3FF) so 1 = pressed.
         * It also computes newKeys (just pressed this frame),
         * heldKeys (currently held), and key repeat state.
         */
        ReadKeys();

        /*
         * Soft reset: pressing A + B + Start + Select simultaneously
         * triggers a warm reboot. This is a standard GBA convention.
         * DoSoftReset() disables interrupts, stops DMA/sound, then
         * calls the BIOS SoftReset function which jumps back to the
         * ROM entry point (as if the game was just powered on).
         *
         * rfu_REQ_stopMode/rfu_waitREQComplete shut down the wireless
         * adapter cleanly before resetting, preventing hardware lockup.
         */
        if (gSoftResetDisabled == FALSE
         && (gMain.heldKeysRaw & A_BUTTON)
         && (gMain.heldKeysRaw & B_START_SELECT) == B_START_SELECT)
        {
            rfu_REQ_stopMode();
            rfu_waitREQComplete();
            DoSoftReset();
        }

        /*
         * Link cable data processing.
         *
         * The GBA link cable (SIO - Serial I/O) allows 2-4 GBAs to
         * exchange data. This section ensures link data is processed
         * in sync with the game callbacks.
         *
         * Three modes:
         * 1. Sending keys: We're broadcasting our button presses to
         *    linked players (e.g., in the overworld multiplayer).
         *    Process link BEFORE callbacks so our data is fresh.
         *
         * 2. Normal: No active link transfer. Just run callbacks.
         *
         * 3. Receiving keys: We're receiving other players' inputs.
         *    Run callbacks TWICE: once to process the received data,
         *    then once more for our own game logic. This prevents
         *    the game from feeling sluggish during link play.
         *    ClearSpriteCopyRequests() prevents sprite updates between
         *    the two callback runs (avoids visual artifacts).
         */
        if (Overworld_SendKeysToLinkIsRunning() == TRUE)
        {
            gLinkTransferringData = TRUE;
            UpdateLinkAndCallCallbacks();
            gLinkTransferringData = FALSE;
        }
        else
        {
            gLinkTransferringData = FALSE;
            UpdateLinkAndCallCallbacks();

            if (Overworld_RecvKeysFromLinkIsRunning() == 1)
            {
                gMain.newKeys = 0;
                ClearSpriteCopyRequests();
                gLinkTransferringData = TRUE;
                UpdateLinkAndCallCallbacks();
                gLinkTransferringData = FALSE;
            }
        }

        PlayTimeCounter_Update();
        MapMusicMain();

        /*
         * WAIT FOR VBLANK
         *
         * This is where the frame ends. WaitForVBlank() spins in a
         * tight loop until the VBlank interrupt handler sets the
         * INTR_FLAG_VBLANK bit in gMain.intrCheck.
         *
         * While we spin, the CPU is idle. The VBlank interrupt fires
         * when the display reaches scanline 160 (end of visible area).
         * The VBlank handler (VBlankIntr) then does all the hardware
         * updates that must happen during the blank period:
         *  - Copy sprite buffer → OAM hardware
         *  - Copy palette buffer → palette RAM
         *  - Apply buffered GPU register writes
         *  - Process DMA3 copy requests (VRAM updates)
         *  - Run the sound engine mixer
         *  - Advance the RNG
         *
         * If the game logic takes longer than one frame (~16.7ms),
         * WaitForVBlank returns immediately (the interrupt already
         * fired while we were still processing). This causes
         * "frame drops" - the screen shows the same image for 2+
         * frames. The game doesn't crash, but animation stutters.
         */
        WaitForVBlank();
    }
}

/*
 * UpdateLinkAndCallCallbacks - Process link data, then run game logic.
 *
 * HandleLinkConnection() is the main link cable state machine driver.
 * It calls LinkMain1() (process send/receive queues) and LinkMain2()
 * (process received commands and run link callbacks).
 *
 * If HandleLinkConnection returns TRUE, it means the link system
 * consumed the entire frame's processing time and game callbacks
 * should NOT run this frame. This happens during link handshakes
 * or when the send/receive queues are very full.
 *
 * If it returns FALSE, CallCallbacks() runs the game's main logic.
 */
static void UpdateLinkAndCallCallbacks(void)
{
    if (!HandleLinkConnection())
        CallCallbacks();
}

/*
 * InitMainCallbacks - Set up the initial game state.
 *
 * The GBA game uses a TWO-CALLBACK architecture:
 *
 *   callback1 (CB1): Input/logic callback. Runs FIRST each frame.
 *     Examples: CB1_Overworld (handle player input, NPC movement)
 *               CB1_UpdateLinkState (process link player movement)
 *     Can be NULL (some screens only need CB2).
 *
 *   callback2 (CB2): Rendering/state callback. Runs SECOND each frame.
 *     Examples: CB2_Overworld (render overworld, run scripts)
 *               CB2_InitBattle (set up battle screen)
 *               CB2_TitleScreen (animate title screen)
 *     This is the PRIMARY callback - setting CB2 is how you switch
 *     between major game states (overworld → battle → menu → etc.)
 *
 *   gMain.state: A generic counter that's reset to 0 whenever CB2
 *     changes. Used by CB2 functions as a step counter for multi-frame
 *     initialization sequences (load graphics step 1, step 2, etc.)
 *
 * At startup, CB2 is set to CB2_InitCopyrightScreenAfterBootup,
 * which displays the Game Freak logo and then transitions to the
 * title screen.
 */
static void InitMainCallbacks(void)
{
    gMain.vblankCounter1 = 0;
    gMain.vblankCounter2 = 0;
    gMain.callback1 = NULL;
    SetMainCallback2(CB2_InitCopyrightScreenAfterBootup);

    /*
     * Initialize save block pointers.
     * gSaveBlock2Ptr and gSaveBlock1Ptr are indirect pointers because
     * the actual SaveBlock data is at a randomized offset within the
     * save block arrays (for anti-cheat/anti-corruption purposes).
     * At startup, before loading a save, they point to the base address.
     */
    gSaveBlock2Ptr = &gSaveBlock2;
    gSaveBlock1Ptr = &gSaveBlock1;
    gSaveBlock2.encryptionKey = 0;
    gQuestLogPlaybackState = QL_PLAYBACK_STATE_STOPPED;
}

/*
 * CallCallbacks - Execute the game's main logic for this frame.
 *
 * Runs CB1 (if set) then CB2 (if set).
 *
 * Two systems can override normal callback execution:
 *  1. Save Failed Screen: If the save chip reports errors, a special
 *     error screen takes over to warn the player.
 *  2. Help System: FireRed's built-in help system (L/R button) can
 *     intercept normal game flow.
 *
 * RunSaveFailedScreen and RunHelpSystemCallback return TRUE if they
 * consumed the frame, preventing normal callbacks from running.
 */
static void CallCallbacks(void)
{
    if (!RunSaveFailedScreen() && !RunHelpSystemCallback())
    {
        if (gMain.callback1)
            gMain.callback1();

        if (gMain.callback2)
            gMain.callback2();
    }
}

/*
 * SetMainCallback2 - Switch to a new game state.
 *
 * This is THE function for major state transitions:
 *   SetMainCallback2(CB2_InitBattle)    → enter battle
 *   SetMainCallback2(CB2_Overworld)     → return to overworld
 *   SetMainCallback2(CB2_TitleScreen)   → show title screen
 *
 * Resets gMain.state to 0, which is used by the new CB2 as a
 * step counter for its initialization sequence. Most CB2 functions
 * use a switch(gMain.state) { case 0: ... case 1: ... } pattern
 * to spread initialization across multiple frames.
 */
void SetMainCallback2(MainCallback callback)
{
    gMain.callback2 = callback;
    gMain.state = 0;
}

/*
 * Timer 1 functions - Used for generating random trainer IDs.
 *
 * REG_TM1CNT_H (0x04000106) is the Timer 1 control register.
 * Bit 7 = enable. When enabled, the timer counts up every CPU cycle.
 * REG_TM1CNT_L (0x04000104) is the current timer value (16-bit).
 *
 * The game starts Timer 1 running, then reads its value later.
 * Because the exact timing depends on user input (how long they
 * wait on the title screen, etc.), the value is effectively random.
 * This is used to seed the PRNG and generate the trainer ID.
 *
 * Hardware timers increment at the CPU clock rate (16.78 MHz),
 * optionally divided by 1, 64, 256, or 1024. Timer 1 here uses
 * the default prescaler (divide by 1), counting at full CPU speed.
 */
void StartTimer1(void)
{
    REG_TM1CNT_H = 0x80;
}

void SeedRngAndSetTrainerId(void)
{
    u16 val = REG_TM1CNT_L;
    SeedRng(val);
    REG_TM1CNT_H = 0;
    gTrainerId = val;
}

u16 GetGeneratedTrainerIdLower(void)
{
    return gTrainerId;
}

/*
 * EnableVCountIntrAtLine150 - Configure VCount interrupt for sound sync.
 *
 * REG_DISPSTAT (0x04000004) controls display-related interrupts:
 *   Bits 0-2: Status flags (VBlank, HBlank, VCount match) - read only
 *   Bit 3: VBlank interrupt enable
 *   Bit 4: HBlank interrupt enable
 *   Bit 5: VCount interrupt enable
 *   Bits 8-15: VCount trigger value
 *
 * Setting the trigger to 150 means the VCount interrupt fires when
 * the display is drawing scanline 150 (10 lines before VBlank at 160).
 * The VCount handler calls m4aSoundVSync() to sync the sound engine.
 *
 * Why line 150 specifically? The sound engine needs to know EXACTLY
 * when VBlank will happen so it can queue the right amount of PCM
 * data. Line 150 gives it 10 scanlines (~67 microseconds) of advance
 * notice to prepare the next audio buffer.
 */
void EnableVCountIntrAtLine150(void)
{
    u16 gpuReg = (GetGpuReg(REG_OFFSET_DISPSTAT) & 0xFF) | (150 << 8);
    SetGpuReg(REG_OFFSET_DISPSTAT, gpuReg | DISPSTAT_VCOUNT_INTR);
    EnableInterrupts(INTR_FLAG_VCOUNT);
}

/*
 * InitKeys - Reset all button state tracking.
 *
 * The GBA has 10 buttons: A, B, L, R, Start, Select, Up, Down, Left, Right.
 * Their state is read from REG_KEYINPUT (0x04000130) - a 10-bit register.
 *
 * The game tracks several derived states:
 *   heldKeysRaw: Buttons currently held (raw hardware state)
 *   heldKeys:    Same, but with L→A remapping if enabled in options
 *   newKeysRaw:  Buttons that were JUST pressed this frame (not held last frame)
 *   newKeys:     Same, with L→A remapping
 *   newAndRepeatedKeys: newKeys + held buttons that triggered key repeat
 */
void InitKeys(void)
{
    gKeyRepeatContinueDelay = 5;
    gKeyRepeatStartDelay = 40;

    gMain.heldKeys = 0;
    gMain.newKeys = 0;
    gMain.newAndRepeatedKeys = 0;
    gMain.heldKeysRaw = 0;
    gMain.newKeysRaw = 0;
}

/*
 * ReadKeys - Sample the hardware button register and compute key states.
 *
 * REG_KEYINPUT is a ACTIVE-LOW register: 0 = pressed, 1 = released.
 * XOR with KEYS_MASK (0x3FF) inverts it to ACTIVE-HIGH: 1 = pressed.
 *
 * Key states computed:
 *   newKeysRaw = buttons pressed THIS frame but NOT last frame
 *              = currentInput & ~previousInput (rising edge detection)
 *   newKeys = same (may be remapped later for L=A mode)
 *   newAndRepeatedKeys = newKeys initially, but if the same buttons are
 *     held for gKeyRepeatStartDelay frames, it starts "repeating" them
 *     every gKeyRepeatContinueDelay frames (like holding a key on a keyboard)
 *
 * L=A button mode: An accessibility option where pressing L acts as A.
 * Useful for one-handed play. Applied AFTER raw key sampling.
 */
static void ReadKeys(void)
{
    u16 keyInput = REG_KEYINPUT ^ KEYS_MASK;
    gMain.newKeysRaw = keyInput & ~gMain.heldKeysRaw;
    gMain.newKeys = gMain.newKeysRaw;
    gMain.newAndRepeatedKeys = gMain.newKeysRaw;

    // BUG: Key repeat won't work when pressing L using L=A button mode
    // because it compares the raw key input with the remapped held keys.
    // Note that newAndRepeatedKeys is never remapped either.

    if (keyInput != 0 && gMain.heldKeys == keyInput)
    {
        gMain.keyRepeatCounter--;

        if (gMain.keyRepeatCounter == 0)
        {
            gMain.newAndRepeatedKeys = keyInput;
            gMain.keyRepeatCounter = gKeyRepeatContinueDelay;
        }
    }
    else
    {
        // If there is no input or the input has changed, reset the counter.
        gMain.keyRepeatCounter = gKeyRepeatStartDelay;
    }

    gMain.heldKeysRaw = keyInput;
    gMain.heldKeys = gMain.heldKeysRaw;

    // Remap L to A if the L=A option is enabled.
    if (gSaveBlock2Ptr->optionsButtonMode == OPTIONS_BUTTON_MODE_L_EQUALS_A)
    {
        if (JOY_NEW(L_BUTTON))
            gMain.newKeys |= A_BUTTON;

        if (JOY_HELD(L_BUTTON))
            gMain.heldKeys |= A_BUTTON;
    }

    if (JOY_NEW(gMain.watchedKeysMask))
        gMain.watchedKeysPressed = TRUE;
}

/*
 * InitIntrHandlers - Set up the GBA interrupt system.
 *
 * The GBA interrupt system works as follows:
 *
 * 1. Hardware detects an interrupt condition (e.g., VBlank)
 * 2. CPU saves current state and jumps to 0x00000018 (ARM exception vector)
 * 3. BIOS handler at 0x00000018 reads INTR_VECTOR (0x03007FFC)
 * 4. Jumps to the address stored there (IntrMain_Buffer)
 * 5. IntrMain_Buffer (our dispatcher) reads REG_IE & REG_IF
 * 6. Finds highest-priority pending interrupt
 * 7. Calls the corresponding function from gIntrTable[]
 * 8. Handler acknowledges the interrupt (writes to REG_IF)
 * 9. Returns; CPU restores state and continues
 *
 * Key registers:
 *   REG_IME (0x04000208): Master interrupt enable. 1 = interrupts enabled.
 *   REG_IE  (0x04000200): Interrupt Enable - which interrupts are allowed.
 *   REG_IF  (0x04000202): Interrupt Flags - which interrupts have fired.
 *     To acknowledge an interrupt, write 1 to its bit in REG_IF.
 *
 * DmaCopy32 copies the interrupt dispatcher from ROM to IWRAM.
 * ROM access requires wait states; IWRAM access is instant.
 * This matters because interrupt dispatch must be as fast as possible.
 *
 * INTR_VECTOR (0x03007FFC) is the BIOS's pointer to the interrupt handler.
 * The BIOS reads this address when any interrupt fires.
 */
void InitIntrHandlers(void)
{
    int i;

    for (i = 0; i < INTR_COUNT; i++)
        gIntrTable[i] = gIntrTableTemplate[i];

    DmaCopy32(3, intr_main, IntrMain_Buffer, sizeof(IntrMain_Buffer));

    INTR_VECTOR = IntrMain_Buffer;

    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    SetSerialCallback(NULL);

    /* Enable the master interrupt switch. Without this, NO interrupts fire. */
    REG_IME = 1;

    /* Enable VBlank interrupt specifically. Other interrupts are enabled
     * later as needed (Serial, Timer, HBlank, VCount). */
    EnableInterrupts(INTR_FLAG_VBLANK);
}

/*
 * Interrupt callback setters.
 * These set function pointers in gMain that are called from the
 * corresponding interrupt handlers below. Setting to NULL disables
 * the callback (the interrupt still fires but does nothing).
 *
 * This two-level design (hardware interrupt → callback function pointer)
 * allows different game states to install their own interrupt handlers
 * without modifying the interrupt table directly.
 */
void SetVBlankCallback(IntrCallback callback)
{
    gMain.vblankCallback = callback;
}

void SetHBlankCallback(IntrCallback callback)
{
    gMain.hblankCallback = callback;
}

void SetVCountCallback(IntrCallback callback)
{
    gMain.vcountCallback = callback;
}

void SetSerialCallback(IntrCallback callback)
{
    gMain.serialCallback = callback;
}

extern void CopyBufferedValuesToGpuRegs(void);
extern void ProcessDma3Requests(void);

/*
 * VBlankIntr - THE most important interrupt handler.
 *
 * Fires once per frame when the display enters the vertical blank period
 * (scanline 160-227). During VBlank, the GPU is not reading VRAM/OAM/palettes,
 * so it's SAFE to modify them without causing visual artifacts (tearing).
 *
 * This ~4.5ms window is when ALL visible hardware updates happen:
 *
 * 1. Link cable sync (LinkVSync/RfuVSync):
 *    The serial transfer state machine advances. For multiplayer,
 *    the master initiates a new SIO transfer each VBlank.
 *
 * 2. VBlank counter increment:
 *    vblankCounter1 is an optional external counter (used for timing).
 *    vblankCounter2 is always incremented - it's the global frame counter.
 *
 * 3. User VBlank callback:
 *    Whatever the current game state needs done during VBlank.
 *    Typically: LoadOam() (copy sprite buffer → OAM hardware),
 *    ProcessSpriteCopyRequests() (decompress sprite graphics → VRAM),
 *    TransferPlttBuffer() (copy palette buffer → palette RAM).
 *
 * 4. GPU register sync:
 *    CopyBufferedValuesToGpuRegs() writes any pending GPU register
 *    changes. This prevents mid-frame register writes.
 *
 * 5. DMA3 processing:
 *    ProcessDma3Requests() runs queued DMA transfers (VRAM copies).
 *    DMA3 is the workhorse for bulk data movement to VRAM.
 *
 * 6. Sound engine:
 *    m4aSoundMain() mixes the next frame's audio and queues it for
 *    DMA playback. This MUST happen every VBlank or audio stutters.
 *
 * 7. Misc:
 *    TryReceiveLinkBattleData() checks for incoming battle data.
 *    Random() advances the PRNG (ensures randomness even in menus).
 *    UpdateWirelessStatusIndicatorSprite() animates the wireless icon.
 *
 * INTR_CHECK and gMain.intrCheck acknowledge the interrupt.
 * INTR_CHECK (0x03007FF8) is read by the BIOS to know which
 * interrupts have been serviced. gMain.intrCheck is used by
 * WaitForVBlank() to know when the current frame's VBlank happened.
 */
static void VBlankIntr(void)
{
    if (gWirelessCommType)
        RfuVSync();
    else if (!gLinkVSyncDisabled)
        LinkVSync();

    if (gMain.vblankCounter1)
        (*gMain.vblankCounter1)++;

    if (gMain.vblankCallback)
        gMain.vblankCallback();

    gMain.vblankCounter2++;

    CopyBufferedValuesToGpuRegs();
    ProcessDma3Requests();

    gPcmDmaCounter = gSoundInfo.pcmDmaCounter;

#ifndef NDEBUG
    sVcountBeforeSound = REG_VCOUNT;
#endif
    m4aSoundMain();
#ifndef NDEBUG
    sVcountAfterSound = REG_VCOUNT;
#endif

    TryReceiveLinkBattleData();
    Random();
    UpdateWirelessStatusIndicatorSprite();

    INTR_CHECK |= INTR_FLAG_VBLANK;
    gMain.intrCheck |= INTR_FLAG_VBLANK;
}

/*
 * InitFlashTimer - Set up Timer 2 for flash memory write timing.
 *
 * Flash memory writes require precise timing. The GBA's flash chip
 * needs specific delays between write commands. Timer 2 provides
 * this timing by firing an interrupt after the required delay.
 * SetFlashTimerIntr() configures the timer and installs a handler
 * in the interrupt table.
 */
void InitFlashTimer(void)
{
    IntrFunc **func = (IntrFunc **)&sTimerIntrFunc;
    SetFlashTimerIntr(2, *func);
}

/*
 * HBlankIntr - Fires at the end of each visible scanline (0-159).
 *
 * Used for "raster effects" - changing hardware state mid-frame to
 * create effects impossible with static registers:
 *  - Wave distortion (battle transitions): Change BG scroll each line
 *  - Gradient skies: Change palette colors each line
 *  - Window effects: Change window dimensions each line
 *  - Parallax scrolling: Change BG scroll speed each line
 *
 * The callback is typically set by ScanlineEffect_InitHBlankDmaTransfer()
 * which uses DMA to copy register values each scanline.
 *
 * HBlank interrupts fire 160 times per frame (once per visible line).
 * The handler must be EXTREMELY fast - there's only ~68 CPU cycles
 * available before the next scanline starts drawing.
 */
static void HBlankIntr(void)
{
    if (gMain.hblankCallback)
        gMain.hblankCallback();

    INTR_CHECK |= INTR_FLAG_HBLANK;
    gMain.intrCheck |= INTR_FLAG_HBLANK;
}

/*
 * VCountIntr - Fires when the scanline counter matches a preset value.
 *
 * Currently configured to fire at scanline 150 (see EnableVCountIntrAtLine150).
 * The sole purpose is to call m4aSoundVSync() which tells the sound engine
 * that VBlank is about to happen. This allows the sound mixer to prepare
 * the exact right amount of PCM data for the next frame.
 *
 * REG_VCOUNT (0x04000006) contains the current scanline number (0-227).
 * In debug builds, we capture this value to measure sound engine timing.
 */
static void VCountIntr(void)
{
#ifndef NDEBUG
    sVcountAtIntr = REG_VCOUNT;
#endif
    m4aSoundVSync();
    INTR_CHECK |= INTR_FLAG_VCOUNT;
    gMain.intrCheck |= INTR_FLAG_VCOUNT;
}

/*
 * SerialIntr - Fires when a serial I/O (link cable) transfer completes.
 *
 * The GBA's SIO (Serial I/O) port supports multiple communication modes:
 *  - Normal: 8-bit or 32-bit bidirectional (2 players)
 *  - Multi-Player: 16-bit broadcast (up to 4 players)
 *  - UART: Asynchronous serial (not used in games)
 *  - GPIO: General purpose I/O (used by RTC in some carts)
 *
 * In Multi-Player mode (used by this game):
 *  - Player 0 is the master, others are slaves
 *  - Master writes to REG_SIOMLT_SEND and sets the START bit
 *  - Hardware simultaneously exchanges all players' SEND values
 *  - After transfer, SIOMULTI0-3 contain all players' sent data
 *  - This interrupt fires when the exchange is complete
 *
 * The callback (gMain.serialCallback) is set to:
 *  - SerialCB (link.c) for normal link play
 *  - MP_SerialCallback (multiplayer.c) for our custom multiplayer
 *  - NULL when no link activity is expected
 */
static void SerialIntr(void)
{
    if (gMain.serialCallback)
        gMain.serialCallback();

    INTR_CHECK |= INTR_FLAG_SERIAL;
    gMain.intrCheck |= INTR_FLAG_SERIAL;
}

/*
 * RestoreSerialTimer3IntrHandlers - Reset interrupt table entries.
 *
 * Some code (like the flash driver) temporarily replaces interrupt
 * handlers. This function restores the Serial and Timer3 entries
 * to their standard handlers. Called after flash operations complete.
 */
void RestoreSerialTimer3IntrHandlers(void)
{
    gIntrTable[1] = SerialIntr;
    gIntrTable[2] = Timer3Intr;
}

/*
 * IntrDummy - Placeholder for unused interrupt slots.
 * Interrupts that fire with this handler are silently ignored.
 */
static void IntrDummy(void)
{}

/*
 * WaitForVBlank - Busy-wait until the VBlank interrupt fires.
 *
 * This is called at the end of each main loop iteration.
 * It clears the VBlank flag, then spins until VBlankIntr() sets it.
 *
 * The spin loop is intentional - there's no "sleep until interrupt"
 * that works reliably on GBA (the BIOS Halt function exists but has
 * caveats with DMA). Busy-waiting wastes power but the GBA is
 * battery-powered with a fixed refresh rate, so there's no benefit
 * to sleeping.
 *
 * If the game logic took longer than one frame (~16.7ms), the VBlank
 * interrupt already fired during processing, and intrCheck already
 * has the flag set. In this case, the clear + check loop exits
 * immediately on the next VBlank - effectively dropping a frame.
 */
static void WaitForVBlank(void)
{
    gMain.intrCheck &= ~INTR_FLAG_VBLANK;

    while (!(gMain.intrCheck & INTR_FLAG_VBLANK))
        ;
}

void SetVBlankCounter1Ptr(u32 *ptr)
{
    gMain.vblankCounter1 = ptr;
}

void DisableVBlankCounter1(void)
{
    gMain.vblankCounter1 = NULL;
}

/*
 * DoSoftReset - Perform a warm reboot of the GBA.
 *
 * Steps:
 * 1. REG_IME = 0: Disable ALL interrupts (prevent handlers from firing
 *    during shutdown, which could access freed memory)
 * 2. m4aSoundVSyncOff: Detach sound engine from VSync
 * 3. ScanlineEffect_Stop: Disable HBlank DMA effects
 * 4. DmaStop(1/2/3): Halt all DMA channels. DMA0 is not stopped because
 *    it may be used by the BIOS
 * 5. SoftReset: BIOS call (SWI 0x00) that:
 *    - Resets the CPU state (registers, stack pointer)
 *    - Clears 0x03007E00-0x03007FFF in IWRAM (BIOS work area)
 *    - Jumps to 0x08000000 (ROM entry point) or 0x02000000 (multiboot)
 *    RESET_SIO_REGS is excluded to preserve link cable state during reset
 */
void DoSoftReset(void)
{
    REG_IME = 0;
    m4aSoundVSyncOff();
    ScanlineEffect_Stop();
    DmaStop(1);
    DmaStop(2);
    DmaStop(3);
    SoftReset(RESET_ALL & ~RESET_SIO_REGS);
}

/*
 * ClearPokemonCrySongs - Zero out the Pokemon cry sound data.
 *
 * Pokemon cries are stored as song-like structures in the sound engine.
 * CpuFill16 uses a CPU loop to fill memory with a 16-bit value (0).
 * This is used instead of DMA because DMA can't fill arbitrary values
 * efficiently (DMA copies FROM a source address, not a constant).
 *
 * CpuFill16/CpuCopy16 are BIOS-provided memory utilities (SWI 0x0C/0x0B)
 * that are faster than naive C loops because they use optimized ARM
 * instructions (STMIA for block stores).
 */
void ClearPokemonCrySongs(void)
{
    CpuFill16(0, gPokemonCrySongs, MAX_POKEMON_CRIES * sizeof(struct PokemonCrySong));
}
