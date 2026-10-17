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
 *   1. Fill the grid with random walls and floors (about 30% floor).
 *   2. Smooth it four times with a cellular-automata rule: a cell becomes
 *      wall if 5 or more of its 8 neighbours are wall, otherwise floor. Blobs
 *      of noise grow into cave-like rooms.
 *   3. Keep a solid border (2 cells on the sides and top, 3 at the bottom)
 *      and open corridors in front of the entrance and the exit.
 *   4. Clean up shapes the tiles draw badly: lone floor cells, 1-cell-wide
 *      wall strands, diagonal wall pinches, bumps and notches.
 *   5. If the entrance can't reach the exit, carve a winding corridor.
 *
 * TILES (PickCellularTile): floor cells get the cave floor tile, with a
 * position-based ~8% sprinkling of sand. Wall cells get a tile chosen from
 * which of their four neighbours (N, S, W, E) are floor; see GetWallTile.
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
#define CELL(x, y) MtMoonGen_GetCell((x), (y))
#define SET_CELL(x, y, v) MtMoonGen_SetCell((x), (y), (v))

/* How many of the 4 cells N, S, W and E of (x, y) are wall (off-grid
 * counts as wall). */
static u8 CountCardinalWalls(s32 x, s32 y)
{
    return (!IS_FLOOR(x, y - 1)) + (!IS_FLOOR(x, y + 1))
         + (!IS_FLOOR(x - 1, y)) + (!IS_FLOOR(x + 1, y));
}

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

/* Borders, plus open corridors leading north from the entrance and south
 * from the exit, so neither ends up walled in. */
static void ReinforceKeyAreas(const struct MtMoonCave *cave)
{
    EnforceBorders(cave);
    ForceFloorRect(cave, cave->entranceX - 1, cave->entranceY - 5, cave->entranceX + 1, cave->entranceY);
    ForceFloorRect(cave, cave->exitX - 1, cave->exitY, cave->exitX + 1, cave->exitY + 5);
}

static void InitRandomGrid(const struct MtMoonCave *cave)
{
    s32 x, y;

    for (y = 0; y < cave->height; y++)
        for (x = 0; x < cave->width; x++)
            SET_CELL(x, y, (MtMoonGen_Random() % 100) < INITIAL_FLOOR_PERCENT
                ? MT_MOON_CELL_FLOOR : MT_MOON_CELL_WALL);

    /* Like ReinforceKeyAreas, but the starting corridors are one cell
     * longer (6 instead of 5) to give smoothing more open space to grow
     * from. */
    EnforceBorders(cave);
    ForceFloorRect(cave, cave->entranceX - 1, cave->entranceY - 6, cave->entranceX + 1, cave->entranceY);
    ForceFloorRect(cave, cave->exitX - 1, cave->exitY, cave->exitX + 1, cave->exitY + 6);
}

/* One cellular-automata pass: a cell is wall when 5+ of its 8 neighbours are. */
static void SmoothGrid(const struct MtMoonCave *cave)
{
    s32 x, y;
    for (y = 2; y < cave->height - 3; y++)
        for (x = 2; x < cave->width - 2; x++)
            SET_CELL(x, y, MtMoonGen_CountWallNeighbors(x, y) >= 5
                ? MT_MOON_CELL_WALL : MT_MOON_CELL_FLOOR);
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
    bool8 floorN, floorS, floorW, floorE;

    for (iter = 0; iter < 3; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
            for (x = 2; x < cave->width - 2; x++)
            {
                if (CELL(x, y) != MT_MOON_CELL_WALL)
                    continue;
                floorN = IS_FLOOR(x,     y - 1);
                floorS = IS_FLOOR(x,     y + 1);
                floorW = IS_FLOOR(x - 1, y);
                floorE = IS_FLOOR(x + 1, y);
                if ((floorN && floorE && IS_FLOOR(x + 1, y - 1))
                    || (floorN && floorW && IS_FLOOR(x - 1, y - 1))
                    || (floorS && floorE && IS_FLOOR(x + 1, y + 1))
                    || (floorS && floorW && IS_FLOOR(x - 1, y + 1)))
                {
                    SET_CELL(x, y, MT_MOON_CELL_FLOOR);
                    changed = TRUE;
                }
            }
        if (!changed)
            break;
    }

    for (iter = 0; iter < 2; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
            for (x = 2; x < cave->width - 2; x++)
            {
                if (CELL(x, y) == MT_MOON_CELL_WALL)
                    continue;
                if (CountCardinalWalls(x, y) >= 3)
                {
                    SET_CELL(x, y, MT_MOON_CELL_WALL);
                    changed = TRUE;
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

    for (iter = 0; iter < 3; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
            for (x = 2; x < cave->width - 3; x++)
            {
                /* The 2x2 block with (x, y) at its top-left corner. */
                bool8 wallTopLeft     = !IS_FLOOR(x,     y);
                bool8 wallTopRight    = !IS_FLOOR(x + 1, y);
                bool8 wallBottomLeft  = !IS_FLOOR(x,     y + 1);
                bool8 wallBottomRight = !IS_FLOOR(x + 1, y + 1);
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

    for (iter = 0; iter < 4; iter++)
    {
        changed = FALSE;
        for (y = 2; y < cave->height - 3; y++)
            for (x = 2; x < cave->width - 2; x++)
            {
                if (CELL(x, y) != MT_MOON_CELL_WALL)
                    continue;
                if ((IS_FLOOR(x - 1, y) && IS_FLOOR(x + 1, y))
                    || (IS_FLOOR(x, y - 1) && IS_FLOOR(x, y + 1)))
                {
                    SET_CELL(x, y, MT_MOON_CELL_FLOOR);
                    changed = TRUE;
                    continue;
                }
                if (CountCardinalWalls(x, y) <= 1)
                {
                    SET_CELL(x, y, MT_MOON_CELL_FLOOR);
                    changed = TRUE;
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
            dx = (MtMoonGen_Random() % 3) - 1;
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

static void BuildCellularShape(const struct MtMoonCave *cave)
{
    s32 pass, x, y;

    InitRandomGrid(cave);

    for (pass = 0; pass < 4; pass++)
    {
        SmoothGrid(cave);
        ReinforceKeyAreas(cave);
    }

    /* Drop floor cells with fewer than 3 floor neighbours. */
    for (y = 3; y < cave->height - 3; y++)
        for (x = 3; x < cave->width - 3; x++)
            if (CELL(x, y) == MT_MOON_CELL_FLOOR && (8 - MtMoonGen_CountWallNeighbors(x, y)) < 3)
                SET_CELL(x, y, MT_MOON_CELL_WALL);
    ReinforceKeyAreas(cave);

    RemoveThinWalls(cave);
    ReinforceKeyAreas(cave);

    FillDiagonalCorners(cave);
    ReinforceKeyAreas(cave);

    SmoothWallEdges(cave);
    ReinforceKeyAreas(cave);

    if (!MtMoonGen_FloodFillReaches(cave->entranceX, cave->entranceY, cave->exitX, cave->exitY))
        CarvePath(cave, cave->entranceX, cave->entranceY, cave->exitX, cave->exitY);
}

static u16 GetWallTile(s32 x, s32 y)
{
    bool8 floorN = IS_FLOOR(x, y - 1);
    bool8 floorS = IS_FLOOR(x, y + 1);
    bool8 floorW = IS_FLOOR(x - 1, y);
    bool8 floorE = IS_FLOOR(x + 1, y);
    /* One bit per side with floor: 8 = N, 4 = S, 2 = W, 1 = E. */
    u8 sides = (floorN << 3) | (floorS << 2) | (floorW << 1) | floorE;

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
