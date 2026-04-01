/**
 * @file money.c
 * @brief Player Money (Pokedollar) Management System
 *
 * FILE OVERVIEW:
 * This file handles the player's primary currency — Pokedollars. It provides
 * functions to get, set, add, remove, and display money. Like coins (coins.c),
 * the money value is XOR-encrypted in the save data as an anti-tampering measure.
 *
 * The maximum money is 999,999 Pokedollars. All arithmetic operations clamp to
 * this maximum and to zero (no negative money).
 *
 * DISPLAY SYSTEM:
 * The money display is a small bordered window showing the Pokedollar symbol
 * followed by the amount. It appears in shops, when checking the Trainer Card,
 * and other contexts where the player needs to see their balance.
 */
#include "global.h"
#include "gflib.h"
#include "event_data.h"
#include "menu.h"
#include "text_window.h"
#include "strings.h"

/* Maximum money the player can hold — displayed as 6 digits */
#define MAX_MONEY 999999

/* Window ID for the money display box, stored in EWRAM */
EWRAM_DATA static u8 sMoneyBoxWindowId = 0;

/**
 * FUNCTION: GetMoney
 *
 * PURPOSE: Returns the player's current money, decrypted from save data.
 *
 * HOW IT WORKS:
 * The money is stored XOR'd with an encryption key. XOR is self-inverse,
 * so applying XOR again with the same key recovers the original value.
 * The pointer-based interface allows this to work with different save
 * block fields (e.g., a second player's money in multiplayer).
 *
 * @param moneyPtr — Pointer to the encrypted money value in save data
 * RETURNS: The decrypted money amount
 */
u32 GetMoney(u32 *moneyPtr)
{
    return *moneyPtr ^ gSaveBlock2Ptr->encryptionKey;
}

/**
 * FUNCTION: SetMoney
 *
 * PURPOSE: Sets the player's money to a new value, encrypting it for storage.
 *
 * @param moneyPtr — Pointer to the money field in save data
 * @param newValue — The new money amount to store
 */
void SetMoney(u32 *moneyPtr, u32 newValue)
{
    *moneyPtr = gSaveBlock2Ptr->encryptionKey ^ newValue;
}

/**
 * FUNCTION: IsEnoughMoney
 *
 * PURPOSE: Checks whether the player has at least 'cost' Pokedollars.
 *
 * @param moneyPtr — Pointer to the encrypted money value
 * @param cost — The amount to check against
 * RETURNS: TRUE if the player can afford 'cost', FALSE otherwise
 */
bool8 IsEnoughMoney(u32 *moneyPtr, u32 cost)
{
    if (GetMoney(moneyPtr) >= cost)
        return TRUE;
    else
        return FALSE;
}

/**
 * FUNCTION: AddMoney
 *
 * PURPOSE: Adds Pokedollars to the player's balance, clamping at MAX_MONEY
 *          and handling integer overflow.
 *
 * GAME LOGIC:
 * Two overflow protections:
 * 1. If (current + toAdd) exceeds MAX_MONEY, clamp to MAX_MONEY
 * 2. If (current + toAdd) wraps around due to u32 overflow (sum < original),
 *    clamp to MAX_MONEY. This is unlikely with MAX_MONEY = 999999 but
 *    protects against code bugs or save manipulation.
 *
 * @param moneyPtr — Pointer to the encrypted money value in save data
 * @param toAdd — Amount of money to add
 */
void AddMoney(u32 *moneyPtr, u32 toAdd)
{
    u32 toSet = GetMoney(moneyPtr);

    // can't have more money than MAX
    if (toSet + toAdd > MAX_MONEY)
    {
        toSet = MAX_MONEY;
    }
    else
    {
        toSet += toAdd;
        // check overflow, can't have less money after you receive more
        if (toSet < GetMoney(moneyPtr))
            toSet = MAX_MONEY;
    }

    SetMoney(moneyPtr, toSet);
}

