/**
 * =EVOLUTION SCENE=
 *
 * FILE OVERVIEW:
 * This file implements the evolution animation sequence — the dramatic scene
 * that plays when a Pokemon evolves into a new species. It handles:
 *   - The pre-evolution sprite display and "What? [name] is evolving!" message
 *   - The morphing animation where the sprite cycles between old and new forms
 *   - Sparkle particle effects (spirals, arcs, circles, sprays)
 *   - The animated background with color-cycling palette effects
 *   - Evolution success/cancel handling (player can press B to stop)
 *   - New move learning after evolution
 *   - Special case: Nincada → Ninjask creates a bonus Shedinja in the party
 *   - Trade evolution variant (same logic but within the trade UI)
 *
 * GBA CONTEXT:
 * The evolution animation is one of the most visually complex scenes in the
 * game. It uses several GBA hardware features:
 *   - SPRITE AFFINE TRANSFORMS: The morphing effect uses sprite scaling
 *   - PALETTE CYCLING: The background color smoothly transitions through
 *     a gradient of colors by updating palette entries each frame
 *   - HARDWARE MOSAIC: The REG_OFFSET_MOSAIC register is used for pixelation
 *   - MULTIPLE BG LAYERS: BG3 scrolls horizontally for the moving background
 *
 * STATE MACHINE:
 * The evolution uses a large state machine (Task_EvolutionScene) with ~26
 * states that progress through the animation sequence. Each state handles
 * one phase (fade in, show message, start music, run sparkles, etc.) and
 * transitions to the next state by incrementing tState.
 */
#include "global.h"
#include "gflib.h"
#include "battle.h"
#include "battle_message.h"
#include "data.h"
#include "decompress.h"
#include "help_system.h"
#include "evolution_scene.h"
#include "evolution_graphics.h"
#include "link.h"
#include "link_rfu.h"
#include "m4a.h"
#include "event_data.h"
#include "trade_scene.h"
#include "new_menu_helpers.h"
#include "menu.h"
#include "overworld.h"
#include "pokedex.h"
#include "pokemon_summary_screen.h"
#include "scanline_effect.h"
#include "strings.h"
#include "task.h"
#include "text_window.h"
#include "trig.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/pokemon.h"
#include "constants/items.h"

/* The evolution table maps each species to its possible evolutions.
 * Each species can have up to EVOS_PER_MON evolution entries, each
 * specifying a method (level-up, stone, trade, etc.) and target species. */
extern struct Evolution gEvolutionTable[][EVOS_PER_MON];

/**
 * EvoInfo — persistent state for the evolution animation.
 *
 * This struct lives in EWRAM and tracks the sprite IDs for the pre- and
 * post-evolution Pokemon, the task driving the animation, and a saved copy
 * of the background palette (so it can be restored after the animation).
 */
struct EvoInfo
{
    u8 preEvoSpriteId;      /* Sprite ID of the pre-evolution Pokemon sprite */
    u8 postEvoSpriteId;     /* Sprite ID of the post-evolution Pokemon sprite */
    u8 evoTaskId;           /* Task ID running the evolution state machine */
    u8 delayTimer;          /* General-purpose delay counter for animation timing */
    u16 savedPalette[48];   /* Backup of 3 background palettes (3 * 16 colors) */
};

/* EWRAM variables — allocated in the GBA's 256KB external work RAM. */
static EWRAM_DATA struct EvoInfo *sEvoStructPtr = NULL;  /* Main evolution state */
static EWRAM_DATA u16 *sBgAnimPal = NULL;                /* Working palette for BG animation */

/* IWRAM common — stored in the faster 32KB internal work RAM.
 * This callback determines where to go after the evolution scene ends
 * (back to the overworld, back to battle, back to the trade screen, etc.). */
COMMON_DATA void (*gCB2_AfterEvolution)(void) = NULL;

/* Aliases into gBattleCommunication[] for evolution-specific state.
 * The battle communication array is reused here since the evolution scene
 * borrows the battle UI infrastructure. */
#define sEvoCursorPos           gBattleCommunication[1] /* Cursor position in move-learning menu */
#define sEvoGraphicsTaskId      gBattleCommunication[2] /* Task ID for sparkle/effects graphics */

// this file's functions
static void Task_EvolutionScene(u8 taskId);
static void Task_TradeEvolutionScene(u8 taskId);
static void CB2_EvolutionSceneUpdate(void);
static void CB2_TradeEvolutionSceneUpdate(void);
static void EvoDummyFunc(void);
static void VBlankCB_EvolutionScene(void);
static void VBlankCB_TradeEvolutionScene(void);
static void StartBgAnimation(bool8 isLink);
static void StopBgAnimation(void);
static void Task_AnimateBg(u8 taskId);
static void RestoreBgAfterAnim(void);

// const data
static const u16 sUnusedPal[] = INCBIN_U16("graphics/evolution_scene/unused.gbapal");
static const u32 sMovingBackgroundTiles[] = INCBIN_U32("graphics/evolution_scene/bg.4bpp.lz");
static const u32 sMovingBackgroundMap1[] = INCBIN_U32("graphics/evolution_scene/bg.bin.lz");
static const u32 sMovingBackgroundMap2[] = INCBIN_U32("graphics/evolution_scene/bg2.bin.lz");
static const u16 sBlackPalette[] = INCBIN_U16("graphics/evolution_scene/gray_transition_intro.gbapal");
static const u16 sUnusedTilemap[] = INCBIN_U16("graphics/evolution_scene/unused_tilemap.bin");
static const u16 sBgAnim_Pal[] = INCBIN_U16("graphics/evolution_scene/transition.gbapal");

static const u8 sText_ShedinjaJapaneseName[] = _("ヌケニン");

static const u8 sText_UnusedColors[] = _("{COLOR DARK_GRAY}{HIGHLIGHT WHITE}{SHADOW LIGHT_GRAY}");

static const u8 sText_UnusedArrows[][10] = {
    _("▶\n "),
    _(" \n▶"),
    _(" \n ")
};

/**
 * Background animation palette control table.
 *
 * Each row defines one phase of the background color cycling animation:
 *   [0] = start index into sBgAnim_PalIndexes (which row to begin at)
 *   [1] = end index (which row to cycle up to before restarting)
 *   [2] = number of times to repeat this cycle
 *   [3] = delay (frames) between each palette index increment
 *
 * The animation progresses through these phases sequentially:
 *   Phase 0: Slow fade from black to blue (rows 0-12, 1 cycle, 6 frame delay)
 *   Phase 1: Fast blue pulsing (rows 13-36, 5 cycles, 2 frame delay)
 *   Phase 2: Quick blue pulse (rows 13-24, 1 cycle, 2 frame delay)
 *   Phase 3: Slow fade from blue to black (rows 37-49, 1 cycle, 6 frame delay)
 */
static const u8 sBgAnim_PaletteControl[][4] =
{
    {  0, 12, 1, 6 },
    { 13, 36, 5, 2 },
    { 13, 24, 1, 2 },
    { 37, 49, 1, 6 },
};

// Indexes into sBgAnim_Pal, 0 is black, transitioning to a bright light blue (172, 213, 255) at 13
static const u8 sBgAnim_PalIndexes[][16] = {
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  0,  0 },
    {  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0,  0 },
    {  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,  0,  0 },
    {  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0, 11,  0,  0 },
    {  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12,  0,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,  0,  0 },
    {  0,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 12,  0,  0 },
    {  0,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 12, 11,  0,  0 },
    {  0,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 12, 11, 10,  0,  0 },
    {  0,  5,  6,  7,  8,  9, 10, 11, 12, 13, 12, 11, 10,  9,  0,  0 },
    {  0,  6,  7,  8,  9, 10, 11, 12, 13, 12, 11, 10,  9,  8,  0,  0 },
    {  0,  7,  8,  9, 10, 11, 12, 13, 12, 11, 10,  9,  8,  7,  0,  0 },
    {  0,  8,  9, 10, 11, 12, 13, 12, 11, 10,  9,  8,  7,  6,  0,  0 },
    {  0,  9, 10, 11, 12, 13, 12, 11, 10,  9,  8,  7,  6,  5,  0,  0 },
    {  0, 10, 11, 12, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  0,  0 },
    {  0, 11, 12, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  0,  0 },
    {  0, 12, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  0,  0 },
    {  0, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0 },
    {  0, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  2,  0,  0 },
    {  0, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  2,  3,  0,  0 },
    {  0, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  2,  3,  4,  0,  0 },
    {  0,  9,  8,  7,  6,  5,  4,  3,  2,  1,  2,  3,  4,  5,  0,  0 },
    {  0,  8,  7,  6,  5,  4,  3,  2,  1,  2,  3,  4,  5,  6,  0,  0 },
    {  0,  7,  6,  5,  4,  3,  2,  1,  2,  3,  4,  5,  6,  7,  0,  0 },
    {  0,  6,  5,  4,  3,  2,  1,  2,  3,  4,  5,  6,  7,  8,  0,  0 },
    {  0,  5,  4,  3,  2,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0,  0 },
    {  0,  4,  3,  2,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,  0,  0 },
    {  0,  3,  2,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,  0,  0 },
    {  0,  2,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12,  0,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,  0,  0 },
    {  0, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0 },
    {  0, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0,  0 },
    {  0, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0,  0,  0 },
    {  0,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0,  0,  0,  0 },
    {  0,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  6,  5,  4,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  5,  4,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  4,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 }
};

