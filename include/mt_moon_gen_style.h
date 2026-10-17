#ifndef GUARD_MT_MOON_GEN_STYLE_H
#define GUARD_MT_MOON_GEN_STYLE_H

/*
 * The interface between the Mt. Moon generator core (src/mt_moon_gen.c) and
 * the floor-style modules (src/mt_moon_style_*.c).
 *
 * WHO DOES WHAT
 *
 * The core does everything that is the same on every floor: the per-visit
 * seed and the private random number generator, the cave grid, checking
 * the ladders, writing the finished map into the game's map buffer, the
 * warps, placing NPCs, and the Continue safety net.
 *
 * A style decides what a floor looks like, through two functions:
 *
 *   buildShape  starts from an all-wall grid, places the floor's ladders
 *               and carves out the floor around them (see LADDERS below),
 *               using MtMoonGen_Random() for every random choice.
 *   pickTile    returns the map cell value to draw for one grid cell,
 *               ladder cells included (MtMoonGen_GetLadderTile() gives the
 *               vanilla ladder tiles). It only looks at the grid, the
 *               ladders and the cell's position, so the same grid always
 *               draws the same way.
 *
 * Today one style, the cellular-automata cave in
 * src/mt_moon_style_cellular.c, provides every generated floor.
 *
 * GIVING A FLOOR ITS OWN STYLE
 *
 * Each generated floor has one style object named after it, declared at the
 * bottom of this file (gMtMoonStyle1F, gMtMoonStyleB1F, gMtMoonStyleB2F).
 * The core uses that name; whichever source file DEFINES it decides the
 * floor's look. To give 1F its own style:
 *
 *   1. Write src/mt_moon_style_1f.c (it is compiled automatically) and define
 *      `const struct MtMoonStyle gMtMoonStyle1F = { Build1FShape, Pick1FTile };`
 *   2. Delete the one gMtMoonStyle1F line from the file that defined it
 *      before (today, src/mt_moon_style_cellular.c).
 *
 * The linker connects the core to the one definition, and complains if
 * there are two or none, so no table in the core needs editing.
 *
 * THE GRID
 *
 * The generator works on an abstract grid with one cell per map tile, the
 * size of the floor's layout (1F and B2F are 48 x 40, B1F is 49 x 40). Each
 * cell holds one of the MT_MOON_CELL_* values. Coordinates are map tile
 * coordinates: (0,0) is the top-left tile, x grows east, y grows south.
 * Reading outside the grid returns MT_MOON_CELL_WALL.
 *
 * LADDERS
 *
 * Mt. Moon is a chain of generated floors,
 *
 *     Route 4 west door <-> 1F <-> B1F <-> B2F <-> Route 4 east door,
 *
 * so every floor has exactly two ladders (struct MtMoonCave):
 *
 *   entrance  the warp toward Route 4 WEST: 1F's doorway, the up-ladders
 *             of B1F and B2F.
 *   exit      the warp toward Route 4 EAST: the down-ladders of 1F and B1F,
 *             and B2F's up-ladder out to Route 4.
 *
 * Which kind each one is comes from the core's floor table. The kinds follow
 * vanilla Mt. Moon; MtMoonGen_IsLadderSite() checks a ladder's FOOTPRINT,
 * the grid cells around it that make it look right:
 *
 *   MT_MOON_LADDER_DOOR  a doorway in the south wall (1F's entrance from
 *       Route 4). Floor: the door cell, the cells west and east of it and
 *       the cell north of it. Wall: the 3 x 2 block below (the doorframe).
 *           . F .        F = floor, W = wall, D = the door
 *           F D F
 *           W W W
 *           W W W
 *   MT_MOON_LADDER_DOWN  a hole in open floor: the hole and all 8 cells
 *       around it are floor.
 *   MT_MOON_LADDER_UP    a ladder at the foot of a north wall. Floor: the
 *       ladder cell and the cell south of it (where the player steps off).
 *       Wall: the 3 cells above the ladder (the ladder's top is drawn into
 *       the middle one) and the cell above that.
 *           . W .
 *           W W W
 *           . U .        U = the ladder
 *           . F .
 *
 * THE LADDER CONTRACT (what buildShape must do)
 *
 *   1. On entry, cave->entrance and cave->exit hold each ladder's kind. A
 *      ladder with `fixed` TRUE already has its position (1F's door is at
 *      vanilla's (18,37)) and must stay there. The others are
 *      MT_MOON_LADDER_UNPLACED: the style chooses where they go, any way it
 *      likes (at random, at the foot of a room's wall, ...).
 *   2. On return, every ladder:
 *        - has a position,
 *        - has its footprint carved into the grid (MtMoonGen_IsLadderSite
 *          holds; MtMoonGen_MarkLadderSite() writes a footprint),
 *        - is at least MT_MOON_LADDER_MIN_DISTANCE from the other ladder,
 *          counted as steps east-west plus steps north-south,
 *      and the two ladders are connected through floor cells.
 *   3. pickTile draws the ladders, usually by returning
 *      MtMoonGen_GetLadderTile() first. A style may draw its own vanilla
 *      variants instead (e.g. B1F's hole 0x2af), keeping the same cells
 *      walkable and solid.
 *
 * The core checks rule 2 after buildShape. A shape that breaks it is a bug
 * in the style: the core throws the shape away and calls buildShape again
 * on a fresh all-wall grid (the random numbers have moved on, so it comes
 * out different), up to 4 times, then gives up and leaves the floor's
 * vanilla map in place.
 */

