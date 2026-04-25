/**
 * =QUICK SELECT MENU=
 *
 * A simple list menu triggered by SELECT in the overworld.  Shows usable
 * HMs (owned item + badge) and registered items in one scrollable list.
 * Uses the same window frame as the START menu.
 *
 * HMs can be used without teaching them to a pokemon — the lead party
 * pokemon performs the field animation.
 */
#include "global.h"
#include "gflib.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_effect.h"
#include "field_player_avatar.h"
#include "item.h"
#include "item_menu.h"
#include "link.h"
#include "list_menu.h"
#include "map_name_popup.h"
#include "menu.h"
#include "new_menu_helpers.h"
#include "overworld.h"
#include "party_menu.h"
#include "region_map.h"
#include "script.h"
#include "sound.h"
#include "task.h"
#include "window.h"
#include "data.h"
#include "quick_select_menu.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/party_menu.h"
#include "constants/songs.h"
#include "event_scripts.h"
#include "pokemon_storage_system.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define NUM_HMS            7
#define MAX_LIST_ENTRIES   (NUM_HMS + REGISTERED_ITEMS_MAX + 1)
#define MAX_VISIBLE        8   /* max items shown before scrolling */

/* List item ID encoding: 0-6 = HM, 100+ = registered item slot, 200 = PC */
#define QS_ID_HM_BASE     0
#define QS_ID_ITEM_BASE   100
#define QS_ID_PC          200

/* Task states */
#define STATE_OPEN    0
#define STATE_INPUT   1
#define STATE_CLOSE   2

/* Window sizing */
#define WIN_WIDTH     13  /* 104 px — fits 14-char item names + cursor */
#define WIN_LEFT      16  /* right-aligned with 1-tile margin */
#define WIN_TOP       1
#define WIN_BASEBLOCK 0x220

/* ── HM data table ────────────────────────────────────────────────────── */

static const struct {
    u16 itemId;
    u8  fieldMove;
    u16 moveId;
} sHmData[NUM_HMS] = {
    { ITEM_HM05, FIELD_MOVE_FLASH,      MOVE_FLASH      },
    { ITEM_HM01, FIELD_MOVE_CUT,        MOVE_CUT        },
    { ITEM_HM02, FIELD_MOVE_FLY,        MOVE_FLY        },
    { ITEM_HM04, FIELD_MOVE_STRENGTH,   MOVE_STRENGTH   },
    { ITEM_HM03, FIELD_MOVE_SURF,       MOVE_SURF       },
    { ITEM_HM06, FIELD_MOVE_ROCK_SMASH, MOVE_ROCK_SMASH },
    { ITEM_HM07, FIELD_MOVE_WATERFALL,  MOVE_WATERFALL  },
};

static const u8 sText_BillsPC[] = _("BILL'S PC");

/* ── Static state ──────────────────────────────────────────────────────── */

static EWRAM_DATA struct {
    u8 taskId;
    u8 windowId;
    u8 listTaskId;
    u8 numEntries;
    u16 cursorPos;
    u16 itemsAbove;
} sQS = {0};

static EWRAM_DATA struct ListMenuItem sListItems[MAX_LIST_ENTRIES] = {0};

/* ── Forward declarations ──────────────────────────────────────────────── */

static void Task_QuickSelectMenu(u8 taskId);
static void BuildMenuList(void);
static bool8 ExecuteHmSelection(u8 hmIndex);
static bool8 ExecuteItemSelection(u8 slot);
static void ExecutePCSelection(u8 taskId);
static void CloseQuickSelectMenu(u8 taskId);

/* ── HM helpers ────────────────────────────────────────────────────────── */

static bool8 IsHmUsable(u8 hmIndex)
{
    if (!CheckBagHasItem(sHmData[hmIndex].itemId, 1))
        return FALSE;
    return FlagGet(FLAG_BADGE01_GET + sHmData[hmIndex].fieldMove);
}

/* ── List building ─────────────────────────────────────────────────────── */

/*
 * Populates sListItems[] with usable HMs first, then registered items
 * that are still in the bag.  Only entries the player can actually use
 * appear in the list.
 */
