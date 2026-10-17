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
 *   3. Check the ladders (CheckLadders).
 *   4. Fill pockets (FillPockets): floor the player can't walk to becomes
 *      wall, so 100% of the floor is reachable.
 *   5. Rewrite the floor's event tables (SetUpEvents): the entrance and exit
 *      warps move onto the generated ladders, every other vanilla warp is
 *      parked at (0,0), a solid tile nobody can stand on, and the hidden
 *      items and signs get a RAM copy whose positions step 6 fills in.
 *   6. Place ALL of the floor's vanilla content (PlaceContent): trainers,
 *      NPCs and item balls on open floor, hidden items on a stamped
 *      boulder and signs on a stamped signpost. Every spot keeps the floor
 *      connected and leaves a tile next to the piece for the player to
 *      stand on, facing it, carving open floor where nothing fits; a final
 *      flood fill checks the result (IsContentReachable). See CONTENT
 *      PLACEMENT below.
 *   If step 3 or 6 fails, the shape is thrown away and the style builds a
 *   new one (back to step 2), up to 4 times; after that the floor stays
 *   the vanilla map. So nothing is ever dropped.
 *   7. Turn each stationary trainer toward its longest straight run of
 *      open floor (FaceStationaryTrainers).
 *   8. Draw every map cell with the style's tile picker, then the stamped
 *      one-tile prefabs on top.
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
#include "constants/event_bg.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/layouts.h"
#include "constants/trainer_types.h"
#include "constants/vars.h"

/* Warp table entries kept per floor (B1F has the most vanilla warps: 8). */
#define MAX_WARPS 8

/* Hidden items and signs kept per floor (B1F has the most: 6 hidden
 * items). Each costs 12 bytes of EWRAM (sMtMoonBgEvents, a static array, so
 * the 72 bytes are reserved for good). */
#define MAX_BG_EVENTS 6

/* One-tile prefabs per floor (see MtMoonGen_StampTile). They are kept in
 * the heap block borrowed for the grid, 4 bytes each, so room for many
 * costs nothing once generation is over. */
#define MAX_STAMPS 128

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

/* The one-tile prefabs for background events, exactly the vanilla cells:
 * every vanilla Mt. Moon hidden item sits on this lone boulder, and 1F's
 * Zubat sign is this signpost. Both solid, elevation 0. */
#define TILE_BOULDER         0x0682
#define TILE_SIGNPOST        0x06cd

/* TRUE if a map cell value can't be walked on (collision bits not 0). */
#define IS_SOLID_TILE(tile) (((tile) & MAPGRID_COLLISION_MASK) != 0)

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
 * fill's queue (FloodQueue), one u16 cell index per map tile, and the work
 * areas of the later steps (struct GenerationWork: the stamped one-tile
 * prefabs and the content placement's bookkeeping). (A u16 index limits a
 * grid to 65,535 cells including the frame; Mt. Moon's floors have about
 * 2,100.) For the biggest floor (B1F, 49 x 40) the block is about 7 KB of
 * the 112 KB heap.
 * The map loader borrows heap memory the same way when it copies tilesets
 * to video memory.
 */
static EWRAM_DATA u8 *sCaveGrid = NULL;

#define GRID_STRIDE (sCave.width + 2)
#define CELL_INDEX(x, y) (((y) + 1) * GRID_STRIDE + (x) + 1)

/* Row y of the grid, so that GRID_ROW(y)[x] is cell (x, y): the core's own,
 * call-free version of MtMoonGen_GetRow for its hot loops. Only for y from
 * -1 to height (the wall frame included). */
#define GRID_ROW(y) (&sCaveGrid[CELL_INDEX(0, (y))])

/* The rewritten event tables. gMapHeader.events points here while a
 * generated floor is loaded, instead of at the read-only ROM tables. The
 * game reads them every time the player steps on a warp or presses A in
 * front of a hidden item or sign, so they must last as long as the floor
 * is loaded (unlike the grid). They are rebuilt from the seed on Continue.
 * (sMtMoonBgEvents' 72 bytes come out of the ~480 bytes of EWRAM that
 * moving the grid to the heap freed.) */
static EWRAM_DATA struct WarpEvent sMtMoonWarps[MAX_WARPS] = {0};
static EWRAM_DATA struct BgEvent sMtMoonBgEvents[MAX_BG_EVENTS] = {0};
static EWRAM_DATA struct MapEvents sMtMoonEvents = {0};

/* One stamped one-tile prefab: map tile (x, y) is drawn as `tile`. */
struct MtMoonStamp
{
    u8 x;
    u8 y;
    u16 tile;
};

/* The stamps of the floor being generated, in the order they were made. */
struct StampList
{
    u16 count;
    struct MtMoonStamp stamps[MAX_STAMPS];
};

/* A rectangle of map tiles, corners included. */
struct Box
{
    s16 x1;
    s16 y1;
    s16 x2;
    s16 y2;
};

/* At most 64 objects (the save block's template array) and the bg events:
 * every PIECE of content (see CONTENT PLACEMENT). */
#define MAX_PIECES (OBJECT_EVENT_TEMPLATES_COUNT + MAX_BG_EVENTS)

/* What content placement keeps track of (see CONTENT PLACEMENT). */
struct Placement
{
    /* The candidate tiles, packed as x | (y << 8) (both are under 256).
     * They live in the flood-fill queue's memory: no flood fill runs while
     * content is being placed. */
    u16 *candidates;
    s32 candidateCount;
    /* The boxes of every piece placed so far (rule 3). */
    s32 count;
    struct Box boxes[MAX_PIECES];
};

/*
 * The fixed-size work areas of one generation, kept in the borrowed heap
 * block after the grid and the flood queue (about 1.1 KB). They are too big
 * for the stack: the CPU's stack lives in IWRAM, between the last IWRAM
 * variable and IWRAM_END - 0x1C0, which leaves it under 2.5 KB for the
 * whole chain of calls that loads a map.
 */
struct GenerationWork
{
    struct StampList stamps;
    struct Placement placement;
};

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

