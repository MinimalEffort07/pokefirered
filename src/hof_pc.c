/**
 * @file hof_pc.c
 * @brief Hall of Fame PC Interface — View Previous Championship Teams
 *
 * FILE OVERVIEW:
 * This file manages the transition into and out of the Hall of Fame PC screen.
 * In every Pokemon Center (and the player's house), there is a PC that can be
 * used to view the Hall of Fame records — the teams that the player used to
 * defeat the Elite Four and Champion. This file handles:
 *
 * - Fading out from the overworld to the Hall of Fame viewer
 * - Returning from the Hall of Fame viewer back to the PC menu
 * - Re-displaying the PC menu after viewing records
 *
 * GAME LOGIC:
 * The Hall of Fame PC option only appears in the PC menu if the player has
 * completed the game at least once (FLAG_SYS_GAME_CLEAR is set). Each time
 * the player beats the Champion, their team is recorded in flash save memory.
 * The Hall of Fame viewer displays these historical teams.
 */
#include "global.h"
#include "gflib.h"
#include "hall_of_fame.h"
#include "overworld.h"
#include "script.h"
#include "script_menu.h"
#include "task.h"

static void ReshowPCMenuAfterHallOfFamePC(void);
static void Task_WaitForPaletteFade(u8 taskId);

/**
 * FUNCTION: Task_WaitFadeAndSetCallback
 *
 * PURPOSE: Waits for the screen fade to complete, then cleans up overworld
 * resources and transitions to the Hall of Fame PC viewer screen.
 *
 * @param taskId — task identifier
 */
static void Task_WaitFadeAndSetCallback(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        FreeAllWindowBuffers();
        ResetBgsAndClearDma3BusyFlags(0);
        DestroyTask(taskId);
        SetMainCallback2(CB2_InitHofPC);  /* Switch to Hall of Fame viewer */
    }
}

/**
 * FUNCTION: HallOfFamePCBeginFade
 *
 * PURPOSE: Starts the transition from the PC menu to the Hall of Fame viewer.
 * Fades the screen to black, locks player controls, and creates the
 * transition task.
 */
void HallOfFamePCBeginFade(void)
{
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
    LockPlayerFieldControls();
    CreateTask(Task_WaitFadeAndSetCallback, 0);
}

/**
 * FUNCTION: ReturnFromHallOfFamePC
 *
 * PURPOSE: Called when the player exits the Hall of Fame viewer. Sets the main
 * callback to return to the overworld and configures the field callback to
 * re-show the PC menu.
 */
void ReturnFromHallOfFamePC(void)
{
    SetMainCallback2(CB2_ReturnToField);
    gFieldCallback = ReshowPCMenuAfterHallOfFamePC;
}

/**
 * FUNCTION: ReshowPCMenuAfterHallOfFamePC
 *
 * PURPOSE: Field callback that restores the PC menu after returning from the
 * Hall of Fame viewer. Locks controls, starts map music, rebuilds the PC menu,
 * and fades back in from black.
 */
static void ReshowPCMenuAfterHallOfFamePC(void)
{
    LockPlayerFieldControls();
    Overworld_PlaySpecialMapMusic();
    CreatePCMenu();
    ScriptMenu_DisplayPCStartupPrompt();
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0x10, 0, RGB_BLACK);
    CreateTask(Task_WaitForPaletteFade, 10);
}

/**
 * FUNCTION: Task_WaitForPaletteFade
 *
 * PURPOSE: Simple utility task that waits for a palette fade to complete
 * and then destroys itself. Used as a "wait then unlock" mechanism.
 */
static void Task_WaitForPaletteFade(u8 taskId)
{
    if (!gPaletteFade.active)
        DestroyTask(taskId);
}
