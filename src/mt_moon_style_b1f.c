/*
 * mt_moon_style_b1f.c - Mt. Moon B1F style: rooms and corridors in black void
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * The floor style for the generated Mt. Moon B1F (see
 * include/mt_moon_gen_style.h for how a style plugs into the generator core
 * in src/mt_moon_gen.c). It copies the look of vanilla B1F
 * (data/layouts/MtMoon_B1F/map.bin): rectangular rooms and corridors of cave
 * floor carved out of black void, with a 2-row rock wall along the north
 * side of every floor area and nothing at all on its west, east and south
 * sides, where the floor simply meets the void. About 16% of vanilla B1F
 * can be walked on.
 *
 * DRAWING: ROLES AND THE TILE TABLE
 *
 * Every map cell gets one ROLE, decided only by which grid cells around it
 * are floor, and each role is drawn as the one tile vanilla B1F uses for it
 * (sRoleTiles). Map cell values are metatile id | collision << 10 |
 * elevation << 12 (see MAP CELL VALUES in the style header): walkable tiles
 * are collision 0, elevation 3 (the floor's height), solid ones collision 1,
 * elevation 0.
 *
 *   role    when                                    tile    (map cell value)
 *   FLOOR   floor cell                              0x281   0x3281 walkable
 *   FACE    wall cell with floor right below it     0x299   0x0699 solid
 *           (the lower, darker row of a north wall)
 *   CAP     wall cell with FACE right below it      0x2c4   0x06c4 solid
 *           (the upper row of a north wall)
 *   VOID    any other wall cell                     0x2f2   0x06f2 solid
 *   RUBBLE  floor cell whose west neighbour is      0x283   0x0683 solid
 *           FACE or CAP (see JUNCTIONS)
 *
 * Plus the ladders, from the core (MtMoonGen_GetLadderTile): the up-ladder
 * 0x296 with its top 0x28f set into the FACE row, and the hole, which this
 * style draws as vanilla B1F's own hole tile 0x2af instead of 1F's 0x285.
 *
 * A tile in a vanilla map only ever touches certain other tiles; putting it
 * next to anything else draws a visible seam. Test P5 compares every pair of
 * neighbouring tiles in the generated B1F with the pairs vanilla B1F's map
 * uses. sEastOk and sSouthOk list the role pairs vanilla B1F has, and
 * IsDrawable() checks a finished shape against them: a shape with any other
 * pair is thrown away, never drawn.
 *
 * SHAPE RULES (what the tile table can draw)
 *
 * Read from that pair list, the drawable shapes are:
 *
 *   1. Above the top cell of every vertical run of floor sit exactly FACE
 *      and CAP, with VOID (or the map's top edge) above the CAP. So floor
 *      starts on row 2 at the earliest, and two runs in one column are at
 *      least 3 wall cells apart (floor, VOID, CAP, FACE, floor).
 *   2. Floor meets VOID freely to the west, east and south.
 *   3. Floor never touches the side of a north wall... except in one place
 *      vanilla allows it: see JUNCTIONS.
 *
 * So two floor areas side by side normally share their top row (one long
 * wall above both). Rooms at different heights connect through junctions.
 *
 * JUNCTIONS
 *
 * Where a lower room's north wall ends against floor on its EAST side,
 * vanilla B1F covers the floor cells beside the wall with rubble: the
 * corridor coming down at x = 41-44 meets the bottom room's wall (rows
 * 32-33), and (41,32) and (41,33) are rubble (with one more above, at
 * (41,31)). So the pairs CAP|RUBBLE and FACE|RUBBLE exist in vanilla.
 * The mirror image (a wall ending against floor on its WEST side) only
 * exists in vanilla behind a patch of sand with special sand-wall tiles,
 * which this style doesn't draw. So:
 *
 *   going east across connected floor, the top of the floor may rise (by 2
 *   rows or more, because a 1-row step would put a CAP beside a FACE), but
 *   never drop.
 *
 * This style puts rubble on the 2 floor cells beside the wall's 2 rows. The
 * rubble is solid. It is stamped onto the grid (MtMoonGen_StampTile), so the
 * core treats those cells as obstacles, like a boulder.
 *
 * HOW MUCH JUNCTION RUBBLE. Each link below makes at most one junction, so
 * at most 2 rubble cells: EAST and WEST links when the new floor is 2+ rows
 * higher or lower, DOWN and UP links always. A layout has up to 15 links
 * (the first room plus 2 rectangles per link, at most MAX_RECTS = 32
 * rectangles), so it could have up to 30 rubble cells. Test P10 wants
 * B1F's rubble between half and one and a half times vanilla B1F's: 7 to
 * 21 tiles. The core's decoration step counts this rubble and only ever
 * ADDS rubble, up to a target in that band (it can't remove any: the
 * junctions need theirs to draw without a seam), so the junction rubble
 * alone must stay at most 21. A layout with more is thrown away and grown
 * again, like one that grew too small (MAX_JUNCTION_RUBBLE).
 *
 * In practice there are 4 to 20. A copy of this style in Python, checked
 * against the game, measured the first shape of every one of the 65,536
 * seeds: none has more than 20, so the limit changes none of them. A shape
 * the core asks for again (see THE LADDER CONTRACT in the style header)
 * starts from other random numbers, and about 1 in 100,000 of those grows
 * a layout with 22; the limit is what keeps P10's band by construction.
 *
 * BUILDING THE SHAPE (BuildB1FShape)
 *
 * The shape is a list of rectangles (struct Layout), grown from a first room
 * on row 2 (rows 0-1 hold its wall, as in vanilla's top rooms). Each step
 * picks an existing room and adds a corridor and a new room through one of
 * four LINKS, each drawable by the rules above. In the pictures P is the
 * parent room, C the corridor, N the new room, = a north wall (CAP and FACE
 * rows) and r the junction rubble:
 *
 *   EAST: C leaves P's east side      WEST: C leaves P's west side level
 *   level with P's top. N's top is    with N's top, which is level with
 *   level with C's or 2-4 rows        P's or 2+ rows lower.
 *   higher.
 *                       ======                         =====
 *                       ======                         =====
 *     ==================rNNNNN         ================rPPPP
 *     ==================rNNNNN         ================rPPPP
 *     PPPPPCCCCCCCCCCCCCNNNNNN         NNNNNCCCCCCCCCCCPPPPP
 *     PPPPPCCCCCCCCCCCCCNNNNNN         NNNNNCCCCCCCCCCCPPPPP
 *     PPPPP             NNNNNN         NNNNN           PPPPP
 *
 *   DOWN: C goes down from P's        UP: C goes up from P's east end,
 *   bottom; N hangs west from its     flush with it, into N above, which
 *   foot, flush with its east side.   spans C.
 *
 *     =========                            =========
 *     =========                            =========
 *     PPPPPPPPP                            NNNNNNNNN
 *     PPPPPPPPP                            NNNNNNNNN
 *        CCC                                  CCC
 *        CCC                                  CCC
 *     ===rCC                               ===rCC
 *     ===rCC                               ===rCC
 *     NNNNNN                               PPPPPP
 *     NNNNNN                               PPPPPP
 *
 * In every link the higher floor is on the east side of the junction, as
 * JUNCTIONS requires. Rooms are at least 4 x 3. Corridors are 2-3 rows
 * tall (EAST, WEST) or 3-4 columns wide (DOWN, UP), so a corridor with
 * rubble at its edge is still 2 cells wide.
 *
 * A new corridor and room must keep clear of every rectangle already placed
 * except the parent (RectsTouch): one cell of void between their floors and
 * walls, so unrelated parts never draw into each other. Growth stops at a
 * random target area. Then:
 *   - IsDrawable() checks the whole shape against the tile table;
 *   - the ladders are placed (PlaceLadders): the up-ladder at the foot of a
 *     north wall, the hole in open floor, at least 24 steps apart;
 *   - HasRoomForContent() makes sure the core can place B1F's 6 hidden
 *     items without carving (see the function);
 *   - the junction rubble is counted, at most MAX_JUNCTION_RUBBLE (see HOW
 *     MUCH JUNCTION RUBBLE), and stamped.
 * A layout that fails any of these (in practice: one that stopped growing
 * too small) is thrown away and grown again; the last attempt uses a fixed
 * layout that passes every check (sFallbackLayout).
 *
 * WHY NOT CELLULAR AUTOMATA: the cellular-automata style the generator
 * started with (since removed) grew blobs with bumps, notches and 1-cell
 * strands that no vanilla tile arrangement draws, and B1F's look is built
 * from rectangles anyway.
 *
 * WHAT THE CORE DOES AFTERWARDS (see the style header): it fills pockets
 * (there are none: every rectangle joins the shape through its link, and
 * the ladders and rubble are placed so they cut nothing off), stamps the
 * hidden items' boulders (0x282) into open floor, which vanilla also has next
 * to floor on every side, and would carve repair space if some piece found no
 * spot, which HasRoomForContent rules out. Then it adds vanilla B1F's
 * decorations wherever these tiles let them fit seamlessly (DECORATIONS in
 * src/mt_moon_gen.c): more rubble, puddles under north walls, a crater
 * against the void west of a room. The junction rubble counts toward the
 * floor's rubble.
 *
 * Every random choice uses MtMoonGen_Random(), the core's seeded generator,
 * so the same seed always builds the same B1F.
 * ============================================================================
 */

