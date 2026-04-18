/*
 * mt_moon_gen.c - Procedural Cave Generation for Mt. Moon
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * This file implements a procedurally generated version of Mt. Moon. Instead
 * of using a fixed map layout from ROM, it generates a random cave layout
 * each time the player enters. This is a CUSTOM ADDITION to Pokemon FireRed
 * (not present in the original game).
 *
 * ALGORITHM:
 * The cave generator uses a cellular automata approach:
 * 1. Initialize a 48x40 grid with random walls/floors (configurable density)
 * 2. Run cellular automata smoothing passes: a cell becomes a wall if it has
 *    too many wall neighbors, or a floor if it has enough floor neighbors
 * 3. Enforce border walls (2-tile thick borders on all sides)
 * 4. Place entrance and exit points with doorframe tiles
 * 5. Ensure a walkable path exists between entrance and exit (flood fill check)
 * 6. If no valid path exists, force corridors to connect them
 * 7. Convert the abstract grid to GBA metatile data and write to the map
 * 8. Set up warp events for entrance/exit connections
 *
 * DATA STRUCTURE:
 * The cave grid uses 2-bit packed storage (4 cells per byte) to save memory:
 *   0 = wall
 *   1 = floor (walkable)
 *   2 = visited (used during flood fill pathfinding)
 *
 * METATILE MAPPING:
 * The grid cells are converted to visual metatiles based on their neighbors.
 * Wall tiles are selected based on which adjacent cells are floors, creating
 * natural-looking cave walls with proper cliff edges, corners, and interiors.
 *
 * GBA CONTEXT:
 * Map data on the GBA is stored as a 2D array of metatile IDs. Each metatile
 * is a 16x16 pixel block composed of four 8x8 tiles. The metatile ID also
 * encodes collision (walkable/blocked) and elevation (used for bridges/ledges).
 * Format: metatileId | (collision << 10) | (elevation << 12)
 * ============================================================================
 */

#include "global.h"
#include "fieldmap.h"
#include "overworld.h"
#include "random.h"
#include "mt_moon_gen.h"
#include "constants/map_groups.h"
#include "constants/maps.h"
#include "constants/layouts.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"

/* Cave dimensions in metatiles (16x16 pixel blocks) */
#define CAVE_W 48
#define CAVE_H 40
#define CAVE_SIZE (CAVE_W * CAVE_H)

// Full u16 metatile entries (metatile | collision<<10 | elevation<<12)
// TILE_FLOOR: the "regular" cave floor (metatile 0x281, palette 8). At
// runtime this palette renders as a muted tan-with-wavy-grain, and
// vanilla Mt Moon uses it for ~44% of all map cells -- it's the
// dominant floor the player walks on in the corridor network.
// TILE_SAND: the brighter yellow-gold sand patch (metatile 0x2fc, palette
// 11) that vanilla scatters as accents near items and in the upper
// plateau area. We sprinkle a minority of floor cells with this tile
// so the cave has visual variety instead of a uniform carpet.
#define TILE_FLOOR          0x3281
#define TILE_SAND           0x32fc
#define TILE_ENTRANCE       0x3287
#define TILE_EXIT           0x3285

/*
 * Wall tiles by cardinal adjacency.
 *
 * The Mt. Moon tileset actually provides TWO wall sets that render at
 * DIFFERENT APPARENT HEIGHTS:
 *
 *   A. Rim-wall (single-cell) set, 0x0688..0x069a. A clean 3x3 panel:
 *        0x0688 TL  0x0689 T   0x068A TR
 *        0x0690 L   0x0691 mid 0x0692 R
 *        0x0698 BL  0x0699 B   0x069A BR
 *      Each cell is a SHORT wall -- the top edge is visible, the
 *      bottom edge is visible, and it reads as "about one tile tall".
 *      Good for thin interior wall strips in procgen caves.
 *
 *   B. Cliff-face (TWO-cell tall) set:
 *                            0x06c4   (upper cliff face -- the row that
 *                                       sits ABOVE a cliff-bottom cell;
 *                                       looks like the top of a tall
 *                                       drop. Has collision.)
 *        0x0710  0x0711  0x0712       (cliff bottom row: BL / B / BR.
 *                                       These are "wall with floor to
 *                                       the south" i.e. the face the
 *                                       player sees when standing in
 *                                       front of a tall wall.)
 *        0x0715          0x0716       (cliff side faces: LEFT edge,
 *                                       RIGHT edge -- for vertical
 *                                       cliff walls with floor to the
 *                                       west or east respectively.)
 *      The 2-row "upper + bottom" structure is what makes vanilla cave
 *      walls look VISUALLY MULTI-LEVEL / TALL. The upper row (0x06c4)
 *      is NOT a separate wall cell in the map grid -- it's what we
 *      place in the wall cell IMMEDIATELY NORTH of a cliff-bottom cell.
 *
 * Procgen strategy:
 *   - If a wall cell would get signature BELOW / BELOW_LEFT / BELOW_RIGHT
 *     AND the cell to its NORTH is also a wall (i.e. this is the bottom
 *     of a wall that is at least 2 cells tall), use the CLIFF set so it
 *     reads as a tall wall. Thin single-cell wall strips still fall
 *     back to the short rim-wall set.
 *   - If a wall cell has NO cardinal floor and its SOUTH neighbour is
 *     a cliff-bottom, place 0x06c4 on it so the rendered column reads
 *     "upper cliff face ABOVE cliff bottom" -- the multi-level look.
 *   - Vertical wall edges (floor E or floor W) also promote to cliff
 *     side faces 0x0716 / 0x0715 when the wall is at least 2 cells
 *     tall, matching the top and bottom cells so the cliff has height.
 *   - Corners/inside cells keep rim-wall style to avoid mismatched
 *     seams where cliff and rim would meet.
 */