static void BuildMenuList(void)
{
    u8 i;
    u16 itemId;

    sQS.numEntries = 0;

    /* Usable HMs */
    for (i = 0; i < NUM_HMS; i++)
    {
        if (IsHmUsable(i))
        {
            sListItems[sQS.numEntries].label = gMoveNames[sHmData[i].moveId];
            sListItems[sQS.numEntries].index = QS_ID_HM_BASE + i;
            sQS.numEntries++;
        }
    }

    /* Registered items still in bag */
    for (i = 0; i < REGISTERED_ITEMS_MAX; i++)
    {
        itemId = gSaveBlock1Ptr->registeredItems[i];
        if (itemId != ITEM_NONE && CheckBagHasItem(itemId, 1))
        {
            sListItems[sQS.numEntries].label = ItemId_GetName(itemId);
            sListItems[sQS.numEntries].index = QS_ID_ITEM_BASE + i;
            sQS.numEntries++;
        }
    }

    /* BILL'S PC — always available in the overworld */
    sListItems[sQS.numEntries].label = sText_BillsPC;
    sListItems[sQS.numEntries].index = QS_ID_PC;
    sQS.numEntries++;
}

/* ── Execution ─────────────────────────────────────────────────────────── */

static bool8 ExecuteHmSelection(u8 hmIndex)
{
    u8 fieldMove = sHmData[hmIndex].fieldMove;

    gPartyMenu.slotId = 0;

    if (!TrySetUpFieldMoveFromQuickSelect(fieldMove))
    {
        /*
         * Conditions not met (no tree, no water, etc.).
         * Close the menu and show a message explaining it.
         * ScriptContext_SetupScript handles locking/unlocking
         * the player and displaying the message box.
         */
        DestroyListMenuTask(sQS.listTaskId, NULL, NULL);
        ClearStdWindowAndFrameToTransparent(sQS.windowId, TRUE);
        RemoveWindow(sQS.windowId);
        ScheduleBgCopyTilemapToVram(0);
        UnfreezeObjectEvents();
        UnlockPlayerFieldControls();
        DestroyTask(sQS.taskId);
        ScriptContext_SetupScript(EventScript_CantUseHMHere);
        return TRUE; /* signal that we handled it (task destroyed) */
    }

    /* Tear down menu — leave player locked for the field effect */
    DestroyListMenuTask(sQS.listTaskId, NULL, NULL);
    ClearStdWindowAndFrameToTransparent(sQS.windowId, TRUE);
    RemoveWindow(sQS.windowId);
    ScheduleBgCopyTilemapToVram(0);
    DestroyTask(sQS.taskId);

    if (fieldMove == FIELD_MOVE_FLY)
    {
        UnfreezeObjectEvents();
        UnlockPlayerFieldControls();
        SetMainCallback2(CB2_OpenFlyMap);
    }
    else
    {
        gFieldCallback2 = NULL;
        gPostMenuFieldCallback();
    }
    return TRUE;
}

static bool8 ExecuteItemSelection(u8 slot)
{
    u16 itemId;
    u8 newTaskId;

    itemId = gSaveBlock1Ptr->registeredItems[slot];
    if (itemId == ITEM_NONE || !CheckBagHasItem(itemId, 1))
        return FALSE;

    /* Tear down menu — leave player locked for the item callback.
     * Note: the list task was already destroyed by the caller (STATE_INPUT)
     * before calling this function, so we must NOT destroy it again here. */
    ClearStdWindowAndFrameToTransparent(sQS.windowId, TRUE);
    RemoveWindow(sQS.windowId);
    ScheduleBgCopyTilemapToVram(0);
    DestroyTask(sQS.taskId);

    gSpecialVar_ItemId = itemId;
    newTaskId = CreateTask(ItemId_GetFieldFunc(itemId), 8);
    gTasks[newTaskId].data[3] = 1;
    return TRUE;
}

/*
 * ExecutePCSelection
 *
 * Tears down the Quick Select menu and opens the full PC storage screen.
 * The player and object events are unfrozen/unlocked first (mirroring the
 * FLY path in ExecuteHmSelection) before handing control to
 * ShowPokemonStorageSystemPC(), which installs its own callbacks.
 *
 * sQS.taskId is the Quick Select task's own ID; taskId passed here is the
 * same value (the task destroys itself via DestroyTask before returning).
 */
