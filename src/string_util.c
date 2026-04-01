/**
 * string_util.c - Custom String Handling for the GBA's Proprietary Text Encoding
 *
 * ============================================================================
 * GBA POKEMON TEXT ENCODING OVERVIEW
 * ============================================================================
 *
 * Pokemon games do NOT use ASCII or UTF-8. They use a completely custom 8-bit
 * character encoding where each byte maps to a different character than you'd
 * expect in standard C:
 *
 *   0xFF = EOS (End Of String) -- NOT '\0' like in standard C!
 *   0xFC = EXT_CTRL_CODE_BEGIN -- Start of an extended control code sequence
 *          (changes text color, font, speed, etc.)
 *   0xFD = PLACEHOLDER_BEGIN -- Start of a placeholder (like {PLAYER_NAME})
 *   0xFE = Line break (newline)
 *   0xF9 = Multi-byte character prefix (the next byte is part of this character)
 *   0xFA, 0xFB = Additional control characters
 *
 * Because of this custom encoding, standard C string functions (strlen, strcmp,
 * strcpy) CANNOT be used -- they all rely on '\0' (0x00) as the terminator,
 * but 0x00 is a valid character in Pokemon's encoding (it might be a space
 * or other glyph). This file provides replacement string functions that use
 * 0xFF (EOS) as the string terminator instead.
 *
 * EXTENDED CONTROL CODES (0xFC prefix):
 * These are inline commands embedded in text strings that control rendering:
 *   0xFC 0x01 [color]     = Set text foreground color
 *   0xFC 0x02 [color]     = Set text highlight/background color
 *   0xFC 0x03 [color]     = Set text shadow color
 *   0xFC 0x04 [fg][bg][shadow] = Set all three colors at once
 *   0xFC 0x06 [fontId]    = Change font
 *   0xFC 0x09 [speed]     = Pause for N frames
 *   0xFC 0x15              = Enable Japanese text mode
 *   0xFC 0x16              = Disable Japanese text mode
 *
 * PLACEHOLDERS (0xFD prefix):
 * These are substitution markers replaced at runtime with dynamic text:
 *   0xFD 0x01 = Player's name
 *   0xFD 0x02 = String variable 1 (gStringVar1)
 *   0xFD 0x03 = String variable 2 (gStringVar2)
 *   0xFD 0x04 = String variable 3 (gStringVar3)
 *   0xFD 0x06 = Rival's name
 *   etc.
 *
 * ============================================================================
 */

#include "global.h"
#include "gflib.h"

/*
 * Global string variable buffers -- these are used by the game's scripting
 * system to store dynamic text that gets substituted into dialog strings.
 * Scripts write values like Pokemon names, item names, or numbers into these
 * buffers, then reference them via placeholders in text strings.
 *
 * gStringVar1-3: General-purpose text buffers used by game scripts (e.g.,
 *   "You received {STR_VAR_1}!" where STR_VAR_1 = "POTION")
 * gStringVar4: Large buffer (1000 bytes) used for fully expanded/formatted
 *   strings that are ready to be rendered to screen.
 * gUnknownStringVar: Small buffer for miscellaneous placeholder expansion.
 */
EWRAM_DATA u8 gStringVar1[32] = {};
EWRAM_DATA u8 gStringVar2[20] = {};
EWRAM_DATA u8 gStringVar3[20] = {};
EWRAM_DATA u8 gStringVar4[1000] = {};
EWRAM_DATA u8 gUnknownStringVar[16] = {0};

/*
 * sDigits: Lookup table mapping digit values (0-15) to their character codes
 * in Pokemon's custom encoding. The __() macro converts ASCII text to the
 * game's proprietary character encoding at compile time.
 * Used by both decimal and hexadecimal number-to-string conversion.
 */
static const u8 sDigits[] = __("0123456789ABCDEF");

/*
 * sPowersOfTen: Precomputed powers of 10 for fast integer-to-string conversion.
 * Rather than computing 10^n with multiplication in a loop, we just look up
 * the value. Index 0 = 10^0 = 1, index 9 = 10^9 = 1,000,000,000.
 * This is a common embedded optimization to avoid expensive division/multiplication.
 */
static const s32 sPowersOfTen[] =
{
             1,
            10,
           100,
          1000,
         10000,
        100000,
       1000000,
      10000000,
     100000000,
    1000000000,
};

/*
 * External placeholder text strings -- these are defined in data files and
 * contain the game-specific names that get substituted into dialog text.
 * The version-specific names (Ruby/Sapphire, Magma/Aqua, etc.) are swapped
 * between FireRed and LeafGreen builds using preprocessor conditionals.
 */
extern u8 gExpandedPlaceholder_Empty[];
extern u8 gExpandedPlaceholder_Kun[];
extern u8 gExpandedPlaceholder_Chan[];
extern u8 gExpandedPlaceholder_Sapphire[];
extern u8 gExpandedPlaceholder_Ruby[];
extern u8 gExpandedPlaceholder_Aqua[];
extern u8 gExpandedPlaceholder_Magma[];
extern u8 gExpandedPlaceholder_Archie[];
extern u8 gExpandedPlaceholder_Maxie[];
extern u8 gExpandedPlaceholder_Kyogre[];
extern u8 gExpandedPlaceholder_Groudon[];
extern u8 gExpandedPlaceholder_Red[];
extern u8 gExpandedPlaceholder_Green[];

/**
 * FUNCTION: StringCopy_Nickname
 *
 * PURPOSE: Copy a Pokemon nickname string, enforcing the maximum nickname length.
 *
 * HOW IT WORKS:
 * Copies up to POKEMON_NAME_LENGTH characters from src to dest. Stops early
 * if it encounters the EOS (0xFF) terminator. Always ensures the destination
 * is EOS-terminated, even if the source nickname was at max length.
 *
 * GAME LOGIC:
 * Pokemon nicknames have a fixed maximum length (10 characters in English).
 * This function prevents buffer overflows when copying user-entered names.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer (must be at least POKEMON_NAME_LENGTH + 1 bytes)
 * @param src  -- Source nickname string
 *
 * RETURNS: Pointer to the EOS terminator in the destination string
 */
