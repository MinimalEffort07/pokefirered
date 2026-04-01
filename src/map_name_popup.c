/**
 * @file map_name_popup.c
 * @brief Map Name Popup Display System
 *
 * FILE OVERVIEW:
 * This file implements the map name popup that slides in from the top of the screen
 * when the player enters a new area (e.g., "PALLET TOWN", "VIRIDIAN FOREST 1F").
 * The popup is a bordered text window that:
 * 1. Slides down from above the screen (invisible) to visible position
 * 2. Holds for about 2 seconds so the player can read it
 * 3. Slides back up and off screen
 * 4. Cleans up the window resources
 *
 * GBA CONTEXT — SCROLLING TRICK:
 * Instead of moving the window itself, this code uses BG0's vertical scroll offset
 * (BG0VOFS register) to scroll the ENTIRE background layer that contains the popup.
 * The window is created at a fixed tile position, but by changing the scroll offset,
 * it appears to slide in and out. This is a common GBA trick — hardware scrolling
 * is free (just a register write), while moving a window's tiles would require
 * expensive VRAM updates every frame.
 *
 * The popup window is placed at tilemap row 29 (off the bottom of the visible
 * 20-row screen), and the BG Y offset is set to a large negative value to
 * "wrap" it to the top. Then tPos is animated from 0 to -24 to scroll it into
 * view, and back to 0 to scroll it away.
 */
#include "global.h"
#include "gflib.h"
#include "task.h"
#include "event_data.h"
#include "text_window.h"
#include "quest_log.h"
#include "region_map.h"
#include "strings.h"

/* Floor number 127 represents the rooftop — displayed as "ROOFTOP" instead of "127F" */
#define FLOOR_ROOFTOP 127

static void Task_MapNamePopup(u8 taskId);
static u16 MapNamePopupCreateWindow(bool32 palIntoFadedBuffer);
static void MapNamePopupPrintMapNameOnWindow(u16 windowId);
static u8 *MapNamePopupAppendFloorNum(u8 *dest, s8 flags);

/*
 * Task data field aliases for readability.
 * These map the generic data[] array indices to meaningful names.
 */
#define tState              data[0]  /* Current state in the animation state machine */
#define tTimer              data[1]  /* Frame counter for the hold duration */
#define tPos                data[2]  /* Current vertical scroll position (pixels) */
#define tReshow             data[3]  /* Flag: should the popup be re-shown with a new name? */
#define tWindowId           data[4]  /* Window ID allocated for the popup */
#define tWindowExists       data[5]  /* Flag: has a window been created? */
#define tWindowCleared      data[6]  /* Flag: has the window content been cleared? */
#define tWindowDestroyed    data[7]  /* Flag: has the window been removed? */
#define tPalIntoFadedBuffer data[8]  /* Flag: load palette into faded buffer? (for fade transitions) */

/**
 * FUNCTION: ShowMapNamePopup
 *
 * PURPOSE: Initiates the map name popup animation when entering a new area.
 *
 * GAME LOGIC:
 * - Respects FLAG_DONT_SHOW_MAP_NAME_POPUP (set in certain scripted sequences)
 * - Won't show during quest log playback
 * - If a popup is already displaying, it triggers a "reshow" — the current popup
 *   slides out, then slides back in with the new map name. This handles the case
 *   where the player moves through areas faster than the popup animation.
 *
 * GBA CONTEXT:
 * ChangeBgY(0, -0x1081, 0) sets BG0's Y scroll to a negative value. The GBA's
 * background scroll registers use fixed-point values — 0x1081 is approximately
 * 16.5 pixels in the engine's fixed-point format. This positions the popup
 * window (at tilemap row 29) just above the top of the visible screen.
 *
 * @param palIntoFadedBuffer — If TRUE, load palette into the faded buffer (used when
 *                             screen is fading in, so the popup colors fade correctly)
 */
void ShowMapNamePopup(bool32 palIntoFadedBuffer)
{
    u8 taskId;
    if (FlagGet(FLAG_DONT_SHOW_MAP_NAME_POPUP) != TRUE && !QL_IS_PLAYBACK_STATE)
    {
        taskId = FindTaskIdByFunc(Task_MapNamePopup);
        if (taskId == TASK_NONE)
        {
            /* No popup currently active — create a new one */
            taskId = CreateTask(Task_MapNamePopup, 90);
            ChangeBgX(0,  0x0000, 0);     /* Reset BG0 horizontal scroll */
            ChangeBgY(0, -0x1081, 0);     /* Set BG0 vertical scroll to position popup off-screen */
            gTasks[taskId].tState = 0;
            gTasks[taskId].tPos = 0;       /* Start position: scrolled away (0 = hidden) */
            gTasks[taskId].tPalIntoFadedBuffer = palIntoFadedBuffer;
        }
        else
        {
            /* Popup already active — trigger a reshow with the new name */
            if (gTasks[taskId].tState != 4)
                gTasks[taskId].tState = 4;  /* Jump to "slide out" state */
            gTasks[taskId].tReshow = TRUE;  /* After sliding out, slide back in with new name */
        }
    }
}