static void ExecutePCSelection(u8 taskId)
{
    DestroyListMenuTask(sQS.listTaskId, NULL, NULL);
    ClearStdWindowAndFrameToTransparent(sQS.windowId, TRUE);
    RemoveWindow(sQS.windowId);
    ScheduleBgCopyTilemapToVram(0);
    UnfreezeObjectEvents();
    UnlockPlayerFieldControls();
    DestroyTask(taskId);
    ShowPokemonStorageSystemPC();
}

/* ── Close ─────────────────────────────────────────────────────────────── */

static void CloseQuickSelectMenu(u8 taskId)
{
    DestroyListMenuTask(sQS.listTaskId, NULL, NULL);
    ClearStdWindowAndFrameToTransparent(sQS.windowId, TRUE);
    RemoveWindow(sQS.windowId);
    ScheduleBgCopyTilemapToVram(0);
    UnfreezeObjectEvents();
    UnlockPlayerFieldControls();
    DestroyTask(taskId);
}

/* ── Task state machine ────────────────────────────────────────────────── */

static void Task_QuickSelectMenu(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    s32 input;
    u8 maxShowed;
    struct WindowTemplate winTemplate;
    struct ListMenuTemplate listTemplate;

    switch (data[0])
    {
    case STATE_OPEN:
        BuildMenuList();

        if (sQS.numEntries == 0)
        {
            /* Nothing to show — close immediately */
            UnfreezeObjectEvents();
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
            return;
        }

        /* Create window sized to fit the list */
        maxShowed = sQS.numEntries;
        if (maxShowed > MAX_VISIBLE)
            maxShowed = MAX_VISIBLE;

        winTemplate = SetWindowTemplateFields(
            0,                      /* bg */
            WIN_LEFT,               /* left */
            WIN_TOP,                /* top */
            WIN_WIDTH,              /* width */
            maxShowed * 2,          /* height in tiles (2 per entry) */
            14,                     /* paletteNum = STD_WINDOW_PALETTE_NUM */
            WIN_BASEBLOCK           /* baseBlock */
        );

        sQS.windowId = AddWindow(&winTemplate);
        LoadStdWindowFrameGfx();
        DrawStdWindowFrame(sQS.windowId, FALSE);
        PutWindowTilemap(sQS.windowId);

        /* Init list menu (same pattern as start menu) */
        listTemplate.items = sListItems;
        listTemplate.moveCursorFunc = ListMenuDefaultCursorMoveFunc;
        listTemplate.itemPrintFunc = NULL;
        listTemplate.totalItems = sQS.numEntries;
        listTemplate.maxShowed = maxShowed;
        listTemplate.windowId = sQS.windowId;
        listTemplate.header_X = 0;
        listTemplate.item_X = 8;
        listTemplate.cursor_X = 0;
        listTemplate.upText_Y = 1;
        listTemplate.cursorPal = 2;
        listTemplate.fillValue = 1;
        listTemplate.cursorShadowPal = 3;
        listTemplate.lettersSpacing = 0;
        listTemplate.itemVerticalPadding = 0;
        listTemplate.scrollMultiple = LIST_NO_MULTIPLE_SCROLL;
        listTemplate.fontId = FONT_NORMAL;
        listTemplate.cursorKind = 0;

        /* Clamp saved cursor position to prevent out-of-bounds reads when
         * the number of entries decreased since the last open (e.g., item
         * was consumed or a badge was lost). */
        if (sQS.cursorPos + sQS.itemsAbove >= sQS.numEntries)
        {
            sQS.cursorPos = 0;
            sQS.itemsAbove = 0;
        }

        sQS.listTaskId = ListMenuInit(&listTemplate, sQS.cursorPos, sQS.itemsAbove);
        CopyWindowToVram(sQS.windowId, COPYWIN_FULL);
        ScheduleBgCopyTilemapToVram(0);

        data[0] = STATE_INPUT;
        break;

    case STATE_INPUT:
        input = ListMenu_ProcessInput(sQS.listTaskId);

        if (JOY_NEW(B_BUTTON))
        {
            PlaySE(SE_SELECT);
            /* Remember cursor position for next open */
            DestroyListMenuTask(sQS.listTaskId, &sQS.cursorPos, &sQS.itemsAbove);
            sQS.listTaskId = 0;
            data[0] = STATE_CLOSE;
            break;
        }

        if (input != LIST_NOTHING_CHOSEN && input != LIST_CANCEL)
        {
            PlaySE(SE_SELECT);

            if (input == QS_ID_PC)
            {
                /* PC selection — tear down menu and open Bill's PC storage */
                ExecutePCSelection(taskId);
                return; /* task destroyed */
            }
            else if (input < QS_ID_ITEM_BASE)
            {
                /*
                 * HM selection — ExecuteHmSelection always destroys the
                 * menu.  On success it starts the field effect; on failure
                 * it shows "That move can't be used here."
                 */
                ExecuteHmSelection((u8)input);
                return; /* task destroyed */
            }
            else
            {
                /* Item selection */
                DestroyListMenuTask(sQS.listTaskId, &sQS.cursorPos, &sQS.itemsAbove);
                sQS.listTaskId = 0;
                if (ExecuteItemSelection((u8)(input - QS_ID_ITEM_BASE)))
                    return; /* task destroyed */
            }

            /* Item execution failed — close menu */
            PlaySE(SE_FAILURE);
            data[0] = STATE_CLOSE;
            break;
        }
        break;

    case STATE_CLOSE:
        if (sQS.listTaskId != 0)
        {
            DestroyListMenuTask(sQS.listTaskId, &sQS.cursorPos, &sQS.itemsAbove);
            sQS.listTaskId = 0;
        }
        ClearStdWindowAndFrameToTransparent(sQS.windowId, TRUE);
        RemoveWindow(sQS.windowId);
        ScheduleBgCopyTilemapToVram(0);
        UnfreezeObjectEvents();
        UnlockPlayerFieldControls();
        DestroyTask(taskId);
        break;
    }
}

