/**
 * @file field_message_box.c
 * @brief Overworld Message Box Display System
 *
 * FILE OVERVIEW:
 * This file manages the text message boxes that appear on the overworld (field) screen.
 * These are the dialogue boxes at the bottom of the screen where NPC dialogue, sign
 * text, and system messages appear. The system handles:
 * - Loading appropriate window graphics (normal dialogue frame vs. signpost frame)
 * - Expanding text placeholders (like player name, Pokemon names)
 * - Drawing the text with proper styling
 * - Auto-scrolling text for quest log playback
 *
 * ARCHITECTURE:
 * The message box uses a simple state machine with three states:
 * - FIELD_MESSAGE_BOX_HIDDEN: No message box visible
 * - FIELD_MESSAGE_BOX_NORMAL: Standard dialogue (A button can speed up text)
 * - FIELD_MESSAGE_BOX_AUTO_SCROLL: Text scrolls automatically (quest log playback)
 *
 * Text rendering is handled asynchronously via Tasks — the Task_DrawFieldMessageBox
 * runs each frame to progress through the setup stages (load graphics, draw frame,
 * wait for text to finish), allowing the game to continue processing other things
 * (like animations) while text is being displayed.
 */
#include "global.h"
#include "field_message_box.h"
#include "gflib.h"
#include "new_menu_helpers.h"
#include "quest_log.h"
#include "script.h"
#include "text_window.h"

/* Current state of the field message box — stored in EWRAM */
static EWRAM_DATA u8 sMessageBoxType = 0;

static void ExpandStringAndStartDrawFieldMessageBox(const u8 *str);
static void StartDrawFieldMessageBox(void);

/**
 * FUNCTION: InitFieldMessageBox
 *
 * PURPOSE: Resets the field message box system to its default state.
 *          Called when entering the overworld or after clearing a message.
 *
 * GAME LOGIC:
 * Initializes all text control flags:
 * - canABSpeedUpPrint: Whether pressing A/B makes text appear faster
 * - useAlternateDownArrow: Whether to use a different "more text" arrow style
 * - autoScroll: Whether text advances automatically without button input
 *   (used during quest log playback)
 */
void InitFieldMessageBox(void)
{
    sMessageBoxType = FIELD_MESSAGE_BOX_HIDDEN;
    gTextFlags.canABSpeedUpPrint = FALSE;
    gTextFlags.useAlternateDownArrow = FALSE;
    gTextFlags.autoScroll = FALSE;
}

/**
 * FUNCTION: Task_DrawFieldMessageBox
 *
 * PURPOSE: Asynchronous task that sets up and draws a field message box over
 *          multiple frames.
 *
 * GAME LOGIC — STATE MACHINE (task->data[0]):
 * State 0: Load the window frame graphics.
 *   - During quest log playback: loads special quest log window tiles and
 *     enables auto-scroll so text advances without player input
 *   - For signs (IsMsgSignpost): loads the signpost-style wooden frame
 *   - For normal dialogue: loads the standard message box frame
 * State 1: Draw the dialogue frame border on BG window 0, with the text
 *   content already queued for printing.
 * State 2: Wait for the text printer to finish displaying all text.
 *   RunTextPrinters_CheckPrinter0Active returns TRUE while text is still
 *   being drawn. Once done, the message box type is reset to HIDDEN
 *   and the task self-destructs.
 *
 * @param taskId — This task's ID in the task system
 */