/**
 * FUNCTION: RemoveMoney
 *
 * PURPOSE: Subtracts Pokedollars from the player's balance, flooring at zero.
 *
 * @param moneyPtr — Pointer to the encrypted money value
 * @param toSub — Amount of money to subtract
 */
void RemoveMoney(u32 *moneyPtr, u32 toSub)
{
    u32 toSet = GetMoney(moneyPtr);

    // can't subtract more than you already have
    if (toSet < toSub)
        toSet = 0;
    else
        toSet -= toSub;

    SetMoney(moneyPtr, toSet);
}

/**
 * FUNCTION: IsEnoughForCostInVar0x8005
 *
 * PURPOSE: Script helper — checks if the player can afford the cost stored in
 *          special variable 0x8005.
 *
 * GAME LOGIC:
 * The scripting engine uses special variables (gSpecialVar_0x8005, etc.) to
 * pass parameters between script commands and C functions. A shop script might
 * set Var0x8005 to an item's price, then call this to check affordability.
 *
 * RETURNS: TRUE if the player has enough money
 */
bool8 IsEnoughForCostInVar0x8005(void)
{
    return IsEnoughMoney(&gSaveBlock1Ptr->money, gSpecialVar_0x8005);
}

/**
 * FUNCTION: SubtractMoneyFromVar0x8005
 *
 * PURPOSE: Script helper — subtracts the cost stored in special variable 0x8005
 *          from the player's money.
 */
void SubtractMoneyFromVar0x8005(void)
{
    RemoveMoney(&gSaveBlock1Ptr->money, gSpecialVar_0x8005);
}

/**
 * FUNCTION: PrintMoneyAmountInMoneyBox
 *
 * PURPOSE: Renders the money amount with a Pokedollar symbol, right-aligned
 *          in the money display window.
 *
 * HOW IT WORKS:
 * 1. Converts the amount to a left-aligned 6-digit string (e.g., "1500")
 * 2. Calculates padding: for "1500" (4 chars), inserts 2 null/space bytes
 *    before the string to right-align within the 6-character field
 * 3. Expands the "$VAR1" placeholder to produce the final display string
 * 4. Draws right-aligned within a 64-pixel area
 *
 * @param windowId — Which window to render into
 * @param amount — Money amount to display
 * @param speed — Text drawing speed (0 = instant)
 */
void PrintMoneyAmountInMoneyBox(u8 windowId, int amount, u8 speed)
{
    u8 *txtPtr;
    s32 strLength;

    ConvertIntToDecimalStringN(gStringVar1, amount, STR_CONV_MODE_LEFT_ALIGN, 6);

    /* Calculate padding needed for right-alignment within 6-character field */
    strLength = 6 - StringLength(gStringVar1);
    txtPtr = gStringVar4;

    /* Insert null bytes as leading padding for right-alignment */
    while (strLength-- != 0)
        *(txtPtr++) = 0;

    /* Expand "$1500" placeholder to produce "Pokedollar-symbol 1500" */
    StringExpandPlaceholders(txtPtr, gText_PokedollarVar1);
    /* Right-align the entire string within a 64-pixel-wide display area */
    AddTextPrinterParameterized(windowId, FONT_SMALL, gStringVar4, 64 - GetStringWidth(FONT_SMALL, gStringVar4, 0), 0xC, speed, NULL);
}

/**
 * FUNCTION: PrintMoneyAmount
 *
 * PURPOSE: Renders the money amount at a specific position within any window.
 *          More flexible version of PrintMoneyAmountInMoneyBox.
 *
 * @param windowId — Which window to render into
 * @param x — X pixel position
 * @param y — Y pixel position
 * @param amount — Money amount to display
 * @param speed — Text drawing speed
 */
void PrintMoneyAmount(u8 windowId, u8 x, u8 y, int amount, u8 speed)
{
    u8 *txtPtr;
    s32 strLength;

    ConvertIntToDecimalStringN(gStringVar1, amount, STR_CONV_MODE_LEFT_ALIGN, 6);

    strLength = 6 - StringLength(gStringVar1);
    txtPtr = gStringVar4;

    while (strLength-- != 0)
        *(txtPtr++) = 0;

    StringExpandPlaceholders(txtPtr, gText_PokedollarVar1);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gStringVar4, x, y, speed, NULL);
}

