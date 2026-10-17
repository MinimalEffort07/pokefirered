/*
 * mt_moon_gen.c - Procedural cave generation for Mt. Moon (generator core)
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * Mt. Moon's floors are not the fixed maps from the ROM: each time the player
 * walks in, the floor is generated. This is a CUSTOM ADDITION to FireRed.
 *
 * SEED AND DETERMINISM
 *
 * Each visit has a 16-bit seed, stored in the game variable
 * VAR_MT_MOON_MAP_SEED (game variables live in the save file). The whole
 * floor (shape, tiles, warps, NPC positions) is a pure function of that seed:
 *
 *   - Fresh entry (door, ladder or warp; InitMap): roll a new seed with the
 *     game's Random(), store it, generate.
 *   - Continue or quest-log replay (InitMapFromSavedGame): read the stored
 *     seed and generate again, which rebuilds the exact cave that was saved.
 *
 * After the seed is rolled, nothing here calls Random(). The game's Random()
 * is shared with the rest of the game and is even advanced once per frame by
 * the VBlank interrupt, so a value drawn from it at Continue would differ
 * from the one drawn on entry. Instead the generator has its own random
 * number generator (MtMoonGen_Random), seeded from the visit's seed with the
 * floor number mixed in.
 *
 * CORE AND STYLES
 *
 * This file is the core: the floor table, the seed, the private random
 * generator, the cave grid, drawing the grid into the map, the warps, NPC
 * placement and the Continue safety net. What a floor LOOKS like (the shape
 * of its cave and the tile drawn for each cell) comes from that floor's style
 * module; see include/mt_moon_gen_style.h.
 *
 * GENERATION STEPS (GenerateFloor)
 *   1. Seed the private random generator.
 *   2. Borrow an all-wall grid the size of the floor's layout and let the
 *      floor's style carve the shape.
 *   3. Draw every map cell with the style's tile picker, then stamp the
 *      entrance doorway and the exit ladder.
 *   4. Rewrite the floor's warp table: the arrival warp sits on the entrance
 *      tile, the exit warp on the ladder; every other vanilla warp is parked
 *      at (0,0), a solid tile nobody can stand on.
 *   5. Place the floor's NPCs and item balls on open floor.
 * Then, on a fresh entry, the player is put on the generated arrival warp
 * (PutPlayerOnArrivalWarp); on Continue, the safety net makes sure the
 * player stands on reachable floor (MovePlayerOntoReachableFloor).
 *
 * GBA CONTEXT
 *
 * A map is a grid of 16x16-pixel "metatiles". The game keeps the loaded map
 * in a buffer (the "VMap") with a 7-tile margin around it (MAP_OFFSET), so map
 * tile (x, y) is VMap cell (x + 7, y + 7). Each cell is a 16-bit value:
 *     metatile id | (collision << 10) | (elevation << 12)
 * where collision 0 means walkable.
 * ============================================================================
 */

#include "global.h"
#include "event_data.h"
#include "fieldmap.h"
#include "malloc.h"
#include "overworld.h"
#include "quest_log.h"
#include "random.h"
#include "mt_moon_gen.h"
#include "mt_moon_gen_style.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/layouts.h"
#include "constants/maps.h"
#include "constants/vars.h"

/* Warp table entries kept per floor (B1F has the most vanilla warps: 8). */
#define MAX_WARPS 8

/* Tiles stamped at the warps (full map cell values, see the GBA notes). */
#define TILE_ENTRANCE   0x3287  /* walkable; stepping south on it warps out */
#define TILE_EXIT       0x3285  /* ladder */
#define TILE_RIM_TOP    0x0689  /* solid; either side of the doorframe */
#define TILE_DOORFRAME  0x06c9  /* solid; the doorway below the entrance */
#define TILE_BOTTOM     0x06a3  /* solid; the row below the doorway */

/* One generated floor and how it connects to the rest of the game. */
struct MtMoonFloor
{
    u16 layoutId;
    u8 floorNumber;     /* mixed into the seed, so floors get different caves */
    u8 arrivalWarpId;   /* vanilla warp a fresh entry arrives through */
    u8 exitWarpId;      /* vanilla warp reused as the exit ladder */
    u8 exitDestWarpId;  /* Route 4 warp the exit ladder leads to */
    s16 entranceX;
    s16 entranceY;
    s16 exitX;
    s16 exitY;
    const struct MtMoonStyle *style;
};

