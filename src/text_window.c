/**
 * =TEXT WINDOW BORDER / FRAME GRAPHICS=
 *
 * FILE OVERVIEW:
 * This file handles loading the decorative border graphics that surround
 * text windows (message boxes, menus, dialog boxes) throughout the game.
 * Pokemon FireRed has several different window frame styles:
 *
 *   - Standard Window: The default menu/dialog border
 *   - Menu Message Window: Used for NPC dialog in the overworld
 *   - Signpost Window: The distinctive signpost-style frame
 *   - Help Message Window: Used for help system messages
 *   - Quest Log Window: Special frame for quest log entries
 *   - User-Selected Frames: The player can choose from multiple frame
 *     styles in the Options menu (stored in optionsWindowFrameType)
 *
 * GBA CONTEXT:
 * Each window border consists of 9 tiles arranged in a 3x3 pattern:
 *   [0][1][2]   (top-left, top, top-right)
 *   [3]   [5]   (left side, -, right side)
 *   [6][7][8]   (bottom-left, bottom, bottom-right)
 * The center (tile 4) is the window's text background, not drawn by
 * these border functions. Tiles 1, 3, 5, 7 are repeated along the
 * edges for windows of any size. DrawTextBorderOuter draws the border
 * OUTSIDE the window bounds, while DrawTextBorderInner draws it using
 * the window's own tile space.
 */
#include "global.h"
#include "gflib.h"
#include "text_window.h"
#include "text_window_graphics.h"
#include "help_message.h"
#include "new_menu_helpers.h"

static void LoadUserWindowGfxByFrame(u8 windowId, u8 frameType, u16 destOffset, u8 palOffset);

// Unused
// Equivalent to LoadHelpMessageWindowGfx, but takes a bg id directly
static void LoadHelpMessageWindowGfxOnBg(u8 bgId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(bgId, gHelpMessageWindow_Gfx, 0x280, destOffset);
    LoadPalette(GetTextWindowPalette(2), palOffset, PLTT_SIZE_4BPP);
}

// Unused
// Equivalent to LoadMenuMessageWindowGfx, but takes a bg id directly
static void LoadMenuMessageWindowGfxOnBg(u8 bgId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(bgId, gMenuMessageWindow_Gfx, 0x280, destOffset);
    LoadPalette(GetTextWindowPalette(0), palOffset, PLTT_SIZE_4BPP);
}

// Unused
// Equivalent to LoadSignpostWindowGfx, but takes a bg id directly
static void LoadSignpostWindowGfxOnBg(u8 bgId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(bgId, gSignpostWindow_Gfx, 0x260, destOffset);
    LoadPalette(GetTextWindowPalette(1), palOffset, PLTT_SIZE_4BPP);
}

// Equivalent to LoadStdWindowGfx, but takes a bg id directly
void LoadStdWindowGfxOnBg(u8 bgId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(bgId, gStdTextWindow_Gfx, 0x120, destOffset);
    LoadPalette(GetTextWindowPalette(3), palOffset, PLTT_SIZE_4BPP);
}

// Unused
// Equivalent to LoadQuestLogWindowTiles, but takes a bg id directly
static void LoadQuestLogWindowTilesOnBg(u8 bgId, u16 destOffset)
{
    LoadBgTiles(bgId, gQuestLogWindow_Gfx, 0x280, destOffset);
}

// Unused
// Equivalent to LoadUserWindowGfxByFrame, but takes a bg id directly
static void LoadUserWindowGfxByFrameOnBg(u8 bgId, u8 frameType, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(bgId, gUserFrames[frameType].tiles, 0x120, destOffset);
    LoadPalette(gUserFrames[frameType].palette, palOffset, PLTT_SIZE_4BPP);
}

// Identical to LoadUserWindowGfx
void LoadUserWindowGfx2(u8 windowId, u16 destOffset, u8 palOffset)
{
    LoadUserWindowGfxByFrame(windowId, gSaveBlock2Ptr->optionsWindowFrameType, destOffset, palOffset);
}

void LoadHelpMessageWindowGfx(u8 windowId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gHelpMessageWindow_Gfx, 0x280, destOffset);
    LoadPalette(GetTextWindowPalette(2), palOffset, PLTT_SIZE_4BPP);
}

void LoadMenuMessageWindowGfx(u8 windowId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gMenuMessageWindow_Gfx, 0x280, destOffset);
    LoadPalette(GetTextWindowPalette(0), palOffset, PLTT_SIZE_4BPP);
}

void LoadSignpostWindowGfx(u8 windowId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gSignpostWindow_Gfx, 0x260, destOffset);
    LoadPalette(GetTextWindowPalette(1), palOffset, PLTT_SIZE_4BPP);
}

void LoadStdWindowGfx(u8 windowId, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gStdTextWindow_Gfx, 0x120, destOffset);
    LoadPalette(GetTextWindowPalette(3), palOffset, PLTT_SIZE_4BPP);
}

