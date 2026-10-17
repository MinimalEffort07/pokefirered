/*
 * mt_moon_style_b2f.c - Mt. Moon B2F style: rooms divided by pink ridges
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * The floor style for the generated Mt. Moon B2F (see
 * include/mt_moon_gen_style.h for how a style plugs into the generator core
 * in src/mt_moon_gen.c). It copies the look of vanilla B2F
 * (data/layouts/MtMoon_B2F/map.bin): a cave divided into rooms by pink-topped
 * RIDGES, rock walls whose top is a pink band and whose south side is a tall
 * cliff face, with small walled ROOMS entered by 2-wide stairs cut into their
 * one-row south wall, and vanilla's fossil chamber as one of those rooms.
 * About 46% of vanilla B2F can be walked on.
 *
 * DRAWING: THE TILE MAP
 *
 * B2F's walls come in more shapes than B1F's (ridges, their ends and
 * crossings, room walls, stairs, rock), and which vanilla tile a wall cell
 * needs depends on which structure it belongs to, not just on the floor
 * around it. So this style decides every map cell's tile while it builds the
 * shape and keeps the result in its work area (MtMoonGen_GetStyleWork, one
 * byte per map cell: a TILE INDEX, T_*, into sTileValues). pickTile then
 * only looks the cell up.
 *
 * Each tile index stands for exactly one vanilla B2F map cell value (metatile
 * id | collision << 10 | elevation << 12, see MAP CELL VALUES in the style
 * header): sTileValues is the hand-written tile table. Every structure below
 * is drawn tile for tile the way vanilla B2F draws it.
 *
 * A tile in a vanilla map only ever touches certain other tiles; putting it
 * next to anything else draws a visible seam. Test P5 compares every pair of
 * neighbouring tiles in the generated B2F with the pairs vanilla B2F's map
 * uses. sEastOk and sSouthOk list, for each tile, which tiles vanilla B2F
 * puts east of it and south of it, and IsDrawable() checks the finished map
 * against them: a layout with any other pair is thrown away, never drawn.
 *
 * THE STRUCTURES (what vanilla B2F is made of)
 *
 * In the pictures, . is cave floor (0x281) and the other letters are walls.
 *
 *   ROCK      The solid rock mass (0x2ce) along the top of the map, in rows
 *             0 and 1 and, inside each of the two top rooms, a few rows
 *             more. Floor may lie right below it, never beside it.
 *
 *   RIDGE     A pink-topped wall. A vertical ridge is 2 columns wide (0x2e6
 *             0x2e7) with floor on both sides. A horizontal ridge is 4 rows
 *             tall: the pink top (0x2df), a ledge (0x2c2) and a 2-row cliff
 *             face (0x2e4) on its south side:
 *
 *                 ......       floor
 *                 TTTTTT       0x2df  pink top
 *                 LLLLLL       0x2c2  ledge
 *                 CCCCCC       0x2e4  cliff face
 *                 CCCCCC       0x2e4  cliff face
 *                 ......       floor
 *
 *             A horizontal ridge ends either against a vertical ridge or
 *             FREE, with an end column of its own: 0x29e/0x31a/0x2e5 at the
 *             east end (floor beyond), 0x29d/0x319/0x2e3 at the west end,
 *             which vanilla only puts next to dithered floor (0x2d9), so a
 *             column of it runs down beside every free west end. A GAP is a
 *             free east end, 3 columns of dithered floor and a free west
 *             end: a way through the ridge.
 *
 *             Where ridges meet, vanilla has a tile pair for each kind of
 *             junction; they are the only ways ridges can meet:
 *               - a vertical ridge coming down onto a horizontal one ends
 *                 in its top row (0x2f1 0x2f0),
 *               - a vertical ridge going down from a horizontal one starts
 *                 in its ledge row (0x2e9 0x2e8),
 *               - a horizontal ridge coming from the west ends against a
 *                 vertical ridge that goes on (0x2f1 over 0x2e9 in the
 *                 vertical's west column),
 *               - the map's corners (0x2f0 and 0x2a5 at the south-west,
 *                 0x2ac and 0x31a at the south-east).
 *             There is NO junction for a horizontal ridge leaving a
 *             vertical one eastward (vanilla never draws one), so a
 *             horizontal ridge always has a free west end.
 *
 *   ROOM      A rectangle of floor walled on three sides by one-cell walls:
 *             0x290 on the west, 0x292 on the east, and a one-row south wall
 *             (0x298 0x299 ... 0x29a) with 2-wide STAIRS (0x284 0x284) cut
 *             into it. The floor along the walls inside is drawn with
 *             vanilla's edge tiles (0x2d8 west, 0x2da east, 0x2e1 south and
 *             their corners). Rooms differ in their north side, which is
 *             always a wall:
 *
 *               HANGING room: cut into the rock mass's bottom row. Its top
 *               corners are 0x29c / 0x29b with a 0x299 face cell beside
 *               them, or 0x2ed / 0x2ec right against a vertical ridge.
 *
 *                 RRRRRRRRRRRRR    R rock (0x2ce)
 *                 RS^:.....:^SR    S 0x299 face, ^ 0x29c / 0x29b corners
 *                   |:.....:|      | 0x290 / 0x292 walls, : 0x2d8 / 0x2da
 *                   |e_..__f|      e _ f 0x2e0 0x2e1 0x2e2, with plain
 *                   LSS==SSSJ      floor in front of the stairs (==, 0x284);
 *                                  L J 0x298 / 0x29a, the wall's ends
 *
 *               ALCOVE: the same room cut into the second cliff row of a
 *               horizontal ridge (top corners 0x2ed / 0x2ec), as vanilla's
 *               alcove at (27..32, 26..28).
 *
 *             Either way the stairs are the only way in. (Vanilla also has
 *             rooms open to the north, whose top row is the edge floor
 *             0x2d0 0x2d1 0x2d2. Those three tiles have the metatile
 *             behaviour MB_IMPASSABLE_NORTH: the player can't step off them
 *             northward or onto them from the north, a one-way edge that
 *             the core's grid, its reachability rules and the tests' path
 *             searches don't know about. This style doesn't draw them.)
 *
 *   BAR       A short free-standing horizontal ridge, both ends free.
 *
 *   CHAMBER   Vanilla's fossil chamber (see THE FOSSIL CHAMBER in the
 *             style header), drawn by the core. It is a room too: its north
 *             wall hangs from the rock mass, and vanilla only puts a
 *             vertical ridge east of the east end of its room's top row
 *             (0x28a, right below the chamber's top-right tile 0x29a), so
 *             this style always puts it in one of the top two rooms,
 *             against the ridge on that room's east side. (The chamber's
 *             own north edge is drawn with 0x2d0/0x2d1/0x2d2, see ROOM;
 *             nothing can reach it from the north anyway, it is wall.)
 *
 *   LADDERS   Both of B2F's ladders are up-ladders standing in open floor
 *             (MT_MOON_LADDER_UP_IN_FLOOR): the ladder 0x296 with its top,
 *             0x28e, a small solid rock on the floor right above it, as all
 *             four of vanilla B2F's ladders are. This style keeps the whole
 *             3 x 4 block around them plain floor.
 *
 * THE LAYOUT (ChooseLayout, DrawSkeleton, TryRooms)
 *
 * The map is framed by ridges (west, east and bottom) and the rock rows at
 * the top. Two horizontal ridges, H1 and H2, cut it into three TERRACES, and
 * each terrace may be split into a west and an east room by a vertical
 * ridge (the top terrace always is, by V0):
 *
 *     RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR   rock (and the
 *     ||  top-west room          ||   top-east room  ||   chamber up here)
 *     ||                         ||                  ||
 *     ||   d=====================++=============e    ||   H1 (4 rows)
 *     ||  middle terrace (maybe split)               ||
 *     ||     d=======e   ggg   d======================+   H2 (gaps: ggg)
 *     ||  bottom terrace (maybe split)               ||
 *     ++=============================================++   bottom ridge
 *
 * Every horizontal ridge has a free west end d (with the dithered column),
 * so there is a way past it along the west side. Its east end is free (e)
 * or joins the east border ridge. The rooms of the terraces must all
 * connect: ConnectTerraces() works out which rooms these ends connect and
 * cuts GAPS where more connections are needed.
 *
 * Then hanging rooms, alcoves and bars are tried at random places, each
 * kept only where it fits: every structure keeps a one-cell ring of floor
 * around its open sides (marked T_KEEP by KeepFloor, so nothing else is
 * built there), so walking around it is always possible and nothing it is
 * built in gets cut in two. With the terraces connected, every floor cell can be reached, and
 * the core's pocket filling never has anything to fill.
 *
 * Finally the ladders are placed (PlaceLadders), and the finished map is
 * checked: IsDrawable() (vanilla pairs only), the walkable share (test P6's
 * band, 36-56%) and HasRoomForContent() (the core can place B2F's content
 * without carving floor this style can't draw). A layout that fails any
 * check is thrown away and a new one is chosen; the last attempt uses a
 * fixed layout that passes every check (sFallbackLayout).
 *
 * WHY NOT CELLULAR AUTOMATA: the cellular-automata style the generator
 * started with (since removed) grew blobs with bumps, notches and 1-cell
 * strands that no vanilla tile arrangement draws, and B2F's look is built
 * from straight walls anyway.
 *
 * WHAT THE CORE DOES AFTERWARDS (see the style header): it stamps the fossil
 * chamber where this style put it, fills pockets (there are none), places the
 * content (it stamps the hidden items' boulders, 0x282, only where
 * canStampAt agrees: plain floor north, south, west and east, which vanilla
 * also has around its boulders) and would carve repair floor if some piece
 * found no spot, which HasRoomForContent rules out.
 *
 * Every random choice uses MtMoonGen_Random(), the core's seeded generator,
 * so the same seed always builds the same B2F.
 *
 * MEMORY: the tile map (1,920 bytes for 48 x 40) is the style's work area,
 * borrowed from the heap with the core's grid, so it costs no permanent RAM.
 * This module has no variables of its own (EWRAM is full, and new IWRAM
 * variables would move addresses the tests rely on; see the IWRAM note in
 * src/mt_moon_gen.c). The layout lives on the stack (22 bytes).
 * ============================================================================
 */

