#ifndef GUARD_MT_MOON_GEN_STYLE_H
#define GUARD_MT_MOON_GEN_STYLE_H

/*
 * The interface between the Mt. Moon generator core (src/mt_moon_gen.c) and
 * the floor-style modules (src/mt_moon_style_*.c).
 *
 * WHO DOES WHAT
 *
 * The core does everything that is the same on every floor: the per-visit
 * seed and the private random number generator, the cave grid, proposing
 * and checking the ladders, stamping B2F's fossil chamber (a prefab copied
 * from vanilla), filling sealed-off pockets of floor, placing
 * the floor's vanilla content (trainers, NPCs, item balls, hidden items and
 * signs) where the player can reach all of it, writing the finished map
 * into the game's map buffer, the warps, and the Continue safety net.
 *
 * A style decides what a floor looks like, through two functions:
 *
 *   buildShape  starts from an all-wall grid with the floor's two ladders
 *               already proposed, and carves out the floor around them,
 *               moving them first if it likes (see LADDERS below). It uses
 *               MtMoonGen_Random() for every random choice.
 *   pickTile    returns the map cell value to draw for one grid cell,
 *               ladder cells included (MtMoonGen_GetLadderTile() gives the
 *               vanilla ladder tiles). It only looks at the grid, the
 *               ladders, the cell's position and what buildShape left in
 *               the style's work area (if it has one), so the same shape
 *               always draws the same way.
 *
 * A style may also have a say in where the core stamps boulders and
 * signposts, and ask for memory of its own during generation (the optional
 * canStampAt and workBytes in struct MtMoonStyle).
 *
 * 1F has its own style (src/mt_moon_style_1f.c); the other floors use the
 * cellular-automata cave in src/mt_moon_style_cellular.c.
 *
 * GIVING A FLOOR ITS OWN STYLE
 *
 * Each generated floor has one style object named after it, declared at the
 * bottom of this file (gMtMoonStyle1F, gMtMoonStyleB1F, gMtMoonStyleB2F).
 * The core uses that name; whichever source file DEFINES it decides the
 * floor's look. To give B2F its own style, for example:
 *
 *   1. Write src/mt_moon_style_b2f.c (it is compiled automatically) and
 *      define `const struct MtMoonStyle gMtMoonStyleB2F = { BuildB2FShape,
 *      PickB2FTile };` (plus any optional fields; 1F's style sets them).
 *   2. Delete the one gMtMoonStyleB2F line from the file that defined it
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
 * Only WALL, FLOOR and VISITED exist while buildShape runs. The core adds
 * BLOCKED cells afterwards, where it stamps a solid one-tile prefab onto
 * the floor (see ONE-TILE PREFABS below). A style's pickTile counts a
 * BLOCKED cell as floor (it is not WALL), so the tiles around a boulder
 * are drawn as open floor, the way vanilla draws them.
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
 * Each kind also has a FOOT (MtMoonGen_GetLadderFoot), the floor cell in
 * front of it where a corridor should join it: north of the door, the hole
 * itself, south of an up-ladder.
 *
 * THE LADDER CONTRACT (what buildShape must do)
 *
 *   1. On entry, both ladders already have a kind and a position. A ladder
 *      with `fixed` TRUE (1F's door, at vanilla's (18,37)) must stay where
 *      it is. For every other ladder the core has PROPOSED a position:
 *        - random, and at least MT_MOON_LADDER_MIN_DISTANCE from the other
 *          ladder;
 *        - an up-ladder on row 2, right under a 2-row top border, so the
 *          north wall it needs is the border;
 *        - any other kind in the interior, at least 4 cells from the side
 *          and top edges and 6 from the bottom edge, so a 5 x 5 patch of
 *          open floor fits around it inside a 2-cell border (3 at the
 *          bottom).
 *      The style either ACCEPTS a proposal (keeps it and builds its shape
 *      around it) or MOVES the ladder wherever suits its shape (at the foot
 *      of a room's wall, ...). The cellular style accepts every proposal.
 *   2. On return, every ladder:
 *        - has its footprint carved into the grid (MtMoonGen_IsLadderSite
 *          holds; MtMoonGen_MarkLadderSite() writes a footprint),
 *        - is at least MT_MOON_LADDER_MIN_DISTANCE from the other ladder,
 *          counted as steps east-west plus steps north-south,
 *      and every floor cell of both footprints is connected to every other
 *      through floor cells WITHOUT stepping on either ladder cell. (Stepping
 *      on a ladder leaves the floor, so the player can never walk through
 *      one. For example the cells west and east of 1F's door must connect
 *      to the cell north of it around the door, not across it.)
 *   3. pickTile draws the ladders, usually by returning
 *      MtMoonGen_GetLadderTile() first. A style may draw its own vanilla
 *      variants instead, keeping the same cells walkable and solid: for
 *      example B1F's hole would be the map cell value 0x32af (metatile
 *      0x2af, walkable, elevation 3; see MAP CELL VALUES below).
 *
 * The core checks rule 2 after buildShape. A shape that breaks it is a bug
 * in the style: the core throws the shape away and calls buildShape again
 * on a fresh all-wall grid with fresh proposals (the random numbers have
 * moved on, so it comes out different), up to 4 times, then gives up and
 * leaves the floor's vanilla map in place.
 *
 * THE FOSSIL CHAMBER (B2F only)
 *
 * Vanilla B2F's fossil chamber is a PREFAB: a 13 x 9 block of vanilla
 * B2F's own tiles (x 8..20, y 7..15 of data/layouts/MtMoon_B2F/map.bin),
 * copied tile for tile onto the generated B2F. Super Nerd Miguel, the Dome
 * and Helix Fossils and Miguel's battle trigger tile move with it, keeping
 * their vanilla places inside it. They have to: after the player takes one
 * fossil, Miguel's script walks him to the other with fixed steps
 * (walk_right, walk_up x3 to the Helix Fossil; walk_up x3 to the Dome
 * Fossil). Its collision, relative to the chamber's top-left tile:
 *
 *        dx  0 1 2 3 4 5 6 7 8 9 10 11 12
 *    dy -1   ? ? ? ? ? W W ? ? ? ?  ?  ?    the 2 cells behind the fossils
 *        0   W W W W W D H W W W W  W  W    D, H  the Dome and Helix Fossils,
 *        1   W . . . . . . . . . .  .  W          on walkable tiles in a gap
 *        2   W . . . . . . . . . .  .  W          of the room's north wall
 *        3   W . . . . . . . . . .  .  W
 *        4   W . . . . M T . . . .  .  W    M  Miguel, T  his trigger tile
 *        5   W W W W W S S W W W .  .  W    S  stairs (walkable)
 *        6   W . . . . F . . . W .  .  W    F  the chamber's FOOT
 *        7   . . . . . . . . . W .  .  W
 *        8   . . . . . . . . . W W  W  W    . walkable, W solid, ? the style's
 *
 * The room (rows 1-4, with an alcove down the east side) is closed except
 * for its stairs, and Miguel and his trigger stand right inside them, so
 * every way in steps on the trigger: the battle can't be skipped. The two
 * cells behind the fossils (row -1) are always wall, so nobody can reach a
 * fossil from outside either. Below the stairs lies the LANDING (rows 6-8,
 * dx 0-8), open floor whose west side (dx 0, rows 7-8) and south side
 * (row 8) are open: that is where the chamber joins the cave. The foot (5,6)
 * is the landing cell in front of the stairs, the cell a corridor should
 * join (MtMoonGen_GetChamberFoot).
 *
 * The chamber works like the ladders (rules 1-3 above):
 *
 *   1. Before buildShape, on a floor that has a chamber, the core PROPOSES
 *      its position, MtMoonGen_GetChamber()->x and ->y (its top-left tile):
 *      random, with the chamber inside the 2-cell border (3 at the bottom),
 *      the row below it still inside the border (so its landing can open
 *      south), and at least 4 cells (any direction, diagonals included)
 *      from both proposed ladders. The style ACCEPTS it or MOVES the
 *      chamber, keeping those rules (for example to make it one of its
 *      rooms). A style that builds around the chamber may write its cells
 *      into the grid with MtMoonGen_MarkChamberSite() first, the way
 *      MtMoonGen_MarkLadderSite() writes a ladder's footprint. The
 *      cellular style ignores the chamber, which leaves everything to the
 *      core.
 *   2. After buildShape, the core checks the rules of rule 1 against the
 *      final ladders (a broken one throws the shape away, as for broken
 *      ladders) and STAMPS the chamber: its cells become FLOOR or WALL as
 *      the diagram says, whatever the style carved there. Stamping can cut
 *      through the style's floor, so the core then CARVES CORRIDORS (wall
 *      cells become floor, the shortest way) wherever needed so that:
 *        - every floor cell of both ladders' footprints connects to the
 *          entrance WITHOUT going through the chamber (so the chamber is
 *          never the only way across the floor: reachable, not forced);
 *        - the chamber's foot connects to the entrance.
 *      Corridors never touch the chamber, the cells behind the fossils,
 *      a ladder's footprint or the border. A shape where no corridor fits
 *      is thrown away.
 *   3. The core draws the chamber: after pickTile has drawn every cell and
 *      the one-tile prefabs are on top, the chamber's 13 x 9 vanilla tiles
 *      replace whatever was drawn there. pickTile sees the chamber's cells
 *      as plain FLOOR and WALL, so it can draw the tiles around it.
 *
 * Nothing else ever changes the chamber: pocket filling skips it, content
 * placement puts nothing inside it or within 1 cell of it (or of the cells
 * behind the fossils), and repair carving never reaches into it.
 *
 * WHAT THE CORE DOES TO THE SHAPE AFTERWARDS
 *
 *   1. Pockets. Every floor cell the player can't walk to from the
 *      entrance (without stepping on a ladder) becomes wall. A style that
 *      would rather CONNECT a pocket (because filling it would leave a
 *      shape its tiles can't draw) must connect it in buildShape.
 *   2. Content. On B2F, Miguel, the two fossils and Miguel's trigger tile
 *      go to their places in the fossil chamber. Every other trainer, NPC
 *      and item ball is put on open floor, and
 *      every hidden item and sign is stamped onto open floor as a one-tile
 *      prefab (a boulder 0x282, the signpost 0x2cd). Objects that move
 *      (wanderers) count as standing on every tile of their roaming box.
 *      Each spot is chosen so that the floor stays in one connected piece
 *      and the player can stand next to every object, hidden item and
 *      sign, facing it. Nothing goes within 2 tiles of a ladder. A
 *      style with a canStampAt (see ONE-TILE PREFABS) also has a say in
 *      where the boulders and signposts go.
 *   3. Repair. If no spot fits, the core carves open floor (wall cells
 *      become floor): the object's box plus one tile on every side, kept
 *      3+ tiles from the ladders and inside the 2-tile border (3 at the
 *      bottom), always touching existing floor so it joins the cave. It
 *      never removes floor, so the ladders' footprints are untouched. A
 *      style that leaves enough open floor rarely sees this happen. If
 *      even that finds no spot for some
 *      piece, or the core's final reachability check fails, the shape is
 *      thrown away and buildShape is called again, exactly as for broken
 *      ladders (rule 2 above), so nothing is ever dropped or unreachable.
 *   4. Facing. Each stationary trainer is turned toward its longest
 *      straight run of open floor (walls, boulders, signposts and the
 *      tiles objects can stand on all end a run), so its sight line can
 *      catch the player.
 *
 * ONE-TILE PREFABS
 *
 * MtMoonGen_StampTile(x, y, tile) draws map tile (x, y) as a fixed vanilla
 * tile instead of whatever pickTile would draw there. The grid follows the
 * tile's collision bits: a walkable tile makes the cell FLOOR; a solid tile
 * on floor makes it BLOCKED (drawn by the prefab, nobody can walk on it);
 * a solid tile on wall leaves it WALL. The core uses it for hidden items
 * and signs; a style (or a later decoration step) may use it for its own
 * one-tile pieces, once its shape is final. Never stamp a ladder's
 * footprint. A later stamp on the same tile replaces an earlier one.
 *
 * Vanilla Mt. Moon only ever puts a hidden item's boulder or a sign's
 * signpost next to plain cave floor (0x281), and the core always stamps them
 * on a tile whose 8 neighbours are floor. If a style draws other floor tiles
 * too (1F's sand), the core could put a signpost next to one of them, which
 * vanilla never does. Such a style gives itself a canStampAt (struct
 * MtMoonStyle), and the core only stamps a hidden item or sign where it
 * returns TRUE.
 *
 * MAP CELL VALUES
 *
 * pickTile returns, and MtMoonGen_GetLadderTile gives, full map cell values,
 * the 16-bit numbers the game keeps for each tile of the loaded map:
 *     metatile id | (collision << 10) | (elevation << 12)
 * Bits 0-9 pick the 16x16-pixel picture (the metatile), bits 10-11 are the
 * collision (0 = walkable, 1 = solid) and bits 12-15 the elevation (the
 * height layer; Mt. Moon's floor is 3, walls are 0). So 0x3287 is metatile
 * 0x287, walkable, elevation 3, and 0x06c9 is metatile 0x2c9, solid.
 */

