/*
 * mt_moon_style_cellular.c - Cellular-automata cave style for Mt. Moon
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * A floor style for the Mt. Moon generator (see include/mt_moon_gen_style.h
 * for how styles plug into the core in src/mt_moon_gen.c). This is the
 * original procedural-cave look, and for now every generated floor uses it.
 *
 * SHAPE (BuildCellularShape):
 *   1. Keep the ladders where the core proposed them (see the ladder
 *      contract in include/mt_moon_gen_style.h). An up-ladder is proposed
 *      on row 2, against the solid top border, so the north wall it must
 *      stand at the foot of is the border, which nothing here ever carves.
 *   2. Fill the grid with random walls and floors (about 30% floor).
 *   3. Smooth it four times with a cellular-automata rule: a cell becomes
 *      wall if 5 or more of its 8 neighbours are wall, otherwise floor. Blobs
 *      of noise grow into cave-like rooms.
 *   4. Keep a solid border (2 cells on the sides and top, 3 at the bottom)
 *      and open floor around each ladder, with its footprint (see
 *      include/mt_moon_gen_style.h) stamped in.
 *   5. Clean up shapes the tiles draw badly: lone floor cells, 1-cell-wide
 *      wall strands, diagonal wall pinches, bumps and notches.
 *   6. If the entrance can't reach the exit, carve a winding corridor
 *      between the floor cells in front of them.
 *
 * TILES (PickCellularTile): ladder cells get the core's vanilla ladder
 * tiles. Other floor cells get the cave floor tile, with a position-based
 * ~8% sprinkling of sand. Wall cells get a tile chosen from which of their
 * four neighbours (N, S, W, E) are floor; see GetWallTile.
 *
 * Every random choice uses MtMoonGen_Random(), the core's seeded generator,
 * so the same seed always produces the same cave.
 *
 * MAP CELL VALUES: a value written into the map is
 *     metatile id | (collision << 10) | (elevation << 12)
 * so 0x3281 is metatile 0x281 (cave floor) with collision 0 (walkable) and
 * elevation 3, and 0x0691 is metatile 0x291 with collision 1 (solid).
 * ============================================================================
 */

#include "global.h"
#include "mt_moon_gen_style.h"

/* Walkable floor: plain cave floor, and the yellow sand used as accents. */
#define TILE_FLOOR  0x3281
#define TILE_SAND   0x32fc

/*
 * Wall tiles, all solid (collision 1, elevation 0). GetWallTile emits:
 *
 *   floor neighbours   tile     floor neighbours   tile
 *   E only             0x0692   S + E              0x069a
 *   W only             0x0690   S + W              0x0698
 *   E + W              0x0682   S + E + W          0x0699
 *   S only             0x0699   N + E + W          0x069f
 *   N + S              0x0682   any other with N   0x06c6
 *   none, top row      0x06c4   none, elsewhere    0x06c6
 */
#define TILE_WALL_FLOOR_E       0x0692
#define TILE_WALL_FLOOR_W       0x0690
#define TILE_WALL_THIN          0x0682
#define TILE_WALL_FLOOR_S       0x0699
#define TILE_WALL_FLOOR_SE      0x069a
#define TILE_WALL_FLOOR_SW      0x0698
#define TILE_WALL_FLOOR_N_E_W   0x069f
#define TILE_WALL_SOLID         0x06c6
#define TILE_WALL_SOLID_TOP_ROW 0x06c4

/* Share of cells that start as floor before smoothing, in percent. */
#define INITIAL_FLOOR_PERCENT 30

/* Short names for the core's grid functions, to keep the dense cell loops
 * below readable. */
#define IS_FLOOR(x, y) MtMoonGen_IsFloor((x), (y))
#define SET_CELL(x, y, v) MtMoonGen_SetCell((x), (y), (v))
#define FLOOR_MASK(x, y) MtMoonGen_GetFloorMask((x), (y))

/*
 * The passes over the whole grid read cells straight from grid rows (see
 * MtMoonGen_GetRow): `row` is the cell's row, `above` and `below` the rows
 * north and south of it. Each pass touches every cell, several times over,
 * while the screen is black between two warps; calling a function for each
 * neighbour made that pause visibly long (test P9).
 */
#define FLOOR_AT(r, x) ((r)[x] != MT_MOON_CELL_WALL)