#include "global.h"
#include "mt_moon_gen_style.h"

/* ------------------------------------------------------------------------- */
/* Tiles                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * The tile indexes kept in the work area. The first three are not tiles:
 * T_NONE (0, so a zeroed work area is all T_NONE) is a cell nothing has
 * claimed yet, T_KEEP a cell that must stay floor (the ring around a
 * structure), and T_CHAMBER a cell of the fossil chamber, which the core
 * draws. Both T_NONE and T_KEEP become T_FLOOR once the layout is drawn.
 */
enum
{
    T_NONE,
    T_KEEP,
    T_CHAMBER,
    /* Walkable */
    T_FLOOR,        /* 0x281 plain cave floor */
    T_DITHER,       /* 0x2d9 dithered floor, beside a ridge's free west end */
    T_STAIRS,       /* 0x284 stairs in a room's south wall */
    T_EDGE_W,       /* 0x2d8 room floor along its west wall */
    T_EDGE_E,       /* 0x2da room floor along its east wall */
    T_EDGE_S,       /* 0x2e1 room floor along its south wall */
    T_EDGE_SW,      /* 0x2e0 */
    T_EDGE_SE,      /* 0x2e2 */
    T_LADDER,       /* 0x296 up-ladder */
    /* Solid */
    T_ROCK,         /* 0x2ce rock mass */
    T_RIDGE_W,      /* 0x2e6 vertical ridge, west column */
    T_RIDGE_E,      /* 0x2e7 vertical ridge, east column */
    T_RIDGE_TOP_W,  /* 0x2ea vertical ridge's top, in the rock */
    T_RIDGE_TOP_E,  /* 0x2eb */
    T_TOP,          /* 0x2df horizontal ridge: pink top row */
    T_LEDGE,        /* 0x2c2 horizontal ridge: ledge row */
    T_CLIFF,        /* 0x2e4 horizontal ridge: cliff rows */
    T_CROSS_W,      /* 0x2f1 junctions in a top row (see THE STRUCTURES) */
    T_CROSS_E,      /* 0x2f0 */
    T_DROP_W,       /* 0x2e9 junctions in a ledge row */
    T_DROP_E,       /* 0x2e8 */
    T_WEND_TOP,     /* 0x29d free west end: top, ledge, cliff */
    T_WEND_LEDGE,   /* 0x319 */
    T_WEND_CLIFF,   /* 0x2e3 */
    T_EEND_TOP,     /* 0x29e free east end: top, ledge, cliff */
    T_EEND_LEDGE,   /* 0x31a */
    T_EEND_CLIFF,   /* 0x2e5 */
    T_CORNER_SW,    /* 0x2a5 the map's south-west corner (ledge row) */
    T_CORNER_SE,    /* 0x2ac the map's south-east corner (top row) */
    T_WALL_W,       /* 0x290 room walls */
    T_WALL_E,       /* 0x292 */
    T_WALL_SW,      /* 0x298 a south wall's west end */
    T_WALL_S,       /* 0x299 one-row wall */
    T_WALL_SE,      /* 0x29a a south wall's east end */
    T_HANG_W,       /* 0x29c a hanging room's top corners */
    T_HANG_E,       /* 0x29b */
    T_CUT_W,        /* 0x2ed a room's top corners beside a ridge or cliff */
    T_CUT_E,        /* 0x2ec */
    T_LADDER_TOP,   /* 0x28e the ladder's top, standing on the floor */
    T_COUNT
};

/* The vanilla B2F map cell value of each tile: walkable tiles are collision
 * 0, elevation 3 (0x3xxx), solid ones collision 1, elevation 0 (0x06xx or,
 * for metatiles 0x300 and up, 0x07xx). */
static const u16 sTileValues[T_COUNT] =
{
    [T_NONE]        = 0x3281,
    [T_KEEP]        = 0x3281,
    [T_CHAMBER]     = 0x3281,  /* never drawn: the core draws the chamber */
    [T_FLOOR]       = 0x3281,
    [T_DITHER]      = 0x32d9,
    [T_STAIRS]      = 0x3284,
    [T_EDGE_W]      = 0x32d8,
    [T_EDGE_E]      = 0x32da,
    [T_EDGE_S]      = 0x32e1,
    [T_EDGE_SW]     = 0x32e0,
    [T_EDGE_SE]     = 0x32e2,
    [T_LADDER]      = 0x3296,
    [T_ROCK]        = 0x06ce,
    [T_RIDGE_W]     = 0x06e6,
    [T_RIDGE_E]     = 0x06e7,
    [T_RIDGE_TOP_W] = 0x06ea,
    [T_RIDGE_TOP_E] = 0x06eb,
    [T_TOP]         = 0x06df,
    [T_LEDGE]       = 0x06c2,
    [T_CLIFF]       = 0x06e4,
    [T_CROSS_W]     = 0x06f1,
    [T_CROSS_E]     = 0x06f0,
    [T_DROP_W]      = 0x06e9,
    [T_DROP_E]      = 0x06e8,
    [T_WEND_TOP]    = 0x069d,
    [T_WEND_LEDGE]  = 0x0719,
    [T_WEND_CLIFF]  = 0x06e3,
    [T_EEND_TOP]    = 0x069e,
    [T_EEND_LEDGE]  = 0x071a,
    [T_EEND_CLIFF]  = 0x06e5,
    [T_CORNER_SW]   = 0x06a5,
    [T_CORNER_SE]   = 0x06ac,
    [T_WALL_W]      = 0x0690,
    [T_WALL_E]      = 0x0692,
    [T_WALL_SW]     = 0x0698,
    [T_WALL_S]      = 0x0699,
    [T_WALL_SE]     = 0x069a,
    [T_HANG_W]      = 0x069c,
    [T_HANG_E]      = 0x069b,
    [T_CUT_W]       = 0x06ed,
    [T_CUT_E]       = 0x06ec,
    [T_LADDER_TOP]  = 0x068e,
};

/* TRUE if a map cell value can be walked on (its collision bits are 0). */
#define IS_WALKABLE_VALUE(value) (((value) & 0x0C00) == 0)

/*
 * The tile pairs vanilla B2F's map.bin has, among this style's tiles: bit t
 * of sEastOk[a] is set if tile t may stand directly EAST of tile a, and
 * sSouthOk[a] likewise for directly SOUTH. A tile index is under 64, so each
 * set is two 32-bit words: word t / 32, bit t % 32 (HAS_TILE). The comment
 * above each entry lists the metatiles of the set. Entries for T_NONE,
 * T_KEEP and T_CHAMBER are empty: IsDrawable skips the chamber's cells, and
 * the other two never remain in a finished map.
 */
#define HAS_TILE(set, t) (((set)[(t) >> 5] >> ((t) & 31)) & 1)

static const u32 sEastOk[T_COUNT][2] =
{
    /* 281: 281 2d9 2da 2e1 2e2 296 2e6 290 298 28e */
    [T_FLOOR] = { 0x00002d98, 0x00000205 },
    /* 2d9: 281 2d9 2da 2e1 2e6 29d 319 2e3 */
    [T_DITHER] = { 0x07002198, 0x00000000 },
    /* 284: 284 299 */
    [T_STAIRS] = { 0x00000020, 0x00000008 },
    /* 2d8: 281 2d9 2da */
    [T_EDGE_W] = { 0x00000098, 0x00000000 },
    /* 2da: 292 29b 2ec */
    [T_EDGE_E] = { 0x00000000, 0x00000142 },
    /* 2e1: 281 2d9 2e1 2e2 */
    [T_EDGE_S] = { 0x00000518, 0x00000000 },
    /* 2e0: 281 2e1 2e2 */
    [T_EDGE_SW] = { 0x00000508, 0x00000000 },
    /* 2e2: 292 */
    [T_EDGE_SE] = { 0x00000000, 0x00000002 },
    /* 296: 281 */
    [T_LADDER] = { 0x00000008, 0x00000000 },
    /* 2ce: 2ce 2e6 2ea 299 */
    [T_ROCK] = { 0x0000b000, 0x00000008 },
    /* 2e6: 2e7 2f0 2e8 */
    [T_RIDGE_W] = { 0x00a04000, 0x00000000 },
    /* 2e7: 281 2ce 2e4 290 298 2ed */
    [T_RIDGE_E] = { 0x00081008, 0x00000085 },
    /* 2ea: 2eb */
    [T_RIDGE_TOP_W] = { 0x00010000, 0x00000000 },
    /* 2eb: 2ce */
    [T_RIDGE_TOP_E] = { 0x00001000, 0x00000000 },
    /* 2df: 2df 2f1 29e 2ac */
    [T_TOP] = { 0x88120000, 0x00000000 },
    /* 2c2: 2c2 2e9 31a */
    [T_LEDGE] = { 0x10440000, 0x00000000 },
    /* 2e4: 2e6 2e4 2e5 2ed */
    [T_CLIFF] = { 0x20082000, 0x00000080 },
    /* 2f1: 2e7 2f0 */
    [T_CROSS_W] = { 0x00204000, 0x00000000 },
    /* 2f0: 2df */
    [T_CROSS_E] = { 0x00020000, 0x00000000 },
    /* 2e9: 2e7 2e8 */
    [T_DROP_W] = { 0x00804000, 0x00000000 },
    /* 2e8: 2c2 */
    [T_DROP_E] = { 0x00040000, 0x00000000 },
    /* 29d: 2df */
    [T_WEND_TOP] = { 0x00020000, 0x00000000 },
    /* 319: 2c2 */
    [T_WEND_LEDGE] = { 0x00040000, 0x00000000 },
    /* 2e3: 2e4 */
    [T_WEND_CLIFF] = { 0x00080000, 0x00000000 },
    /* 29e: 281 2d9 */
    [T_EEND_TOP] = { 0x00000018, 0x00000000 },
    /* 31a: 281 2d9 */
    [T_EEND_LEDGE] = { 0x00000018, 0x00000000 },
    /* 2e5: 281 2d9 2e6 */
    [T_EEND_CLIFF] = { 0x00002018, 0x00000000 },
    /* 2a5: 2c2 */
    [T_CORNER_SW] = { 0x00040000, 0x00000000 },
    /* 2ac: 2e7 */
    [T_CORNER_SE] = { 0x00004000, 0x00000000 },
    /* 290: 2d8 2e0 */
    [T_WALL_W] = { 0x00000240, 0x00000000 },
    /* 292: 281 2e6 290 */
    [T_WALL_E] = { 0x00002008, 0x00000001 },
    /* 298: 284 299 */
    [T_WALL_SW] = { 0x00000020, 0x00000008 },
    /* 299: 284 2ce 299 29a 29c */
    [T_WALL_S] = { 0x00001020, 0x00000038 },
    /* 29a: 281 2e6 298 */
    [T_WALL_SE] = { 0x00002008, 0x00000004 },
    /* 29c: 2d8 */
    [T_HANG_W] = { 0x00000040, 0x00000000 },
    /* 29b: 299 */
    [T_HANG_E] = { 0x00000000, 0x00000008 },
    /* 2ed: 2d8 */
    [T_CUT_W] = { 0x00000040, 0x00000000 },
    /* 2ec: 2e6 2e4 */
    [T_CUT_E] = { 0x00082000, 0x00000000 },
    /* 28e: 281 */
    [T_LADDER_TOP] = { 0x00000008, 0x00000000 },
};