void LoadStdWindowTiles(u8 windowId, u16 destOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gStdTextWindow_Gfx, 0x120, destOffset);
}

void LoadQuestLogWindowTiles(u8 windowId, u16 destOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gQuestLogWindow_Gfx, 0x280, destOffset);
}

static void LoadUserWindowGfxByFrame(u8 windowId, u8 frameType, u16 destOffset, u8 palOffset)
{
    LoadBgTiles(GetWindowAttribute(windowId, WINDOW_BG), gUserFrames[frameType].tiles, 0x120, destOffset);
    LoadPalette(gUserFrames[frameType].palette, palOffset, PLTT_SIZE_4BPP);
}

void LoadUserWindowGfx(u8 windowId, u16 destOffset, u8 palOffset)
{
    LoadUserWindowGfxByFrame(windowId, gSaveBlock2Ptr->optionsWindowFrameType, destOffset, palOffset);
}

void DrawTextBorderOuter(u8 windowId, u16 tileNum, u8 palNum)
{
    u8 bgLayer = GetWindowAttribute(windowId, WINDOW_BG);
    u16 tilemapLeft = GetWindowAttribute(windowId, WINDOW_TILEMAP_LEFT);
    u16 tilemapTop = GetWindowAttribute(windowId, WINDOW_TILEMAP_TOP);
    u16 width = GetWindowAttribute(windowId, WINDOW_WIDTH);
    u16 height = GetWindowAttribute(windowId, WINDOW_HEIGHT);

    FillBgTilemapBufferRect(bgLayer, tileNum + 0, tilemapLeft - 1,      tilemapTop - 1,         1,      1,      palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 1, tilemapLeft,          tilemapTop - 1,         width,  1,      palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 2, tilemapLeft + width,  tilemapTop - 1,         1,      1,      palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 3, tilemapLeft - 1,      tilemapTop,             1,      height, palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 5, tilemapLeft + width,  tilemapTop,             1,      height, palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 6, tilemapLeft - 1,      tilemapTop + height,    1,      1,      palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 7, tilemapLeft,          tilemapTop + height,    width,  1,      palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 8, tilemapLeft + width,  tilemapTop + height,    1,      1,      palNum);
}

void DrawTextBorderInner(u8 windowId, u16 tileNum, u8 palNum)
{
    u8 bgLayer = GetWindowAttribute(windowId, WINDOW_BG);
    u16 tilemapLeft = GetWindowAttribute(windowId, WINDOW_TILEMAP_LEFT);
    u16 tilemapTop = GetWindowAttribute(windowId, WINDOW_TILEMAP_TOP);
    u16 width = GetWindowAttribute(windowId, WINDOW_WIDTH);
    u16 height = GetWindowAttribute(windowId, WINDOW_HEIGHT);

    FillBgTilemapBufferRect(bgLayer, tileNum + 0, tilemapLeft,              tilemapTop,                 1,          1,          palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 1, tilemapLeft + 1,          tilemapTop,                 width - 2,  1,          palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 2, tilemapLeft + width - 1,  tilemapTop,                 1,          1,          palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 3, tilemapLeft,              tilemapTop + 1,             1,          height - 2, palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 5, tilemapLeft + width - 1,  tilemapTop + 1,             1,          height - 2, palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 6, tilemapLeft,              tilemapTop + height - 1,    1,          1,          palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 7, tilemapLeft + 1,          tilemapTop + height - 1,    width -     2,  1,      palNum);
    FillBgTilemapBufferRect(bgLayer, tileNum + 8, tilemapLeft + width - 1,  tilemapTop + height - 1,    1,          1,          palNum);
}

void rbox_fill_rectangle(u8 windowId)
{
    u8 bgLayer = GetWindowAttribute(windowId, WINDOW_BG);
    u16 tilemapLeft = GetWindowAttribute(windowId, WINDOW_TILEMAP_LEFT);
    u16 tilemapTop = GetWindowAttribute(windowId, WINDOW_TILEMAP_TOP);
    u16 width = GetWindowAttribute(windowId, WINDOW_WIDTH);
    u16 height = GetWindowAttribute(windowId, WINDOW_HEIGHT);

    FillBgTilemapBufferRect(bgLayer, 0, tilemapLeft - 1, tilemapTop - 1, width + 2, height + 2, 17);
}

const u16 *GetTextWindowPalette(u8 id)
{
    switch (id)
    {
    case 0:
        id = 0;
        break;
    case 1:
        id = 0x10;
        break;
    case 2:
        id = 0x20;
        break;
    case 3:
        id = 0x30;
        break;
    case 4:
    default:
        id = 0x40;
        break;
    }

    return (const u16 *)(gTextWindowPalettes) + id;
}