/* ── Public: entry point ───────────────────────────────────────────────── */

bool8 OpenQuickSelectMenu(void)
{
    if (InUnionRoom() == TRUE)
        return FALSE;

    DismissMapNamePopup();
    ChangeBgY(0, 0, 0);
    LockPlayerFieldControls();
    FreezeObjectEvents();
    HandleEnforcedLookDirectionOnPlayerStopMoving();
    StopPlayerAvatar();

    sQS.taskId = CreateTask(Task_QuickSelectMenu, 0);
    gTasks[sQS.taskId].data[0] = STATE_OPEN;
    return TRUE;
}

/* ── Public: item registration helpers (called by item_menu.c) ─────────── */

bool8 QuickSelect_IsItemRegistered(u16 itemId)
{
    u8 i;

    for (i = 0; i < REGISTERED_ITEMS_MAX; i++)
    {
        if (gSaveBlock1Ptr->registeredItems[i] == itemId)
            return TRUE;
    }
    return FALSE;
}

bool8 QuickSelect_HasEmptyItemSlot(void)
{
    u8 i;

    for (i = 0; i < REGISTERED_ITEMS_MAX; i++)
    {
        if (gSaveBlock1Ptr->registeredItems[i] == ITEM_NONE)
            return TRUE;
    }
    return FALSE;
}

void QuickSelect_RegisterItem(u16 itemId)
{
    u8 i;

    for (i = 0; i < REGISTERED_ITEMS_MAX; i++)
    {
        if (gSaveBlock1Ptr->registeredItems[i] == ITEM_NONE)
        {
            gSaveBlock1Ptr->registeredItems[i] = itemId;
            return;
        }
    }
}

void QuickSelect_UnregisterItem(u16 itemId)
{
    u8 i;

    for (i = 0; i < REGISTERED_ITEMS_MAX; i++)
    {
        if (gSaveBlock1Ptr->registeredItems[i] == itemId)
        {
            gSaveBlock1Ptr->registeredItems[i] = ITEM_NONE;
            return;
        }
    }
}