#define MT_MOON_CELL_WALL    0
#define MT_MOON_CELL_FLOOR   1
#define MT_MOON_CELL_VISITED 2  /* floor already reached by a flood fill */
#define MT_MOON_CELL_BLOCKED 3  /* floor under a solid one-tile prefab (a
                                 * boulder, a sign): drawn by the prefab,
                                 * not walkable; see ONE-TILE PREFABS. (The
                                 * core also uses it for a moment, inside
                                 * its own steps, to hide the fossil
                                 * chamber's floor from its flood fills; a
                                 * style never sees that.) */

/* Ladder kinds (see LADDERS above). */
#define MT_MOON_LADDER_DOOR 0
#define MT_MOON_LADDER_DOWN 1
#define MT_MOON_LADDER_UP   2

/* Minimum distance between a floor's two ladders, in steps (east-west steps
 * plus north-south steps). */
#define MT_MOON_LADDER_MIN_DISTANCE 24

/* One of a floor's two ladders (or 1F's door). 4 bytes. */
struct MtMoonLadder
{
    u8 x;        /* map tile of the warp */
    u8 y;
    u8 kind;     /* MT_MOON_LADDER_* */
    bool8 fixed; /* TRUE: the floor table fixes the position; it can't move */
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

    /* --- Optional (NULL or 0: not used) ---------------------------------
     *
     * canStampAt  TRUE if the core may stamp a hidden item's boulder or a
     *             sign's signpost on map tile (x, y): pickTile draws plain
     *             cave floor (0x281) on the tiles north, south, west and
     *             east of it (see ONE-TILE PREFABS). The core asks it during
     *             content placement, only about tiles that already follow
     *             all of the core's own rules. It answers from the cave (the
     *             ladders), the position and its own work area, not from the
     *             grid, which is in the middle of content placement. NULL:
     *             wherever the core's own rules allow.
     * workBytes   How many bytes of memory the style wants for itself while
     *             a floor is generated (see MtMoonGen_GetStyleWork). The
     *             grid only says wall or floor, and buildShape, canStampAt
     *             and pickTile are separate calls, so a style that decides
     *             more than that in buildShape (1F's sand) keeps it here.
     *             0: none.
     */
    bool8 (*canStampAt)(const struct MtMoonCave *cave, s32 x, s32 y);
    u16 workBytes;
};