static const u32 sSouthOk[T_COUNT][2] =
{
    /* 281: 281 2d9 284 2e1 2df 29d 29e 28e */
    [T_FLOOR] = { 0x09020138, 0x00000200 },
    /* 2d9: 281 2d9 284 2d8 2e1 2df 29d */
    [T_DITHER] = { 0x01020178, 0x00000000 },
    /* 284: 281 */
    [T_STAIRS] = { 0x00000008, 0x00000000 },
    /* 2d8: 284 2d8 2e0 29e */
    [T_EDGE_W] = { 0x08000260, 0x00000000 },
    /* 2da: 2da 2e2 2df */
    [T_EDGE_E] = { 0x00020480, 0x00000000 },
    /* 2e1: 299 29c 29b */
    [T_EDGE_S] = { 0x00000000, 0x00000068 },
    /* 2e0: 299 */
    [T_EDGE_SW] = { 0x00000000, 0x00000008 },
    /* 2e2: 299 */
    [T_EDGE_SE] = { 0x00000000, 0x00000008 },
    /* 296: 281 */
    [T_LADDER] = { 0x00000008, 0x00000000 },
    /* 2ce: 281 2d8 2da 2ce 2ea 2eb 299 29c 29b 2ed 2ec */
    [T_ROCK] = { 0x000190c8, 0x000001e8 },
    /* 2e6: 2e6 2f1 319 2a5 2ac */
    [T_RIDGE_W] = { 0xc2102000, 0x00000000 },
    /* 2e7: 2e7 2f0 31a */
    [T_RIDGE_E] = { 0x10204000, 0x00000000 },
    /* 2ea: 2e6 */
    [T_RIDGE_TOP_W] = { 0x00002000, 0x00000000 },
    /* 2eb: 2e7 */
    [T_RIDGE_TOP_E] = { 0x00004000, 0x00000000 },
    /* 2df: 2c2 2e9 2e8 */
    [T_TOP] = { 0x00c40000, 0x00000000 },
    /* 2c2: 2e4 2e5 */
    [T_LEDGE] = { 0x20080000, 0x00000000 },
    /* 2e4: 281 2d9 2d8 2da 2e4 29c 2ed 2ec */
    [T_CLIFF] = { 0x000800d8, 0x000001a0 },
    /* 2f1: 2c2 2e9 */
    [T_CROSS_W] = { 0x00440000, 0x00000000 },
    /* 2f0: 2c2 */
    [T_CROSS_E] = { 0x00040000, 0x00000000 },
    /* 2e9: 2e6 */
    [T_DROP_W] = { 0x00002000, 0x00000000 },
    /* 2e8: 2e7 */
    [T_DROP_E] = { 0x00004000, 0x00000000 },
    /* 29d: 2e6 319 */
    [T_WEND_TOP] = { 0x02002000, 0x00000000 },
    /* 319: 2e3 */
    [T_WEND_LEDGE] = { 0x04000000, 0x00000000 },
    /* 2e3: 281 2d9 2e3 299 */
    [T_WEND_CLIFF] = { 0x04000018, 0x00000008 },
    /* 29e: 2e7 31a */
    [T_EEND_TOP] = { 0x10004000, 0x00000000 },
    /* 31a: 2e5 */
    [T_EEND_LEDGE] = { 0x20000000, 0x00000000 },
    /* 2e5: 281 2d9 2e5 */
    [T_EEND_CLIFF] = { 0x20000018, 0x00000000 },
    /* 2a5: 2e3 */
    [T_CORNER_SW] = { 0x04000000, 0x00000000 },
    /* 2ac: 2c2 */
    [T_CORNER_SE] = { 0x00040000, 0x00000000 },
    /* 290: 2df 290 298 */
    [T_WALL_W] = { 0x00020000, 0x00000005 },
    /* 292: 2df 292 29a */
    [T_WALL_E] = { 0x00020000, 0x00000012 },
    /* 298: 281 */
    [T_WALL_SW] = { 0x00000008, 0x00000000 },
    /* 299: 281 2ce 299 29c */
    [T_WALL_S] = { 0x00001008, 0x00000028 },
    /* 29a: 281 */
    [T_WALL_SE] = { 0x00000008, 0x00000000 },
    /* 29c: 290 */
    [T_HANG_W] = { 0x00000000, 0x00000001 },
    /* 29b: 292 */
    [T_HANG_E] = { 0x00000000, 0x00000002 },
    /* 2ed: 290 */
    [T_CUT_W] = { 0x00000000, 0x00000001 },
    /* 2ec: 292 */
    [T_CUT_E] = { 0x00000000, 0x00000002 },
    /* 28e: 296 */
    [T_LADDER_TOP] = { 0x00000800, 0x00000000 },
};

/* ------------------------------------------------------------------------- */
/* The tile map                                                              */
/* ------------------------------------------------------------------------- */

/* How many map cells the work area holds: B2F's layout is 48 x 40. */
#define WORK_CELLS (48 * 40)

/* The tile map being built: the work area, and the map's size. */
struct TileMap
{
    u8 *tiles;   /* tiles[y * width + x] is the tile index of map tile (x, y) */
    s32 width;
    s32 height;
};

#define TILE_AT(map, x, y) ((map)->tiles[(y) * (map)->width + (x)])

/*
 * Floor stays inside a border of 2 cells on the west, east and top sides,
 * the border the core and the other styles keep too. At the bottom the
 * border is the 4-row bottom ridge (rows height - 4 to height - 1), so floor
 * ends at row height - 5.
 */
#define MIN_X 2
#define MIN_Y 2
#define MAX_X(map) ((map)->width - 3)
#define MAX_Y(map) ((map)->height - 5)

/* A random number in [lo, hi] (hi >= lo). */
static s32 RandomBetween(s32 lo, s32 hi)
{
    return lo + MtMoonGen_RandomBelow(hi - lo + 1);
}

static void SetTile(struct TileMap *map, s32 x, s32 y, u8 tile)
{
    TILE_AT(map, x, y) = tile;
}

/* TRUE if every cell of the rectangle is inside the floor's bounds and still
 * unclaimed (T_NONE), so a structure may be built there. */
static bool8 IsFree(const struct TileMap *map, s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 x, y;

    if (x1 < MIN_X || y1 < MIN_Y || x2 > MAX_X(map) || y2 > MAX_Y(map))
        return FALSE;
    for (y = y1; y <= y2; y++)
    {
        for (x = x1; x <= x2; x++)
        {
            if (TILE_AT(map, x, y) != T_NONE)
                return FALSE;
        }
    }
    return TRUE;
}

/* Mark the still unclaimed cells of the rectangle T_KEEP: floor that no
 * later structure may build on (the ring of floor around a structure). */