/**
 * FUNCTION: Task_MapNamePopup
 *
 * PURPOSE: State machine that controls the popup's slide-in, hold, and slide-out animation.
 *
 * GAME LOGIC — STATE MACHINE:
 * State 0: Create the popup window, load graphics, and print the map name.
 * State 1: Wait for DMA transfers to complete (window graphics being copied to VRAM).
 * State 2: Slide in — decrease tPos by 2 pixels each frame until -24 (fully visible).
 *          Setting BG0VOFS to negative values scrolls the background up, revealing
 *          the popup window that was placed below the visible area.
 * State 3: Hold — wait 120 frames (2 seconds at 60fps) with the popup visible.
 * State 4: Slide out — increase tPos by 2 pixels each frame until 0 (hidden).
 *          If tReshow is set, go back to state 1 to show the new name.
 * State 5: (unused state, acts as a no-op)
 * State 6: Clear the window content and tilemap.
 * State 7: Wait for DMA to finish clearing, then remove the window and reset BG0 scroll.
 * State 8: Destroy the task — popup lifecycle is complete.
 *
 * The BG0VOFS hardware register is written at the end of most states (via the
 * SetGpuReg call after the switch) to keep the scroll position in sync with tPos.
 *
 * @param taskId — This task's ID in the task system
 */
static void Task_MapNamePopup(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->tState)
    {
    case 0:
        /* Create the popup window and load all graphics */
        task->tWindowId = MapNamePopupCreateWindow(task->tPalIntoFadedBuffer);
        task->tWindowExists = TRUE;
        task->tState = 1;
        break;
    case 1:
        /* Wait for DMA3 to finish copying window graphics to VRAM */
        if (IsDma3ManagerBusyWithBgCopy())
            break;
        // fallthrough — DMA done, start sliding in
    case 2:
        /* Slide in: move popup down by 2 pixels per frame */
        task->tPos -= 2;  /* Negative values scroll BG up, revealing the popup */
        if (task->tPos <= -24)  /* -24 pixels = fully slid into view (3 tiles * 8 pixels) */
        {
            task->tState = 3;  /* Start holding */
            task->tTimer = 0;
        }
        break;
    case 3:
        /* Hold: display the popup for 120 frames (2 seconds) */
        task->tTimer++;
        if (task->tTimer > 120)
        {
            task->tTimer = 0;
            task->tState = 4;  /* Start sliding out */
        }
        break;
    case 4:
        /* Slide out: move popup back up by 2 pixels per frame */
        task->tPos += 2;  /* Moving toward 0 scrolls BG back to normal, hiding popup */
        if (task->tPos >= 0)  /* 0 = fully hidden */
        {
            if (task->tReshow)
            {
                /* Re-show with new map name: reprint and slide back in */
                MapNamePopupPrintMapNameOnWindow(task->tWindowId);
                CopyWindowToVram(task->tWindowId, COPYWIN_GFX);
                task->tState = 1;  /* Go back to waiting for DMA, then slide in */
                task->tReshow = FALSE;
            }
            else
            {
                /* Done — proceed to cleanup */
                task->tState = 6;
                return;
            }
        }
    case 5:
        break;  /* No-op state (fallthrough target if state 4 didn't return) */
    case 6:
        /* Clear the window content from VRAM */
        if (task->tWindowExists && !task->tWindowCleared)
        {
            rbox_fill_rectangle(task->tWindowId);  /* Fill the window area with blank tiles */
            CopyWindowToVram(task->tWindowId, COPYWIN_MAP);  /* Push tilemap changes to VRAM */
            task->tWindowCleared = TRUE;
        }
        task->tState = 7;
        return;
    case 7:
        /* Wait for clearing DMA to finish, then remove the window */
        if (!IsDma3ManagerBusyWithBgCopy())
        {
            if (task->tWindowExists)
            {
                RemoveWindow(task->tWindowId);  /* Free the window slot */
                task->tWindowExists = FALSE;
                task->tWindowDestroyed = TRUE;
            }
            task->tState = 8;
            ChangeBgY(0, 0x00000000, 0);  /* Reset BG0 Y scroll to normal */
        }
        return;
    case 8:
        /* All done — destroy the task */
        DestroyTask(taskId);
        return;
    }
    /* Update the hardware scroll register to match the current animation position.
     * REG_OFFSET_BG0VOFS is the offset from the I/O register base for BG0's
     * vertical scroll register (0x04000012). Writing tPos here moves the background. */
    SetGpuReg(REG_OFFSET_BG0VOFS, task->tPos);
}