/* --- Rim-wall (short) set. Used for single-cell wall strips and
 * anywhere the cliff styling doesn't apply. ----------------------- */
#define TILE_W_NONE         0x0691  // rim interior
#define TILE_W_RIM_BELOW    0x0699  // rim bottom-center
#define TILE_W_ABOVE        0x0689  // rim top-center
#define TILE_W_RIM_LEFT     0x0690  // rim left
#define TILE_W_RIM_RIGHT    0x0692  // rim right
#define TILE_W_ABOVE_LEFT   0x0688  // rim top-left corner
#define TILE_W_ABOVE_RIGHT  0x068a  // rim top-right corner
#define TILE_W_RIM_BL       0x0698  // rim bottom-left corner
#define TILE_W_RIM_BR       0x069a  // rim bottom-right corner
#define TILE_W_ABOVE_LR     0x069f  // narrow vertical wall, top
#define TILE_W_LEFT_RIGHT   0x0682  // single-column wall
#define TILE_W_ABOVE_BELOW  0x0682  // single-row wall sandwiched N-S
#define TILE_W_SURROUNDED   0x0691  // floors on all 4 sides -> isolated wall

/* --- Cliff-face (tall) set. The "BELOW" variants go in the BOTTOM
 * cell of a tall wall; TILE_W_UPPER_FACE goes in the wall cell
 * immediately ABOVE a TILE_W_CLIFF_B* so the pair renders as a 2-row
 * tall cliff. SIDE_LEFT/RIGHT are the 2-row-tall vertical cliff faces
 * for walls with only floor west or east respectively. ------------ */
#define TILE_W_CLIFF_B      0x0711  // cliff bottom-center (floor south)
#define TILE_W_CLIFF_BL     0x0710  // cliff bottom-left corner (S+W)
#define TILE_W_CLIFF_BR     0x0712  // cliff bottom-right corner (S+E)
#define TILE_W_CLIFF_SIDE_L 0x0715  // cliff left edge (floor west)
#define TILE_W_CLIFF_SIDE_R 0x0716  // cliff right edge (floor east)
#define TILE_W_CLIFF_BODY   0x06c6  // cliff body -- STACKABLE vertically
                                    // (vanilla stacks 0x06c6 on itself
                                    // for any row height, forming the
                                    // tall cliff body between the top
                                    // and the cliff bottom 0x0711).
#define TILE_W_UPPER_FACE   0x06c4  // cliff top-face (placed ABOVE a
                                    // cliff-body column -- vanilla uses
                                    // this at the map's top border).

// Entrance doorframe
#define TILE_DOORFRAME_M    0x06c9  // 713
#define TILE_BOTTOM_WALL    0x06a3  // 675

// Cave grid: 2-bit packed (0=wall, 1=floor, 2=visited)
#define GRID_BYTES ((CAVE_SIZE * 2 + 7) / 8)
static EWRAM_DATA u8 sCaveGridPacked[GRID_BYTES] = {0};

static EWRAM_DATA struct WarpEvent sMtMoonWarps[8] = {0};
static EWRAM_DATA struct MapEvents sMtMoonEvents = {0};

// Current generation entrance/exit positions (set per generation)
static s32 sEntranceX, sEntranceY, sExitX, sExitY;

static u8 GridGet(s32 x, s32 y)
{
    s32 idx = y * CAVE_W + x;
    s32 byteIdx = idx / 4;
    s32 shift = (idx % 4) * 2;
    return (sCaveGridPacked[byteIdx] >> shift) & 0x3;
}

static void GridSet(s32 x, s32 y, u8 val)
{
    s32 idx = y * CAVE_W + x;
    s32 byteIdx = idx / 4;
    s32 shift = (idx % 4) * 2;
    sCaveGridPacked[byteIdx] = (sCaveGridPacked[byteIdx] & ~(0x3 << shift)) | ((val & 0x3) << shift);
}