static void KeepFloor(struct TileMap *map, s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 x, y;

    for (y = y1; y <= y2; y++)
    {
        for (x = x1; x <= x2; x++)
        {
            if (TILE_AT(map, x, y) == T_NONE)
                TILE_AT(map, x, y) = T_KEEP;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* The layout                                                                */
/* ------------------------------------------------------------------------- */

/*
 * The terraces (see THE LAYOUT): 0 at the top, 1 in the middle, 2 at the
 * bottom. Horizontal ridge k (H1 = 0, H2 = 1) lies between terrace k and
 * terrace k + 1.
 */
#define TERRACES 3
#define RIDGES   (TERRACES - 1)

/* The most gaps one horizontal ridge gets. */
#define MAX_GAPS 2

/* Gaps: 3 columns of dithered floor between a free east end and a free west
 * end (see THE STRUCTURES). */
#define GAP_WIDTH 3

struct Layout
{
    /* The west column of the vertical ridge that splits terrace k into a
     * west and an east room, or 0 if the terrace isn't split. split[0], V0,
     * is never 0. */
    u8 split[TERRACES];
    /* Horizontal ridge k: its top row (rows row..row+3), its west end
     * column (always free), its east end column, and whether the east end
     * is free (FALSE: it runs on to the east border ridge, east = MAX_X). */
    u8 row[RIDGES];
    u8 west[RIDGES];
    u8 east[RIDGES];
    bool8 openEast[RIDGES];
    /* Gaps in ridge k: the first dithered column of each. */
    u8 gaps[RIDGES][MAX_GAPS];
    u8 gapCount[RIDGES];
    /* The last rock row of the top terrace's west room ([0]) and east room
     * ([1]). In the chamber's room it is the chamber's top row: the rock is
     * one row deeper beside the chamber than above it. */
    u8 rockBottom[2];
    /* The fossil chamber's top-left tile, and whether it is in the top-west
     * room (against V0) or the top-east room (against the east border). */
    u8 chamberX;
    u8 chamberY;
    bool8 chamberWest;
};

/* The first and last floor row of terrace k's room `side` (0 west, 1 east;
 * only the top terrace's two rooms differ). */
static s32 TerraceTop(const struct Layout *layout, s32 k, s32 side)
{
    if (k == 0)
        return layout->rockBottom[side] + 1;
    return layout->row[k - 1] + 4;
}

static s32 TerraceBottom(const struct TileMap *map, const struct Layout *layout, s32 k)
{
    if (k == TERRACES - 1)
        return MAX_Y(map);
    return layout->row[k] - 1;
}

/* The first and last column of terrace k's room `side`. */
static s32 RoomWest(const struct Layout *layout, s32 k, s32 side)
{
    if (side == 0)
        return MIN_X;
    return layout->split[k] + 2;
}

static s32 RoomEast(const struct TileMap *map, const struct Layout *layout, s32 k, s32 side)
{
    if (side == 0 && layout->split[k] != 0)
        return layout->split[k] - 1;
    return MAX_X(map);
}

/*
 * CONNECTING THE TERRACES
 *
 * The rooms (up to 2 per terrace) are numbered 2k + side. Which of them are
 * connected is tracked with a "union-find": parent[i] points at another room
 * of the same group, and following parent links from any room ends at the
 * group's ROOT, the one room pointing at itself. Two rooms are connected
 * when they have the same root; joining two groups points one root at the
 * other.
 */
#define ROOM_OF(layout, k, x) (2 * (k) + ((layout)->split[k] != 0 && (x) > (layout)->split[k]))

static u8 FindRoot(u8 *parent, u8 room)
{
    while (parent[room] != room)
        room = parent[room];
    return room;
}

static void Join(u8 *parent, u8 a, u8 b)
{
    parent[FindRoot(parent, a)] = FindRoot(parent, b);
}

/* TRUE if every room of every terrace is in one group. */
static bool8 AllConnected(const struct Layout *layout, u8 *parent)
{
    u8 root = FindRoot(parent, 0);
    s32 k;

    for (k = 0; k < TERRACES; k++)
    {
        if (FindRoot(parent, 2 * k) != root)
            return FALSE;
        if (layout->split[k] != 0 && FindRoot(parent, 2 * k + 1) != root)
            return FALSE;
    }
    return TRUE;
}

/* TRUE if a gap starting at column g keeps its ends clear of the junction at
 * columns x, x + 1: the gap's east end (g - 1) and west end (g + GAP_WIDTH)
 * each need a plain ridge column between them and the junction, since
 * vanilla has no tile pair joining an end to a junction directly. */
#define GAP_CLEAR_OF(g, x) ((x) == 0 || (g) >= (x) + 4 || (g) + GAP_WIDTH <= (x) - 2)

/*
 * A random place for a new gap in ridge k, or 0 if there is none: inside the
 * ridge with a plain column between each end and the ridge's own ends, clear
 * of the junctions of the vertical ridges above (split[k]) and below
 * (split[k + 1]) and 8+ columns from the ridge's other gaps. With `mustJoin`,
 * only places that connect two rooms not connected yet. Every place gets an
 * equal chance ("reservoir sampling": the n-th place found replaces the
 * choice so far with probability 1/n).
 */
static s32 RandomGapPlace(const struct Layout *layout, s32 k, u8 *parent, bool8 mustJoin)
{
    s32 g, i, found = 0, choice = 0;

    for (g = layout->west[k] + 3; g + GAP_WIDTH + 2 <= layout->east[k] - 1; g++)
    {
        if (!GAP_CLEAR_OF(g, layout->split[k]) || !GAP_CLEAR_OF(g, layout->split[k + 1]))
            continue;
        for (i = 0; i < layout->gapCount[k]; i++)
        {
            if (abs(g - layout->gaps[k][i]) < 8)
                break;
        }
        if (i < layout->gapCount[k])
            continue;
        if (mustJoin && FindRoot(parent, ROOM_OF(layout, k, g)) == FindRoot(parent, ROOM_OF(layout, k + 1, g)))
            continue;
        found++;
        if (MtMoonGen_RandomBelow(found) == 0)
            choice = g;
    }
    return choice;
}

static void AddGap(struct Layout *layout, s32 k, s32 g, u8 *parent)
{
    layout->gaps[k][layout->gapCount[k]++] = g;
    Join(parent, ROOM_OF(layout, k, g), ROOM_OF(layout, k + 1, g));
}

/* How often a ridge gets a gap that isn't needed, for variety (percent). */
#define EXTRA_GAP_PERCENT 50

/*
 * Make every room of the terraces reachable (see CONNECTING THE TERRACES):
 * each ridge's free west end joins the west rooms above and below it, a free
 * east end the east rooms, and gaps whatever is still apart. Returns FALSE
 * if no choice of gaps connects them (the caller picks a new layout).
 */
static bool8 ConnectTerraces(const struct TileMap *map, struct Layout *layout)
{
    u8 parent[2 * TERRACES];
    s32 i, k, g, round;

    for (i = 0; i < 2 * TERRACES; i++)
        parent[i] = i;
    for (k = 0; k < RIDGES; k++)
    {
        layout->gapCount[k] = 0;
        Join(parent, ROOM_OF(layout, k, MIN_X), ROOM_OF(layout, k + 1, MIN_X));
        if (layout->openEast[k])
            Join(parent, ROOM_OF(layout, k, MAX_X(map)), ROOM_OF(layout, k + 1, MAX_X(map)));
    }
    if (MtMoonGen_RandomBelow(100) < EXTRA_GAP_PERCENT)
    {
        k = MtMoonGen_RandomBelow(RIDGES);
        g = RandomGapPlace(layout, k, parent, FALSE);
        if (g != 0)
            AddGap(layout, k, g, parent);
    }
    /* Each round adds at most one gap that joins two groups; 6 rooms need at
     * most 5 joins, and the ends always give 2. */
    for (round = 0; round < 4 && !AllConnected(layout, parent); round++)
    {
        k = MtMoonGen_RandomBelow(RIDGES);
        for (i = 0; i < RIDGES; i++, k = 1 - k)
        {
            if (layout->gapCount[k] == MAX_GAPS)
                continue;
            g = RandomGapPlace(layout, k, parent, TRUE);
            if (g != 0)
            {
                AddGap(layout, k, g, parent);
                break;
            }
        }
    }
    return AllConnected(layout, parent);
}

/*
 * A random column for a vertical ridge in [lo, hi] at least 4 columns from
 * the ridge at `avoid` (0: none), or 0 if there is none. (A vertical ridge's
 * junctions take 2 columns, and 2 more between two of them keep them apart.)
 */
static s32 RandomSplit(s32 lo, s32 hi, s32 avoid)
{
    s32 x, found = 0, choice = 0;

    for (x = lo; x <= hi; x++)
    {
        if (avoid != 0 && abs(x - avoid) < 4)
            continue;
        found++;
        if (MtMoonGen_RandomBelow(found) == 0)
            choice = x;
    }
    return choice;
}

/*
 * The ranges below are tuned for B2F's 48 x 40 map. In a Python prototype
 * of this style they gave layouts whose walkable share is 42-59% (mean
 * about 51%; vanilla B2F: 46%) before the band check; the chamber's room
 * keeps a floor row below the chamber, and every terrace keeps at least 5
 * floor rows.
 */
#define V0_MIN            17   /* V0's west column */
#define V0_MAX            29
#define CHAMBER_Y_MIN      2   /* the chamber's top row */
#define CHAMBER_Y_MAX      5
#define ROCK_MIN           3   /* the other top room's last rock row */
#define ROCK_MAX           9
#define H1_ROW_MAX        19   /* H1's top row */
#define H2_ROW_MAX        27   /* H2's top row */
#define TERRACE_MIN_ROWS   5
#define H1_WEST_MIN        6   /* H1's free west end; the way past it is the */
#define H1_WEST_MAX        9   /* columns west of it, so at least 4 wide */
#define H2_WEST_MIN        6
#define H2_WEST_MAX       12
#define H1_EAST_GAP_MIN    4   /* columns left free east of a free east end */
#define H1_EAST_GAP_MAX    8
#define H2_EAST_GAP_MIN    4
#define H2_EAST_GAP_MAX   10
#define SPLIT_PERCENT     70   /* how often the lower terraces are split */
#define OPEN_EAST_PERCENT 60   /* how often a ridge's east end is free */

/*
 * Choose a random layout (see THE LAYOUT). Returns FALSE if the draws don't
 * fit together (an empty range, or rooms no gaps can connect); the caller
 * then chooses again, which costs nothing but a few random numbers.
 */
static bool8 ChooseLayout(const struct TileMap *map, struct Layout *layout)
{
    s32 cy, otherRock, lo, hi;
    s32 maxX = MAX_X(map);

    layout->split[0] = RandomBetween(V0_MIN, V0_MAX);
    layout->chamberWest = MtMoonGen_RandomBelow(2);
    cy = RandomBetween(CHAMBER_Y_MIN, CHAMBER_Y_MAX);
    otherRock = RandomBetween(ROCK_MIN, ROCK_MAX);
    layout->chamberY = cy;
    if (layout->chamberWest)
    {
        /* Against V0, in the top-west room. */
        layout->chamberX = layout->split[0] - MT_MOON_CHAMBER_WIDTH;
        layout->rockBottom[0] = cy;
        layout->rockBottom[1] = otherRock;
    }
    else
    {
        /* Against the east border ridge, in the top-east room. */
        layout->chamberX = maxX + 1 - MT_MOON_CHAMBER_WIDTH;
        layout->rockBottom[0] = otherRock;
        layout->rockBottom[1] = cy;
    }

    /* H1 below the chamber (and the floor row under it) and below the
     * other room's rock, leaving that room at least 7 floor rows; H2 with
     * at least TERRACE_MIN_ROWS floor rows on either side. */
    lo = max(cy + MT_MOON_CHAMBER_HEIGHT + 1, otherRock + 8);
    if (lo > H1_ROW_MAX)
        return FALSE;
    layout->row[0] = RandomBetween(lo, H1_ROW_MAX);
    lo = layout->row[0] + 4 + TERRACE_MIN_ROWS;
    if (lo > H2_ROW_MAX)
        return FALSE;
    layout->row[1] = RandomBetween(lo, H2_ROW_MAX);

    layout->west[0] = RandomBetween(H1_WEST_MIN, H1_WEST_MAX);
    layout->openEast[0] = MtMoonGen_RandomBelow(100) < OPEN_EAST_PERCENT;
    layout->east[0] = layout->openEast[0] ? maxX - RandomBetween(H1_EAST_GAP_MIN, H1_EAST_GAP_MAX) : maxX;
    layout->west[1] = RandomBetween(H2_WEST_MIN, H2_WEST_MAX);
    layout->openEast[1] = MtMoonGen_RandomBelow(100) < OPEN_EAST_PERCENT;
    layout->east[1] = layout->openEast[1] ? maxX - RandomBetween(H2_EAST_GAP_MIN, H2_EAST_GAP_MAX) : maxX;

    /* A vertical ridge under a horizontal one starts in its ledge row, so
     * it must stand where both ridges it touches are plain: 7+ columns from
     * their west ends and 8+ from their east ends. */
    layout->split[1] = 0;
    layout->split[2] = 0;
    if (MtMoonGen_RandomBelow(100) < SPLIT_PERCENT)
    {
        lo = max(layout->west[0], layout->west[1]) + 7;
        hi = min(layout->east[0], layout->east[1]) - 8;
        layout->split[1] = RandomSplit(lo, hi, layout->split[0]);
    }
    if (MtMoonGen_RandomBelow(100) < SPLIT_PERCENT)
        layout->split[2] = RandomSplit(layout->west[1] + 7, layout->east[1] - 8, layout->split[1]);

    return ConnectTerraces(map, layout);
}

/*
 * The last attempt's layout, fixed: the chamber against V0 at (11,3), V0 at
 * column 24, H1 on rows 14-17 (columns 7-40, both ends free), H2 on rows
 * 24-27 (columns 9-38, both ends free), no lower splits and no gaps, plus
 * the rooms in sFallbackRooms. A Python prototype of this style checked it:
 * every pair is a vanilla pair, 46% is walkable, and for every possible
 * choice of the two ladders at least 69 content spots remain (37 are
 * needed); a test build that always used it passed every B2F property check
 * on 30 seeds. It only exists so that B2F is always generated: in 500
 * generations of the game, 469 kept their first random layout, 30 needed a
 * second, 1 a third, and none needed this one.
 */
static const struct Layout sFallbackLayout =
{
    .split = { 24, 0, 0 },
    .row = { 14, 24 },
    .west = { 7, 9 },
    .east = { 40, 38 },
    .openEast = { TRUE, TRUE },
    .gapCount = { 0, 0 },
    .rockBottom = { 3, 8 },
    .chamberX = 11,
    .chamberY = 3,
    .chamberWest = TRUE,
};

/* ------------------------------------------------------------------------- */
/* Drawing the structures                                                    */
/* ------------------------------------------------------------------------- */

/* The two columns of a vertical ridge, rows y1..y2. */
static void DrawRidgeColumns(struct TileMap *map, s32 x, s32 y1, s32 y2)
{
    s32 y;

    for (y = y1; y <= y2; y++)
    {
        SetTile(map, x, y, T_RIDGE_W);
        SetTile(map, x + 1, y, T_RIDGE_E);
    }
}

/* A vertical ridge starting in the top rock: its top (0x2ea 0x2eb) in row
 * 1, the rock's lowest row of the top band, then columns down to row y2. */
static void DrawRidgeFromTop(struct TileMap *map, s32 x, s32 y2)
{
    SetTile(map, x, 1, T_RIDGE_TOP_W);
    SetTile(map, x + 1, 1, T_RIDGE_TOP_E);
    DrawRidgeColumns(map, x, 2, y2);
}

/* One column of a horizontal ridge's 4 rows (row..row+3): the pink top,
 * the ledge and the 2-row cliff face. */
static void DrawRidgeSlice(struct TileMap *map, s32 x, s32 row)
{
    SetTile(map, x, row, T_TOP);
    SetTile(map, x, row + 1, T_LEDGE);
    SetTile(map, x, row + 2, T_CLIFF);
    SetTile(map, x, row + 3, T_CLIFF);
}

/* The 4 rows of a horizontal ridge's free west end at column x, with the
 * dithered floor vanilla puts beside it in column x - 1. */
static void DrawWestEnd(struct TileMap *map, s32 x, s32 row)
{
    s32 y;

    SetTile(map, x, row, T_WEND_TOP);
    SetTile(map, x, row + 1, T_WEND_LEDGE);
    SetTile(map, x, row + 2, T_WEND_CLIFF);
    SetTile(map, x, row + 3, T_WEND_CLIFF);
    for (y = row; y < row + 4; y++)
        SetTile(map, x - 1, y, T_DITHER);
}

/* The 4 rows of a horizontal ridge's free east end at column x. */
static void DrawEastEnd(struct TileMap *map, s32 x, s32 row)
{
    SetTile(map, x, row, T_EEND_TOP);
    SetTile(map, x, row + 1, T_EEND_LEDGE);
    SetTile(map, x, row + 2, T_EEND_CLIFF);
    SetTile(map, x, row + 3, T_EEND_CLIFF);
}

/* A horizontal ridge on rows row..row+3 from column west to east, both ends
 * free (a gap or a bar is built from these too). */
static void DrawRidgeRows(struct TileMap *map, s32 west, s32 east, s32 row)
{
    s32 x;

    for (x = west; x <= east; x++)
        DrawRidgeSlice(map, x, row);
    DrawWestEnd(map, west, row);
    DrawEastEnd(map, east, row);
}

/* Horizontal ridge k of the layout, with its junctions and gaps. */
static void DrawRidge(struct TileMap *map, const struct Layout *layout, s32 k)
{
    s32 row = layout->row[k];
    s32 above = layout->split[k];      /* vertical ridge ending on it */
    s32 below = layout->split[k + 1];  /* vertical ridge leaving it downward */
    s32 i, x, y;

    DrawRidgeRows(map, layout->west[k], layout->east[k], row);
    if (!layout->openEast[k])
    {
        /* It runs into the east border ridge, which goes on below: the
         * junction sits in the border's west column. */
        x = MAX_X(map);
        DrawRidgeSlice(map, x, row);
        SetTile(map, x + 1, row, T_CROSS_W);
        SetTile(map, x + 1, row + 1, T_DROP_W);
    }
    if (above != 0)
    {
        SetTile(map, above, row, T_CROSS_W);
        SetTile(map, above + 1, row, T_CROSS_E);
    }
    if (below != 0)
    {
        SetTile(map, below, row + 1, T_DROP_W);
        SetTile(map, below + 1, row + 1, T_DROP_E);
        DrawRidgeColumns(map, below, row + 2, row + 3);
    }
    for (i = 0; i < layout->gapCount[k]; i++)
    {
        s32 g = layout->gaps[k][i];

        DrawEastEnd(map, g - 1, row);
        DrawWestEnd(map, g + GAP_WIDTH, row);
        for (y = row; y < row + 4; y++)
        {
            for (x = g; x < g + GAP_WIDTH; x++)
                SetTile(map, x, y, T_DITHER);
        }
    }
}

/* Rock (0x2ce) on rows 2..lastRow, columns x1..x2. */
static void DrawRock(struct TileMap *map, s32 x1, s32 x2, s32 lastRow)
{
    s32 x, y;

    for (y = 2; y <= lastRow; y++)
    {
        for (x = x1; x <= x2; x++)
            SetTile(map, x, y, T_ROCK);
    }
}

/*
 * The frame: rock on rows 0 and 1, a vertical ridge down each side, and a
 * horizontal ridge along the bottom (the map's last 4 rows), with vanilla's
 * corner tiles where the side ridges end on it (as at vanilla's (0,13)-
 * (1,16) and (35,33)-(36,36)).
 */
static void DrawFrame(struct TileMap *map)
{
    s32 w = map->width;
    s32 bottom = map->height - 4;   /* the bottom ridge's top row */
    s32 x;

    for (x = 0; x < w; x++)
    {
        SetTile(map, x, 0, T_ROCK);
        SetTile(map, x, 1, T_ROCK);
    }
    DrawRidgeFromTop(map, 0, bottom - 1);
    DrawRidgeFromTop(map, w - 2, bottom - 1);
    for (x = 2; x < w - 2; x++)
        DrawRidgeSlice(map, x, bottom);
    /* South-west: the west ridge ends on the bottom ridge going east. */
    SetTile(map, 0, bottom, T_RIDGE_W);
    SetTile(map, 1, bottom, T_CROSS_E);
    SetTile(map, 0, bottom + 1, T_CORNER_SW);
    SetTile(map, 1, bottom + 1, T_LEDGE);
    SetTile(map, 0, bottom + 2, T_WEND_CLIFF);
    SetTile(map, 0, bottom + 3, T_WEND_CLIFF);
    SetTile(map, 1, bottom + 2, T_CLIFF);
    SetTile(map, 1, bottom + 3, T_CLIFF);
    /* South-east: the east ridge ends on the bottom ridge from the west. */
    SetTile(map, w - 2, bottom, T_CORNER_SE);
    SetTile(map, w - 1, bottom, T_RIDGE_E);
    SetTile(map, w - 2, bottom + 1, T_LEDGE);
    SetTile(map, w - 1, bottom + 1, T_EEND_LEDGE);
    SetTile(map, w - 2, bottom + 2, T_CLIFF);
    SetTile(map, w - 2, bottom + 3, T_CLIFF);
    SetTile(map, w - 1, bottom + 2, T_EEND_CLIFF);
    SetTile(map, w - 1, bottom + 3, T_EEND_CLIFF);
}

/*
 * Everything of the layout except the hanging rooms, alcoves and bars:
 * the frame, the rock, the ridges and the fossil chamber.
 */
static void DrawSkeleton(struct TileMap *map, const struct Layout *layout)
{
    s32 v0 = layout->split[0];
    s32 cx = layout->chamberX;
    s32 cy = layout->chamberY;
    s32 x, y;

    DrawFrame(map);
    DrawRock(map, MIN_X, v0 - 1, layout->rockBottom[0]);
    DrawRock(map, v0 + 2, MAX_X(map), layout->rockBottom[1]);
    DrawRidgeFromTop(map, v0, layout->row[0] - 1);
    DrawRidge(map, layout, 0);
    DrawRidge(map, layout, 1);
    if (layout->split[1] != 0)
        DrawRidgeColumns(map, layout->split[1], layout->row[0] + 4, layout->row[1] - 1);
    if (layout->split[2] != 0)
    {
        x = layout->split[2];
        DrawRidgeColumns(map, x, layout->row[1] + 4, MAX_Y(map));
        SetTile(map, x, MAX_Y(map) + 1, T_CROSS_W);
        SetTile(map, x + 1, MAX_Y(map) + 1, T_CROSS_E);
    }

    /* The chamber, drawn by the core; its room's rock is one row deeper
     * than above it, ending at the chamber's top row. Its landing opens
     * west and south: keep a column of floor west of it and a row below. */
    for (y = cy; y < cy + MT_MOON_CHAMBER_HEIGHT; y++)
    {
        for (x = cx; x < cx + MT_MOON_CHAMBER_WIDTH; x++)
            SetTile(map, x, y, T_CHAMBER);
    }
    KeepFloor(map, cx - 1, cy + 1, cx - 1, cy + MT_MOON_CHAMBER_HEIGHT);
    KeepFloor(map, cx, cy + MT_MOON_CHAMBER_HEIGHT, cx + MT_MOON_CHAMBER_WIDTH - 1, cy + MT_MOON_CHAMBER_HEIGHT);
}

/* ------------------------------------------------------------------------- */
/* Rooms                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * A room's walls stand on columns x1 and x2 and its floor fills the columns
 * between; its floor rows are top..top+h-1 and its south wall is row top+h.
 * The top row is drawn by the room's kind; RoomBody draws the rest (the
 * side walls, the edge tiles, the last floor row and the south wall with its
 * stairs at a random place).
 */
static void RoomBody(struct TileMap *map, s32 x1, s32 x2, s32 top, s32 h)
{
    s32 last = top + h - 1;     /* the last floor row */
    s32 stairs = RandomBetween(x1 + 1, x2 - 3);
    s32 x, y;

    for (y = top + 1; y <= last; y++)
    {
        SetTile(map, x1, y, T_WALL_W);
        SetTile(map, x2, y, T_WALL_E);
        if (y == last)
            break;
        SetTile(map, x1 + 1, y, T_EDGE_W);
        for (x = x1 + 2; x < x2 - 1; x++)
            SetTile(map, x, y, T_FLOOR);
        SetTile(map, x2 - 1, y, T_EDGE_E);
    }
    /* The last floor row: the south edge, with plain floor in front of the
     * stairs (vanilla's 0x284 only has plain floor, or the west edge 0x2d8
     * in a corner, north of it), and the south wall. The stairs never touch
     * the east end (0x284 never has 0x29a east of it). */
    for (x = x1 + 1; x < x2; x++)
    {
        u8 tile;

        if (x == stairs || x == stairs + 1)
            tile = (x == x1 + 1) ? T_EDGE_W : T_FLOOR;
        else if (x == x1 + 1)
            tile = T_EDGE_SW;
        else if (x == x2 - 1)
            tile = T_EDGE_SE;
        else
            tile = T_EDGE_S;
        SetTile(map, x, last, tile);
        SetTile(map, x, last + 1, (x == stairs || x == stairs + 1) ? T_STAIRS : T_WALL_S);
    }
    SetTile(map, x1, last + 1, T_WALL_SW);
    SetTile(map, x2, last + 1, T_WALL_SE);
}

/* A room's top row: its corners and the floor between, drawn with the given
 * tiles for the west end, the middle and the east end. */
static void RoomTop(struct TileMap *map, s32 x1, s32 x2, s32 top, u8 cornerW, u8 cornerE,
                    u8 edgeW, u8 edge, u8 edgeE)
{
    s32 x;

    SetTile(map, x1, top, cornerW);
    SetTile(map, x2, top, cornerE);
    SetTile(map, x1 + 1, top, edgeW);
    for (x = x1 + 2; x < x2 - 1; x++)
        SetTile(map, x, top, edge);
    SetTile(map, x2 - 1, top, edgeE);
}

/*
 * A HANGING room cut into the rock's bottom row `top` of a top room, with
 * walls on columns x1 and x2. Beside a vertical ridge its top corner is
 * 0x2ed / 0x2ec; elsewhere it is 0x29c / 0x29b with a 0x299 face cell
 * beside it, and vanilla only puts rock beside that face cell. Returns FALSE,
 * drawing nothing, if it doesn't fit.
 */
static bool8 TryHangingRoom(struct TileMap *map, s32 x1, s32 x2, s32 top, s32 h)
{
    u8 cornerW, cornerE;
    s32 freeW, freeE, x;

    for (x = x1; x <= x2; x++)
    {
        if (TILE_AT(map, x, top) != T_ROCK || TILE_AT(map, x, top - 1) != T_ROCK)
            return FALSE;
    }
    /* freeW..freeE: the columns that must be floor below the top row: the
     * room, and the floor under each face cell. */
    if (TILE_AT(map, x1 - 1, top) == T_RIDGE_E)
    {
        cornerW = T_CUT_W;
        freeW = x1;
    }
    else if (TILE_AT(map, x1 - 1, top) == T_ROCK && TILE_AT(map, x1 - 2, top) == T_ROCK)
    {
        cornerW = T_HANG_W;
        freeW = x1 - 1;
    }
    else
    {
        return FALSE;
    }
    if (TILE_AT(map, x2 + 1, top) == T_RIDGE_W)
    {
        cornerE = T_CUT_E;
        freeE = x2;
    }
    else if (TILE_AT(map, x2 + 1, top) == T_ROCK && TILE_AT(map, x2 + 2, top) == T_ROCK)
    {
        cornerE = T_HANG_E;
        freeE = x2 + 1;
    }
    else
    {
        return FALSE;
    }
    if (!IsFree(map, freeW, top + 1, freeE, top + h + 1))
        return FALSE;
    if (cornerW == T_HANG_W)
        SetTile(map, x1 - 1, top, T_WALL_S);
    if (cornerE == T_HANG_E)
        SetTile(map, x2 + 1, top, T_WALL_S);
    RoomTop(map, x1, x2, top, cornerW, cornerE, T_EDGE_W, T_FLOOR, T_EDGE_E);
    RoomBody(map, x1, x2, top, h);
    KeepFloor(map, freeW, top + 1, freeE, top + h + 1);
    return TRUE;
}

/*
 * An ALCOVE cut into the second cliff row (row + 3) of the horizontal ridge
 * on rows row..row+3, walls on columns x1 and x2: vanilla's alcove at
 * (27..32, 26..28). The cliff must run on for a column beyond each wall.
 */
static bool8 TryAlcove(struct TileMap *map, s32 x1, s32 x2, s32 row, s32 h)
{
    s32 top = row + 3;
    s32 x;

    for (x = x1 - 1; x <= x2 + 1; x++)
    {
        if (TILE_AT(map, x, top) != T_CLIFF || TILE_AT(map, x, top - 1) != T_CLIFF)
            return FALSE;
    }
    if (!IsFree(map, x1 - 1, top + 1, x2 + 1, top + h + 1))
        return FALSE;
    RoomTop(map, x1, x2, top, T_CUT_W, T_CUT_E, T_EDGE_W, T_FLOOR, T_EDGE_E);
    RoomBody(map, x1, x2, top, h);
    KeepFloor(map, x1 - 1, top + 1, x2 + 1, top + h + 1);
    return TRUE;
}

/* A BAR: a free-standing horizontal ridge from column west to east, rows
 * row..row+3, with floor all around it (and the dithered column at its
 * west end). */
static bool8 TryBar(struct TileMap *map, s32 west, s32 east, s32 row)
{
    if (!IsFree(map, west - 2, row - 1, east + 1, row + 4))
        return FALSE;
    DrawRidgeRows(map, west, east, row);
    KeepFloor(map, west - 2, row - 1, east + 1, row + 4);
    return TRUE;
}

/*
 * How many places to try for each kind of room. A try that doesn't fit is
 * skipped; in the prototype of this style these kept about 1.2 hanging
 * rooms, 2.4 alcoves and 0.9 bars per layout.
 */
#define HANGING_TRIES   6
#define ALCOVE_TRIES   16
#define BAR_TRIES      16

/*
 * Try the rooms of a random layout at random places (see THE STRUCTURES):
 * hanging rooms under the rock of the top room without the chamber,
 * alcoves under H1 and H2, and bars in the lower terraces. The sizes are
 * in floor columns and floor rows.
 */
static void TryRooms(struct TileMap *map, const struct Layout *layout)
{
    s32 i, k, side, w, h, x, x1, x2, top, bottom, row;

    side = layout->chamberWest ? 1 : 0;
    x1 = RoomWest(layout, 0, side);
    x2 = RoomEast(map, layout, 0, side);
    for (i = 0; i < HANGING_TRIES; i++)
    {
        w = RandomBetween(4, 9);
        h = RandomBetween(2, 4);
        if (x2 - w - 1 < x1)
            continue;
        x = RandomBetween(x1, x2 - w - 1);   /* the hanging room's west wall */
        TryHangingRoom(map, x, x + w + 1, layout->rockBottom[side], h);
    }

    for (i = 0; i < ALCOVE_TRIES; i++)
    {
        k = MtMoonGen_RandomBelow(RIDGES);
        w = RandomBetween(4, 8);
        h = RandomBetween(2, 4);
        /* The west wall's column: the cliff must run on for a column
         * beyond each wall, so neither wall is next to the ridge's ends. */
        x1 = layout->west[k] + 2;
        x2 = layout->east[k] - 3 - w;
        if (x2 < x1)
            continue;
        x = RandomBetween(x1, x2);
        TryAlcove(map, x, x + w + 1, layout->row[k], h);
    }

    for (i = 0; i < BAR_TRIES; i++)
    {
        k = 1 + MtMoonGen_RandomBelow(TERRACES - 1);
        side = (layout->split[k] != 0) ? MtMoonGen_RandomBelow(2) : 0;
        x1 = RoomWest(layout, k, side);
        x2 = RoomEast(map, layout, k, side);
        top = TerraceTop(layout, k, side);
        bottom = TerraceBottom(map, layout, k);
        w = RandomBetween(4, 16);
        if (bottom - 4 < top + 1 || x2 - 1 - w < x1 + 2)
            continue;
        row = RandomBetween(top + 1, bottom - 4);
        x = RandomBetween(x1 + 2, x2 - 1 - w);   /* the bar's west end */
        TryBar(map, x, x + w - 1, row);
    }
}

/* The rooms of the fallback layout (sFallbackLayout). */
enum
{
    ROOM_HANGING,
    ROOM_ALCOVE,
    ROOM_BAR,
};

struct FallbackRoom
{
    u8 kind;
    u8 x1;   /* west wall (a bar's west end) */
    u8 x2;   /* east wall (a bar's east end) */
    u8 row;  /* hanging: the rock's bottom row; alcove: its ridge's top row;
              * bar: its top row */
    u8 h;    /* floor rows (unused for a bar) */
};

static const struct FallbackRoom sFallbackRooms[] =
{
    { ROOM_HANGING,  28, 37,  8, 3 },
    { ROOM_ALCOVE,   17, 23, 14, 3 },
    { ROOM_BAR,      36, 43, 19, 0 },
    { ROOM_ALCOVE,   21, 28, 24, 2 },
    { ROOM_BAR,       6, 14, 30, 0 },
    { ROOM_BAR,      32, 41, 30, 0 },
};

static void DrawFallbackRooms(struct TileMap *map)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sFallbackRooms); i++)
    {
        const struct FallbackRoom *room = &sFallbackRooms[i];

        switch (room->kind)
        {
        case ROOM_HANGING:
            TryHangingRoom(map, room->x1, room->x2, room->row, room->h);
            break;
        case ROOM_ALCOVE:
            TryAlcove(map, room->x1, room->x2, room->row, room->h);
            break;
        case ROOM_BAR:
            TryBar(map, room->x1, room->x2, room->row);
            break;
        }
    }
}