#define MT_MOON_CELL_WALL    0
#define MT_MOON_CELL_FLOOR   1
#define MT_MOON_CELL_VISITED 2  /* floor already reached by a flood fill */

/* Ladder kinds (see LADDERS above). */
#define MT_MOON_LADDER_DOOR 0
#define MT_MOON_LADDER_DOWN 1
#define MT_MOON_LADDER_UP   2

/* Minimum distance between a floor's two ladders, in steps (east-west steps
 * plus north-south steps). */
#define MT_MOON_LADDER_MIN_DISTANCE 24

/* The x and y of a ladder the style hasn't placed yet. */
#define MT_MOON_LADDER_UNPLACED 0xFF

/* One of a floor's two ladders (or 1F's door). 4 bytes. */
struct MtMoonLadder
{
    u8 x;        /* map tile of the warp, or MT_MOON_LADDER_UNPLACED */
    u8 y;
    u8 kind;     /* MT_MOON_LADDER_* */
    bool8 fixed; /* TRUE: the position comes from the core and can't move */
};

/* The floor being generated. 12 bytes (see the IWRAM note in the core). */
struct MtMoonCave
{
    s16 width;                    /* grid size, from the floor's layout */
    s16 height;
    struct MtMoonLadder entrance; /* toward Route 4 west */
    struct MtMoonLadder exit;     /* toward Route 4 east */
};

struct MtMoonStyle
{
    void (*buildShape)(struct MtMoonCave *cave);
    u16 (*pickTile)(const struct MtMoonCave *cave, s32 x, s32 y);
};

/* --- Grid access (provided by the core) ---------------------------------- */

u8 MtMoonGen_GetCell(s32 x, s32 y);
void MtMoonGen_SetCell(s32 x, s32 y, u8 value);

/* TRUE for MT_MOON_CELL_FLOOR and MT_MOON_CELL_VISITED; FALSE off the grid. */
bool8 MtMoonGen_IsFloor(s32 x, s32 y);

/* How many of the 8 cells around (x, y) are wall; off-grid counts as wall. */
u8 MtMoonGen_CountWallNeighbors(s32 x, s32 y);