static const struct MtMoonFloor sFloors[] =
{
    /* 1F: Route 4 west leads in (Route 4 warp 0 -> 1F warp 3).
     * The exit ladder leads out to Route 4 east (Route 4 warp 1). */
    {
        .layoutId = LAYOUT_MT_MOON_1F,
        .floorNumber = 1,
        .arrivalWarpId = 3,
        .exitWarpId = 0,
        .exitDestWarpId = 1,
        .entranceX = 18,
        .entranceY = 36,
        .exitX = 24,
        .exitY = 3,
        .style = &gMtMoonStyle1F,
    },

    /* B1F: Route 4 east leads in (Route 4 warp 1 -> B1F warp 7).
     * The exit ladder leads out to Route 4 west (Route 4 warp 0). */
    {
        .layoutId = LAYOUT_MT_MOON_B1F,
        .floorNumber = 2,
        .arrivalWarpId = 7,
        .exitWarpId = 0,
        .exitDestWarpId = 0,
        .entranceX = 18,
        .entranceY = 36,
        .exitX = 24,
        .exitY = 3,
        .style = &gMtMoonStyleB1F,
    },
};

/*
 * IWRAM BUDGET: this file's uninitialised statics land in IWRAM .bss, which
 * the linker places just before m4a's sound buffer and the COMMON block
 * (gMain, gSaveBlock1Ptr, gRngValue, ...). Many tests hard-code those COMMON
 * addresses, so this file keeps exactly 16 bytes of IWRAM statics, the same
 * as before the generator was split up. New state belongs in EWRAM_DATA.
 * (Declaration order matters: agbcc aligns the struct to 8 bytes, so
 * putting the u32 first would leave a 4-byte gap.)
 */
static struct MtMoonCave sCave; /* the floor being generated (12 bytes) */
static u32 sRngState;           /* MtMoonGen_Random's state */

/*
 * The cave grid, one cell per map tile, at the floor layout's own size
 * (1F is 48 x 40, B1F is 49 x 40). Each cell is 2 bits, 4 cells per
 * byte: cell i (i = y * width + x) is byte i / 4, bits (i % 4) * 2 and up,
 * holding one of the MT_MOON_CELL_* values.
 *
 * The grid is only needed while a floor is being generated, and EWRAM (the
 * GBA's 256 KB work RAM, where statically allocated game data lives) is
 * completely full. So the grid is borrowed from the game's heap (Alloc,
 * src/malloc.c) for the duration of one generation and freed at the end;
 * only this pointer is permanent. The map loader borrows heap memory the
 * same way when it copies tilesets to video memory.
 */
static EWRAM_DATA u8 *sCaveGrid = NULL;

/* The rewritten event tables. gMapHeader.events points here while a
 * generated floor is loaded, instead of at the read-only ROM tables. */
static EWRAM_DATA struct WarpEvent sMtMoonWarps[MAX_WARPS] = {0};
static EWRAM_DATA struct MapEvents sMtMoonEvents = {0};

/* ------------------------------------------------------------------------- */
/* Private random number generator                                           */
/* ------------------------------------------------------------------------- */

/*
 * Same linear congruential formula as the game's Random(), on a private
 * state. The 16-bit seed is copied into both halves of the 32-bit state and
 * the floor number is mixed in by XOR with a multiple of 0x9E3779B9 (2^32
 * divided by the golden ratio, which spreads small numbers across all 32
 * bits). The first few outputs are thrown away so that seeds which differ
 * by 1 don't start with similar numbers.
 */
static void SeedCaveRng(u16 seed, u8 floorNumber)
{
    s32 i;

    sRngState = ((u32)seed << 16) | seed;
    sRngState ^= floorNumber * 0x9E3779B9;
    for (i = 0; i < 4; i++)
        MtMoonGen_Random();
}

u16 MtMoonGen_Random(void)
{
    sRngState = ISO_RANDOMIZE1(sRngState);
    return sRngState >> 16;
}

/* ------------------------------------------------------------------------- */
/* Cave grid                                                                 */
/* ------------------------------------------------------------------------- */

