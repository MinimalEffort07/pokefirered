/**
 * @file fldeff_sweetscent.c
 * @brief Sweet Scent Field Effect — Force a Wild Encounter
 *
 * FILE OVERVIEW:
 * Implements the field effect for the move Sweet Scent. When used outside of battle,
 * Sweet Scent forces a wild Pokemon encounter if the player is standing in an area
 * where wild Pokemon can appear (grass, caves, water). If no wild Pokemon are
 * available, a message says "It's no use in this area."
 *
 * VISUAL EFFECT:
 * The screen flashes red (palette fade toward RGB(31,0,0)) while the Sweet Scent
 * sound effect plays. The player's sprite is excluded from the red tint so they
 * remain normally colored. After 64 frames, the game attempts the encounter.
 *
 * GBA CONTEXT — PALETTE FADE TRICK:
 * The red flash effect is done by temporarily swapping the unfaded palette with
 * the faded palette, then fading toward red. This is a clever trick:
 * 1. The game copies gPlttBufferUnfaded (true colors) to a backup
 * 2. Copies gPlttBufferFaded (current visual) into gPlttBufferUnfaded
 * 3. Starts a fade from 0 to 8 intensity toward RGB(31,0,0)
 * 4. The player's palette is excluded via a bitmask (~(1 << palette))
 * After the effect, the original unfaded palette is restored from the backup.
 */
#include "global.h"
#include "gflib.h"
#include "field_player_avatar.h"
#include "field_effect.h"
#include "party_menu.h"
#include "script.h"
#include "fldeff.h"
#include "event_scripts.h"
#include "field_weather.h"
#include "wild_encounter.h"
#include "constants/songs.h"

/* Backup of the original unfaded palette, stored during the red flash effect */
static EWRAM_DATA u8 *sPlttBufferBak = NULL;

static void FieldCallback_SweetScent(void);
static void StartSweetScentFieldEffect(void);
static void TrySweetScentEncounter(u8 taskId);
static void FailSweetScentEncounter(u8 taskId);

/**
 * FUNCTION: Unused_StartSweetscentFldeff
 *
 * PURPOSE: (Unused) Would start Sweet Scent from a script context rather than
 * the party menu. Uses slot 0 (first Pokemon) as the default.
 */
static void Unused_StartSweetscentFldeff(void)
{
	gPartyMenu.slotId = 0;
	FieldCallback_SweetScent();
}

/**
 * FUNCTION: SetUpFieldMove_SweetScent
 *
 * PURPOSE: Sets up Sweet Scent to be used after the party menu closes.
 * Unlike other HM field effects, Sweet Scent always succeeds — there are
 * no preconditions to check (the encounter attempt happens later).
 *
 * @return Always TRUE (Sweet Scent can always be used anywhere)
 */
bool8 SetUpFieldMove_SweetScent(void)
{
    gFieldCallback2 = FieldCallback_PrepareFadeInFromMenu;
    gPostMenuFieldCallback = FieldCallback_SweetScent;
    return TRUE;
}

/**
 * FUNCTION: FieldCallback_SweetScent
 *
 * PURPOSE: Post-menu callback that starts the Sweet Scent field effect animation.
 */
static void FieldCallback_SweetScent(void)
{
    FieldEffectStart(FLDEFF_SWEET_SCENT);
    gFieldEffectArguments[0] = GetCursorSelectionMonId();
}

/**
 * FUNCTION: FldEff_SweetScent
 *
 * PURPOSE: Creates the Pokemon show animation and prepares the weather for
 * the red screen flash effect.
 *
 * @return FALSE (field effects handle their own cleanup)
 */
bool8 FldEff_SweetScent(void)
{
    u8 taskId;

    SetWeatherScreenFadeOut();  /* Prepare weather system for the palette manipulation */
    taskId = CreateFieldEffectShowMon();
    FLDEFF_SET_FUNC_TO_DATA(StartSweetScentFieldEffect);
    return FALSE;
}