/* How many of the 8 neighbours of cell x are wall. */
#define WALLS_AROUND(above, row, below, x)                                      \
    (!FLOOR_AT(above, (x) - 1) + !FLOOR_AT(above, (x)) + !FLOOR_AT(above, (x) + 1) \
   + !FLOOR_AT(row, (x) - 1)                          + !FLOOR_AT(row, (x) + 1)   \
   + !FLOOR_AT(below, (x) - 1) + !FLOOR_AT(below, (x)) + !FLOOR_AT(below, (x) + 1))

/* How many of the 4 neighbours N, S, W and E of cell x are wall. */
#define CARDINAL_WALLS(above, row, below, x) \
    (!FLOOR_AT(above, (x)) + !FLOOR_AT(below, (x)) + !FLOOR_AT(row, (x) - 1) + !FLOOR_AT(row, (x) + 1))

/* Make every cell in the rectangle floor, staying clear of the border. */
static void ForceFloorRect(const struct MtMoonCave *cave, s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 x, y;
    for (y = y1; y <= y2; y++)
        for (x = x1; x <= x2; x++)
            if (x >= 2 && x < cave->width - 2 && y >= 2 && y < cave->height - 3)
                SET_CELL(x, y, MT_MOON_CELL_FLOOR);
}

/* Solid walls: 2 cells thick on the top, left and right, 3 at the bottom. */
static void EnforceBorders(const struct MtMoonCave *cave)
{
    s32 x, y;
    s32 w = cave->width;
    s32 h = cave->height;

    for (x = 0; x < w; x++)
    {
        SET_CELL(x, 0, MT_MOON_CELL_WALL);
        SET_CELL(x, 1, MT_MOON_CELL_WALL);
        SET_CELL(x, h - 1, MT_MOON_CELL_WALL);
        SET_CELL(x, h - 2, MT_MOON_CELL_WALL);
        SET_CELL(x, h - 3, MT_MOON_CELL_WALL);
    }
    for (y = 0; y < h; y++)
    {
        SET_CELL(0, y, MT_MOON_CELL_WALL);
        SET_CELL(1, y, MT_MOON_CELL_WALL);
        SET_CELL(w - 1, y, MT_MOON_CELL_WALL);
        SET_CELL(w - 2, y, MT_MOON_CELL_WALL);
    }
}

/*
 * Open floor around one ladder, then its footprint (which also writes the
 * cells ForceFloorRect can't reach, such as 1F's door in the bottom border):
 *   door  a 3-wide corridor running 5 cells north from the doorway;
 *   down  a 5 x 5 patch of floor around the hole;
 *   up    a 3-wide patch of floor 4 cells deep below the ladder. The wall
 *         above is the top border (up-ladders are proposed on row 2).
 */
static void ReinforceLadder(const struct MtMoonCave *cave, const struct MtMoonLadder *ladder)
{
    s32 x = ladder->x;
    s32 y = ladder->y;

    switch (ladder->kind)
    {
    case MT_MOON_LADDER_DOOR:
        ForceFloorRect(cave, x - 1, y - 5, x + 1, y - 1);
        break;
    case MT_MOON_LADDER_DOWN:
        ForceFloorRect(cave, x - 2, y - 2, x + 2, y + 2);
        break;
    case MT_MOON_LADDER_UP:
        ForceFloorRect(cave, x - 1, y, x + 1, y + 3);
        break;
    }
    MtMoonGen_MarkLadderSite(ladder);
}

/* Open floor around both ladders, so neither ends up walled in or loses its
 * footprint. Runs after every pass that changes cells. (The borders don't
 * need the same: they are written once, after the random fill, and every
 * later pass only turns border cells into wall, never floor.) */
static void ReinforceKeyAreas(const struct MtMoonCave *cave)
{
    ReinforceLadder(cave, &cave->entrance);
    ReinforceLadder(cave, &cave->exit);
}

static void InitRandomGrid(const struct MtMoonCave *cave)
{
    s32 x, y;

    for (y = 0; y < cave->height; y++)
        for (x = 0; x < cave->width; x++)
            /* A 16-bit random number is below 30% of 65536 30% of the time. */
            SET_CELL(x, y, MtMoonGen_Random() < INITIAL_FLOOR_PERCENT * 0x10000 / 100
                ? MT_MOON_CELL_FLOOR : MT_MOON_CELL_WALL);
    EnforceBorders(cave);
    ReinforceKeyAreas(cave);
}