#include "global.h"
#include "mt_moon_gen_style.h"

/* ------------------------------------------------------------------------- */
/* Roles and the tile table                                                  */
/* ------------------------------------------------------------------------- */

enum
{
    ROLE_VOID,
    ROLE_CAP,
    ROLE_FACE,
    ROLE_FLOOR,
    ROLE_RUBBLE,
    ROLE_COUNT
};

/* The vanilla B1F tile of each role, as full map cell values. */
static const u16 sRoleTiles[ROLE_COUNT] =
{
    [ROLE_VOID]   = 0x06f2,
    [ROLE_CAP]    = 0x06c4,
    [ROLE_FACE]   = 0x0699,
    [ROLE_FLOOR]  = 0x3281,
    [ROLE_RUBBLE] = 0x0683,
};

/* The core's hole tile (1F's, see MtMoonGen_GetLadderTile) and the one
 * vanilla B1F uses instead. Both are walkable, elevation 3. */
#define TILE_CORE_HOLE 0x3285
#define TILE_B1F_HOLE  0x32af

#define BIT(role) (1 << (role))

/*
 * The role pairs vanilla B1F's map.bin has, with the tile pair each stands
 * for. sEastOk[a] has bit b set if a cell of role b may sit directly EAST of
 * a cell of role a; sSouthOk[a] likewise for directly SOUTH. Every pair not
 * listed (FLOOR beside FACE or CAP, CAP above FLOOR, RUBBLE above VOID, ...)
 * never occurs in vanilla B1F.
 */