/**
 * FUNCTION: StartSweetScentFieldEffect
 *
 * PURPOSE: Begins the red screen flash effect and creates the encounter attempt task.
 *
 * HOW IT WORKS:
 * 1. Play the Sweet Scent sound effect
 * 2. Allocate a backup buffer and save the original unfaded palette
 * 3. Copy the current faded palette into the unfaded buffer (so the fade starts
 *    from the current visual state, not the "true" colors)
 * 4. Start a palette fade toward red, EXCLUDING the player's sprite palette
 *    (so the player doesn't turn red — only the environment does)
 * 5. Create a task to wait for the red flash, then attempt the encounter
 *
 * GBA CONTEXT:
 * The palette exclusion bitmask ~(1 << (paletteNum + 16)) works because:
 * - Sprite palettes are indices 16-31 in the combined BG+OBJ palette
 * - The player's OAM palette number tells us which sprite palette slot they use
 * - The ~ inverts the mask: "fade everything EXCEPT this palette"
 */
static void StartSweetScentFieldEffect(void)
{
    u8 taskId;

    PlaySE(SE_M_SWEET_SCENT);
    /* Save original unfaded palette for later restoration */
    sPlttBufferBak = (u8 *)Alloc(PLTT_SIZE);
    CpuFastCopy(gPlttBufferUnfaded, sPlttBufferBak, PLTT_SIZE);
    /* Swap faded into unfaded so the fade starts from current visual state */
    CpuFastCopy(gPlttBufferFaded, gPlttBufferUnfaded, PLTT_SIZE);
    /* Fade all palettes except the player's toward red, intensity 0->8, over 4 frames/step */
    BeginNormalPaletteFade(~(1 << (gSprites[GetPlayerAvatarObjectId()].oam.paletteNum + 16)), 4, 0, 8, RGB(31, 0, 0));
    taskId = CreateTask(TrySweetScentEncounter, 0);
    gTasks[taskId].data[0] = 0;
    FieldEffectActiveListRemove(FLDEFF_SWEET_SCENT);
}

/**
 * FUNCTION: TrySweetScentEncounter
 *
 * PURPOSE: Waits 64 frames after the red flash, then attempts a wild encounter.
 * If SweetScentWildEncounter() finds a Pokemon, the battle starts. If not,
 * the red tint fades back and a "no effect" message is shown.
 *
 * GAME LOGIC:
 * The 64-frame wait gives the player time to see the red flash effect before
 * the battle screen transition begins (or the failure message appears).
 *
 * @param taskId — task identifier
 */
static void TrySweetScentEncounter(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    if (!gPaletteFade.active)
    {
        if (data[0] == 64)
        {
            data[0] = 0;
            if (SweetScentWildEncounter() == TRUE)
            {
                /* Encounter found — battle will start, clean up backup */
                Free(sPlttBufferBak);
                DestroyTask(taskId);
            }
            else
            {
                /* No encounter available — fade the red back out */
                gTasks[taskId].func = FailSweetScentEncounter;
                BeginNormalPaletteFade(~(1 << (gSprites[GetPlayerAvatarObjectId()].oam.paletteNum + 16)), 4, 8, 0, RGB(31, 0, 0));
            }
        }
        else
        {
            data[0]++;
        }
    }
}

/**
 * FUNCTION: FailSweetScentEncounter
 *
 * PURPOSE: Handles the case where Sweet Scent found no wild Pokemon.
 * Waits for the red fade-out to complete, restores the original palette,
 * resumes weather processing, and runs the failure script ("It's no use").
 *
 * @param taskId — task identifier
 */
static void FailSweetScentEncounter(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        /* Restore the original unfaded palette from the backup */
        CpuFastCopy(sPlttBufferBak, gPlttBufferUnfaded, PLTT_SIZE);
        WeatherProcessingIdle();  /* Resume weather palette processing */
        Free(sPlttBufferBak);
        ScriptContext_SetupScript(EventScript_FailSweetScent);
        DestroyTask(taskId);
    }
}
