/*
 * mt_moon_style_1f.c - Mt. Moon 1F style: rock islands and sand in open floor
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * The floor style for Mt. Moon 1F (see include/mt_moon_gen_style.h for how
 * styles plug into the generator core in src/mt_moon_gen.c). Vanilla 1F is
 * a big open cave floor with pink rock "islands" standing in it and patches
 * of yellow sand, inside a 2-tile band of rock around the map's edge. This
 * style builds that, from three parts:
 *
 *   1. The BORDER BAND: the same 2 tiles of rock around the map as vanilla
 *      1F (rows 0-1 and 38-39, columns 0-1 and 46-47), with 1F's doorway to
 *      Route 4 in the south band at (18,37).
 *   2. ROCK ISLANDS in the open floor: rectangles at least 3 x 3 tiles and
 *      L-shapes, with at least 2 floor tiles between any two of them.
 *   3. SAND PATCHES with soft edges: staircase-shaped patches in the
 *      corners, sometimes a strip along the north wall with a patch hanging
 *      from it, sometimes a free patch in the open.
 *
 * Every tile is drawn with the vanilla tile for its role, from the tables
 * below, and only next to tiles vanilla 1F itself puts beside it: every pair
 * of north-south or east-west neighbours this style draws appears somewhere
 * in vanilla 1F's own map (data/layouts/MtMoon_1F/map.bin). That is what
 * test P5-1F checks. Shapes those tiles can't draw are never generated; the
 * sections below say which shapes those are and why.
 *
 * About 61% of the map is walkable (57-66% over 100 seeds; vanilla: 60%;
 * test P6-1F wants 50-70%). Every random choice uses MtMoonGen_Random(), so
 * a seed always builds the same floor.
 *
 * MAP CELL VALUES: a value written into the map is
 *     metatile id | (collision << 10) | (elevation << 12)
 * so 0x3281 is metatile 0x281 (cave floor) with collision 0 (walkable) and
 * elevation 3 (the height of the cave floor), and 0x0691 is metatile 0x291
 * with collision 1 (solid) and elevation 0. Below, "tile 289" means
 * metatile 0x289.
 *
 * WHAT THE CORE DOES AFTERWARDS (see include/mt_moon_gen_style.h):
 *   - Pockets: there are none. The floor inside the border band is one
 *     connected piece, because islands are 2+ tiles apart and only touch the
 *     west and south borders (see ROCK ISLANDS).
 *   - Content: 1F's 13 trainers, NPCs and item balls stand on open floor or
 *     sand (they are sprites, so the tiles under them don't change). Its
 *     one sign is a stamped signpost, which vanilla only puts next to plain
 *     floor, so CanStamp1FPrefabAt keeps it away from sand.
 *   - Repairs: the core only carves new floor when no spot fits a piece of
 *     content. With 57-66% open floor that doesn't happen: in a prototype of
 *     this style over 5,000 layouts, the most cramped still had 36 spots for
 *     the Hiker's 5 x 5 roaming box and 214 for the sign. (A carve would cut
 *     into an island and draw pairs test P5-1F rejects.)
 *
 * LOAD TIME: all of this runs while the screen is black between two warps
 * (test P9 allows 30 frames more than a vanilla cave). See the notes on the
 * island tries and on the sand map.
 * ============================================================================
 */

#include "global.h"
#include "mt_moon_gen_style.h"

/* The border band is this many tiles thick on every side. */
#define BAND 2

/*
 * ============================================================================
 * ROCK ISLANDS
 * ============================================================================
 *
 * An island is drawn from vanilla's 3 x 3 rock panel (metatiles 288-29a),
 * stretched by repeating its middle row and column:
 *
 *     288 289 289 289 28a      the rock's thin top rim, floor north of it
 *     290 291 291 291 292      west edge, top of the rock, east edge
 *     290 291 291 291 292
 *     298 299 299 299 29a      the rock's tall south face, floor south of it
 *
 * The camera looks at the cave from the south, so a rock shows its tall,
 * shaded side to the SOUTH and only a thin rim to the north. Each rock cell
 * picks its tile from which of its neighbours are floor (GetRockTile):
 * floor to the north gives the rim, floor to the south the face, floor to
 * the west or east an edge, two of them a corner.
 *
 * L-shapes add the inner corner 29c, where the bottom of a horizontal arm
 * meets the west edge of an arm hanging down from its east end:
 *
 *     288 289 289 289 289 28a
 *     290 291 291 291 291 292
 *     298 299 299 29c 291 292      29c: floor to its south-west only
 *                 290 291 292
 *                 298 299 29a
 *
 * WHICH SHAPES VANILLA'S TILES CAN DRAW (from vanilla 1F's tile pairs):
 *   - At least 3 x 3: a rim corner (288, 28a) is never next to the other
 *     rim corner or above a face corner (298, 29a), so rock 1 or 2 tiles
 *     thick has no tiles.
 *   - L-shapes only as drawn above (a "7"). The other three ways of bending
 *     an L need an inner corner next to neighbours vanilla 1F never gives
 *     it. Both arms are 3 thick, and each reaches at least 2 tiles past the
 *     other (29c is never next to a corner tile).
 *   - An island may stand against the west border (as vanilla's L does)
 *     and on the south band (as vanilla's pillars by the doorway do): the
 *     band's tiles there (292 to the west, 289 to the south) fit next to an
 *     island's edge and face. It keeps 2 floor tiles from the north border
 *     (vanilla never puts a rim corner 288 under the north wall's face 299)
 *     and from the east border (a face corner 29a never touches its edge
 *     290). An island against the border is still drawn with its own edge
 *     or face there: GetRockTile counts the border band as open ground.
 *
 * Islands are at least 2 floor tiles apart (sometimes 3), diagonals
 * included. With only the west and south borders touching them, and an L
 * never touching a border (its open corner would wall off a pocket), the
 * floor around them is always one connected piece.
 */
#define TILE_ROCK_TOP_LEFT     0x0688
#define TILE_ROCK_TOP          0x0689
#define TILE_ROCK_TOP_RIGHT    0x068a
#define TILE_ROCK_LEFT         0x0690
#define TILE_ROCK_MIDDLE       0x0691
#define TILE_ROCK_RIGHT        0x0692
#define TILE_ROCK_BOTTOM_LEFT  0x0698
#define TILE_ROCK_BOTTOM       0x0699
#define TILE_ROCK_BOTTOM_RIGHT 0x069a
#define TILE_ROCK_INNER_SW     0x069c  /* L-shape inner corner, floor to the SW */