/**
 * FUNCTION: DismissMapNamePopup
 *
 * PURPOSE: Forces the popup to start sliding out immediately, skipping the hold timer.
 *          Used when a script or screen transition needs the popup gone right away.
 */
void DismissMapNamePopup(void)
{
    u8 taskId;
    s16 *data;
    taskId = FindTaskIdByFunc(Task_MapNamePopup);
    if (taskId != TASK_NONE)
    {
        data = gTasks[taskId].data;
        if (tState < 6)
            tState = 6;  /* Jump straight to cleanup, skipping the slide-out */
    }
}

/**
 * FUNCTION: IsMapNamePopupTaskActive
 *
 * PURPOSE: Checks whether a map name popup is currently active (any state).
 *
 * RETURNS: TRUE if a popup task exists, FALSE otherwise
 */
bool32 IsMapNamePopupTaskActive(void)
{
    return FindTaskIdByFunc(Task_MapNamePopup) != TASK_NONE ? TRUE : FALSE;
}

/* Palette number used for the popup window frame (background palette 13) */
#define WIN_PAL_NUM  13

/**
 * FUNCTION: MapNamePopupCreateWindow
 *
 * PURPOSE: Creates the popup window, loads its graphics and palette, draws the
 *          border, and prints the map name text.
 *
 * HOW IT WORKS:
 * The window is created on BG layer 0, positioned at tilemap column 1, row 29
 * (below the visible 20-row screen area). The base width is 14 tiles (112 pixels),
 * which is widened if the map has a floor number:
 * - Normal floors (B1F, 2F, etc.): +5 tiles = 19 tiles wide
 * - Rooftop: +8 tiles = 22 tiles wide (to fit "ROOFTOP" text)
 *
 * The tileNum offset is adjusted based on width to prevent overlapping with
 * the border tiles in VRAM.
 *
 * GBA CONTEXT:
 * palintoFadedBuffer controls where the palette is loaded:
 * - TRUE: LoadPalette writes directly to hardware palette RAM — used when the
 *   screen is already displaying and needs immediate visibility
 * - FALSE: CpuCopy16 writes to gPlttBufferUnfaded (the unfaded palette buffer) —
 *   used when a fade-in will happen later, so the popup fades in with the screen
 *
 * @param palintoFadedBuffer — Where to load the palette data
 * RETURNS: The window ID of the created popup window
 */
static u16 MapNamePopupCreateWindow(bool32 palintoFadedBuffer)
{
    struct WindowTemplate windowTemplate = {
        .bg = 0,              /* Background layer 0 */
        .tilemapLeft = 1,     /* Column 1 (leaving 1 tile margin on left) */
        .tilemapTop = 29,     /* Row 29 — below the visible screen (rows 0-19) */
        .width = 14,          /* 14 tiles wide by default (112 pixels) */
        .height = 2,          /* 2 tiles tall (16 pixels — fits one line of text plus padding) */
        .paletteNum = WIN_PAL_NUM,  /* Use background palette 13 */
        .baseBlock = 0x001    /* Starting tile number in VRAM for this window's content */
    };
    u16 windowId;
    u16 tileNum = 0x01D;  /* VRAM tile offset for the border frame graphics */

    /* Widen the window if the map has a floor number */
    if (gMapHeader.floorNum != 0)
    {
        if (gMapHeader.floorNum != FLOOR_ROOFTOP)
        {
            /* Normal floor (e.g., "POKEMON TOWER 3F") — need extra space for floor label */
            windowTemplate.width += 5;
            tileNum = 0x027;  /* Adjusted tile offset to avoid overlap */
        }
        else
        {
            /* Rooftop — "POKEMON TOWER ROOFTOP" needs even more space */
            // ROOFTOP
            windowTemplate.width += 8;
            tileNum = 0x02D;
        }
    }
    windowId = AddWindow(&windowTemplate);

    /* Load the popup's palette — destination depends on fade state */
    if (palintoFadedBuffer)
        LoadPalette(GetTextWindowPalette(3), BG_PLTT_ID(WIN_PAL_NUM), PLTT_SIZE_4BPP);
    else
        CpuCopy16(GetTextWindowPalette(3), &gPlttBufferUnfaded[BG_PLTT_ID(WIN_PAL_NUM)], PLTT_SIZE_4BPP);

    /* Load and draw the window border, then render the map name text */
    LoadStdWindowTiles(windowId, tileNum);
    DrawTextBorderOuter(windowId, tileNum, WIN_PAL_NUM);
    PutWindowTilemap(windowId);
    MapNamePopupPrintMapNameOnWindow(windowId);
    CopyWindowToVram(windowId, COPYWIN_FULL);  /* Copy both tilemap and graphics to VRAM */
    return windowId;
}