static void Task_DrawFieldMessageBox(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    switch (task->data[0])
    {
    case 0:
        /* State 0: Load window frame graphics based on context */
        if (gQuestLogState == QL_STATE_PLAYBACK)
        {
            /* Quest log playback mode — auto-scroll text, use quest log window style */
            gTextFlags.autoScroll = TRUE;
            LoadQuestLogWindowTiles(0, 0x200);  /* Load tiles at offset 0x200 in VRAM */
        }
        else if (!IsMsgSignpost())
            LoadStdWindowFrameGfx();   /* Normal dialogue — standard frame */
        else
            LoadSignpostWindowFrameGfx();  /* Reading a sign — wooden signpost frame */
        task->data[0]++;
        break;
    case 1:
        /* State 1: Draw the dialogue frame border and text area */
        DrawDialogueFrame(0, TRUE);  /* Window 0, clear old contents */
        task->data[0]++;
        break;
    case 2:
        /* State 2: Wait for text printing to complete */
        if (RunTextPrinters_CheckPrinter0Active() != TRUE)
        {
            /* Text is fully displayed — clean up */
            sMessageBoxType = FIELD_MESSAGE_BOX_HIDDEN;
            DestroyTask(taskId);
        }
        break;
    }
}

/**
 * FUNCTION: CreateTask_DrawFieldMessageBox
 *
 * PURPOSE: Creates the message box drawing task at priority 80.
 */
static void CreateTask_DrawFieldMessageBox(void)
{
    CreateTask(Task_DrawFieldMessageBox, 80);
}

/**
 * FUNCTION: DestroyTask_DrawFieldMessageBox
 *
 * PURPOSE: Finds and destroys any active message box drawing task.
 *
 * GAME LOGIC:
 * FindTaskIdByFunc searches for a task with the given function pointer.
 * Returns 0xFF if no such task exists, so we check before destroying.
 */
static void DestroyTask_DrawFieldMessageBox(void)
{
    u8 taskId = FindTaskIdByFunc(Task_DrawFieldMessageBox);
    if (taskId != 0xFF)
        DestroyTask(taskId);
}

/**
 * FUNCTION: ShowFieldMessage
 *
 * PURPOSE: Displays a standard dialogue message on the overworld. This is the
 *          main entry point for showing text from scripts and NPC interactions.
 *
 * GAME LOGIC:
 * Only one message box can be active at a time. If one is already showing,
 * this function returns FALSE (the caller should wait and try again).
 * The string is expanded first (replacing placeholders like {PLAYER} with
 * the actual player name) before being queued for display.
 *
 * @param str — The message string to display (may contain placeholders)
 * RETURNS: TRUE if the message was successfully started, FALSE if a message is already showing
 */
bool8 ShowFieldMessage(const u8 *str)
{
    if (sMessageBoxType != FIELD_MESSAGE_BOX_HIDDEN)
        return FALSE;  /* A message is already displaying */
    ExpandStringAndStartDrawFieldMessageBox(str);
    sMessageBoxType = FIELD_MESSAGE_BOX_NORMAL;
    return TRUE;
}

/**
 * FUNCTION: ShowFieldAutoScrollMessage
 *
 * PURPOSE: Displays an auto-scrolling message that advances without player input.
 *          Used during quest log playback.
 *
 * @param str — The message string to display
 * RETURNS: TRUE if started successfully, FALSE if another message is active
 */
bool8 ShowFieldAutoScrollMessage(const u8 *str)
{
    if (sMessageBoxType != FIELD_MESSAGE_BOX_HIDDEN)
        return FALSE;
    sMessageBoxType = FIELD_MESSAGE_BOX_AUTO_SCROLL;
    ExpandStringAndStartDrawFieldMessageBox(str);
    return TRUE;
}

/**
 * FUNCTION: ForceShowFieldAutoScrollMessage
 *
 * PURPOSE: Forces an auto-scroll message to display, ignoring any currently
 *          active message box. (Unused in the final game.)
 *
 * @param str — The message string to display
 * RETURNS: Always TRUE
 */
// Unused
static bool8 ForceShowFieldAutoScrollMessage(const u8 *str)
{
    sMessageBoxType = FIELD_MESSAGE_BOX_AUTO_SCROLL;
    ExpandStringAndStartDrawFieldMessageBox(str);
    return TRUE;
}

// Unused
// Same as ShowFieldMessage, but instead of accepting a string argument,
// it just prints whatever that's already in gStringVar4
/**
 * FUNCTION: ShowFieldMessageFromBuffer
 *
 * PURPOSE: Shows a message from the pre-filled gStringVar4 buffer instead of
 *          expanding a new string. (Unused in the final game.)
 *
 * RETURNS: TRUE if started successfully, FALSE if another message is active
 */