/*
 * How islands are placed (Build1FShape, TryIsland): random island shapes
 * are tried at random spots, or half the time lined up 2-3 tiles beside a
 * recent island (vanilla's islands stand in rows and columns), and kept
 * where they fit (IsIslandSpotFree), until the rock reaches a random target
 * of 420-499 tiles or the tries run out. A last round of tries fills gaps
 * with small 3 x 3, 3 x 4 and 4 x 3 islands. Most layouts end with
 * 380-450 rock tiles: the spacing rules stop the target being reached.
 * (In a prototype, twice as many tries added only about 0.5% more rock.)
 *
 * Shape mix: 30% bars (4-14 wide, 3-4 tall), 35% pillars (3-4 wide, 4-12
 * tall), 17% blocks (3-6 x 3-6), 18% L-shapes (5-11 wide, 5-10 tall).
 *
 * LOAD TIME: a try reads the grid cells around its spot and stops at the
 * first one that isn't floor, which in a filled cave comes early.
 */
#define ISLAND_TRIES        700
#define FILL_TRIES          300
#define ROCK_TARGET_MIN     420
#define ROCK_TARGET_SPREAD  80
#define RECENT_ISLANDS      16   /* islands remembered for lining up; a power of 2 */

/* How far islands keep from other things, in tiles. */
#define ISLAND_GAP          2    /* floor tiles between two islands (+1 a quarter of the time) */
#define BORDER_GAP          2    /* floor tiles to a border the island doesn't stand against */
#define HOLE_GAP            2    /* rock is more than this from the hole: its 3 x 3 footprint
                                  * plus a ring of floor */
#define DOOR_PLAZA_HALF     3    /* no rock within 3 columns of the doorway, */
#define DOOR_PLAZA_DEPTH    4    /* in its row and the 4 rows above it */

/* A box of map tiles, corners included. */
struct TileBox
{
    s8 x1;
    s8 y1;
    s8 x2;
    s8 y2;
};

/*
 * ============================================================================
 * SAND
 * ============================================================================
 *
 * Vanilla's sand tiles describe the sand by CORNER. Think of a grid of
 * points ("vertices") at the corners of the map tiles: vertex (vx, vy) is
 * the top-left corner of tile (vx, vy). Each vertex is sand or not, and a
 * floor tile is drawn from its four corners, with sand where they are:
 *
 *     corners       tile   picture
 *     none          281    plain cave floor
 *     all four      2fc    full sand
 *     NW NE         304    sand in the tile's top half
 *     NW            305    a corner of sand in the tile's top-left
 *     NW NE SW      301    sand everywhere but the bottom-right
 *     ... and the rest of the 16 (sSandTiles), but for the two "diagonal"
 *     ones (NW+SE, NE+SW), which have no vanilla tile.
 *
 * So a patch of sand is a set of sand vertices, and its edge runs through
 * the middle of the tiles around it, which are drawn half sand, half floor:
 * the soft edge. Example, with sand vertices # and floor vertices .:
 *
 *     .   .   .   .           tiles:  2f3 2f4 2f5
 *       +---+---+                     2fb 2fc 2fd
 *     . | # | # | .                   303 304 305
 *       +---+---+
 *     . | # | # | .
 *       +---+---+
 *     .   .   .   .
 *
 * The border band has sand versions of its edge tiles too (the north
 * wall's face 311 instead of 299, ...), chosen from the sand vertices along
 * it (GetBorderTile).
 *
 * WHICH SAND SHAPES THE TILES CAN DRAW (from vanilla 1F's tile pairs):
 *   - No two diagonal vertices of a tile may be sand with the other two not
 *     (no tile). Every shape below is made so this can't happen.
 *   - A patch's first and last row and column are at least 2 vertices long:
 *     a 1-vertex tip draws as two corner tiles side by side (303 305, 2f3
 *     above 303, ...), which vanilla never does. (Along the west and east
 *     borders a 1-vertex column is fine: it draws as a strip of half-sand
 *     tiles beside the border, as in vanilla.)
 *   - Two patches are at least 3 vertices apart, so a whole tile of plain
 *     floor is between them: a tile touching two patches has no picture.
 *   - Sand keeps away from the hole, the doorway and rock islands: vanilla
 *     only puts plain floor (281) next to those.
 *   - The north-west, south-west and south-east corners of the map always
 *     have sand: vanilla's border has no tiles for those corners with plain
 *     floor in front of them (its corner tiles only meet the sand versions
 *     of the border's edges).
 *   - Under the north wall, sand can only start at the north-west corner:
 *     vanilla has a tile where the wall's strip of sand ends going east
 *     (314), but none where one begins. So the sand there is either the
 *     north-west corner's patch, or a strip along the whole north wall.
 *   - Where sand along the west or east border ends, the patch steps away
 *     from the border: the border tile there (31e, 30e, 313, 30d) is only
 *     ever next to a patch's corner tile.
 *
 * THE PATCHES, in vertex coordinates (for 1F's 48 x 40 map, the inside of
 * the border band spans vertices 2..46 across and 2..38 down):
 *   - CORNER patches at the north-west, south-west and south-east corners
 *     (and at the north-east with the north strip): a staircase hugging the
 *     corner. It runs 3-11 vertices along the north or south border as a
 *     1-vertex-tall row, then steps away from that border toward the west or
 *     east border, ending in a 1-vertex-wide tail along it, at most 16
 *     vertices from the corner. Each step is a rectangle anchored at the
 *     corner, so if a vertex is sand, so is every vertex between it and the
 *     corner: no diagonal pair can occur. With those limits a north and a
 *     south patch stay 4+ vertices apart.
 *   - The NORTH STRIP (1 cave in 4): sand along the whole north wall, with a
 *     patch hanging under it between vertex columns 17 and 31, each row
 *     narrower than the one above, the last at least 2 wide (it can't reach
 *     the corner patches, which end at column 13 and start at 35).
 *   - One FREE patch (3 caves in 4 without the strip): an octagon 6-12
 *     vertices wide and 4-8 tall, kept 3+ vertices from the other patches
 *     and the doorway and away from the hole; dropped if it doesn't fit.
 * A corner patch that would reach the hole or the doorway is drawn smaller,
 * at worst as a 4 x 4 staircase that the hole is never put near
 * (MoveExitClearOfCornerSand).
 *
 * THE SAND MAP: buildShape lays the sand once (LaySand) and keeps it as one
 * bit per vertex in the style's work area (MtMoonGen_GetStyleWork, 252
 * bytes of the heap block the core borrows for the grid): the grid itself
 * only says wall or floor. pickTile and CanStamp1FPrefabAt read the bits.
 */