static const u8 sEastOk[ROLE_COUNT] =
{
    /* 2f2|2f2, 2f2|2c4, 2f2|299, 2f2|281, 2f2|283 */
    [ROLE_VOID]   = BIT(ROLE_VOID) | BIT(ROLE_CAP) | BIT(ROLE_FACE) | BIT(ROLE_FLOOR) | BIT(ROLE_RUBBLE),
    /* 2c4|2f2, 2c4|2c4, 2c4|283 */
    [ROLE_CAP]    = BIT(ROLE_VOID) | BIT(ROLE_CAP) | BIT(ROLE_RUBBLE),
    /* 299|2f2, 299|299, 299|283 */
    [ROLE_FACE]   = BIT(ROLE_VOID) | BIT(ROLE_FACE) | BIT(ROLE_RUBBLE),
    /* 281|2f2, 281|281, 281|283 */
    [ROLE_FLOOR]  = BIT(ROLE_VOID) | BIT(ROLE_FLOOR) | BIT(ROLE_RUBBLE),
    /* 283|2f2, 283|281, 283|283 */
    [ROLE_RUBBLE] = BIT(ROLE_VOID) | BIT(ROLE_FLOOR) | BIT(ROLE_RUBBLE),
};

static const u8 sSouthOk[ROLE_COUNT] =
{
    /* 2f2 above 2f2, 2c4 */
    [ROLE_VOID]   = BIT(ROLE_VOID) | BIT(ROLE_CAP),
    /* 2c4 above 299 */
    [ROLE_CAP]    = BIT(ROLE_FACE),
    /* 299 above 281, 283 */
    [ROLE_FACE]   = BIT(ROLE_FLOOR) | BIT(ROLE_RUBBLE),
    /* 281 above 2f2, 281, 283 */
    [ROLE_FLOOR]  = BIT(ROLE_VOID) | BIT(ROLE_FLOOR) | BIT(ROLE_RUBBLE),
    /* 283 above 281, 283 */
    [ROLE_RUBBLE] = BIT(ROLE_FLOOR) | BIT(ROLE_RUBBLE),
};

/* A cell is floor if it isn't wall (FLOOR, VISITED or BLOCKED: the core's
 * boulders and this style's rubble count as floor, see the style header). */
#define FLOOR_AT(row, x) ((row)[x] != MT_MOON_CELL_WALL)

/*
 * A cell's role depends on the cell, the two cells below it and the cell
 * west of it, so the code reads three grid rows at a time: row y and the
 * two rows below it, as pointers from MtMoonGen_GetRow (row[x] is cell
 * (x, y); x may be -1 or width, the grid's frame of wall cells).
 */
struct RowWindow
{
    const u8 *row;
    const u8 *below;
    const u8 *below2;
};

/* Point `rows` at row y (0 to height - 1) and the two rows below it. Below
 * the grid's last row comes its frame row, all wall, and MtMoonGen_GetRow
 * may read no further, so both lower rows stop there. */
static void ReadRows(const struct MtMoonCave *cave, s32 y, struct RowWindow *rows)
{
    rows->row = MtMoonGen_GetRow(y);
    rows->below = MtMoonGen_GetRow(min(y + 1, cave->height));
    rows->below2 = MtMoonGen_GetRow(min(y + 2, cave->height));
}

/* TRUE if cell x of the window's row is part of a north wall (FACE or
 * CAP): a wall cell with floor 1 or 2 cells below it. */
#define IS_NORTH_WALL(rows, x) \
    (!FLOOR_AT((rows)->row, x) && (FLOOR_AT((rows)->below, x) || FLOOR_AT((rows)->below2, x)))

/* The role of cell x of the window's row. */
static u8 RoleAt(const struct RowWindow *rows, s32 x)
{
    if (FLOOR_AT(rows->row, x))
        return IS_NORTH_WALL(rows, x - 1) ? ROLE_RUBBLE : ROLE_FLOOR;
    if (FLOOR_AT(rows->below, x))
        return ROLE_FACE;
    if (FLOOR_AT(rows->below2, x))
        return ROLE_CAP;
    return ROLE_VOID;
}

static u8 GetRole(const struct MtMoonCave *cave, s32 x, s32 y)
{
    struct RowWindow rows;

    ReadRows(cave, y, &rows);
    return RoleAt(&rows, x);
}

/* TRUE for a floor cell that is drawn as plain floor (not rubble). */
static bool8 IsPlainFloor(const struct MtMoonCave *cave, s32 x, s32 y)
{
    return GetRole(cave, x, y) == ROLE_FLOOR;
}

/* ------------------------------------------------------------------------- */
/* Shape bounds                                                              */
/* ------------------------------------------------------------------------- */

/* Floor stays inside a border of 2 cells on the west, east and top sides
 * and 3 at the bottom, the border the core and the other styles keep too.
 * The top border is exactly the 2-row wall above a room on row 2. */
#define MIN_X 2
#define MIN_Y 2
#define MAX_X(cave) ((cave)->width - 3)
#define MAX_Y(cave) ((cave)->height - 4)