#define GRID(x, y) GridGet((x), (y))
#define GRID_SET(x, y, v) GridSet((x), (y), (v))

static bool8 IsFloor(s32 x, s32 y)
{
    if (x < 0 || x >= CAVE_W || y < 0 || y >= CAVE_H)
        return FALSE;
    return GRID(x, y) != 0;
}

static void ForceFloorRect(s32 x1, s32 y1, s32 x2, s32 y2)
{
    s32 x, y;
    for (y = y1; y <= y2; y++)
        for (x = x1; x <= x2; x++)
            if (x >= 2 && x < CAVE_W - 2 && y >= 2 && y < CAVE_H - 3)
                GRID_SET(x, y, 1);
}

static void EnforceBorders(void)
{
    s32 x, y;
    for (x = 0; x < CAVE_W; x++)
    {
        GRID_SET(x, 0, 0);
        GRID_SET(x, 1, 0);
        GRID_SET(x, CAVE_H - 1, 0);
        GRID_SET(x, CAVE_H - 2, 0);
        GRID_SET(x, CAVE_H - 3, 0);
    }
    for (y = 0; y < CAVE_H; y++)
    {
        GRID_SET(0, y, 0);
        GRID_SET(1, y, 0);
        GRID_SET(CAVE_W - 1, y, 0);
        GRID_SET(CAVE_W - 2, y, 0);
    }
}

static u8 CountWallNeighbors(s32 cx, s32 cy)
{
    s32 x, y;
    u8 count = 0;
    for (y = cy - 1; y <= cy + 1; y++)
        for (x = cx - 1; x <= cx + 1; x++)
        {
            if (x == cx && y == cy) continue;
            if (x < 0 || x >= CAVE_W || y < 0 || y >= CAVE_H)
                count++;
            else if (GRID(x, y) == 0)
                count++;
        }
    return count;
}

static void ReinforceKeyAreas(void)
{
    EnforceBorders();
    // Entrance corridor
    ForceFloorRect(sEntranceX - 1, sEntranceY - 5, sEntranceX + 1, sEntranceY);
    // Exit corridor
    ForceFloorRect(sExitX - 1, sExitY, sExitX + 1, sExitY + 5);
}

static void InitRandomGrid(void)
{
    s32 x, y;

    for (y = 0; y < CAVE_H; y++)
        for (x = 0; x < CAVE_W; x++)
            GRID_SET(x, y, (Random() % 100) < 30 ? 1 : 0);

    EnforceBorders();
    ForceFloorRect(sEntranceX - 1, sEntranceY - 6, sEntranceX + 1, sEntranceY);
    ForceFloorRect(sExitX - 1, sExitY, sExitX + 1, sExitY + 6);
}

static void SmoothGrid(void)
{
    s32 x, y;
    for (y = 2; y < CAVE_H - 3; y++)
        for (x = 2; x < CAVE_W - 2; x++)
        {
            u8 walls = CountWallNeighbors(x, y);
            GRID_SET(x, y, (walls >= 5) ? 0 : 1);
        }
}

/*
 * Smooth wall boundaries so they match vanilla Mt Moon's rounded,
 * axis-aligned cliff edges instead of jagged stair-steps.
 *
 * We run two cleanups:
 *
 *   A. REMOVE WALL BUMPS -- a wall cell at a convex corner (2 adjacent
 *      cardinal floors, say N and E) whose 2x2 block's OTHER three
 *      cells aren't all walls is a single-cell "bump" jutting into the
 *      floor. It renders with a cliff-corner tile that looks like a
 *      sharp diagonal point, because the tile expects to sit on top of
 *      a 2x2 wall block. Remove it.
 *
 *   B. FILL WALL NOTCHES -- a floor cell with 3+ cardinal wall
 *      neighbours is a 1-cell indent scooped out of a wall mass. It
 *      makes the wall boundary look serrated from the floor side.
 *      Fill it back to wall.
 *
 * Each cleanup iterates a few times because one fix can expose another.
 * Applied AFTER the thin-wall removal so walls are already at least
 * 2 cells wide; the remaining artefacts are irregular edges.
 */