#define TILE_FLOOR 0x3281

/* The four corners of a tile, as bits of a sand mask. */
#define SAND_NW (1 << 3)
#define SAND_NE (1 << 2)
#define SAND_SW (1 << 1)
#define SAND_SE (1 << 0)

/* The floor tile for each sand mask: walkable, elevation 3. The two
 * diagonal masks (6 and 9) have no vanilla tile and are never produced (see
 * above); 0 is no Mt. Moon tile, so test P5-1F would catch one at once. */
static const u16 sSandTiles[16] =
{
    TILE_FLOOR, /* .  .  .  . : plain floor */
    0x32f3, /*  .  .  . SE */
    0x32f5, /*  .  . SW  . */
    0x32f4, /*  .  . SW SE : sand in the bottom half */
    0x3303, /*  . NE  .  . */
    0x32fb, /*  . NE  . SE : sand in the right half */
    0,      /*  . NE SW  . : never */
    0x32f8, /*  . NE SW SE */
    0x3305, /* NW  .  .  . */
    0,      /* NW  .  . SE : never */
    0x32fd, /* NW  . SW  . : sand in the left half */
    0x32f9, /* NW  . SW SE */
    0x3304, /* NW NE  .  . : sand in the top half */
    0x3300, /* NW NE  . SE */
    0x3301, /* NW NE SW  . */
    0x32fc, /* NW NE SW SE : full sand */
};

/* Corner patches, in the order they are laid. NE only exists with the north
 * strip, so it comes last. */
enum
{
    CORNER_NW,
    CORNER_SW,
    CORNER_SE,
    CORNER_NE,
    CORNER_COUNT
};

/* Which way each corner patch grows from its corner vertex: +1 east or
 * south, -1 west or north. Its corner vertex (CORNER_X, CORNER_Y) is
 * (2, 2), (2, 38), (46, 38) or (46, 2) on 1F. */
static const s8 sCornerDirX[CORNER_COUNT] = { 1, 1, -1, -1 };
static const s8 sCornerDirY[CORNER_COUNT] = { 1, -1, -1, 1 };
#define CORNER_X(cave, c) (sCornerDirX[c] > 0 ? BAND : (cave)->width - BAND)
#define CORNER_Y(cave, c) (sCornerDirY[c] > 0 ? BAND : (cave)->height - BAND)

#define SAND_MAX_DEPTH  16  /* most vertices a corner patch reaches along the west/east border */
#define SAND_MAX_STEPS  12  /* steps of a staircase (see GenerateSteps) */
#define SAND_SHRINKS    3   /* sizes tried before the smallest corner patch */
#define SAND_PATCH_GAP  3   /* vertices between two patches */

#define STRIP_ROW       2   /* the north strip's vertex row */
#define HANG_X_MIN      17  /* the hanging patch stays within these vertex */
#define HANG_X_MAX      31  /* columns, clear of the corner patches */
#define HANG_MAX_ROWS   5
#define FREE_PATCH_MIN  5   /* the free patch's top-left vertex is at least this
                             * far from the map's top-left */

/* The sand map: one bit per vertex of 1F's 48 x 40 map, row by row. */
#define SAND_MAP_WIDTH  49
#define SAND_MAP_HEIGHT 41
#define SAND_MAP_WORDS  ((SAND_MAP_WIDTH * SAND_MAP_HEIGHT + 31) / 32)

struct SandMap
{
    u32 bits[SAND_MAP_WORDS];
};

/* A box of vertices, corners included. */
struct VertexBox
{
    s16 x1;
    s16 y1;
    s16 x2;
    s16 y2;
};

/*
 * A corner patch: step i is the rectangle of vertices that reach up to
 * reach[i] from the corner along the north or south border and up to
 * depth[i] along the west or east border. reach shrinks and depth grows from
 * one step to the next, and the last step has reach 0 (the tail along the
 * west or east border).
 */
struct SandCorner
{
    u8 count;   /* number of steps */
    u8 reach[SAND_MAX_STEPS];
    u8 depth[SAND_MAX_STEPS];
};

/*
 * ============================================================================
 * BORDER BAND
 * ============================================================================
 *
 * Vanilla 1F's 2-tile band of rock, tile for tile (GetBorderTile). The
 * outer ring is the cave's outer rock; the inner ring faces the floor, and
 * its tiles follow the sand in front of them:
 *
 *   north, row 1      299 face over floor, 311 face over sand, 314 where
 *                     the sand in front ends going east
 *   west, column 1    292 edge beside floor, 316 beside sand, 31e and 30e
 *                     where the sand beside it ends or starts going south
 *   east, column 46   290, 315, 313, 30d: the same, mirrored
 *   south, row 38     289, the rim of the south band, whatever is in front
 *   corners           29b (north-west, outer and inner), 29c (north-east),
 *                     2a3 (south-west), 2a4 (south-east)
 * The doorway at (18,37), its frame below it and the wall below that are
 * the core's ladder tiles (MtMoonGen_GetLadderTile).
 */