/* Turn every cell nothing claimed (T_NONE) and every kept cell (T_KEEP)
 * into plain floor. */
static void FillFloor(struct TileMap *map)
{
    s32 i;
    s32 count = map->width * map->height;

    for (i = 0; i < count; i++)
    {
        if (map->tiles[i] < T_CHAMBER)
            map->tiles[i] = T_FLOOR;
    }
}

/* ------------------------------------------------------------------------- */
/* Ladders                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * How far the chamber's box (the chamber and the row above it) stays from
 * each ladder, in king moves (steps in any of the 8 directions): rule 1 of
 * THE FOSSIL CHAMBER in the style header.
 */
#define CHAMBER_LADDER_CLEARANCE 4

/* Steps between two cells when diagonal steps count as one. */
static s32 KingDistance(s32 x1, s32 y1, s32 x2, s32 y2)
{
    return max(abs(x1 - x2), abs(y1 - y2));
}

/* King moves from (x, y) to the chamber's box (0 inside it). */
static s32 ChamberBoxDistance(const struct Layout *layout, s32 x, s32 y)
{
    s32 dx = max(max(layout->chamberX - x, x - (layout->chamberX + MT_MOON_CHAMBER_WIDTH - 1)), 0);
    s32 dy = max(max(layout->chamberY - 1 - y, y - (layout->chamberY + MT_MOON_CHAMBER_HEIGHT - 1)), 0);

    return max(dx, dy);
}