/* How many of the 3 cells in column x of the rows above, at and below are
 * wall. */
#define COLUMN_WALLS(above, row, below, x) \
    (!FLOOR_AT(above, (x)) + !FLOOR_AT(row, (x)) + !FLOOR_AT(below, (x)))

/*
 * One cellular-automata pass: a cell is wall when 5+ of its 8 neighbours
 * are. Cells change in place, so a cell sees the new values of the cells
 * before it (west of it and in the rows above).
 *
 * This is the most-run loop of the style (every cell, four times), so it
 * slides a 3 x 3 window along each row instead of reading all 8 neighbours
 * per cell: `left`, `mid` and `right` count the walls in the window's
 * three columns, and each step only reads the new right column. The cell
 * itself is in the middle column, so it is subtracted from the total.
 */
static void SmoothGrid(const struct MtMoonCave *cave)
{
    s32 x, y;
    const u8 *above, *row, *below;

    for (y = 2; y < cave->height - 3; y++)
    {
        s32 left, mid, right;

        above = MtMoonGen_GetRow(y - 1);
        row = MtMoonGen_GetRow(y);
        below = MtMoonGen_GetRow(y + 1);
        left = COLUMN_WALLS(above, row, below, 1);
        mid = COLUMN_WALLS(above, row, below, 2);
        for (x = 2; x < cave->width - 2; x++)
        {
            s32 wasWall = !FLOOR_AT(row, x);
            s32 isWall;

            right = COLUMN_WALLS(above, row, below, x + 1);
            isWall = (left + mid + right - wasWall >= 5);
            if (isWall != wasWall)
                SET_CELL(x, y, isWall ? MT_MOON_CELL_WALL : MT_MOON_CELL_FLOOR);
            /* Slide right. The cell just decided is now in the left column,
             * with its new value. */
            left = mid - wasWall + isWall;
            mid = right;
        }
    }
}

/* Drop floor cells with fewer than 3 floor neighbours (out of 8). */
static void DropLoneFloor(const struct MtMoonCave *cave)
{
    s32 x, y;
    const u8 *above, *row, *below;

    for (y = 3; y < cave->height - 3; y++)
    {
        above = MtMoonGen_GetRow(y - 1);
        row = MtMoonGen_GetRow(y);
        below = MtMoonGen_GetRow(y + 1);
        for (x = 3; x < cave->width - 3; x++)
            if (row[x] == MT_MOON_CELL_FLOOR && 8 - WALLS_AROUND(above, row, below, x) < 3)
                SET_CELL(x, y, MT_MOON_CELL_WALL);
    }
}

/*
 * Straighten wall edges.
 *
 *   A. Remove wall bumps: a wall cell with floor on two adjacent sides (say
 *      N and E) AND on the diagonal between them (NE) sticks out into the
 *      floor as a single-cell point. Make it floor.
 *   B. Fill notches: a floor cell with wall on 3 or 4 sides is a 1-cell dent
 *      in a wall. Make it wall.
 *
 * Each pass repeats a few times because one fix can expose another.
 */
static void SmoothWallEdges(const struct MtMoonCave *cave)
{
    s32 x, y, iter;
    bool8 changed;
    const u8 *above, *row, *below;

    for (iter = 0; iter < 3; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
        {
            above = MtMoonGen_GetRow(y - 1);
            row = MtMoonGen_GetRow(y);
            below = MtMoonGen_GetRow(y + 1);
            for (x = 2; x < cave->width - 2; x++)
            {
                bool8 floorN, floorS, floorW, floorE;

                if (row[x] != MT_MOON_CELL_WALL)
                    continue;
                floorN = FLOOR_AT(above, x);
                floorS = FLOOR_AT(below, x);
                floorW = FLOOR_AT(row, x - 1);
                floorE = FLOOR_AT(row, x + 1);
                if ((floorN && floorE && FLOOR_AT(above, x + 1))
                    || (floorN && floorW && FLOOR_AT(above, x - 1))
                    || (floorS && floorE && FLOOR_AT(below, x + 1))
                    || (floorS && floorW && FLOOR_AT(below, x - 1)))
                {
                    SET_CELL(x, y, MT_MOON_CELL_FLOOR);
                    changed = TRUE;
                }
            }
        }
        if (!changed)
            break;
    }

    for (iter = 0; iter < 2; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
        {
            above = MtMoonGen_GetRow(y - 1);
            row = MtMoonGen_GetRow(y);
            below = MtMoonGen_GetRow(y + 1);
            for (x = 2; x < cave->width - 2; x++)
            {
                if (row[x] == MT_MOON_CELL_WALL)
                    continue;
                if (CARDINAL_WALLS(above, row, below, x) >= 3)
                {
                    SET_CELL(x, y, MT_MOON_CELL_WALL);
                    changed = TRUE;
                }
            }
        }
        if (!changed)
            break;
    }
}