#define TILE_BORDER_NW              0x069b  /* outer and inner north-west corner */
#define TILE_BORDER_NE              0x069c  /* outer and inner north-east corner */
#define TILE_BORDER_OUTER_NORTH     0x06c4
#define TILE_BORDER_OUTER_WEST      0x06c7
#define TILE_BORDER_OUTER_EAST      0x06c6
#define TILE_BORDER_OUTER_SOUTH     0x06a3  /* also the inner south-west corner */
#define TILE_BORDER_SOUTH_EAST      0x06a4  /* inner south-east corner */
#define TILE_BORDER_SOUTH           0x0689  /* the south band's rim */
#define TILE_BORDER_NORTH           0x0699  /* the face, floor in front */
#define TILE_BORDER_NORTH_SAND      0x0711  /* the face, sand in front */
#define TILE_BORDER_NORTH_SAND_END  0x0714  /* sand in front of its west half only */
#define TILE_BORDER_WEST            0x0692
#define TILE_BORDER_WEST_SAND       0x0716
#define TILE_BORDER_WEST_SAND_END   0x071e  /* sand beside its top half only */
#define TILE_BORDER_WEST_SAND_START 0x070e  /* sand beside its bottom half only */
#define TILE_BORDER_EAST            0x0690
#define TILE_BORDER_EAST_SAND       0x0715
#define TILE_BORDER_EAST_SAND_END   0x0713
#define TILE_BORDER_EAST_SAND_START 0x070d

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static bool8 BoxesMeet(const struct VertexBox *a, const struct VertexBox *b, s32 gap)
{
    return a->x1 - gap <= b->x2 && b->x1 - gap <= a->x2
        && a->y1 - gap <= b->y2 && b->y1 - gap <= a->y2;
}

static void SetBox(struct VertexBox *box, s32 x1, s32 y1, s32 x2, s32 y2)
{
    box->x1 = x1;
    box->y1 = y1;
    box->x2 = x2;
    box->y2 = y2;
}

/* Sand keeps out of these boxes of vertices: around the hole, its four
 * neighbours must be plain floor (their corners are the vertices
 * x-1..x+2 x y-1..y+2), plus one more vertex all round; around the doorway,
 * the same for its three floor neighbours, down to the south band. */
static void GetHoleBox(const struct MtMoonCave *cave, struct VertexBox *box)
{
    SetBox(box, cave->exit.x - 2, cave->exit.y - 2, cave->exit.x + 3, cave->exit.y + 3);
}

static void GetDoorBox(const struct MtMoonCave *cave, struct VertexBox *box)
{
    SetBox(box, cave->entrance.x - 2, cave->entrance.y - 2, cave->entrance.x + 3, cave->height - BAND);
}

/* ------------------------------------------------------------------------- */
/* The sand map                                                              */
/* ------------------------------------------------------------------------- */

static struct SandMap *GetSandMap(void)
{
    return MtMoonGen_GetStyleWork();
}

/* TRUE if vertex (vx, vy) is sand. Vertices off the map are not. */
static bool8 IsSand(const struct SandMap *map, s32 vx, s32 vy)
{
    u32 i;

    if ((u32)vx >= SAND_MAP_WIDTH || (u32)vy >= SAND_MAP_HEIGHT)
        return FALSE;
    i = vy * SAND_MAP_WIDTH + vx;
    return (map->bits[i >> 5] >> (i & 31)) & 1;
}

static void SetSand(struct SandMap *map, s32 vx, s32 vy)
{
    u32 i;

    if ((u32)vx >= SAND_MAP_WIDTH || (u32)vy >= SAND_MAP_HEIGHT)
        return;
    i = vy * SAND_MAP_WIDTH + vx;
    map->bits[i >> 5] |= 1u << (i & 31);
}

/* Make vertices x1..x2 of row vy sand. */
static void SetSandRow(struct SandMap *map, s32 vy, s32 x1, s32 x2)
{
    for (; x1 <= x2; x1++)
        SetSand(map, x1, vy);
}

/* Bit i of the sand map (vertex i, counting row by row). */
#define SAND_BIT(map, i) (((map)->bits[(i) >> 5] >> ((i) & 31)) & 1)

/* Which corners of map tile (x, y) are sand, as a mask of SAND_NW, SAND_NE,
 * SAND_SW and SAND_SE. pickTile asks this for every floor tile, so it reads
 * the four bits directly: a tile on the map always has its corners in the
 * sand map. */
static u32 GetSandCorners(const struct SandMap *map, s32 x, s32 y)
{
    u32 i = y * SAND_MAP_WIDTH + x;  /* the tile's north-west corner */

    return (SAND_BIT(map, i) << 3) | (SAND_BIT(map, i + 1) << 2)
         | (SAND_BIT(map, i + SAND_MAP_WIDTH) << 1) | SAND_BIT(map, i + SAND_MAP_WIDTH + 1);
}

