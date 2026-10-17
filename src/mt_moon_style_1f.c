/*
 * mt_moon_style_1f.c - Mt. Moon 1F style: rock islands in open floor
 *
 * (Work in progress: slice 1 of the TDD loop, the island shape.)
 */

#include "global.h"
#include "mt_moon_gen_style.h"

#define TILE_FLOOR 0x3281

#define TILE_ROCK_TOP_LEFT     0x0688
#define TILE_ROCK_TOP          0x0689
#define TILE_ROCK_TOP_RIGHT    0x068a
#define TILE_ROCK_LEFT         0x0690
#define TILE_ROCK_MIDDLE       0x0691
#define TILE_ROCK_RIGHT        0x0692
#define TILE_ROCK_BOTTOM_LEFT  0x0698
#define TILE_ROCK_BOTTOM       0x0699
#define TILE_ROCK_BOTTOM_RIGHT 0x069a
#define TILE_ROCK_INNER_SW     0x069c

#define TILE_BORDER_CORNER_NW    0x069b
#define TILE_BORDER_CORNER_NE    0x069c
#define TILE_BORDER_TOP          0x06c4
#define TILE_BORDER_WEST_OUTER   0x06c7
#define TILE_BORDER_EAST_OUTER   0x06c6
#define TILE_BORDER_SOUTH        0x06a3
#define TILE_BORDER_SOUTH_EAST   0x06a4
#define TILE_BORDER_SOUTH_RIM    0x0689
#define TILE_BORDER_NORTH        0x0699
#define TILE_BORDER_WEST         0x0692
#define TILE_BORDER_EAST         0x0690

#define ISLAND_TRIES 1500
#define FILL_TRIES 500
#define ROCK_TARGET_MIN 420
#define ROCK_TARGET_SPREAD 80
#define RECENT_ISLANDS 16

struct IslandBox
{
    s8 x1;
    s8 y1;
    s8 x2;
    s8 y2;
};

static bool8 IsIslandSpotFree(const struct MtMoonCave *cave, s32 x1, s32 y1, s32 x2, s32 y2, s32 gap, bool8 mayTouchBorder)
{
    s32 x, y, top, bottom, left, right;

    if (!(x1 >= 4 || (x1 == 2 && mayTouchBorder)) || x2 > cave->width - 5)
        return FALSE;
    if (y1 < 4 || !(y2 <= cave->height - 5 || (y2 == cave->height - 3 && mayTouchBorder)))
        return FALSE;
    if (x1 - 2 <= cave->exit.x && cave->exit.x <= x2 + 2 && y1 - 2 <= cave->exit.y && cave->exit.y <= y2 + 2)
        return FALSE;
    if (x2 >= cave->entrance.x - 3 && x1 <= cave->entrance.x + 3 && y2 >= cave->entrance.y - 4)
        return FALSE;
    top = max(2, y1 - gap);
    bottom = min(cave->height - 3, y2 + gap);
    left = max(2, x1 - gap);
    right = min(cave->width - 3, x2 + gap);
    for (y = top; y <= bottom; y++)
    {
        const u8 *row = MtMoonGen_GetRow(y);

        for (x = left; x <= right; x++)
        {
            if (row[x] != MT_MOON_CELL_FLOOR)
                return FALSE;
        }
    }
    return TRUE;
}

static s32 FillRock(s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 x, y;

    for (y = y1; y <= y2; y++)
        for (x = x1; x <= x2; x++)
            MtMoonGen_SetCell(x, y, MT_MOON_CELL_WALL);
    return (x2 - x1 + 1) * (y2 - y1 + 1);
}

