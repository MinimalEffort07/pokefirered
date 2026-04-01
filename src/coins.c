/**
 * @file coins.c
 * @brief Game Corner Coin Currency System
 *
 * FILE OVERVIEW:
 * This file manages the player's coin balance for the Celadon Game Corner
 * (the slot machine / casino area in Pokemon FireRed). Coins are a secondary
 * currency separate from regular money (Pokedollars) — they can be earned by
 * playing slot machines or purchased at the Game Corner counter, then exchanged
 * for prizes like TMs and rare Pokemon.
 *
 * SECURITY / ANTI-CHEAT:
 * The coin value is XOR-encrypted with an encryption key stored in the save data.
 * This means if someone tries to edit the raw save file to give themselves 9999 coins,
 * the XOR would produce a garbled value unless they also know the encryption key.
 * This is a simple but effective anti-tampering measure used for both coins and money.
 *
 * The encryption key (gSaveBlock2Ptr->encryptionKey) is generated randomly when a
 * new game starts, making it different for every save file.
 */
#include "global.h"
#include "gflib.h"
#include "string_util.h"
#include "menu.h"
#include "text_window.h"
#include "strings.h"
#include "constants/coins.h"

/* Window ID for the coin counter display — stored in EWRAM (external work RAM) */
EWRAM_DATA static u8 sCoinsWindowId = 0;

/**
 * FUNCTION: GetCoins
 *
 * PURPOSE: Returns the player's current coin count, decrypted from save data.
 *
 * HOW IT WORKS:
 * The stored value is XOR'd with the encryption key. Since XOR is its own inverse
 * (A ^ B ^ B = A), XOR-ing the encrypted value with the same key recovers the
 * original coin count.
 *
 * RETURNS: The player's current coin count (0 to MAX_COINS)
 */
u16 GetCoins(void)
{
    return gSaveBlock1Ptr->coins ^ gSaveBlock2Ptr->encryptionKey;
}

/**
 * FUNCTION: SetCoins
 *
 * PURPOSE: Sets the player's coin count, encrypting it before storing in save data.
 *
 * @param coinAmount — The new coin count to store
 */
void SetCoins(u16 coinAmount)
{
    gSaveBlock1Ptr->coins = coinAmount ^ gSaveBlock2Ptr->encryptionKey;
}

/**
 * FUNCTION: AddCoins
 *
 * PURPOSE: Adds coins to the player's balance, clamping at MAX_COINS and
 *          handling integer overflow.
 *
 * GAME LOGIC:
 * The overflow check (coins <= coins + toAdd) catches the case where adding
 * a large number wraps the u16 value past 65535 back to a small number.
 * For example, if coins = 60000 and toAdd = 10000, the sum would be 70000
 * which overflows a u16, wrapping to 4464. The check detects this because
 * 60000 > 4464.
 *
 * @param toAdd — Number of coins to add
 * RETURNS: TRUE if coins were added (even if clamped), FALSE if already at MAX_COINS
 */
bool8 AddCoins(u16 toAdd)
{
    u16 coins = GetCoins();
    if (coins >= MAX_COINS)
        return FALSE;  /* Already at maximum — cannot add more */
    // check overflow, can't have less coins than previously
    if (coins <= coins + toAdd)
    {
        /* Normal case: addition didn't overflow */
        coins += toAdd;
        if (coins > MAX_COINS)
            coins = MAX_COINS;  /* Clamp to maximum */
    }
    else
    {
        /* Overflow detected: the sum wrapped around, so clamp to max */
        coins = MAX_COINS;
    }
    SetCoins(coins);
    return TRUE;
}

/**
 * FUNCTION: RemoveCoins
 *
 * PURPOSE: Removes coins from the player's balance. Fails if insufficient funds.
 *
 * @param toSub — Number of coins to remove
 * RETURNS: TRUE if coins were successfully removed, FALSE if insufficient coins
 */
bool8 RemoveCoins(u16 toSub)
{
    u16 coins = GetCoins();
    if (coins >= toSub)
    {
        SetCoins(coins - toSub);
        return TRUE;
    }
    return FALSE;  /* Not enough coins */
}

/**
 * FUNCTION: PrintCoinsString_Parameterized
 *
 * PURPOSE: Renders the coin count string into a specified window at a given position.
 *
 * HOW IT WORKS:
 * 1. Converts the coin amount to a right-aligned 4-digit decimal string (e.g., "  42")
 * 2. Expands the placeholder in the "COINS" text template with the number
 * 3. Draws the formatted string using the small font
 *
 * @param windowId — Which window to draw in
 * @param coinAmount — The coin count to display
 * @param x — X pixel position within the window
 * @param y — Y pixel position within the window
 * @param speed — Text drawing speed (0 = instant, 0xFF = wait for manual advance)
 */