/* Borrow an all-wall grid (wall is 0, so zeroed memory) of the given size. */
static bool8 AllocCaveGrid(s32 width, s32 height)
{
    sCave.width = width;
    sCave.height = height;
    sCaveGrid = AllocZeroed((width * height * 2 + 7) / 8);
    return sCaveGrid != NULL;
}

static void FreeCaveGrid(void)
{
    if (sCaveGrid != NULL)
        Free(sCaveGrid);
    sCaveGrid = NULL;
}

static bool8 InGrid(s32 x, s32 y)
{
    return sCaveGrid != NULL && x >= 0 && x < sCave.width && y >= 0 && y < sCave.height;
}

u8 MtMoonGen_GetCell(s32 x, s32 y)
{
    s32 i;

    if (!InGrid(x, y))
        return MT_MOON_CELL_WALL;
    i = y * sCave.width + x;
    return (sCaveGrid[i / 4] >> ((i % 4) * 2)) & 3;
}

void MtMoonGen_SetCell(s32 x, s32 y, u8 value)
{
    s32 i, shift;

    if (!InGrid(x, y))
        return;
    i = y * sCave.width + x;
    shift = (i % 4) * 2;
    sCaveGrid[i / 4] = (sCaveGrid[i / 4] & ~(3 << shift)) | ((value & 3) << shift);
}

bool8 MtMoonGen_IsFloor(s32 x, s32 y)
{
    return MtMoonGen_GetCell(x, y) != MT_MOON_CELL_WALL;
}

u8 MtMoonGen_CountWallNeighbors(s32 cx, s32 cy)
{
    s32 x, y;
    u8 count = 0;

    for (y = cy - 1; y <= cy + 1; y++)
        for (x = cx - 1; x <= cx + 1; x++)
            if (!(x == cx && y == cy) && !MtMoonGen_IsFloor(x, y))
                count++;
    return count;
}

/*
 * Mark every floor cell connected to (fromX, fromY) as VISITED. This is a
 * "relaxation" flood fill: sweep the grid, turning any floor cell next to a
 * visited cell into a visited cell, until a sweep changes nothing. It needs
 * no queue memory, which matters with EWRAM almost full.
 */
static void MarkReachableFrom(s32 fromX, s32 fromY)
{
    s32 x, y;
    bool8 changed;

    if (MtMoonGen_GetCell(fromX, fromY) != MT_MOON_CELL_FLOOR)
        return;
    MtMoonGen_SetCell(fromX, fromY, MT_MOON_CELL_VISITED);
    do
    {
        changed = FALSE;
        for (y = 1; y < sCave.height - 1; y++)
            for (x = 1; x < sCave.width - 1; x++)
                if (MtMoonGen_GetCell(x, y) == MT_MOON_CELL_FLOOR
                    && (MtMoonGen_GetCell(x - 1, y) == MT_MOON_CELL_VISITED
                        || MtMoonGen_GetCell(x + 1, y) == MT_MOON_CELL_VISITED
                        || MtMoonGen_GetCell(x, y - 1) == MT_MOON_CELL_VISITED
                        || MtMoonGen_GetCell(x, y + 1) == MT_MOON_CELL_VISITED))
                {
                    MtMoonGen_SetCell(x, y, MT_MOON_CELL_VISITED);
                    changed = TRUE;
                }
    } while (changed);
}

/* Turn every VISITED cell back into plain floor. */
static void ClearVisited(void)
{
    s32 x, y;

    for (y = 0; y < sCave.height; y++)
        for (x = 0; x < sCave.width; x++)
            if (MtMoonGen_GetCell(x, y) == MT_MOON_CELL_VISITED)
                MtMoonGen_SetCell(x, y, MT_MOON_CELL_FLOOR);
}

bool8 MtMoonGen_FloodFillReaches(s32 fromX, s32 fromY, s32 toX, s32 toY)
{
    bool8 reachable;

    if (MtMoonGen_GetCell(fromX, fromY) == MT_MOON_CELL_WALL)
        return FALSE;
    MarkReachableFrom(fromX, fromY);
    reachable = (MtMoonGen_GetCell(toX, toY) == MT_MOON_CELL_VISITED);
    ClearVisited();
    return reachable;
}

