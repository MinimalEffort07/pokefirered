/**
 * @file save_menu_util.c
 * @brief Save Menu Statistics Formatter
 *
 * FILE OVERVIEW:
 * This file provides a utility function for formatting the player's game statistics
 * that appear on the save confirmation screen. When the player chooses to save,
 * a summary panel shows their name, Pokedex count, play time, current location,
 * and badge count. This function formats each of those statistics as a colored
 * text string for display.
 *
 * TEXT RENDERING:
 * The GBA Pokemon engine uses a custom text system with inline control codes for
 * formatting. EXT_CTRL_CODE_BEGIN signals the start of a control sequence,
 * followed by a code type (COLOR, SHADOW, etc.) and the value. This allows
 * different parts of the same string to be rendered in different colors.
 */
#include "global.h"
#include "gflib.h"
#include "event_data.h"
#include "pokedex.h"
#include "region_map.h"
#include "save_menu_util.h"

/**
 * FUNCTION: SaveStatToString
 *
 * PURPOSE: Converts a game statistic (name, Pokedex count, time, location, or badges)
 *          into a formatted, colored text string for the save menu display.
 *
 * HOW IT WORKS:
 * 1. Writes color control codes at the start of the destination buffer to set
 *    the text color and its shadow color (shadow = color + 1)
 * 2. Based on gameStatId, formats the appropriate statistic:
 *    - SAVE_STAT_NAME: Copies the player's name
 *    - SAVE_STAT_POKEDEX: Formats the Pokedex seen/caught count (National or Kanto)
 *    - SAVE_STAT_TIME: Formats play time as "HHH:MM" (left-aligned hours)
 *    - SAVE_STAT_TIME_HR_RT_ALIGN: Same but with right-aligned hours
 *    - SAVE_STAT_LOCATION: Gets the current map's region map name
 *    - SAVE_STAT_BADGES: Counts earned badges and formats as "N" + Japanese counter suffix
 *
 * GBA CONTEXT:
 * The text control code system:
 * - EXT_CTRL_CODE_BEGIN (0xFC): Signals "the next byte is a control code type"
 * - EXT_CTRL_CODE_COLOR: Sets the foreground text color
 * - EXT_CTRL_CODE_SHADOW: Sets the shadow/outline color
 * Colors are palette indices — the shadow is typically color+1 (a darker shade).
 *
 * @param gameStatId — Which statistic to format (SAVE_STAT_NAME, SAVE_STAT_POKEDEX, etc.)
 * @param dest0 — Destination buffer to write the formatted string into
 * @param color — Palette color index for the text
 */
void SaveStatToString(u8 gameStatId, u8 *dest0, u8 color)
{
    int nBadges;
    int flagId;

    u8 *dest = dest0;

    /* Write text color control codes: set foreground color and shadow color */
    *dest++ = EXT_CTRL_CODE_BEGIN;     /* Start of control sequence */
    *dest++ = EXT_CTRL_CODE_COLOR;     /* "Set text color" command */
    *dest++ = color;                   /* The color palette index */
    *dest++ = EXT_CTRL_CODE_BEGIN;     /* Start of another control sequence */
    *dest++ = EXT_CTRL_CODE_SHADOW;    /* "Set shadow color" command */
    *dest++ = color + 1;              /* Shadow is typically one shade darker */

    switch (gameStatId)
    {
    case SAVE_STAT_NAME:
        /* Simply copy the player's name from save data */
        dest = StringCopy(dest, gSaveBlock2Ptr->playerName);
        break;
    case SAVE_STAT_POKEDEX:
        /* Show caught count — use National Dex count if unlocked, otherwise Kanto only */
        if (IsNationalPokedexEnabled())
            dest = ConvertIntToDecimalStringN(dest, GetNationalPokedexCount(1), STR_CONV_MODE_LEFT_ALIGN, 3);
        else
            dest = ConvertIntToDecimalStringN(dest, GetKantoPokedexCount(1), STR_CONV_MODE_LEFT_ALIGN, 3);
        break;
    case SAVE_STAT_TIME:
        /* Format play time as "HHH:MM" with left-aligned hours (e.g., "12:05") */
        dest = ConvertIntToDecimalStringN(dest, gSaveBlock2Ptr->playTimeHours, STR_CONV_MODE_LEFT_ALIGN, 3);
        *dest++ = CHAR_COLON;  /* Colon separator between hours and minutes */
        dest = ConvertIntToDecimalStringN(dest, gSaveBlock2Ptr->playTimeMinutes, STR_CONV_MODE_LEADING_ZEROS, 2);
        break;
    case SAVE_STAT_TIME_HR_RT_ALIGN:
        /* Same as above but right-aligned hours (e.g., " 12:05") */
        dest = ConvertIntToDecimalStringN(dest, gSaveBlock2Ptr->playTimeHours, STR_CONV_MODE_RIGHT_ALIGN, 3);
        *dest++ = CHAR_COLON;
        dest = ConvertIntToDecimalStringN(dest, gSaveBlock2Ptr->playTimeMinutes, STR_CONV_MODE_LEADING_ZEROS, 2);
        break;
    case SAVE_STAT_LOCATION:
        /* Get the name of the current area from the region map */
        GetMapNameGeneric(dest, gMapHeader.regionMapSectionId);
        break;
    case SAVE_STAT_BADGES:
        /* Count how many of the 8 gym badges the player has earned.
         * FLAGS FLAG_BADGE01_GET through FLAG_BADGE08_GET are consecutive flag IDs. */
        for (flagId = FLAG_BADGE01_GET, nBadges = 0; flagId < FLAG_BADGE01_GET + 8; flagId++)
        {
            if (FlagGet(flagId))
                nBadges++;
        }
        *dest++ = nBadges + CHAR_0;  /* Convert digit to character (CHAR_0 + n = character 'n') */
        *dest++ = 10; // 'こ' — Japanese counter suffix for small objects
        *dest++ = EOS;               /* End-of-string terminator */
        break;
    }
}