static void PrintCoinsString_Parameterized(u8 windowId, u32 coinAmount, u8 x, u8 y, u8 speed)
{
    ConvertIntToDecimalStringN(gStringVar1, coinAmount, STR_CONV_MODE_RIGHT_ALIGN, 4);
    StringExpandPlaceholders(gStringVar4, gText_Coins);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gStringVar4, x, y, speed, NULL);
}

/**
 * FUNCTION: ShowCoinsWindow_Parameterized
 *
 * PURPOSE: Draws a bordered coin display window with the "COINS" label and amount.
 *          (Unused in the final game but kept in the code.)
 *
 * @param windowId — Which window to draw in
 * @param tileStart — Starting tile index for the window frame graphics
 * @param palette — Which palette to use for the frame
 * @param coinAmount — The coin count to display
 */
// Unused
static void ShowCoinsWindow_Parameterized(u8 windowId, u16 tileStart, u8 palette, u32 coinAmount)
{
    DrawStdFrameWithCustomTileAndPalette(windowId, FALSE, tileStart, palette);
    AddTextPrinterParameterized(windowId, FONT_NORMAL, gText_Coins_2, 0, 0, 0xFF, 0);
    PrintCoinsString_Parameterized(windowId, coinAmount, 0x10, 0xC, 0);
}

/**
 * FUNCTION: PrintCoinsString
 *
 * PURPOSE: Updates the coin count display in the coin window, right-aligning the text.
 *
 * HOW IT WORKS:
 * Formats the coin amount and right-aligns it within a 64-pixel-wide area by
 * calculating the string width and subtracting from 64 to get the x offset.
 *
 * @param coinAmount — The coin count to display
 */
void PrintCoinsString(u32 coinAmount)
{
    u8 windowId;
    int width;

    ConvertIntToDecimalStringN(gStringVar1, coinAmount, STR_CONV_MODE_RIGHT_ALIGN, 4);
    StringExpandPlaceholders(gStringVar4, gText_Coins);
    width = GetStringWidth(FONT_SMALL, gStringVar4, 0);
    windowId = sCoinsWindowId;
    /* Right-align: subtract string width from 64 pixels (the display area width) */
    AddTextPrinterParameterized(windowId, FONT_SMALL, gStringVar4, 64 - width, 0xC, 0, NULL);
}

/**
 * FUNCTION: ShowCoinsWindow
 *
 * PURPOSE: Creates and displays the coin counter window on screen, including the
 *          decorative frame border.
 *
 * GAME LOGIC:
 * This is called when the player enters the Game Corner to show their current
 * coin balance in a small box on screen. The window is 8 tiles wide by 3 tiles
 * tall (64x24 pixels), positioned at the caller-specified (x, y) tile coordinates.
 *
 * GBA CONTEXT:
 * The window system allocates a rectangular region of a background layer for
 * text rendering. LoadStdWindowGfx loads the border frame tile graphics into
 * VRAM starting at tile 0x21D, using palette slot 13. BG_PLTT_ID(13) converts
 * palette number 13 into the byte offset within palette RAM (13 * 16 * 2 bytes).
 *
 * @param coinAmount — Current coin count to display
 * @param x — X tile position on screen (0-29)
 * @param y — Y tile position on screen (0-19)
 */
void ShowCoinsWindow(u32 coinAmount, u8 x, u8 y)
{
    struct WindowTemplate template;

    /* Create a window template: BG 0, offset by +1 tile for frame border,
     * 8 tiles wide, 3 tiles tall, palette 0xF (15), base tile 0x20 */
    template = SetWindowTemplateFields(0, x + 1, y + 1, 8, 3, 0xF, 0x20);
    sCoinsWindowId = AddWindow(&template);
    FillWindowPixelBuffer(sCoinsWindowId, 0);     /* Clear the window to black/transparent */
    PutWindowTilemap(sCoinsWindowId);              /* Register this window's tiles on the BG tilemap */
    LoadStdWindowGfx(sCoinsWindowId, 0x21D, BG_PLTT_ID(13));  /* Load border frame graphics */
    DrawStdFrameWithCustomTileAndPalette(sCoinsWindowId, FALSE, 0x21D, 13);  /* Draw the frame border */
    AddTextPrinterParameterized(sCoinsWindowId, FONT_NORMAL, gText_Coins_2, 0, 0, 0xFF, 0);  /* "COINS" label */
    PrintCoinsString(coinAmount);  /* Display the actual coin count */
}

/**
 * FUNCTION: HideCoinsWindow
 *
 * PURPOSE: Removes the coin counter window from the screen and frees its resources.
 *
 * HOW IT WORKS:
 * 1. Removes the window's tiles from the background tilemap
 * 2. Clears the window frame graphics to transparent
 * 3. Frees the window slot for reuse by other UI elements
 */
void HideCoinsWindow(void)
{
    ClearWindowTilemap(sCoinsWindowId);
    ClearStdWindowAndFrameToTransparent(sCoinsWindowId, TRUE);
    RemoveWindow(sCoinsWindowId);
}