static void CB2_BeginEvolutionScene(void)
{
    UpdatePaletteFade();
    RunTasks();
}

#define tState              data[0]
#define tPreEvoSpecies      data[1]
#define tPostEvoSpecies     data[2]
#define tCanStop            data[3]
#define tBits               data[3]
#define tLearnsFirstMove    data[4]
#define tLearnMoveState     data[6]
#define tLearnMoveYesState  data[7]
#define tLearnMoveNoState   data[8]
#define tEvoWasStopped      data[9]
#define tPartyId            data[10]

#define TASK_BIT_CAN_STOP       (1 << 0)
#define TASK_BIT_LEARN_MOVE     (1 << 7)

static void Task_BeginEvolutionScene(u8 taskId)
{
    struct Pokemon* mon = NULL;
    switch (gTasks[taskId].tState)
    {
    case 0:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
        gTasks[taskId].tState++;
        break;
    case 1:
        if (!gPaletteFade.active)
        {
            u16 postEvoSpecies;
            bool8 canStopEvo;
            u8 partyId;

            mon = &gPlayerParty[gTasks[taskId].tPartyId];
            postEvoSpecies = gTasks[taskId].tPostEvoSpecies;
            canStopEvo = gTasks[taskId].tCanStop;
            partyId = gTasks[taskId].tPartyId;

            DestroyTask(taskId);
            EvolutionScene(mon, postEvoSpecies, canStopEvo, partyId);
        }
        break;
    }
}

/**
 * FUNCTION: BeginEvolutionScene
 *
 * PURPOSE: Entry point for starting an evolution from the overworld (e.g.,
 * after gaining a level). Creates a transition task that fades to black,
 * then hands off to EvolutionScene for the full animation.
 *
 * @param mon — pointer to the Pokemon that is evolving
 * @param postEvoSpecies — the species it will evolve into
 * @param canStopEvo — TRUE if the player can press B to cancel (FALSE for trade evolutions)
 * @param partyId — index of the Pokemon in the player's party (0-5)
 */
void BeginEvolutionScene(struct Pokemon* mon, u16 postEvoSpecies, bool8 canStopEvo, u8 partyId)
{
    u8 taskId = CreateTask(Task_BeginEvolutionScene, 0);
    gTasks[taskId].tState = 0;
    gTasks[taskId].tPostEvoSpecies = postEvoSpecies;
    gTasks[taskId].tCanStop = canStopEvo;
    gTasks[taskId].tPartyId = partyId;
    SetMainCallback2(CB2_BeginEvolutionScene);
}

/**
 * FUNCTION: EvolutionScene
 *
 * PURPOSE: Sets up the full evolution scene — clears the screen, loads both
 * the pre-evolution and post-evolution Pokemon sprites, initializes the
 * battle-style background, and starts the evolution state machine.
 *
 * HOW IT WORKS:
 * 1. Clears ALL of VRAM (Video RAM, 96KB at 0x06000000) to start with a blank screen
 * 2. Resets all window registers (no masking effects active)
 * 3. Decompresses and creates sprites for both the old and new Pokemon forms
 * 4. Loads each sprite's palette into separate OBJ palette slots
 *    (slot 1 for pre-evo, slot 2 for post-evo) so they can be shown independently
 * 5. Both sprites start invisible — the state machine reveals them in sequence
 * 6. Saves the current BG palette so it can be restored after the animation
 * 7. Starts playing no music (m4aMPlayAllStop) for dramatic silence
 *
 * GBA CONTEXT:
 * VRAM is the 96KB of memory at 0x06000000 used for all graphics data on the
 * GBA (tile data, tilemaps, sprite graphics). CpuFill32 fills it with zeros,
 * which clears all visible graphics. The OBJ palette slots (OBJ_PLTT_ID) are
 * in palette RAM at 0x05000200 — each slot holds 16 colors for sprites.
 *
 * @param mon — the evolving Pokemon
 * @param postEvoSpecies — target species
 * @param canStopEvo — whether B button can cancel
 * @param partyId — party slot index
 */
void EvolutionScene(struct Pokemon* mon, u16 postEvoSpecies, bool8 canStopEvo, u8 partyId)
{
    u8 name[20];
    u16 currSpecies;
    u32 trainerId, personality;
    const struct CompressedSpritePalette* pokePal;
    u8 id;

    SetHBlankCallback(NULL);
    SetVBlankCallback(NULL);
    /* Clear all 96KB of VRAM — erases all tile data, tilemaps, and sprite
     * graphics from the previous screen. */
    CpuFill32(0, (void *)(VRAM), VRAM_SIZE);

    SetGpuReg(REG_OFFSET_MOSAIC, 0);
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_WIN1H, 0);
    SetGpuReg(REG_OFFSET_WIN1V, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);

    ResetPaletteFade();

    gBattle_BG0_X = 0;
    gBattle_BG0_Y = 0;
    gBattle_BG1_X = 0;
    gBattle_BG1_Y = 0;
    gBattle_BG2_X = 0;
    gBattle_BG2_Y = 0;
    gBattle_BG3_X = 256;
    gBattle_BG3_Y = 0;

    gBattleTerrain = BATTLE_TERRAIN_PLAIN;

    InitBattleBgsVideo();
    LoadBattleTextboxAndBackground();
    ResetSpriteData();
    ScanlineEffect_Stop();
    ResetTasks();
    FreeAllSpritePalettes();

    gReservedSpritePaletteCount = 4;

    sEvoStructPtr = AllocZeroed(sizeof(struct EvoInfo));
    AllocateMonSpritesGfx();

    GetMonData(mon, MON_DATA_NICKNAME, name);
    StringCopy_Nickname(gStringVar1, name);
    StringCopy(gStringVar2, gSpeciesNames[postEvoSpecies]);

    // preEvo sprite
    currSpecies = GetMonData(mon, MON_DATA_SPECIES);
    trainerId = GetMonData(mon, MON_DATA_OT_ID);
    personality = GetMonData(mon, MON_DATA_PERSONALITY);
    DecompressPicFromTable(&gMonFrontPicTable[currSpecies],
                             gMonSpritesGfxPtr->sprites[B_POSITION_OPPONENT_LEFT],
                             currSpecies);
    pokePal = GetMonSpritePalStructFromOtIdPersonality(currSpecies, trainerId, personality);
    LoadCompressedPalette(pokePal->data, OBJ_PLTT_ID(1), PLTT_SIZE_4BPP);

    SetMultiuseSpriteTemplateToPokemon(currSpecies, B_POSITION_OPPONENT_LEFT);
    gMultiuseSpriteTemplate.affineAnims = gDummySpriteAffineAnimTable;
    sEvoStructPtr->preEvoSpriteId = id = CreateSprite(&gMultiuseSpriteTemplate, 120, 64, 30);

    gSprites[id].callback = SpriteCallbackDummy_2;
    gSprites[id].oam.paletteNum = 1;
    gSprites[id].invisible = TRUE;

    // postEvo sprite
    DecompressPicFromTable(&gMonFrontPicTable[postEvoSpecies],
                             gMonSpritesGfxPtr->sprites[B_POSITION_OPPONENT_RIGHT],
                             postEvoSpecies);
    pokePal = GetMonSpritePalStructFromOtIdPersonality(postEvoSpecies, trainerId, personality);
    LoadCompressedPalette(pokePal->data, OBJ_PLTT_ID(2), PLTT_SIZE_4BPP);

    SetMultiuseSpriteTemplateToPokemon(postEvoSpecies, B_POSITION_OPPONENT_RIGHT);
    gMultiuseSpriteTemplate.affineAnims = gDummySpriteAffineAnimTable;
    sEvoStructPtr->postEvoSpriteId = id = CreateSprite(&gMultiuseSpriteTemplate, 120, 64, 30);
    gSprites[id].callback = SpriteCallbackDummy_2;
    gSprites[id].oam.paletteNum = 2;
    gSprites[id].invisible = TRUE;

    LoadEvoSparkleSpriteAndPal();

    sEvoStructPtr->evoTaskId = id = CreateTask(Task_EvolutionScene, 0);
    gTasks[id].tState = 0;
    gTasks[id].tPreEvoSpecies = currSpecies;
    gTasks[id].tPostEvoSpecies = postEvoSpecies;
    gTasks[id].tCanStop = canStopEvo;
    gTasks[id].tLearnsFirstMove = TRUE;
    gTasks[id].tEvoWasStopped = FALSE;
    gTasks[id].tPartyId = partyId;

    memcpy(&sEvoStructPtr->savedPalette, &gPlttBufferUnfaded[BG_PLTT_ID(2)], sizeof(sEvoStructPtr->savedPalette));

    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_BG_ALL_ON | DISPCNT_OBJ_1D_MAP);

    SetHBlankCallback(EvoDummyFunc);
    SetVBlankCallback(VBlankCB_EvolutionScene);
    m4aMPlayAllStop();
    HelpSystem_Disable();
    SetMainCallback2(CB2_EvolutionSceneUpdate);
}

