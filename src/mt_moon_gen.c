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
 *      (ProposeLadders) and, on B2F, for the fossil chamber
 *      (ProposeChamber), and let the floor's style carve the shape around
 *      them, moving them first if it wants.
 *   3. Check the ladders (CheckLadders). On B2F, stamp the fossil chamber,
 *      a prefab copied from vanilla, and carve corridors so that both
 *      ladders connect around it and its foot connects to them
 *      (PlaceChamber; see FOSSIL CHAMBER below).
 *   4. Fill pockets (FillPockets): floor the player can't walk to becomes
 *      wall, so 100% of the floor is reachable.
 *   5. Rewrite the floor's event tables (SetUpEvents): the entrance and exit
 *      warps move onto the generated ladders, every other vanilla warp is
 *      parked at (0,0), a solid tile nobody can stand on, the hidden items
 *      and signs get a RAM copy whose positions step 6 fills in, and
 *      Miguel's trigger tile moves with the fossil chamber.
 *   6. Place ALL of the floor's vanilla content (PlaceContent): Miguel and
 *      the fossils in their places in the chamber; trainers, NPCs and item
 *      balls on open floor, hidden items on a stamped boulder and signs on
 *      a stamped signpost. Every spot keeps the floor
 *      connected and leaves a tile next to the piece for the player to
 *      stand on, facing it, carving open floor where nothing fits. See
 *      CONTENT PLACEMENT below.
 *   7. Draw every map cell with the style's tile picker, then the stamped
 *      one-tile prefabs on top, and the fossil chamber's vanilla tiles
 *      over everything (DrawCave).
 *   8. Add vanilla's decorations (rubble, craters, puddles) wherever they
 *      fit without a seam, about as many as the floor's vanilla map has
 *      (DecorateCave; see DECORATIONS below).
 *   9. A final flood fill checks that, with the rubble in place, the
 *      player can still reach both ladders, every floor tile and every
 *      piece of content (IsContentReachable).
 *   If step 3, 6 or 9 fails, the shape is thrown away and the style builds
 *   a new one (back to step 2), up to 4 times; after that the floor stays
 *   the vanilla map (RestoreVanillaMap undoes step 7). So nothing is ever
 *   dropped.
 *   10. Turn each stationary trainer toward its longest straight run of
 *      open floor (FaceStationaryTrainers).
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
#include "constants/flags.h"
#include "constants/layouts.h"
#include "constants/trainer_types.h"
#include "constants/vars.h"

/* Warp table entries kept per floor (B1F has the most vanilla warps: 8). */
#define MAX_WARPS 8

/* Hidden items and signs kept per floor (B1F has the most: 6 hidden
 * items). Each costs 12 bytes of EWRAM (sMtMoonBgEvents, a static array, so
 * the 72 bytes are reserved for good). */
#define MAX_BG_EVENTS 6

/* Coordinate events (trigger tiles) kept per floor: B2F has one, Super Nerd
 * Miguel's battle trigger; 1F and B1F have none. 16 bytes of EWRAM each
 * (sMtMoonCoordEvents). */
#define MAX_COORD_EVENTS 1

/* One-tile prefabs per floor (see MtMoonGen_StampTile). They are kept in
 * the heap block borrowed for the grid, 4 bytes each, so room for many
 * costs nothing once generation is over. The most a floor can use is 77
 * (1F: its sign, 28 rubble tiles, 9 craters and 3 puddles of 4 tiles each;
 * see DECORATIONS), and the decorations stop at a full list. */
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
#define TILE_UP_LADDER_TOP_IN_FLOOR 0x068e  /* solid; the ladder's top, a small
                                             * rock standing on the floor */

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

/* The decoration kinds, in the order DecorateCave places them (see
 * DECORATIONS). */
enum
{
    DECORATION_PUDDLE,
    DECORATION_CRATER,
    DECORATION_RUBBLE,
    DECORATION_KINDS
};

/* A decoration in a floor's vanilla map: its kind and its top-left tile. */
struct VanillaDecoration
{
    u8 kind;  /* DECORATION_* */
    u8 x;
    u8 y;
};

/*
 * Every decoration of each floor's vanilla map (data/layouts/MtMoon_<floor>/
 * map.bin), copied out of it by a throwaway script: each puddle by its west
 * drip tile (0x2ee), each crater by its top-left tile (0x2d5), each rubble
 * tile (0x283), in that order, each kind in reading order. They
 * tell the decoration step how many of each kind vanilla has and where to
 * read what vanilla puts around them (see THE VANILLA NEIGHBOURS); the step
 * checks that the map really has that decoration at each place, so a wrong
 * entry is skipped, never trusted.
 */
static const struct VanillaDecoration sVanillaDecorations1F[] =
{
    { DECORATION_PUDDLE, 4, 21 }, { DECORATION_PUDDLE, 25, 31 },
    { DECORATION_CRATER, 10, 4 }, { DECORATION_CRATER, 35, 5 }, { DECORATION_CRATER, 8, 12 },
    { DECORATION_CRATER, 36, 26 }, { DECORATION_CRATER, 4, 28 }, { DECORATION_CRATER, 37, 35 },
    { DECORATION_RUBBLE, 11, 2 }, { DECORATION_RUBBLE, 12, 2 }, { DECORATION_RUBBLE, 12, 3 },
    { DECORATION_RUBBLE, 17, 5 }, { DECORATION_RUBBLE, 16, 7 }, { DECORATION_RUBBLE, 42, 9 },
    { DECORATION_RUBBLE, 24, 13 }, { DECORATION_RUBBLE, 32, 13 }, { DECORATION_RUBBLE, 23, 14 },
    { DECORATION_RUBBLE, 23, 15 }, { DECORATION_RUBBLE, 6, 22 }, { DECORATION_RUBBLE, 7, 22 },
    { DECORATION_RUBBLE, 7, 23 }, { DECORATION_RUBBLE, 24, 32 }, { DECORATION_RUBBLE, 24, 33 },
    { DECORATION_RUBBLE, 25, 33 }, { DECORATION_RUBBLE, 24, 34 }, { DECORATION_RUBBLE, 25, 34 },
    { DECORATION_RUBBLE, 24, 35 },
};

static const struct VanillaDecoration sVanillaDecorationsB1F[] =
{
    { DECORATION_PUDDLE, 21, 1 }, { DECORATION_PUDDLE, 13, 15 },
    { DECORATION_CRATER, 41, 25 },
    { DECORATION_RUBBLE, 37, 2 }, { DECORATION_RUBBLE, 38, 2 }, { DECORATION_RUBBLE, 39, 2 },
    { DECORATION_RUBBLE, 40, 2 }, { DECORATION_RUBBLE, 16, 3 }, { DECORATION_RUBBLE, 37, 3 },
    { DECORATION_RUBBLE, 46, 3 }, { DECORATION_RUBBLE, 36, 4 }, { DECORATION_RUBBLE, 6, 9 },
    { DECORATION_RUBBLE, 5, 11 }, { DECORATION_RUBBLE, 41, 31 }, { DECORATION_RUBBLE, 41, 32 },
    { DECORATION_RUBBLE, 41, 33 }, { DECORATION_RUBBLE, 38, 34 },
};

/* B2F's puddle at (10,12) and rubble at (8,13) are inside the fossil
 * chamber's vanilla block, so the generated B2F has them too (see HOW
 * MANY). */
static const struct VanillaDecoration sVanillaDecorationsB2F[] =
{
    { DECORATION_PUDDLE, 39, 5 }, { DECORATION_PUDDLE, 10, 12 },
    { DECORATION_CRATER, 5, 4 }, { DECORATION_CRATER, 24, 6 }, { DECORATION_CRATER, 34, 19 },
    { DECORATION_CRATER, 10, 23 },
    { DECORATION_RUBBLE, 6, 10 }, { DECORATION_RUBBLE, 7, 12 }, { DECORATION_RUBBLE, 8, 13 },
    { DECORATION_RUBBLE, 27, 21 }, { DECORATION_RUBBLE, 26, 22 }, { DECORATION_RUBBLE, 15, 28 },
    { DECORATION_RUBBLE, 16, 31 },
};