static void SmoothWallEdges(void)
{
    s32 x, y, iter;
    bool8 changed;
    bool8 fA, fB, fL, fR;
    u8 walls;

    /* Pass A: remove convex-corner bumps */
    for (iter = 0; iter < 3; iter++)
    {
        changed = FALSE;
        for (y = 2; y < CAVE_H - 3; y++)
            for (x = 2; x < CAVE_W - 2; x++)
            {
                if (GRID(x, y) != 0) continue;  /* skip floor */
                fA = IsFloor(x,     y - 1);
                fB = IsFloor(x,     y + 1);
                fL = IsFloor(x - 1, y);
                fR = IsFloor(x + 1, y);
                /* Check each convex-corner case (2 adjacent cardinal
                 * floors). The "inside" diagonal of that corner should
                 * be a wall -- if it's a floor, this cell is a bump. */
                if (fA && fR && IsFloor(x + 1, y - 1))      { GRID_SET(x, y, 1); changed = TRUE; continue; }
                if (fA && fL && IsFloor(x - 1, y - 1))      { GRID_SET(x, y, 1); changed = TRUE; continue; }
                if (fB && fR && IsFloor(x + 1, y + 1))      { GRID_SET(x, y, 1); changed = TRUE; continue; }
                if (fB && fL && IsFloor(x - 1, y + 1))      { GRID_SET(x, y, 1); changed = TRUE; continue; }
            }
        if (!changed) break;
    }

    /* Pass B: fill notches (floor with 3+ cardinal wall neighbours) */
    for (iter = 0; iter < 2; iter++)
    {
        changed = FALSE;
        for (y = 2; y < CAVE_H - 3; y++)
            for (x = 2; x < CAVE_W - 2; x++)
            {
                if (GRID(x, y) == 0) continue;  /* skip wall */
                walls = (!IsFloor(x, y - 1))
                      + (!IsFloor(x, y + 1))
                      + (!IsFloor(x - 1, y))
                      + (!IsFloor(x + 1, y));
                if (walls >= 3)
                {
                    GRID_SET(x, y, 0);
                    changed = TRUE;
                }
            }
        if (!changed) break;
    }
}

/*
 * Fill in diagonal wall-floor checkerboards to eliminate 2x2 patterns
 * where walls touch diagonally at a single point. In a block like
 *
 *     W F             F W
 *     F W     or      W F
 *
 * the wall mass visibly "pinches" to zero thickness at the shared
 * corner, which reads as a jagged serration once the tile picker
 * emits a rim corner for each wall cell. Vanilla Mt Moon never
 * contains this pattern -- its walls are always thickened to 2x2
 * blocks at bends. We match that by promoting one of the two floor
 * cells in the pattern to wall, closing the pinch.
 *
 * We pick the floor cell that already has the MORE wall neighbours;
 * that keeps the cave's overall walkable area intact and avoids
 * accidentally blocking corridors. The loop runs a few times because
 * one fill can create a new diagonal elsewhere.
 */
static void FillDiagonalCorners(void)
{
    s32 x, y, iter;
    bool8 changed;
    for (iter = 0; iter < 3; iter++)
    {
        changed = FALSE;
        for (y = 2; y < CAVE_H - 3; y++)
            for (x = 2; x < CAVE_W - 3; x++)
            {
                bool8 a = !IsFloor(x,     y);
                bool8 b = !IsFloor(x + 1, y);
                bool8 c = !IsFloor(x,     y + 1);
                bool8 d = !IsFloor(x + 1, y + 1);
                if (a && d && !b && !c)
                {
                    /* pick the floor with more wall neighbours */
                    if (CountWallNeighbors(x + 1, y) >= CountWallNeighbors(x, y + 1))
                        GRID_SET(x + 1, y, 0);
                    else
                        GRID_SET(x, y + 1, 0);
                    changed = TRUE;
                }
                else if (!a && !d && b && c)
                {
                    if (CountWallNeighbors(x, y) >= CountWallNeighbors(x + 1, y + 1))
                        GRID_SET(x, y, 0);
                    else
                        GRID_SET(x + 1, y + 1, 0);
                    changed = TRUE;
                }
            }
        if (!changed) break;
    }
}

/*
 * Convert 1-cell-wide wall strands back to floor.
 *
 * Cellular automata smoothing leaves thin vertical or horizontal wall
 * fingers sprinkled through the cave: cells that are wall but have
 * FLOOR on both opposite sides (floor W and floor E -- a vertical
 * 1-wide strand; or floor N and floor S -- a horizontal 1-wide strand).
 *
 * Those thin strands render as jagged cliff protrusions jutting out
 * onto the walkable area, because our tile picker falls back to the
 * short rim-wall set and the cliff-body set for them in awkward ways,
 * and because vanilla Mt Moon never has walls thinner than 2 cells.
 *
 * Wiping them produces the smooth, chunky wall masses the player
 * expects, and also gives the tile picker enough wall thickness to
 * consistently emit the 2-cell-tall cliff (upper face + body + bottom)
 * stack that makes cave walls read as "tall".
 *
 * Iterates up to 3 times because removing one strand can expose a
 * neighbour as newly 1-wide.
 */