/* ------------------------------------------------------------------------- */
/* Drawing the grid into the map                                             */
/* ------------------------------------------------------------------------- */

/*
 * Write a tile for every cell of the floor's layout. Cells outside the grid
 * read as wall, so the style draws them as wall too.
 */
static void DrawCave(const struct MtMoonFloor *floor)
{
    s32 x, y;
    s32 mapW = gMapHeader.mapLayout->width;
    s32 mapH = gMapHeader.mapLayout->height;
    s32 ex = sCave.entranceX;
    s32 ey = sCave.entranceY;

    for (y = 0; y < mapH; y++)
        for (x = 0; x < mapW; x++)
            MapGridSetMetatileEntryAt(x + MAP_OFFSET, y + MAP_OFFSET, floor->style->pickTile(&sCave, x, y));

    /* Entrance: the warp tile, with a doorframe in the wall south of it. */
    MapGridSetMetatileEntryAt(ex + MAP_OFFSET, ey + MAP_OFFSET, TILE_ENTRANCE);
    MapGridSetMetatileEntryAt(ex - 1 + MAP_OFFSET, ey + 1 + MAP_OFFSET, TILE_RIM_TOP);
    MapGridSetMetatileEntryAt(ex + MAP_OFFSET, ey + 1 + MAP_OFFSET, TILE_DOORFRAME);
    MapGridSetMetatileEntryAt(ex + 1 + MAP_OFFSET, ey + 1 + MAP_OFFSET, TILE_RIM_TOP);
    MapGridSetMetatileEntryAt(ex - 1 + MAP_OFFSET, ey + 2 + MAP_OFFSET, TILE_BOTTOM);
    MapGridSetMetatileEntryAt(ex + MAP_OFFSET, ey + 2 + MAP_OFFSET, TILE_BOTTOM);
    MapGridSetMetatileEntryAt(ex + 1 + MAP_OFFSET, ey + 2 + MAP_OFFSET, TILE_BOTTOM);

    MapGridSetMetatileEntryAt(sCave.exitX + MAP_OFFSET, sCave.exitY + MAP_OFFSET, TILE_EXIT);
}

/* ------------------------------------------------------------------------- */
/* Warps                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Copy the floor's vanilla warp table into RAM and rewrite it: the arrival
 * warp moves onto the entrance tile, the exit warp onto the ladder (leading
 * to Route 4), and every other warp is parked at (0,0), which is always
 * solid wall, so it can never be stepped on. Signs and hidden items (the
 * background events) are dropped. gMapHeader.events then points at the copy.
 */
static void SetUpWarps(const struct MtMoonFloor *floor)
{
    const struct MapEvents *rom = gMapHeader.events;
    s32 i;
    s32 count = rom->warpCount;

    if (count > MAX_WARPS)
        count = MAX_WARPS;

    sMtMoonEvents = *rom;
    for (i = 0; i < count; i++)
    {
        sMtMoonWarps[i] = rom->warps[i];
        sMtMoonWarps[i].x = 0;
        sMtMoonWarps[i].y = 0;
    }

    sMtMoonWarps[floor->exitWarpId].x = sCave.exitX;
    sMtMoonWarps[floor->exitWarpId].y = sCave.exitY;
    /* A warp only fires for a player at the same elevation (height layer)
     * as the warp. The cave floor and the ladder tile are elevation 3. */
    sMtMoonWarps[floor->exitWarpId].elevation = 3;
    sMtMoonWarps[floor->exitWarpId].warpId = floor->exitDestWarpId;
    sMtMoonWarps[floor->exitWarpId].mapNum = MAP_NUM(MAP_ROUTE4);
    sMtMoonWarps[floor->exitWarpId].mapGroup = MAP_GROUP(MAP_ROUTE4);

    sMtMoonWarps[floor->arrivalWarpId].x = sCave.entranceX;
    sMtMoonWarps[floor->arrivalWarpId].y = sCave.entranceY;

    sMtMoonEvents.warps = sMtMoonWarps;
    sMtMoonEvents.warpCount = count;
    sMtMoonEvents.bgEventCount = 0;
    gMapHeader.events = &sMtMoonEvents;
}