/* One generated floor and how it connects to the rest of the game. */
struct MtMoonFloor
{
    u16 layoutId;
    u8 floorNumber;     /* mixed into the seed, so floors get different caves */
    bool8 hasChamber;   /* TRUE: the floor gets the fossil chamber prefab */
    struct MtMoonFloorLadder entrance;  /* toward Route 4 west */
    struct MtMoonFloorLadder exit;      /* toward Route 4 east */
    const struct MtMoonStyle *style;
    const struct VanillaDecoration *decorations;  /* the vanilla map's */
    u8 decorationCount;
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
        .decorations = sVanillaDecorations1F,
        .decorationCount = ARRAY_COUNT(sVanillaDecorations1F),
    },
    {
        .layoutId = LAYOUT_MT_MOON_B1F,
        .floorNumber = 2,
        .entrance = LADDER_ANYWHERE(0, MT_MOON_LADDER_UP),
        .exit = LADDER_ANYWHERE(3, MT_MOON_LADDER_DOWN),
        .style = &gMtMoonStyleB1F,
        .decorations = sVanillaDecorationsB1F,
        .decorationCount = ARRAY_COUNT(sVanillaDecorationsB1F),
    },
    {
        .layoutId = LAYOUT_MT_MOON_B2F,
        .floorNumber = 3,
        .hasChamber = TRUE,
        .entrance = LADDER_ANYWHERE(0, MT_MOON_LADDER_UP_IN_FLOOR),
        .exit = LADDER_ANYWHERE(1, MT_MOON_LADDER_UP_IN_FLOOR),
        .style = &gMtMoonStyleB2F,
        .decorations = sVanillaDecorationsB2F,
        .decorationCount = ARRAY_COUNT(sVanillaDecorationsB2F),
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
 * prefabs, the content placement's bookkeeping and the decorations' sets
 * of vanilla neighbours) and the style's own work area. (A u16 index
 * limits a grid to 65,535 cells including the frame; Mt. Moon's floors
 * have about 2,100.) The block is about 8 KB on 1F and B1F and 9.6 KB on
 * B2F (the biggest: its style's tile map is 1.9 KB), of the 112 KB heap.
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

/* Map tile (x, y) in the game's map buffer (VMap; see the GBA notes at the
 * top), where the floor is drawn: VMap has a MAP_OFFSET-tile margin on
 * every side, so the tile is VMap cell (x + MAP_OFFSET, y + MAP_OFFSET),
 * and the next row's tile is VMap.Xsize cells further on. */
static u16 *MapBufferCell(s32 x, s32 y)
{
    return &VMap.map[(y + MAP_OFFSET) * VMap.Xsize + x + MAP_OFFSET];
}

/* The rewritten event tables. gMapHeader.events points here while a
 * generated floor is loaded, instead of at the read-only ROM tables. The
 * game reads them every time the player steps on a warp or presses A in
 * front of a hidden item or sign, so they must last as long as the floor
 * is loaded (unlike the grid). They are rebuilt from the seed on Continue.
 * (sMtMoonBgEvents' 72 bytes and sMtMoonCoordEvents' 16 come out of the
 * ~480 bytes of EWRAM that moving the grid to the heap freed.) */
static EWRAM_DATA struct WarpEvent sMtMoonWarps[MAX_WARPS] = {0};
static EWRAM_DATA struct CoordEvent sMtMoonCoordEvents[MAX_COORD_EVENTS] = {0};
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

/* At most 64 objects (the save block's template array) and the bg events:
 * every PIECE of content (see CONTENT PLACEMENT). */
#define MAX_PIECES (OBJECT_EVENT_TEMPLATES_COUNT + MAX_BG_EVENTS)

/* What content placement keeps track of (see CONTENT PLACEMENT). */
struct Placement
{
    /* The floor's style, for rule 6 (canStampAt). */
    const struct MtMoonStyle *style;
    /* The candidate tiles, packed as x | (y << 8) (both are under 256).
     * They live in the flood-fill queue's memory: no flood fill runs while
     * content is being placed. */
    u16 *candidates;
    s32 candidateCount;
    /* The boxes of every piece placed so far (rule 3). */
    s32 count;
    struct Box boxes[MAX_PIECES];
    /* The fossil chamber's box (GetChamberBox), which nothing else may come
     * near, if the floor has a chamber (rule 5). */
    bool8 hasChamber;
    struct Box chamberBox;
};

/*
 * The decoration tiles (see DECORATIONS below): rubble, the 4 tiles of a
 * crater and the 4 of a puddle, numbered for the tables of the decoration
 * step.
 */
enum
{
    DTILE_RUBBLE,
    DTILE_CRATER_NW,
    DTILE_CRATER_NE,
    DTILE_CRATER_SW,
    DTILE_CRATER_SE,
    DTILE_DRIP_W,
    DTILE_DRIP_E,
    DTILE_PUDDLE_W,
    DTILE_PUDDLE_E,
    DTILE_COUNT
};

/*
 * A SET OF METATILES, one bit per metatile of the cave tileset's block
 * 0x280..0x33f (192 metatiles in 6 words; the cave tileset has 161, from
 * 0x280 to 0x320, and every Mt. Moon map uses only those). Metatile m is
 * bit (m - 0x280) % 32 of word (m - 0x280) / 32, worked out as
 * (m - 0x280) & 31 and (m - 0x280) >> 5 (the low 5 bits, and a shift right
 * by 5, which divides by 32), since the GBA's CPU can't divide. A metatile
 * outside the block is never in a set: m - 0x280, read as an unsigned
 * number, is then 192 or more (below 0x280 the subtraction wraps around
 * to a huge number), so one comparison checks both ends of the block.
 * Testing a bit is a subtraction, a shift and a mask, much quicker on the
 * GBA than searching a list.
 */
#define METATILE_SET_BASE  0x280
#define METATILE_SET_WORDS 6

/* TRUE if metatile `metatile` is in metatile set `set` (u32[6]). */
#define IN_METATILE_SET(set, metatile) \
    ((u32)((metatile) - METATILE_SET_BASE) < METATILE_SET_WORDS * 32 \
     && (((set)[((metatile) - METATILE_SET_BASE) >> 5] >> (((metatile) - METATILE_SET_BASE) & 31)) & 1))

/*
 * What the floor's vanilla map has around its decorations (see THE VANILLA
 * NEIGHBOURS under DECORATIONS): allowed[t][side] is the set of metatiles
 * vanilla puts on that side of decoration tile t (DTILE_*; the sides are
 * numbered like sStepDx: north, south, west, east). Plus the set of the
 * decoration metatiles themselves, and how many decorations of each kind
 * the vanilla map has. 892 bytes.
 */
struct VanillaNeighbours
{
    u32 allowed[DTILE_COUNT][4][METATILE_SET_WORDS];
    u32 decorationTiles[METATILE_SET_WORDS];  /* the 9 decoration metatiles */
    u8 vanillaCount[DECORATION_KINDS];
};

/*
 * The fixed-size work areas of one generation, kept in the borrowed heap
 * block after the grid and the flood queue (2,000 bytes). They are too big
 * for the stack: the CPU's stack lives in IWRAM, between the last IWRAM
 * variable and IWRAM_END - 0x1C0, which leaves it under 2.5 KB for the
 * whole chain of calls that loads a map.
 */
struct GenerationWork
{
    struct StampList stamps;
    struct Placement placement;
    bool8 hasChamber;              /* the floor table's hasChamber */
    struct MtMoonChamber chamber;  /* where the fossil chamber goes */
    struct VanillaNeighbours neighbours;  /* for the decorations */
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
 *     [grid] [flood queue] [struct GenerationWork] [the style's work area]
 * with each part's size rounded up to a multiple of 4 bytes, so every part
 * starts on an address the CPU can read it from (a u16 needs an even
 * address; a u32 or a pointer, a multiple of 4). The heap hands out
 * blocks that start on a multiple of 4. (struct GenerationWork holds
 * pointers, so its size is already a multiple of 4.) The style's work area
 * is the workBytes its struct MtMoonStyle asks for, often none.
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
 * plus the flood-fill queue and the work areas (an empty stamp list, and
 * `styleBytes` of zeroed memory for the style). */
static bool8 AllocCaveGrid(s32 width, s32 height, u16 styleBytes)
{
    sCave.width = width;
    sCave.height = height;
    sCaveGrid = AllocZeroed(GridBytes() + QueueBytes() + sizeof(struct GenerationWork) + styleBytes);
    return sCaveGrid != NULL;
}

/* The style's work area: right after struct GenerationWork. */
void *MtMoonGen_GetStyleWork(void)
{
    return Work() + 1;
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

/*  F F F
 *  F T F      T = the ladder's top (0x28e), solid, standing on the floor
 *  F U F      U = the up-ladder (0x296), walkable
 *  F F F  */
static const struct LadderCell sUpLadderInFloorCells[] =
{
    { -1, -2, MT_MOON_CELL_FLOOR, 0 },
    { 0, -2, MT_MOON_CELL_FLOOR, 0 },
    { 1, -2, MT_MOON_CELL_FLOOR, 0 },
    { -1, -1, MT_MOON_CELL_FLOOR, 0 },
    { 0, -1, MT_MOON_CELL_WALL, TILE_UP_LADDER_TOP_IN_FLOOR },
    { 1, -1, MT_MOON_CELL_FLOOR, 0 },
    { -1, 0, MT_MOON_CELL_FLOOR, 0 },
    { 0, 0, MT_MOON_CELL_FLOOR, TILE_UP_LADDER },
    { 1, 0, MT_MOON_CELL_FLOOR, 0 },
    { -1, 1, MT_MOON_CELL_FLOOR, 0 },
    { 0, 1, MT_MOON_CELL_FLOOR, 0 },
    { 1, 1, MT_MOON_CELL_FLOOR, 0 },
};

static const struct LadderKind sLadderKinds[] =
{
    /* The foot: north of the door, the hole itself, south of an up-ladder. */
    [MT_MOON_LADDER_DOOR] = { sDoorCells,     ARRAY_COUNT(sDoorCells),     0, -1 },
    [MT_MOON_LADDER_DOWN] = { sHoleCells,     ARRAY_COUNT(sHoleCells),     0,  0 },
    [MT_MOON_LADDER_UP]   = { sUpLadderCells, ARRAY_COUNT(sUpLadderCells), 0,  1 },
    [MT_MOON_LADDER_UP_IN_FLOOR] = { sUpLadderInFloorCells, ARRAY_COUNT(sUpLadderInFloorCells), 0, 1 },
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
 * at the foot of a wall (MT_MOON_LADDER_UP) goes on row 2, right under a
 * 2-row top border, which is its north wall; any other kind (B2F's up-ladders
 * standing in open floor included) gets y in [4, height - 7]. Those margins
 * leave room for a 5 x 5 patch of open floor around the ladder inside a
 * 2-cell border (3 at the bottom).
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
 * takes a draw or two. The fallback after 200 draws, which practically
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
     * (north of the door, north-west of a hole or of a ladder standing in
     * open floor, south of an up-ladder at the foot of a wall). */
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
/* Fossil chamber                                                            */
/* ------------------------------------------------------------------------- */

/*
 * FOSSIL CHAMBER
 *
 * B2F's fossil chamber is a prefab: a 13 x 9 block of vanilla B2F's own
 * tiles, copied tile for tile to wherever the chamber goes on the generated
 * B2F. Super Nerd Miguel, the Dome and Helix Fossils and Miguel's battle
 * trigger tile all stand inside that block in vanilla, and they move with
 * it, keeping their vanilla places in it: after the player takes a fossil,
 * Miguel's script walks him to the other one with fixed steps (walk_right,
 * walk_up x3 to the Helix Fossil; walk_up x3 to the Dome Fossil), which
 * only end in front of the right fossil if nothing inside moved. The
 * chamber's layout, its rules and what a style may do with it are in
 * include/mt_moon_gen_style.h (THE FOSSIL CHAMBER).
 *
 * The tiles are read straight from the floor's own vanilla layout in ROM
 * (gMapHeader.mapLayout->map, the map.bin the game was built with), so no
 * copy of them is kept here and they can't drift from vanilla.
 *
 * In GenerateFloor, for a floor that has a chamber:
 *   1. ProposeChamber, before buildShape: a random position that follows
 *      the rules (IsChamberSpotAllowed).
 *   2. PlaceChamber, after buildShape: check the (possibly moved) position,
 *      stamp the chamber's cells into the grid, then carve corridors so
 *      that both ladders still connect without going through the chamber
 *      and the chamber's foot connects to them (CarveCorridorToEntrance).
 *   3. SetUpEvents and PlaceContent move Miguel's trigger, Miguel and the
 *      fossils to their places in it, and keep everything else out.
 *   4. DrawCave draws its vanilla tiles last, over the style's.
 */

/* The chamber's top-left tile in vanilla B2F (see THE FOSSIL CHAMBER in
 * mt_moon_gen_style.h for its size and layout). */
#define CHAMBER_SRC_X 8
#define CHAMBER_SRC_Y 7

/* The chamber's foot, relative to its top-left tile: the landing cell in
 * front of its stairs. */
#define CHAMBER_FOOT_DX 5
#define CHAMBER_FOOT_DY 6

/* How far the chamber's box (GetChamberBox) stays from each ladder, in king
 * moves. A ladder's footprint reaches 2 cells out from it, so this leaves
 * at least one free cell between a footprint and the chamber. */
#define CHAMBER_LADDER_CLEARANCE 4

/* How many random positions ProposeChamber tries before scanning for one. */
#define CHAMBER_RANDOM_TRIES 64

struct MtMoonChamber *MtMoonGen_GetChamber(void)
{
    if (sCaveGrid == NULL || !Work()->hasChamber)
        return NULL;
    return &Work()->chamber;
}

static bool8 HasChamber(void)
{
    return MtMoonGen_GetChamber() != NULL;
}

/* The vanilla map cell value of the chamber's tile (dx, dy), counted from
 * its top-left tile. */
static u16 GetChamberTile(s32 dx, s32 dy)
{
    const struct MapLayout *layout = gMapHeader.mapLayout;

    return layout->map[(CHAMBER_SRC_Y + dy) * layout->width + CHAMBER_SRC_X + dx];
}

/*
 * The box nothing else may come into, for a chamber with its top-left tile
 * at (x, y): the chamber itself plus the row above it (which holds the 2
 * cells behind the fossils). Content placement keeps every piece a free
 * tile away from it, and no carving ever reaches it.
 */
static void MakeChamberBox(s32 x, s32 y, struct Box *box)
{
    box->x1 = x;
    box->y1 = y - 1;
    box->x2 = x + MT_MOON_CHAMBER_WIDTH - 1;
    box->y2 = y + MT_MOON_CHAMBER_HEIGHT - 1;
}

/* The box of the floor's chamber, where it is now. */
static void GetChamberBox(struct Box *box)
{
    MakeChamberBox(Work()->chamber.x, Work()->chamber.y, box);
}

/* TRUE if vanilla tile (x, y) of the floor lies in the chamber's vanilla
 * block, so whatever vanilla put there moves with the chamber. */
static bool8 IsInChamberSource(s32 x, s32 y)
{
    return x >= CHAMBER_SRC_X && x < CHAMBER_SRC_X + MT_MOON_CHAMBER_WIDTH
        && y >= CHAMBER_SRC_Y && y < CHAMBER_SRC_Y + MT_MOON_CHAMBER_HEIGHT;
}

/* Where vanilla tile (vanillaX, vanillaY) of the chamber's block is on the
 * generated floor: the same place in the chamber. */
static s32 ChamberX(s32 vanillaX)
{
    return Work()->chamber.x + vanillaX - CHAMBER_SRC_X;
}

static s32 ChamberY(s32 vanillaY)
{
    return Work()->chamber.y + vanillaY - CHAMBER_SRC_Y;
}

/* TRUE if a vanilla object template stands inside the chamber's block, so
 * the object moves with the chamber (on B2F: Miguel and the two fossils). */
static bool8 IsChamberObjectTemplate(const struct ObjectEventTemplate *rom)
{
    return rom->kind != OBJ_KIND_CLONE && IsInChamberSource(rom->x, rom->y);
}

void MtMoonGen_MarkChamberSite(void)
{
    const struct MtMoonChamber *chamber = MtMoonGen_GetChamber();
    s32 dx, dy;

    if (chamber == NULL)
        return;
    for (dy = 0; dy < MT_MOON_CHAMBER_HEIGHT; dy++)
    {
        for (dx = 0; dx < MT_MOON_CHAMBER_WIDTH; dx++)
            MtMoonGen_SetCell(chamber->x + dx, chamber->y + dy,
                IS_SOLID_TILE(GetChamberTile(dx, dy)) ? MT_MOON_CELL_WALL : MT_MOON_CELL_FLOOR);
    }
    /* The fossils stand on the walkable tiles of the top row; the cells
     * above them must be wall, or a fossil could be taken from outside,
     * without passing Miguel's trigger. */
    for (dx = 0; dx < MT_MOON_CHAMBER_WIDTH; dx++)
    {
        if (!IS_SOLID_TILE(GetChamberTile(dx, 0)))
            MtMoonGen_SetCell(chamber->x + dx, chamber->y - 1, MT_MOON_CELL_WALL);
    }
}

void MtMoonGen_GetChamberFoot(s32 *x, s32 *y)
{
    const struct MtMoonChamber *chamber = MtMoonGen_GetChamber();

    *x = *y = -1;
    if (chamber == NULL)
        return;
    *x = chamber->x + CHAMBER_FOOT_DX;
    *y = chamber->y + CHAMBER_FOOT_DY;
}

/* Turn every `from` cell of the chamber into `to` (see HideChamberFloor). */
static void ReplaceChamberCells(u8 from, u8 to)
{
    const struct MtMoonChamber *chamber = &Work()->chamber;
    s32 x, y;

    for (y = chamber->y; y < chamber->y + MT_MOON_CHAMBER_HEIGHT; y++)
    {
        u8 *row = &sCaveGrid[CELL_INDEX(0, y)];

        for (x = chamber->x; x < chamber->x + MT_MOON_CHAMBER_WIDTH; x++)
        {
            if (row[x] == from)
                row[x] = to;
        }
    }
}

/*
 * Hide the chamber's floor from the flood fills: while hidden, its floor
 * cells are BLOCKED, which no flood fill crosses (MarkReachableFrom only
 * spreads into FLOOR), so the rest of the cave is looked at as if the
 * chamber were solid rock. Nothing else in the chamber is BLOCKED (its
 * cells are only ever FLOOR or WALL, MtMoonGen_MarkChamberSite), so
 * ShowChamberFloor turns exactly those cells back.
 */
static void HideChamberFloor(void)
{
    ReplaceChamberCells(MT_MOON_CELL_FLOOR, MT_MOON_CELL_BLOCKED);
}

static void ShowChamberFloor(void)
{
    ReplaceChamberCells(MT_MOON_CELL_BLOCKED, MT_MOON_CELL_FLOOR);
}

/*
 * Rule 1 of THE FOSSIL CHAMBER (mt_moon_gen_style.h): with its top-left
 * tile at (x, y), the chamber lies inside the 2-cell border (3 at the
 * bottom) with the row below it inside too (its landing opens south), and
 * its box is CHAMBER_LADDER_CLEARANCE from both ladders.
 */
static bool8 IsChamberSpotAllowed(s32 x, s32 y)
{
    struct Box box;

    if (x < 2 || y < 2 || x + MT_MOON_CHAMBER_WIDTH > sCave.width - 2
        || y + MT_MOON_CHAMBER_HEIGHT > sCave.height - 4)
        return FALSE;
    MakeChamberBox(x, y, &box);
    return !IsNearLadder(&box, &sCave.entrance, CHAMBER_LADDER_CLEARANCE)
        && !IsNearLadder(&box, &sCave.exit, CHAMBER_LADDER_CLEARANCE);
}

/*
 * The core's proposal (rule 1): random positions until one is allowed,
 * then, if none of CHAMBER_RANDOM_TRIES was (practically never: on a
 * 48 x 40 floor most positions are), the first allowed one in reading
 * order. If there is none at all, the last random one stays and
 * PlaceChamber rejects the shape.
 */
static void ProposeChamber(void)
{
    struct MtMoonChamber *chamber = &Work()->chamber;
    /* x runs from 2 to width - 15, y from 2 to height - 13 (see
     * IsChamberSpotAllowed). */
    s32 xCount = sCave.width - MT_MOON_CHAMBER_WIDTH - 3;
    s32 yCount = sCave.height - MT_MOON_CHAMBER_HEIGHT - 5;
    s32 i, x, y;

    for (i = 0; i < CHAMBER_RANDOM_TRIES; i++)
    {
        chamber->x = MtMoonGen_RandomBelow(xCount) + 2;
        chamber->y = MtMoonGen_RandomBelow(yCount) + 2;
        if (IsChamberSpotAllowed(chamber->x, chamber->y))
            return;
    }
    for (y = 2; y < yCount + 2; y++)
    {
        for (x = 2; x < xCount + 2; x++)
        {
            if (IsChamberSpotAllowed(x, y))
            {
                chamber->x = x;
                chamber->y = y;
                return;
            }
        }
    }
}

/*
 * CORRIDORS
 *
 * Stamping the chamber turns a 13 x 9 block into its own walls and floor,
 * whatever the style made there, so it can cut the style's cave apart.
 * CarveCorridorToEntrance repairs that: it makes floor cell (x, y)
 * reachable on foot from the entrance (without stepping on a ladder),
 * carving the shortest corridor of wall cells if it isn't already.
 *
 * It is a breadth-first search, like MarkReachableFrom, but starting from
 * (x, y) and allowed to step into wall cells it may carve: inside the
 * 2-cell border (3 at the bottom, the border every style keeps), outside
 * the chamber's box and away from both ladders' footprints (NEAR_LADDER).
 * It may also cross floor the entrance can't reach yet (a pocket). It ends
 * at the first cell next to floor the entrance reaches. That cell and the
 * search's way back to (x, y) become floor: every cell the search enters
 * remembers which step entered it (north, south, west or east), so the way
 * back is followed one remembered step at a time.
 *
 * The search keeps its notes in the grid itself, as values that exist only
 * while it runs (no extra memory is needed): CELL_SEARCHED_WALL + step for
 * a wall cell, CELL_SEARCHED_FLOOR + step for an unreached floor cell. At
 * the end every cell gets its old value back, apart from the corridor.
 * Cells the chamber hides (HideChamberFloor) can't be crossed.
 */
/* Both are multiples of 4, so a step (0-3) added to either fits in the
 * lowest 2 bits, and the value is above every other cell value. */
#define CELL_SEARCHED_WALL  8   /* + step */
#define CELL_SEARCHED_FLOOR 12  /* + step */

/* The steps: north, south, west, east. */
static const s8 sStepDx[4] = { 0, 0, -1, 1 };
static const s8 sStepDy[4] = { -1, 1, 0, 0 };

/* TRUE if the search may carve through wall cell (x, y), which is inside
 * the grid or on its wall frame. */
static bool8 IsCarvableWall(s32 x, s32 y, const struct Box *chamberBox)
{
    if (x < 2 || y < 2 || x > sCave.width - 3 || y > sCave.height - 4)
        return FALSE;
    if (NEAR_LADDER(sCave.entrance, x, y) || NEAR_LADDER(sCave.exit, x, y))
        return FALSE;
    return chamberBox == NULL
        || x < chamberBox->x1 || x > chamberBox->x2 || y < chamberBox->y1 || y > chamberBox->y2;
}

static bool8 CarveCorridorToEntrance(s32 x, s32 y)
{
    u16 *queue = FloodQueue();
    s32 head = 0, tail = 0;
    s32 i, step, cx = 0, cy = 0;
    s32 count = (sCave.width + 2) * (sCave.height + 2);
    s32 marked = MarkWalkableFromEntrance();
    bool8 found = FALSE;
    struct Box box;
    const struct Box *chamberBox = NULL;
    u8 start = MtMoonGen_GetCell(x, y);

    if (start != MT_MOON_CELL_FLOOR)
    {
        ClearWalkableMarks(marked);
        return start == MT_MOON_CELL_VISITED; /* already reachable */
    }
    if (HasChamber())
    {
        GetChamberBox(&box);
        chamberBox = &box;
    }

    /* The queue now holds the search's cells (packed x | y << 8), so the
     * marks are undone by the sweep at the end instead of ClearVisited. */
    sCaveGrid[CELL_INDEX(x, y)] = CELL_SEARCHED_FLOOR;
    queue[tail++] = x | (y << 8);
    while (head < tail && !found)
    {
        cx = queue[head] & 0xFF;
        cy = queue[head] >> 8;
        head++;
        for (step = 0; step < 4; step++)
        {
            s32 nx = cx + sStepDx[step];
            s32 ny = cy + sStepDy[step];
            u8 *cell = &sCaveGrid[CELL_INDEX(nx, ny)]; /* the wall frame keeps it in the array */

            if (*cell == MT_MOON_CELL_VISITED)
            {
                found = TRUE;
                break;
            }
            if (*cell == MT_MOON_CELL_FLOOR)
                *cell = CELL_SEARCHED_FLOOR + step;
            else if (*cell == MT_MOON_CELL_WALL && IsCarvableWall(nx, ny, chamberBox))
                *cell = CELL_SEARCHED_WALL + step;
            else
                continue;
            queue[tail++] = nx | (ny << 8);
        }
    }

    /* Carve the way back from (cx, cy), the cell next to reachable floor. */
    if (found)
    {
        while (cx != x || cy != y)
        {
            u8 *cell = &sCaveGrid[CELL_INDEX(cx, cy)];

            /* The cell holds CELL_SEARCHED_WALL or CELL_SEARCHED_FLOOR plus
             * the step that entered it. Both bases are multiples of 4 and a
             * step is 0-3, so the lowest 2 bits are the step. */
            step = *cell & 3;
            *cell = MT_MOON_CELL_FLOOR;
            cx -= sStepDx[step];
            cy -= sStepDy[step];
        }
    }

    /* Give every other cell its value back (the wall frame is wall). */
    for (i = 0; i < count; i++)
    {
        u8 value = sCaveGrid[i];

        if (value == MT_MOON_CELL_VISITED || value >= CELL_SEARCHED_FLOOR)
            sCaveGrid[i] = MT_MOON_CELL_FLOOR;
        else if (value >= CELL_SEARCHED_WALL)
            sCaveGrid[i] = MT_MOON_CELL_WALL;
    }
    sCaveGrid[CELL_INDEX(sCave.entrance.x, sCave.entrance.y)] = MT_MOON_CELL_FLOOR;
    sCaveGrid[CELL_INDEX(sCave.exit.x, sCave.exit.y)] = MT_MOON_CELL_FLOOR;
    return found;
}

/*
 * Make every floor cell of `ladder`'s footprint (apart from the ladder
 * itself) reachable from the entrance.
 *
 * Each CarveCorridorToEntrance call starts with a flood fill over the whole
 * floor, and B2F's ladders have 10 such cells each, so the usual case (the
 * style already connected them all) is checked with a single flood fill
 * first: on a B2F that is half floor, one fill per cell cost several frames
 * of load time (test P9). Only a footprint that isn't connected yet is
 * carved cell by cell.
 */
static bool8 ConnectFootprint(const struct MtMoonLadder *ladder)
{
    const struct LadderKind *kind = &sLadderKinds[ladder->kind];
    s32 i;
    s32 marked = MarkWalkableFromEntrance();
    bool8 connected = IsFootprintVisited(ladder);

    ClearWalkableMarks(marked);
    if (connected)
        return TRUE;
    for (i = 0; i < kind->cellCount; i++)
    {
        if (IsFootprintFloor(&kind->cells[i])
            && !CarveCorridorToEntrance(ladder->x + kind->cells[i].dx, ladder->y + kind->cells[i].dy))
            return FALSE;
    }
    return TRUE;
}

/*
 * Rule 2 of THE FOSSIL CHAMBER (mt_moon_gen_style.h): check where the style
 * left the chamber, stamp its cells and carve the corridors. While the
 * ladders are reconnected, the chamber's floor is BLOCKED, so they connect
 * around it, never through it: the chamber stays a side room, never the
 * only way across the floor. Then the chamber's foot is connected too.
 * Returns FALSE if the shape can't take the chamber (GenerateFloor then
 * asks the style for a new one).
 */
static bool8 PlaceChamber(void)
{
    const struct MtMoonChamber *chamber = &Work()->chamber;
    bool8 connected;
    s32 footX, footY;

    if (!IsChamberSpotAllowed(chamber->x, chamber->y))
        return FALSE;
    MtMoonGen_MarkChamberSite();
    HideChamberFloor();
    connected = ConnectFootprint(&sCave.entrance) && ConnectFootprint(&sCave.exit);
    ShowChamberFloor();
    if (!connected)
        return FALSE;
    MtMoonGen_GetChamberFoot(&footX, &footY);
    return CarveCorridorToEntrance(footX, footY);
}

/* Draw the chamber's vanilla tiles over whatever the style drew there,
 * straight into the map buffer (see DrawCave). */
static void DrawChamber(void)
{
    const struct MtMoonChamber *chamber = &Work()->chamber;
    s32 dx, dy;

    for (dy = 0; dy < MT_MOON_CHAMBER_HEIGHT; dy++)
    {
        u16 *dest = MapBufferCell(chamber->x, chamber->y + dy);

        for (dx = 0; dx < MT_MOON_CHAMBER_WIDTH; dx++)
            dest[dx] = GetChamberTile(dx, dy);
    }
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
 *
 * The fossil chamber is never touched (PlaceChamber has connected it, so
 * none of it is a pocket anyway): its floor is hidden while the pockets
 * are filled (HideChamberFloor).
 */
static void FillPockets(void)
{
    s32 i;
    s32 count = (sCave.width + 2) * (sCave.height + 2);
    s32 marked = MarkWalkableFromEntrance();

    if (HasChamber())
        HideChamberFloor();
    /* The reached cells are VISITED now, so the FLOOR ones left are the
     * pockets. (The wall frame is wall, so the loop can run over it.) */
    for (i = 0; i < count; i++)
    {
        if (sCaveGrid[i] == MT_MOON_CELL_FLOOR)
            sCaveGrid[i] = MT_MOON_CELL_WALL;
    }
    ClearWalkableMarks(marked);
    if (HasChamber())
        ShowChamberFloor();
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
 * The stamped one-tile prefabs (MtMoonGen_StampTile) are written on top of
 * whatever the style drew there, and the fossil chamber (DrawChamber) last
 * of all. The decorations come after this (DecorateCave), drawn straight
 * into the map buffer.
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
        u16 *dest = MapBufferCell(0, y);

        for (x = 0; x < mapW; x++)
            dest[x] = pickTile(&sCave, x, y);
    }
    for (i = 0; i < stamps->count; i++)
    {
        const struct MtMoonStamp *stamp = &stamps->stamps[i];

        *MapBufferCell(stamp->x, stamp->y) = stamp->tile;
    }
    if (HasChamber())
        DrawChamber();
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
 *   - coordinate events (trigger tiles): copied whole. B2F's one trigger,
 *     Miguel's, lies in the fossil chamber's vanilla block, so it moves
 *     with the chamber, keeping its place right next to Miguel. (Mt. Moon
 *     has no other trigger; one outside a chamber would keep its vanilla
 *     tile.)
 *   - objects keep pointing at the ROM; they get their generated positions
 *     in the save block's templates instead (PlaceContent).
 */
static void SetUpEvents(const struct MtMoonFloor *floor, const struct MapEvents *rom)
{
    s32 i;
    s32 warpCount = rom->warpCount;
    s32 coordCount = rom->coordEventCount;
    s32 bgCount = rom->bgEventCount;

    /* Never true on Mt. Moon (B1F has the most warps and bg events, B2F
     * the one trigger); just a guard against overrunning the RAM tables if
     * the maps ever change. */
    if (warpCount > MAX_WARPS)
        warpCount = MAX_WARPS;
    if (coordCount > MAX_COORD_EVENTS)
        coordCount = MAX_COORD_EVENTS;
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
    for (i = 0; i < coordCount; i++)
    {
        sMtMoonCoordEvents[i] = rom->coordEvents[i];
        if (HasChamber() && IsInChamberSource(rom->coordEvents[i].x, rom->coordEvents[i].y))
        {
            sMtMoonCoordEvents[i].x = ChamberX(rom->coordEvents[i].x);
            sMtMoonCoordEvents[i].y = ChamberY(rom->coordEvents[i].y);
        }
    }
    for (i = 0; i < bgCount; i++)
        sMtMoonBgEvents[i] = rom->bgEvents[i];

    sMtMoonEvents.warps = sMtMoonWarps;
    sMtMoonEvents.warpCount = warpCount;
    sMtMoonEvents.coordEvents = sMtMoonCoordEvents;
    sMtMoonEvents.coordEventCount = coordCount;
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
 * or the single tile of a hidden item or sign. A spot is good (IsGoodSpot
 * checks rules 1-5, StyleAllowsPrefab rule 6) when:
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
 *      vanilla's sign does);
 *   5. on a floor with the fossil chamber, the box is at least 2 tiles from
 *      the chamber's box (GetChamberBox), so nothing is placed in the
 *      chamber, and nothing in the free tile around it, the tile in front
 *      of its landing included. Because the box's ring then lies outside
 *      the chamber too, rule 4's way around a piece never goes through the
 *      chamber: the ladders stay connected without it (PlaceChamber).
 *   6. a hidden item or sign only goes where the floor's style can draw
 *      around it: its canStampAt, if it has one, returns TRUE for the tile
 *      (see ONE-TILE PREFABS in mt_moon_gen_style.h).
 * Since every placement keeps the floor in one piece (rule 4) and no later
 * piece can take a tile next to an earlier one (rule 3), every rule above
 * still holds once the last piece is placed. IsContentReachable then checks
 * the result with one flood fill anyway.
 *
 * The fossil chamber's own content comes first and isn't placed by these
 * rules: Miguel and the two fossils simply go to their places in the
 * chamber (PlaceChamberObjects), the vanilla ones moved by the chamber's
 * offset, and their tiles are taken like any piece's.
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
 * decorations are placed (DecorateCave keeps them free) and the trainers
 * have been turned (FaceStationaryTrainers: objects block sight lines),
 * then become floor again (ReleaseObjectTiles), because the
 * Continue safety net needs the real terrain. Hidden items and signs stay
 * BLOCKED: their boulder and signpost tiles are solid.
 *
 * LOAD TIME: on a cramped floor many pieces need ways b and c, which look
 * at every candidate, so those loops are kept lean: they never look at a
 * wall tile, rule checks read the grid directly, and the cheapest rules
 * run first.
 */

/* An object's tile while content is placed: not FLOOR, so flood fills and
 * the rules above treat it as taken, and not WALL. It is still there while
 * the floor is drawn and decorated (DrawCave, DecorateCave), where the
 * style's pickTile counts it as floor, as it must every cell that isn't
 * WALL (THE GRID in mt_moon_gen_style.h), and gone afterwards
 * (ReleaseObjectTiles). */
#define CELL_OCCUPIED 4

#define LADDER_CLEARANCE 3   /* rule 2 */
#define PIECE_SPACING    2   /* rule 3 */
#define CHAMBER_SPACING  2   /* rule 5 */
#define RANDOM_TRIES     64  /* way a */

/* The biggest ring: around a 31 x 31 box, the largest a movement range
 * (4 bits, so at most 15 tiles each way) allows. ReadRing's buffer is this
 * many bytes of stack. */
#define MAX_RING_TILES (4 * 31 + 4)

/* What a piece needs: its movement (which sets its box) and whether its
 * whole ring must be open floor. needOpenRing is TRUE exactly for the
 * one-tile prefabs (hidden items and signs), which rule 6 also applies to. */
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

/* Rules 2, 3 and 5, with the given clearance from the ladders. */
static bool8 IsClearOfLaddersAndPieces(const struct Box *box, s32 ladderClearance, const struct Placement *placement)
{
    s32 i;

    if (IsNearLadder(box, &sCave.entrance, ladderClearance) || IsNearLadder(box, &sCave.exit, ladderClearance))
        return FALSE;
    if (placement->hasChamber && BOXES_CLOSER_THAN(box, &placement->chamberBox, CHAMBER_SPACING))
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

/* Rule 6: a one-tile prefab only goes where the style's canStampAt (if it
 * has one) agrees. Asked last, after the core's own rules. */
static bool8 StyleAllowsPrefab(const struct PieceShape *shape, const struct Placement *placement, s32 x, s32 y)
{
    bool8 (*canStampAt)(const struct MtMoonCave *cave, s32 x, s32 y) = placement->style->canStampAt;

    return !shape->needOpenRing || canStampAt == NULL || canStampAt(&sCave, x, y);
}

/* Rules 1 to 5, cheapest first. */
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
 * pieces and the fossil chamber must hold (so the carving never reaches
 * the chamber), nothing stamped or occupied may be inside, and at
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
        return IsGoodSpot(&box, shape, placement) && StyleAllowsPrefab(shape, placement, x, y);
    return CanCarveAround(&box, placement) && StyleAllowsPrefab(shape, placement, x, y);
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
        if (CanCarveAround(&box, placement) && StyleAllowsPrefab(shape, placement, x, y))
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

/* List every free floor tile as a candidate (see struct Placement), apart
 * from those rule 5 rules out for any piece: the tiles in or right next to
 * the fossil chamber. */
static void ListCandidates(struct Placement *placement)
{
    s32 x, y;
    struct Box tile;

    placement->candidates = FloodQueue();
    placement->candidateCount = 0;
    for (y = 0; y < sCave.height; y++)
    {
        const u8 *row = GRID_ROW(y);

        for (x = 0; x < sCave.width; x++)
        {
            if (row[x] != MT_MOON_CELL_FLOOR)
                continue;
            tile.x1 = tile.x2 = x;
            tile.y1 = tile.y2 = y;
            if (placement->hasChamber && BOXES_CLOSER_THAN(&tile, &placement->chamberBox, CHAMBER_SPACING))
                continue;
            placement->candidates[placement->candidateCount++] = x | (y << 8);
        }
    }
}

/* TRUE if object i of the floor moves with the fossil chamber (see
 * IsChamberObjectTemplate), decided by its vanilla (ROM) position. */
static bool8 IsChamberObject(s32 i)
{
    return HasChamber() && IsChamberObjectTemplate(&sMtMoonEvents.objectEvents[i]);
}

/* Put the fossil chamber's objects in their places in the chamber: their
 * vanilla positions, moved by the chamber's offset from vanilla. Their
 * tiles are taken like any piece's, so the final check (IsContentReachable)
 * makes sure the player can stand next to each of them. */
static void PlaceChamberObjects(struct Placement *placement)
{
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    struct PieceShape shape;
    struct Box box;
    s32 i;

    for (i = 0; i < sMtMoonEvents.objectEventCount; i++)
    {
        if (!IsChamberObject(i))
            continue;
        templates[i].x = ChamberX(sMtMoonEvents.objectEvents[i].x);
        templates[i].y = ChamberY(sMtMoonEvents.objectEvents[i].y);
        shape.movementType = templates[i].objUnion.normal.movementType;
        shape.rangeX = templates[i].objUnion.normal.movementRangeX;
        shape.rangeY = templates[i].objUnion.normal.movementRangeY;
        GetPieceBox(&shape, templates[i].x, templates[i].y, &box);
        TakeBox(placement, &box, CELL_OCCUPIED);
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
 * keeping their vanilla movement types and ranges: first the fossil
 * chamber's objects in their places in it, then the movers, the hidden
 * items and signs, and everything else. Returns FALSE if some piece found
 * no spot; GenerateFloor then tries a new shape.
 */
static bool8 PlaceContent(const struct MtMoonStyle *style)
{
    struct Placement *placement = &Work()->placement;
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    s32 objectCount = sMtMoonEvents.objectEventCount;
    s32 i;

    ResetObjectTemplates(&sMtMoonEvents);
    placement->style = style;
    placement->count = 0;
    placement->hasChamber = HasChamber();
    if (placement->hasChamber)
        GetChamberBox(&placement->chamberBox);
    PlaceChamberObjects(placement);
    ListCandidates(placement);

    for (i = 0; i < objectCount; i++)
    {
        if (templates[i].kind != OBJ_KIND_CLONE && !IsChamberObject(i)
            && IsMovingType(templates[i].objUnion.normal.movementType)
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
        if (templates[i].kind != OBJ_KIND_CLONE && !IsChamberObject(i)
            && !IsMovingType(templates[i].objUnion.normal.movementType)
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
 * (dx, dy) at a time, before the first tile that isn't (a wall, a boulder,
 * signpost or rubble, or a tile an object could stand on). */
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
 * boulders, signposts and rubble, and at every tile an object could stand
 * on, so this runs while the objects' tiles are still OCCUPIED (and after
 * the decorations: rubble is solid). Ties keep the
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
/* Decorations                                                               */
/* ------------------------------------------------------------------------- */

/*
 * DECORATIONS
 *
 * Vanilla Mt. Moon's caves are dotted with small decorations. Each one is a
 * PREFAB: a few vanilla tiles in a fixed arrangement, copied tile for tile
 * (full map cell values, see MAP CELL VALUES in mt_moon_gen_style.h):
 *
 *   RUBBLE  loose rocks on the floor, one solid tile:  0x0683
 *   CRATER  a 2 x 2 dent in the floor, walkable:       0x32d5 0x32d6
 *                                                      0x32dd 0x32de
 *   PUDDLE  water dripping from a wall: two drip       0x06ee 0x06ef  (wall)
 *           tiles take the place of two tiles of a     0x32f6 0x32f7  (floor)
 *           wall's bottom row (solid), over a 2-wide
 *           puddle on the floor below them (walkable).
 *
 * Their metatile behaviours (data/tilesets/secondary/cave/
 * metatile_attributes.bin) are those of plain wall and cave floor: MB_NORMAL
 * for the solid rubble and drips, MB_CAVE (like floor 0x281) for the
 * craters and the puddle. None of them is one-way or special in any other
 * way, so the grid, which only knows wall and floor, describes them fully.
 *
 * HOW MANY. For each floor and kind, a random TARGET between half and one
 * and a half times as many as the floor's vanilla map has, rounded inward
 * to whole numbers: from ceil(v / 2) to floor(3v / 2) for a vanilla count
 * v (1F has 19 rubble tiles, 6 craters and 2 puddles, B1F 14 / 1 / 2, B2F
 * 7 / 4 / 2; so 1F gets 10-28 rubble tiles, 3-9 craters and 1-3 puddles).
 * What the player sees is what counts, so decorations already on the
 * floor count too and the target is only topped up, never cut: B1F's
 * junction rubble (its style stamps it, see JUNCTIONS in
 * src/mt_moon_style_b1f.c) and the rubble and puddle inside B2F's fossil
 * chamber (part of the vanilla prefab, and of vanilla's counts). A floor
 * with fewer spots than its target gets as many as fit: the step doesn't
 * throw a shape away for its decorations. The styles' shapes leave room
 * (in a prototype over 100 caves per floor, the scarcest were B1F's
 * crater and 1F's puddles, with at least 2 spots where 1 is needed), and
 * test P10 checks every count is in its band.
 *
 * WHERE (DecorationFits). A decoration may go anywhere it fits:
 *   D1. each of its walkable tiles and its rubble covers free floor (not a
 *       tile an object may stand on, a boulder, a signpost or another
 *       decoration), and each drip tile covers wall;
 *   D2. its box is 3+ tiles (king moves, diagonal steps counting as one)
 *       from both ladders (LADDER_CLEARANCE), so it never touches a
 *       ladder's footprint, B2F's 3 x 4 block of floor included;
 *   D3. its box is 2+ tiles from the fossil chamber's box
 *       (CHAMBER_SPACING), so it stays out of the chamber and the free ring
 *       around it, like every piece of content (rule 5 of CONTENT
 *       PLACEMENT);
 *   D4. no seams: every tile north, south, west or east of it is a tile the
 *       floor's own vanilla map puts on that side of that decoration tile
 *       (THE VANILLA NEIGHBOURS below), which is what test P5 checks. This
 *       rule is what decides where each kind goes on each floor. On 1F,
 *       rubble always hangs below a rock face, more rubble or a puddle
 *       (vanilla 1F never has floor north of rubble), and puddles drip from
 *       the south face of a rock island. On B1F, puddles drip from north
 *       walls and the crater lies against the void west of a room (vanilla
 *       B1F's one crater does). On B2F, rubble stands alone (B2F never puts
 *       rubble beside rubble) and puddles drip from a room's south wall.
 *   D5. rubble, which is solid, keeps the floor in one piece (rule 4 of
 *       CONTENT PLACEMENT, KeepsFloorConnected; its ring lies outside the
 *       chamber by D3, so the chamber still never becomes the only way
 *       across) and never covers a tile north, south, west or east of a
 *       piece of content, so every piece can still be faced from every tile
 *       it could before.
 * Rubble comes in small clusters, as in vanilla: after each one, up to two
 * more may follow beside it (east, west or south), if they fit too.
 *
 * Puddles are placed first, as they fit in the fewest places, then craters,
 * then rubble (which may then also hang below a puddle, as on vanilla 1F).
 *
 * FINDING SPOTS (PlaceDecorations). For each kind, random tiles first:
 * cheap where the kind fits on many tiles (rubble and craters fit on 1
 * tile in 4 to 15, except B1F's crater). If those don't find enough, one pass
 * over the map lists the tiles where the decoration may fit, by the
 * quickest tests (the grid cells under its first and last tile, the tiles
 * north and west of its first tile), and the rest are picked from that
 * list at random. Puddles and B1F's crater fit on about 1 tile in 100, so
 * they mostly come from the list. Every pick is checked against every rule
 * before it is put down.
 *
 * WHEN: AFTER DRAWING, BEFORE THE FINAL CHECK. D4 compares a decoration
 * with the tiles drawn around it: the style's, the stamped boulders and
 * signposts, earlier decorations. Once the cave is drawn (DrawCave), all of
 * those are in the map buffer (VMap), so DecorateCave reads them there, and
 * writes each decoration both into the map buffer and into the stamp list
 * (MtMoonGen_StampTile, which keeps the grid's walls and floor matching
 * what is drawn: rubble cells become BLOCKED). GenerateFloor calls it right
 * after DrawCave, with the objects' tiles still marked taken
 * (CELL_OCCUPIED), and then runs content placement's final reachability
 * check (IsContentReachable) with the rubble in place. D5 makes that check
 * pass by construction; if it ever failed, the shape would be thrown away
 * like any other failure (the next attempt draws the map afresh).
 * The step draws its random numbers after every step of the shape and the
 * content, so a seed builds the same shape, ladders and content with or
 * without decorations, unless an earlier attempt got as far as decorating
 * and then failed the final check (never seen: its decorations' random
 * numbers would shift every later attempt's). Only the trainers' facing
 * can change (FaceStationaryTrainers runs after this step, so rubble in a
 * trainer's line of sight counts).
 *
 * THE VANILLA NEIGHBOURS (ReadVanillaNeighbours). D4 needs, for each of
 * the 9 decoration tiles and each side of it, the metatiles the floor's
 * vanilla map puts there. They are read at the start of the step from that
 * map itself (data/layouts/MtMoon_<floor>/map.bin, which is in ROM as
 * gMapHeader.mapLayout->map), around each of the map's decorations, which
 * the floor table lists (sVanillaDecorations1F, ...), so the counts of HOW
 * MANY come from the same list. So the rule is exactly the floor's own
 * vanilla pairs, and no table of pairs is written by hand.
 *
 * LOAD TIME: the step costs about 1.5 to 3 frames on average (test P9
 * allows a floor 30 frames more than loading a vanilla cave, and the
 * floors took at most 22 before it). On the GBA every pass over the map's
 * ~2,000 tiles costs most of a frame however little it does per tile (a
 * function call per tile costs a few), so the step reads only the listed
 * vanilla decorations, counts the decorations already drawn from the stamp
 * list and the chamber, places most decorations by random tries, and makes
 * a pass over the map only for a kind that random tries don't place, with
 * the quickest tests written out in its loop.
 */

/* The decoration tiles (DTILE_*) as full map cell values: solid tiles are
 * collision 1, elevation 0 (0x06xx); walkable ones collision 0, elevation
 * 3 (0x32xx), the floor's height. */
static const u16 sDecorationTiles[DTILE_COUNT] =
{
    [DTILE_RUBBLE]    = 0x0683,
    [DTILE_CRATER_NW] = 0x32d5,
    [DTILE_CRATER_NE] = 0x32d6,
    [DTILE_CRATER_SW] = 0x32dd,
    [DTILE_CRATER_SE] = 0x32de,
    [DTILE_DRIP_W]    = 0x06ee,
    [DTILE_DRIP_E]    = 0x06ef,
    [DTILE_PUDDLE_W]  = 0x32f6,
    [DTILE_PUDDLE_E]  = 0x32f7,
};

/* A decoration prefab: `width` x `height` tiles, listed row by row. */
struct Decoration
{
    u8 width;
    u8 height;
    u8 tiles[4];      /* DTILE_* */
    u8 onWall;        /* bit i set: tile i covers a wall cell (a drip tile)
                       * instead of a floor cell */
    bool8 isSolid;    /* TRUE: it covers floor with a solid tile (rubble),
                       * so rule D5 applies */
    u8 clusterMax;    /* how many may come together (see DECORATIONS) */
};

static const struct Decoration sDecorations[DECORATION_KINDS] =
{
    [DECORATION_PUDDLE] =
    {
        .width = 2, .height = 2,
        .tiles = { DTILE_DRIP_W, DTILE_DRIP_E, DTILE_PUDDLE_W, DTILE_PUDDLE_E },
        .onWall = 0x3,
        .clusterMax = 1,
    },
    [DECORATION_CRATER] =
    {
        .width = 2, .height = 2,
        .tiles = { DTILE_CRATER_NW, DTILE_CRATER_NE, DTILE_CRATER_SW, DTILE_CRATER_SE },
        .clusterMax = 1,
    },
    [DECORATION_RUBBLE] =
    {
        .width = 1, .height = 1,
        .tiles = { DTILE_RUBBLE },
        .isSolid = TRUE,
        .clusterMax = 3,
    },
};

/* The sides of a tile, numbered like sStepDx and sStepDy. */
#define SIDE_NORTH 0
#define SIDE_WEST  2

/* How many random tiles PlaceDecorations tries per decoration it needs
 * before it lists the spots (see FINDING SPOTS in DECORATIONS). */
#define DECORATION_RANDOM_TRIES 16

/* The metatile id of a map cell value (bits 0-9). */
#define METATILE_OF(value) ((value) & MAPGRID_METATILE_ID_MASK)

/* The grid cell decoration tile i of `decoration` must cover (D1): wall for
 * a drip tile, floor for the rest. */
#define CELL_UNDER(decoration, i) \
    ((((decoration)->onWall >> (i)) & 1) ? MT_MOON_CELL_WALL : MT_MOON_CELL_FLOOR)

/* TRUE if offset (dx, dy) from a decoration's top-left tile is one of its
 * own tiles; the tiles around it (the "outer sides" of its tiles) aren't. */
#define IS_IN_DECORATION(decoration, dx, dy) \
    ((dx) >= 0 && (dy) >= 0 && (dx) < (decoration)->width && (dy) < (decoration)->height)

/* Add metatile `metatile` to metatile set `set` (see A SET OF METATILES). */
static void AddToMetatileSet(u32 *set, u16 metatile)
{
    u32 i = metatile - METATILE_SET_BASE;

    if (i < METATILE_SET_WORDS * 32)
        set[i >> 5] |= 1u << (i & 31);
}

/* The metatile of vanilla map tile (x, y), or 0 off the map. */
static u16 GetVanillaMetatile(s32 x, s32 y)
{
    const struct MapLayout *layout = gMapHeader.mapLayout;

    if (x < 0 || y < 0 || x >= layout->width || y >= layout->height)
        return 0;
    return METATILE_OF(layout->map[y * layout->width + x]);
}

/* TRUE if the floor's vanilla map has a decoration of `kind` with its
 * top-left tile on (x, y). */
static bool8 IsVanillaDecoration(s32 kind, s32 x, s32 y)
{
    const struct Decoration *decoration = &sDecorations[kind];
    s32 dx, dy, i;

    for (i = 0, dy = 0; dy < decoration->height; dy++)
    {
        for (dx = 0; dx < decoration->width; dx++, i++)
        {
            if (GetVanillaMetatile(x + dx, y + dy) != METATILE_OF(sDecorationTiles[decoration->tiles[i]]))
                return FALSE;
        }
    }
    return TRUE;
}

/*
 * Read THE VANILLA NEIGHBOURS (see DECORATIONS) from the floor's vanilla
 * map: for each decoration the floor table lists for it (and which the map
 * really has there), count it, and add the metatile on every outer side of
 * each of its tiles to that tile's sets.
 *
 * LOAD TIME: only the listed decorations are read, about 50 tiles and
 * their neighbours. Scanning the whole map for them instead cost about a
 * frame (on the GBA every pass over the map's ~2,000 tiles does).
 */
static void ReadVanillaNeighbours(const struct MtMoonFloor *floor, struct VanillaNeighbours *neighbours)
{
    s32 n, dx, dy, i, side;

    memset(neighbours, 0, sizeof(*neighbours));
    for (i = 0; i < DTILE_COUNT; i++)
        AddToMetatileSet(neighbours->decorationTiles, METATILE_OF(sDecorationTiles[i]));
    for (n = 0; n < floor->decorationCount; n++)
    {
        const struct VanillaDecoration *vanilla = &floor->decorations[n];
        const struct Decoration *decoration = &sDecorations[vanilla->kind];

        if (!IsVanillaDecoration(vanilla->kind, vanilla->x, vanilla->y))
            continue;
        neighbours->vanillaCount[vanilla->kind]++;
        for (i = 0, dy = 0; dy < decoration->height; dy++)
        {
            for (dx = 0; dx < decoration->width; dx++, i++)
            {
                for (side = 0; side < 4; side++)
                {
                    s32 nx = dx + sStepDx[side];
                    s32 ny = dy + sStepDy[side];

                    if (!IS_IN_DECORATION(decoration, nx, ny))
                        AddToMetatileSet(neighbours->allowed[decoration->tiles[i]][side],
                                         GetVanillaMetatile(vanilla->x + nx, vanilla->y + ny));
                }
            }
        }
    }
}

/* Count `tile` (a map cell value) into `counts` if it is the first tile
 * of a decoration kind (each decoration is counted by its top-left tile). */
static void CountDecorationTile(u16 tile, s32 *counts)
{
    s32 kind;

    for (kind = 0; kind < DECORATION_KINDS; kind++)
    {
        if (METATILE_OF(tile) == METATILE_OF(sDecorationTiles[sDecorations[kind].tiles[0]]))
            counts[kind]++;
    }
}

/*
 * How many decorations of each kind the floor already has before this
 * step (see HOW MANY): B1F's junction rubble and the fossil chamber's
 * rubble and puddle. Decoration tiles only reach the drawn map through the
 * stamp list and the chamber's vanilla block (a style that draws one also
 * stamps it, see ONE-TILE PREFABS in mt_moon_gen_style.h), so those are
 * counted, rather than the ~2,000 drawn tiles.
 */
static void CountDecorationsDrawn(s32 *counts)
{
    const struct StampList *list = Stamps();
    const struct MtMoonChamber *chamber = MtMoonGen_GetChamber();
    s32 i, dx, dy;

    for (i = 0; i < DECORATION_KINDS; i++)
        counts[i] = 0;
    for (i = 0; i < list->count; i++)
        CountDecorationTile(list->stamps[i].tile, counts);
    if (chamber == NULL)
        return;
    for (dy = 0; dy < MT_MOON_CHAMBER_HEIGHT; dy++)
    {
        for (dx = 0; dx < MT_MOON_CHAMBER_WIDTH; dx++)
            CountDecorationTile(GetChamberTile(dx, dy), counts);
    }
}

/*
 * TRUE if a tile north, south, west or east of map tile (x, y) belongs to a
 * piece of content, so (x, y) is a tile the player may stand on to face it
 * (D5): an object's tile (CELL_OCCUPIED, a wanderer's whole roaming box
 * included), or a hidden item's boulder or a sign's signpost, stamped as
 * TILE_BOULDER and TILE_SIGNPOST. (The fossil chamber's objects are far
 * away, by D3.)
 */
static bool8 IsNextToContent(s32 x, s32 y)
{
    s32 side;

    for (side = 0; side < 4; side++)
    {
        s32 nx = x + sStepDx[side];
        s32 ny = y + sStepDy[side];
        u16 tile = *MapBufferCell(nx, ny);

        if (GRID_ROW(ny)[nx] == CELL_OCCUPIED || tile == TILE_BOULDER || tile == TILE_SIGNPOST)
            return TRUE;
    }
    return FALSE;
}

/*
 * Rules D1-D5 (see DECORATIONS): can a decoration of `kind` go with its
 * top-left tile on map tile (x, y)? Changes nothing.
 *
 * LOAD TIME: random tries ask this about many tiles where nothing fits, so
 * it first makes the tests that rule out nearly every such tile, with as
 * little set-up as possible: the grid cell under the first tile (D1), and
 * the tiles north and west of the first tile (D4). Only then the rest.
 * The caller passes `placement` and `neighbours` (both in Work()) rather
 * than each call looking them up: Work() costs a few function calls, and
 * this runs hundreds of times per floor.
 */
static bool8 DecorationFits(s32 kind, s32 x, s32 y, const struct Placement *placement,
                            const struct VanillaNeighbours *neighbours)
{
    const struct Decoration *decoration = &sDecorations[kind];
    s32 w = decoration->width;
    s32 h = decoration->height;
    s32 stride = VMap.Xsize;
    const u16 *drawn;
    s32 dx, dy, side, i;
    struct Box box;

    /* Inside the grid with a tile to spare all round (D4 and D5 read the
     * tiles around it). */
    if (x < 1 || y < 1 || x + w > sCave.width - 1 || y + h > sCave.height - 1)
        return FALSE;

    /* The quick tests. `drawn` points at the decoration's top-left tile in
     * the map buffer: drawn[-stride] is the tile north of it, drawn[-1]
     * the tile west of it, drawn[dy * stride + dx] its tile (dx, dy). */
    drawn = MapBufferCell(x, y);
    if (GRID_ROW(y)[x] != CELL_UNDER(decoration, 0)
        || !IN_METATILE_SET(neighbours->allowed[decoration->tiles[0]][SIDE_NORTH], METATILE_OF(drawn[-stride]))
        || !IN_METATILE_SET(neighbours->allowed[decoration->tiles[0]][SIDE_WEST], METATILE_OF(drawn[-1])))
        return FALSE;

    /* D1: floor under the floor tiles, wall under the drips, and no
     * decoration this step already put down. (Rubble makes its cell
     * BLOCKED, which the grid test rules out; a crater or a puddle leaves
     * its cells FLOOR and WALL, and an exact copy on top of one would pass
     * D4.) */
    for (i = 0, dy = 0; dy < h; dy++)
    {
        const u8 *row = GRID_ROW(y + dy);

        for (dx = 0; dx < w; dx++, i++)
        {
            if (row[x + dx] != CELL_UNDER(decoration, i)
                || IN_METATILE_SET(neighbours->decorationTiles, METATILE_OF(drawn[dy * stride + dx])))
                return FALSE;
        }
    }

    /* D4: every outer side of the prefab against the tile drawn there
     * (inside the prefab, the pairs are vanilla's own). */
    for (i = 0, dy = 0; dy < h; dy++)
    {
        for (dx = 0; dx < w; dx++, i++)
        {
            const u32 (*allowed)[METATILE_SET_WORDS] = neighbours->allowed[decoration->tiles[i]];

            for (side = 0; side < 4; side++)
            {
                s32 nx = dx + sStepDx[side];
                s32 ny = dy + sStepDy[side];

                if (!IS_IN_DECORATION(decoration, nx, ny)
                    && !IN_METATILE_SET(allowed[side], METATILE_OF(drawn[ny * stride + nx])))
                    return FALSE; /* vanilla never puts that tile there */
            }
        }
    }

    /* D2 and D3. */
    box.x1 = x;
    box.y1 = y;
    box.x2 = x + w - 1;
    box.y2 = y + h - 1;
    if (IsNearLadder(&box, &sCave.entrance, LADDER_CLEARANCE) || IsNearLadder(&box, &sCave.exit, LADDER_CLEARANCE))
        return FALSE;
    if (placement->hasChamber && BOXES_CLOSER_THAN(&box, &placement->chamberBox, CHAMBER_SPACING))
        return FALSE;

    /* D5. */
    if (decoration->isSolid)
        return !IsNextToContent(x, y) && KeepsFloorConnected(&box, FALSE);
    return TRUE;
}

/* Draw a decoration of `kind` with its top-left tile on map tile (x, y):
 * into the map buffer, and as stamps, so the grid follows (rubble cells
 * become BLOCKED; drip tiles leave their wall cells WALL). PlaceDecorations
 * has made sure the stamp list has room. */
static void PutDecoration(s32 kind, s32 x, s32 y)
{
    const struct Decoration *decoration = &sDecorations[kind];
    s32 dx, dy, i;

    for (i = 0, dy = 0; dy < decoration->height; dy++)
    {
        for (dx = 0; dx < decoration->width; dx++, i++)
        {
            u16 tile = sDecorationTiles[decoration->tiles[i]];

            MtMoonGen_StampTile(x + dx, y + dy, tile);
            *MapBufferCell(x + dx, y + dy) = tile;
        }
    }
}

/* Put a decoration on (x, y), which fits, and, for a kind that comes in
 * clusters (rubble), maybe more next to it (east, west or south of the last
 * one, each with a 1 in 2 chance, while they fit), up to its clusterMax
 * and never more than `room`. Returns how many it put. */
static s32 PutDecorationGroup(s32 kind, s32 x, s32 y, s32 room, const struct Placement *placement,
                              const struct VanillaNeighbours *neighbours)
{
    s32 put = 1;
    s32 step;

    PutDecoration(kind, x, y);
    while (put < room && put < sDecorations[kind].clusterMax && MtMoonGen_RandomBelow(2) == 0)
    {
        step = 1 + MtMoonGen_RandomBelow(3);  /* sStepDx/Dy: south, west or east */
        x += sStepDx[step];
        y += sStepDy[step];
        if (!DecorationFits(kind, x, y, placement, neighbours))
            break;
        PutDecoration(kind, x, y);
        put++;
    }
    return put;
}

/*
 * List every tile where a decoration of `kind` may fit, into `spots`
 * (packed as x | y << 8, both under 256), and return how many there are.
 * A tile is listed if the grid cells under the decoration's first and last
 * tile are right (D1) and the tiles north and west of its first tile are
 * ones vanilla puts there (D4); the other rules are checked when a spot is
 * picked. These tests are written out in the loop: a DecorationFits call
 * per map tile would cost a few frames.
 */
static s32 ListDecorationSpots(s32 kind, u16 *spots, const struct VanillaNeighbours *neighbours)
{
    const struct Decoration *decoration = &sDecorations[kind];
    s32 w = decoration->width;
    s32 h = decoration->height;
    u8 firstCell = CELL_UNDER(decoration, 0);
    u8 lastCell = CELL_UNDER(decoration, w * h - 1);
    const u32 *northOk = neighbours->allowed[decoration->tiles[0]][SIDE_NORTH];
    const u32 *westOk = neighbours->allowed[decoration->tiles[0]][SIDE_WEST];
    s32 count = 0;
    s32 x, y;

    for (y = 1; y + h <= sCave.height - 1; y++)
    {
        const u8 *top = GRID_ROW(y);
        const u8 *bottom = GRID_ROW(y + h - 1);
        const u16 *above = MapBufferCell(0, y - 1);
        const u16 *here = MapBufferCell(0, y);

        for (x = 1; x + w <= sCave.width - 1; x++)
        {
            if (top[x] == firstCell && bottom[x + w - 1] == lastCell
                && IN_METATILE_SET(northOk, METATILE_OF(above[x]))
                && IN_METATILE_SET(westOk, METATILE_OF(here[x - 1])))
                spots[count++] = x | (y << 8);
        }
    }
    return count;
}

/*
 * Put `need` decorations of `kind` where they fit (see FINDING SPOTS in
 * DECORATIONS), or as many as there is room for: first at random tiles;
 * then, if those didn't find enough, list the tiles where one may fit and
 * pick from the list at random. Every pick is checked against every rule
 * (DecorationFits), as earlier ones may have covered a spot or changed the
 * tiles around it.
 *
 * The list lives in the flood-fill queue's memory, which has room for one
 * spot per map tile: no flood fill runs while decorations are placed.
 */
static void PlaceDecorations(s32 kind, s32 need, const struct Placement *placement,
                             const struct VanillaNeighbours *neighbours)
{
    s32 size = sDecorations[kind].width * sDecorations[kind].height;
    s32 stampsLeft = MAX_STAMPS - Stamps()->count;
    u16 *spots = FloodQueue();
    s32 placed = 0;
    s32 room, tries, spotCount, j, x, y;
    u16 random;

    /* Never more than the stamp list has room for, worked out once here
     * (the loops below run hundreds of times): stampsLeft / size, by
     * repeated subtraction, as the GBA's CPU can't divide. */
    for (room = 0; stampsLeft >= size; stampsLeft -= size)
        room++;
    if (need > room)
        need = room;

    for (tries = need * DECORATION_RANDOM_TRIES; tries > 0 && placed < need; tries--)
    {
        /* One random number gives both coordinates: its low byte (0-255)
         * scaled to the width, its high byte to the height (byte * size /
         * 256, with a shift instead of a division). */
        random = MtMoonGen_Random();
        x = ((random & 0xFF) * sCave.width) >> 8;
        y = ((random >> 8) * sCave.height) >> 8;
        if (DecorationFits(kind, x, y, placement, neighbours))
            placed += PutDecorationGroup(kind, x, y, need - placed, placement, neighbours);
    }
    if (placed == need)
        return;

    spotCount = ListDecorationSpots(kind, spots, neighbours);
    while (placed < need && spotCount > 0)
    {
        j = MtMoonGen_RandomBelow(spotCount);
        x = spots[j] & 0xFF;
        y = spots[j] >> 8;
        spots[j] = spots[--spotCount];
        if (DecorationFits(kind, x, y, placement, neighbours))
            placed += PutDecorationGroup(kind, x, y, need - placed, placement, neighbours);
    }
}

/*
 * Add the floor's decorations to the drawn cave (see DECORATIONS): choose
 * each kind's target and place what is missing. Called right after
 * DrawCave, with the objects' tiles still CELL_OCCUPIED and before
 * content placement's final check (IsContentReachable).
 */
static void DecorateCave(const struct MtMoonFloor *floor)
{
    struct VanillaNeighbours *neighbours = &Work()->neighbours;
    const struct Placement *placement = &Work()->placement;
    s32 alreadyDrawn[DECORATION_KINDS];
    s32 target[DECORATION_KINDS];
    s32 kind, low, high;

    ReadVanillaNeighbours(floor, neighbours);
    CountDecorationsDrawn(alreadyDrawn);
    /* The targets: from ceil(v / 2) to floor(3v / 2) for vanilla's count
     * v. Shifting right by 1 halves a number, rounding down. */
    for (kind = 0; kind < DECORATION_KINDS; kind++)
    {
        low = (neighbours->vanillaCount[kind] + 1) >> 1;
        high = (neighbours->vanillaCount[kind] * 3) >> 1;
        target[kind] = low + MtMoonGen_RandomBelow(high - low + 1);
    }
    for (kind = 0; kind < DECORATION_KINDS; kind++)
    {
        if (target[kind] > alreadyDrawn[kind])
            PlaceDecorations(kind, target[kind] - alreadyDrawn[kind], placement, neighbours);
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
 * Copy the floor's vanilla map back into the map buffer, the way the map
 * loader put it there before generation (InitBackupMapLayoutData in
 * src/fieldmap.c), after a thrown-away attempt drew into it: the fallback
 * when no shape works.
 *
 * It goes a row at a time because the two are laid out differently: the
 * vanilla map (in ROM) is `width` cells per row, while a row of the map
 * buffer is width + 2 * MAP_OFFSET cells, with the margin on both sides
 * (MapBufferCell finds a row's first map tile). CpuCopy16 is the GBA BIOS's
 * memory-copy routine (CpuSet), copying the given number of bytes 16 bits
 * at a time, much faster than a C loop.
 */
static void RestoreVanillaMap(void)
{
    const struct MapLayout *layout = gMapHeader.mapLayout;
    s32 y;

    for (y = 0; y < layout->height; y++)
        CpuCopy16(&layout->map[y * layout->width], MapBufferCell(0, y), layout->width * sizeof(u16));
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
 * Both are should-never-happen fallbacks: the block is at most about
 * 9.6 KB of a 112 KB heap, and the property test checks every style on
 * 100 seeds per floor.)
 */
static bool8 GenerateFloor(const struct MtMoonFloor *floor, u16 seed)
{
    s32 attempt;
    const struct MapEvents *rom = gMapHeader.events; /* the map loader set it from the ROM header */

    if (!AllocCaveGrid(gMapHeader.mapLayout->width, gMapHeader.mapLayout->height, floor->style->workBytes))
        return FALSE;
    Work()->hasChamber = floor->hasChamber;

    SeedCaveRng(seed, floor->floorNumber);
    for (attempt = 0; attempt < MAX_SHAPE_ATTEMPTS; attempt++)
    {
        ClearCaveGrid();
        InitLadder(&sCave.entrance, &floor->entrance);
        InitLadder(&sCave.exit, &floor->exit);
        ProposeLadders();
        if (floor->hasChamber)
            ProposeChamber();
        floor->style->buildShape(&sCave);
        if (!CheckLadders(floor))
            continue;
        if (floor->hasChamber && !PlaceChamber())
            continue;
        FillPockets();
        SetUpEvents(floor, rom);
        if (!PlaceContent(floor->style))
            continue;
        /* Every step above may change the grid (fill, stamp, carve), so
         * the map is drawn only now. The decorations go on the drawn map
         * (they look at the tiles drawn around them), and before the final
         * check, which so runs with their rubble in place. The objects'
         * tiles stay taken (CELL_OCCUPIED) until the trainers are turned;
         * the style draws them as the floor they are. */
        DrawCave(floor);
        DecorateCave(floor);
        if (IsContentReachable())
            break;
    }
    if (attempt == MAX_SHAPE_ATTEMPTS)
    {
        /* Undo what a failed attempt may have changed outside the grid. */
        gMapHeader.events = rom;
        ResetObjectTemplates(rom);
        RestoreVanillaMap();
        return FALSE;
    }
    FaceStationaryTrainers();
    ReleaseObjectTiles();
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

/*
 * MIGUEL AFTER CONTINUE
 *
 * After the player takes a fossil, Miguel's script walks him to the other
 * one and ends with `copyobjectxytoperm`, which writes the tile he walked
 * to into his object template, the position he is drawn at whenever he is
 * (re)spawned. The save keeps that template, and vanilla keeps him there
 * until the player leaves the floor (a fresh entry starts him at his
 * chamber position again). Generation resets every template to vanilla,
 * so on Continue the chamber's objects get their saved position back,
 * once a fossil has been taken (FLAG_GOT_FOSSIL_FROM_MT_MOON). Only then:
 * both fossils are gone by that point, so wherever Miguel stands in the
 * chamber he can't keep the player from anything. And only to a walkable
 * tile of the chamber, the only place his script can take him (a save from
 * before the chamber existed may hold any position).
 *
 * The saved positions are read before generation resets them, for up to
 * MAX_CHAMBER_OBJECTS chamber objects (B2F has exactly 3: Miguel and the
 * two fossils).
 */
#define MAX_CHAMBER_OBJECTS 3

/* Remember the saved template position of each chamber object of `floor`,
 * in the order of the floor's object events. Call before GenerateFloor. */
static void RememberChamberObjects(const struct MtMoonFloor *floor, struct Coords16 *saved)
{
    const struct MapEvents *rom = gMapHeader.events;
    s32 i, kept = 0;

    if (!floor->hasChamber)
        return;
    for (i = 0; i < rom->objectEventCount && kept < MAX_CHAMBER_OBJECTS; i++)
    {
        if (IsChamberObjectTemplate(&rom->objectEvents[i]))
        {
            saved[kept].x = gSaveBlock1Ptr->objectEventTemplates[i].x;
            saved[kept].y = gSaveBlock1Ptr->objectEventTemplates[i].y;
            kept++;
        }
    }
}

/* Put the chamber objects back on their saved positions (see MIGUEL AFTER
 * CONTINUE). Call after GenerateFloor, before the grid is freed. */
static void RestoreChamberObjects(const struct Coords16 *saved)
{
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    const struct MtMoonChamber *chamber = MtMoonGen_GetChamber();
    s32 i, kept = 0;

    if (chamber == NULL || !FlagGet(FLAG_GOT_FOSSIL_FROM_MT_MOON))
        return;
    for (i = 0; i < sMtMoonEvents.objectEventCount && kept < MAX_CHAMBER_OBJECTS; i++)
    {
        s32 dx, dy;

        if (!IsChamberObject(i))
            continue;
        dx = saved[kept].x - chamber->x;
        dy = saved[kept].y - chamber->y;
        kept++;
        if (dx >= 0 && dx < MT_MOON_CHAMBER_WIDTH && dy >= 0 && dy < MT_MOON_CHAMBER_HEIGHT
            && !IS_SOLID_TILE(GetChamberTile(dx, dy)))
        {
            templates[i].x = chamber->x + dx;
            templates[i].y = chamber->y + dy;
        }
    }
}

void MtMoonGen_RegenerateFromSave(void)
{
    const struct MtMoonFloor *floor = GetCurrentFloor();
    struct Coords16 saved[MAX_CHAMBER_OBJECTS];

    if (floor == NULL)
        return;
    RememberChamberObjects(floor, saved);
    if (GenerateFloor(floor, VarGet(VAR_MT_MOON_MAP_SEED)))
    {
        RestoreChamberObjects(saved);
        MovePlayerOntoReachableFloor();
    }
    FreeCaveGrid();
}