static void RemoveThinWalls(void)
{
    s32 x, y, iter;
    u8 cardinalWalls;
    bool8 changed;
    for (iter = 0; iter < 4; iter++)
    {
        changed = FALSE;
        for (y = 2; y < CAVE_H - 3; y++)
            for (x = 2; x < CAVE_W - 2; x++)
            {
                if (GRID(x, y) != 0) continue;
                /* 1-wide strand: floor on both opposing cardinals */
                if ((IsFloor(x - 1, y) && IsFloor(x + 1, y))
                    || (IsFloor(x, y - 1) && IsFloor(x, y + 1)))
                {
                    GRID_SET(x, y, 1);
                    changed = TRUE;
                    continue;
                }
                /* 1-cell protrusion / tip: the cell connects to the
                 * main wall mass through at most ONE cardinal wall.
                 * It's a single wall "finger" jutting into floor and
                 * renders as a pointy rim corner stub that the player
                 * reads as an out-of-place tile. Demote to floor --
                 * the tile-picker then emits the surrounding wall
                 * edge tiles cleanly around the newly-opened cell. */
                cardinalWalls = (!IsFloor(x, y - 1))
                              + (!IsFloor(x, y + 1))
                              + (!IsFloor(x - 1, y))
                              + (!IsFloor(x + 1, y));
                if (cardinalWalls <= 1)
                {
                    GRID_SET(x, y, 1);
                    changed = TRUE;
                }
            }
        if (!changed) break;
    }
}

static bool8 FloodFillReaches(s32 fromX, s32 fromY, s32 toX, s32 toY)
{
    s32 x, y;
    bool8 changed;

    if (GRID(fromX, fromY) == 0)
        return FALSE;

    GRID_SET(fromX, fromY, 2);

    do
    {
        changed = FALSE;
        for (y = 1; y < CAVE_H - 1; y++)
            for (x = 1; x < CAVE_W - 1; x++)
                if (GRID(x, y) == 1
                    && (GRID(x-1, y) == 2 || GRID(x+1, y) == 2
                        || GRID(x, y-1) == 2 || GRID(x, y+1) == 2))
                {
                    GRID_SET(x, y, 2);
                    changed = TRUE;
                }
    } while (changed);

    {
        bool8 reachable = (GRID(toX, toY) == 2);
        for (y = 0; y < CAVE_H; y++)
            for (x = 0; x < CAVE_W; x++)
                if (GRID(x, y) == 2)
                    GRID_SET(x, y, 1);
        return reachable;
    }
}

static void CarvePath(s32 fromX, s32 fromY, s32 toX, s32 toY)
{
    s32 x = fromX;
    s32 y = fromY;

    while (x != toX || y != toY)
    {
        s32 dx;
        GRID_SET(x, y, 1);
        if (x > 2) GRID_SET(x - 1, y, 1);
        if (x < CAVE_W - 3) GRID_SET(x + 1, y, 1);

        if (y > toY)
        {
            y--;
            dx = (Random() % 3) - 1;
            if (x + dx > 2 && x + dx < CAVE_W - 3)
                x += dx;
        }
        else if (y < toY)
        {
            y++;
            dx = (Random() % 3) - 1;
            if (x + dx > 2 && x + dx < CAVE_W - 3)
                x += dx;
        }
        else if (x > toX)
            x--;
        else
            x++;
    }
    ForceFloorRect(toX - 1, toY - 1, toX + 1, toY + 1);
}

/*
 * Scan straight down from (x, y) looking for floor within `maxDepth`
 * rows. Returns the y-coordinate of the floor cell found, or -1 if
 * no floor within range. Used by GetWallTile to decide whether a
 * solid-interior wall cell is part of a tall cliff whose face is
 * visible below, in which case we render it as cliff body.
 */
static s32 FloorBelowWithin(s32 x, s32 y, s32 maxDepth)
{
    s32 dy;
    for (dy = 1; dy <= maxDepth; dy++)
    {
        if (y + dy >= CAVE_H) return -1;
        if (IsFloor(x, y + dy)) return y + dy;
    }
    return -1;
}