/*
 * Close diagonal pinches. In a 2x2 block like
 *
 *     W F        F W
 *     F W   or   W F
 *
 * two walls touch only at a corner, which draws as a jagged point. Turn one
 * of the two floor cells into wall: the one with more wall neighbours, so
 * the least open space is lost.
 */
static void FillDiagonalCorners(const struct MtMoonCave *cave)
{
    s32 x, y, iter;
    bool8 changed;
    const u8 *row, *below;

    for (iter = 0; iter < 3; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
        {
            row = MtMoonGen_GetRow(y);
            below = MtMoonGen_GetRow(y + 1);
            for (x = 2; x < cave->width - 3; x++)
            {
                /* The 2x2 block with (x, y) at its top-left corner. */
                bool8 wallTopLeft     = !FLOOR_AT(row, x);
                bool8 wallTopRight    = !FLOOR_AT(row, x + 1);
                bool8 wallBottomLeft  = !FLOOR_AT(below, x);
                bool8 wallBottomRight = !FLOOR_AT(below, x + 1);
                if (wallTopLeft && wallBottomRight && !wallTopRight && !wallBottomLeft)
                {
                    if (MtMoonGen_CountWallNeighbors(x + 1, y) >= MtMoonGen_CountWallNeighbors(x, y + 1))
                        SET_CELL(x + 1, y, MT_MOON_CELL_WALL);
                    else
                        SET_CELL(x, y + 1, MT_MOON_CELL_WALL);
                    changed = TRUE;
                }
                else if (!wallTopLeft && !wallBottomRight && wallTopRight && wallBottomLeft)
                {
                    if (MtMoonGen_CountWallNeighbors(x, y) >= MtMoonGen_CountWallNeighbors(x + 1, y + 1))
                        SET_CELL(x, y, MT_MOON_CELL_WALL);
                    else
                        SET_CELL(x + 1, y + 1, MT_MOON_CELL_WALL);
                    changed = TRUE;
                }
            }
        }
        if (!changed)
            break;
    }
}

/*
 * Remove thin walls: wall cells with floor on both opposite sides (a 1-wide
 * strand), and wall cells joined to the rest of the wall by at most one side
 * (a 1-cell finger). Both become floor.
 */
static void RemoveThinWalls(const struct MtMoonCave *cave)
{
    s32 x, y, iter;
    bool8 changed;
    const u8 *above, *row, *below;

    for (iter = 0; iter < 4; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
        {
            above = MtMoonGen_GetRow(y - 1);
            row = MtMoonGen_GetRow(y);
            below = MtMoonGen_GetRow(y + 1);
            for (x = 2; x < cave->width - 2; x++)
            {
                if (row[x] != MT_MOON_CELL_WALL)
                    continue;
                if ((FLOOR_AT(row, x - 1) && FLOOR_AT(row, x + 1))
                    || (FLOOR_AT(above, x) && FLOOR_AT(below, x)))
                {
                    SET_CELL(x, y, MT_MOON_CELL_FLOOR);
                    changed = TRUE;
                    continue;
                }
                if (CARDINAL_WALLS(above, row, below, x) <= 1)
                {
                    SET_CELL(x, y, MT_MOON_CELL_FLOOR);
                    changed = TRUE;
                }
            }
        }
        if (!changed)
            break;
    }
}

/*
 * Carve a 3-wide corridor from (fromX, fromY) to (toX, toY). Each step
 * north or south also drifts randomly one cell west, east or not at all, so
 * the corridor winds instead of running dead straight.
 */