static void CB2_EvolutionSceneLoadGraphics(void)
{
    u8 id;
    const struct CompressedSpritePalette* pokePal;
    u16 postEvoSpecies;
    u32 trainerId, personality;
    struct Pokemon* mon = &gPlayerParty[gTasks[sEvoStructPtr->evoTaskId].tPartyId];

    postEvoSpecies = gTasks[sEvoStructPtr->evoTaskId].tPostEvoSpecies;
    trainerId = GetMonData(mon, MON_DATA_OT_ID);
    personality = GetMonData(mon, MON_DATA_PERSONALITY);

    SetHBlankCallback(NULL);
    SetVBlankCallback(NULL);
    CpuFill32(0, (void *)(VRAM), VRAM_SIZE);

    SetGpuReg(REG_OFFSET_MOSAIC, 0);
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_WIN1H, 0);
    SetGpuReg(REG_OFFSET_WIN1V, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);

    ResetPaletteFade();

    gBattle_BG0_X = 0;
    gBattle_BG0_Y = 0;
    gBattle_BG1_X = 0;
    gBattle_BG1_Y = 0;
    gBattle_BG2_X = 0;
    gBattle_BG2_Y = 0;
    gBattle_BG3_X = 256;
    gBattle_BG3_Y = 0;

    gBattleTerrain = BATTLE_TERRAIN_PLAIN;

    InitBattleBgsVideo();
    LoadBattleTextboxAndBackground();
    ResetSpriteData();
    FreeAllSpritePalettes();
    gReservedSpritePaletteCount = 4;

    DecompressPicFromTable(&gMonFrontPicTable[postEvoSpecies],
                             gMonSpritesGfxPtr->sprites[B_POSITION_OPPONENT_RIGHT],
                             postEvoSpecies);
    pokePal = GetMonSpritePalStructFromOtIdPersonality(postEvoSpecies, trainerId, personality);

    LoadCompressedPalette(pokePal->data, OBJ_PLTT_ID(2), PLTT_SIZE_4BPP);

    SetMultiuseSpriteTemplateToPokemon(postEvoSpecies, B_POSITION_OPPONENT_RIGHT);
    gMultiuseSpriteTemplate.affineAnims = gDummySpriteAffineAnimTable;
    sEvoStructPtr->postEvoSpriteId = id = CreateSprite(&gMultiuseSpriteTemplate, 120, 64, 30);

    gSprites[id].callback = SpriteCallbackDummy_2;
    gSprites[id].oam.paletteNum = 2;

    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_BG_ALL_ON | DISPCNT_OBJ_1D_MAP);

    SetHBlankCallback(EvoDummyFunc);
    SetVBlankCallback(VBlankCB_EvolutionScene);
    SetMainCallback2(CB2_EvolutionSceneUpdate);

    BeginNormalPaletteFade(PALETTES_ALL, 0, 0x10, 0, RGB_BLACK);

    ShowBg(0);
    ShowBg(1);
    ShowBg(2);
    ShowBg(3);
}

static void CB2_TradeEvolutionSceneLoadGraphics(void)
{
    struct Pokemon* mon = &gPlayerParty[gTasks[sEvoStructPtr->evoTaskId].tPartyId];
    u16 postEvoSpecies = gTasks[sEvoStructPtr->evoTaskId].tPostEvoSpecies;

    switch (gMain.state)
    {
    case 0:
        SetGpuReg(REG_OFFSET_DISPCNT, 0);
        SetHBlankCallback(NULL);
        SetVBlankCallback(NULL);
        ResetSpriteData();
        FreeAllSpritePalettes();
        gReservedSpritePaletteCount = 4;
        gBattle_BG0_X = 0;
        gBattle_BG0_Y = 0;
        gBattle_BG1_X = 0;
        gBattle_BG1_Y = 0;
        gBattle_BG2_X = 0;
        gBattle_BG2_Y = 0;
        gBattle_BG3_X = 256;
        gBattle_BG3_Y = 0;
        gMain.state++;
        break;
    case 1:
        ResetPaletteFade();
        SetHBlankCallback(EvoDummyFunc);
        SetVBlankCallback(VBlankCB_TradeEvolutionScene);
        gMain.state++;
        break;
    case 2:
        LoadTradeAnimGfx();
        gMain.state++;
        break;
    case 3:
        FillBgTilemapBufferRect(1, 0, 0, 0, 0x20, 0x20, 17);
        CopyBgTilemapBufferToVram(1);
        gMain.state++;
        break;
    case 4:
        {
            const struct CompressedSpritePalette* pokePal;
            u32 trainerId = GetMonData(mon, MON_DATA_OT_ID);
            u32 personality = GetMonData(mon, MON_DATA_PERSONALITY);
            DecompressPicFromTable(&gMonFrontPicTable[postEvoSpecies],
                                     gMonSpritesGfxPtr->sprites[B_POSITION_OPPONENT_RIGHT],
                                     postEvoSpecies);
            pokePal = GetMonSpritePalStructFromOtIdPersonality(postEvoSpecies, trainerId, personality);
            LoadCompressedPalette(pokePal->data, OBJ_PLTT_ID(2), PLTT_SIZE_4BPP);
            gMain.state++;
        }
        break;
    case 5:
        {
            u8 id;

            SetMultiuseSpriteTemplateToPokemon(postEvoSpecies, B_POSITION_OPPONENT_LEFT);
            gMultiuseSpriteTemplate.affineAnims = gDummySpriteAffineAnimTable;
            sEvoStructPtr->postEvoSpriteId = id = CreateSprite(&gMultiuseSpriteTemplate, 120, 64, 30);

            gSprites[id].callback = SpriteCallbackDummy_2;
            gSprites[id].oam.paletteNum = 2;
            gMain.state++;
            LinkTradeDrawWindow();
        }
        break;
    case 6:
        if (gWirelessCommType)
        {
            LoadWirelessStatusIndicatorSpriteGfx();
            CreateWirelessStatusIndicatorSprite(0, 0);
        }
        BlendPalettes(PALETTES_ALL, 0x10, RGB_BLACK);
        gMain.state++;
        break;
    case 7:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0x10, 0, RGB_BLACK);
        InitTradeSequenceBgGpuRegs();
        ShowBg(0);
        ShowBg(1);
        SetMainCallback2(CB2_TradeEvolutionSceneUpdate);
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_OBJ_1D_MAP);
        break;
    }
}

void TradeEvolutionScene(struct Pokemon* mon, u16 postEvoSpecies, u8 preEvoSpriteId, u8 partyId)
{
    u8 name[20];
    u16 currSpecies;
    u32 trainerId, personality;
    const struct CompressedSpritePalette* pokePal;
    u8 id;

    GetMonData(mon, MON_DATA_NICKNAME, name);
    StringCopy_Nickname(gStringVar1, name);
    StringCopy(gStringVar2, gSpeciesNames[postEvoSpecies]);

    gAffineAnimsDisabled = TRUE;

    // preEvo sprite
    currSpecies = GetMonData(mon, MON_DATA_SPECIES);
    personality = GetMonData(mon, MON_DATA_PERSONALITY);
    trainerId = GetMonData(mon, MON_DATA_OT_ID);

    sEvoStructPtr = AllocZeroed(sizeof(struct EvoInfo));
    sEvoStructPtr->preEvoSpriteId = preEvoSpriteId;

    DecompressPicFromTable(&gMonFrontPicTable[postEvoSpecies],
                            gMonSpritesGfxPtr->sprites[B_POSITION_OPPONENT_LEFT],
                            postEvoSpecies);

    pokePal = GetMonSpritePalStructFromOtIdPersonality(postEvoSpecies, trainerId, personality);
    LoadCompressedPalette(pokePal->data, OBJ_PLTT_ID(2), PLTT_SIZE_4BPP);

    SetMultiuseSpriteTemplateToPokemon(postEvoSpecies, B_POSITION_OPPONENT_LEFT);
    gMultiuseSpriteTemplate.affineAnims = gDummySpriteAffineAnimTable;
    sEvoStructPtr->postEvoSpriteId = id = CreateSprite(&gMultiuseSpriteTemplate, 120, 64, 30);

    gSprites[id].callback = SpriteCallbackDummy_2;
    gSprites[id].oam.paletteNum = 2;
    gSprites[id].invisible = TRUE;

    LoadEvoSparkleSpriteAndPal();

    sEvoStructPtr->evoTaskId = id = CreateTask(Task_TradeEvolutionScene, 0);
    gTasks[id].tState = 0;
    gTasks[id].tPreEvoSpecies = currSpecies;
    gTasks[id].tPostEvoSpecies = postEvoSpecies;
    gTasks[id].tLearnsFirstMove = TRUE;
    gTasks[id].tEvoWasStopped = FALSE;
    gTasks[id].tPartyId = partyId;

    gBattle_BG0_X = 0;
    gBattle_BG0_Y = 0;
    gBattle_BG1_X = 0;
    gBattle_BG1_Y = 0;
    gBattle_BG2_X = 0;
    gBattle_BG2_Y = 0;
    gBattle_BG3_X = 256;
    gBattle_BG3_Y = 0;

    gTextFlags.useAlternateDownArrow = TRUE;

    SetVBlankCallback(VBlankCB_TradeEvolutionScene);
    SetMainCallback2(CB2_TradeEvolutionSceneUpdate);
}

static void CB2_EvolutionSceneUpdate(void)
{
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    RunTasks();
}

static void CB2_TradeEvolutionSceneUpdate(void)
{
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    RunTasks();
}