u8 *StringCopy_Nickname(u8 *dest, const u8 *src)
{
    u8 i;
    u32 limit = POKEMON_NAME_LENGTH;

    for (i = 0; i < limit; i++)
    {
        dest[i] = src[i];

        if (dest[i] == EOS)
            return &dest[i];
    }

    /* If we copied max characters without hitting EOS, add one */
    dest[i] = EOS;
    return &dest[i];
}

/**
 * FUNCTION: StringGet_Nickname
 *
 * PURPOSE: Find the end of a nickname string, enforcing the maximum length.
 *
 * HOW IT WORKS:
 * Scans up to POKEMON_NAME_LENGTH characters looking for EOS. If found,
 * returns a pointer to it. If the string is longer than the max, forces
 * an EOS at the max length position and returns that pointer.
 *
 * This is essentially a "find or create the terminator" function.
 *
 * PARAMETERS:
 * @param str -- The nickname string to scan
 *
 * RETURNS: Pointer to the EOS terminator (existing or newly placed)
 */
u8 *StringGet_Nickname(u8 *str)
{
    u8 i;
    u32 limit = POKEMON_NAME_LENGTH;

    for (i = 0; i < limit; i++)
        if (str[i] == EOS)
            return &str[i];

    str[i] = EOS;
    return &str[i];
}

/**
 * FUNCTION: StringCopy_PlayerName
 *
 * PURPOSE: Copy a player name string, enforcing the maximum player name length.
 *
 * HOW IT WORKS:
 * Same as StringCopy_Nickname but uses PLAYER_NAME_LENGTH as the limit instead.
 * Player names have a different maximum length than Pokemon nicknames.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer
 * @param src  -- Source player name string
 *
 * RETURNS: Pointer to the EOS terminator in the destination
 */
u8 *StringCopy_PlayerName(u8 *dest, const u8 *src)
{
    s32 i;
    s32 limit = PLAYER_NAME_LENGTH;

    for (i = 0; i < limit; i++)
    {
        dest[i] = src[i];

        if (dest[i] == EOS)
            return &dest[i];
    }

    dest[i] = EOS;
    return &dest[i];
}

/**
 * FUNCTION: StringCopy
 *
 * PURPOSE: Copy a string using Pokemon's custom EOS (0xFF) terminator.
 *          This is the Pokemon equivalent of strcpy().
 *
 * HOW IT WORKS:
 * Copies bytes from src to dest until it encounters the EOS byte (0xFF),
 * then writes the EOS terminator to dest and returns a pointer past it.
 *
 * Unlike strcpy, this terminates on 0xFF instead of 0x00, and returns a
 * pointer to the EOS position (allowing easy string concatenation).
 *
 * PARAMETERS:
 * @param dest -- Destination buffer (must be large enough)
 * @param src  -- Source string (EOS-terminated)
 *
 * RETURNS: Pointer to the EOS terminator in the destination
 */