/*
 * TRUE if an up-ladder can stand on (x, y): its whole footprint (the 3 x 4
 * block around the ladder and its top, see MT_MOON_LADDER_UP_IN_FLOOR) is
 * plain floor, which is all vanilla B2F puts around its ladders, and it is
 * far enough from the chamber. Taking the ladder and its top out of a block
 * of floor this size leaves the other 10 cells in one piece, so a ladder
 * never cuts the floor apart.
 */
static bool8 IsLadderSpot(const struct TileMap *map, const struct Layout *layout, s32 x, s32 y)
{
    s32 dx, dy;

    for (dy = -2; dy <= 1; dy++)
    {
        for (dx = -1; dx <= 1; dx++)
        {
            if (TILE_AT(map, x + dx, y + dy) != T_FLOOR)
                return FALSE;
        }
    }
    return ChamberBoxDistance(layout, x, y) >= CHAMBER_LADDER_CLEARANCE;
}

/* TRUE if a ladder can stand on (x, y) (IsLadderSpot) at least
 * MT_MOON_LADDER_MIN_DISTANCE from `otherLadder`, if that isn't NULL. */
static bool8 IsLadderSpotAwayFrom(const struct TileMap *map, const struct Layout *layout,
                                  const struct MtMoonLadder *otherLadder, s32 x, s32 y)
{
    struct MtMoonLadder spot;

    if (otherLadder != NULL)
    {
        spot.x = x;
        spot.y = y;
        if (MtMoonGen_LadderDistance(&spot, otherLadder) < MT_MOON_LADDER_MIN_DISTANCE)
            return FALSE;
    }
    return IsLadderSpot(map, layout, x, y);
}