/**
 * FUNCTION: CreateShedinja
 *
 * PURPOSE: Handles the unique Nincada → Ninjask evolution by creating a
 * bonus Shedinja in the player's party if there is room.
 *
 * GAME LOGIC:
 * When Nincada evolves into Ninjask, if the player has an empty party slot
 * (fewer than 6 Pokemon), a Shedinja magically appears in the party. Shedinja
 * is a copy of the original Nincada but with its species changed, stats
 * recalculated, and all ribbons/held items/markings cleared. This is one of
 * the most unusual evolution mechanics in the entire Pokemon series.
 *
 * The function also handles a special case for Japanese-language games where
 * Shedinja gets its Japanese name set explicitly.
 *
 * @param preEvoSpecies — the species before evolution (should be Nincada)
 * @param mon — pointer to the Pokemon that just evolved into Ninjask
 */
static void CreateShedinja(u16 preEvoSpecies, struct Pokemon* mon)
{
    u32 data = 0;
    /* Only create Shedinja if the pre-evo was Nincada (via Ninjask method)
     * AND there's room in the party. */
    if (gEvolutionTable[preEvoSpecies][0].method == EVO_LEVEL_NINJASK && gPlayerPartyCount < PARTY_SIZE)
    {
        s32 i;
        struct Pokemon* shedinja = &gPlayerParty[gPlayerPartyCount];

        CopyMon(&gPlayerParty[gPlayerPartyCount], mon, sizeof(struct Pokemon));
        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_SPECIES, &gEvolutionTable[preEvoSpecies][1].targetSpecies);
        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_NICKNAME, gSpeciesNames[gEvolutionTable[preEvoSpecies][1].targetSpecies]);
        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_HELD_ITEM, &data);
        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_MARKINGS, &data);
        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_ENCRYPT_SEPARATOR, &data);

        for (i = MON_DATA_COOL_RIBBON; i < MON_DATA_COOL_RIBBON + CONTEST_CATEGORIES_COUNT; i++)
            SetMonData(&gPlayerParty[gPlayerPartyCount], i, &data);
        for (i = MON_DATA_CHAMPION_RIBBON; i <= MON_DATA_UNUSED_RIBBONS; i++)
            SetMonData(&gPlayerParty[gPlayerPartyCount], i, &data);

        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_STATUS, &data);
        data = MAIL_NONE;
        SetMonData(&gPlayerParty[gPlayerPartyCount], MON_DATA_MAIL, &data);

        CalculateMonStats(&gPlayerParty[gPlayerPartyCount]);
        CalculatePlayerPartyCount();

        GetSetPokedexFlag(SpeciesToNationalPokedexNum(gEvolutionTable[preEvoSpecies][1].targetSpecies), FLAG_SET_SEEN);
        GetSetPokedexFlag(SpeciesToNationalPokedexNum(gEvolutionTable[preEvoSpecies][1].targetSpecies), FLAG_SET_CAUGHT);

        if (GetMonData(shedinja, MON_DATA_SPECIES) == SPECIES_SHEDINJA
            && GetMonData(shedinja, MON_DATA_LANGUAGE) == LANGUAGE_JAPANESE
            && GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NINJASK)
                SetMonData(shedinja, MON_DATA_NICKNAME, sText_ShedinjaJapaneseName);
    }
}

/**
 * States for the evolution scene state machine (Task_EvolutionScene).
 *
 * The evolution animation progresses through these states in order:
 *   1. FADE_IN → INTRO_MSG: Fade from black, show "What? X is evolving!"
 *   2. INTRO_MON_ANIM → INTRO_SOUND: Animate pre-evo sprite, play cry
 *   3. START_MUSIC: Begin the evolution music track
 *   4. START_BG_AND_SPARKLE_SPIRAL: Show the animated background and
 *      sparkle effects spiraling around the Pokemon
 *   5. CYCLE_MON_SPRITE: The core morphing effect — rapidly alternates
 *      between pre-evo and post-evo sprites
 *   6. SPARKLE_CIRCLE → SPARKLE_SPRAY: More sparkle effects
 *   7. RESTORE_SCREEN → EVO_MON_ANIM: Show the new Pokemon
 *   8. SET_MON_EVOLVED: Actually modify the Pokemon's data to the new species
 *   9. TRY_LEARN_MOVE: Check for and handle new moves learned at this level
 *   10. END: Fade out and return to the previous screen
 *
 *   CANCEL states handle the player pressing B to stop evolution.
 *   REPLACE_MOVE states handle the sub-dialog for learning moves when the
 *   Pokemon already knows 4 moves.
 */
enum {
    EVOSTATE_FADE_IN,
    EVOSTATE_INTRO_MSG,
    EVOSTATE_INTRO_MON_ANIM,
    EVOSTATE_INTRO_SOUND,
    EVOSTATE_START_MUSIC,
    EVOSTATE_START_BG_AND_SPARKLE_SPIRAL,
    EVOSTATE_SPARKLE_ARC,
    EVOSTATE_CYCLE_MON_SPRITE,
    EVOSTATE_WAIT_CYCLE_MON_SPRITE,
    EVOSTATE_SPARKLE_CIRCLE,
    EVOSTATE_SPARKLE_SPRAY,
    EVOSTATE_EVO_SOUND,
    EVOSTATE_RESTORE_SCREEN,
    EVOSTATE_EVO_MON_ANIM,
    EVOSTATE_SET_MON_EVOLVED,
    EVOSTATE_TRY_LEARN_MOVE,
    EVOSTATE_END,
    EVOSTATE_CANCEL,
    EVOSTATE_CANCEL_MON_ANIM,
    EVOSTATE_CANCEL_MSG,
    EVOSTATE_LEARNED_MOVE,
    EVOSTATE_TRY_LEARN_ANOTHER_MOVE,
    EVOSTATE_REPLACE_MOVE,
};

// States for the switch in EVOSTATE_REPLACE_MOVE
enum {
    MVSTATE_INTRO_MSG_1,
    MVSTATE_INTRO_MSG_2,
    MVSTATE_INTRO_MSG_3,
    MVSTATE_PRINT_YES_NO,
    MVSTATE_HANDLE_YES_NO,
    MVSTATE_SHOW_MOVE_SELECT,
    MVSTATE_HANDLE_MOVE_SELECT,
    MVSTATE_FORGET_MSG_1,
    MVSTATE_FORGET_MSG_2,
    MVSTATE_LEARNED_MOVE,
    MVSTATE_ASK_CANCEL,
    MVSTATE_CANCEL,
    MVSTATE_RETRY_AFTER_HM,
};

// Task data from CycleEvolutionMonSprite
#define tEvoStopped data[8]

