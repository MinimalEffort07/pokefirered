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
 * THE CHAIN
 *
 * Mt. Moon is three generated floors linked in a line:
 *
 *     Route 4 west door <-> 1F <-> B1F <-> B2F <-> Route 4 east door
 *
 * Each floor has exactly two warps the player can use, its "entrance"
 * (toward Route 4 west) and its "exit" (toward Route 4 east); every other
 * vanilla warp of the floor is switched off. Where each warp LEADS is the
 * ordinary warp data in data/maps/<map>/map.json (B2F's warp 1 and Route
 * 4's warp 1 were repointed at each other to close the chain); the
 * generator only moves the two warps onto the generated ladders.
 *
 * SEED AND DETERMINISM
 *
 * Each visit has a 16-bit seed, stored in the game variable
 * VAR_MT_MOON_MAP_SEED (game variables live in the save file). The whole
 * floor (shape, tiles, ladders, NPC positions) is a pure function of that
 * seed:
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
 * generator, the cave grid, the ladder rules, drawing the grid into the
 * map, the warps, NPC placement and the Continue safety net. What a floor
 * LOOKS like (the shape of its cave, where its ladders go and the tile drawn
 * for each cell) comes from that floor's style module; see
 * include/mt_moon_gen_style.h, which also describes the ladder rules.
 *
 * GENERATION STEPS (GenerateFloor)
 *   1. Seed the private random generator.
 *   2. Borrow an all-wall grid the size of the floor's layout, propose a
 *      position for each ladder the floor table doesn't fix
 *      (ProposeLadders), and let the floor's style carve the shape around
 *      the ladders, moving them first if it wants.
 *   3. Check the ladders (CheckLadders). A style that breaks the rules gets
 *      up to 4 tries; after that the floor stays the vanilla map.
 *   4. Draw every map cell with the style's tile picker.
 *   5. Rewrite the floor's warp table: the entrance and exit warps move
 *      onto the generated ladders; every other vanilla warp is parked at
 *      (0,0), a solid tile nobody can stand on.
 *   6. Place the floor's NPCs and item balls on open floor.
 * Then, on a fresh entry, the player is put on the generated tile of the
 * warp they came through (PutPlayerOnArrivalWarp); on Continue, the safety
 * net makes sure the player stands on reachable floor
 * (MovePlayerOntoReachableFloor).
 *
 * LOAD TIME
 *
 * All of this runs inside one call while the screen is black between two
 * warps, so it has to be quick: the test P9 allows at most 30 frames
 * (half a second) more than loading a vanilla cave. The grid is one byte
 * per cell with a frame of wall around it, so reading a cell is a single
 * memory read with no bounds arithmetic, and flood fills use a queue (each
 * cell is visited once) instead of sweeping the grid until nothing changes.
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
#include "constants/vars.h"

/* Warp table entries kept per floor (B1F has the most vanilla warps: 8). */
#define MAX_WARPS 8

/* How many shapes a style gets to produce valid ladders (see CheckLadders). */
#define MAX_SHAPE_ATTEMPTS 4

/*
 * The vanilla ladder tiles, as full map cell values (see the GBA notes):
 * metatile id | collision << 10 | elevation << 12. Walkable tiles are
 * collision 0, elevation 3 (the cave floor's height); solid ones are
 * collision 1, elevation 0.
 */
#define TILE_DOOR            0x3287  /* walkable; standing on it and pressing
                                      * DOWN warps out (a "south arrow warp") */
#define TILE_DOORFRAME_WEST  0x06c8  /* solid; the doorframe below the door */
#define TILE_DOORFRAME_MID   0x06c9
#define TILE_DOORFRAME_EAST  0x06ca
#define TILE_DOOR_BASE       0x06a3  /* solid; the wall below the doorframe */
#define TILE_HOLE            0x3285  /* walkable; stepping on it warps down */
#define TILE_UP_LADDER       0x3296  /* walkable; stepping on it warps up */
#define TILE_UP_LADDER_TOP   0x068f  /* solid; the ladder's top, in the wall */

/* A floor-table coordinate meaning "not fixed: the core proposes one". */
#define NOT_FIXED 0xFF

/* A floor's ladder, as the floor table describes it. */
struct MtMoonFloorLadder
{
    u8 warpId;  /* index in the floor's vanilla warp table (map.json order) */
    u8 kind;    /* MT_MOON_LADDER_* */
    u8 fixedX;  /* fixed position, or NOT_FIXED */
    u8 fixedY;
};

#define LADDER_ANYWHERE(warp, ladderKind) \
    { .warpId = (warp), .kind = (ladderKind), .fixedX = NOT_FIXED, .fixedY = NOT_FIXED }
#define LADDER_AT(warp, ladderKind, x, y) \
    { .warpId = (warp), .kind = (ladderKind), .fixedX = (x), .fixedY = (y) }

/* One generated floor and how it connects to the rest of the game. */
struct MtMoonFloor
{
    u16 layoutId;
    u8 floorNumber;     /* mixed into the seed, so floors get different caves */
    struct MtMoonFloorLadder entrance;  /* toward Route 4 west */
    struct MtMoonFloorLadder exit;      /* toward Route 4 east */
    const struct MtMoonStyle *style;
};

/*
 * The chain, floor by floor. Each warp keeps the destination its map.json
 * gives it, so the warp ids below are picked to make those destinations
 * line up:
 *
 *   Route 4 warp 0 (west door) <-> 1F warp 3 (the door, fixed at vanilla's
 *       (18,37), where the south-edge doorframe is)
 *   1F warp 0 (down-ladder)    <-> B1F warp 0 (up-ladder)
 *   B1F warp 3 (down-ladder)   <-> B2F warp 0 (up-ladder)
 *   B2F warp 1 (up-ladder)     <-> Route 4 warp 1 (east door)
 */
static const struct MtMoonFloor sFloors[] =
{
    {
        .layoutId = LAYOUT_MT_MOON_1F,
        .floorNumber = 1,
        .entrance = LADDER_AT(3, MT_MOON_LADDER_DOOR, 18, 37),
        .exit = LADDER_ANYWHERE(0, MT_MOON_LADDER_DOWN),
        .style = &gMtMoonStyle1F,
    },
    {
        .layoutId = LAYOUT_MT_MOON_B1F,
        .floorNumber = 2,
        .entrance = LADDER_ANYWHERE(0, MT_MOON_LADDER_UP),
        .exit = LADDER_ANYWHERE(3, MT_MOON_LADDER_DOWN),
        .style = &gMtMoonStyleB1F,
    },
    {
        .layoutId = LAYOUT_MT_MOON_B2F,
        .floorNumber = 3,
        .entrance = LADDER_ANYWHERE(0, MT_MOON_LADDER_UP),
        .exit = LADDER_ANYWHERE(1, MT_MOON_LADDER_UP),
        .style = &gMtMoonStyleB2F,
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
 * The cave grid: one byte per map tile, holding one of the MT_MOON_CELL_*
 * values, with a frame of one extra wall cell on every side. Map tile
 * (x, y) is sCaveGrid[(y + 1) * (width + 2) + (x + 1)] (see CELL_INDEX).
 * The frame means the 8 neighbours of any map tile are always inside the
 * array, so hot loops can read them without checking the edges first; the
 * frame is never written, so it always reads as wall.
 *
 * The grid is only needed while a floor is being generated, and EWRAM (the
 * GBA's 256 KB work RAM, where statically allocated game data lives) is
 * completely full. So the grid is borrowed from the game's heap (Alloc,
 * src/malloc.c) for the duration of one generation and freed at the end;
 * only this pointer is permanent. The same block also holds the flood
 * fill's queue (FloodQueue), one u16 cell index per map tile. (A u16 index
 * limits a grid to 65,535 cells including the frame; Mt. Moon's floors
 * have about 2,100.) For the biggest floor (B1F, 49 x 40) the block is
 * about 6 KB of the 112 KB heap.
 * The map loader borrows heap memory the same way when it copies tilesets
 * to video memory.
 */
static EWRAM_DATA u8 *sCaveGrid = NULL;

#define GRID_STRIDE (sCave.width + 2)
#define CELL_INDEX(x, y) (((y) + 1) * GRID_STRIDE + (x) + 1)

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

/*
 * A random number in [0, n) without dividing: the ARM7 CPU has no divide
 * instruction, so `Random() % n` calls a slow library routine (around 100
 * cycles), while scaling the 16-bit random number by n and keeping the top
 * 16 bits is one multiply.
 */
u16 MtMoonGen_RandomBelow(u16 n)
{
    return ((u32)MtMoonGen_Random() * n) >> 16;
}

/* ------------------------------------------------------------------------- */
/* Cave grid                                                                 */
/* ------------------------------------------------------------------------- */

/* Size in bytes of the grid part of the borrowed block, rounded up to an
 * even number so the u16 queue after it is aligned (the CPU can only read
 * a u16 from an even address). */
static s32 GridBytes(void)
{
    return ((sCave.width + 2) * (sCave.height + 2) + 1) & ~1;
}

static u16 *FloodQueue(void)
{
    return (u16 *)(sCaveGrid + GridBytes());
}

/* Borrow an all-wall grid (wall is 0, so zeroed memory) of the given size,
 * plus the flood-fill queue. */
static bool8 AllocCaveGrid(s32 width, s32 height)
{
    sCave.width = width;
    sCave.height = height;
    sCaveGrid = AllocZeroed(GridBytes() + width * height * sizeof(u16));
    return sCaveGrid != NULL;
}

/* Give the grid back. The size goes to 0 too, so any late read sees an
 * empty grid (all wall) instead of following a dangling pointer. */
static void FreeCaveGrid(void)
{
    if (sCaveGrid != NULL)
        Free(sCaveGrid);
    sCaveGrid = NULL;
    sCave.width = 0;
    sCave.height = 0;
}

/* Make every cell wall again (the frame included, which is already wall). */
static void ClearCaveGrid(void)
{
    s32 i;
    s32 count = (sCave.width + 2) * (sCave.height + 2);

    for (i = 0; i < count; i++)
        sCaveGrid[i] = MT_MOON_CELL_WALL;
}

/*
 * TRUE if map tile (x, y) is inside the grid. Casting to u32 makes a
 * negative coordinate a huge number, so one comparison per axis checks
 * both ends. (The size is 0 while no grid is borrowed.) A macro rather than
 * a function: the styles read cells tens of thousands of times per floor,
 * and on the GBA each extra function call costs noticeably in load time.
 */
#define IN_GRID(x, y) ((u32)(x) < (u32)sCave.width && (u32)(y) < (u32)sCave.height)

u8 MtMoonGen_GetCell(s32 x, s32 y)
{
    if (!IN_GRID(x, y))
        return MT_MOON_CELL_WALL;
    return sCaveGrid[CELL_INDEX(x, y)];
}

void MtMoonGen_SetCell(s32 x, s32 y, u8 value)
{
    if (IN_GRID(x, y))
        sCaveGrid[CELL_INDEX(x, y)] = value;
}

bool8 MtMoonGen_IsFloor(s32 x, s32 y)
{
    if (!IN_GRID(x, y))
        return FALSE;
    return sCaveGrid[CELL_INDEX(x, y)] != MT_MOON_CELL_WALL;
}

/* Row y, starting at cell (0, y). Rows -1 and height are the wall frame;
 * anything further out gets the top frame row, which is all wall too. */
const u8 *MtMoonGen_GetRow(s32 y)
{
    if (y < -1 || y > sCave.height)
        y = -1;
    return &sCaveGrid[CELL_INDEX(0, y)];
}

u8 MtMoonGen_GetFloorMask(s32 cx, s32 cy)
{
    const u8 *above, *row, *below;
    s32 stride = GRID_STRIDE;
    u8 mask = 0;

    /* Off the grid, use the checked reads. */
    if (!IN_GRID(cx, cy))
    {
        if (MtMoonGen_IsFloor(cx, cy - 1))     mask |= MT_MOON_N;
        if (MtMoonGen_IsFloor(cx + 1, cy - 1)) mask |= MT_MOON_NE;
        if (MtMoonGen_IsFloor(cx + 1, cy))     mask |= MT_MOON_E;
        if (MtMoonGen_IsFloor(cx + 1, cy + 1)) mask |= MT_MOON_SE;
        if (MtMoonGen_IsFloor(cx, cy + 1))     mask |= MT_MOON_S;
        if (MtMoonGen_IsFloor(cx - 1, cy + 1)) mask |= MT_MOON_SW;
        if (MtMoonGen_IsFloor(cx - 1, cy))     mask |= MT_MOON_W;
        if (MtMoonGen_IsFloor(cx - 1, cy - 1)) mask |= MT_MOON_NW;
        return mask;
    }

    /* Inside the grid all 8 neighbours are in the array (the wall frame
     * covers the edges), so read them directly. */
    row = &sCaveGrid[CELL_INDEX(cx, cy)];
    above = row - stride;
    below = row + stride;
    if (above[0] != MT_MOON_CELL_WALL)  mask |= MT_MOON_N;
    if (above[1] != MT_MOON_CELL_WALL)  mask |= MT_MOON_NE;
    if (row[1] != MT_MOON_CELL_WALL)    mask |= MT_MOON_E;
    if (below[1] != MT_MOON_CELL_WALL)  mask |= MT_MOON_SE;
    if (below[0] != MT_MOON_CELL_WALL)  mask |= MT_MOON_S;
    if (below[-1] != MT_MOON_CELL_WALL) mask |= MT_MOON_SW;
    if (row[-1] != MT_MOON_CELL_WALL)   mask |= MT_MOON_W;
    if (above[-1] != MT_MOON_CELL_WALL) mask |= MT_MOON_NW;
    return mask;
}

u8 MtMoonGen_CountWallNeighbors(s32 cx, s32 cy)
{
    const u8 *above, *row, *below;
    s32 stride = GRID_STRIDE;
    u8 mask, walls;

    /* Off the grid: count the clear bits of the floor mask. */
    if (!IN_GRID(cx, cy))
    {
        mask = MtMoonGen_GetFloorMask(cx, cy);
        walls = 8;
        while (mask != 0)
        {
            mask &= mask - 1;  /* clears the lowest set bit */
            walls--;
        }
        return walls;
    }

    /* Inside the grid, add up the 8 neighbours directly (the wall frame
     * keeps them inside the array). PlaceObjects asks this for hundreds of
     * candidate tiles, so it avoids the mask's branches. */
    row = &sCaveGrid[CELL_INDEX(cx, cy)];
    above = row - stride;
    below = row + stride;
    return (above[-1] == MT_MOON_CELL_WALL) + (above[0] == MT_MOON_CELL_WALL) + (above[1] == MT_MOON_CELL_WALL)
         + (row[-1] == MT_MOON_CELL_WALL)                                     + (row[1] == MT_MOON_CELL_WALL)
         + (below[-1] == MT_MOON_CELL_WALL) + (below[0] == MT_MOON_CELL_WALL) + (below[1] == MT_MOON_CELL_WALL);
}

/*
 * Mark every floor cell connected to (fromX, fromY) as VISITED, with a
 * breadth-first flood fill: start from one cell, and keep a queue of cells
 * whose neighbours still need looking at. Each floor cell is marked when it
 * joins the queue, so it joins only once, and the fill touches each cell a
 * handful of times in total. Cells are queued by their array index; the
 * wall frame means index +-1 (west, east) and +-stride (north, south) are
 * always inside the array.
 *
 * Returns how many cells were marked. The queue still lists exactly those
 * cells afterwards, which is how ClearVisited() finds them again.
 */
static s32 MarkReachableFrom(s32 fromX, s32 fromY)
{
    u16 *queue = FloodQueue();
    s32 stride = GRID_STRIDE;
    s32 head = 0, tail = 0;

    if (MtMoonGen_GetCell(fromX, fromY) != MT_MOON_CELL_FLOOR)
        return 0;
    sCaveGrid[CELL_INDEX(fromX, fromY)] = MT_MOON_CELL_VISITED;
    queue[tail++] = CELL_INDEX(fromX, fromY);
    while (head < tail)
    {
        s32 i = queue[head++];

        if (sCaveGrid[i - 1] == MT_MOON_CELL_FLOOR)
        {
            sCaveGrid[i - 1] = MT_MOON_CELL_VISITED;
            queue[tail++] = i - 1;
        }
        if (sCaveGrid[i + 1] == MT_MOON_CELL_FLOOR)
        {
            sCaveGrid[i + 1] = MT_MOON_CELL_VISITED;
            queue[tail++] = i + 1;
        }
        if (sCaveGrid[i - stride] == MT_MOON_CELL_FLOOR)
        {
            sCaveGrid[i - stride] = MT_MOON_CELL_VISITED;
            queue[tail++] = i - stride;
        }
        if (sCaveGrid[i + stride] == MT_MOON_CELL_FLOOR)
        {
            sCaveGrid[i + stride] = MT_MOON_CELL_VISITED;
            queue[tail++] = i + stride;
        }
    }
    return tail;
}

/* Turn the `count` cells the last MarkReachableFrom() marked VISITED back
 * into plain floor. */
static void ClearVisited(s32 count)
{
    u16 *queue = FloodQueue();
    s32 i;

    for (i = 0; i < count; i++)
        sCaveGrid[queue[i]] = MT_MOON_CELL_FLOOR;
}

bool8 MtMoonGen_FloodFillReaches(s32 fromX, s32 fromY, s32 toX, s32 toY)
{
    bool8 reachable;
    s32 count;

    if (MtMoonGen_GetCell(fromX, fromY) == MT_MOON_CELL_WALL)
        return FALSE;
    count = MarkReachableFrom(fromX, fromY);
    reachable = (MtMoonGen_GetCell(toX, toY) == MT_MOON_CELL_VISITED);
    ClearVisited(count);
    return reachable;
}

/* ------------------------------------------------------------------------- */
/* Ladders                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Everything the core knows about each ladder kind (see LADDERS in
 * mt_moon_gen_style.h) is in one table, sLadderKinds: the kind's footprint,
 * the vanilla tile drawn on each footprint cell, and its foot.
 *
 * A footprint cell is an offset from the ladder cell (dx east, dy south),
 * whether that cell must be floor or wall, and the vanilla map cell value
 * drawn there, or 0 where the style draws its own floor or wall.
 */
struct LadderCell
{
    s8 dx;
    s8 dy;
    u8 cell;   /* MT_MOON_CELL_FLOOR or MT_MOON_CELL_WALL */
    u16 tile;  /* vanilla map cell value, or 0 */
};

struct LadderKind
{
    const struct LadderCell *cells;
    u8 cellCount;
    s8 footDx;  /* the foot, relative to the ladder */
    s8 footDy;
};

/*  . F .      F = floor, W = wall
 *  F D F      D = the door, walkable (0x287)
 *  W W W      the doorframe (0x2c8 0x2c9 0x2ca)
 *  W W W      the wall below it (0x2a3)        */
static const struct LadderCell sDoorCells[] =
{
    { 0, -1, MT_MOON_CELL_FLOOR, 0 },
    { -1, 0, MT_MOON_CELL_FLOOR, 0 },
    { 0, 0, MT_MOON_CELL_FLOOR, TILE_DOOR },
    { 1, 0, MT_MOON_CELL_FLOOR, 0 },
    { -1, 1, MT_MOON_CELL_WALL, TILE_DOORFRAME_WEST },
    { 0, 1, MT_MOON_CELL_WALL, TILE_DOORFRAME_MID },
    { 1, 1, MT_MOON_CELL_WALL, TILE_DOORFRAME_EAST },
    { -1, 2, MT_MOON_CELL_WALL, TILE_DOOR_BASE },
    { 0, 2, MT_MOON_CELL_WALL, TILE_DOOR_BASE },
    { 1, 2, MT_MOON_CELL_WALL, TILE_DOOR_BASE },
};

/*  F F F
 *  F H F      H = the hole (0x285), in open floor
 *  F F F  */
static const struct LadderCell sHoleCells[] =
{
    { -1, -1, MT_MOON_CELL_FLOOR, 0 },
    { 0, -1, MT_MOON_CELL_FLOOR, 0 },
    { 1, -1, MT_MOON_CELL_FLOOR, 0 },
    { -1, 0, MT_MOON_CELL_FLOOR, 0 },
    { 0, 0, MT_MOON_CELL_FLOOR, TILE_HOLE },
    { 1, 0, MT_MOON_CELL_FLOOR, 0 },
    { -1, 1, MT_MOON_CELL_FLOOR, 0 },
    { 0, 1, MT_MOON_CELL_FLOOR, 0 },
    { 1, 1, MT_MOON_CELL_FLOOR, 0 },
};

/*  . W .
 *  W T W      T = the ladder's top (0x28f), solid, set into the wall
 *  . U .      U = the up-ladder (0x296), walkable
 *  . F .  */
static const struct LadderCell sUpLadderCells[] =
{
    { 0, -2, MT_MOON_CELL_WALL, 0 },
    { -1, -1, MT_MOON_CELL_WALL, 0 },
    { 0, -1, MT_MOON_CELL_WALL, TILE_UP_LADDER_TOP },
    { 1, -1, MT_MOON_CELL_WALL, 0 },
    { 0, 0, MT_MOON_CELL_FLOOR, TILE_UP_LADDER },
    { 0, 1, MT_MOON_CELL_FLOOR, 0 },
};

static const struct LadderKind sLadderKinds[] =
{
    /* The foot: north of the door, the hole itself, south of an up-ladder. */
    [MT_MOON_LADDER_DOOR] = { sDoorCells,     ARRAY_COUNT(sDoorCells),     0, -1 },
    [MT_MOON_LADDER_DOWN] = { sHoleCells,     ARRAY_COUNT(sHoleCells),     0,  0 },
    [MT_MOON_LADDER_UP]   = { sUpLadderCells, ARRAY_COUNT(sUpLadderCells), 0,  1 },
};

s32 MtMoonGen_LadderDistance(const struct MtMoonLadder *a, const struct MtMoonLadder *b)
{
    return abs(a->x - b->x) + abs(a->y - b->y);
}

bool8 MtMoonGen_IsLadderSite(const struct MtMoonLadder *ladder)
{
    const struct LadderKind *kind = &sLadderKinds[ladder->kind];
    s32 i;

    if (!IN_GRID(ladder->x, ladder->y))
        return FALSE;
    for (i = 0; i < kind->cellCount; i++)
    {
        const struct LadderCell *cell = &kind->cells[i];
        bool8 floor = MtMoonGen_IsFloor(ladder->x + cell->dx, ladder->y + cell->dy);

        if (floor != (cell->cell == MT_MOON_CELL_FLOOR))
            return FALSE;
    }
    return TRUE;
}

void MtMoonGen_MarkLadderSite(const struct MtMoonLadder *ladder)
{
    const struct LadderKind *kind = &sLadderKinds[ladder->kind];
    s32 i;

    for (i = 0; i < kind->cellCount; i++)
        MtMoonGen_SetCell(ladder->x + kind->cells[i].dx, ladder->y + kind->cells[i].dy, kind->cells[i].cell);
}

void MtMoonGen_GetLadderFoot(const struct MtMoonLadder *ladder, s32 *x, s32 *y)
{
    *x = ladder->x + sLadderKinds[ladder->kind].footDx;
    *y = ladder->y + sLadderKinds[ladder->kind].footDy;
}

/* The vanilla tile `ladder` puts on map tile (x, y), or 0. */
static u16 GetTileOfLadder(const struct MtMoonLadder *ladder, s32 x, s32 y)
{
    const struct LadderKind *kind = &sLadderKinds[ladder->kind];
    s32 dx = x - ladder->x;
    s32 dy = y - ladder->y;
    s32 i;

    for (i = 0; i < kind->cellCount; i++)
        if (kind->cells[i].dx == dx && kind->cells[i].dy == dy)
            return kind->cells[i].tile;
    return 0;
}

/* MtMoonGen_GetLadderTile is asked for every map cell while drawing, so
 * the common answer (far from both ladders) is found without a call: every
 * kind's cells lie within one column and two rows of the ladder. */
#define NEAR_LADDER(ladder, x, y) \
    ((x) >= (ladder).x - 1 && (x) <= (ladder).x + 1 && (y) >= (ladder).y - 2 && (y) <= (ladder).y + 2)

u16 MtMoonGen_GetLadderTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u16 tile = 0;

    if (NEAR_LADDER(cave->entrance, x, y))
        tile = GetTileOfLadder(&cave->entrance, x, y);
    if (tile == 0 && NEAR_LADDER(cave->exit, x, y))
        tile = GetTileOfLadder(&cave->exit, x, y);
    return tile;
}

/* Start a ladder from the floor table: its kind, and its position if the
 * table fixes one (otherwise ProposeLadders gives it one next). */
static void InitLadder(struct MtMoonLadder *ladder, const struct MtMoonFloorLadder *spec)
{
    ladder->kind = spec->kind;
    ladder->fixed = (spec->fixedX != NOT_FIXED);
    ladder->x = spec->fixedX;
    ladder->y = spec->fixedY;
}

/*
 * The core's proposal for a ladder that isn't fixed (rule 1 of the ladder
 * contract in mt_moon_gen_style.h). x is in [4, width - 5]. An up-ladder
 * goes on row 2, right under a 2-row top border, which is its north wall;
 * any other kind gets y in [4, height - 7]. Those margins leave room for a
 * 5 x 5 patch of open floor around the ladder inside a 2-cell border (3 at
 * the bottom).
 */
static void ProposeLadderSpot(struct MtMoonLadder *ladder)
{
    ladder->x = MtMoonGen_RandomBelow(sCave.width - 8) + 4;
    if (ladder->kind == MT_MOON_LADDER_UP)
        ladder->y = 2;
    else
        ladder->y = MtMoonGen_RandomBelow(sCave.height - 10) + 4;
}

/*
 * Propose positions for the ladders that aren't fixed: random spots,
 * redrawn until the two are MT_MOON_LADDER_MIN_DISTANCE apart. That usually
 * takes a few draws (two up-ladders, both on row 2, need 24 columns between
 * them: about 1 draw in 6). The fallback after 200 draws, which practically
 * never runs, puts free ladders in opposite corners, which are far enough
 * apart on every Mt. Moon floor (1F's door is fixed at the bottom middle).
 */
static void ProposeLadders(void)
{
    s32 attempt;

    for (attempt = 0; attempt < 200; attempt++)
    {
        if (!sCave.entrance.fixed)
            ProposeLadderSpot(&sCave.entrance);
        if (!sCave.exit.fixed)
            ProposeLadderSpot(&sCave.exit);
        if (MtMoonGen_LadderDistance(&sCave.entrance, &sCave.exit) >= MT_MOON_LADDER_MIN_DISTANCE)
            return;
    }
    if (!sCave.entrance.fixed)
    {
        sCave.entrance.x = 4;
        sCave.entrance.y = (sCave.entrance.kind == MT_MOON_LADDER_UP) ? 2 : 4;
    }
    if (!sCave.exit.fixed)
    {
        sCave.exit.x = sCave.width - 5;
        sCave.exit.y = (sCave.exit.kind == MT_MOON_LADDER_UP) ? 2 : 4;
    }
}

/* TRUE if `ladder` kept its fixed position (if it has one) and has its
 * footprint in the grid. */
static bool8 IsLadderValid(const struct MtMoonLadder *ladder, const struct MtMoonFloorLadder *spec)
{
    if (spec->fixedX != NOT_FIXED
        && (ladder->x != spec->fixedX || ladder->y != spec->fixedY))
        return FALSE;
    return MtMoonGen_IsLadderSite(ladder);
}

/*
 * Rule 2 of the ladder contract (mt_moon_gen_style.h): both ladders on
 * their footprints, far enough apart, and connected through floor.
 */
static bool8 CheckLadders(const struct MtMoonFloor *floor)
{
    if (!IsLadderValid(&sCave.entrance, &floor->entrance)
        || !IsLadderValid(&sCave.exit, &floor->exit))
        return FALSE;
    if (MtMoonGen_LadderDistance(&sCave.entrance, &sCave.exit) < MT_MOON_LADDER_MIN_DISTANCE)
        return FALSE;
    return MtMoonGen_FloodFillReaches(sCave.entrance.x, sCave.entrance.y, sCave.exit.x, sCave.exit.y);
}

/* ------------------------------------------------------------------------- */
/* Drawing the grid into the map                                             */
/* ------------------------------------------------------------------------- */

/*
 * Write a tile for every cell of the floor's layout, ladders included.
 * Cells outside the grid read as wall, so the style draws them as wall too.
 *
 * The tiles go straight into the map buffer (VMap) a row at a time, instead
 * of through MapGridSetMetatileEntryAt(), which checks the bounds of every
 * single cell. VMap is the map plus a MAP_OFFSET margin on every side
 * (InitMapLayoutData in src/fieldmap.c sizes it so), so map tile (x, y) is
 * always inside it at (x + MAP_OFFSET, y + MAP_OFFSET).
 */
static void DrawCave(const struct MtMoonFloor *floor)
{
    s32 x, y;
    s32 mapW = gMapHeader.mapLayout->width;
    s32 mapH = gMapHeader.mapLayout->height;
    u16 (*pickTile)(const struct MtMoonCave *cave, s32 x, s32 y) = floor->style->pickTile;

    for (y = 0; y < mapH; y++)
    {
        u16 *dest = &VMap.map[(y + MAP_OFFSET) * VMap.Xsize + MAP_OFFSET];

        for (x = 0; x < mapW; x++)
            dest[x] = pickTile(&sCave, x, y);
    }
}

/* ------------------------------------------------------------------------- */
/* Warps                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Copy the floor's vanilla warp table into RAM and rewrite it: the entrance
 * and exit warps move onto the generated ladders, keeping their vanilla
 * destinations (see sFloors), and every other warp is parked at (0,0),
 * which is always solid wall, so it can never be stepped on. Signs and
 * hidden items (the background events) are dropped. gMapHeader.events then
 * points at the copy.
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

    sMtMoonWarps[floor->entrance.warpId].x = sCave.entrance.x;
    sMtMoonWarps[floor->entrance.warpId].y = sCave.entrance.y;
    sMtMoonWarps[floor->exit.warpId].x = sCave.exit.x;
    sMtMoonWarps[floor->exit.warpId].y = sCave.exit.y;

    sMtMoonEvents.warps = sMtMoonWarps;
    sMtMoonEvents.warpCount = count;
    sMtMoonEvents.bgEventCount = 0;
    gMapHeader.events = &sMtMoonEvents;
}

/* ------------------------------------------------------------------------- */
/* NPC placement                                                             */
/* ------------------------------------------------------------------------- */

/* TRUE if every cell an NPC can wander over is floor. (Reads the grid
 * directly: PlaceObjects asks this hundreds of times per object.) */
static bool8 IsMovementAreaClear(s16 cx, s16 cy, s16 rangeX, s16 rangeY)
{
    s32 x, y;

    for (y = cy - rangeY; y <= cy + rangeY; y++)
        for (x = cx - rangeX; x <= cx + rangeX; x++)
            if (!IN_GRID(x, y) || sCaveGrid[CELL_INDEX(x, y)] == MT_MOON_CELL_WALL)
                return FALSE;
    return TRUE;
}

/* TRUE if (x, y) is within 2 tiles (in both x and y) of `ladder`. */
static bool8 IsNearLadder(const struct MtMoonLadder *ladder, s32 x, s32 y)
{
    return x >= ladder->x - 2 && x <= ladder->x + 2 && y >= ladder->y - 2 && y <= ladder->y + 2;
}

/*
 * Give every object template (the starting position of each trainer, NPC
 * and item ball, kept in the save block) a random spot on open floor. Each
 * template starts from the ROM copy, so the result depends only on the seed
 * even on Continue, when the save still holds the previous placement.
 *
 * Only the first two item balls are kept; the rest are parked at (0,0).
 * Objects are kept 3+ tiles from both ladders and each other, need at least
 * 4 floor neighbours, and wanderers need their whole roaming box on floor.
 * An object with no valid spot after 1000 tries is parked at (0,0).
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
             * with a tile to spare. An object that fits nowhere runs all
             * 1000 tries, so a try that lands on wall (most of them) is
             * rejected with one direct grid read; (x, y) is always inside
             * the grid here. */
            x = MtMoonGen_RandomBelow(sCave.width - 8) + 4;
            y = MtMoonGen_RandomBelow(sCave.height - 10) + 5;
            attempts++;

            overlap = FALSE;
            if (sCaveGrid[CELL_INDEX(x, y)] == MT_MOON_CELL_WALL)
            {
                overlap = TRUE;
                continue;
            }
            if (IsNearLadder(&sCave.entrance, x, y) || IsNearLadder(&sCave.exit, x, y))
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
 * the grid or the style never produced valid ladders; the floor then
 * simply stays the vanilla map from the ROM, with its vanilla warps and
 * objects, which is fully playable. (On Continue the player would keep
 * coordinates from the generated cave, which may be wall in the vanilla
 * map, and the safety net can't run without a generated cave. Both are
 * should-never-happen fallbacks: the grid is about 6 KB of a 112 KB heap,
 * and the property test checks the ladders of every style on 100 seeds.)
 */
static bool8 GenerateFloor(const struct MtMoonFloor *floor, u16 seed)
{
    s32 attempt;

    if (!AllocCaveGrid(gMapHeader.mapLayout->width, gMapHeader.mapLayout->height))
        return FALSE;

    SeedCaveRng(seed, floor->floorNumber);
    for (attempt = 0; attempt < MAX_SHAPE_ATTEMPTS; attempt++)
    {
        ClearCaveGrid();
        InitLadder(&sCave.entrance, &floor->entrance);
        InitLadder(&sCave.exit, &floor->exit);
        ProposeLadders();
        floor->style->buildShape(&sCave);
        if (CheckLadders(floor))
            break;
    }
    if (attempt == MAX_SHAPE_ATTEMPTS)
        return FALSE;

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
    s32 marked = MarkReachableFrom(sCave.entrance.x, sCave.entrance.y);

    if (MtMoonGen_GetCell(px, py) == MT_MOON_CELL_VISITED)
    {
        ClearVisited(marked);
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
    ClearVisited(marked);
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