/* How many random tiles PlaceLadder tries before it looks at every tile. */
#define LADDER_RANDOM_TRIES 48

/*
 * Move `ladder` to a random spot (IsLadderSpotAwayFrom). It tries random
 * tiles first: in the prototype about 1 tile in 5 was a spot (1 in 14 at
 * worst), so a few tries usually find one, which is much quicker than
 * looking at every tile (P9 measures load time). If they all miss, it looks
 * at every tile, giving each spot an equal chance (reservoir sampling, as
 * in RandomGapPlace). Either way every spot is equally likely. Returns
 * FALSE if there is no spot.
 */
static bool8 PlaceLadder(const struct TileMap *map, const struct Layout *layout,
                         struct MtMoonLadder *ladder, const struct MtMoonLadder *otherLadder)
{
    s32 x, y, i, found = 0;
    s32 xCount = MAX_X(map) - MIN_X - 1;   /* x runs from MIN_X + 1 to MAX_X - 1 */
    s32 yCount = MAX_Y(map) - MIN_Y - 2;   /* y runs from MIN_Y + 2 to MAX_Y - 1 */

    for (i = 0; i < LADDER_RANDOM_TRIES; i++)
    {
        x = MIN_X + 1 + MtMoonGen_RandomBelow(xCount);
        y = MIN_Y + 2 + MtMoonGen_RandomBelow(yCount);
        if (IsLadderSpotAwayFrom(map, layout, otherLadder, x, y))
        {
            ladder->x = x;
            ladder->y = y;
            return TRUE;
        }
    }
    for (y = MIN_Y + 2; y <= MAX_Y(map) - 1; y++)
    {
        for (x = MIN_X + 1; x <= MAX_X(map) - 1; x++)
        {
            if (!IsLadderSpotAwayFrom(map, layout, otherLadder, x, y))
                continue;
            found++;
            if (MtMoonGen_RandomBelow(found) == 0)
            {
                ladder->x = x;
                ladder->y = y;
            }
        }
    }
    return found > 0;
}

/* How many entrance spots PlaceLadders tries before giving up on a layout. */
#define ENTRANCE_TRIES 4

/*
 * Put both ladders on the map, replacing the core's proposals (which assume
 * nothing about the shape): the entrance first, then the exit far enough
 * from it; if no exit spot is far enough, another entrance. B2F's two
 * ladders are never fixed (see sFloors in the core). Draws them into the
 * tile map.
 */