/*
 * Which of the 8 cells around (x, y) are floor, as one byte with a bit per
 * neighbour (off-grid counts as wall). Reading all 8 in one call is much
 * cheaper than 8 MtMoonGen_IsFloor() calls, which matters for load time:
 * a style's passes look at neighbours tens of thousands of times.
 * Example: (mask & (MT_MOON_N | MT_MOON_S)) == (MT_MOON_N | MT_MOON_S)
 * means floor both north and south.
 */
#define MT_MOON_N  (1 << 0)
#define MT_MOON_NE (1 << 1)
#define MT_MOON_E  (1 << 2)
#define MT_MOON_SE (1 << 3)
#define MT_MOON_S  (1 << 4)
#define MT_MOON_SW (1 << 5)
#define MT_MOON_W  (1 << 6)
#define MT_MOON_NW (1 << 7)
u8 MtMoonGen_GetFloorMask(s32 x, s32 y);

/*
 * Fast reading for a style's whole-grid passes: a pointer to row y of the
 * grid, so that row[x] is the cell at (x, y). Valid for y from -1 to
 * height and x from -1 to width: the grid is stored with a frame of wall
 * cells one cell wide around it, so every cell's 8 neighbours can be read
 * this way without checking the edges. Never read further out than that.
 * The pointer is read-only (write cells with MtMoonGen_SetCell, and the
 * row shows the change at once) and only valid during buildShape and
 * pickTile. A pass over every cell that reads through rows instead of
 * calling MtMoonGen_GetCell() for each neighbour is several times faster,
 * which keeps the floor's load time down (test P9).
 */
const u8 *MtMoonGen_GetRow(s32 y);

/* TRUE if floor connects (fromX, fromY) to (toX, toY) in N/S/E/W steps. */
bool8 MtMoonGen_FloodFillReaches(s32 fromX, s32 fromY, s32 toX, s32 toY);

/*
 * The generator's private random number generator, seeded from the visit's
 * seed. Every random choice during generation must come from here and never
 * from the game's Random(): Random() is also advanced every frame by the
 * VBlank interrupt, so anything drawn from it would differ between the
 * fresh entry and a later Continue.
 */
u16 MtMoonGen_Random(void);

/* A random number in [0, n), from MtMoonGen_Random(). Prefer it to
 * `MtMoonGen_Random() % n`: the GBA's CPU can't divide, so % is slow. */
u16 MtMoonGen_RandomBelow(u16 n);

/* --- Ladders (provided by the core) -------------------------------------- */

/* Steps between two ladders: |x1 - x2| + |y1 - y2|. */
s32 MtMoonGen_LadderDistance(const struct MtMoonLadder *a, const struct MtMoonLadder *b);

/* TRUE if the grid around the (placed) ladder matches its kind's footprint. */
bool8 MtMoonGen_IsLadderSite(const struct MtMoonLadder *ladder);

/* Write the ladder's footprint into the grid (cells off the grid are
 * skipped). Afterwards MtMoonGen_IsLadderSite() holds for it. */
void MtMoonGen_MarkLadderSite(const struct MtMoonLadder *ladder);

/*
 * The vanilla tile for map cell (x, y) if it is part of one of the cave's
 * ladders, else 0 (no map cell value is 0 in Mt. Moon). The tiles are:
 *   door  0x287 (walkable), with the doorframe 0x2c8 0x2c9 0x2ca below it
 *         and the wall 0x2a3 below that;
 *   down  the hole 0x285;
 *   up    the ladder 0x296 (walkable), with its top 0x28f (solid) set into
 *         the wall above.
 */
u16 MtMoonGen_GetLadderTile(const struct MtMoonCave *cave, s32 x, s32 y);

/* --- One style object per generated floor --------------------------------- */

extern const struct MtMoonStyle gMtMoonStyle1F;
extern const struct MtMoonStyle gMtMoonStyleB1F;
extern const struct MtMoonStyle gMtMoonStyleB2F;

#endif /* GUARD_MT_MOON_GEN_STYLE_H */