static void Task_EvolutionScene(u8 taskId)
{
    u32 var;
    struct Pokemon* mon = &gPlayerParty[gTasks[taskId].tPartyId];

    // Automatically cancel if the Pokemon would evolve into a species you have not
    // yet unlocked, such as Crobat.
    if (!IsNationalPokedexEnabled()
        && gTasks[taskId].tState == EVOSTATE_WAIT_CYCLE_MON_SPRITE
        && gTasks[taskId].tPostEvoSpecies > SPECIES_MEW)
    {
        gTasks[taskId].tState = EVOSTATE_CANCEL;
        gTasks[taskId].tEvoWasStopped = TRUE;
        gTasks[sEvoGraphicsTaskId].tEvoStopped = TRUE;
        StopBgAnimation();
        return;
    }

    // check if B Button was held, so the evolution gets stopped
    if (gMain.heldKeys == B_BUTTON
        && gTasks[taskId].tState == EVOSTATE_WAIT_CYCLE_MON_SPRITE
        && gTasks[sEvoGraphicsTaskId].isActive
        && gTasks[taskId].tBits & TASK_BIT_CAN_STOP)
    {
        gTasks[taskId].tState = EVOSTATE_CANCEL;
        gTasks[sEvoGraphicsTaskId].tEvoStopped = TRUE;
        StopBgAnimation();
        return;
    }

    switch (gTasks[taskId].tState)
    {
    case EVOSTATE_FADE_IN:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0x10, 0, RGB_BLACK);
        gSprites[sEvoStructPtr->preEvoSpriteId].invisible = FALSE;
        gTasks[taskId].tState++;
        ShowBg(0);
        ShowBg(1);
        ShowBg(2);
        ShowBg(3);
        break;
    case EVOSTATE_INTRO_MSG:
        if (!gPaletteFade.active)
        {
            StringExpandPlaceholders(gStringVar4, gText_PkmnIsEvolving);
            BattlePutTextOnWindow(gStringVar4, B_WIN_MSG);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_INTRO_MON_ANIM:
        if (!IsTextPrinterActive(0))
        {
            PlayCry_Normal(gTasks[taskId].tPreEvoSpecies, 0);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_INTRO_SOUND:
        if (IsCryFinished()) // wait for animation, play tu du SE
        {
            PlaySE(MUS_EVOLUTION_INTRO);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_START_MUSIC:
        if (!IsSEPlaying())
        {
            // Start music, fade background to black
            PlayNewMapMusic(MUS_EVOLUTION);
            gTasks[taskId].tState++;
            BeginNormalPaletteFade(0x1C, 4, 0, 0x10, RGB_BLACK);
        }
        break;
    case EVOSTATE_START_BG_AND_SPARKLE_SPIRAL:
        if (!gPaletteFade.active)
        {
            StartBgAnimation(FALSE);
            sEvoGraphicsTaskId = EvolutionSparkles_SpiralUpward(17);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_SPARKLE_ARC:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            gTasks[taskId].tState++;
            sEvoStructPtr->delayTimer = 1;
            sEvoGraphicsTaskId = EvolutionSparkles_ArcDown();
        }
        break;
    case EVOSTATE_CYCLE_MON_SPRITE:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            sEvoGraphicsTaskId = CycleEvolutionMonSprite(sEvoStructPtr->preEvoSpriteId, sEvoStructPtr->postEvoSpriteId);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_WAIT_CYCLE_MON_SPRITE:
        if (--sEvoStructPtr->delayTimer == 0)
        {
            sEvoStructPtr->delayTimer = 3;
            if (!gTasks[sEvoGraphicsTaskId].isActive)
                gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_SPARKLE_CIRCLE:
        sEvoGraphicsTaskId = EvolutionSparkles_CircleInward();
        gTasks[taskId].tState++;
        break;
    case EVOSTATE_SPARKLE_SPRAY:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            sEvoGraphicsTaskId = EvolutionSparkles_SprayAndFlash(gTasks[taskId].tPostEvoSpecies);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_EVO_SOUND:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            PlaySE(SE_EXP);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_RESTORE_SCREEN:
        if (IsSEPlaying())
        {
            m4aMPlayAllStop();
            memcpy(&gPlttBufferUnfaded[BG_PLTT_ID(2)], sEvoStructPtr->savedPalette, sizeof(sEvoStructPtr->savedPalette));
            RestoreBgAfterAnim();
            BeginNormalPaletteFade(0x1C, 0, 0x10, 0, RGB_BLACK);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_EVO_MON_ANIM:
        if (!gPaletteFade.active)
        {
            PlayCry_Normal(gTasks[taskId].tPostEvoSpecies, 0);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_SET_MON_EVOLVED:
        if (IsCryFinished())
        {
            StringExpandPlaceholders(gStringVar4, gText_CongratsPkmnEvolved);
            BattlePutTextOnWindow(gStringVar4, B_WIN_MSG);
            PlayBGM(MUS_EVOLVED);
            gTasks[taskId].tState++;
            SetMonData(mon, MON_DATA_SPECIES, (void *)(&gTasks[taskId].tPostEvoSpecies));
            CalculateMonStats(mon);
            EvolutionRenameMon(mon, gTasks[taskId].tPreEvoSpecies, gTasks[taskId].tPostEvoSpecies);
            GetSetPokedexFlag(SpeciesToNationalPokedexNum(gTasks[taskId].tPostEvoSpecies), FLAG_SET_SEEN);
            GetSetPokedexFlag(SpeciesToNationalPokedexNum(gTasks[taskId].tPostEvoSpecies), FLAG_SET_CAUGHT);
            IncrementGameStat(GAME_STAT_EVOLVED_POKEMON);
        }
        break;
    case EVOSTATE_TRY_LEARN_MOVE:
        if (!IsTextPrinterActive(0))
        {
            HelpSystem_Enable();
            var = MonTryLearningNewMove(mon, gTasks[taskId].tLearnsFirstMove);
            if (var != MOVE_NONE && !gTasks[taskId].tEvoWasStopped)
            {
                u8 text[20];

                StopMapMusic();
                Overworld_PlaySpecialMapMusic();
                gTasks[taskId].tBits |= TASK_BIT_LEARN_MOVE;
                gTasks[taskId].tLearnsFirstMove = FALSE;
                gTasks[taskId].tLearnMoveState = MVSTATE_INTRO_MSG_1;
                GetMonData(mon, MON_DATA_NICKNAME, text);
                StringCopy_Nickname(gBattleTextBuff1, text);

                if (var == MON_HAS_MAX_MOVES)
                    gTasks[taskId].tState = EVOSTATE_REPLACE_MOVE;
                else if (var == MON_ALREADY_KNOWS_MOVE)
                    break;
                else
                    gTasks[taskId].tState = EVOSTATE_LEARNED_MOVE;
            }
            else // no move to learn, or evolution was canceled
            {
                BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
                gTasks[taskId].tState++;
            }
        }
        break;
    case EVOSTATE_END:
        if (!gPaletteFade.active)
        {
            if (!(gTasks[taskId].tBits & TASK_BIT_LEARN_MOVE))
            {
                StopMapMusic();
                Overworld_PlaySpecialMapMusic();
            }
            if (!gTasks[taskId].tEvoWasStopped)
                CreateShedinja(gTasks[taskId].tPreEvoSpecies, mon);

            DestroyTask(taskId);
            FreeMonSpritesGfx();
            FREE_AND_SET_NULL(sEvoStructPtr);
            FreeAllWindowBuffers();
            SetMainCallback2(gCB2_AfterEvolution);
        }
        break;
    case EVOSTATE_CANCEL:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            m4aMPlayAllStop();
            BeginNormalPaletteFade(0x6001C, 0, 0x10, 0, RGB_WHITE);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_CANCEL_MON_ANIM:
        if (!gPaletteFade.active)
        {
            PlayCry_Normal(gTasks[taskId].tPreEvoSpecies, 0);
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_CANCEL_MSG:
        if (IsCryFinished())
        {
            if (gTasks[taskId].tEvoWasStopped)
                StringExpandPlaceholders(gStringVar4, gText_EllipsisQuestionMark);
            else
                StringExpandPlaceholders(gStringVar4, gText_PkmnStoppedEvolving);

            BattlePutTextOnWindow(gStringVar4, B_WIN_MSG);
            gTasks[taskId].tEvoWasStopped = TRUE;
            gTasks[taskId].tState = EVOSTATE_TRY_LEARN_MOVE;
        }
        break;
    case EVOSTATE_LEARNED_MOVE:
        if (!IsTextPrinterActive(0) && !IsSEPlaying())
        {
            BufferMoveToLearnIntoBattleTextBuff2();
            PlayFanfare(MUS_LEVEL_UP);
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_PKMNLEARNEDMOVE - BATTLESTRINGS_TABLE_START]);
            BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
            gTasks[taskId].tLearnsFirstMove = 0x40; // re-used as a counter
            gTasks[taskId].tState++;
        }
        break;
    case EVOSTATE_TRY_LEARN_ANOTHER_MOVE:
        if (!IsTextPrinterActive(0) && !IsSEPlaying() && --gTasks[taskId].tLearnsFirstMove == 0)
            gTasks[taskId].tState = EVOSTATE_TRY_LEARN_MOVE;
        break;
    case EVOSTATE_REPLACE_MOVE:
        switch (gTasks[taskId].tLearnMoveState)
        {
        case MVSTATE_INTRO_MSG_1:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                // "{mon} is trying to learn {move}"
                BufferMoveToLearnIntoBattleTextBuff2();
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_TRYTOLEARNMOVE1 - BATTLESTRINGS_TABLE_START]);
                BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case MVSTATE_INTRO_MSG_2:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                // "But, {mon} can't learn more than four moves"
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_TRYTOLEARNMOVE2 - BATTLESTRINGS_TABLE_START]);
                BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case MVSTATE_INTRO_MSG_3:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                // "Delete a move to make room for {move}?"
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_TRYTOLEARNMOVE3 - BATTLESTRINGS_TABLE_START]);
                BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
                gTasks[taskId].tLearnMoveYesState = MVSTATE_SHOW_MOVE_SELECT;
                gTasks[taskId].tLearnMoveNoState = MVSTATE_ASK_CANCEL;
                gTasks[taskId].tLearnMoveState++;
            }
        case MVSTATE_PRINT_YES_NO:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                HandleBattleWindow(23, 8, 29, 13, 0);
                BattlePutTextOnWindow(gText_BattleYesNoChoice, B_WIN_YESNO);
                gTasks[taskId].tLearnMoveState++;
                sEvoCursorPos = 0;
                BattleCreateYesNoCursorAt();
            }
            break;
        case MVSTATE_HANDLE_YES_NO:
            // This Yes/No is used for both the initial "delete move?" prompt
            // and for the "stop learning move?" prompt
            // What Yes/No do next is determined by tLearnMoveYesState / tLearnMoveNoState
            if (JOY_NEW(DPAD_UP) && sEvoCursorPos != 0)
            {
                // Moved onto YES
                PlaySE(SE_SELECT);
                BattleDestroyYesNoCursorAt();
                sEvoCursorPos = 0;
                BattleCreateYesNoCursorAt();
            }
            if (JOY_NEW(DPAD_DOWN) && sEvoCursorPos == 0)
            {
                // Moved onto NO
                PlaySE(SE_SELECT);
                BattleDestroyYesNoCursorAt();
                sEvoCursorPos = 1;
                BattleCreateYesNoCursorAt();
            }
            if (JOY_NEW(A_BUTTON))
            {
                HandleBattleWindow(0x17, 8, 0x1D, 0xD, WINDOW_CLEAR);
                PlaySE(SE_SELECT);

                if (sEvoCursorPos != 0)
                {
                    // NO
                    gTasks[taskId].tLearnMoveState = gTasks[taskId].tLearnMoveNoState;
                }
                else
                {
                    // YES
                    gTasks[taskId].tLearnMoveState = gTasks[taskId].tLearnMoveYesState;
                    if (gTasks[taskId].tLearnMoveState == MVSTATE_SHOW_MOVE_SELECT)
                        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
                }
            }
            if (JOY_NEW(B_BUTTON))
            {
                // Equivalent to selecting NO
                HandleBattleWindow(0x17, 8, 0x1D, 0xD, WINDOW_CLEAR);
                PlaySE(SE_SELECT);
                gTasks[taskId].tLearnMoveState = gTasks[taskId].tLearnMoveNoState;
            }
            break;
        case MVSTATE_SHOW_MOVE_SELECT:
            if (!gPaletteFade.active)
            {
                FreeAllWindowBuffers();
                ShowSelectMovePokemonSummaryScreen(gPlayerParty, gTasks[taskId].tPartyId,
                            gPlayerPartyCount - 1, CB2_EvolutionSceneLoadGraphics,
                            gMoveToLearn);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case MVSTATE_HANDLE_MOVE_SELECT:
            if (!gPaletteFade.active && gMain.callback2 == CB2_EvolutionSceneUpdate)
            {
                var = GetMoveSlotToReplace();
                if (var == MAX_MON_MOVES)
                {
                    // Didn't select move slot
                    gTasks[taskId].tLearnMoveState = MVSTATE_ASK_CANCEL;
                }
                else
                {
                    // Selected move to forget
                    u16 move = GetMonData(mon, var + MON_DATA_MOVE1);
                    if (IsHMMove2(move))
                    {
                        // Can't forget HMs
                        BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_HMMOVESCANTBEFORGOTTEN - BATTLESTRINGS_TABLE_START]);
                        BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
                        gTasks[taskId].tLearnMoveState = MVSTATE_RETRY_AFTER_HM;
                    }
                    else
                    {
                        // Forget move
                        PREPARE_MOVE_BUFFER(gBattleTextBuff2, move)

                        RemoveMonPPBonus(mon, var);
                        SetMonMoveSlot(mon, gMoveToLearn, var);
                        gTasks[taskId].tLearnMoveState++;
                    }
                }
            }
            break;
        case MVSTATE_FORGET_MSG_1:
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_123POOF - BATTLESTRINGS_TABLE_START]);
            BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
            gTasks[taskId].tLearnMoveState++;
            break;
        case MVSTATE_FORGET_MSG_2:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_PKMNFORGOTMOVE - BATTLESTRINGS_TABLE_START]);
                BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case MVSTATE_LEARNED_MOVE:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_ANDELLIPSIS - BATTLESTRINGS_TABLE_START]);
                BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
                gTasks[taskId].tState = EVOSTATE_LEARNED_MOVE;
            }
            break;
        case MVSTATE_ASK_CANCEL:
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_STOPLEARNINGMOVE - BATTLESTRINGS_TABLE_START]);
            BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
            gTasks[taskId].tLearnMoveYesState = MVSTATE_CANCEL;
            gTasks[taskId].tLearnMoveNoState = MVSTATE_INTRO_MSG_1;
            gTasks[taskId].tLearnMoveState = MVSTATE_PRINT_YES_NO;
            break;
        case MVSTATE_CANCEL:
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_DIDNOTLEARNMOVE - BATTLESTRINGS_TABLE_START]);
            BattlePutTextOnWindow(gDisplayedStringBattle, B_WIN_MSG);
            gTasks[taskId].tState = EVOSTATE_TRY_LEARN_MOVE;
            break;
        case MVSTATE_RETRY_AFTER_HM:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
                gTasks[taskId].tLearnMoveState = MVSTATE_SHOW_MOVE_SELECT;
            break;
        }
        break;
    }
}