static bool8 PlaceLadders(struct TileMap *map, const struct Layout *layout, struct MtMoonCave *cave)
{
    s32 i;

    for (i = 0; i < ENTRANCE_TRIES; i++)
    {
        if (!PlaceLadder(map, layout, &cave->entrance, NULL))
            return FALSE;
        if (PlaceLadder(map, layout, &cave->exit, &cave->entrance))
        {
            SetTile(map, cave->entrance.x, cave->entrance.y - 1, T_LADDER_TOP);
            SetTile(map, cave->entrance.x, cave->entrance.y, T_LADDER);
            SetTile(map, cave->exit.x, cave->exit.y - 1, T_LADDER_TOP);
            SetTile(map, cave->exit.x, cave->exit.y, T_LADDER);
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Checks                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * TRUE if every pair of neighbouring tiles is a pair vanilla B2F has
 * (sEastOk, sSouthOk). The fossil chamber's cells are skipped: the core
 * draws vanilla's own tiles there, and the layout always gives the chamber
 * the surroundings vanilla gives it (rock above, floor west and south, a
 * vertical ridge east), apart from the one pair test P5 allows at its
 * north-east corner (the rock above the chamber's top-right tile 0x29a,
 * which vanilla only puts under 0x292).
 */
static bool8 IsDrawable(const struct TileMap *map)
{
    const u8 *cell = map->tiles;   /* walks the map in reading order */
    s32 x, y;

    for (y = 0; y < map->height; y++)
    {
        for (x = 0; x < map->width; x++, cell++)
        {
            u8 tile = cell[0];
            u8 east, south;

            if (tile == T_CHAMBER)
                continue;
            if (x + 1 < map->width)
            {
                east = cell[1];
                if (east != T_CHAMBER && !HAS_TILE(sEastOk[tile], east))
                    return FALSE;
            }
            if (y + 1 < map->height)
            {
                south = cell[map->width];
                if (south != T_CHAMBER && !HAS_TILE(sSouthOk[tile], south))
                    return FALSE;
            }
        }
    }
    return TRUE;
}

/*
 * The walkable share test P6 allows, in percent of the map's tiles, a point
 * inside its 36-56% band at each end; and what the count adds or takes
 * away: the fossil chamber's 80 walkable tiles (the core draws them), minus
 * the 2 hidden items' boulders the core stamps onto floor.
 */
#define MIN_WALKABLE_PERCENT 37
#define MAX_WALKABLE_PERCENT 55
#define CHAMBER_WALKABLE     80
#define B2F_HIDDEN_ITEMS      2

/* TRUE if `tile` can be walked on (the chamber's cells don't count here). */
static bool8 IsWalkableTile(u8 tile)
{
    return tile != T_CHAMBER && IS_WALKABLE_VALUE(sTileValues[tile]);
}

static bool8 IsDensityOk(const struct TileMap *map)
{
    s32 i, walkable = CHAMBER_WALKABLE - B2F_HIDDEN_ITEMS;
    s32 count = map->width * map->height;

    for (i = 0; i < count; i++)
    {
        if (IsWalkableTile(map->tiles[i]))
            walkable++;
    }
    return walkable * 100 >= MIN_WALKABLE_PERCENT * count
        && walkable * 100 <= MAX_WALKABLE_PERCENT * count;
}

/*
 * ROOM FOR THE CONTENT
 *
 * Outside the chamber, B2F's content is 10 pieces (data/maps/MtMoon_B2F):
 * 4 Team Rocket grunts, 4 item balls and 2 hidden items. The core puts each
 * on a free floor tile at least 3 steps (counting diagonal steps) from both
 * ladders, 2 from the chamber's box and 2 from every other piece, picking
 * spots at random, and stamps the hidden items' boulders where canStampAt
 * agrees. If it found no spot it would carve new floor, which this style's
 * tiles can't draw. So the layout must have room for all 10 however the
 * first 9 were placed.
 *
 * An OPEN SPOT here is a tile whose 3 x 3 block is walkable floor, with
 * plain floor on it and north, south, west and east of it (so canStampAt
 * agrees), clear of the ladders and the chamber as above. Only tiles with an
 * even x and an even y count, so any two open spots are at least 2 steps
 * apart: a placed piece is then within 1 step of at most 4 of them (the
 * 2 x 2 of even tiles in its 3 x 3), and every other open spot still
 * follows all of the core's rules. With 4 x 9 + 1 = 37 open spots, 9
 * pieces anywhere leave one for the 10th.
 */
#define B2F_CONTENT_PIECES 10
#define MIN_OPEN_SPOTS (4 * (B2F_CONTENT_PIECES - 1) + 1)

/* TRUE if (x, y) and the tiles north, south, west and east of it are plain
 * floor: where the core may stamp a boulder (CanStampB2FPrefabAt). (x, y)
 * must not be on the map's edge. */
static bool8 IsPlainFloorCross(const struct TileMap *map, s32 x, s32 y)
{
    return TILE_AT(map, x, y) == T_FLOOR
        && TILE_AT(map, x - 1, y) == T_FLOOR && TILE_AT(map, x + 1, y) == T_FLOOR
        && TILE_AT(map, x, y - 1) == T_FLOOR && TILE_AT(map, x, y + 1) == T_FLOOR;
}

static bool8 IsOpenSpot(const struct TileMap *map, const struct Layout *layout,
                        const struct MtMoonCave *cave, s32 x, s32 y)
{
    s32 dx, dy;

    if (!IsPlainFloorCross(map, x, y))
        return FALSE;
    for (dy = -1; dy <= 1; dy += 2)
    {
        for (dx = -1; dx <= 1; dx += 2)
        {
            if (!IsWalkableTile(TILE_AT(map, x + dx, y + dy)))
                return FALSE;
        }
    }
    return KingDistance(x, y, cave->entrance.x, cave->entrance.y) >= 3
        && KingDistance(x, y, cave->exit.x, cave->exit.y) >= 3
        && ChamberBoxDistance(layout, x, y) >= 2;
}

static bool8 HasRoomForContent(const struct TileMap *map, const struct Layout *layout,
                               const struct MtMoonCave *cave)
{
    s32 x, y, count = 0;

    for (y = MIN_Y; y <= MAX_Y(map); y += 2)
    {
        for (x = MIN_X; x <= MAX_X(map); x += 2)
        {
            if (IsOpenSpot(map, layout, cave, x, y) && ++count == MIN_OPEN_SPOTS)
                return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Putting it together                                                       */
/* ------------------------------------------------------------------------- */

/* How many whole layouts to try (the last one is sFallbackLayout), and how
 * many random draws ChooseLayout gets per layout. */
#define MAX_LAYOUTS  8
#define CHOOSE_TRIES 16

/* The work area as a tile map of the cave's size, or FALSE if the cave is
 * bigger than the work area (never: B2F is 48 x 40). */
static bool8 GetTileMap(const struct MtMoonCave *cave, struct TileMap *map)
{
    map->tiles = MtMoonGen_GetStyleWork();
    map->width = cave->width;
    map->height = cave->height;
    return map->width * map->height <= WORK_CELLS;
}

/* Write the finished layout into the core's grid: walkable tiles are floor
 * (the grid is all wall when buildShape starts), and the chamber goes where
 * the layout put it. */
static void WriteGrid(const struct TileMap *map, const struct Layout *layout)
{
    struct MtMoonChamber *chamber = MtMoonGen_GetChamber();
    s32 x, y;

    for (y = 0; y < map->height; y++)
    {
        for (x = 0; x < map->width; x++)
        {
            if (IsWalkableTile(TILE_AT(map, x, y)))
                MtMoonGen_SetCell(x, y, MT_MOON_CELL_FLOOR);
        }
    }
    if (chamber != NULL)
    {
        chamber->x = layout->chamberX;
        chamber->y = layout->chamberY;
        MtMoonGen_MarkChamberSite();
    }
}

static void BuildB2FShape(struct MtMoonCave *cave)
{
    struct TileMap map;
    struct Layout layout;
    s32 attempt, tries, i, cells;

    if (!GetTileMap(cave, &map))
        return;
    cells = map.width * map.height;
    for (attempt = 0; attempt < MAX_LAYOUTS; attempt++)
    {
        for (i = 0; i < cells; i++)
            map.tiles[i] = T_NONE;
        if (attempt < MAX_LAYOUTS - 1)
        {
            for (tries = 0; tries < CHOOSE_TRIES; tries++)
            {
                if (ChooseLayout(&map, &layout))
                    break;
            }
            if (tries == CHOOSE_TRIES)
                continue;
            DrawSkeleton(&map, &layout);
            TryRooms(&map, &layout);
        }
        else
        {
            layout = sFallbackLayout;
            DrawSkeleton(&map, &layout);
            DrawFallbackRooms(&map);
        }
        FillFloor(&map);
        if (PlaceLadders(&map, &layout, cave) && IsDrawable(&map) && IsDensityOk(&map)
            && HasRoomForContent(&map, &layout, cave))
        {
            WriteGrid(&map, &layout);
            return;
        }
    }
    /* Never reached: the fallback layout passes. If it ever didn't, the
     * grid is all wall, the core's ladder check fails, and the core asks
     * for a new shape (and in the end keeps the vanilla map). */
}

/* Plain floor or rock, matching whether the core's grid says (x, y) can be
 * walked on. */
static u16 PlainTileFor(s32 x, s32 y)
{
    return sTileValues[MtMoonGen_IsFloor(x, y) ? T_FLOOR : T_ROCK];
}

/*
 * The tile for map tile (x, y): the one the layout chose. The ladders are
 * in the tile map too, with vanilla B2F's freestanding top 0x28e. The
 * chamber's cells get plain floor or rock: the core draws the chamber's own
 * tiles over them.
 *
 * SAFETY NET: the core never changes a finished layout of this style (it
 * has no pockets and room for all the content), but if it ever did (filling
 * a pocket, carving a corridor), the tile chosen here would no longer match
 * whether the cell can be walked on, and the player could walk through rock
 * or bump into floor. Then the cell gets plain floor or rock, which keeps
 * the floor walkable exactly where the core thinks it is; test P5 would show
 * the seam.
 */
static u16 PickB2FTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    struct TileMap map;
    u8 tile;
    u16 value;

    if (!GetTileMap(cave, &map))
        return PlainTileFor(x, y);
    tile = TILE_AT(&map, x, y);
    if (tile == T_CHAMBER)
        return PlainTileFor(x, y);
    value = sTileValues[tile];
    if (IS_WALKABLE_VALUE(value) != MtMoonGen_IsFloor(x, y))
        return PlainTileFor(x, y);
    return value;
}

/*
 * The core asks this before it stamps a hidden item's boulder (0x282) on tile
 * (x, y) (canStampAt in include/mt_moon_gen_style.h). Vanilla B2F only puts
 * plain floor (or more boulders) north, south, west and east of its
 * boulders, so (x, y) and those four must be plain floor here: not the edge
 * floor of a room, the dithered floor or the stairs.
 */
static bool8 CanStampB2FPrefabAt(const struct MtMoonCave *cave, s32 x, s32 y)
{
    struct TileMap map;

    if (!GetTileMap(cave, &map) || x < 1 || y < 1 || x >= map.width - 1 || y >= map.height - 1)
        return FALSE;
    return IsPlainFloorCross(&map, x, y);
}

const struct MtMoonStyle gMtMoonStyleB2F =
{
    .buildShape = BuildB2FShape,
    .pickTile = PickB2FTile,
    .canStampAt = CanStampB2FPrefabAt,
    .workBytes = WORK_CELLS,
};