/* TRUE if any vertex of the box is sand. */
static bool8 IsSandInBox(const struct SandMap *map, const struct VertexBox *box)
{
    s32 vx, vy;

    for (vy = box->y1; vy <= box->y2; vy++)
        for (vx = box->x1; vx <= box->x2; vx++)
            if (IsSand(map, vx, vy))
                return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Laying the sand: corner patches                                           */
/* ------------------------------------------------------------------------- */

static void AddStep(struct SandCorner *corner, s32 reach, s32 depth)
{
    corner->reach[corner->count] = reach;
    corner->depth[corner->count] = depth;
    corner->count++;
}

/*
 * The staircase of a corner patch, from the random `bits`; each `shrink`
 * halves its reach along the north/south border and its tail. Step by step:
 *   - reach 4-11 along the north or south border (at least 3), 1 vertex tall
 *     (sand along those borders must end in a step: tile 314 is followed by
 *     a corner tile);
 *   - 1-2 shorter, 2 vertices deep: this keeps a 3 x 3 block of vertices at
 *     the corner, so the patch has full sand tiles;
 *   - then 1-3 shorter and 1-2 deeper at each step, until the reach is 0;
 *   - that last step is the 1-8 vertex tail along the west or east border.
 * After the first two steps the reach is at most 10, and it drops by at
 * least 1 per step, so there are at most 12 steps.
 */
static void GenerateSteps(struct SandCorner *corner, u32 bits, s32 shrink)
{
    s32 reach = max(3, (s32)(4 + (bits & 7)) >> shrink);
    s32 depth;

    corner->count = 0;
    bits >>= 3;
    AddStep(corner, reach, 0);
    reach = max(2, reach - 1 - (s32)(bits & 1));
    bits >>= 1;
    depth = 2;
    AddStep(corner, reach, depth);
    while (reach > 0)
    {
        s32 shorter = 1 + (bits & 1) + ((bits >> 1) & 1);
        s32 deeper = 1 + ((bits >> 2) & 1);

        bits >>= 3;
        reach = max(0, reach - shorter);
        if (reach == 0)
        {
            deeper = 1 + ((s32)(bits & 7) >> shrink);
            bits >>= 3;
        }
        depth += deeper;
        AddStep(corner, reach, depth);
    }
}

/* How far a corner patch reaches along the north/south border at `depth`
 * vertices from it: the reach of the first step that deep, or -1 if the
 * patch doesn't reach that deep. */
static s32 CornerReachAt(const struct SandCorner *corner, s32 depth)
{
    s32 i;

    for (i = 0; i < corner->count; i++)
    {
        if (corner->depth[i] >= depth)
            return corner->reach[i];
    }
    return -1;
}

/* The vertices corner patch `c` covers, as a box. */
static void GetCornerBox(const struct MtMoonCave *cave, s32 c, const struct SandCorner *corner, struct VertexBox *box)
{
    s32 ax = CORNER_X(cave, c), ay = CORNER_Y(cave, c);
    s32 farX = ax + sCornerDirX[c] * corner->reach[0];
    s32 farY = ay + sCornerDirY[c] * corner->depth[corner->count - 1];

    SetBox(box, min(ax, farX), min(ay, farY), max(ax, farX), max(ay, farY));
}

/* TRUE if some step of corner patch `c` reaches into `box`. A step is a
 * rectangle anchored at the corner, so it reaches the box exactly when it
 * reaches the box's vertex nearest the corner. */
static bool8 CornerMeetsBox(const struct MtMoonCave *cave, s32 c, const struct SandCorner *corner, const struct VertexBox *box)
{
    s32 ax = CORNER_X(cave, c), ay = CORNER_Y(cave, c);
    s32 nearX, nearY, farX, farY, i;

    /* The box's sides, counted inward from the corner. */
    if (sCornerDirX[c] > 0)
    {
        nearX = box->x1 - ax;
        farX = box->x2 - ax;
    }
    else
    {
        nearX = ax - box->x2;
        farX = ax - box->x1;
    }
    if (sCornerDirY[c] > 0)
    {
        nearY = box->y1 - ay;
        farY = box->y2 - ay;
    }
    else
    {
        nearY = ay - box->y2;
        farY = ay - box->y1;
    }
    if (farX < 0 || farY < 0)
        return FALSE;  /* the box is beyond the corner, outside the band */
    nearX = max(0, nearX);
    nearY = max(0, nearY);
    for (i = 0; i < corner->count; i++)
    {
        if (corner->reach[i] >= nearX && corner->depth[i] >= nearY)
            return TRUE;
    }
    return FALSE;
}

/* Choose corner patch `c`: the full-size staircase, else a smaller one,
 * else the smallest, whichever first stays within SAND_MAX_DEPTH and clear
 * of the hole and the doorway. */
static void ChooseCorner(const struct MtMoonCave *cave, s32 c, struct SandCorner *corner)
{
    u32 bits = ((u32)MtMoonGen_Random() << 16) | MtMoonGen_Random();
    struct VertexBox hole, door;
    s32 shrink;

    GetHoleBox(cave, &hole);
    GetDoorBox(cave, &door);
    for (shrink = 0; shrink < SAND_SHRINKS; shrink++)
    {
        GenerateSteps(corner, bits, shrink);
        if (corner->depth[corner->count - 1] <= SAND_MAX_DEPTH
            && !CornerMeetsBox(cave, c, corner, &hole)
            && !CornerMeetsBox(cave, c, corner, &door))
            return;
    }
    /* The smallest patch: 3 x 3 vertices at the corner (one full sand
     * tile), with the 1-vertex tips each border needs. The hole is never put
     * near it (MoveExitClearOfCornerSand), and the doorway is far away. */
    corner->count = 0;
    AddStep(corner, 3, 0);
    AddStep(corner, 2, 2);
    AddStep(corner, 0, 3);
}

/* Put corner patch `c` into the sand map, row by row away from the
 * north/south border. */
static void DrawCorner(const struct MtMoonCave *cave, struct SandMap *map, s32 c, const struct SandCorner *corner)
{
    s32 ax = CORNER_X(cave, c), ay = CORNER_Y(cave, c);
    s32 depth, reach, dx;

    for (depth = 0; (reach = CornerReachAt(corner, depth)) >= 0; depth++)
        for (dx = 0; dx <= reach; dx++)
            SetSand(map, ax + sCornerDirX[c] * dx, ay + sCornerDirY[c] * depth);
}

/* ------------------------------------------------------------------------- */
/* Laying the sand: the north strip's hanging patch and the free patch       */
/* ------------------------------------------------------------------------- */


/*
 * The patch hanging under the north strip (see THE PATCHES): 7-14 vertices
 * wide right under the strip, 2-5 rows deep, each row 1-2 vertices shorter
 * at each end than the one above, stopping before a row would be shorter
 * than 2. Dropped if it would come near the hole.
 */
static void LayHangingPatch(const struct MtMoonCave *cave, struct SandMap *map)
{
    s32 width = 7 + MtMoonGen_RandomBelow(8);
    s32 rowX1 = HANG_X_MIN + MtMoonGen_RandomBelow(HANG_X_MAX - HANG_X_MIN + 2 - width);
    s32 rowX2 = rowX1 + width - 1;
    s32 rows = 2 + MtMoonGen_RandomBelow(HANG_MAX_ROWS - 1);
    u32 narrowing = MtMoonGen_Random();  /* 2 bits per row: west end, east end */
    s32 x1[HANG_MAX_ROWS], x2[HANG_MAX_ROWS];
    s32 count, i;
    struct VertexBox box, hole;

    for (count = 0; count < rows && rowX2 - rowX1 >= 1; count++)
    {
        x1[count] = rowX1;
        x2[count] = rowX2;
        rowX1 += 1 + (narrowing & 1);
        rowX2 -= 1 + ((narrowing >> 1) & 1);
        narrowing >>= 2;
    }
    GetHoleBox(cave, &hole);
    SetBox(&box, x1[0], STRIP_ROW + 1, x2[0], STRIP_ROW + count);
    if (BoxesMeet(&box, &hole, 1))
        return;
    for (i = 0; i < count; i++)
        SetSandRow(map, STRIP_ROW + 1 + i, x1[i], x2[i]);
}

/*
 * The free patch (see THE PATCHES): an octagon, a 6-12 x 4-8 rectangle of
 * vertices with each corner cut off by 0-3 vertices, in steps of one vertex
 * per row. Each cut is limited so that the top and bottom rows and the
 * west and east columns keep at least 2 vertices. Dropped if it doesn't fit
 * inside the band with room to spare, or comes near the doorway, the hole
 * or a corner patch.
 */
static void LayFreePatch(const struct MtMoonCave *cave, struct SandMap *map, const struct VertexBox *cornerBoxes, s32 cornerCount)
{
    s32 width = 6 + MtMoonGen_RandomBelow(4) + MtMoonGen_RandomBelow(4);
    s32 height = 4 + MtMoonGen_RandomBelow(4) + MtMoonGen_RandomBelow(2);
    s32 x = FREE_PATCH_MIN + MtMoonGen_RandomBelow(cave->width - 18);
    s32 y = FREE_PATCH_MIN + MtMoonGen_RandomBelow(cave->height - 18);
    s32 cut[4];  /* north-west, north-east, south-west, south-east */
    s32 c, row;
    struct VertexBox box, hole, door;

    for (c = 0; c < 4; c++)
        cut[c] = MtMoonGen_RandomBelow(4);
    cut[0] = min(cut[0], min(height - 2, width - 2));
    cut[1] = min(cut[1], min(height - 2, width - 2 - cut[0]));
    cut[2] = min(cut[2], min(height - 2 - cut[0], width - 2));
    cut[3] = min(cut[3], min(height - 2 - cut[1], width - 2 - cut[2]));

    SetBox(&box, x, y, x + width - 1, y + height - 1);
    GetHoleBox(cave, &hole);
    GetDoorBox(cave, &door);
    if (box.x2 > cave->width - 5 || box.y2 > cave->height - 5
        || BoxesMeet(&box, &door, SAND_PATCH_GAP) || BoxesMeet(&box, &hole, 1))
        return;
    for (c = 0; c < cornerCount; c++)
    {
        if (BoxesMeet(&box, &cornerBoxes[c], SAND_PATCH_GAP))
            return;
    }
    /* Each row's ends are cut in by the cuts of the two corners on that
     * side, one vertex less for each row away from the corner. */
    for (row = 0; row < height; row++)
    {
        s32 fromBottom = height - 1 - row;
        s32 cutWest = max(0, max(cut[0] - row, cut[2] - fromBottom));
        s32 cutEast = max(0, max(cut[1] - row, cut[3] - fromBottom));

        SetSandRow(map, y + row, box.x1 + cutWest, box.x2 - cutEast);
    }
}

/*
 * Lay all of the cave's sand into the sand map (see THE PATCHES): the north
 * strip in 1 cave in 4 (if the hole is clear of it and of the smallest
 * north-east patch, vertices width-5..width-2 x 2..5), the corner patches,
 * then the strip's hanging patch, or else the free patch in 3 caves in 4.
 */
static void LaySand(const struct MtMoonCave *cave, struct SandMap *map)
{
    struct SandCorner corner;
    struct VertexBox cornerBoxes[CORNER_COUNT], hole;
    bool8 strip = (MtMoonGen_RandomBelow(4) == 0);
    s32 c, cornerCount, i;

    for (i = 0; i < SAND_MAP_WORDS; i++)
        map->bits[i] = 0;
    GetHoleBox(cave, &hole);
    if (hole.y1 <= STRIP_ROW || (hole.x2 >= cave->width - BAND - 3 && hole.y1 <= STRIP_ROW + 3))
        strip = FALSE;
    cornerCount = strip ? CORNER_COUNT : CORNER_NE;
    for (c = 0; c < cornerCount; c++)
    {
        ChooseCorner(cave, c, &corner);
        DrawCorner(cave, map, c, &corner);
        GetCornerBox(cave, c, &corner, &cornerBoxes[c]);
    }
    if (strip)
    {
        SetSandRow(map, STRIP_ROW, BAND, cave->width - BAND);
        LayHangingPatch(cave, map);
    }
    else if (MtMoonGen_RandomBelow(4) != 0)
    {
        LayFreePatch(cave, map, cornerBoxes, cornerCount);
    }
}

/* ------------------------------------------------------------------------- */
/* Shape                                                                     */
/* ------------------------------------------------------------------------- */

/* TRUE if a hole at (x, y) is clear of the smallest north-west, south-west
 * and south-east corner patches (4 x 4 vertices each), which those corners
 * always have. */
static bool8 IsHoleClearOfCornerSand(const struct MtMoonCave *cave, s32 x, s32 y)
{
    struct VertexBox hole, corner;
    s32 right = cave->width - BAND, bottom = cave->height - BAND;

    SetBox(&hole, x - 2, y - 2, x + 3, y + 3);
    SetBox(&corner, BAND, BAND, BAND + 3, BAND + 3);
    if (BoxesMeet(&hole, &corner, 0))
        return FALSE;
    SetBox(&corner, BAND, bottom - 3, BAND + 3, bottom);
    if (BoxesMeet(&hole, &corner, 0))
        return FALSE;
    SetBox(&corner, right - 3, bottom - 3, right, bottom);
    return !BoxesMeet(&hole, &corner, 0);
}

/*
 * The ladders (see the ladder contract in include/mt_moon_gen_style.h):
 * 1F's door is fixed at (18,37), in the south band. The exit hole keeps the
 * core's proposal unless that is next to a corner that always has sand (the
 * hole's neighbours must be plain floor); then it is redrawn the way the
 * core draws it, at least MT_MOON_LADDER_MIN_DISTANCE from the door. (After
 * 200 draws, which never happens, it goes to the middle of the map's top,
 * far from the door and the corners.)
 */
static void MoveExitClearOfCornerSand(struct MtMoonCave *cave)
{
    s32 tries;

    for (tries = 0; tries < 200; tries++)
    {
        if (IsHoleClearOfCornerSand(cave, cave->exit.x, cave->exit.y)
            && MtMoonGen_LadderDistance(&cave->entrance, &cave->exit) >= MT_MOON_LADDER_MIN_DISTANCE)
            return;
        cave->exit.x = 4 + MtMoonGen_RandomBelow(cave->width - 8);
        cave->exit.y = 4 + MtMoonGen_RandomBelow(cave->height - 10);
    }
    cave->exit.x = cave->width / 2;
    cave->exit.y = 6;
}

/*
 * TRUE if an island covering tiles x1..x2 x y1..y2 may go there:
 *   - against the west border (x1 == 2) if `mayTouchBorder`, else 2+ floor
 *     tiles from it; 2+ floor tiles from the north and east borders; on the
 *     south band (y2 == height - 3) if `mayTouchBorder`, else 2+ floor tiles
 *     from it (see ROCK ISLANDS);
 *   - its rock 3+ tiles from the hole, and out of the doorway's plaza;
 *   - every cell within `gap` tiles of it (diagonals too) still floor, so
 *     it is `gap` floor tiles from every other island;
 *   - no sand at the corners of the tiles around it, so it only touches
 *     plain floor.
 */
static bool8 IsIslandSpotFree(const struct MtMoonCave *cave, const struct SandMap *map,
                              s32 x1, s32 y1, s32 x2, s32 y2, s32 gap, bool8 mayTouchBorder)
{
    s32 x, y, top, bottom, left, right;
    struct VertexBox around;
    const struct MtMoonLadder *hole = &cave->exit;
    const struct MtMoonLadder *door = &cave->entrance;

    if (!(x1 >= BAND + BORDER_GAP || (x1 == BAND && mayTouchBorder))
        || x2 > cave->width - 1 - BAND - BORDER_GAP)
        return FALSE;
    if (y1 < BAND + BORDER_GAP
        || !(y2 <= cave->height - 1 - BAND - BORDER_GAP || (y2 == cave->height - 1 - BAND && mayTouchBorder)))
        return FALSE;
    if (x1 - HOLE_GAP <= hole->x && hole->x <= x2 + HOLE_GAP
        && y1 - HOLE_GAP <= hole->y && hole->y <= y2 + HOLE_GAP)
        return FALSE;
    if (x2 >= door->x - DOOR_PLAZA_HALF && x1 <= door->x + DOOR_PLAZA_HALF && y2 >= door->y - DOOR_PLAZA_DEPTH)
        return FALSE;

    top = max(BAND, y1 - gap);
    bottom = min(cave->height - 1 - BAND, y2 + gap);
    left = max(BAND, x1 - gap);
    right = min(cave->width - 1 - BAND, x2 + gap);
    for (y = top; y <= bottom; y++)
    {
        const u8 *row = MtMoonGen_GetRow(y);

        for (x = left; x <= right; x++)
        {
            if (row[x] != MT_MOON_CELL_FLOOR)
                return FALSE;
        }
    }
    /* The tiles around the island have their corners at vertices
     * x1-1..x2+2 x y1-1..y2+2. */
    SetBox(&around, x1 - 1, y1 - 1, x2 + 2, y2 + 2);
    return !IsSandInBox(map, &around);
}

/* Turn the tile box into rock; returns how many tiles that is. */
static s32 FillRock(s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 x, y;

    for (y = y1; y <= y2; y++)
        for (x = x1; x <= x2; x++)
            MtMoonGen_SetCell(x, y, MT_MOON_CELL_WALL);
    return (x2 - x1 + 1) * (y2 - y1 + 1);
}

/*
 * One try at placing an island of a random shape (see ROCK ISLANDS for the
 * mix): at a random spot or, half the time once there are islands, beside a
 * recent one on a random side, `gap` floor tiles from it, with its top or
 * left edge level with that island's. Returns how many rock tiles it added
 * (0 if it didn't fit) and remembers the island in `recent`.
 */
static s32 TryIsland(struct MtMoonCave *cave, const struct SandMap *map, struct TileBox *recent, s32 *placed)
{
    s32 kind = MtMoonGen_RandomBelow(100);
    s32 w, h, x, y, gap, rock;
    bool8 isL = FALSE;
    struct TileBox *slot;

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
        /* The L: a 3-tall arm w wide along the top and a 3-wide arm h tall
         * down its east end (see the drawing in ROCK ISLANDS). */
        w = 5 + MtMoonGen_RandomBelow(7);
        h = 5 + MtMoonGen_RandomBelow(6);
        isL = TRUE;
    }
    gap = ISLAND_GAP + (MtMoonGen_RandomBelow(4) == 0);
    if (*placed > 0 && MtMoonGen_RandomBelow(2) == 0)
    {
        const struct TileBox *next = &recent[MtMoonGen_RandomBelow(min(*placed, RECENT_ISLANDS))];

        switch (MtMoonGen_RandomBelow(4))
        {
        case 0:  x = next->x2 + gap + 1; y = next->y1;           break;
        case 1:  x = next->x1 - gap - w; y = next->y1;           break;
        case 2:  x = next->x1;           y = next->y2 + gap + 1; break;
        default: x = next->x1;           y = next->y1 - gap - h; break;
        }
    }
    else
    {
        x = BAND + MtMoonGen_RandomBelow(cave->width - 1 - BAND - BORDER_GAP - w);
        y = BAND + BORDER_GAP + MtMoonGen_RandomBelow(cave->height - 1 - BAND - BORDER_GAP - h);
    }
    if (!IsIslandSpotFree(cave, map, x, y, x + w - 1, y + h - 1, gap, !isL))
        return 0;
    if (isL)
        rock = FillRock(x, y, x + w - 1, y + 2) + FillRock(x + w - 3, y + 3, x + w - 1, y + h - 1);
    else
        rock = FillRock(x, y, x + w - 1, y + h - 1);
    slot = &recent[*placed & (RECENT_ISLANDS - 1)];
    slot->x1 = x;
    slot->y1 = y;
    slot->x2 = x + w - 1;
    slot->y2 = y + h - 1;
    (*placed)++;
    return rock;
}