static void CarvePath(const struct MtMoonCave *cave, s32 fromX, s32 fromY, s32 toX, s32 toY)
{
    s32 x = fromX;
    s32 y = fromY;

    while (x != toX || y != toY)
    {
        s32 dx;
        SET_CELL(x, y, MT_MOON_CELL_FLOOR);
        if (x > 2)
            SET_CELL(x - 1, y, MT_MOON_CELL_FLOOR);
        if (x < cave->width - 3)
            SET_CELL(x + 1, y, MT_MOON_CELL_FLOOR);

        if (y != toY)
        {
            y += (y > toY) ? -1 : 1;
            dx = MtMoonGen_RandomBelow(3) - 1;
            if (x + dx > 2 && x + dx < cave->width - 3)
                x += dx;
        }
        else if (x > toX)
        {
            x--;
        }
        else
        {
            x++;
        }
    }
    ForceFloorRect(cave, toX - 1, toY - 1, toX + 1, toY + 1);
}

/*
 * See the overview at the top. Every pass that changes cells is followed
 * by ReinforceKeyAreas, which puts the ladders' open floor and footprints
 * back. The corridor carved at the end only ever adds floor,
 * and stays between rows 2 and height - 4, so it can't break a footprint:
 * the walls an up-ladder needs are the top border (rows 0 and 1), and the
 * walls under 1F's door are the last two rows.
 */
static void BuildCellularShape(struct MtMoonCave *cave)
{
    s32 pass;
    s32 fromX, fromY, toX, toY;

    InitRandomGrid(cave);

    for (pass = 0; pass < 4; pass++)
    {
        SmoothGrid(cave);
        ReinforceKeyAreas(cave);
    }

    DropLoneFloor(cave);
    ReinforceKeyAreas(cave);

    RemoveThinWalls(cave);
    ReinforceKeyAreas(cave);

    FillDiagonalCorners(cave);
    ReinforceKeyAreas(cave);

    SmoothWallEdges(cave);
    ReinforceKeyAreas(cave);

    MtMoonGen_GetLadderFoot(&cave->entrance, &fromX, &fromY);
    MtMoonGen_GetLadderFoot(&cave->exit, &toX, &toY);
    if (!MtMoonGen_FloodFillReaches(fromX, fromY, toX, toY))
        CarvePath(cave, fromX, fromY, toX, toY);
}

static u16 GetWallTile(s32 x, s32 y)
{
    u8 floorMask = FLOOR_MASK(x, y);
    /* One bit per side with floor: 8 = N, 4 = S, 2 = W, 1 = E. */
    u8 sides = ((floorMask & MT_MOON_N) ? 8 : 0) | ((floorMask & MT_MOON_S) ? 4 : 0)
             | ((floorMask & MT_MOON_W) ? 2 : 0) | ((floorMask & MT_MOON_E) ? 1 : 0);

    switch (sides)
    {
    case 0x0: return (y == 0) ? TILE_WALL_SOLID_TOP_ROW : TILE_WALL_SOLID;
    case 0x1: return TILE_WALL_FLOOR_E;
    case 0x2: return TILE_WALL_FLOOR_W;
    case 0x3: return TILE_WALL_THIN;
    case 0x4: return TILE_WALL_FLOOR_S;
    case 0x5: return TILE_WALL_FLOOR_SE;
    case 0x6: return TILE_WALL_FLOOR_SW;
    case 0x7: return TILE_WALL_FLOOR_S;
    case 0xB: return TILE_WALL_FLOOR_N_E_W;
    case 0xC: return TILE_WALL_THIN;
    default:  return TILE_WALL_SOLID;
    }
}

static u16 PickCellularTile(const struct MtMoonCave *cave, s32 x, s32 y)
{
    u16 ladderTile = MtMoonGen_GetLadderTile(cave, x, y);

    if (ladderTile != 0)
        return ladderTile;
    if (IS_FLOOR(x, y))
    {
        /* A fixed hash of the position picks ~8% of floor cells (values
         * below 21 out of 256) as sand. It only depends on (x, y), so the
         * same cave always gets the same sand. */
        u32 hash = (x * 73u + y * 151u + x * y * 17u) & 0xff;
        return (hash < 21) ? TILE_SAND : TILE_FLOOR;
    }
    return GetWallTile(x, y);
}

/*
 * Every generated floor currently uses this style. A floor that gets its own
 * style deletes its line here and defines the same object in its own module
 * (see include/mt_moon_gen_style.h).
 */

const struct MtMoonStyle gMtMoonStyle1F = { BuildCellularShape, PickCellularTile };

const struct MtMoonStyle gMtMoonStyleB1F = { BuildCellularShape, PickCellularTile };

const struct MtMoonStyle gMtMoonStyleB2F = { BuildCellularShape, PickCellularTile };