// States for the main switch in Task_TradeEvolutionScene
enum {
    T_EVOSTATE_INTRO_MSG,
    T_EVOSTATE_INTRO_CRY,
    T_EVOSTATE_INTRO_SOUND,
    T_EVOSTATE_START_MUSIC,
    T_EVOSTATE_START_BG_AND_SPARKLE_SPIRAL,
    T_EVOSTATE_SPARKLE_ARC,
    T_EVOSTATE_CYCLE_MON_SPRITE,
    T_EVOSTATE_WAIT_CYCLE_MON_SPRITE,
    T_EVOSTATE_SPARKLE_CIRCLE,
    T_EVOSTATE_SPARKLE_SPRAY,
    T_EVOSTATE_EVO_SOUND,
    T_EVOSTATE_EVO_MON_ANIM,
    T_EVOSTATE_SET_MON_EVOLVED,
    T_EVOSTATE_TRY_LEARN_MOVE,
    T_EVOSTATE_END,
    T_EVOSTATE_CANCEL,
    T_EVOSTATE_CANCEL_MON_ANIM,
    T_EVOSTATE_CANCEL_MSG,
    T_EVOSTATE_LEARNED_MOVE,
    T_EVOSTATE_TRY_LEARN_ANOTHER_MOVE,
    T_EVOSTATE_REPLACE_MOVE,
};

// States for the switch in T_EVOSTATE_REPLACE_MOVE
enum {
    T_MVSTATE_INTRO_MSG_1,
    T_MVSTATE_INTRO_MSG_2,
    T_MVSTATE_INTRO_MSG_3,
    T_MVSTATE_PRINT_YES_NO,
    T_MVSTATE_HANDLE_YES_NO,
    T_MVSTATE_SHOW_MOVE_SELECT,
    T_MVSTATE_HANDLE_MOVE_SELECT,
    T_MVSTATE_FORGET_MSG,
    T_MVSTATE_LEARNED_MOVE,
    T_MVSTATE_ASK_CANCEL,
    T_MVSTATE_CANCEL,
    T_MVSTATE_RETRY_AFTER_HM,
};