/**
 * FUNCTION: PrintMoneyAmountInMoneyBoxWithBorder
 *
 * PURPOSE: Draws a bordered frame around a window, adds the "MONEY" label,
 *          and prints the money amount. Used for the full money box display.
 *
 * @param windowId — Which window to draw into
 * @param tileStart — Starting VRAM tile for the frame graphics
 * @param paletteNum — Which palette to use for the frame
 * @param amount — Money amount to display
 */
void PrintMoneyAmountInMoneyBoxWithBorder(u8 windowId, u16 tileStart, u8 paletteNum, int amount)
{
    DrawStdFrameWithCustomTileAndPalette(windowId, FALSE, tileStart, paletteNum);
    AddTextPrinterParameterized(windowId, FONT_NORMAL, gText_TrainerCardMoney, 0, 0, 0xFF, 0);
    PrintMoneyAmountInMoneyBox(windowId, amount, 0);
}

/**
 * FUNCTION: ChangeAmountInMoneyBox
 *
 * PURPOSE: Updates just the money amount in the already-displayed money box.
 *          Used when the amount changes (e.g., after buying/selling in a shop).
 *
 * @param amount — New money amount to display
 */
void ChangeAmountInMoneyBox(int amount)
{
    PrintMoneyAmountInMoneyBox(sMoneyBoxWindowId, amount, 0);
}

/**
 * FUNCTION: DrawMoneyBox
 *
 * PURPOSE: Creates and displays the money box window with border, "MONEY" label,
 *          and the current amount.
 *
 * GAME LOGIC:
 * This is the main entry point for showing the money display, typically called
 * when entering a shop. The window is 8 tiles wide by 3 tiles tall, placed
 * at the specified tile coordinates.
 *
 * GBA CONTEXT:
 * The window template fields: BG layer 0, position (x+1, y+1) to account for
 * the border frame, size 8x3 tiles, palette 15, base tile number 8.
 * 0x21D is the VRAM tile offset where the standard window border graphics
 * are loaded. BG_PLTT_ID(13) converts palette 13 to its byte offset.
 *
 * @param amount — Money amount to display
 * @param x — X tile position on screen
 * @param y — Y tile position on screen
 */
void DrawMoneyBox(int amount, u8 x, u8 y)
{
    struct WindowTemplate template;

    template = SetWindowTemplateFields(0, x + 1, y + 1, 8, 3, 15, 8);
    sMoneyBoxWindowId = AddWindow(&template);
    FillWindowPixelBuffer(sMoneyBoxWindowId, 0);     /* Clear window contents */
    PutWindowTilemap(sMoneyBoxWindowId);              /* Map the window tiles to the BG layer */
    LoadStdWindowGfx(sMoneyBoxWindowId, 0x21D, BG_PLTT_ID(13));  /* Load frame border tiles */
    PrintMoneyAmountInMoneyBoxWithBorder(sMoneyBoxWindowId, 0x21D, 13, amount);
}

/**
 * FUNCTION: HideMoneyBox
 *
 * PURPOSE: Removes the money box from the screen and frees its window resources.
 *
 * HOW IT WORKS:
 * 1. Clears the window and frame to transparent pixels
 * 2. Copies the cleared window data to VRAM (COPYWIN_GFX copies only the
 *    tile graphics, not the tilemap, since ClearStdWindowAndFrameToTransparent
 *    already handled that)
 * 3. Frees the window slot
 */
void HideMoneyBox(void)
{
    ClearStdWindowAndFrameToTransparent(sMoneyBoxWindowId, FALSE);
    CopyWindowToVram(sMoneyBoxWindowId, COPYWIN_GFX);  /* Push cleared graphics to VRAM */
    RemoveWindow(sMoneyBoxWindowId);
}