/* --- Grid access (provided by the core) ---------------------------------- */

u8 MtMoonGen_GetCell(s32 x, s32 y);
void MtMoonGen_SetCell(s32 x, s32 y, u8 value);

/* TRUE for every cell that isn't MT_MOON_CELL_WALL (FLOOR, VISITED and
 * BLOCKED); FALSE off the grid. */
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

/* TRUE if the grid around the ladder matches its kind's footprint. */
bool8 MtMoonGen_IsLadderSite(const struct MtMoonLadder *ladder);

/* Write the ladder's footprint into the grid (cells off the grid are
 * skipped). Afterwards MtMoonGen_IsLadderSite() holds for it. */
void MtMoonGen_MarkLadderSite(const struct MtMoonLadder *ladder);

/* The ladder's foot (see LADDERS above): the floor cell where a corridor
 * should join it. */
void MtMoonGen_GetLadderFoot(const struct MtMoonLadder *ladder, s32 *x, s32 *y);

/*
 * The vanilla map cell value (see MAP CELL VALUES above) for map tile
 * (x, y) if a ladder's own tiles cover it, else 0 (no Mt. Moon map cell
 * value is 0). Footprint cells without a vanilla tile of their own (the
 * floor around a hole, the wall beside a ladder's top) also give 0: the
 * style draws its usual floor or wall there. The tiles are:
 *   door  0x3287 (metatile 0x287, walkable), with the doorframe 0x06c8,
 *         0x06c9, 0x06ca (solid) below it and the wall 0x06a3 below that;
 *   down  the hole 0x3285 (walkable);
 *   up    the ladder 0x3296 (walkable), with its top 0x068f (solid) set
 *         into the wall above.
 */