static void Build1FShape(struct MtMoonCave *cave)
{
    struct IslandBox recent[RECENT_ISLANDS];
    s32 placed = 0;
    s32 rock = 0;
    s32 target, attempt, x, y;

    for (y = 2; y <= cave->height - 3; y++)
        for (x = 2; x <= cave->width - 3; x++)
            MtMoonGen_SetCell(x, y, MT_MOON_CELL_FLOOR);

    target = ROCK_TARGET_MIN + MtMoonGen_RandomBelow(ROCK_TARGET_SPREAD);
    for (attempt = 0; attempt < ISLAND_TRIES && rock < target; attempt++)
    {
        s32 kind = MtMoonGen_RandomBelow(100);
        s32 w, h, gap;
        bool8 isL = FALSE;

        if (kind < 30)
        {
            w = 4 + MtMoonGen_RandomBelow(11);
            h = 3 + (MtMoonGen_RandomBelow(4) == 0);
        }
        else if (kind < 65)
        {
            w = 3 + (MtMoonGen_RandomBelow(4) == 0);
            h = 4 + MtMoonGen_RandomBelow(9);
        }
        else if (kind < 82)
        {
            w = 3 + MtMoonGen_RandomBelow(4);
            h = 3 + MtMoonGen_RandomBelow(4);
        }
        else
        {
            w = 5 + MtMoonGen_RandomBelow(7);
            h = 5 + MtMoonGen_RandomBelow(6);
            isL = TRUE;
        }
        gap = 2 + (MtMoonGen_RandomBelow(4) == 0);
        if (placed > 0 && MtMoonGen_RandomBelow(2) == 0)
        {
            const struct IslandBox *next = &recent[MtMoonGen_RandomBelow(min(placed, RECENT_ISLANDS))];

            switch (MtMoonGen_RandomBelow(4))
            {
            case 0:  x = next->x2 + gap + 1; y = next->y1; break;
            case 1:  x = next->x1 - gap - w; y = next->y1; break;
            case 2:  x = next->x1;           y = next->y2 + gap + 1; break;
            default: x = next->x1;           y = next->y1 - gap - h; break;
            }
        }
        else
        {
            x = 2 + MtMoonGen_RandomBelow(cave->width - 5 - w);
            y = 4 + MtMoonGen_RandomBelow(cave->height - 5 - h);
        }
        if (!IsIslandSpotFree(cave, x, y, x + w - 1, y + h - 1, gap, !isL))
            continue;
        if (isL)
        {
            rock += FillRock(x, y, x + w - 1, y + 2);
            rock += FillRock(x + w - 3, y + 3, x + w - 1, y + h - 1);
        }
        else
        {
            rock += FillRock(x, y, x + w - 1, y + h - 1);
        }
        recent[placed % RECENT_ISLANDS].x1 = x;
        recent[placed % RECENT_ISLANDS].y1 = y;
        recent[placed % RECENT_ISLANDS].x2 = x + w - 1;
        recent[placed % RECENT_ISLANDS].y2 = y + h - 1;
        placed++;
    }
    for (attempt = 0; attempt < FILL_TRIES && rock < target; attempt++)
    {
        s32 w = 3, h = 3;

        if (MtMoonGen_RandomBelow(2) == 0)
        {
            if (MtMoonGen_RandomBelow(2) == 0)
                h = 4;
            else
                w = 4;
        }
        x = 2 + MtMoonGen_RandomBelow(cave->width - 5 - w);
        y = 4 + MtMoonGen_RandomBelow(cave->height - 5 - h);
        if (IsIslandSpotFree(cave, x, y, x + w - 1, y + h - 1, 2, TRUE))
            rock += FillRock(x, y, x + w - 1, y + h - 1);
    }
    MtMoonGen_MarkLadderSite(&cave->entrance);
    MtMoonGen_MarkLadderSite(&cave->exit);
}

static u16 GetRockTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u8 open = MtMoonGen_GetFloorMask(x, y);

    if (x == 2)
        open |= MT_MOON_NW | MT_MOON_W | MT_MOON_SW;
    if (x == cave->width - 3)
        open |= MT_MOON_NE | MT_MOON_E | MT_MOON_SE;
    if (y == 2)
        open |= MT_MOON_NW | MT_MOON_N | MT_MOON_NE;
    if (y == cave->height - 3)
        open |= MT_MOON_SW | MT_MOON_S | MT_MOON_SE;
    if (open & MT_MOON_N)
        return (open & MT_MOON_W) ? TILE_ROCK_TOP_LEFT : (open & MT_MOON_E) ? TILE_ROCK_TOP_RIGHT : TILE_ROCK_TOP;
    if (open & MT_MOON_S)
        return (open & MT_MOON_W) ? TILE_ROCK_BOTTOM_LEFT : (open & MT_MOON_E) ? TILE_ROCK_BOTTOM_RIGHT : TILE_ROCK_BOTTOM;
    if (open & MT_MOON_W)
        return TILE_ROCK_LEFT;
    if (open & MT_MOON_E)
        return TILE_ROCK_RIGHT;
    if (open & MT_MOON_SW)
        return TILE_ROCK_INNER_SW;
    return TILE_ROCK_MIDDLE;
}

static u16 GetBorderTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    s32 right = cave->width - 1;
    s32 bottom = cave->height - 1;

    if (y == 0)
        return (x == 0) ? TILE_BORDER_CORNER_NW : (x == right) ? TILE_BORDER_CORNER_NE : TILE_BORDER_TOP;
    if (y == bottom)
        return TILE_BORDER_SOUTH;
    if (x == 0)
        return TILE_BORDER_WEST_OUTER;
    if (x == right)
        return TILE_BORDER_EAST_OUTER;
    if (y == 1)
        return (x == 1) ? TILE_BORDER_CORNER_NW : (x == right - 1) ? TILE_BORDER_CORNER_NE : TILE_BORDER_NORTH;
    if (y == bottom - 1)
        return (x == 1) ? TILE_BORDER_SOUTH : (x == right - 1) ? TILE_BORDER_SOUTH_EAST : TILE_BORDER_SOUTH_RIM;
    if (x == 1)
        return TILE_BORDER_WEST;
    return TILE_BORDER_EAST;
}

static u16 Pick1FTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u16 tile = MtMoonGen_GetLadderTile(cave, x, y);

    if (tile != 0)
        return tile;
    if (x < 2 || y < 2 || x > cave->width - 3 || y > cave->height - 3)
        return GetBorderTile(cave, x, y);
    if (!MtMoonGen_IsFloor(x, y))
        return GetRockTile(cave, x, y);
    return TILE_FLOOR;
}

const struct MtMoonStyle gMtMoonStyle1F = { Build1FShape, Pick1FTile };