/**
 * FUNCTION: MapNamePopupPrintMapNameOnWindow
 *
 * PURPOSE: Renders the map name (and optional floor number) centered in the popup window.
 *
 * HOW IT WORKS:
 * 1. Gets the map name from the region map data
 * 2. If the map has a floor number, appends it (e.g., " 3F", " B2F", " ROOFTOP")
 * 3. Calculates the horizontal center position by measuring the string width
 *    and centering within the available width (112, 152, or 176 pixels)
 * 4. Fills the window with color 1 (background) and draws the text
 *
 * @param windowId — The window to print the map name into
 */
static void MapNamePopupPrintMapNameOnWindow(u16 windowId)
{
    u8 mapName[25];  /* Buffer for the map name string (max ~20 chars) */
    u32 maxWidth = 112;  /* Default text area width in pixels (14 tiles * 8) */
    u32 xpos;

    /* Get the map name and optionally append floor number */
    u8 *ptr = GetMapName(mapName, gMapHeader.regionMapSectionId, 0);
    if (gMapHeader.floorNum != 0)
    {
        ptr = MapNamePopupAppendFloorNum(ptr, gMapHeader.floorNum);
        maxWidth = gMapHeader.floorNum != FLOOR_ROOFTOP ? 152 : 176;
    }

    /* Center the text horizontally within the window */
    xpos = (maxWidth - GetStringWidth(FONT_NORMAL, mapName, -1)) / 2;

    /* Clear window to background color (palette index 1) and draw the name */
    FillWindowPixelBuffer(windowId, PIXEL_FILL(1));
    AddTextPrinterParameterized(windowId, FONT_NORMAL, mapName, xpos, 2, TEXT_SKIP_DRAW, NULL);
}

/**
 * FUNCTION: MapNamePopupAppendFloorNum
 *
 * PURPOSE: Appends a floor number suffix to the map name string.
 *
 * GAME LOGIC:
 * Multi-floor buildings like Pokemon Tower and Silph Co. show which floor the
 * player is on. The format varies:
 * - Positive numbers: "1F", "2F", etc. (above ground)
 * - Negative numbers: "B1F", "B2F", etc. (basement, the B prefix is added)
 * - FLOOR_ROOFTOP (127): Shows "ROOFTOP" text instead of a number
 *
 * @param dest — Pointer to the current end of the map name string
 * @param floorNum — The floor number (positive = above, negative = basement, 127 = roof)
 * RETURNS: Pointer to the new end of the string
 */
static u8 *MapNamePopupAppendFloorNum(u8 *dest, s8 floorNum)
{
    if (floorNum == 0)
        return dest;  /* No floor number — shouldn't happen but handle gracefully */
    *dest++ = CHAR_SPACE;  /* Space separator between map name and floor */
    if (floorNum == FLOOR_ROOFTOP)
        return StringCopy(dest, gText_Rooftop2);  /* Special "ROOFTOP" text */
    if (floorNum < 0)
    {
        *dest++ = CHAR_B;       /* "B" prefix for basement floors */
        floorNum *= -1;         /* Make positive for number display */
    }
    dest = ConvertIntToDecimalStringN(dest, floorNum, STR_CONV_MODE_LEFT_ALIGN, 2);
    *dest++ = CHAR_F;           /* "F" suffix (e.g., "3F") */
    *dest = EOS;                /* Null terminator */
    return dest;
}

/* Clean up task data macros to prevent pollution of the global macro namespace */
#undef tPalIntoFadedBuffer
#undef tWindowDestroyed
#undef tWindowCleared
#undef tWindowExists
#undef tWindowId
#undef tReshow
#undef tPos
#undef tTimer
#undef tState