/*
 * The shape: keep the door, move the hole away from the corner sand if
 * needed, lay the sand (into the style's work area, where pickTile finds
 * it), make everything inside the border band floor, add the islands (see
 * ROCK ISLANDS) and carve the ladders' footprints. The floor inside the
 * band stays one connected piece, so both footprints connect (rule 2 of the
 * ladder contract).
 */
static void Build1FShape(struct MtMoonCave *cave)
{
    struct TileBox recent[RECENT_ISLANDS];
    struct SandMap *map = GetSandMap();
    s32 placed = 0;
    s32 rock = 0;
    s32 target, tries, x, y;

    MoveExitClearOfCornerSand(cave);
    LaySand(cave, map);
    for (y = BAND; y < cave->height - BAND; y++)
        for (x = BAND; x < cave->width - BAND; x++)
            MtMoonGen_SetCell(x, y, MT_MOON_CELL_FLOOR);

    target = ROCK_TARGET_MIN + MtMoonGen_RandomBelow(ROCK_TARGET_SPREAD);
    for (tries = 0; tries < ISLAND_TRIES && rock < target; tries++)
        rock += TryIsland(cave, map, recent, &placed);

    /* Fill gaps with small islands: 3 x 3, 3 x 4 or 4 x 3. */
    for (tries = 0; tries < FILL_TRIES && rock < target; tries++)
    {
        s32 w = 3, h = 3;

        if (MtMoonGen_RandomBelow(2) == 0)
        {
            if (MtMoonGen_RandomBelow(2) == 0)
                h = 4;
            else
                w = 4;
        }
        x = BAND + MtMoonGen_RandomBelow(cave->width - 1 - BAND - BORDER_GAP - w);
        y = BAND + BORDER_GAP + MtMoonGen_RandomBelow(cave->height - 1 - BAND - BORDER_GAP - h);
        if (IsIslandSpotFree(cave, map, x, y, x + w - 1, y + h - 1, ISLAND_GAP, TRUE))
            rock += FillRock(x, y, x + w - 1, y + h - 1);
    }
    MtMoonGen_MarkLadderSite(&cave->entrance);
    MtMoonGen_MarkLadderSite(&cave->exit);
}