/*
 * The borrowed block is laid out as
 *     [grid] [flood queue] [struct GenerationWork]
 * with each part's size rounded up to a multiple of 4 bytes, so every part
 * starts on an address the CPU can read it from (a u16 needs an even
 * address; a u32 or a pointer, a multiple of 4). The heap hands out
 * blocks that start on a multiple of 4.
 */
static s32 GridBytes(void)
{
    return ((sCave.width + 2) * (sCave.height + 2) + 3) & ~3;
}

static s32 QueueBytes(void)
{
    return (sCave.width * sCave.height * sizeof(u16) + 3) & ~3;
}

static u16 *FloodQueue(void)
{
    return (u16 *)(sCaveGrid + GridBytes());
}

static struct GenerationWork *Work(void)
{
    return (struct GenerationWork *)(sCaveGrid + GridBytes() + QueueBytes());
}

static struct StampList *Stamps(void)
{
    return &Work()->stamps;
}

/* Borrow an all-wall grid (wall is 0, so zeroed memory) of the given size,
 * plus the flood-fill queue and the work areas (an empty stamp list). */
static bool8 AllocCaveGrid(s32 width, s32 height)
{
    sCave.width = width;
    sCave.height = height;
    sCaveGrid = AllocZeroed(GridBytes() + QueueBytes() + sizeof(struct GenerationWork));
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

/* Make every cell wall again (the frame included, which is already wall),
 * and forget any stamps a thrown-away shape made. */
static void ClearCaveGrid(void)
{
    s32 i;
    s32 count = (sCave.width + 2) * (sCave.height + 2);

    for (i = 0; i < count; i++)
        sCaveGrid[i] = MT_MOON_CELL_WALL;
    Stamps()->count = 0;
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
     * keeps them inside the array). Style passes ask this for many cells,
     * so it avoids the mask's branches. */
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
 * WHAT THE PLAYER CAN WALK TO
 *
 * The player can step ONTO a ladder but never walk THROUGH one: stepping on
 * it leaves the floor. So the flood fills that decide what the player can
 * reach treat both ladder cells as solid while they run.
 *
 * MarkWalkableFromEntrance marks VISITED every floor cell the player can
 * walk to from the entrance without stepping on a ladder. While it runs,
 * the two ladder cells are BLOCKED (MarkReachableFrom only spreads into
 * FLOOR cells). It returns the count ClearWalkableMarks needs to undo all
 * of it, ladder cells included.
 */

/* TRUE for the floor cells of a ladder's footprint other than the ladder
 * itself. */
static bool8 IsFootprintFloor(const struct LadderCell *cell)
{
    return cell->cell == MT_MOON_CELL_FLOOR && (cell->dx != 0 || cell->dy != 0);
}

static s32 MarkWalkableFromEntrance(void)
{
    const struct LadderKind *kind = &sLadderKinds[sCave.entrance.kind];
    s32 i;

    sCaveGrid[CELL_INDEX(sCave.entrance.x, sCave.entrance.y)] = MT_MOON_CELL_BLOCKED;
    sCaveGrid[CELL_INDEX(sCave.exit.x, sCave.exit.y)] = MT_MOON_CELL_BLOCKED;
    /* Start where the player stands right after arriving, in effect: the
     * first floor cell of the entrance's footprint that isn't the ladder
     * (north of the door, north-west of a hole, south of an up-ladder). */
    for (i = 0; i < kind->cellCount; i++)
    {
        if (IsFootprintFloor(&kind->cells[i]))
            return MarkReachableFrom(sCave.entrance.x + kind->cells[i].dx, sCave.entrance.y + kind->cells[i].dy);
    }
    return 0;
}

static void ClearWalkableMarks(s32 marked)
{
    ClearVisited(marked);
    sCaveGrid[CELL_INDEX(sCave.entrance.x, sCave.entrance.y)] = MT_MOON_CELL_FLOOR;
    sCaveGrid[CELL_INDEX(sCave.exit.x, sCave.exit.y)] = MT_MOON_CELL_FLOOR;
}

/* TRUE if every floor cell of `ladder`'s footprint, apart from the ladder
 * itself, was reached by the last flood fill. */
static bool8 IsFootprintVisited(const struct MtMoonLadder *ladder)
{
    const struct LadderKind *kind = &sLadderKinds[ladder->kind];
    s32 i;

    for (i = 0; i < kind->cellCount; i++)
    {
        const struct LadderCell *cell = &kind->cells[i];

        if (IsFootprintFloor(cell)
            && MtMoonGen_GetCell(ladder->x + cell->dx, ladder->y + cell->dy) != MT_MOON_CELL_VISITED)
            return FALSE;
    }
    return TRUE;
}

/*
 * Rule 2 of the ladder contract (mt_moon_gen_style.h): both ladders on
 * their footprints, far enough apart, and every floor cell of both
 * footprints connected to the others without stepping on a ladder.
 */
static bool8 CheckLadders(const struct MtMoonFloor *floor)
{
    s32 marked;
    bool8 connected;

    if (!IsLadderValid(&sCave.entrance, &floor->entrance)
        || !IsLadderValid(&sCave.exit, &floor->exit))
        return FALSE;
    if (MtMoonGen_LadderDistance(&sCave.entrance, &sCave.exit) < MT_MOON_LADDER_MIN_DISTANCE)
        return FALSE;

    marked = MarkWalkableFromEntrance();
    connected = IsFootprintVisited(&sCave.entrance) && IsFootprintVisited(&sCave.exit);
    ClearWalkableMarks(marked);
    return connected;
}

/* ------------------------------------------------------------------------- */
/* Pockets                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Turn every floor cell the player can't walk to into wall, so that 100%
 * of the floor is reachable (test P3). CheckLadders has already made sure
 * both ladders' footprints can be walked to.
 *
 * After this, the floor without the two ladder cells is one connected
 * piece, and every floor cell next to a ladder belongs to it. Content
 * placement (PlaceContent) keeps it that way.
 */
static void FillPockets(void)
{
    s32 i;
    s32 count = (sCave.width + 2) * (sCave.height + 2);
    s32 marked = MarkWalkableFromEntrance();

    /* The reached cells are VISITED now, so the FLOOR ones left are the
     * pockets. (The wall frame is wall, so the loop can run over it.) */
    for (i = 0; i < count; i++)
    {
        if (sCaveGrid[i] == MT_MOON_CELL_FLOOR)
            sCaveGrid[i] = MT_MOON_CELL_WALL;
    }
    ClearWalkableMarks(marked);
}

/* ------------------------------------------------------------------------- */
/* One-tile prefabs                                                          */
/* ------------------------------------------------------------------------- */

bool8 MtMoonGen_StampTile(s32 x, s32 y, u16 tile)
{
    struct StampList *list = Stamps();
    u8 *cell;

    if (!IN_GRID(x, y) || list->count >= MAX_STAMPS)
        return FALSE;
    list->stamps[list->count].x = x;
    list->stamps[list->count].y = y;
    list->stamps[list->count].tile = tile;
    list->count++;

    /* The grid follows the tile, so reachability matches what is drawn. */
    cell = &sCaveGrid[CELL_INDEX(x, y)];
    if (!IS_SOLID_TILE(tile))
        *cell = MT_MOON_CELL_FLOOR;
    else if (*cell != MT_MOON_CELL_WALL)
        *cell = MT_MOON_CELL_BLOCKED;
    return TRUE;
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
 *
 * The stamped one-tile prefabs (MtMoonGen_StampTile) are written last, on
 * top of whatever the style drew there.
 */
static void DrawCave(const struct MtMoonFloor *floor)
{
    s32 x, y, i;
    s32 mapW = gMapHeader.mapLayout->width;
    s32 mapH = gMapHeader.mapLayout->height;
    u16 (*pickTile)(const struct MtMoonCave *cave, s32 x, s32 y) = floor->style->pickTile;
    const struct StampList *stamps = Stamps();

    for (y = 0; y < mapH; y++)
    {
        u16 *dest = &VMap.map[(y + MAP_OFFSET) * VMap.Xsize + MAP_OFFSET];

        for (x = 0; x < mapW; x++)
            dest[x] = pickTile(&sCave, x, y);
    }
    for (i = 0; i < stamps->count; i++)
    {
        const struct MtMoonStamp *stamp = &stamps->stamps[i];

        VMap.map[(stamp->y + MAP_OFFSET) * VMap.Xsize + stamp->x + MAP_OFFSET] = stamp->tile;
    }
}

/* ------------------------------------------------------------------------- */
/* Event tables                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Copy the floor's vanilla event tables (`rom`) into RAM and point
 * gMapHeader.events at the copy:
 *   - warps: the entrance and exit warps move onto the generated ladders,
 *     keeping their vanilla destinations (see sFloors); every other warp is
 *     parked at (0,0), which is always solid wall, so it can never fire;
 *   - hidden items and signs: copied whole (which item, which flag, which
 *     script); PlaceContent then gives each one its generated position;
 *   - objects and coordinate triggers keep pointing at the ROM. Objects
 *     get their generated positions in the save block's templates instead
 *     (PlaceContent), and B2F's one trigger, Miguel's, stays at its vanilla
 *     tile (14,11) until the fossil chamber is built around it.
 */
static void SetUpEvents(const struct MtMoonFloor *floor, const struct MapEvents *rom)
{
    s32 i;
    s32 warpCount = rom->warpCount;
    s32 bgCount = rom->bgEventCount;

    /* Never true on Mt. Moon (B1F has the most of both); just a guard
     * against overrunning the RAM tables if the maps ever change. */
    if (warpCount > MAX_WARPS)
        warpCount = MAX_WARPS;
    if (bgCount > MAX_BG_EVENTS)
        bgCount = MAX_BG_EVENTS;

    sMtMoonEvents = *rom;
    for (i = 0; i < warpCount; i++)
    {
        sMtMoonWarps[i] = rom->warps[i];
        sMtMoonWarps[i].x = 0;
        sMtMoonWarps[i].y = 0;
    }
    sMtMoonWarps[floor->entrance.warpId].x = sCave.entrance.x;
    sMtMoonWarps[floor->entrance.warpId].y = sCave.entrance.y;
    sMtMoonWarps[floor->exit.warpId].x = sCave.exit.x;
    sMtMoonWarps[floor->exit.warpId].y = sCave.exit.y;
    for (i = 0; i < bgCount; i++)
        sMtMoonBgEvents[i] = rom->bgEvents[i];

    sMtMoonEvents.warps = sMtMoonWarps;
    sMtMoonEvents.warpCount = warpCount;
    sMtMoonEvents.bgEvents = sMtMoonBgEvents;
    sMtMoonEvents.bgEventCount = bgCount;
    gMapHeader.events = &sMtMoonEvents;
}

/* ------------------------------------------------------------------------- */
/* Content placement                                                         */
/* ------------------------------------------------------------------------- */

/*
 * CONTENT PLACEMENT
 *
 * Every vanilla object (trainers, NPCs, item balls) and background event
 * (hidden items, signs) of the floor gets a spot on the generated floor;
 * nothing is dropped. Each of them is a PIECE of content here (not to be
 * confused with the game's items). Afterwards the player, arriving by
 * either ladder, must be able to (tests P2 and P3):
 *   - walk to the other ladder;
 *   - stand next to every piece, facing it;
 *   - walk to every floor tile that no object can stand on.
 * Objects are solid, and a defeated trainer never moves out of the way, so
 * an object in the only gap between two parts of the cave would cut one of
 * them off for good. An object that moves (a wanderer) could be on any tile
 * of its roaming box, so the whole box counts as taken.
 *
 * Each piece covers a BOX of tiles: an object's roaming box (GetPieceBox),
 * or the single tile of a hidden item or sign. A spot is good (IsGoodSpot)
 * when:
 *   1. every tile of the box is free floor, so a mover can roam all of it;
 *   2. the box is at least 3 tiles from both ladders, so nothing crowds the
 *      arrival tiles or stands in a ladder's footprint;
 *   3. the box is at least 2 tiles from every box placed before it, so a
 *      free tile separates any two pieces and no piece can take the tile
 *      next to another (the tile the player faces it from);
 *   4. covering the box keeps the floor in one piece. This is checked on
 *      the RING of tiles around the box (its 8 neighbours, for a 1-tile
 *      box). Going around the ring, each tile shares a side with the next,
 *      so a run of consecutive free ring tiles is a path. If all the free
 *      ring tiles that share a side with the box lie in ONE such run, any
 *      walk that crossed the box can go around it along the ring instead,
 *      so covering the box cuts nothing off. At least one such tile must
 *      exist: it is where the player stands to face the piece.
 *      Hidden items and signs are stricter: their whole ring must be free
 *      floor, so the boulder or signpost stands alone in open floor (as
 *      vanilla's sign does).
 * Since every placement keeps the floor in one piece (rule 4) and no later
 * piece can take a tile next to an earlier one (rule 3), every rule above
 * still holds once the last piece is placed. IsContentReachable then checks
 * the result with one flood fill anyway.
 *
 * Finding a spot (FindSpot) looks at the CANDIDATES (the tiles that were
 * free floor when placement started, plus any floor carved since), trying
 * each way in turn until one works:
 *   a. random candidates;
 *   b. every candidate in turn, starting from a random one;
 *   c. REPAIR: pick a candidate where the box and its ring could be carved
 *      out (CanCarveAround) and turn their wall tiles into floor. The box
 *      and its ring are then all free floor, so rules 1 and 4 hold. Floor
 *      is only ever added, next to floor that is already connected, so no
 *      pocket appears and no ladder footprint is touched;
 *   d. REPAIR, on a cramped floor where every candidate is too close to a
 *      placed piece or a ladder: the same carving, anchored on ANY tile
 *      (wall too) whose box and ring hold some free floor, which joins the
 *      carved area to the cave (CarveAnywhere).
 * If no way works for some piece, or the final check fails, the whole
 * shape is thrown away and the style builds a new one (GenerateFloor), the
 * same as for a shape with broken ladders; after 4 shapes the floor stays
 * the vanilla map, where every piece is reachable as vanilla placed it.
 * Big boxes are the hardest to fit, so movers go first, then the hidden
 * items and signs, then everything else.
 *
 * While placement runs, the tiles an object covers are marked OCCUPIED in
 * the grid, so the rules see them as taken. They stay marked until the
 * trainers have been turned (FaceStationaryTrainers: objects block sight
 * lines), then become floor again (ReleaseObjectTiles), because the
 * Continue safety net needs the real terrain. Hidden items and signs stay
 * BLOCKED: their boulder and signpost tiles are solid.
 *
 * LOAD TIME: on a cramped floor many pieces need ways b and c, which look
 * at every candidate, so those loops are kept lean: they never look at a
 * wall tile, rule checks read the grid directly, and the cheapest rules
 * run first.
 */

/* An object's tile while content is placed: not FLOOR, so flood fills and
 * the rules above treat it as taken, and not WALL. Gone before the floor is
 * drawn, so a style never sees it. */
#define CELL_OCCUPIED 4

#define LADDER_CLEARANCE 3   /* rule 2 */
#define PIECE_SPACING    2   /* rule 3 */
#define RANDOM_TRIES     64  /* way a */

/* The biggest ring: around a 31 x 31 box, the largest a movement range
 * (4 bits, so at most 15 tiles each way) allows. ReadRing's buffer is this
 * many bytes of stack. */
#define MAX_RING_TILES (4 * 31 + 4)

/* What a piece needs: its movement (which sets its box) and whether its
 * whole ring must be open floor. */
struct PieceShape
{
    u8 movementType;
    u8 rangeX;
    u8 rangeY;
    bool8 needOpenRing;
};

enum
{
    SPOT_FITS,   /* ways a and b: the spot already follows every rule */
    SPOT_CARVE,  /* way c: the spot follows them once it is carved out */
};

/* TRUE for the movement types that walk around inside their movement range
 * (src/event_object_movement.c refuses any step that would leave it). */
static bool8 IsMovingType(u8 movementType)
{
    switch (movementType)
    {
    case MOVEMENT_TYPE_WANDER_AROUND:
    case MOVEMENT_TYPE_WANDER_UP_AND_DOWN:
    case MOVEMENT_TYPE_WANDER_DOWN_AND_UP:
    case MOVEMENT_TYPE_WANDER_LEFT_AND_RIGHT:
    case MOVEMENT_TYPE_WANDER_RIGHT_AND_LEFT:
    case MOVEMENT_TYPE_WANDER_AROUND_SLOWER:
    case MOVEMENT_TYPE_COPY_PLAYER_IN_GRASS:
    case MOVEMENT_TYPE_COPY_PLAYER_OPPOSITE_IN_GRASS:
    case MOVEMENT_TYPE_COPY_PLAYER_COUNTERCLOCKWISE_IN_GRASS:
    case MOVEMENT_TYPE_COPY_PLAYER_CLOCKWISE_IN_GRASS:
        return TRUE;
    }
    /* The walkers, every walk sequence and the plain copy-player types are
     * numbered as one block. */
    return movementType >= MOVEMENT_TYPE_WALK_UP_AND_DOWN
        && movementType <= MOVEMENT_TYPE_COPY_PLAYER_CLOCKWISE;
}

/*
 * The box of tiles a piece placed at (x, y) can be on:
 *   - a wanderer, walker or copier: anywhere within its movement range,
 *     [x - rangeX, x + rangeX] x [y - rangeY, y + rangeY];
 *   - MOVEMENT_TYPE_WALK_SEQUENCE_LEFT_DOWN_RIGHT_UP (1F's Lass Miriam):
 *     she walks west to x - rangeX, south to y + rangeY, east back to x and
 *     north back to y (MovementType_WalkSequenceLeftDownRightUp_Step1), so
 *     her box is [x - rangeX, x] x [y, y + rangeY];
 *   - anything else stands still: just (x, y).
 */
static void GetPieceBox(const struct PieceShape *shape, s32 x, s32 y, struct Box *box)
{
    s32 rx = shape->rangeX;
    s32 ry = shape->rangeY;

    box->x1 = x;
    box->y1 = y;
    box->x2 = x;
    box->y2 = y;
    if (shape->movementType == MOVEMENT_TYPE_WALK_SEQUENCE_LEFT_DOWN_RIGHT_UP)
    {
        box->x1 = x - rx;
        box->y2 = y + ry;
    }
    else if (IsMovingType(shape->movementType))
    {
        box->x1 = x - rx;
        box->y1 = y - ry;
        box->x2 = x + rx;
        box->y2 = y + ry;
    }
}

/*
 * TRUE if two boxes are fewer than `gap` king moves apart (steps in any of
 * the 8 directions; 0 when they overlap). The gap between the boxes along x
 * is the larger of (b.x1 - a.x2) and (a.x1 - b.x2), and they are closer
 * than `gap` exactly when the gaps along both x and y are.
 */
#define BOXES_CLOSER_THAN(a, b, gap)                         \
    ((b)->x1 - (a)->x2 < (gap) && (a)->x1 - (b)->x2 < (gap)  \
  && (b)->y1 - (a)->y2 < (gap) && (a)->y1 - (b)->y2 < (gap))

static bool8 IsNearLadder(const struct Box *box, const struct MtMoonLadder *ladder, s32 clearance)
{
    struct Box tile;

    tile.x1 = tile.x2 = ladder->x;
    tile.y1 = tile.y2 = ladder->y;
    return BOXES_CLOSER_THAN(box, &tile, clearance);
}

/* Rules 2 and 3, with the given clearance from the ladders. */
static bool8 IsClearOfLaddersAndPieces(const struct Box *box, s32 ladderClearance, const struct Placement *placement)
{
    s32 i;

    if (IsNearLadder(box, &sCave.entrance, ladderClearance) || IsNearLadder(box, &sCave.exit, ladderClearance))
        return FALSE;
    for (i = 0; i < placement->count; i++)
    {
        if (BOXES_CLOSER_THAN(box, &placement->boxes[i], PIECE_SPACING))
            return FALSE;
    }
    return TRUE;
}

/* Rule 1: the box is free floor. It must also leave room for its ring
 * inside the grid (rule 4 reads the ring). */
static bool8 IsBoxFreeFloor(const struct Box *box)
{
    s32 x, y;

    if (box->x1 < 1 || box->y1 < 1 || box->x2 > sCave.width - 2 || box->y2 > sCave.height - 2)
        return FALSE;
    for (y = box->y1; y <= box->y2; y++)
    {
        const u8 *row = GRID_ROW(y);

        for (x = box->x1; x <= box->x2; x++)
        {
            if (row[x] != MT_MOON_CELL_FLOOR)
                return FALSE;
        }
    }
    return TRUE;
}

/* What ReadRing records for each ring tile. */
#define RING_FREE    (1 << 0)  /* free floor */
#define RING_TOUCHES (1 << 1)  /* shares a side with the box (not a corner) */

/*
 * Read the ring around `box` (rule 4) into ring[], going clockwise from the
 * tile diagonally north-west of the box: along the north side (both corners
 * included), down the east side, back along the south side (both corners)
 * and up the west side. Each tile shares a side with the next, and the last
 * with the first. The 4 corner tiles touch the box only diagonally (the
 * player can't face the box from them). Returns the number of ring tiles.
 * The box must pass IsBoxFreeFloor first, which keeps the ring on the grid.
 */
static s32 ReadRing(const struct Box *box, u8 *ring)
{
    s32 n = 0, x, y;
    const u8 *row;

    row = GRID_ROW(box->y1 - 1);
    for (x = box->x1 - 1; x <= box->x2 + 1; x++)
        ring[n++] = (row[x] == MT_MOON_CELL_FLOOR ? RING_FREE : 0) | (x >= box->x1 && x <= box->x2 ? RING_TOUCHES : 0);
    for (y = box->y1; y <= box->y2; y++)
        ring[n++] = (GRID_ROW(y)[box->x2 + 1] == MT_MOON_CELL_FLOOR ? RING_FREE : 0) | RING_TOUCHES;
    row = GRID_ROW(box->y2 + 1);
    for (x = box->x2 + 1; x >= box->x1 - 1; x--)
        ring[n++] = (row[x] == MT_MOON_CELL_FLOOR ? RING_FREE : 0) | (x >= box->x1 && x <= box->x2 ? RING_TOUCHES : 0);
    for (y = box->y2; y >= box->y1; y--)
        ring[n++] = (GRID_ROW(y)[box->x1 - 1] == MT_MOON_CELL_FLOOR ? RING_FREE : 0) | RING_TOUCHES;
    return n;
}

/* Rule 4 (see CONTENT PLACEMENT). */
static bool8 KeepsFloorConnected(const struct Box *box, bool8 needOpenRing)
{
    u8 ring[MAX_RING_TILES];
    s32 length = ReadRing(box, ring);
    s32 i, k;
    s32 start = -1;
    s32 runs = 0;
    bool8 inRun = FALSE, runTouches = FALSE;

    for (k = 0; k < length; k++)
    {
        if (!(ring[k] & RING_FREE))
        {
            if (needOpenRing)
                return FALSE;
            if (start < 0)
                start = k;
        }
    }
    if (start < 0)
        return TRUE; /* the whole ring is free floor */

    /* Go once around the ring, starting just after a taken tile so no run
     * is split in two, and count the runs of free tiles that share a side
     * with the box. The last tile looked at is `start` again (taken), which
     * ends the final run. */
    for (i = 1; i <= length; i++)
    {
        k = start + i;
        if (k >= length)
            k -= length;
        if (ring[k] & RING_FREE)
        {
            if (!inRun)
            {
                inRun = TRUE;
                runTouches = FALSE;
            }
            if (ring[k] & RING_TOUCHES)
                runTouches = TRUE;
        }
        else if (inRun)
        {
            if (runTouches)
                runs++;
            inRun = FALSE;
        }
    }
    return runs == 1;
}

/* All four rules, cheapest first. */
static bool8 IsGoodSpot(const struct Box *box, const struct PieceShape *shape, const struct Placement *placement)
{
    return IsBoxFreeFloor(box)
        && IsClearOfLaddersAndPieces(box, LADDER_CLEARANCE, placement)
        && KeepsFloorConnected(box, shape->needOpenRing);
}

/*
 * Ways c and d (repair): can the box and its ring be carved into open
 * floor? They must stay inside a 2-tile border (3 at the bottom, the border
 * every style keeps solid), the ring must stay 3+ tiles from the ladders
 * (their footprints reach 2 tiles out), the usual spacing from placed
 * pieces must hold, nothing stamped or occupied may be inside, and at
 * least one tile inside must already be free floor: that tile joins the
 * carved area to the rest of the cave (all free floor is connected).
 */
static bool8 CanCarveAround(const struct Box *box, const struct Placement *placement)
{
    s32 x, y;
    bool8 touchesFloor = FALSE;

    if (box->x1 - 1 < 2 || box->y1 - 1 < 2
        || box->x2 + 1 > sCave.width - 3 || box->y2 + 1 > sCave.height - 4)
        return FALSE;
    /* The area first: way d asks this for every map tile, and most of them
     * are deep in rock, with no floor in reach. */
    for (y = box->y1 - 1; y <= box->y2 + 1; y++)
    {
        const u8 *row = GRID_ROW(y);

        for (x = box->x1 - 1; x <= box->x2 + 1; x++)
        {
            if (row[x] == MT_MOON_CELL_FLOOR)
                touchesFloor = TRUE;
            else if (row[x] != MT_MOON_CELL_WALL)
                return FALSE;
        }
    }
    return touchesFloor && IsClearOfLaddersAndPieces(box, LADDER_CLEARANCE + 1, placement);
}

/* Turn the wall tiles of the box and its ring into floor, and add them to
 * the candidates. (The candidate list has room for every map tile, and a
 * carved tile was wall, so it was never listed before.) */
static void CarveAround(const struct Box *box, struct Placement *placement)
{
    s32 x, y;

    for (y = box->y1 - 1; y <= box->y2 + 1; y++)
    {
        for (x = box->x1 - 1; x <= box->x2 + 1; x++)
        {
            if (sCaveGrid[CELL_INDEX(x, y)] == MT_MOON_CELL_WALL)
            {
                sCaveGrid[CELL_INDEX(x, y)] = MT_MOON_CELL_FLOOR;
                placement->candidates[placement->candidateCount++] = x | (y << 8);
            }
        }
    }
}

/* Can the piece go on candidate `packed` (x | y << 8) the way `mode`
 * allows? The tile must still be free floor (for SPOT_CARVE that is what
 * connects the carved area to the rest of the cave). Changes nothing. */
static bool8 IsUsableSpot(const struct PieceShape *shape, const struct Placement *placement, u16 packed, u8 mode)
{
    struct Box box;
    s32 x = packed & 0xFF;
    s32 y = packed >> 8;

    if (sCaveGrid[CELL_INDEX(x, y)] != MT_MOON_CELL_FLOOR)
        return FALSE;
    GetPieceBox(shape, x, y, &box);
    if (mode == SPOT_FITS)
        return IsGoodSpot(&box, shape, placement);
    return CanCarveAround(&box, placement);
}

/* Way d: carve out a spot anchored on any tile, wall or floor, whose box
 * and ring hold some free floor (CanCarveAround). Every map tile is looked
 * at once, in reading order from a random one. */
static bool8 CarveAnywhere(const struct PieceShape *shape, struct Placement *placement, s32 *outX, s32 *outY)
{
    s32 width = sCave.width;
    s32 height = sCave.height;
    s32 x = MtMoonGen_RandomBelow(width);
    s32 y = MtMoonGen_RandomBelow(height);
    s32 i;
    struct Box box;

    for (i = 0; i < width * height; i++)
    {
        GetPieceBox(shape, x, y, &box);
        if (CanCarveAround(&box, placement))
        {
            CarveAround(&box, placement);
            *outX = x;
            *outY = y;
            return TRUE;
        }
        if (++x == width)
        {
            x = 0;
            if (++y == height)
                y = 0;
        }
    }
    return FALSE;
}

/* Find the piece a spot (ways a to d in CONTENT PLACEMENT), carving it out
 * if it takes way c or d, and store it in (*outX, *outY). Returns FALSE if
 * there is none. */
static bool8 FindSpot(const struct PieceShape *shape, struct Placement *placement, s32 *outX, s32 *outY)
{
    s32 count = placement->candidateCount;
    s32 i, j, start;
    u8 mode;
    struct Box box;

    if (count == 0)
        return FALSE;
    for (i = 0; i < RANDOM_TRIES; i++)
    {
        j = MtMoonGen_RandomBelow(count);
        if (IsUsableSpot(shape, placement, placement->candidates[j], SPOT_FITS))
        {
            mode = SPOT_FITS;
            goto found;
        }
    }

    /* Ways b and c each look at every candidate once, starting from the
     * same random one and wrapping around at the end of the list. */
    start = MtMoonGen_RandomBelow(count);
    for (mode = SPOT_FITS; mode <= SPOT_CARVE; mode++)
    {
        j = start;
        for (i = 0; i < count; i++)
        {
            if (IsUsableSpot(shape, placement, placement->candidates[j], mode))
                goto found;
            if (++j == count)
                j = 0;
        }
    }
    return CarveAnywhere(shape, placement, outX, outY);

found:
    *outX = placement->candidates[j] & 0xFF;
    *outY = placement->candidates[j] >> 8;
    if (mode == SPOT_CARVE)
    {
        GetPieceBox(shape, *outX, *outY, &box);
        CarveAround(&box, placement);
    }
    return TRUE;
}

/* Remember the box for rule 3 and mark its floor tiles with `cell`. */
static void TakeBox(struct Placement *placement, const struct Box *box, u8 cell)
{
    s32 x, y;

    if (placement->count < MAX_PIECES)
        placement->boxes[placement->count++] = *box;
    for (y = box->y1; y <= box->y2; y++)
    {
        for (x = box->x1; x <= box->x2; x++)
        {
            if (IN_GRID(x, y) && sCaveGrid[CELL_INDEX(x, y)] == MT_MOON_CELL_FLOOR)
                sCaveGrid[CELL_INDEX(x, y)] = cell;
        }
    }
}

static bool8 PlaceObject(struct ObjectEventTemplate *template, struct Placement *placement)
{
    struct PieceShape shape;
    struct Box box;
    s32 x, y;

    shape.movementType = template->objUnion.normal.movementType;
    shape.rangeX = template->objUnion.normal.movementRangeX;
    shape.rangeY = template->objUnion.normal.movementRangeY;
    shape.needOpenRing = FALSE;
    if (!FindSpot(&shape, placement, &x, &y))
        return FALSE;
    template->x = x;
    template->y = y;
    GetPieceBox(&shape, x, y, &box);
    TakeBox(placement, &box, CELL_OCCUPIED);
    return TRUE;
}

/* A hidden item or sign: a one-tile prefab in open floor. Stamping its
 * solid tile turns the cell BLOCKED. */
static bool8 PlaceBgEvent(struct BgEvent *bgEvent, struct Placement *placement)
{
    struct PieceShape shape;
    struct Box box;
    s32 x, y;

    shape.movementType = MOVEMENT_TYPE_NONE;
    shape.rangeX = 0;
    shape.rangeY = 0;
    shape.needOpenRing = TRUE;
    if (!FindSpot(&shape, placement, &x, &y))
        return FALSE;
    bgEvent->x = x;
    bgEvent->y = y;
    MtMoonGen_StampTile(x, y, bgEvent->kind == BG_EVENT_HIDDEN_ITEM ? TILE_BOULDER : TILE_SIGNPOST);
    GetPieceBox(&shape, x, y, &box);
    TakeBox(placement, &box, MT_MOON_CELL_BLOCKED);
    return TRUE;
}

/* List every free floor tile as a candidate (see struct Placement). */
static void ListCandidates(struct Placement *placement)
{
    s32 x, y;

    placement->candidates = FloodQueue();
    placement->candidateCount = 0;
    for (y = 0; y < sCave.height; y++)
    {
        const u8 *row = GRID_ROW(y);

        for (x = 0; x < sCave.width; x++)
        {
            if (row[x] == MT_MOON_CELL_FLOOR)
                placement->candidates[placement->candidateCount++] = x | (y << 8);
        }
    }
}

/*
 * Put every object template (the starting position and movement of each
 * trainer, NPC and item ball, kept in the save block) back to the ROM's
 * vanilla values. Placement starts from them, so the result depends only on
 * the seed, even on Continue, when the save still holds the previous
 * placement; and a floor that falls back to the vanilla map gets them back.
 */
static void ResetObjectTemplates(const struct MapEvents *rom)
{
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    s32 i;

    for (i = 0; i < rom->objectEventCount; i++)
    {
        /* A clone template stands in for an object of a neighbouring map,
         * and its fields hold that map's ids instead of a position and a
         * movement. Mt. Moon has none, but leave any alone. */
        if (templates[i].kind == OBJ_KIND_CLONE)
            continue;
        templates[i].x = rom->objectEvents[i].x;
        templates[i].y = rom->objectEvents[i].y;
        templates[i].objUnion.normal.movementType = rom->objectEvents[i].objUnion.normal.movementType;
        templates[i].objUnion.normal.movementRangeX = rom->objectEvents[i].objUnion.normal.movementRangeX;
        templates[i].objUnion.normal.movementRangeY = rom->objectEvents[i].objUnion.normal.movementRangeY;
    }
}

/*
 * Place all of the floor's content (see CONTENT PLACEMENT above), objects
 * keeping their vanilla movement types and ranges. Returns FALSE if some
 * piece found no spot; GenerateFloor then tries a new shape.
 */
static bool8 PlaceContent(void)
{
    struct Placement *placement = &Work()->placement;
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    s32 objectCount = sMtMoonEvents.objectEventCount;
    s32 i;

    ResetObjectTemplates(&sMtMoonEvents);
    placement->count = 0;
    ListCandidates(placement);

    for (i = 0; i < objectCount; i++)
    {
        if (templates[i].kind != OBJ_KIND_CLONE && IsMovingType(templates[i].objUnion.normal.movementType)
            && !PlaceObject(&templates[i], placement))
            return FALSE;
    }
    for (i = 0; i < sMtMoonEvents.bgEventCount; i++)
    {
        if (!PlaceBgEvent(&sMtMoonBgEvents[i], placement))
            return FALSE;
    }
    for (i = 0; i < objectCount; i++)
    {
        if (templates[i].kind != OBJ_KIND_CLONE && !IsMovingType(templates[i].objUnion.normal.movementType)
            && !PlaceObject(&templates[i], placement))
            return FALSE;
    }
    return TRUE;
}

/* TRUE if a tile next to the box (north, south, west or east of it, where
 * the player could stand facing it) was reached by the last flood fill. */
static bool8 IsBoxNextToVisited(const struct Box *box)
{
    s32 x, y;

    for (x = box->x1; x <= box->x2; x++)
    {
        if (MtMoonGen_GetCell(x, box->y1 - 1) == MT_MOON_CELL_VISITED
            || MtMoonGen_GetCell(x, box->y2 + 1) == MT_MOON_CELL_VISITED)
            return TRUE;
    }
    for (y = box->y1; y <= box->y2; y++)
    {
        if (MtMoonGen_GetCell(box->x1 - 1, y) == MT_MOON_CELL_VISITED
            || MtMoonGen_GetCell(box->x2 + 1, y) == MT_MOON_CELL_VISITED)
            return TRUE;
    }
    return FALSE;
}

/*
 * The final check of CONTENT PLACEMENT, with every piece's tiles still
 * taken: from the entrance, without stepping on a ladder, the player can
 * walk to both ladders' footprints, to every free floor tile (no pockets),
 * and next to every piece. The rules make this true by construction; one
 * flood fill makes sure.
 */
static bool8 IsContentReachable(void)
{
    const struct Placement *placement = &Work()->placement;
    s32 i;
    s32 count = (sCave.width + 2) * (sCave.height + 2);
    s32 marked = MarkWalkableFromEntrance();
    bool8 reachable = IsFootprintVisited(&sCave.entrance) && IsFootprintVisited(&sCave.exit);

    for (i = 0; reachable && i < count; i++)
    {
        if (sCaveGrid[i] == MT_MOON_CELL_FLOOR)
            reachable = FALSE; /* free floor the fill didn't reach */
    }
    for (i = 0; reachable && i < placement->count; i++)
        reachable = IsBoxNextToVisited(&placement->boxes[i]);
    ClearWalkableMarks(marked);
    return reachable;
}

/* Objects stand on floor: give their tiles back (see CONTENT PLACEMENT). */
static void ReleaseObjectTiles(void)
{
    s32 i;
    s32 count = (sCave.width + 2) * (sCave.height + 2);

    for (i = 0; i < count; i++)
    {
        if (sCaveGrid[i] == CELL_OCCUPIED)
            sCaveGrid[i] = MT_MOON_CELL_FLOOR;
    }
}

/* ------------------------------------------------------------------------- */
/* Trainer facing                                                            */
/* ------------------------------------------------------------------------- */

/* The four ways a stationary trainer can face, and the step each looks
 * along. When two runs tie, the first in this list wins (unless the
 * trainer's vanilla facing is one of them; see FaceStationaryTrainers). */
static const struct
{
    u8 movementType;
    s8 dx;
    s8 dy;
} sFacings[] =
{
    { MOVEMENT_TYPE_FACE_DOWN,  0,  1 },
    { MOVEMENT_TYPE_FACE_UP,    0, -1 },
    { MOVEMENT_TYPE_FACE_LEFT, -1,  0 },
    { MOVEMENT_TYPE_FACE_RIGHT, 1,  0 },
};

/* How many free floor tiles follow (x, y) in a straight line, one step of
 * (dx, dy) at a time, before the first tile that isn't (a wall, a boulder
 * or signpost, or a tile an object could stand on). */
static s32 FloorRunLength(s32 x, s32 y, s32 dx, s32 dy)
{
    s32 length = 0;

    for (x += dx, y += dy; IN_GRID(x, y) && sCaveGrid[CELL_INDEX(x, y)] == MT_MOON_CELL_FLOOR; x += dx, y += dy)
        length++;
    return length;
}

/*
 * A trainer only spots the player along the direction it faces, and the
 * game's sight check (src/trainer_see.c) stops at walls and at objects.
 * Turn every stationary trainer (a trainer whose movement type is one of
 * the four FACE_ types) toward its longest straight run of open floor, so
 * its sight line can actually catch the player. Runs end at walls,
 * boulders and signposts, and at every tile an object could stand on, so
 * this runs while the objects' tiles are still OCCUPIED. Ties keep the
 * vanilla facing when it is one of the longest. Look-around trainers,
 * wanderers and non-trainers keep their vanilla movement.
 */
static void FaceStationaryTrainers(void)
{
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    s32 i, j;

    for (i = 0; i < sMtMoonEvents.objectEventCount; i++)
    {
        struct ObjectEventTemplate *trainer = &templates[i];
        u8 bestType = trainer->objUnion.normal.movementType;
        s32 best = -1;

        if (trainer->kind == OBJ_KIND_CLONE || trainer->objUnion.normal.trainerType == TRAINER_TYPE_NONE)
            continue;
        for (j = 0; j < ARRAY_COUNT(sFacings); j++)
        {
            if (sFacings[j].movementType == bestType)
                best = FloorRunLength(trainer->x, trainer->y, sFacings[j].dx, sFacings[j].dy);
        }
        if (best < 0)
            continue; /* not a stationary trainer */
        for (j = 0; j < ARRAY_COUNT(sFacings); j++)
        {
            s32 run = FloorRunLength(trainer->x, trainer->y, sFacings[j].dx, sFacings[j].dy);

            if (run > best)
            {
                best = run;
                bestType = sFacings[j].movementType;
            }
        }
        trainer->objUnion.normal.movementType = bestType;
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
 * FreeCaveGrid().
 *
 * A shape is thrown away, and the style asked for a new one, if its ladders
 * break the rules (CheckLadders) or if some piece of content finds no spot
 * on it, even with repairs (PlaceContent, IsContentReachable). Returns
 * FALSE if the heap can't lend the grid or no shape works in 4 tries; the
 * floor then simply stays the vanilla map from the ROM, with its vanilla
 * warps and objects, which is fully playable. (On Continue the player
 * would keep coordinates from the generated cave, which may be wall in the
 * vanilla map, and the safety net can't run without a generated cave.
 * Both are should-never-happen fallbacks: the block is about 7 KB of a
 * 112 KB heap, and the property test checks every style on 100 seeds per
 * floor.)
 */
static bool8 GenerateFloor(const struct MtMoonFloor *floor, u16 seed)
{
    s32 attempt;
    const struct MapEvents *rom = gMapHeader.events; /* the map loader set it from the ROM header */

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
        if (!CheckLadders(floor))
            continue;
        FillPockets();
        SetUpEvents(floor, rom);
        if (PlaceContent() && IsContentReachable())
            break;
    }
    if (attempt == MAX_SHAPE_ATTEMPTS)
    {
        /* Undo what a failed attempt may have changed outside the grid. */
        gMapHeader.events = rom;
        ResetObjectTemplates(rom);
        return FALSE;
    }

    /* Every step above may change the grid (fill, stamp, carve), so the
     * map is drawn last. */
    FaceStationaryTrainers();
    ReleaseObjectTiles();
    DrawCave(floor);
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
