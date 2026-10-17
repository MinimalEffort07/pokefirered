#ifndef GUARD_MT_MOON_GEN_STYLE_H
#define GUARD_MT_MOON_GEN_STYLE_H

/*
 * The interface between the Mt. Moon generator core (src/mt_moon_gen.c) and
 * the floor-style modules (src/mt_moon_style_*.c).
 *
 * WHO DOES WHAT
 *
 * The core does everything that is the same on every floor: the per-visit
 * seed and the private random number generator, the cave grid, writing the
 * finished map into the game's map buffer, the warps, placing NPCs, and the
 * Continue safety net.
 *
 * A style decides what a floor looks like, through two functions:
 *
 *   buildShape  starts from an all-wall grid and carves out the floor,
 *               using MtMoonGen_Random() for every random choice. It must
 *               leave the arrival and exit tiles as floor and connected.
 *   pickTile    returns the map cell value to draw for one grid cell. It
 *               only looks at the grid (and the cell's position), so the
 *               same grid always draws the same way.
 *
 * Today one style, the cellular-automata cave in
 * src/mt_moon_style_cellular.c, provides every generated floor.
 *
 * GIVING A FLOOR ITS OWN STYLE
 *
 * Each generated floor has one style object named after it, declared at the
 * bottom of this file (gMtMoonStyle1F, gMtMoonStyleB1F, ...). The core uses
 * that name; whichever source file DEFINES it decides the floor's look. To
 * give 1F its own style:
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
 * size of the floor's layout (B1F is 49 x 40; 1F and B2F are 48 x 40). Each
 * cell holds one of the MT_MOON_CELL_* values. Coordinates are map tile
 * coordinates: (0,0) is the top-left tile, x grows east, y grows south.
 * Reading outside the grid returns MT_MOON_CELL_WALL.
 */

#define MT_MOON_CELL_WALL    0
#define MT_MOON_CELL_FLOOR   1
#define MT_MOON_CELL_VISITED 2  /* floor already reached by a flood fill */

/* The floor being generated. Coordinates are map tile coordinates. */
struct MtMoonCave
{
    s16 width;      /* grid size, from the floor's layout */
    s16 height;
    s16 entranceX;  /* where the player arrives (always floor) */
    s16 entranceY;
    s16 exitX;      /* the exit ladder (always floor) */
    s16 exitY;
};

struct MtMoonStyle
{
    void (*buildShape)(const struct MtMoonCave *cave);
    u16 (*pickTile)(const struct MtMoonCave *cave, s32 x, s32 y);
};

/* --- Grid access (provided by the core) ---------------------------------- */

u8 MtMoonGen_GetCell(s32 x, s32 y);
void MtMoonGen_SetCell(s32 x, s32 y, u8 value);

/* TRUE for MT_MOON_CELL_FLOOR and MT_MOON_CELL_VISITED; FALSE off the grid. */
bool8 MtMoonGen_IsFloor(s32 x, s32 y);

/* How many of the 8 cells around (x, y) are wall; off-grid counts as wall. */
u8 MtMoonGen_CountWallNeighbors(s32 x, s32 y);

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

/* --- One style object per generated floor --------------------------------- */

extern const struct MtMoonStyle gMtMoonStyle1F;
extern const struct MtMoonStyle gMtMoonStyleB1F;

#endif /* GUARD_MT_MOON_GEN_STYLE_H */