static void Task_TradeEvolutionScene(u8 taskId)
{
    u32 var = 0;
    struct Pokemon* mon = &gPlayerParty[gTasks[taskId].tPartyId];

    // Automatically cancel if the Pokemon would evolve into a species you have not
    // yet unlocked, such as Crobat.
    if (!IsNationalPokedexEnabled()
        && gTasks[taskId].tState == T_EVOSTATE_WAIT_CYCLE_MON_SPRITE
        && gTasks[taskId].tPostEvoSpecies > SPECIES_MEW)
    {
        gTasks[taskId].tState = EVOSTATE_TRY_LEARN_MOVE;
        gTasks[taskId].tEvoWasStopped = TRUE;
        if (gTasks[sEvoGraphicsTaskId].isActive)
        {
            gTasks[sEvoGraphicsTaskId].tEvoStopped = TRUE;
            StopBgAnimation();
        }
    }

    switch (gTasks[taskId].tState)
    {
    case T_EVOSTATE_INTRO_MSG:
        StringExpandPlaceholders(gStringVar4, gText_PkmnIsEvolving);
        DrawTextOnTradeWindow(0, gStringVar4, 1);
        gTasks[taskId].tState++;
        break;
    case T_EVOSTATE_INTRO_CRY:
        if (!IsTextPrinterActive(0))
        {
            PlayCry_Normal(gTasks[taskId].tPreEvoSpecies, 0);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_INTRO_SOUND:
        if (IsCryFinished())
        {
            m4aSongNumStop(MUS_EVOLUTION);
            PlaySE(MUS_EVOLUTION_INTRO);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_START_MUSIC:
        if (!IsSEPlaying())
        {
            PlayBGM(MUS_EVOLUTION);
            gTasks[taskId].tState++;
            BeginNormalPaletteFade(0x1C, 4, 0, 0x10, RGB_BLACK);
        }
        break;
    case T_EVOSTATE_START_BG_AND_SPARKLE_SPIRAL:
        if (!gPaletteFade.active)
        {
            StartBgAnimation(TRUE);
            var = gSprites[sEvoStructPtr->preEvoSpriteId].oam.paletteNum + 16;
            sEvoGraphicsTaskId = EvolutionSparkles_SpiralUpward(var);
            gTasks[taskId].tState++;
            SetGpuReg(REG_OFFSET_BG3CNT, BGCNT_PRIORITY(3) | BGCNT_CHARBASE(0) | BGCNT_SCREENBASE(6));
        }
        break;
    case T_EVOSTATE_SPARKLE_ARC:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            gTasks[taskId].tState++;
            sEvoStructPtr->delayTimer = 1;
            sEvoGraphicsTaskId = EvolutionSparkles_ArcDown();
        }
        break;
    case T_EVOSTATE_CYCLE_MON_SPRITE:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            sEvoGraphicsTaskId = CycleEvolutionMonSprite(sEvoStructPtr->preEvoSpriteId, sEvoStructPtr->postEvoSpriteId);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_WAIT_CYCLE_MON_SPRITE:
        if (--sEvoStructPtr->delayTimer == 0)
        {
            sEvoStructPtr->delayTimer = 3;
            if (!gTasks[sEvoGraphicsTaskId].isActive)
                gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_SPARKLE_CIRCLE:
        sEvoGraphicsTaskId = EvolutionSparkles_CircleInward();
        gTasks[taskId].tState++;
        break;
    case T_EVOSTATE_SPARKLE_SPRAY:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            sEvoGraphicsTaskId = EvolutionSparkles_SprayAndFlash_Trade(gTasks[taskId].tPostEvoSpecies);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_EVO_SOUND:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            PlaySE(SE_EXP);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_EVO_MON_ANIM:
        /*
         * BUG: This check causes the evolved Pokemon's cry to play over the sfx.
         * Negate the below condition.
         */
        if (IsSEPlaying())
        {
//            Free(sBgAnimPal);
            PlayCry_Normal(gTasks[taskId].tPostEvoSpecies, 0);
            memcpy(&gPlttBufferUnfaded[BG_PLTT_ID(2)], sEvoStructPtr->savedPalette, sizeof(sEvoStructPtr->savedPalette));
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_SET_MON_EVOLVED:
        if (IsCryFinished())
        {
            StringExpandPlaceholders(gStringVar4, gText_CongratsPkmnEvolved);
            DrawTextOnTradeWindow(0, gStringVar4, 1);
            PlayFanfare(MUS_EVOLVED);
            gTasks[taskId].tState++;
            SetMonData(mon, MON_DATA_SPECIES, (&gTasks[taskId].tPostEvoSpecies));
            CalculateMonStats(mon);
            EvolutionRenameMon(mon, gTasks[taskId].tPreEvoSpecies, gTasks[taskId].tPostEvoSpecies);
            GetSetPokedexFlag(SpeciesToNationalPokedexNum(gTasks[taskId].tPostEvoSpecies), FLAG_SET_SEEN);
            GetSetPokedexFlag(SpeciesToNationalPokedexNum(gTasks[taskId].tPostEvoSpecies), FLAG_SET_CAUGHT);
            IncrementGameStat(GAME_STAT_EVOLVED_POKEMON);
        }
        break;
    case T_EVOSTATE_TRY_LEARN_MOVE:
        if (!IsTextPrinterActive(0) && IsFanfareTaskInactive() == TRUE)
        {
            var = MonTryLearningNewMove(mon, gTasks[taskId].tLearnsFirstMove);
            if (var != MOVE_NONE && !gTasks[taskId].tEvoWasStopped)
            {
                u8 text[20];

                gTasks[taskId].tBits |= TASK_BIT_LEARN_MOVE;
                gTasks[taskId].tLearnsFirstMove = FALSE;
                gTasks[taskId].tLearnMoveState = 0;
                GetMonData(mon, MON_DATA_NICKNAME, text);
                StringCopy_Nickname(gBattleTextBuff1, text);

                if (var == MON_HAS_MAX_MOVES)
                    gTasks[taskId].tState = T_EVOSTATE_REPLACE_MOVE;
                else if (var == MON_ALREADY_KNOWS_MOVE)
                    break;
                else
                    gTasks[taskId].tState = T_EVOSTATE_LEARNED_MOVE;
            }
            else
            {
                PlayBGM(MUS_EVOLUTION);
                DrawTextOnTradeWindow(0, gText_CommunicationStandby5, 1);
                gTasks[taskId].tState++;
            }
        }
        break;
    case T_EVOSTATE_END:
        if (!IsTextPrinterActive(0))
        {
            DestroyTask(taskId);
            FREE_AND_SET_NULL(sEvoStructPtr);
            sEvoStructPtr = NULL;
            gTextFlags.useAlternateDownArrow = FALSE;
            SetMainCallback2(gCB2_AfterEvolution);
        }
        break;
    case T_EVOSTATE_CANCEL:
        if (!gTasks[sEvoGraphicsTaskId].isActive)
        {
            m4aMPlayAllStop();
            BeginNormalPaletteFade((1 << (gSprites[sEvoStructPtr->preEvoSpriteId].oam.paletteNum + 16)) | (0x4001C), 0, 0x10, 0, RGB_WHITE);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_CANCEL_MON_ANIM:
        if (!gPaletteFade.active)
        {
            PlayCry_Normal(gTasks[taskId].tPreEvoSpecies, 0);
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_CANCEL_MSG:
        if (IsCryFinished())
        {
            StringExpandPlaceholders(gStringVar4, gText_EllipsisQuestionMark);
            DrawTextOnTradeWindow(0, gStringVar4, 1);
            gTasks[taskId].tEvoWasStopped = TRUE;
            gTasks[taskId].tState = T_EVOSTATE_TRY_LEARN_MOVE;
        }
        break;
    case T_EVOSTATE_LEARNED_MOVE:
        if (!IsTextPrinterActive(0) && !IsSEPlaying())
        {
            BufferMoveToLearnIntoBattleTextBuff2();
            PlayFanfare(MUS_LEVEL_UP);
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_PKMNLEARNEDMOVE - BATTLESTRINGS_TABLE_START]);
            DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
            gTasks[taskId].tLearnsFirstMove = 0x40; // re-used as a counter
            gTasks[taskId].tState++;
        }
        break;
    case T_EVOSTATE_TRY_LEARN_ANOTHER_MOVE:
        if (!IsTextPrinterActive(0) && IsFanfareTaskInactive() == TRUE && --gTasks[taskId].tLearnsFirstMove == 0)
            gTasks[taskId].tState = T_EVOSTATE_TRY_LEARN_MOVE;
        break;
    case T_EVOSTATE_REPLACE_MOVE:
        switch (gTasks[taskId].tLearnMoveState)
        {
        case T_MVSTATE_INTRO_MSG_1:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                // "{mon} is trying to learn {move}"
                BufferMoveToLearnIntoBattleTextBuff2();
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_TRYTOLEARNMOVE1 - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case T_MVSTATE_INTRO_MSG_2:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                // "But, {mon} can't learn more than four moves"
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_TRYTOLEARNMOVE2 - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case T_MVSTATE_INTRO_MSG_3:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                // "Delete a move to make room for {move}?"
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_TRYTOLEARNMOVE3 - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tLearnMoveYesState = T_MVSTATE_SHOW_MOVE_SELECT;
                gTasks[taskId].tLearnMoveNoState = T_MVSTATE_ASK_CANCEL;
                gTasks[taskId].tLearnMoveState++;
            }
        case T_MVSTATE_PRINT_YES_NO:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                LoadUserWindowGfx2(0, 0xA8, BG_PLTT_ID(14));
                CreateYesNoMenu(&gTradeEvolutionSceneYesNoWindowTemplate, FONT_NORMAL_COPY_2, 0, 2, 0xA8, 14, 0);
                sEvoCursorPos = 0;
                gTasks[taskId].tLearnMoveState++;
                sEvoCursorPos = 0;
            }
            break;
        case T_MVSTATE_HANDLE_YES_NO:
            switch (Menu_ProcessInputNoWrapClearOnChoose())
            {
            case 0: // YES
                sEvoCursorPos = 0;
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_EMPTYSTRING3 - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tLearnMoveState = gTasks[taskId].tLearnMoveYesState;
                if (gTasks[taskId].tLearnMoveState == T_MVSTATE_SHOW_MOVE_SELECT)
                    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
                break;
            case 1: // NO
            case MENU_B_PRESSED:
                sEvoCursorPos = 1;
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_EMPTYSTRING3 - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tLearnMoveState = gTasks[taskId].tLearnMoveNoState;
                break;
            }
            break;
        case T_MVSTATE_SHOW_MOVE_SELECT:
            if (!gPaletteFade.active)
            {
                if (gWirelessCommType)
                    DestroyWirelessStatusIndicatorSprite();

                Free(GetBgTilemapBuffer(3));
                Free(GetBgTilemapBuffer(1));
                Free(GetBgTilemapBuffer(0));
                FreeAllWindowBuffers();

                ShowSelectMovePokemonSummaryScreen(gPlayerParty, gTasks[taskId].tPartyId,
                            gPlayerPartyCount - 1, CB2_TradeEvolutionSceneLoadGraphics,
                            gMoveToLearn);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case T_MVSTATE_HANDLE_MOVE_SELECT:
            if (!gPaletteFade.active && gMain.callback2 == CB2_TradeEvolutionSceneUpdate)
            {
                var = GetMoveSlotToReplace();
                if (var == MAX_MON_MOVES)
                {
                    // Didn't select move slot
                    gTasks[taskId].tLearnMoveState = T_MVSTATE_ASK_CANCEL;
                }
                else
                {
                    // Selected move to forget
                    u16 move = GetMonData(mon, var + MON_DATA_MOVE1);
                    if (IsHMMove2(move))
                    {
                        // Can't forget HMs
                        BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_HMMOVESCANTBEFORGOTTEN - BATTLESTRINGS_TABLE_START]);
                        DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                        gTasks[taskId].tLearnMoveState = T_MVSTATE_RETRY_AFTER_HM;
                    }
                    else
                    {
                        // Forget move
                        PREPARE_MOVE_BUFFER(gBattleTextBuff2, move)

                        RemoveMonPPBonus(mon, var);
                        SetMonMoveSlot(mon, gMoveToLearn, var);
                        BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_123POOF - BATTLESTRINGS_TABLE_START]);
                        DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                        gTasks[taskId].tLearnMoveState++;
                    }
                }
            }
            break;
        case T_MVSTATE_FORGET_MSG:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_PKMNFORGOTMOVE - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tLearnMoveState++;
            }
            break;
        case T_MVSTATE_LEARNED_MOVE:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
            {
                BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_ANDELLIPSIS - BATTLESTRINGS_TABLE_START]);
                DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
                gTasks[taskId].tState = T_EVOSTATE_LEARNED_MOVE;
            }
            break;
        case T_MVSTATE_ASK_CANCEL:
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_STOPLEARNINGMOVE - BATTLESTRINGS_TABLE_START]);
            DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
            gTasks[taskId].tLearnMoveYesState = T_MVSTATE_CANCEL;
            gTasks[taskId].tLearnMoveNoState = T_MVSTATE_INTRO_MSG_1;
            gTasks[taskId].tLearnMoveState = T_MVSTATE_PRINT_YES_NO;
            break;
        case T_MVSTATE_CANCEL:
            BattleStringExpandPlaceholdersToDisplayedString(gBattleStringsTable[STRINGID_DIDNOTLEARNMOVE - BATTLESTRINGS_TABLE_START]);
            DrawTextOnTradeWindow(0, gDisplayedStringBattle, 1);
            gTasks[taskId].tState = T_EVOSTATE_TRY_LEARN_MOVE;
            break;
        case T_MVSTATE_RETRY_AFTER_HM:
            if (!IsTextPrinterActive(0) && !IsSEPlaying())
                gTasks[taskId].tLearnMoveState = T_MVSTATE_SHOW_MOVE_SELECT;
            break;
        }
        break;
    }
}