static u16 GetWallTile(s32 x, s32 y)
{
    bool8 fA = IsFloor(x, y - 1);
    bool8 fB = IsFloor(x, y + 1);
    bool8 fL = IsFloor(x - 1, y);
    bool8 fR = IsFloor(x + 1, y);
    u8 cardinal = (fA << 3) | (fB << 2) | (fL << 1) | fR;

    if (cardinal != 0)
    {
        switch (cardinal)
        {
        /* Wall-floor edges: RIM tiles (0x0688..0x069a) are the "cave
         * floor context" wall set -- they render as dark rocky walls
         * with a clean tan transition to the regular floor (0x3281).
         * The CLIFF set (0x0711, 0x0710, 0x0712, 0x0715, 0x0716) is
         * visually near-identical but has a yellow/gold pixel strip
         * along its floor-side edge; that yellow reads as "beach sand"
         * and doesn't belong against a tan cave floor. We reserve the
         * cliff set for walls adjacent to sand patches (handled via
         * the cliff body fallback for interior cells). */
        case 0x1: return TILE_W_RIM_RIGHT;     /* floor E   -> dark rocky right edge */
        case 0x2: return TILE_W_RIM_LEFT;      /* floor W   -> dark rocky left edge */
        case 0x3: return TILE_W_LEFT_RIGHT;    /* floor E+W (thin strand) */
        case 0x4: return TILE_W_RIM_BELOW;     /* floor S   -> dark rocky bottom */
        case 0x5: return TILE_W_RIM_BR;        /* floor S+E -> dark rocky BR corner */
        case 0x6: return TILE_W_RIM_BL;        /* floor S+W -> dark rocky BL corner */
        case 0x7: return TILE_W_RIM_BELOW;     /* floor S+E+W */
        /* Walls with floor NORTH: in vanilla these are rendered with
         * the rim-top set (0x0288/0x0289/0x028a), but those tiles are
         * mostly FLOOR-COLOURED ground with only a thin dark cliff-edge
         * strip -- they're designed to represent "looking down at a
         * short wall from above". Dropped adjacent to the dark cliff-
         * body cells we emit for interior walls, they read as a pale
         * flipped panel and break the tall-cliff illusion.
         * Emit cliff body instead so the wall stays uniformly dark all
         * the way up to the floor edge above. */
        case 0x8: return TILE_W_CLIFF_BODY;
        case 0x9: return TILE_W_CLIFF_BODY;
        case 0xA: return TILE_W_CLIFF_BODY;
        case 0xB: return TILE_W_ABOVE_LR;      /* floor N+E+W (thin strand) */
        case 0xC: return TILE_W_ABOVE_BELOW;   /* floor N+S (thin strand) */
        default:  return TILE_W_CLIFF_BODY;    /* floor on all 4 sides (isolated wall) */
        }
    }

    /*
     * Solid interior wall cell (no cardinal floor neighbour).
     *
     * Always emit TILE_W_CLIFF_BODY so interior wall cells blend with
     * the cliff-body / cliff-bottom tiles we emit along the wall-floor
     * boundary. The earlier design used TILE_W_NONE (rim interior) for
     * deep interior cells, but that tile renders as a light pinkish-tan
     * "dirt" panel under the cave's runtime palette and stands out
     * against the dark cliff-body walls around it -- exactly the
     * "random top wall tile sticking out" artefact.
     *
     * TILE_W_UPPER_FACE is only used at y == 0 (the top map border),
     * because vanilla uses that tile there to represent "cliff meeting
     * the void" at the map's edge. Anywhere else it produces a bright
     * flat-top tile that also breaks the cliff illusion.
     */
    if (y == 0)
        return TILE_W_UPPER_FACE;
    return TILE_W_CLIFF_BODY;
}

static void WriteGridToVMap(void)
{
    s32 x, y;
    s32 mapW = gMapHeader.mapLayout->width;
    s32 mapH = gMapHeader.mapLayout->height;

    // First: fill the ENTIRE map area with interior wall to prevent
    // any original ROM tiles from leaking through (important for B1F which is 49 wide)
    for (y = 0; y < mapH; y++)
        for (x = 0; x < mapW; x++)
            MapGridSetMetatileEntryAt(x + MAP_OFFSET, y + MAP_OFFSET, TILE_W_NONE);

    // Then write the generated cave grid over it.
    //
    // Floor cells: use TILE_FLOOR for the majority (vanilla's corridor
    // floor) and TILE_SAND for ~8% of cells as accent patches. A
    // position-based pseudo-hash keeps the pattern stable across frames
    // so we don't shimmer the floor on every redraw. (We can't use
    // Random() here because it advances the global RNG every call,
    // and WriteGridToVMap runs on every reload.)
    for (y = 0; y < CAVE_H; y++)
        for (x = 0; x < CAVE_W; x++)
        {
            u16 tile;
            if (GRID(x, y))
            {
                /* hash(x,y) -> 0..255; <21 (~8%) becomes sand */
                u32 h = (x * 73u + y * 151u + x * y * 17u) & 0xff;
                tile = (h < 21) ? TILE_SAND : TILE_FLOOR;
            }
            else
            {
                tile = GetWallTile(x, y);
            }
            MapGridSetMetatileEntryAt(x + MAP_OFFSET, y + MAP_OFFSET, tile);
        }

    // Entrance tile + doorframe below it
    MapGridSetMetatileEntryAt(sEntranceX + MAP_OFFSET, sEntranceY + MAP_OFFSET, TILE_ENTRANCE);
    MapGridSetMetatileEntryAt(sEntranceX - 1 + MAP_OFFSET, sEntranceY + 1 + MAP_OFFSET, TILE_W_ABOVE);
    MapGridSetMetatileEntryAt(sEntranceX + MAP_OFFSET, sEntranceY + 1 + MAP_OFFSET, TILE_DOORFRAME_M);
    MapGridSetMetatileEntryAt(sEntranceX + 1 + MAP_OFFSET, sEntranceY + 1 + MAP_OFFSET, TILE_W_ABOVE);
    MapGridSetMetatileEntryAt(sEntranceX - 1 + MAP_OFFSET, sEntranceY + 2 + MAP_OFFSET, TILE_BOTTOM_WALL);
    MapGridSetMetatileEntryAt(sEntranceX + MAP_OFFSET, sEntranceY + 2 + MAP_OFFSET, TILE_BOTTOM_WALL);
    MapGridSetMetatileEntryAt(sEntranceX + 1 + MAP_OFFSET, sEntranceY + 2 + MAP_OFFSET, TILE_BOTTOM_WALL);

    // Exit tile (ladder)
    MapGridSetMetatileEntryAt(sExitX + MAP_OFFSET, sExitY + MAP_OFFSET, TILE_EXIT);
}