u16 MtMoonGen_GetLadderTile(const struct MtMoonCave *cave, s32 x, s32 y);

/* --- The style's work area (provided by the core) ------------------------ */

/*
 * The style's own memory for the floor being generated: workBytes bytes
 * (struct MtMoonStyle), starting on a multiple of 4 bytes. It is borrowed
 * from the heap together with the grid, so it costs no permanent RAM, and is
 * only valid from buildShape until the floor's last pickTile. It is zeroed
 * once per floor, not before each shape (see THE LADDER CONTRACT: the core
 * may call buildShape again), so buildShape sets up everything it keeps
 * there. Only meaningful for a style that asked for workBytes.
 */
void *MtMoonGen_GetStyleWork(void);

/* --- One-tile prefabs (provided by the core) ------------------------------ */

/*
 * Draw map tile (x, y) as `tile`, a full map cell value (see MAP CELL
 * VALUES), instead of what the style's pickTile would draw, and update the
 * grid to match the tile's collision (see ONE-TILE PREFABS above). Returns
 * FALSE, changing nothing, if (x, y) is off the grid or the core's list of
 * stamps (room for 128 per floor) is full.
 */
bool8 MtMoonGen_StampTile(s32 x, s32 y, u16 tile);

/* --- The fossil chamber (provided by the core) ---------------------------- */

