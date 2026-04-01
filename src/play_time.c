/*
 * play_time.c - Play Time Counter
 *
 * ============================================================================
 * HOW PLAY TIME IS TRACKED
 * ============================================================================
 *
 * The GBA has no real-time clock (well, some cartridges do, but not
 * Pokemon FireRed). Play time is tracked by counting VBlank interrupts.
 *
 * VBlank occurs approximately 59.73 times per second (the GBA's exact
 * refresh rate). Each VBlank, PlayTimeCounter_Update() is called from
 * the main game loop (AgbMain). It increments a "VBlanks" counter.
 * When VBlanks reaches 60 (approximately 1 second), it rolls over to
 * seconds. Seconds roll to minutes, minutes roll to hours.
 *
 * Note: Using 60 VBlanks per second (not 59.73) means the play time
 * runs slightly fast - about 0.45% fast, or roughly 26 seconds per hour.
 * This is a deliberate simplification for clean code.
 *
 * Play time is stored in the save block (gSaveBlock2Ptr) so it persists
 * across save/load cycles. The maximum play time is 999:59:59, after
 * which the counter stops (MAXED_OUT state).
 *
 * The counter can be in three states:
 *   STOPPED:   Counter does not advance (during battles, cutscenes, etc.)
 *   RUNNING:   Counter advances each VBlank (normal gameplay)
 *   MAXED_OUT: Counter has reached 999:59:59 and stopped permanently
 *
 * ============================================================================
 */

#include "play_time.h"

/* Internal state of the play time counter */
static u8 sPlayTimeCounterState;

enum
{
    STOPPED,     /* Counter is paused */
    RUNNING,     /* Counter is actively counting */
    MAXED_OUT,   /* Counter reached maximum and stopped */
};

/**
 * FUNCTION: PlayTimeCounter_Reset
 *
 * PURPOSE: Reset the play time to 0:00:00 and stop the counter.
 *
 * GAME LOGIC:
 * Called when starting a new game. Zeroes all time fields in the save block.
 */
void PlayTimeCounter_Reset(void)
{
    sPlayTimeCounterState = STOPPED;
    gSaveBlock2Ptr->playTimeHours = 0;
    gSaveBlock2Ptr->playTimeMinutes = 0;
    gSaveBlock2Ptr->playTimeSeconds = 0;
    gSaveBlock2Ptr->playTimeVBlanks = 0;
}

/**
 * FUNCTION: PlayTimeCounter_Start
 *
 * PURPOSE: Start (or resume) counting play time.
 *
 * GAME LOGIC:
 * Called when the player enters the overworld or resumes gameplay.
 * If the loaded save already has >999 hours, immediately maxes out
 * to prevent overflow.
 */
void PlayTimeCounter_Start(void)
{
    sPlayTimeCounterState = RUNNING;
    if (gSaveBlock2Ptr->playTimeHours > 999)
        PlayTimeCounter_SetToMax();
}

/**
 * FUNCTION: PlayTimeCounter_Stop
 *
 * PURPOSE: Pause the play time counter.
 *
 * GAME LOGIC:
 * Called during title screen, menus, link communications, etc.
 * where the player isn't actively playing.
 */
void PlayTimeCounter_Stop(void)
{
    sPlayTimeCounterState = STOPPED;
}

/**
 * FUNCTION: PlayTimeCounter_Update
 *
 * PURPOSE: Advance the play time by one VBlank frame (~1/60 second).
 *
 * HOW IT WORKS:
 * Called once per frame from the main game loop (AgbMain).
 * Increments VBlanks. When VBlanks hits 60, it rolls over and
 * increments seconds. Seconds roll at 60 to minutes, minutes
 * roll at 60 to hours. Hours cap at 999.
 *
 * The cascading if-statements handle the rollover chain:
 *   VBlanks 0-59 -> Seconds 0-59 -> Minutes 0-59 -> Hours 0-999
 *
 * GBA CONTEXT:
 * The VBlank rate is ~59.73 Hz, but this code uses 60 as the rollover.
 * This means 1 "second" of play time is actually ~1.0045 real seconds.
 * Over 100 hours of play, the clock would be ~27 minutes fast.
 * This inaccuracy is acceptable for a play time display.
 */
void PlayTimeCounter_Update(void)
{
    if (sPlayTimeCounterState == RUNNING)
    {
        gSaveBlock2Ptr->playTimeVBlanks++;
        if (gSaveBlock2Ptr->playTimeVBlanks > 59)     /* 60 VBlanks = ~1 second */
        {
            gSaveBlock2Ptr->playTimeVBlanks = 0;
            gSaveBlock2Ptr->playTimeSeconds++;
            if (gSaveBlock2Ptr->playTimeSeconds > 59)  /* 60 seconds = 1 minute */
            {
                gSaveBlock2Ptr->playTimeSeconds = 0;
                gSaveBlock2Ptr->playTimeMinutes++;
                if (gSaveBlock2Ptr->playTimeMinutes > 59)  /* 60 minutes = 1 hour */
                {
                    gSaveBlock2Ptr->playTimeMinutes = 0;
                    gSaveBlock2Ptr->playTimeHours++;
                    if (gSaveBlock2Ptr->playTimeHours > 999)  /* Max at 999 hours */
                        PlayTimeCounter_SetToMax();
                }
            }
        }
    }
}

/**
 * FUNCTION: PlayTimeCounter_SetToMax
 *
 * PURPOSE: Set the play time to 999:59:59 and stop counting permanently.
 *
 * GAME LOGIC:
 * The trainer card displays play time with 3 digits for hours.
 * 999:59:59 is the maximum displayable value. Once reached,
 * the counter enters MAXED_OUT state and never advances again.
 */
void PlayTimeCounter_SetToMax(void)
{
    sPlayTimeCounterState = MAXED_OUT;
    gSaveBlock2Ptr->playTimeHours = 999;
    gSaveBlock2Ptr->playTimeMinutes = 59;
    gSaveBlock2Ptr->playTimeSeconds = 59;
    gSaveBlock2Ptr->playTimeVBlanks = 59;
}