/* ------------------------------------------------------------------------- */
/* NPC placement                                                             */
/* ------------------------------------------------------------------------- */

/* TRUE if every cell an NPC can wander over is floor. */
static bool8 IsMovementAreaClear(s16 cx, s16 cy, s16 rangeX, s16 rangeY)
{
    s32 x, y;

    for (y = cy - rangeY; y <= cy + rangeY; y++)
        for (x = cx - rangeX; x <= cx + rangeX; x++)
            if (!MtMoonGen_IsFloor(x, y))
                return FALSE;
    return TRUE;
}

/*
 * Give every object template (the starting position of each trainer, NPC
 * and item ball, kept in the save block) a random spot on open floor. Each
 * template starts from the ROM copy, so the result depends only on the seed
 * even on Continue, when the save still holds the previous placement.
 *
 * Only the first two item balls are kept; the rest are parked at (0,0).
 * Objects are kept 3+ tiles from the entrance, the exit and each other, need
 * at least 4 floor neighbours, and wanderers need their whole roaming box on
 * floor. An object with no valid spot after 1000 tries is parked at (0,0).
 */
static void PlaceObjects(void)
{
    s32 i, j;
    u8 objectCount = gMapHeader.events->objectEventCount;
    const struct ObjectEventTemplate *rom = gMapHeader.events->objectEvents;
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    u8 itemCount = 0;

    for (i = 0; i < objectCount; i++)
    {
        s32 attempts = 0;
        s16 x, y;
        bool8 overlap;
        bool8 isItemBall;
        s16 moveRangeX, moveRangeY;
        u8 movementType;

        /* A clone template stands in for an object of a neighbouring map,
         * and its fields hold that map's ids instead of a movement type.
         * Mt. Moon has none, but leave any alone rather than overwrite them. */
        if (templates[i].kind != OBJ_KIND_CLONE)
        {
            templates[i].x = rom[i].x;
            templates[i].y = rom[i].y;
            templates[i].objUnion.normal.movementType = rom[i].objUnion.normal.movementType;
            templates[i].objUnion.normal.movementRangeX = rom[i].objUnion.normal.movementRangeX;
            templates[i].objUnion.normal.movementRangeY = rom[i].objUnion.normal.movementRangeY;
        }
        isItemBall = (templates[i].graphicsId == OBJ_EVENT_GFX_ITEM_BALL);
        movementType = templates[i].objUnion.normal.movementType;

        if (isItemBall)
        {
            itemCount++;
            if (itemCount > 2)
            {
                templates[i].x = 0;
                templates[i].y = 0;
                continue;
            }
        }

        /* Wanderers and walk-sequence NPCs become 1-tile-range wanderers. */
        if (movementType == MOVEMENT_TYPE_WALK_SEQUENCE_LEFT_DOWN_RIGHT_UP
            || movementType == MOVEMENT_TYPE_WANDER_AROUND
            || movementType == MOVEMENT_TYPE_WANDER_AROUND_SLOWER)
        {
            templates[i].objUnion.normal.movementType = MOVEMENT_TYPE_WANDER_AROUND;
            templates[i].objUnion.normal.movementRangeX = 1;
            templates[i].objUnion.normal.movementRangeY = 1;
        }

        moveRangeX = templates[i].objUnion.normal.movementRangeX;
        moveRangeY = templates[i].objUnion.normal.movementRangeY;
        if (movementType == MOVEMENT_TYPE_FACE_DOWN
            || movementType == MOVEMENT_TYPE_FACE_DOWN_AND_LEFT)
        {
            moveRangeX = 0;
            moveRangeY = 0;
        }

        do
        {
            /* Random candidate: x in [4, width - 5], y in [5, height - 6].
             * The margins keep a 1-tile roaming box clear of the solid
             * border (2 tiles thick at the sides and top, 3 at the bottom)
             * with a tile to spare. */
            x = (MtMoonGen_Random() % (sCave.width - 8)) + 4;
            y = (MtMoonGen_Random() % (sCave.height - 10)) + 5;
            attempts++;

            overlap = FALSE;
            if (!MtMoonGen_IsFloor(x, y))
            {
                overlap = TRUE;
                continue;
            }
            if ((x >= sCave.entranceX - 2 && x <= sCave.entranceX + 2
                 && y >= sCave.entranceY - 2 && y <= sCave.entranceY + 2)
                || (x >= sCave.exitX - 2 && x <= sCave.exitX + 2
                    && y >= sCave.exitY - 2 && y <= sCave.exitY + 2))
                overlap = TRUE;
            for (j = 0; j < i && !overlap; j++)
                if (abs(templates[j].x - x) <= 2 && abs(templates[j].y - y) <= 2)
                    overlap = TRUE;
            if (!overlap && !IsMovementAreaClear(x, y, moveRangeX, moveRangeY))
                overlap = TRUE;
            if (!overlap && (8 - MtMoonGen_CountWallNeighbors(x, y)) < 4)
                overlap = TRUE;
        }
        while (overlap && attempts < 1000);

        if (attempts < 1000)
        {
            templates[i].x = x;
            templates[i].y = y;
        }
        else
        {
            templates[i].x = 0;
            templates[i].y = 0;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Putting it together                                                       */
/* ------------------------------------------------------------------------- */

static const struct MtMoonFloor *GetCurrentFloor(void)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sFloors); i++)
        if (sFloors[i].layoutId == gMapHeader.mapLayoutId)
            return &sFloors[i];
    return NULL;
}