/* The widest map IsDrawable() can check: it keeps the roles of two rows,
 * one byte per column, on the stack. */
#define MAX_WIDTH 64

/*
 * TRUE if every pair of neighbouring cells in the grid has a role pair
 * vanilla B1F has (sEastOk, sSouthOk), and no floor lies outside the bounds.
 *
 * It works a row at a time, keeping the roles of the row above in `above`
 * to check the south pairs, so each cell's role is worked out once.
 */
static bool8 IsDrawable(const struct MtMoonCave *cave)
{
    u8 rolesA[MAX_WIDTH], rolesB[MAX_WIDTH];
    u8 *above = rolesA, *roles = rolesB, *swap;
    s32 x, y;

    if (cave->width > MAX_WIDTH)
        return FALSE;
    for (y = 0; y < cave->height; y++)
    {
        struct RowWindow rows;
        bool8 inRows = (y >= MIN_Y && y <= MAX_Y(cave));

        ReadRows(cave, y, &rows);
        for (x = 0; x < cave->width; x++)
        {
            u8 role = RoleAt(&rows, x);

            roles[x] = role;
            if ((role == ROLE_FLOOR || role == ROLE_RUBBLE)
                && (!inRows || x < MIN_X || x > MAX_X(cave)))
                return FALSE;
            if (x > 0 && !(sEastOk[roles[x - 1]] & BIT(role)))
                return FALSE;
            if (y > 0 && !(sSouthOk[above[x]] & BIT(role)))
                return FALSE;
        }
        swap = above;
        above = roles;
        roles = swap;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Layout: the shape as a list of rectangles                                 */
/* ------------------------------------------------------------------------- */

/* A rectangle of floor cells, corners included. Signed: a link computes
 * candidate rectangles that may stick out of the map before the bounds
 * check rejects them. */
struct Rect
{
    s8 x1;
    s8 y1;
    s8 x2;
    s8 y2;
};

/* The most rectangles a layout holds. Growth stops early when it's full;
 * the prototype of this style never needed more than 27. */
#define MAX_RECTS 32

/*
 * The shape being grown: every rectangle (rooms and corridors), and which of
 * them are rooms (only rooms get new links). About 170 bytes, kept on the
 * stack: the module may not add static variables (EWRAM is full, and new
 * IWRAM variables would move the addresses tests rely on; see the IWRAM
 * note in src/mt_moon_gen.c).
 */
struct Layout
{
    struct Rect rects[MAX_RECTS];
    u8 rooms[MAX_RECTS];   /* indexes into rects[] */
    u8 rectCount;
    u8 roomCount;
    s16 floorCount;        /* floor cells in all rects (they never overlap) */
};

/*
 * Growth: how many link attempts per layout, the floor area it aims for
 * (a random target in [GROW_TARGET, GROW_TARGET + GROW_TARGET_SPAN)), and
 * the least and most floor a layout may have. They keep the walkable share
 * of B1F's 49 x 40 = 1960 cells inside test P6's 15-30% (294-588 cells):
 *   - at least 340 floor cells, of which at most 36 end up solid: 6
 *     boulders and the rubble, 2 cells per link at most, with at most 15
 *     links (MAX_RECTS). So at least 304 are walkable;
 *   - at most 580 floor cells (a link that would pass that isn't added),
 *     and ladders count as walkable, so at most 580 are.
 * The prototype of this style averaged about 20% walkable (vanilla: 16%).
 */
#define GROW_TRIES        200
#define GROW_TARGET       340
#define GROW_TARGET_SPAN  150
#define MIN_FLOOR_CELLS   340
#define MAX_FLOOR_CELLS   580

/* How often a link starts from the newest room rather than a random one
 * (percent): about half, so the cave strings out into chains of rooms
 * instead of bunching around the first one. */
#define NEWEST_PARENT_PERCENT 50

/* The four links (see BUILDING THE SHAPE) and how often each is tried
 * (percent). */
enum
{
    LINK_EAST,
    LINK_WEST,
    LINK_DOWN,
    LINK_UP,
    LINK_COUNT
};

static const u8 sLinkPercent[LINK_COUNT] = { 20, 20, 35, 25 };

/* How many whole layouts to try; the last one is sFallbackLayout. */
#define MAX_LAYOUTS 8

/* A random number in [lo, hi]. */
static s32 RandomBetween(s32 lo, s32 hi)
{
    return lo + MtMoonGen_RandomBelow(hi - lo + 1);
}

static s32 RectArea(const struct Rect *r)
{
    return (r->x2 - r->x1 + 1) * (r->y2 - r->y1 + 1);
}

static bool8 IsRectInBounds(const struct MtMoonCave *cave, const struct Rect *r)
{
    return r->x1 >= MIN_X && r->x1 <= r->x2 && r->x2 <= MAX_X(cave)
        && r->y1 >= MIN_Y && r->y1 <= r->y2 && r->y2 <= MAX_Y(cave);
}

/*
 * TRUE if two rectangles would draw into each other. Each covers its floor
 * plus the 2 wall rows above it; they touch when those areas, one of them
 * grown by a cell on every side, overlap. (The test is symmetric.) Two
 * rectangles that don't touch always have void between them.
 */
static bool8 RectsTouch(const struct Rect *a, const struct Rect *b)
{
    return a->x1 - 1 <= b->x2 && b->x1 <= a->x2 + 1
        && a->y1 - 3 <= b->y2 && b->y1 - 2 <= a->y2 + 1;
}

/* TRUE if `r` is inside the bounds and touches no rectangle of the layout
 * except number `parent` (-1 for none), which the link joins on purpose. */
static bool8 FitsLayout(const struct MtMoonCave *cave, const struct Layout *layout, const struct Rect *r, s32 parent)
{
    s32 i;

    if (!IsRectInBounds(cave, r))
        return FALSE;
    for (i = 0; i < layout->rectCount; i++)
    {
        if (i != parent && RectsTouch(&layout->rects[i], r))
            return FALSE;
    }
    return TRUE;
}

static void SetRect(struct Rect *r, s32 x1, s32 y1, s32 x2, s32 y2)
{
    r->x1 = x1;
    r->y1 = y1;
    r->x2 = x2;
    r->y2 = y2;
}

/*
 * Work out the corridor and the new room for a link of `kind` from room P
 * (see BUILDING THE SHAPE for the pictures). Returns FALSE if P is too small
 * for that kind of link. The rectangles may still be out of bounds or hit
 * other parts of the layout; the caller checks that.
 */
static bool8 MakeLink(u8 kind, const struct Rect *p, struct Rect *corridor, struct Rect *room)
{
    s32 pw = p->x2 - p->x1 + 1;
    s32 ph = p->y2 - p->y1 + 1;
    s32 height, length, rise, drop, width, depth, roomW, roomH, c1, c2, x1;

    switch (kind)
    {
    case LINK_EAST:
        /* A corridor 2-3 rows tall, level with P's top, then a room whose
         * top is level with it (rise 0) or 2-4 rows higher. A 1-row rise
         * can't be drawn (CAP beside FACE), so draws of 0 and 1 mean 0. */
        height = RandomBetween(2, 3);
        if (height > ph)
            return FALSE;
        length = RandomBetween(2, 7);
        rise = MtMoonGen_RandomBelow(5);
        if (rise < 2)
            rise = 0;
        roomW = RandomBetween(4, 11);
        roomH = RandomBetween(max(3, rise + height), rise + height + 3);
        SetRect(corridor, p->x2 + 1, p->y1, p->x2 + length, p->y1 + height - 1);
        SetRect(room, p->x2 + length + 1, p->y1 - rise, p->x2 + length + roomW, p->y1 - rise + roomH - 1);
        return TRUE;
    case LINK_WEST:
        /* A room whose top is level with P's (drop 0) or 2+ rows lower,
         * and a corridor 2-3 rows tall level with the room's top that
         * meets P within P's rows. */
        height = RandomBetween(2, 3);
        if (height > ph)
            return FALSE;
        drop = MtMoonGen_RandomBelow(ph - height + 1);
        if (drop == 1)
            drop = 0;
        length = RandomBetween(2, 7);
        roomW = RandomBetween(4, 11);
        roomH = RandomBetween(max(3, height), height + 3);
        SetRect(corridor, p->x1 - length, p->y1 + drop, p->x1 - 1, p->y1 + drop + height - 1);
        SetRect(room, p->x1 - length - roomW, p->y1 + drop, p->x1 - length - 1, p->y1 + drop + roomH - 1);
        return TRUE;
    case LINK_DOWN:
        /* A corridor 3-4 columns wide anywhere under P, then a room
         * hanging west from the corridor's foot, flush with its east side.
         * The room's top is 4-9 rows below P's bottom (so the corridor is
         * 3-8 rows long): at least 4, so a row of void separates the
         * room's wall from P. */
        width = RandomBetween(3, 4);
        if (width > pw)
            return FALSE;
        c2 = p->x1 + width - 1 + MtMoonGen_RandomBelow(pw - width + 1);
        c1 = c2 - width + 1;
        depth = RandomBetween(4, 9);
        roomW = RandomBetween(width + 2, width + 10);
        roomH = RandomBetween(3, 5);
        SetRect(room, c2 - roomW + 1, p->y2 + depth, c2, p->y2 + depth + roomH - 1);
        SetRect(corridor, c1, p->y2 + 1, c2, p->y2 + depth - 1);
        return TRUE;
    case LINK_UP:
        /* A corridor 3-4 columns wide at P's east end, going up into a
         * room above that spans it. The room's bottom is 4-9 rows above
         * P's top (so the corridor is 3-8 rows long). P keeps at least 2
         * columns west of the corridor. */
        width = RandomBetween(3, 4);
        if (width > pw - 2)
            return FALSE;
        c1 = p->x2 - width + 1;
        depth = RandomBetween(4, 9);
        roomH = RandomBetween(3, 5);
        roomW = RandomBetween(width + 1, width + 10);
        x1 = c1 - MtMoonGen_RandomBelow(roomW - width + 1);
        SetRect(room, x1, p->y1 - depth - roomH + 1, x1 + roomW - 1, p->y1 - depth);
        SetRect(corridor, c1, p->y1 - depth + 1, p->x2, p->y1 - 1);
        return TRUE;
    }
    return FALSE;
}

/* Add a rectangle; `isRoom` makes it a parent for later links. */
static void AddRect(struct Layout *layout, const struct Rect *r, bool8 isRoom)
{
    if (isRoom)
        layout->rooms[layout->roomCount++] = layout->rectCount;
    layout->rects[layout->rectCount++] = *r;
    layout->floorCount += RectArea(r);
}

/* Try one link from a random room. Returns TRUE if it was added. */
static bool8 TryLink(const struct MtMoonCave *cave, struct Layout *layout)
{
    struct Rect corridor, room;
    s32 parent, roll, i;
    u8 kind = 0;

    if (layout->rectCount + 2 > MAX_RECTS)
        return FALSE;
    if (MtMoonGen_RandomBelow(100) < NEWEST_PARENT_PERCENT)
        parent = layout->rooms[layout->roomCount - 1];
    else
        parent = layout->rooms[MtMoonGen_RandomBelow(layout->roomCount)];
    roll = MtMoonGen_RandomBelow(100);
    for (i = 0; i < LINK_COUNT; i++)
    {
        if (roll < sLinkPercent[i])
        {
            kind = i;
            break;
        }
        roll -= sLinkPercent[i];
    }
    if (!MakeLink(kind, &layout->rects[parent], &corridor, &room))
        return FALSE;
    /* The corridor touches the parent (and the room) on purpose; the link's
     * geometry makes those contacts drawable. The room keeps clear of
     * everything, the parent included. */
    if (!FitsLayout(cave, layout, &corridor, parent) || !FitsLayout(cave, layout, &room, -1)
        || layout->floorCount + RectArea(&corridor) + RectArea(&room) > MAX_FLOOR_CELLS)
        return FALSE;
    AddRect(layout, &corridor, FALSE);
    AddRect(layout, &room, TRUE);
    return TRUE;
}

static void ClearLayout(struct Layout *layout)
{
    layout->rectCount = 0;
    layout->roomCount = 0;
    layout->floorCount = 0;
}

/* Grow a random layout from a first room on the top row. */
static void GrowLayout(const struct MtMoonCave *cave, struct Layout *layout)
{
    struct Rect first;
    s32 width = RandomBetween(5, 11);
    s32 height = RandomBetween(3, 5);
    s32 x = MIN_X + MtMoonGen_RandomBelow(MAX_X(cave) - MIN_X - width + 2);
    s32 target = GROW_TARGET + MtMoonGen_RandomBelow(GROW_TARGET_SPAN);
    s32 tries;

    ClearLayout(layout);
    SetRect(&first, x, MIN_Y, x + width - 1, MIN_Y + height - 1);
    AddRect(layout, &first, TRUE);
    for (tries = 0; tries < GROW_TRIES && layout->floorCount < target; tries++)
        TryLink(cave, layout);
}

/*
 * The last attempt's layout, fixed: one the growth above produced, chosen
 * because it passes every check below for any choice of ladders (54 spots
 * for the up-ladder, each with a hole spot 24+ steps away; room for 60
 * hidden items; 491 floor cells, about 24% walkable; 14 junction rubble
 * tiles, inside P10's 7-21, and the ladders never change that count). It
 * only exists so that B1F is always generated; on the 100 seeds of the
 * property test the random layouts never needed more than a few attempts.
 */
static const struct Rect sFallbackLayout[] =
{
    { 37,  2, 41,  6 }, { 30,  4, 36,  5 }, { 21,  4, 29,  6 }, { 21,  7, 24, 10 },
    { 11, 11, 24, 15 }, { 39,  7, 41,  9 }, { 29, 10, 41, 13 }, { 13, 16, 16, 23 },
    {  6, 24, 16, 26 }, { 13, 27, 15, 30 }, {  5, 31, 15, 35 }, { 31, 14, 34, 21 },
    { 24, 22, 34, 25 }, { 30, 26, 32, 30 }, { 22, 31, 32, 35 },
};

static void UseFallbackLayout(struct Layout *layout)
{
    s32 i;

    ClearLayout(layout);
    for (i = 0; i < ARRAY_COUNT(sFallbackLayout); i++)
        AddRect(layout, &sFallbackLayout[i], FALSE);
}

/* Write every rectangle of the layout into the grid as `cell`. */
static void CarveLayout(const struct Layout *layout, u8 cell)
{
    s32 i, x, y;

    for (i = 0; i < layout->rectCount; i++)
    {
        const struct Rect *r = &layout->rects[i];

        for (y = r->y1; y <= r->y2; y++)
            for (x = r->x1; x <= r->x2; x++)
                MtMoonGen_SetCell(x, y, cell);
    }
}

/* ------------------------------------------------------------------------- */
/* Ladders                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * TRUE if an up-ladder can stand on (x, y): at the foot of a north wall
 * (the cell above is wall; the core's footprint check sees to the rest),
 * with plain floor west, east, south, south-west and south-east of it.
 * Vanilla B1F only puts floor beside and below the ladder (0x296) and only
 * FACE beside its top (0x28f), so the cells west and east must be floor
 * with their wall at the same height. The 3 x 2 block of floor also means
 * the ladder cuts nothing off: the player can't walk through a ladder, and
 * its neighbours still meet below it.
 */
static bool8 IsUpLadderSpot(const struct MtMoonCave *cave, s32 x, s32 y)
{
    const u8 *above = MtMoonGen_GetRow(y - 1);
    const u8 *row = MtMoonGen_GetRow(y);
    const u8 *below = MtMoonGen_GetRow(y + 1);

    /* Cheap tests first: most cells are not the top of a run. */
    if (!FLOOR_AT(row, x) || FLOOR_AT(above, x)
        || !FLOOR_AT(row, x - 1) || FLOOR_AT(above, x - 1)
        || !FLOOR_AT(row, x + 1) || FLOOR_AT(above, x + 1)
        || !FLOOR_AT(below, x - 1) || !FLOOR_AT(below, x) || !FLOOR_AT(below, x + 1))
        return FALSE;
    /* All six cells are floor, so only the west column can be rubble (a
     * cell is rubble when its west neighbour is wall). */
    return IsPlainFloor(cave, x - 1, y) && IsPlainFloor(cave, x - 1, y + 1);
}

/*
 * TRUE if (x, y) and all 8 cells around it are plain floor: a spot for the
 * hole (vanilla's 0x2af only touches plain floor 0x281, and the core wants
 * a hole in open floor), and for a hidden item's boulder (the core stamps
 * those into open floor only).
 */
static bool8 IsOpenSpot(const struct MtMoonCave *cave, s32 x, s32 y)
{
    if (!MtMoonGen_IsFloor(x, y) || MtMoonGen_GetFloorMask(x, y) != 0xFF)
        return FALSE;
    /* As above, only the west column of the 3 x 3 can be rubble. */
    return IsPlainFloor(cave, x - 1, y - 1) && IsPlainFloor(cave, x - 1, y) && IsPlainFloor(cave, x - 1, y + 1);
}

static bool8 IsLadderSpot(const struct MtMoonCave *cave, const struct MtMoonLadder *ladder, s32 x, s32 y)
{
    if (ladder->kind == MT_MOON_LADDER_UP)
        return IsUpLadderSpot(cave, x, y);
    if (ladder->kind == MT_MOON_LADDER_DOWN)
        return IsOpenSpot(cave, x, y);
    return FALSE; /* B1F has no door */
}

/*
 * Move `ladder` to a random spot that suits its kind, at least
 * MT_MOON_LADDER_MIN_DISTANCE from `other` if `other` isn't NULL. Every spot
 * gets an equal chance ("reservoir sampling": the k-th suitable spot found
 * replaces the choice so far with probability 1/k), in one pass over the
 * layout's floor. Returns FALSE if there is no such spot.
 *
 * LOAD TIME: the passes over the shape (here, HasRoomForContent,
 * CountJunctionRubble and StampRubble) only visit the layout's rectangles,
 * about a fifth of the map, instead of every map cell. The rectangles never overlap, so each
 * floor cell is visited once. An up-ladder's cell has wall above it, so it
 * is on the top row of its rectangle (any lower row has floor above it, in
 * the same rectangle): only top rows are visited for one.
 */
static bool8 PlaceLadder(const struct MtMoonCave *cave, const struct Layout *layout,
                         struct MtMoonLadder *ladder, const struct MtMoonLadder *other)
{
    struct MtMoonLadder spot = *ladder;
    s32 found = 0;
    s32 i, x, y, lastRow;

    for (i = 0; i < layout->rectCount; i++)
    {
        const struct Rect *r = &layout->rects[i];

        lastRow = (ladder->kind == MT_MOON_LADDER_UP) ? r->y1 : r->y2;
        for (y = r->y1; y <= lastRow; y++)
        {
            for (x = r->x1; x <= r->x2; x++)
            {
                if (!IsLadderSpot(cave, ladder, x, y))
                    continue;
                spot.x = x;
                spot.y = y;
                if (other != NULL && MtMoonGen_LadderDistance(&spot, other) < MT_MOON_LADDER_MIN_DISTANCE)
                    continue;
                found++;
                if (MtMoonGen_RandomBelow(found) == 0)
                {
                    ladder->x = x;
                    ladder->y = y;
                }
            }
        }
    }
    return found > 0;
}

/* How many entrance spots PlaceLadders tries before giving up on a shape. */
#define ENTRANCE_TRIES 4

/*
 * Put both ladders on the shape, replacing the core's proposals (which
 * assume nothing about the shape): the entrance first, then the exit far
 * enough from it. If no exit spot is far enough, try another entrance.
 * B1F's two ladders are never fixed (see sFloors in the core).
 */
static bool8 PlaceLadders(struct MtMoonCave *cave, const struct Layout *layout)
{
    s32 i;

    for (i = 0; i < ENTRANCE_TRIES; i++)
    {
        if (!PlaceLadder(cave, layout, &cave->entrance, NULL))
            return FALSE;
        if (PlaceLadder(cave, layout, &cave->exit, &cave->entrance))
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Room for the content                                                      */
/* ------------------------------------------------------------------------- */

/*
 * B1F's content is 6 hidden items and nothing else (data/maps/MtMoon_B1F).
 * The core stamps each one as a boulder on a spot that is open floor all
 * around (IsOpenSpot), at least 3 steps (counting diagonal steps) from both
 * ladders and at least 2 from every other boulder, picking spots at random.
 * If it found no spot it would carve new floor, which this style's tiles
 * can't draw. So the shape must have room for 6 boulders however the first
 * 5 were placed.
 *
 * A set of spots pairwise 2+ steps apart can have at most 4 of them within
 * 1 step of any one boulder (the corners of the 3 x 3 around it). So with
 * 4 x 5 + 1 = 21 such spots, 5 boulders anywhere still leave one free for
 * the 6th. HasRoomForContent counts such a set greedily.
 */
#define B1F_CONTENT_PIECES 6
#define MIN_OPEN_SPOTS (4 * (B1F_CONTENT_PIECES - 1) + 1)

/* Steps between two cells when diagonal steps count as one. */
static s32 KingDistance(s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 dx = abs(x1 - x2);
    s32 dy = abs(y1 - y2);

    return max(dx, dy);
}

static bool8 HasRoomForContent(const struct MtMoonCave *cave, const struct Layout *layout)
{
    struct { u8 x, y; } spots[MIN_OPEN_SPOTS];
    s32 count = 0;
    s32 x, y, i, j;

    for (j = 0; j < layout->rectCount; j++)
    {
        const struct Rect *r = &layout->rects[j];

        for (y = r->y1; y <= r->y2; y++)
        {
            for (x = r->x1; x <= r->x2; x++)
            {
                if (KingDistance(x, y, cave->entrance.x, cave->entrance.y) < 3
                    || KingDistance(x, y, cave->exit.x, cave->exit.y) < 3
                    || !IsOpenSpot(cave, x, y))
                    continue;
                for (i = 0; i < count; i++)
                {
                    if (KingDistance(x, y, spots[i].x, spots[i].y) < 2)
                        break;
                }
                if (i < count)
                    continue;
                spots[count].x = x;
                spots[count].y = y;
                if (++count == MIN_OPEN_SPOTS)
                    return TRUE;
            }
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Putting it together                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The most junction rubble a layout may have (see HOW MUCH JUNCTION
 * RUBBLE): the top of test P10's band for B1F's rubble. Vanilla B1F has 14
 * rubble tiles (metatile 0x283 in data/layouts/MtMoon_B1F/map.bin), and P10
 * wants between half and one and a half times as many on a generated B1F,
 * rounded inward to whole tiles: ceil(14 / 2) = 7 to floor(14 x 3 / 2) =
 * 21, the same band the core's decoration step aims for (DECORATIONS, HOW
 * MANY, in src/mt_moon_gen.c). The division is done by the compiler, so it
 * costs nothing on the GBA.
 */
#define VANILLA_B1F_RUBBLE  14
#define MAX_JUNCTION_RUBBLE (VANILLA_B1F_RUBBLE * 3 / 2)

/* How many cells of the layout are junction rubble (see JUNCTIONS). Rubble
 * is floor with wall to its west, so it is always in the west column of its
 * rectangle: only those columns are looked at. */
static s32 CountJunctionRubble(const struct MtMoonCave *cave, const struct Layout *layout)
{
    s32 i, y;
    s32 count = 0;

    for (i = 0; i < layout->rectCount; i++)
    {
        const struct Rect *r = &layout->rects[i];

        for (y = r->y1; y <= r->y2; y++)
        {
            if (GetRole(cave, r->x1, y) == ROLE_RUBBLE)
                count++;
        }
    }
    return count;
}

/* Stamp every junction's rubble (see JUNCTIONS) onto the grid, from the
 * same west columns as CountJunctionRubble. */
static void StampRubble(const struct MtMoonCave *cave, const struct Layout *layout)
{
    s32 i, y;

    for (i = 0; i < layout->rectCount; i++)
    {
        const struct Rect *r = &layout->rects[i];

        for (y = r->y1; y <= r->y2; y++)
        {
            if (GetRole(cave, r->x1, y) == ROLE_RUBBLE)
                MtMoonGen_StampTile(r->x1, y, sRoleTiles[ROLE_RUBBLE]);
        }
    }
}

static void BuildB1FShape(struct MtMoonCave *cave)
{
    struct Layout layout;
    s32 attempt;

    for (attempt = 0; attempt < MAX_LAYOUTS; attempt++)
    {
        if (attempt < MAX_LAYOUTS - 1)
        {
            GrowLayout(cave, &layout);
            if (layout.floorCount < MIN_FLOOR_CELLS)
                continue; /* nothing carved yet */
        }
        else
        {
            UseFallbackLayout(&layout);
        }
        CarveLayout(&layout, MT_MOON_CELL_FLOOR);
        /* The rubble count comes last, so it can only turn away a layout
         * that passed everything else: no other layout, and no random
         * number drawn for one, changes. */
        if (IsDrawable(cave) && PlaceLadders(cave, &layout) && HasRoomForContent(cave, &layout)
            && CountJunctionRubble(cave, &layout) <= MAX_JUNCTION_RUBBLE)
        {
            StampRubble(cave, &layout);
            return;
        }
        CarveLayout(&layout, MT_MOON_CELL_WALL); /* back to all wall */
    }
    /* Never reached: the fallback layout passes. If it ever didn't, the
     * grid is all wall, the core's ladder check fails, and the core asks
     * for a new shape (and in the end keeps the vanilla map). */
}

static u16 PickB1FTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u16 tile = MtMoonGen_GetLadderTile(cave, x, y);

    if (tile == TILE_CORE_HOLE)
        return TILE_B1F_HOLE;
    if (tile != 0)
        return tile;
    return sRoleTiles[GetRole(cave, x, y)];
}

const struct MtMoonStyle gMtMoonStyleB1F = { BuildB1FShape, PickB1FTile };
