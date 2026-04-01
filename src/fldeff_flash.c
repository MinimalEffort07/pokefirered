/**
 * @file fldeff_flash.c
 * @brief Flash Field Move and Cave Transition Animations
 *
 * FILE OVERVIEW:
 * This file handles TWO related systems:
 *
 * 1. HM FLASH FIELD EFFECT: When the player uses Flash in a dark cave, the
 *    screen brightens by expanding the visibility circle (see field_screen_effect.c).
 *    Flash can only be used once per cave visit (tracked by FLAG_SYS_FLASH_ACTIVE).
 *
 * 2. CAVE TRANSITION ANIMATIONS: The visual transitions that play when entering
 *    or exiting caves (MAP_TYPE_UNDERGROUND). These are the dramatic "entering
 *    cave" and "exiting cave" screen effects:
 *    - Enter: Screen starts bright, fades through a cave-mouth silhouette to black
 *    - Exit: Screen starts dark, fades through the silhouette to bright
 *    Both use a special tilemap (cave_transition) that shows a rocky cave opening
 *    shape, with alpha blending to create a smooth fade effect.
 *
 * 3. MAP PREVIEW SCREEN: The "[CAVE NAME]" text that appears when entering a new
 *    cave area. Shows the name of the location before the cave transition plays.
 *
 * MAP TRANSITION TABLE:
 * The sTransitionTypes[] table maps every possible (fromType, toType) pair to
 * determine whether a cave transition animation should play. Any transition
 * TO MAP_TYPE_UNDERGROUND is an "enter" animation; any FROM it is an "exit."
 *
 * GBA CONTEXT — ALPHA BLENDING FOR TRANSITIONS:
 * The cave transitions use the GBA's alpha blending hardware to smoothly fade
 * a cave-shaped silhouette in or out. BLDCNT configures BG0 (the cave shape)
 * as the first target, and all other layers as second targets. BLDALPHA controls
 * the blend ratio, which is gradually adjusted each frame.
 */
#include "global.h"
#include "gflib.h"
#include "event_data.h"
#include "event_scripts.h"
#include "fldeff.h"
#include "field_effect.h"
#include "map_preview_screen.h"
#include "overworld.h"
#include "party_menu.h"
#include "script.h"
#include "constants/songs.h"
#include "constants/map_types.h"

struct FlashStruct
{
    u8 fromType;
    u8 toType;
    bool8 isEnter;
    bool8 isExit;
    void (*func1)(void);
    void (*func2)(u8 mapSecId);
};

static void FieldCallback_Flash(void);
static void FldEff_UseFlash(void);
static bool8 TryDoMapTransition(void);
static void FlashTransition_Exit(void);
static void Task_FlashTransition_Exit_0(u8 taskId);
static void Task_FlashTransition_Exit_1(u8 taskId);
static void Task_FlashTransition_Exit_2(u8 taskId);
static void Task_FlashTransition_Exit_3(u8 taskId);
static void Task_FlashTransition_Exit_4(u8 taskId);
static void FlashTransition_Enter(void);
static void Task_FlashTransition_Enter_0(u8 taskId);
static void Task_FlashTransition_Enter_1(u8 taskId);
static void Task_FlashTransition_Enter_2(u8 taskId);
static void Task_FlashTransition_Enter_3(u8 taskId);
static void RunMapPreviewScreen(u8 mapsecId);
static void Task_MapPreviewScreen_0(u8 taskId);