static void SetupWarps1F(void)
{
    const struct MapEvents *origEvents = gMapHeader.events;
    s32 i;

    sMtMoonEvents = *origEvents;
    for (i = 0; i < origEvents->warpCount && i < 4; i++)
        sMtMoonWarps[i] = origEvents->warps[i];

    // Warp 0: exit ladder → Route 4 east
    sMtMoonWarps[0].x = sExitX;
    sMtMoonWarps[0].y = sExitY;
    sMtMoonWarps[0].elevation = 3;
    sMtMoonWarps[0].warpId = 1;
    sMtMoonWarps[0].mapNum = MAP_ROUTE4 & 0xFF;
    sMtMoonWarps[0].mapGroup = MAP_ROUTE4 >> 8;

    sMtMoonWarps[1].x = 0;
    sMtMoonWarps[1].y = 0;
    sMtMoonWarps[2].x = 0;
    sMtMoonWarps[2].y = 0;

    // Warp 3: entrance (player arrives from Route 4 west)
    sMtMoonWarps[3].x = sEntranceX;
    sMtMoonWarps[3].y = sEntranceY;

    sMtMoonEvents.warps = sMtMoonWarps;
    sMtMoonEvents.warpCount = 4;
    sMtMoonEvents.bgEventCount = 0; // Remove signs/hidden items from ROM data
    gMapHeader.events = &sMtMoonEvents;
}

static void SetupWarpsB1F(void)
{
    const struct MapEvents *origEvents = gMapHeader.events;
    s32 i;

    sMtMoonEvents = *origEvents;
    for (i = 0; i < origEvents->warpCount && i < 8; i++)
        sMtMoonWarps[i] = origEvents->warps[i];

    // Move warp 7 to entrance position (player will be teleported there)
    sMtMoonWarps[7].x = sEntranceX;
    sMtMoonWarps[7].y = sEntranceY;

    // Warp 0: exit ladder → Route 4 west (entrance/pokecenter side)
    sMtMoonWarps[0].x = sExitX;
    sMtMoonWarps[0].y = sExitY;
    sMtMoonWarps[0].elevation = 3;
    sMtMoonWarps[0].warpId = 0;
    sMtMoonWarps[0].mapNum = MAP_ROUTE4 & 0xFF;
    sMtMoonWarps[0].mapGroup = MAP_ROUTE4 >> 8;

    for (i = 1; i < 7; i++)
    {
        sMtMoonWarps[i].x = 0;
        sMtMoonWarps[i].y = 0;
    }

    sMtMoonEvents.warps = sMtMoonWarps;
    sMtMoonEvents.bgEventCount = 0; // Remove hidden items/signs from ROM data
    gMapHeader.events = &sMtMoonEvents;
}

// Check if an NPC's full movement area is walkable floor
static bool8 IsMovementAreaClear(s16 cx, s16 cy, s16 rangeX, s16 rangeY)
{
    s32 x, y;
    for (y = cy - rangeY; y <= cy + rangeY; y++)
        for (x = cx - rangeX; x <= cx + rangeX; x++)
            if (!IsFloor(x, y))
                return FALSE;
    return TRUE;
}

// Check if position is reachable from entrance using the existing grid
static bool8 IsTileReachable(s16 tx, s16 ty)
{
    return FloodFillReaches(sEntranceX, sEntranceY, tx, ty);
}