/*
 * Build `floor` from `seed` and write it into the loaded map. Leaves the
 * grid allocated for the caller's follow-up step; the caller must call
 * FreeCaveGrid(). Returns FALSE, changing nothing, if the heap can't lend
 * the grid; the floor then simply stays the vanilla map from the ROM, with
 * its vanilla warps and objects, which is fully playable. (On Continue the
 * player would keep coordinates from the generated cave, which may be wall
 * in the vanilla map, and the safety net can't run without the grid. The
 * grid is under 500 bytes of a 112 KB heap, so this is a should-never-
 * happen fallback, not an expected path.)
 */
static bool8 GenerateFloor(const struct MtMoonFloor *floor, u16 seed)
{
    if (!AllocCaveGrid(gMapHeader.mapLayout->width, gMapHeader.mapLayout->height))
        return FALSE;
    sCave.entranceX = floor->entranceX;
    sCave.entranceY = floor->entranceY;
    sCave.exitX = floor->exitX;
    sCave.exitY = floor->exitY;

    SeedCaveRng(seed, floor->floorNumber);
    floor->style->buildShape(&sCave);
    DrawCave(floor);
    SetUpWarps(floor);
    PlaceObjects();
    return TRUE;
}

/*
 * Before the map loads, the warp code puts the player on the coordinates of
 * the arrival warp in the ROM's warp table (SetPlayerCoordsFromWarp). The
 * generator has since moved that warp, so move the player to where it is
 * now. location.warpId is the warp the player came through; it is -1 (none)
 * for arrivals that aren't through a warp, which keep their position.
 */
static void PutPlayerOnArrivalWarp(void)
{
    s8 warpId = gSaveBlock1Ptr->location.warpId;
    const struct WarpEvent *warp;

    if (warpId < 0 || warpId >= gMapHeader.events->warpCount)
        return;
    warp = &gMapHeader.events->warps[warpId];
    if (warp->x == 0 && warp->y == 0)
        return; /* a parked, unused warp */
    gSaveBlock1Ptr->pos.x = warp->x;
    gSaveBlock1Ptr->pos.y = warp->y;
}

/* ------------------------------------------------------------------------- */
/* Continue safety net                                                       */
/* ------------------------------------------------------------------------- */

/* TRUE if a warp or an object (its template or a live one) is on the tile. */
static bool8 IsTileTaken(s32 x, s32 y)
{
    s32 i;
    const struct MapEvents *events = gMapHeader.events;

    for (i = 0; i < events->warpCount; i++)
        if (events->warps[i].x == x && events->warps[i].y == y)
            return TRUE;
    for (i = 0; i < events->objectEventCount; i++)
        if (gSaveBlock1Ptr->objectEventTemplates[i].x == x && gSaveBlock1Ptr->objectEventTemplates[i].y == y)
            return TRUE;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        if (gObjectEvents[i].active && !gObjectEvents[i].isPlayer
            && gObjectEvents[i].currentCoords.x - MAP_OFFSET == x
            && gObjectEvents[i].currentCoords.y - MAP_OFFSET == y)
            return TRUE;
    return FALSE;
}