/* ------------------------------------------------------------------------- */
/* Tiles                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * An island's tile (see ROCK ISLANDS), from which of its 8 neighbours are
 * open. The border band counts as open, so an island standing against it
 * is still drawn with its own edge or face there, as vanilla does.
 * Neighbour patterns no island shape has (floor only to the north-east,
 * say) never occur.
 */
static u16 GetRockTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u8 open = MtMoonGen_GetFloorMask(x, y);

    if (x == BAND)
        open |= MT_MOON_NW | MT_MOON_W | MT_MOON_SW;
    if (x == cave->width - 1 - BAND)
        open |= MT_MOON_NE | MT_MOON_E | MT_MOON_SE;
    if (y == BAND)
        open |= MT_MOON_NW | MT_MOON_N | MT_MOON_NE;
    if (y == cave->height - 1 - BAND)
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

/* A tile of the border band (see BORDER BAND). The sand in front of the
 * inner ring is read from the corners of the floor tile next to it. */
static u16 GetBorderTile(const struct MtMoonCave *cave, const struct SandMap *map, s32 x, s32 y)
{
    s32 right = cave->width - 1;
    s32 bottom = cave->height - 1;
    u32 corners;

    if (y == 0)
        return (x == 0) ? TILE_BORDER_NW : (x == right) ? TILE_BORDER_NE : TILE_BORDER_OUTER_NORTH;
    if (y == bottom)
        return TILE_BORDER_OUTER_SOUTH;
    if (x == 0)
        return TILE_BORDER_OUTER_WEST;
    if (x == right)
        return TILE_BORDER_OUTER_EAST;
    if (y == 1)
    {
        if (x == 1)
            return TILE_BORDER_NW;
        if (x == right - 1)
            return TILE_BORDER_NE;
        /* The sand along the floor tile's top edge. Sand under the north
         * wall always starts at the north-west corner (see SAND), so its
         * east end is never sand on its own. */
        corners = GetSandCorners(map, x, y + 1);
        if (!(corners & SAND_NW))
            return TILE_BORDER_NORTH;
        return (corners & SAND_NE) ? TILE_BORDER_NORTH_SAND : TILE_BORDER_NORTH_SAND_END;
    }
    if (y == bottom - 1)
        return (x == 1) ? TILE_BORDER_OUTER_SOUTH : (x == right - 1) ? TILE_BORDER_SOUTH_EAST : TILE_BORDER_SOUTH;
    if (x == 1)
    {
        /* The sand along the floor tile's west edge. */
        corners = GetSandCorners(map, x + 1, y);
        if (corners & SAND_NW)
            return (corners & SAND_SW) ? TILE_BORDER_WEST_SAND : TILE_BORDER_WEST_SAND_END;
        return (corners & SAND_SW) ? TILE_BORDER_WEST_SAND_START : TILE_BORDER_WEST;
    }
    /* x == right - 1: the sand along the floor tile's east edge. */
    corners = GetSandCorners(map, x - 1, y);
    if (corners & SAND_NE)
        return (corners & SAND_SE) ? TILE_BORDER_EAST_SAND : TILE_BORDER_EAST_SAND_END;
    return (corners & SAND_SE) ? TILE_BORDER_EAST_SAND_START : TILE_BORDER_EAST;
}