static void RelocateNPCs(void)
{
    s32 i, j;
    u8 objectCount = gMapHeader.events->objectEventCount;
    struct ObjectEventTemplate *templates = gSaveBlock1Ptr->objectEventTemplates;
    u8 itemCount = 0;

    for (i = 0; i < objectCount; i++)
    {
        s32 attempts = 0;
        s16 x, y;
        bool8 overlap;
        bool8 isItemBall = (templates[i].graphicsId == OBJ_EVENT_GFX_ITEM_BALL);
        s16 moveRangeX, moveRangeY;
        u8 movType = templates[i].objUnion.normal.movementType;

        // Skip excess item balls
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

        // Clamp movement ranges to fit in the cave and change
        // sequence walkers to simple wanderers
        if (movType == MOVEMENT_TYPE_WALK_SEQUENCE_LEFT_DOWN_RIGHT_UP
            || movType == MOVEMENT_TYPE_WANDER_AROUND
            || movType == MOVEMENT_TYPE_WANDER_AROUND_SLOWER)
        {
            templates[i].objUnion.normal.movementType = MOVEMENT_TYPE_WANDER_AROUND;
            templates[i].objUnion.normal.movementRangeX = 1;
            templates[i].objUnion.normal.movementRangeY = 1;
        }

        moveRangeX = templates[i].objUnion.normal.movementRangeX;
        moveRangeY = templates[i].objUnion.normal.movementRangeY;
        // Stationary NPCs still need range 0
        if (movType == MOVEMENT_TYPE_FACE_DOWN
            || movType == MOVEMENT_TYPE_FACE_DOWN_AND_LEFT)
        {
            moveRangeX = 0;
            moveRangeY = 0;
        }

        do
        {
            x = (Random() % (CAVE_W - 8)) + 4;
            y = (Random() % (CAVE_H - 10)) + 5;
            attempts++;

            overlap = FALSE;
            if (!IsFloor(x, y))
            {
                overlap = TRUE;
                continue;
            }
            // Don't place near entrance/exit
            if ((x >= sEntranceX - 2 && x <= sEntranceX + 2 && y >= sEntranceY - 2 && y <= sEntranceY + 2)
                || (x >= sExitX - 2 && x <= sExitX + 2 && y >= sExitY - 2 && y <= sExitY + 2))
                overlap = TRUE;
            // Don't overlap or be adjacent to other NPCs
            for (j = 0; j < i && !overlap; j++)
                if (abs(templates[j].x - x) <= 2 && abs(templates[j].y - y) <= 2)
                    overlap = TRUE;
            // Ensure entire movement range is walkable floor
            if (!overlap && !IsMovementAreaClear(x, y, moveRangeX, moveRangeY))
                overlap = TRUE;
            // Must be in an open area (not blocking a corridor)
            if (!overlap && (8 - CountWallNeighbors(x, y)) < 4)
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
            // Couldn't place - hide at unreachable position
            templates[i].x = 0;
            templates[i].y = 0;
        }
    }
}

static void GenerateCave(void)
{
    s32 pass, x, y;

    CpuFastFill(0, sCaveGridPacked, sizeof(sCaveGridPacked));
    InitRandomGrid();

    for (pass = 0; pass < 4; pass++)
    {
        SmoothGrid();
        ReinforceKeyAreas();
    }

    // Cleanup: remove isolated floor cells
    for (y = 3; y < CAVE_H - 3; y++)
        for (x = 3; x < CAVE_W - 3; x++)
            if (GRID(x, y) == 1 && (8 - CountWallNeighbors(x, y)) < 3)
                GRID_SET(x, y, 0);
    ReinforceKeyAreas();

    // Remove 1-cell-wide wall strands that otherwise render as jagged
    // cliff fingers over the floor.
    RemoveThinWalls();
    ReinforceKeyAreas();

    // Fill 2x2 diagonal wall pinches so wall corners are clean blocks
    // instead of single-point serrations.
    FillDiagonalCorners();
    ReinforceKeyAreas();

    // Smooth wall edges: remove single-cell convex-corner bumps and
    // fill single-cell notches so the wall-floor boundary reads as
    // rounded axis-aligned edges instead of jagged stair-steps.
    SmoothWallEdges();
    ReinforceKeyAreas();

    // Ensure connectivity
    if (!FloodFillReaches(sEntranceX, sEntranceY, sExitX, sExitY))
        CarvePath(sEntranceX, sEntranceY, sExitX, sExitY);

    // Final check after NPC placement: verify exit still reachable
    WriteGridToVMap();
}

void GenerateMtMoonCave(void)
{
    bool8 isB1F = (gMapHeader.mapLayoutId == LAYOUT_MT_MOON_B1F);

    // Same cave layout for both sides
    sEntranceX = 18;
    sEntranceY = 36;
    sExitX = 24;
    sExitY = 3;

    GenerateCave();

    if (isB1F)
    {
        // Override player position to the entrance spot
        // (ROM warp 7 is at 45,4 but we move the player here instead)
        gSaveBlock1Ptr->pos.x = sEntranceX;
        gSaveBlock1Ptr->pos.y = sEntranceY;
        SetupWarpsB1F();
    }
    else
    {
        SetupWarps1F();
    }

    RelocateNPCs();

    // Post-NPC connectivity check
    if (!FloodFillReaches(sEntranceX, sEntranceY, sExitX, sExitY))
    {
        CarvePath(sEntranceX, sEntranceY, sExitX, sExitY);
        WriteGridToVMap();
    }
}

void RelocateMtMoonNPCs(void)
{
}