/* The chamber's size in map tiles (see THE FOSSIL CHAMBER above). */
#define MT_MOON_CHAMBER_WIDTH  13
#define MT_MOON_CHAMBER_HEIGHT 9

/*
 * Where the floor's fossil chamber goes. It isn't part of struct
 * MtMoonCave because the core keeps that struct in IWRAM, which has no
 * room to spare; the core keeps this one with its other work areas.
 */
struct MtMoonChamber
{
    u8 x;  /* map tile of the chamber's top-left corner */
    u8 y;
};

/* The floor's fossil chamber, or NULL on a floor that has none (only B2F
 * has one). Valid during buildShape and pickTile; buildShape may change
 * x and y (rule 1 of THE FOSSIL CHAMBER). */
struct MtMoonChamber *MtMoonGen_GetChamber(void);

/* Write the chamber's cells, and the 2 wall cells behind its fossils, into
 * the grid at its current position (cells off the grid are skipped). Does
 * nothing on a floor without a chamber. */
void MtMoonGen_MarkChamberSite(void);

/* The chamber's foot: the landing cell in front of its stairs, where a
 * corridor should join it. (-1, -1) on a floor without a chamber. */
void MtMoonGen_GetChamberFoot(s32 *x, s32 *y);

/* --- One style object per generated floor --------------------------------- */

extern const struct MtMoonStyle gMtMoonStyle1F;
extern const struct MtMoonStyle gMtMoonStyleB1F;
extern const struct MtMoonStyle gMtMoonStyleB2F;

#endif /* GUARD_MT_MOON_GEN_STYLE_H */