#undef tState
#undef tPreEvoSpecies
#undef tPostEvoSpecies
#undef tCanStop
#undef tBits
#undef tLearnsFirstMove
#undef tLearnMoveState
#undef tLearnMoveYesState
#undef tLearnMoveNoState
#undef tEvoWasStopped
#undef tPartyId

static void EvoDummyFunc(void)
{
}

static void VBlankCB_EvolutionScene(void)
{
    SetGpuReg(REG_OFFSET_BG0HOFS, gBattle_BG0_X);
    SetGpuReg(REG_OFFSET_BG0VOFS, gBattle_BG0_Y);
    SetGpuReg(REG_OFFSET_BG1HOFS, gBattle_BG1_X);
    SetGpuReg(REG_OFFSET_BG1VOFS, gBattle_BG1_Y);
    SetGpuReg(REG_OFFSET_BG2HOFS, gBattle_BG2_X);
    SetGpuReg(REG_OFFSET_BG2VOFS, gBattle_BG2_Y);
    SetGpuReg(REG_OFFSET_BG3HOFS, gBattle_BG3_X);
    SetGpuReg(REG_OFFSET_BG3VOFS, gBattle_BG3_Y);

    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
    ScanlineEffect_InitHBlankDmaTransfer();
}

static void VBlankCB_TradeEvolutionScene(void)
{
    SetGpuReg(REG_OFFSET_BG0HOFS, gBattle_BG0_X);
    SetGpuReg(REG_OFFSET_BG0VOFS, gBattle_BG0_Y);
    SetGpuReg(REG_OFFSET_BG1HOFS, gBattle_BG1_X);
    SetGpuReg(REG_OFFSET_BG1VOFS, gBattle_BG1_Y);
    SetGpuReg(REG_OFFSET_BG2HOFS, gBattle_BG2_X);
    SetGpuReg(REG_OFFSET_BG2VOFS, gBattle_BG2_Y);
    SetGpuReg(REG_OFFSET_BG3HOFS, gBattle_BG3_X);
    SetGpuReg(REG_OFFSET_BG3VOFS, gBattle_BG3_Y);

    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
    ScanlineEffect_InitHBlankDmaTransfer();
}

#define tCycleTimer   data[0]
#define tPalStage     data[1]
#define tControlStage data[2]
#define tNumCycles    data[3]
#define tStartTimer   data[5]
#define tPaused       data[6]

// See comments above sBgAnim_PaletteControl
#define START_PAL sBgAnim_PaletteControl[tControlStage][0]
#define END_PAL   sBgAnim_PaletteControl[tControlStage][1]
#define CYCLES    sBgAnim_PaletteControl[tControlStage][2]
#define DELAY     sBgAnim_PaletteControl[tControlStage][3]

// Cycles the background through a set range of palettes in a series
// of stages, each stage having a different palette range and timing
static void Task_UpdateBgPalette(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    if (tPaused)
        return;
    if (tStartTimer++ < 20)
        return;

    if (tCycleTimer++ > DELAY)
    {
        if (END_PAL == tPalStage)
        {
            // Reached final palette in current stage, completed a 'cycle'
            // If this is the final cycle for this stage, move to the next stage
            tNumCycles++;
            if (tNumCycles == CYCLES)
            {
                tNumCycles = 0;
                tControlStage++;
            }
            tPalStage = START_PAL;
        }
        else
        {
            // Haven't reached final palette in current stage, load the current palette
            LoadPalette(&sBgAnimPal[tPalStage * 16], BG_PLTT_ID(10), PLTT_SIZE_4BPP);
            tCycleTimer = 0;
            tPalStage++;
        }
    }

    if (tControlStage == (int)ARRAY_COUNT(sBgAnim_PaletteControl[0]))
        DestroyTask(taskId);
}

#undef tCycleTimer
#undef tPalStage
#undef tControlStage
#undef tNumCycles
#undef tStartTimer
#undef START_PAL
#undef END_PAL
#undef CYCLES
#undef DELAY

#define tIsLink data[2]

static void CreateBgAnimTask(bool8 isLink)
{
    u8 taskId = CreateTask(Task_AnimateBg, 7);

    if (!isLink)
        gTasks[taskId].data[2] = FALSE;
    else
        gTasks[taskId].data[2] = TRUE;
}

static void Task_AnimateBg(u8 taskId)
{
    u16 *outer_X, *outer_Y;

    u16 *inner_X = &gBattle_BG1_X;
    u16 *inner_Y = &gBattle_BG1_Y;

    if (!gTasks[taskId].data[2])
    {
        outer_X = &gBattle_BG2_X;
        outer_Y = &gBattle_BG2_Y;
    }
    else
    {
        outer_X = &gBattle_BG3_X;
        outer_Y = &gBattle_BG3_Y;
    }

    gTasks[taskId].data[0] = (gTasks[taskId].data[0] + 5) & 0xFF;
    gTasks[taskId].data[1] = (gTasks[taskId].data[0] + 0x80) & 0xFF;

    *inner_X = Cos(gTasks[taskId].data[0], 4) + 8;
    *inner_Y = Sin(gTasks[taskId].data[0], 4) + 16;

    *outer_X = Cos(gTasks[taskId].data[1], 4) + 8;
    *outer_Y = Sin(gTasks[taskId].data[1], 4) + 16;

    if (!FuncIsActiveTask(Task_UpdateBgPalette))
    {
        DestroyTask(taskId);

        *inner_X = 0;
        *inner_Y = 0;

        *outer_X = 256;
        *outer_Y = 0;
    }
}

#undef tIsLink

static void InitMovingBgPalette(u16 *palette)
{
    s32 i, j;

    for (i = 0; i < (int)ARRAY_COUNT(sBgAnim_PalIndexes); i++)
    {
        for (j = 0; j < 16; j++)
        {
            palette[i * 16 + j] = sBgAnim_Pal[sBgAnim_PalIndexes[i][j]];
        }
    }
}

static void StartBgAnimation(bool8 isLink)
{
    u8 innerBgId, outerBgId;

    sBgAnimPal = AllocZeroed(0x640);
    InitMovingBgPalette(sBgAnimPal);

    if (!isLink)
        innerBgId = 1, outerBgId = 2;
    else
        innerBgId = 1, outerBgId = 3;

    LoadPalette(sBlackPalette, BG_PLTT_ID(10), sizeof(sBlackPalette));

    DecompressAndLoadBgGfxUsingHeap(1, sMovingBackgroundTiles, FALSE, 0, 0);
    CopyToBgTilemapBuffer(1, sMovingBackgroundMap1, 0, 0);
    CopyToBgTilemapBuffer(outerBgId, sMovingBackgroundMap2, 0, 0);
    CopyBgTilemapBufferToVram(1);
    CopyBgTilemapBufferToVram(outerBgId);

    if (!isLink)
    {
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG2);
        SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(8, 8));
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_BG2_ON | DISPCNT_BG1_ON | DISPCNT_BG0_ON | DISPCNT_OBJ_1D_MAP);

        SetBgAttribute(innerBgId, BG_ATTR_PRIORITY, 2);
        SetBgAttribute(outerBgId, BG_ATTR_PRIORITY, 2);

        ShowBg(1);
        ShowBg(2);
    }
    else
    {
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG3);
        SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(8, 8));
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_BG3_ON | DISPCNT_BG1_ON | DISPCNT_BG0_ON | DISPCNT_OBJ_1D_MAP);
    }

    CreateTask(Task_UpdateBgPalette, 5);
    CreateBgAnimTask(isLink);
}

void IsMovingBackgroundTaskRunning(void) // unused
{
    u8 taskId = FindTaskIdByFunc(Task_UpdateBgPalette);

    if (taskId != TASK_NONE)
        gTasks[taskId].tPaused = TRUE;

    FillPalette(RGB_BLACK, BG_PLTT_ID(10), PLTT_SIZE_4BPP);
}

#undef tPaused

static void StopBgAnimation(void)
{
    u8 taskId;

    if ((taskId = FindTaskIdByFunc(Task_UpdateBgPalette)) != TASK_NONE)
        DestroyTask(taskId);
    if ((taskId = FindTaskIdByFunc(Task_AnimateBg)) != TASK_NONE)
        DestroyTask(taskId);

    FillPalette(RGB_BLACK, BG_PLTT_ID(10), PLTT_SIZE_4BPP);
    RestoreBgAfterAnim();
}

static void RestoreBgAfterAnim(void)
{
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    gBattle_BG1_X = 0;
    gBattle_BG1_Y = 0;
    gBattle_BG2_X = 0;
    SetBgAttribute(1, BG_ATTR_PRIORITY, GetBattleBgTemplateData(1, 5));
    SetBgAttribute(2, BG_ATTR_PRIORITY, GetBattleBgTemplateData(2, 5));
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_BG3_ON | DISPCNT_BG0_ON | DISPCNT_OBJ_1D_MAP);
    Free(sBgAnimPal);
}