static const struct FlashStruct sTransitionTypes[] = {
    {
        .fromType = MAP_TYPE_TOWN,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_CITY,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_ROUTE,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERWATER,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_OCEAN_ROUTE,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNKNOWN,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_INDOOR,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_SECRET_BASE,
        .toType = MAP_TYPE_UNDERGROUND,
        .isEnter = TRUE,
        .isExit = FALSE,
        .func1 = FlashTransition_Enter,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_TOWN,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_CITY,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_ROUTE,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_UNDERWATER,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_OCEAN_ROUTE,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_UNKNOWN,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_INDOOR,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {
        .fromType = MAP_TYPE_UNDERGROUND,
        .toType = MAP_TYPE_SECRET_BASE,
        .isEnter = FALSE,
        .isExit = TRUE,
        .func1 = FlashTransition_Exit,
        .func2 = RunMapPreviewScreen
    }, {0}
};

static const u16 sCaveTransitionPalette_White[] = INCBIN_U16("graphics/cave_transition/white.gbapal");
static const u16 sCaveTransitionPalette_Black[] = INCBIN_U16("graphics/cave_transition/black.gbapal");

static const u16 sCaveTransitionPalette_Enter[] = INCBIN_U16("graphics/cave_transition/enter.gbapal");
static const u16 sCaveTransitionPalette_Exit[] = INCBIN_U16("graphics/cave_transition/exit.gbapal");
static const u32 sCaveTransitionTilemap[] = INCBIN_U32("graphics/cave_transition/tilemap.bin.lz");
static const u32 sCaveTransitionTiles[] = INCBIN_U32("graphics/cave_transition/tiles.4bpp.lz");

/**
 * FUNCTION: SetUpFieldMove_Flash
 *
 * PURPOSE: Validates whether Flash can be used. Requirements:
 * 1. The current map must be a cave (gMapHeader.cave == TRUE)
 * 2. Flash must not have already been used in this cave visit
 *    (FLAG_SYS_FLASH_ACTIVE is not set)
 *
 * @return TRUE if Flash can be used, FALSE otherwise
 */
bool8 SetUpFieldMove_Flash(void)
{
    if (gMapHeader.cave != TRUE)
        return FALSE;

    if (FlagGet(FLAG_SYS_FLASH_ACTIVE))
        return FALSE;

    gFieldCallback2 = FieldCallback_PrepareFadeInFromMenu;
    gPostMenuFieldCallback = FieldCallback_Flash;
    return TRUE;
}

static void FieldCallback_Flash(void)
{
    u8 taskId = CreateFieldEffectShowMon();
    gFieldEffectArguments[0] = GetCursorSelectionMonId();
    gTasks[taskId].data[8] = ((uintptr_t)FldEff_UseFlash) >> 16;
    gTasks[taskId].data[9] = ((uintptr_t)FldEff_UseFlash);
}

static void FldEff_UseFlash(void)
{
    PlaySE(SE_M_REFLECT);
    FlagSet(FLAG_SYS_FLASH_ACTIVE);
    ScriptContext_SetupScript(EventScript_FldEffFlash);
}

// Map transition animatics

static void CB2_ChangeMapMain(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    UpdatePaletteFade();
}

static void VBC_ChangeMapVBlank(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

/**
 * FUNCTION: CB2_DoChangeMap
 *
 * PURPOSE: Main callback that handles the map change transition screen.
 * Resets all GPU state, clears VRAM/OAM/palette, then checks if a cave
 * transition animation should play.
 *
 * GBA CONTEXT:
 * This function performs a full GPU reset — the same kind of cleanup done
 * when transitioning between major game screens. It:
 * - Disables the display (DISPCNT = 0) to prevent garbage on screen
 * - Zeros all BG control and scroll registers
 * - Clears all of VRAM (96KB), OAM (1KB), and palette RAM (512 bytes)
 *   using DMA channel 3 for fast hardware-accelerated fills
 * - Resets the task, sprite, and palette fade systems
 * - Re-enables VBlank interrupt and sets up the transition callbacks
 */
void CB2_DoChangeMap(void)
{
    SetVBlankCallback(NULL);

    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    SetGpuReg(REG_OFFSET_BG2CNT, 0);
    SetGpuReg(REG_OFFSET_BG1CNT, 0);
    SetGpuReg(REG_OFFSET_BG0CNT, 0);
    SetGpuReg(REG_OFFSET_BG2HOFS, 0);
    SetGpuReg(REG_OFFSET_BG2VOFS, 0);
    SetGpuReg(REG_OFFSET_BG1HOFS, 0);
    SetGpuReg(REG_OFFSET_BG1VOFS, 0);
    SetGpuReg(REG_OFFSET_BG0HOFS, 0);
    SetGpuReg(REG_OFFSET_BG0VOFS, 0);

    DmaFill16(3, 0, (void *)VRAM, VRAM_SIZE);
    DmaFill32(3, 0, (void *)OAM, OAM_SIZE);
    DmaFill16(3, 0, (void *)(PLTT + 2), PLTT_SIZE - 2);
    ResetPaletteFade();
    ResetTasks();
    ResetSpriteData();
    EnableInterrupts(INTR_FLAG_VBLANK);
    SetVBlankCallback(VBC_ChangeMapVBlank);
    SetMainCallback2(CB2_ChangeMapMain);
    if (!TryDoMapTransition())
        SetMainCallback2(gMain.savedCallback);
}

static bool8 TryDoMapTransition(void)
{
    u8 fromType = GetLastUsedWarpMapType();
    u8 toType = GetCurrentMapType();
    u8 i = 0;
    if (GetLastUsedWarpMapSectionId() != gMapHeader.regionMapSectionId && MapHasPreviewScreen_HandleQLState2(gMapHeader.regionMapSectionId, MPS_TYPE_CAVE) == TRUE)
    {
        RunMapPreviewScreen(gMapHeader.regionMapSectionId);
        return TRUE;
    }
    for (; sTransitionTypes[i].fromType != 0; i++)
    {
        if (sTransitionTypes[i].fromType == fromType && sTransitionTypes[i].toType == toType)
        {
            sTransitionTypes[i].func1();
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * FUNCTION: MapTransitionIsEnter
 *
 * PURPOSE: Checks if a map transition from one type to another is an "enter"
 * transition (going into a cave/underground area). Used by the fade system
 * to decide whether to fade to white (enter) or black (exit).
 *
 * @param _fromType — the source map's type
 * @param _toType — the destination map's type
 * @return TRUE if this is an enter transition, FALSE otherwise
 */
bool8 MapTransitionIsEnter(u8 _fromType, u8 _toType)
{
    u8 fromType = _fromType;
    u8 toType = _toType;
    u8 i = 0;
    for (; sTransitionTypes[i].fromType != 0; i++)
    {
        if (sTransitionTypes[i].fromType == fromType && sTransitionTypes[i].toType == toType)
        {
            return sTransitionTypes[i].isEnter;
        }
    }
    return FALSE;
}

/**
 * FUNCTION: MapTransitionIsExit
 *
 * PURPOSE: Checks if a map transition is an "exit" transition (leaving a cave
 * to go back to an outdoor/indoor area).
 *
 * @param _fromType — the source map's type
 * @param _toType — the destination map's type
 * @return TRUE if this is an exit transition, FALSE otherwise
 */
bool8 MapTransitionIsExit(u8 _fromType, u8 _toType)
{
    u8 fromType = _fromType;
    u8 toType = _toType;
    u8 i = 0;
    for (; sTransitionTypes[i].fromType != 0; i++)
    {
        if (sTransitionTypes[i].fromType == fromType && sTransitionTypes[i].toType == toType)
        {
            return sTransitionTypes[i].isExit;
        }
    }
    return FALSE;
}

static void FlashTransition_Exit(void)
{
    CreateTask(Task_FlashTransition_Exit_0, 0);
}

static void Task_FlashTransition_Exit_0(u8 taskId)
{
    gTasks[taskId].func = Task_FlashTransition_Exit_1;
}

static void Task_FlashTransition_Exit_1(u8 taskId)
{
    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    LZ77UnCompVram(sCaveTransitionTiles, (void *)BG_CHAR_ADDR(3));
    LZ77UnCompVram(sCaveTransitionTilemap, (void *)BG_SCREEN_ADDR(31));
    LoadPalette(sCaveTransitionPalette_White, BG_PLTT_ID(14), sizeof(sCaveTransitionPalette_White));
    LoadPalette(sCaveTransitionPalette_Exit, BG_PLTT_ID(14), sizeof(sCaveTransitionPalette_Exit));
    SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG0 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ | BLDCNT_TGT2_BD);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    SetGpuReg(REG_OFFSET_BG0CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(3) | BGCNT_SCREENBASE(31));
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_1D_MAP | DISPCNT_BG0_ON | DISPCNT_OBJ_ON);
    gTasks[taskId].func = Task_FlashTransition_Exit_2;
    gTasks[taskId].data[0] = 16;
    gTasks[taskId].data[1] = 0;
}

static void Task_FlashTransition_Exit_2(u8 taskId)
{
    u16 r4 = gTasks[taskId].data[1];
    SetGpuReg(REG_OFFSET_BLDALPHA, (16 << 8) + r4);
    if (r4 <= 16)
    {
        gTasks[taskId].data[1]++;
    }
    else
    {
        gTasks[taskId].data[2] = 0;
        gTasks[taskId].func = Task_FlashTransition_Exit_3;
    }
}

static void Task_FlashTransition_Exit_3(u8 taskId)
{
    u16 count;
    SetGpuReg(REG_OFFSET_BLDALPHA, (16 << 8) + 16);
    count = gTasks[taskId].data[2];
    if (count < 8)
    {
        gTasks[taskId].data[2]++;
        LoadPalette(&sCaveTransitionPalette_Exit[count], BG_PLTT_ID(14), sizeof(sCaveTransitionPalette_Exit) - PLTT_SIZEOF(count));
    }
    else
    {
        LoadPalette(sCaveTransitionPalette_White, BG_PLTT_ID(0), sizeof(sCaveTransitionPalette_White));
        gTasks[taskId].func = Task_FlashTransition_Exit_4;
        gTasks[taskId].data[2] = 8;
    }
}

static void Task_FlashTransition_Exit_4(u8 taskId)
{
    if (gTasks[taskId].data[2] != 0)
        gTasks[taskId].data[2]--;
    else
        SetMainCallback2(gMain.savedCallback);
}

static void FlashTransition_Enter(void)
{
    CreateTask(Task_FlashTransition_Enter_0, 0);
}

static void Task_FlashTransition_Enter_0(u8 taskId)
{
    gTasks[taskId].func = Task_FlashTransition_Enter_1;
}

static void Task_FlashTransition_Enter_1(u8 taskId)
{
    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    LZ77UnCompVram(sCaveTransitionTiles, (void *)BG_CHAR_ADDR(3));
    LZ77UnCompVram(sCaveTransitionTilemap, (void *)BG_SCREEN_ADDR(31));
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    SetGpuReg(REG_OFFSET_BG0CNT, BGCNT_PRIORITY(0) | BGCNT_CHARBASE(3) | BGCNT_SCREENBASE(31));
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_1D_MAP | DISPCNT_BG0_ON | DISPCNT_OBJ_ON);
    LoadPalette(sCaveTransitionPalette_White, BG_PLTT_ID(14), sizeof(sCaveTransitionPalette_White));
    LoadPalette(sCaveTransitionPalette_Black, BG_PLTT_ID(0), sizeof(sCaveTransitionPalette_Black));
    gTasks[taskId].func = Task_FlashTransition_Enter_2;
    gTasks[taskId].data[0] = 16;
    gTasks[taskId].data[1] = 0;
    gTasks[taskId].data[2] = 0;
}

static void Task_FlashTransition_Enter_2(u8 taskId)
{
    u16 count = gTasks[taskId].data[2];
    if (count < 16)
    {
        gTasks[taskId].data[2]++;
        gTasks[taskId].data[2]++;
        LoadPalette(&sCaveTransitionPalette_Enter[15 - count], BG_PLTT_ID(14), PLTT_SIZEOF(count + 1));
    }
    else
    {
        SetGpuReg(REG_OFFSET_BLDALPHA, (16 << 8) + 16);
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG0 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ | BLDCNT_TGT2_BD);
        gTasks[taskId].func = Task_FlashTransition_Enter_3;
    }
}

static void Task_FlashTransition_Enter_3(u8 taskId)
{
    u16 r4 = 16 - gTasks[taskId].data[1];
    SetGpuReg(REG_OFFSET_BLDALPHA, (16 << 8) + r4);
    if (r4 != 0)
    {
        gTasks[taskId].data[1]++;
    }
    else
    {
        LoadPalette(sCaveTransitionPalette_Black, BG_PLTT_ID(0), sizeof(sCaveTransitionPalette_Black));
        SetMainCallback2(gMain.savedCallback);
    }
}

static void RunMapPreviewScreen(u8 mapSecId)
{
    u8 taskId = CreateTask(Task_MapPreviewScreen_0, 0);
    gTasks[taskId].data[3] = mapSecId;
}

static void Task_MapPreviewScreen_0(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    switch (data[0])
    {
    case 0:
        SetWordTaskArg(taskId, 5, (uintptr_t)gMain.vblankCallback);
        SetVBlankCallback(NULL);
        MapPreview_InitBgs();
        MapPreview_LoadGfx(data[3]);
        BlendPalettes(PALETTES_ALL, 0x10, RGB_BLACK);
        data[0]++;
        break;
    case 1:
        if (!MapPreview_IsGfxLoadFinished())
        {
            data[4] = MapPreview_CreateMapNameWindow(data[3]);
            CopyWindowToVram(data[4], COPYWIN_FULL);
            data[0]++;
        }
        break;
    case 2:
        if (!IsDma3ManagerBusyWithBgCopy())
        {
            BeginNormalPaletteFade(PALETTES_ALL, -1, 16, 0, RGB_BLACK);
            SetVBlankCallback((IntrCallback)GetWordTaskArg(taskId, 5));
            data[0]++;
        }
        break;
    case 3:
        if (!UpdatePaletteFade())
        {
            data[2] = MapPreview_GetDuration(data[3]);
            data[0]++;
        }
        break;
    case 4:
        data[1]++;
        if (data[1] > data[2] || JOY_HELD(B_BUTTON))
        {
            BeginNormalPaletteFade(PALETTES_ALL, -2, 0, 16, RGB_WHITE);
            data[0]++;
        }
        break;
    case 5:
        if (!UpdatePaletteFade())
        {
            int i;
            for (i = 0; i < 16; i++)
            {
                data[i] = 0;
            }
            MapPreview_Unload(data[4]);
            gTasks[taskId].func = Task_FlashTransition_Enter_1;
        }
        break;
    }
}