/*
 * After Continue rebuilds a floor, the player's saved tile should be floor
 * the player can walk away from. It might not be: a save made before cave
 * seeds existed (the seed var reads 0 there, so a different cave is built),
 * or a future generator change that alters what a seed produces. In that
 * case move the player to the nearest floor tile that is reachable on foot
 * from the entrance and holds no warp or object. "Nearest" is by Manhattan
 * distance (steps east-west plus steps north-south); ties go to the first
 * tile in reading order, top row first.
 *
 * On Continue the player is drawn from the saved object events, which were
 * copied into gObjectEvents when the save was loaded, so the player's
 * object moves as well as SaveBlock1.pos (the map position the view is
 * drawn around). Object coordinates include the 7-tile MAP_OFFSET margin.
 */
static void MovePlayerOntoReachableFloor(void)
{
    s32 px = gSaveBlock1Ptr->pos.x;
    s32 py = gSaveBlock1Ptr->pos.y;
    s32 x, y, i;
    s32 bestX = -1, bestY = -1, bestDist = 0;

    MarkReachableFrom(sCave.entranceX, sCave.entranceY);
    if (MtMoonGen_GetCell(px, py) == MT_MOON_CELL_VISITED)
    {
        ClearVisited();
        return;
    }

    for (y = 0; y < sCave.height; y++)
    {
        for (x = 0; x < sCave.width; x++)
        {
            s32 dist;

            if (MtMoonGen_GetCell(x, y) != MT_MOON_CELL_VISITED || IsTileTaken(x, y))
                continue;
            dist = abs(x - px) + abs(y - py);
            if (bestX < 0 || dist < bestDist)
            {
                bestX = x;
                bestY = y;
                bestDist = dist;
            }
        }
    }
    ClearVisited();
    if (bestX < 0)
        return;

    /* The fields are written directly instead of with
     * MoveObjectEventToMapCoords(): that function also moves the object's
     * sprite, and at this point of a Continue no sprites exist yet (the
     * saved spriteId is stale). The sprite is created later from these
     * coordinates. */
    gSaveBlock1Ptr->pos.x = bestX;
    gSaveBlock1Ptr->pos.y = bestY;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        struct ObjectEvent *player = &gObjectEvents[i];

        if (!player->active || !player->isPlayer)
            continue;
        player->currentCoords.x = bestX + MAP_OFFSET;
        player->currentCoords.y = bestY + MAP_OFFSET;
        player->previousCoords = player->currentCoords;
        player->initialCoords = player->currentCoords;
        player->currentElevation = MapGridGetElevationAt(bestX + MAP_OFFSET, bestY + MAP_OFFSET);
        player->previousElevation = player->currentElevation;
    }
}

bool8 MtMoonGen_IsGeneratedMap(void)
{
    return GetCurrentFloor() != NULL;
}

void MtMoonGen_GenerateOnFreshEntry(void)
{
    const struct MtMoonFloor *floor = GetCurrentFloor();

    if (floor == NULL)
        return;
    /*
     * The quest-log replay ("Previously on your quest...") restores the game
     * variables as they were when each replayed scene was recorded, then
     * loads the scene's map through this fresh-entry path. Keep the restored
     * seed instead of rolling, so the replay shows the cave the scene was
     * recorded in. (The replay's variables are thrown away afterwards: the
     * save is reloaded before the real Continue.)
     */
    if (!QL_IS_PLAYBACK_STATE)
        VarSet(VAR_MT_MOON_MAP_SEED, Random());
    if (GenerateFloor(floor, VarGet(VAR_MT_MOON_MAP_SEED)))
        PutPlayerOnArrivalWarp();
    FreeCaveGrid();
}

void MtMoonGen_RegenerateFromSave(void)
{
    const struct MtMoonFloor *floor = GetCurrentFloor();

    if (floor == NULL)
        return;
    if (GenerateFloor(floor, VarGet(VAR_MT_MOON_MAP_SEED)))
        MovePlayerOntoReachableFloor();
    FreeCaveGrid();
}
