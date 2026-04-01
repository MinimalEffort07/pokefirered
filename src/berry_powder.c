/**
 * =BERRY POWDER SYSTEM=
 *
 * FILE OVERVIEW:
 * This file manages Berry Powder — a currency earned from the Berry Crush
 * minigame that can be spent at special vendors. It provides functions to
 * get, give, take, and display the player's Berry Powder amount.
 *
 * SAVE DATA ENCRYPTION:
 * Berry Powder is stored encrypted in the save file using XOR encryption
 * with gSaveBlock2Ptr->encryptionKey. This is an anti-cheat measure that
 * makes it harder to edit save files with a hex editor. Every read must
 * decrypt (XOR with key), and every write must encrypt (XOR with key).
 * Since XOR is its own inverse (A ^ K ^ K = A), the same key is used
 * for both encryption and decryption.
 */
#include "global.h"
#include "event_data.h"
#include "load_save.h"
#include "menu.h"
#include "palette.h"
#include "quest_log.h"
#include "script_menu.h"
#include "string_util.h"
#include "strings.h"
#include "text.h"
#include "text_window.h"

#define MAX_BERRY_POWDER 99999  /* Maximum powder the player can hold */

static EWRAM_DATA u8 sBerryPowderVendorWindowId = 0;

/**
 * FUNCTION: DecryptBerryPowder
 *
 * PURPOSE: Reads the encrypted Berry Powder value and returns the actual amount.
 * XOR with the encryption key reverses the encryption applied during storage.
 */
u32 DecryptBerryPowder(u32 *powder)
{
    return *powder ^ gSaveBlock2Ptr->encryptionKey;
}

/**
 * FUNCTION: SetBerryPowder
 *
 * PURPOSE: Stores a Berry Powder amount by encrypting it with XOR.
 * The encrypted value is what gets saved to flash memory.
 */
void SetBerryPowder(u32 *powder, u32 amount)
{
    *powder = amount ^ gSaveBlock2Ptr->encryptionKey;
}

/**
 * FUNCTION: ApplyNewEncryptionKeyToBerryPowder
 *
 * PURPOSE: Re-encrypts the Berry Powder when the save encryption key changes.
 * This happens during save operations to keep the encrypted data consistent.
 * Without this, loading the save with a new key would produce garbage values.
 */
void ApplyNewEncryptionKeyToBerryPowder(u32 encryptionKey)
{
    ApplyNewEncryptionKeyToWord(&gSaveBlock2Ptr->berryCrush.berryPowderAmount, encryptionKey);
}

static bool8 HasEnoughBerryPowder(u32 cost)
{
    if (DecryptBerryPowder(&gSaveBlock2Ptr->berryCrush.berryPowderAmount) < cost)
        return FALSE;
    else
        return TRUE;
}

bool8 Script_HasEnoughBerryPowder(void)
{
    if (DecryptBerryPowder(&gSaveBlock2Ptr->berryCrush.berryPowderAmount) < gSpecialVar_0x8004)
        return FALSE;
    else
        return TRUE;
}

/**
 * FUNCTION: GiveBerryPowder
 *
 * PURPOSE: Adds Berry Powder to the player's total, capping at MAX_BERRY_POWDER.
 *
 * RETURNS: TRUE if the full amount was added, FALSE if capped at maximum
 */
bool8 GiveBerryPowder(u32 amountToAdd)
{
    u32 *powder = &gSaveBlock2Ptr->berryCrush.berryPowderAmount;
    u32 amount = DecryptBerryPowder(powder) + amountToAdd;
    if (amount > MAX_BERRY_POWDER)
    {
        SetBerryPowder(powder, MAX_BERRY_POWDER);
        return FALSE;
    }
    else
    {
        SetBerryPowder(powder, amount);
        return TRUE;
    }
}

static bool8 TakeBerryPowder(u32 cost)
{
    u32 *powder = &gSaveBlock2Ptr->berryCrush.berryPowderAmount;
    if (!HasEnoughBerryPowder(cost))
        return FALSE;
    else
    {
        u32 amount = DecryptBerryPowder(powder);
        SetBerryPowder(powder, amount - cost);
        return TRUE;
    }
}

bool8 Script_TakeBerryPowder(void)
{
    u32 *powder = &gSaveBlock2Ptr->berryCrush.berryPowderAmount;
    if (!HasEnoughBerryPowder(gSpecialVar_0x8004))
        return FALSE;
    else
    {
        u32 amount = DecryptBerryPowder(powder);
        SetBerryPowder(powder, amount - gSpecialVar_0x8004);
        return TRUE;
    }
}

u32 GetBerryPowder(void)
{
    return DecryptBerryPowder(&gSaveBlock2Ptr->berryCrush.berryPowderAmount);
}

static void PrintBerryPowderAmount(u8 windowId, u32 amount, u8 x, u8 y, u8 speed)
{
    ConvertIntToDecimalStringN(gStringVar1, amount, STR_CONV_MODE_RIGHT_ALIGN, 5);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gStringVar1, x, y, speed, NULL);
}

static void DrawPlayerPowderAmount(u8 windowId, u16 baseBlock, u8 palette, u32 amount)
{
    DrawStdFrameWithCustomTileAndPalette(windowId, FALSE, baseBlock, palette);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gOtherText_Powder, 0, 0, -1, NULL);
    PrintBerryPowderAmount(windowId, amount, 39, 12, 0);
}

void PrintPlayerBerryPowderAmount(void)
{
    PrintBerryPowderAmount(sBerryPowderVendorWindowId, GetBerryPowder(), 39, 12, 0);
}

void DisplayBerryPowderVendorMenu(void)
{
    struct WindowTemplate template;

    if (QL_AvoidDisplay(QL_DestroyAbortedDisplay) == TRUE)
        return;

    template = SetWindowTemplateFields(0, 1, 1, 8, 3, 15, 32);
    sBerryPowderVendorWindowId = AddWindow(&template);
    FillWindowPixelBuffer(sBerryPowderVendorWindowId, 0);
    PutWindowTilemap(sBerryPowderVendorWindowId);
    LoadStdWindowGfx(sBerryPowderVendorWindowId, 0x21D, BG_PLTT_ID(13));
    DrawPlayerPowderAmount(sBerryPowderVendorWindowId, 0x21D, 13, GetBerryPowder());
}

void RemoveBerryPowderVendorMenu(void)
{
    ClearWindowTilemap(sBerryPowderVendorWindowId);
    ClearStdWindowAndFrameToTransparent(sBerryPowderVendorWindowId, 1);
    RemoveWindow(sBerryPowderVendorWindowId);
}