u8 *StringCopy(u8 *dest, const u8 *src)
{
    while (*src != EOS)
    {
        *dest = *src;
        dest++;
        src++;
    }

    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: StringAppend
 *
 * PURPOSE: Append a string to the end of another. Pokemon equivalent of strcat().
 *
 * HOW IT WORKS:
 * Finds the EOS terminator in dest, then copies src starting at that position.
 * Returns a pointer to the new EOS terminator.
 *
 * PARAMETERS:
 * @param dest -- Destination string to append to
 * @param src  -- Source string to append
 *
 * RETURNS: Pointer to the EOS terminator after the appended text
 */
u8 *StringAppend(u8 *dest, const u8 *src)
{
    while (*dest != EOS)
        dest++;

    return StringCopy(dest, src);
}

/**
 * FUNCTION: StringCopyN
 *
 * PURPOSE: Copy exactly N bytes from src to dest (no EOS handling).
 *          Pokemon equivalent of memcpy for string data.
 *
 * HOW IT WORKS:
 * Copies exactly n bytes regardless of EOS terminators. Does NOT add an
 * EOS at the end. Used when you know the exact length or want to copy
 * partial strings / raw byte data.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer
 * @param src  -- Source data
 * @param n    -- Number of bytes to copy
 *
 * RETURNS: Pointer to the byte immediately after the last copied byte in dest
 */
u8 *StringCopyN(u8 *dest, const u8 *src, u8 n)
{
    u16 i;

    for (i = 0; i < n; i++)
        dest[i] = src[i];

    return &dest[n];
}

/**
 * FUNCTION: StringAppendN
 *
 * PURPOSE: Append exactly N bytes from src to the end of dest.
 *
 * HOW IT WORKS:
 * Finds the EOS in dest, then copies exactly n bytes from src at that position.
 * Does NOT add an EOS terminator after the copied bytes.
 *
 * PARAMETERS:
 * @param dest -- Destination string to append to
 * @param src  -- Source data
 * @param n    -- Number of bytes to copy
 *
 * RETURNS: Pointer past the last appended byte
 */
u8 *StringAppendN(u8 *dest, const u8 *src, u8 n)
{
    while (*dest != EOS)
        dest++;

    return StringCopyN(dest, src, n);
}

/**
 * FUNCTION: StringLength
 *
 * PURPOSE: Get the length of a Pokemon-encoded string. Equivalent of strlen().
 *
 * HOW IT WORKS:
 * Counts bytes until hitting the EOS (0xFF) terminator. Does not count the
 * terminator itself.
 *
 * PARAMETERS:
 * @param str -- The string to measure
 *
 * RETURNS: Number of bytes before the EOS terminator
 */
u16 StringLength(const u8 *str)
{
    u16 length = 0;

    while (str[length] != EOS)
        length++;

    return length;
}

/**
 * FUNCTION: StringCompare
 *
 * PURPOSE: Compare two Pokemon-encoded strings. Equivalent of strcmp().
 *
 * HOW IT WORKS:
 * Compares strings byte-by-byte. Returns 0 if equal, positive if str1 > str2,
 * negative if str1 < str2. Comparison ends at EOS or first difference.
 *
 * PARAMETERS:
 * @param str1 -- First string
 * @param str2 -- Second string
 *
 * RETURNS: 0 if equal, difference of first non-matching byte values otherwise
 */
s32 StringCompare(const u8 *str1, const u8 *str2)
{
    while (*str1 == *str2)
    {
        if (*str1 == EOS)
            return 0;
        str1++;
        str2++;
    }

    return *str1 - *str2;
}

/**
 * FUNCTION: StringCompareN
 *
 * PURPOSE: Compare up to N bytes of two strings. Equivalent of strncmp().
 *
 * PARAMETERS:
 * @param str1 -- First string
 * @param str2 -- Second string
 * @param n    -- Maximum number of bytes to compare
 *
 * RETURNS: 0 if equal (up to n bytes), difference otherwise
 */
s32 StringCompareN(const u8 *str1, const u8 *str2, u32 n)
{
    while (*str1 == *str2)
    {
        if (*str1 == EOS)
            return 0;
        str1++;
        str2++;
        if (--n == 0)
            return 0;
    }

    return *str1 - *str2;
}

/**
 * FUNCTION: ConvertIntToDecimalStringN
 *
 * PURPOSE: Convert an integer to a decimal string with formatting options.
 *
 * HOW IT WORKS:
 * Converts the integer 'value' into a string of at most 'n' decimal digits,
 * using the precomputed sPowersOfTen[] table for fast digit extraction.
 *
 * Three formatting modes are available:
 *   STR_CONV_MODE_LEFT_ALIGN:   "42" (no padding, leading zeros suppressed)
 *   STR_CONV_MODE_RIGHT_ALIGN:  "  42" (padded with spaces on the left)
 *   STR_CONV_MODE_LEADING_ZEROS: "0042" (padded with zeros on the left)
 *
 * The algorithm uses a state machine:
 *   WAITING_FOR_NONZERO_DIGIT: Haven't seen a non-zero digit yet (left-align skips)
 *   WRITING_DIGITS: We've started outputting digits
 *   WRITING_SPACES: Right-align mode outputs spaces until first non-zero digit
 *
 * For each power of 10, it divides to extract the digit, then subtracts to
 * get the remainder for the next iteration.
 *
 * GAME LOGIC:
 * Used everywhere the game displays numbers: HP, level, money, item counts,
 * Pokedex numbers, etc. The formatting modes ensure consistent alignment in
 * menus and stat displays.
 *
 * PARAMETERS:
 * @param dest  -- Destination buffer for the string
 * @param value -- Integer to convert
 * @param mode  -- STR_CONV_MODE_LEFT_ALIGN, RIGHT_ALIGN, or LEADING_ZEROS
 * @param n     -- Number of digit positions (1-10)
 *
 * RETURNS: Pointer to the EOS terminator after the number string
 */
u8 *ConvertIntToDecimalStringN(u8 *dest, s32 value, enum StringConvertMode mode, u8 n)
{
    enum { WAITING_FOR_NONZERO_DIGIT, WRITING_DIGITS, WRITING_SPACES } state;
    s32 powerOfTen;
    s32 largestPowerOfTen = sPowersOfTen[n - 1];

    state = WAITING_FOR_NONZERO_DIGIT;

    if (mode == STR_CONV_MODE_RIGHT_ALIGN)
        state = WRITING_SPACES;

    if (mode == STR_CONV_MODE_LEADING_ZEROS)
        state = WRITING_DIGITS;

    for (powerOfTen = largestPowerOfTen; powerOfTen > 0; powerOfTen /= 10)
    {
        u8 *out;
        u8 c;
        /* Extract the current digit by dividing by the current power of 10 */
        u16 digit = value / powerOfTen;
        /* Get the remainder for extracting subsequent digits */
        s32 temp = value - (powerOfTen * digit);

        if (state == WRITING_DIGITS)
        {
            out = dest++;

            if (digit <= 9)
                c = sDigits[digit];  /* Look up the character code for this digit */
            else
                c = CHAR_QUESTION_MARK;  /* Overflow protection -- shouldn't happen normally */

            *out = c;
        }
        else if (digit != 0 || powerOfTen == 1)
        {
            /*
             * Found the first non-zero digit (or this is the ones place,
             * which we always output). Switch to digit-writing mode.
             */
            state = WRITING_DIGITS;
            out = dest++;

            if (digit <= 9)
                c = sDigits[digit];
            else
                c = CHAR_QUESTION_MARK;

            *out = c;
        }
        else if (state == WRITING_SPACES)
        {
            /* Right-align mode: output a space for each leading zero position */
            *dest++ = CHAR_SPACE;
        }
        /* else: left-align mode, skip leading zeros entirely */

        value = temp;
    }

    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: ConvertIntToHexStringN
 *
 * PURPOSE: Convert an integer to a hexadecimal string with formatting.
 *
 * HOW IT WORKS:
 * Same algorithm as ConvertIntToDecimalStringN but uses powers of 16 instead
 * of powers of 10. Since there's no precomputed powers-of-16 table, it
 * calculates the largest power of 16 needed with a loop.
 *
 * Supports the same three formatting modes (left-align, right-align, leading zeros).
 * Digits A-F use uppercase letters.
 *
 * GAME LOGIC:
 * Used for debugging displays and certain technical information screens.
 *
 * PARAMETERS:
 * @param dest  -- Destination buffer
 * @param value -- Integer to convert
 * @param mode  -- Formatting mode
 * @param n     -- Number of hex digit positions (1-8)
 *
 * RETURNS: Pointer to the EOS terminator
 */
u8 *ConvertIntToHexStringN(u8 *dest, s32 value, enum StringConvertMode mode, u8 n)
{
    enum { WAITING_FOR_NONZERO_DIGIT, WRITING_DIGITS, WRITING_SPACES } state;
    u8 i;
    s32 powerOfSixteen;
    s32 largestPowerOfSixteen = 1;

    /* Calculate 16^(n-1) -- the largest power of 16 we'll divide by */
    for (i = 1; i < n; i++)
        largestPowerOfSixteen *= 16;

    state = WAITING_FOR_NONZERO_DIGIT;

    if (mode == STR_CONV_MODE_RIGHT_ALIGN)
        state = WRITING_SPACES;

    if (mode == STR_CONV_MODE_LEADING_ZEROS)
        state = WRITING_DIGITS;

    for (powerOfSixteen = largestPowerOfSixteen; powerOfSixteen > 0; powerOfSixteen /= 16)
    {
        u8 *out;
        u8 c;
        u32 digit = value / powerOfSixteen;
        s32 temp = value % powerOfSixteen;

        if (state == WRITING_DIGITS)
        {
            out = dest++;

            if (digit <= 0xF)
                c = sDigits[digit];  /* 0-9 maps to digit chars, A-F to letter chars */
            else
                c = CHAR_QUESTION_MARK;

            *out = c;
        }
        else if (digit != 0 || powerOfSixteen == 1)
        {
            state = WRITING_DIGITS;
            out = dest++;

            if (digit <= 0xF)
                c = sDigits[digit];
            else
                c = CHAR_QUESTION_MARK;

            *out = c;
        }
        else if (state == WRITING_SPACES)
        {
            *dest++ = CHAR_SPACE;
        }

        value = temp;
    }

    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: StringExpandPlaceholders
 *
 * PURPOSE: Process a template string, replacing placeholder markers with
 *          their actual text values.
 *
 * HOW IT WORKS:
 * Scans through the source string byte by byte, handling three types of content:
 *
 * 1. PLACEHOLDER_BEGIN (0xFD): The next byte is a placeholder ID. Looks up the
 *    corresponding text (e.g., player name, Pokemon name) and recursively expands
 *    it (in case the replacement itself contains placeholders).
 *
 * 2. EXT_CTRL_CODE_BEGIN (0xFC): An extended control code sequence. These are
 *    multi-byte sequences that change text appearance (color, font, etc.).
 *    The function copies them verbatim to the output, using GetExtCtrlCodeLength
 *    to determine how many parameter bytes to copy.
 *
 * 3. Regular characters: Copied directly to the output.
 *
 * GAME LOGIC:
 * This is the core text substitution engine. When the game needs to display
 * text like "Go, PIKACHU!", the source string stored in ROM is something like
 * "Go, {STR_VAR_1}!" where {STR_VAR_1} is a placeholder byte. The game first
 * writes "PIKACHU" into gStringVar1, then calls this function to produce the
 * final displayable string.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer for the expanded string
 * @param src  -- Source template string with placeholders
 *
 * RETURNS: Pointer to the EOS terminator in the output
 */
u8 *StringExpandPlaceholders(u8 *dest, const u8 *src)
{
    for (;;)
    {
        u8 c = *src++;
        u8 placeholderId;
        u8 *expandedString;

        switch (c)
        {
            case PLACEHOLDER_BEGIN:
                /* Next byte identifies which placeholder (player name, string var, etc.) */
                placeholderId = *src++;
                expandedString = GetExpandedPlaceholder(placeholderId);
                /* Recursively expand in case the replacement also contains placeholders */
                dest = StringExpandPlaceholders(dest, expandedString);
                break;
            case EXT_CTRL_CODE_BEGIN:
                /*
                 * Extended control code: copy the 0xFC marker and the command byte,
                 * then copy any parameter bytes based on the command's known length.
                 * The fall-through in the switch cases handles commands that take
                 * different numbers of parameters (0x04 takes 3 params, 0x0B takes 2, etc.)
                 */
                *dest++ = c;
                c = *src++;
                *dest++ = c;

                switch (c)
                {
                    case 0x07:  /* 0 additional parameter bytes */
                    case 0x09:
                    case 0x0F:
                    case 0x15:
                    case 0x16:
                    case 0x17:
                    case 0x18:
                        break;
                    case 0x04:  /* 3 parameter bytes (falls through twice + default) */
                        *dest++ = *src++;
                    case 0x0B:  /* 2 parameter bytes (falls through once + default) */
                        *dest++ = *src++;
                    default:    /* 1 parameter byte */
                        *dest++ = *src++;
                }
                break;
            case EOS:
                /* End of source string -- terminate output and return */
                *dest = EOS;
                return dest;
            case 0xFA:  /* Prompt/wait control characters -- copy verbatim */
            case 0xFB:
            case 0xFE:  /* Newline character -- copy verbatim */
            default:
                /* Regular character -- copy directly */
                *dest++ = c;
        }
    }
}

/**
 * FUNCTION: StringBraille
 *
 * PURPOSE: Convert a text string to Braille display format.
 *
 * HOW IT WORKS:
 * Braille text on the GBA is rendered using a special font (font ID 0x06).
 * Each Braille character occupies TWO tiles: the dot pattern tile (original
 * character) and a second tile at character + 0x40 (which holds the lower
 * portion of the Braille cell pattern).
 *
 * The function:
 * 1. Prepends a "set Braille font" control code (0xFC 0x06 0x06 = use font 6)
 * 2. For each character, outputs both the character and its companion tile
 * 3. Converts newline markers (0xFE) to a "goto line 2" control sequence
 *
 * GAME LOGIC:
 * In Pokemon FireRed/LeafGreen, Braille puzzles appear in certain locations
 * (inherited from Ruby/Sapphire). The player must decode Braille dot patterns
 * to solve puzzles.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer for the Braille-formatted string
 * @param src  -- Source text string to convert
 *
 * RETURNS: Pointer to the EOS terminator in the output
 */
u8 *StringBraille(u8 *dest, const u8 *src)
{
    /* Control code: 0xFC 0x06 0x06 = set font to font ID 6 (Braille font) */
    u8 setBrailleFont[] = { 0xFC, 0x06, 0x06, 0xFF };
    /*
     * Control sequence for line break in Braille:
     * 0xFE = newline, 0xFC 0x0E 0x02 = set Y position to line 2
     */
    u8 gotoLine2[] = { 0xFE, 0xFC, 0x0E, 0x02, 0xFF };

    /* Start with the "use Braille font" control code */
    dest = StringCopy(dest, setBrailleFont);

    for (;;)
    {
        u8 c = *src++;

        switch (c)
        {
            case EOS:
                *dest = c;
                return dest;
            case 0xFE:
                /* Newline: insert the full goto-line-2 sequence */
                dest = StringCopy(dest, gotoLine2);
                break;
            default:
                /*
                 * Each Braille character needs two tiles:
                 * The character itself and a companion tile 0x40 positions later.
                 * This creates the full 2-tile-wide Braille cell appearance.
                 */
                *dest++ = c;
                *dest++ = c + 0x40;
                break;
        }
    }
}

/**
 * FUNCTION: ExpandPlaceholder_UnknownStringVar
 *
 * PURPOSE: Return the unknown/miscellaneous string variable buffer.
 *
 * RETURNS: Pointer to gUnknownStringVar
 */
static u8 *ExpandPlaceholder_UnknownStringVar(void)
{
    return gUnknownStringVar;
}

/**
 * FUNCTION: ExpandPlaceholder_PlayerName
 *
 * PURPOSE: Return the player's name from the save data.
 *
 * GAME LOGIC:
 * Used when dialog text says things like "{PLAYER} received an item!"
 * Reads directly from the save block rather than a buffer, so it always
 * reflects the current player name.
 *
 * RETURNS: Pointer to the player's name in gSaveBlock2
 */
static u8 *ExpandPlaceholder_PlayerName(void)
{
    return gSaveBlock2Ptr->playerName;
}

/** Return the gStringVar1 buffer (general-purpose script variable 1) */
static u8 *ExpandPlaceholder_StringVar1(void)
{
    return gStringVar1;
}

/** Return the gStringVar2 buffer (general-purpose script variable 2) */
static u8 *ExpandPlaceholder_StringVar2(void)
{
    return gStringVar2;
}

/** Return the gStringVar3 buffer (general-purpose script variable 3) */
static u8 *ExpandPlaceholder_StringVar3(void)
{
    return gStringVar3;
}

/**
 * FUNCTION: ExpandPlaceholder_KunChan
 *
 * PURPOSE: Return the gender-appropriate Japanese honorific.
 *
 * GAME LOGIC:
 * In Japanese, "-kun" is used for males and "-chan" for females.
 * This placeholder allows dialog to address the player with the correct
 * honorific based on their chosen gender.
 *
 * RETURNS: Pointer to "kun" or "chan" string based on player gender
 */
static u8 *ExpandPlaceholder_KunChan(void)
{
    if (gSaveBlock2Ptr->playerGender == MALE)
        return gExpandedPlaceholder_Kun;
    else
        return gExpandedPlaceholder_Chan;
}

/**
 * FUNCTION: ExpandPlaceholder_RivalName
 *
 * PURPOSE: Return the rival's name.
 *
 * GAME LOGIC:
 * If the player hasn't named the rival yet (rivalName starts with EOS),
 * uses the default name: "Green" if the player is male (playing as Red),
 * "Red" if the player is female (playing as Leaf/Green).
 *
 * RETURNS: Pointer to the rival's name string
 */
static u8 *ExpandPlaceholder_RivalName(void)
{
    if (gSaveBlock1Ptr->rivalName[0] == EOS)
    {
        if (gSaveBlock2Ptr->playerGender == MALE)
            return gExpandedPlaceholder_Green;
        else
            return gExpandedPlaceholder_Red;
    }
    else
    {
        return gSaveBlock1Ptr->rivalName;
    }
}

/**
 * FUNCTION: ExpandPlaceholder_Version
 *
 * PURPOSE: Return the version-specific game name for cross-version references.
 *
 * GAME LOGIC:
 * FireRed references "Ruby" and LeafGreen references "Sapphire" when
 * talking about the corresponding GBA game in dialog. These are conditional
 * compilation switches -- the ROM is built as either FIRERED or LEAFGREEN.
 *
 * RETURNS: Pointer to the version name string
 */
static u8 *ExpandPlaceholder_Version(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Ruby;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Sapphire;
#endif
}

/*
 * The following placeholder functions provide version-swapped team/character
 * names. In FireRed, "Team Magma" is the primary reference and "Team Aqua"
 * is the secondary. In LeafGreen, they're reversed. This allows the same
 * dialog scripts to work in both versions with different faction names.
 */

/** Return "Team Magma" (FireRed) or "Team Aqua" (LeafGreen) */
static u8 *ExpandPlaceholder_Magma(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Magma;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Aqua;
#endif
}

/** Return "Team Aqua" (FireRed) or "Team Magma" (LeafGreen) */
static u8 *ExpandPlaceholder_Aqua(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Aqua;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Magma;
#endif
}

/** Return "Maxie" (FireRed) or "Archie" (LeafGreen) */
static u8 *ExpandPlaceholder_Maxie(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Maxie;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Archie;
#endif
}

/** Return "Archie" (FireRed) or "Maxie" (LeafGreen) */
static u8 *ExpandPlaceholder_Archie(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Archie;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Maxie;
#endif
}

/** Return "Groudon" (FireRed) or "Kyogre" (LeafGreen) */
static u8 *ExpandPlaceholder_Groudon(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Groudon;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Kyogre;
#endif
}

/** Return "Kyogre" (FireRed) or "Groudon" (LeafGreen) */
static u8 *ExpandPlaceholder_Kyogre(void)
{
#if defined(FIRERED)
    return gExpandedPlaceholder_Kyogre;
#elif defined(LEAFGREEN)
    return gExpandedPlaceholder_Groudon;
#endif
}

/**
 * FUNCTION: GetExpandedPlaceholder
 *
 * PURPOSE: Look up a placeholder by ID and return the corresponding text.
 *
 * HOW IT WORKS:
 * Uses a function pointer table to dispatch to the correct expansion function.
 * Each placeholder ID maps to a function that returns the appropriate string.
 * If the ID is out of range, returns an empty string.
 *
 * This pattern (array of function pointers indexed by ID) is a common
 * alternative to switch statements in embedded C. It's slightly faster
 * for large numbers of cases and uses less code space.
 *
 * PARAMETERS:
 * @param id -- Placeholder ID (PLACEHOLDER_ID_PLAYER, etc.)
 *
 * RETURNS: Pointer to the expanded text string
 */
u8 *GetExpandedPlaceholder(u32 id)
{
    typedef u8 *(*ExpandPlaceholderFunc)(void);

    static const ExpandPlaceholderFunc funcs[] =
    {
        [PLACEHOLDER_ID_UNKNOWN]      = ExpandPlaceholder_UnknownStringVar,
        [PLACEHOLDER_ID_PLAYER]       = ExpandPlaceholder_PlayerName,
        [PLACEHOLDER_ID_STRING_VAR_1] = ExpandPlaceholder_StringVar1,
        [PLACEHOLDER_ID_STRING_VAR_2] = ExpandPlaceholder_StringVar2,
        [PLACEHOLDER_ID_STRING_VAR_3] = ExpandPlaceholder_StringVar3,
        [PLACEHOLDER_ID_KUN]          = ExpandPlaceholder_KunChan,
        [PLACEHOLDER_ID_RIVAL]        = ExpandPlaceholder_RivalName,
        [PLACEHOLDER_ID_VERSION]      = ExpandPlaceholder_Version,
        [PLACEHOLDER_ID_MAGMA]        = ExpandPlaceholder_Magma,
        [PLACEHOLDER_ID_AQUA]         = ExpandPlaceholder_Aqua,
        [PLACEHOLDER_ID_MAXIE]        = ExpandPlaceholder_Maxie,
        [PLACEHOLDER_ID_ARCHIE]       = ExpandPlaceholder_Archie,
        [PLACEHOLDER_ID_GROUDON]      = ExpandPlaceholder_Groudon,
        [PLACEHOLDER_ID_KYOGRE]       = ExpandPlaceholder_Kyogre,
    };

    if (id >= NELEMS(funcs))
        return gExpandedPlaceholder_Empty;
    else
        return funcs[id]();
}

/**
 * FUNCTION: StringFill
 *
 * PURPOSE: Fill a buffer with N copies of a single character, then add EOS.
 *
 * HOW IT WORKS:
 * Writes the character 'c' exactly 'n' times, then terminates with EOS.
 * Used to create padding strings (e.g., strings of spaces for alignment).
 *
 * PARAMETERS:
 * @param dest -- Destination buffer
 * @param c    -- Character to fill with
 * @param n    -- Number of times to write the character
 *
 * RETURNS: Pointer to the EOS terminator
 */
u8 *StringFill(u8 *dest, u8 c, u16 n)
{
    u16 i;

    for (i = 0; i < n; i++)
        *dest++ = c;

    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: StringCopyPadded
 *
 * PURPOSE: Copy a string and pad the remainder with a fill character to a fixed width.
 *
 * HOW IT WORKS:
 * Copies characters from src to dest until EOS is reached, then fills the
 * remaining positions (up to width n) with the padding character 'c'.
 * This ensures the output is always exactly n characters wide.
 *
 * GAME LOGIC:
 * Used for fixed-width displays like menu items, stat labels, and name fields
 * where consistent alignment is needed. For example, Pokemon names in a list
 * are padded with spaces to all be the same width.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer (must be at least n+1 bytes)
 * @param src  -- Source string to copy
 * @param c    -- Padding character (usually CHAR_SPACE)
 * @param n    -- Total desired width (number of characters)
 *
 * RETURNS: Pointer to the EOS terminator
 */
u8 *StringCopyPadded(u8 *dest, const u8 *src, u8 c, u16 n)
{
    while (*src != EOS)
    {
        *dest++ = *src++;

        if (n)
            n--;
    }

    /* n is pre-decremented once more (accounts for the EOS position) */
    n--;

    /* Fill remaining positions with the padding character */
    while (n != (u16)-1)  /* Loop until n underflows past zero (unsigned wraparound) */
    {
        *dest++ = c;
        n--;
    }

    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: StringFillWithTerminator
 *
 * PURPOSE: Fill a buffer with N copies of the EOS terminator (0xFF).
 *
 * HOW IT WORKS:
 * Calls StringFill with EOS as the fill character. This completely clears
 * a string buffer by writing the terminator to every position.
 *
 * PARAMETERS:
 * @param dest -- Buffer to clear
 * @param n    -- Number of bytes to fill with EOS
 *
 * RETURNS: Pointer to the final EOS terminator
 */
u8 *StringFillWithTerminator(u8 *dest, u16 n)
{
    return StringFill(dest, EOS, n);
}

/**
 * FUNCTION: StringCopyN_Multibyte
 *
 * PURPOSE: Copy up to N characters from src to dest, handling multi-byte characters.
 *
 * HOW IT WORKS:
 * Similar to StringCopyN but aware of multi-byte character sequences.
 * In Pokemon's encoding, 0xF9 is a prefix byte indicating the next byte
 * is part of a two-byte character. When this prefix is encountered, both
 * bytes are copied together to avoid splitting a multi-byte character.
 *
 * The count 'n' represents the number of CHARACTERS (not bytes) to copy.
 *
 * PARAMETERS:
 * @param dest -- Destination buffer
 * @param src  -- Source string
 * @param n    -- Maximum number of characters to copy
 *
 * RETURNS: Pointer to the EOS terminator in the destination
 */
u8 *StringCopyN_Multibyte(u8 *dest, const u8 *src, u32 n)
{
    u32 i;

    for (i = n - 1; i != -1u; i--)  /* Count down from n-1 to 0 (unsigned) */
    {
        if (*src == EOS)
        {
            break;
        }
        else
        {
            *dest++ = *src++;
            /*
             * 0xF9 = multi-byte character prefix. The previous byte we just
             * copied was 0xF9, so the next byte is the second half of the
             * character -- copy it too (doesn't count as a separate character).
             */
            if (*(src - 1) == 0xF9)
                *dest++ = *src++;
        }
    }

    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: StringLength_Multibyte
 *
 * PURPOSE: Count the number of characters in a string, treating multi-byte
 *          sequences as single characters.
 *
 * HOW IT WORKS:
 * Scans the string, counting each character. When the multi-byte prefix
 * (0xF9) is found, skips the following byte so the two-byte sequence
 * counts as one character instead of two.
 *
 * PARAMETERS:
 * @param str -- The string to measure
 *
 * RETURNS: Number of characters (not bytes) before the EOS terminator
 */
u32 StringLength_Multibyte(const u8 *str)
{
    u32 length = 0;

    while (*str != EOS)
    {
        if (*str == 0xF9)
            str++;    /* Skip the second byte of a multi-byte character */
        str++;
        length++;
    }

    return length;
}

/**
 * FUNCTION: WriteColorChangeControlCode
 *
 * PURPOSE: Write an extended control code sequence that changes text color.
 *
 * HOW IT WORKS:
 * Generates a 3-byte control code: 0xFC [type] [color]
 * The type byte specifies which color component to change:
 *   colorType 0 -> control code 0x01 = foreground (text) color
 *   colorType 1 -> control code 0x03 = shadow color
 *   colorType 2 -> control code 0x02 = background/highlight color
 *
 * GAME LOGIC:
 * Used to dynamically change text colors within a string. For example,
 * making a Pokemon's name appear in a different color than the surrounding text.
 *
 * PARAMETERS:
 * @param dest      -- Destination buffer
 * @param colorType -- 0 = foreground, 1 = shadow, 2 = background
 * @param color     -- Color index (into the text palette)
 *
 * RETURNS: Pointer to the EOS terminator after the control code
 */
u8 *WriteColorChangeControlCode(u8 *dest, u32 colorType, u8 color)
{
    *dest = 0xFC;  /* EXT_CTRL_CODE_BEGIN marker */
    dest++;

    switch (colorType)
    {
    case 0:
        *dest = 1;   /* Control code for foreground color */
        dest++;
        break;
    case 1:
        *dest = 3;   /* Control code for shadow color */
        dest++;
        break;
    case 2:
        *dest = 2;   /* Control code for background color */
        dest++;
        break;
    }

    *dest = color;   /* The actual color index */
    dest++;
    *dest = EOS;
    return dest;
}

/**
 * FUNCTION: GetExtCtrlCodeLength
 *
 * PURPOSE: Return the number of parameter bytes that follow a given
 *          extended control code command byte.
 *
 * HOW IT WORKS:
 * Each control code (the byte after 0xFC) has a fixed number of parameter
 * bytes. This lookup table maps command codes to their parameter counts.
 * For example, the "set font" command (0x06) takes 1 parameter (font ID),
 * while the "set all colors" command (0x04) takes 3 parameters.
 *
 * The table stores the TOTAL length including the command byte itself:
 *   Length 1 = command only, no parameters
 *   Length 2 = command + 1 parameter byte
 *   Length 3 = command + 2 parameter bytes
 *   Length 4 = command + 3 parameter bytes
 *
 * PARAMETERS:
 * @param code -- The control code command byte (value after the 0xFC prefix)
 *
 * RETURNS: Total length in bytes (including the command byte, excluding the 0xFC prefix)
 */
u8 GetExtCtrlCodeLength(u8 code)
{
    static const u8 lengths[] =
    {
        1,  /* 0x00: 1 byte (command only) */
        2,  /* 0x01: set foreground color (1 param) */
        2,  /* 0x02: set background color (1 param) */
        2,  /* 0x03: set shadow color (1 param) */
        4,  /* 0x04: set all colors (3 params: fg, bg, shadow) */
        2,  /* 0x05: (1 param) */
        2,  /* 0x06: set font (1 param: font ID) */
        1,  /* 0x07: reset font (no params) */
        2,  /* 0x08: (1 param) */
        1,  /* 0x09: pause (no params, or handled separately) */
        1,  /* 0x0A: (no params) */
        3,  /* 0x0B: (2 params) */
        2,  /* 0x0C: (1 param) */
        2,  /* 0x0D: (1 param) */
        2,  /* 0x0E: set Y position (1 param) */
        1,  /* 0x0F: (no params) */
        3,  /* 0x10: (2 params) */
        2,  /* 0x11: (1 param) */
        2,  /* 0x12: (1 param) */
        2,  /* 0x13: (1 param) */
        2,  /* 0x14: (1 param) */
        1,  /* 0x15: enable Japanese mode (no params) */
        1,  /* 0x16: disable Japanese mode (no params) */
        1,  /* 0x17: (no params) */
        1,  /* 0x18: (no params) */
    };

    u8 length = 0;
    if (code < NELEMS(lengths))
        length = lengths[code];
    return length;
}

/**
 * FUNCTION: SkipExtCtrlCode
 *
 * PURPOSE: Advance a string pointer past any consecutive control code sequences.
 *
 * HOW IT WORKS:
 * While the current byte is 0xFC (control code prefix), skips past it and
 * its parameter bytes (whose count is looked up via GetExtCtrlCodeLength).
 * Stops when a non-control-code byte is reached.
 *
 * Used by StringCompareWithoutExtCtrlCodes to ignore formatting when comparing strings.
 *
 * PARAMETERS:
 * @param s -- String pointer, potentially pointing at a control code
 *
 * RETURNS: Pointer to the next non-control-code byte in the string
 */
static const u8 *SkipExtCtrlCode(const u8 *s)
{
    while (*s == 0xFC)
    {
        s++;                              /* Skip the 0xFC prefix */
        s += GetExtCtrlCodeLength(*s);    /* Skip the command and its parameters */
    }

    return s;
}

/**
 * FUNCTION: StringCompareWithoutExtCtrlCodes
 *
 * PURPOSE: Compare two strings while ignoring embedded control codes.
 *
 * HOW IT WORKS:
 * Advances both string pointers past any control codes before comparing
 * each character. This allows comparing the "plain text" content of two
 * strings that may have different formatting (colors, fonts, etc.).
 *
 * The comparison logic handles the EOS (0xFF) terminator specially:
 * if one string ends before the other, the shorter string is considered
 * "less than" the longer one, unless the terminated string is str2
 * (in which case str1 > str2 = positive).
 *
 * PARAMETERS:
 * @param str1 -- First string
 * @param str2 -- Second string
 *
 * RETURNS: 0 if equal, positive if str1 > str2, negative if str1 < str2
 */
s32 StringCompareWithoutExtCtrlCodes(const u8 *str1, const u8 *str2)
{
    s32 retVal = 0;

    while (1)
    {
        /* Skip over any control codes in both strings */
        str1 = SkipExtCtrlCode(str1);
        str2 = SkipExtCtrlCode(str2);

        if (*str1 > *str2)
            break;

        if (*str1 < *str2)
        {
            retVal = -1;
            if (*str2 == 0xFF)     /* str2 ended but str1 didn't => str1 is "greater" */
                retVal = 1;
        }

        if (*str1 == 0xFF)         /* Both strings reached EOS (since str1 <= str2 here) */
            return retVal;

        str1++;
        str2++;
    }

    retVal = 1;

    if (*str1 == 0xFF)             /* str1 ended but str2 didn't => str1 is "less" */
        retVal = -1;

    return retVal;
}

/**
 * FUNCTION: ConvertInternationalString
 *
 * PURPOSE: Wrap a Japanese string with Japanese-mode control codes for
 *          display in the international (English) version of the game.
 *
 * HOW IT WORKS:
 * If the string's language is Japanese, this function:
 * 1. Strips any existing control codes from the string
 * 2. Appends a "disable Japanese mode" control code (0xFC 0x16) at the end
 * 3. Shifts the entire string right by 2 bytes
 * 4. Prepends an "enable Japanese mode" control code (0xFC 0x15) at the start
 *
 * This ensures the text renderer switches to the Japanese character set for
 * this string and back to the Latin set afterward.
 *
 * GAME LOGIC:
 * When trading Pokemon between Japanese and international game cartridges,
 * Japanese nicknames and trainer names need to be displayable. This function
 * wraps them in the appropriate font-switching control codes.
 *
 * PARAMETERS:
 * @param s        -- The string to convert (modified in place)
 * @param language -- The language of the string (LANGUAGE_JAPANESE triggers conversion)
 */
void ConvertInternationalString(u8 *s, u8 language)
{
    if (language == LANGUAGE_JAPANESE)
    {
        u8 i;

        /* Remove any existing control codes */
        StripExtCtrlCodes(s);
        i = StringLength(s);

        /* Append "disable Japanese mode" control code at the end */
        s[i++] = 0xFC;     /* EXT_CTRL_CODE_BEGIN */
        s[i++] = 22;       /* 0x16 = disable Japanese mode */
        s[i++] = 0xFF;     /* EOS */

        /*
         * Shift entire string right by 2 bytes to make room for the
         * "enable Japanese mode" prefix. Works backward to avoid overwriting.
         */
        i--;

        while (i != (u8)-1)  /* Loop until i underflows past 0 (unsigned wraparound) */
        {
            s[i + 2] = s[i];
            i--;
        }

        /* Write "enable Japanese mode" control code at the start */
        s[0] = 0xFC;        /* EXT_CTRL_CODE_BEGIN */
        s[1] = 21;          /* 0x15 = enable Japanese mode */
    }
}

/**
 * FUNCTION: StripExtCtrlCodes
 *
 * PURPOSE: Remove all extended control code sequences from a string in place.
 *
 * HOW IT WORKS:
 * Uses two indices (srcIndex and destIndex) to do an in-place filter.
 * When a control code prefix (0xFC) is found, the source index skips past
 * it and all its parameter bytes. Regular characters are copied from the
 * source position to the destination position. Since control codes are
 * being removed, destIndex grows more slowly than srcIndex, effectively
 * compressing the string.
 *
 * PARAMETERS:
 * @param str -- The string to strip control codes from (modified in place)
 */
void StripExtCtrlCodes(u8 *str)
{
    u16 srcIndex = 0;
    u16 destIndex = 0;
    while (str[srcIndex] != 0xFF)  /* 0xFF = EOS */
    {
        if (str[srcIndex] == 0xFC)  /* 0xFC = control code prefix */
        {
            srcIndex++;                                    /* Skip the 0xFC */
            srcIndex += GetExtCtrlCodeLength(str[srcIndex]); /* Skip command + params */
        }
        else
        {
            /* Regular character: copy to the (potentially earlier) destination position */
            str[destIndex++] = str[srcIndex++];
        }
    }
    str[destIndex] = 0xFF;  /* Terminate the shortened string */
}