static bool8 ShowFieldMessageFromBuffer(void)
{
    if (sMessageBoxType != FIELD_MESSAGE_BOX_HIDDEN)
        return FALSE;
    sMessageBoxType = FIELD_MESSAGE_BOX_NORMAL;
    StartDrawFieldMessageBox();
    return TRUE;
}

/**
 * FUNCTION: ExpandStringAndStartDrawFieldMessageBox
 *
 * PURPOSE: Expands placeholder tokens in the string (like {PLAYER}, {STR_VAR_1})
 *          into their actual values, then starts the message box drawing task.
 *
 * HOW IT WORKS:
 * StringExpandPlaceholders processes the input string, replacing tokens like
 * {PLAYER} with gSaveBlock2Ptr->playerName, and stores the result in gStringVar4.
 * AddTextPrinterDiffStyle queues the expanded text for rendering.
 * The task then handles the actual frame drawing and text animation.
 *
 * @param str — The raw message string with placeholders
 */
static void ExpandStringAndStartDrawFieldMessageBox(const u8 *str)
{
    StringExpandPlaceholders(gStringVar4, str);
    AddTextPrinterDiffStyle(TRUE);
    CreateTask_DrawFieldMessageBox();
}

/**
 * FUNCTION: StartDrawFieldMessageBox
 *
 * PURPOSE: Starts drawing a message box using the text already in gStringVar4.
 *          Used when the string expansion was already done separately.
 */
static void StartDrawFieldMessageBox(void)
{
    AddTextPrinterDiffStyle(TRUE);
    CreateTask_DrawFieldMessageBox();
}

/**
 * FUNCTION: HideFieldMessageBox
 *
 * PURPOSE: Immediately removes the message box from the screen and resets state.
 *
 * GAME LOGIC:
 * Called when a script explicitly closes the message box (e.g., after the player
 * presses A to dismiss dialogue). Destroys the drawing task, clears the window
 * and its frame from the screen, and resets the message box state to HIDDEN.
 * Window 0 is the standard dialogue window used for field messages.
 */
void HideFieldMessageBox(void)
{
    DestroyTask_DrawFieldMessageBox();
    ClearDialogWindowAndFrame(0, TRUE);  /* Clear window 0 contents and frame border */
    sMessageBoxType = FIELD_MESSAGE_BOX_HIDDEN;
}

/**
 * FUNCTION: GetFieldMessageBoxType
 *
 * PURPOSE: Returns the current message box state (HIDDEN, NORMAL, or AUTO_SCROLL).
 *
 * RETURNS: The current sMessageBoxType value
 */
u8 GetFieldMessageBoxType(void)
{
    return sMessageBoxType;
}

/**
 * FUNCTION: IsFieldMessageBoxHidden
 *
 * PURPOSE: Checks whether the message box is currently hidden (no message displaying).
 *          Commonly used by other systems to know when a message has finished.
 *
 * RETURNS: TRUE if no message box is visible, FALSE if one is active
 */
bool8 IsFieldMessageBoxHidden(void)
{
    if (sMessageBoxType == FIELD_MESSAGE_BOX_HIDDEN)
        return TRUE;
    else
        return FALSE;
}

/**
 * FUNCTION: ReplaceFieldMessageWithFrame
 *
 * PURPOSE: Replaces the current dialogue message box with just a standard
 *          window frame (no text). (Unused in the final game.)
 *
 * GAME LOGIC:
 * This would clear the dialogue text but leave a bordered window frame on screen.
 * Possibly intended for a UI element that was cut during development.
 */
// Unused
static void ReplaceFieldMessageWithFrame(void)
{
    DestroyTask_DrawFieldMessageBox();
    DrawStdWindowFrame(0, TRUE);  /* Draw just the frame border on window 0 */
    sMessageBoxType = FIELD_MESSAGE_BOX_HIDDEN;
}