/* The tile for map tile (x, y): a ladder's own tile, the border band, a
 * rock island, or floor with or without sand. */
static u16 Pick1FTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u16 tile = MtMoonGen_GetLadderTile(cave, x, y);

    if (tile != 0)
        return tile;
    if (x < BAND || y < BAND || x >= cave->width - BAND || y >= cave->height - BAND)
        return GetBorderTile(cave, GetSandMap(), x, y);
    if (!MtMoonGen_IsFloor(x, y))
        return GetRockTile(cave, x, y);
    return sSandTiles[GetSandCorners(GetSandMap(), x, y)];
}

/*
 * The core asks this before it stamps a hidden item's boulder or a sign's
 * signpost on tile (x, y) (canStampAt in include/mt_moon_gen_style.h).
 * Vanilla only puts plain floor next to those, so the four tiles around
 * (x, y) must have no sand at any corner: vertices x-1..x+2 x y-1..y+2.
 * (1F has one sign, the Zubat warning, and no hidden items.)
 */
static bool8 CanStamp1FPrefabAt(const struct MtMoonCave *cave, s32 x, s32 y)
{
    struct VertexBox around;

    SetBox(&around, x - 1, y - 1, x + 2, y + 2);
    return !IsSandInBox(GetSandMap(), &around);
}

const struct MtMoonStyle gMtMoonStyle1F =
{
    .buildShape = Build1FShape,
    .pickTile = Pick1FTile,
    .canStampAt = CanStamp1FPrefabAt,
    .workBytes = sizeof(struct SandMap),
};
