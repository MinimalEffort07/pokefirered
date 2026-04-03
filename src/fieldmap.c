/*
 * =Pokemon FireRed Field Map / Metatile System=
 *
 * This file manages the game's 2D tile-based map system. It handles:
 *   - Loading map layout data (metatile grids) into a virtual map buffer
 *   - Connecting adjacent maps seamlessly (the "connection" system)
 *   - Reading metatile properties (collision, behavior, elevation, terrain)
 *   - Saving/restoring map state across map transitions
 *   - Loading tileset graphics and palettes into VRAM
 *   - Camera movement and map boundary detection
 *
 * GBA CONTEXT - THE METATILE SYSTEM:
 * The GBA renders backgrounds using 8x8 pixel tiles stored in VRAM. However,
 * building a map from individual 8x8 tiles would be extremely tedious and
 * memory-intensive. Instead, Pokemon uses a two-level tile system:
 *
 * 1. TILES (8x8 pixels): The atomic graphical unit, stored in VRAM tilesets.
 * 2. METATILES (16x16 pixels): Groups of four 8x8 tiles arranged in a 2x2 grid.
 *    Each metatile also has associated attributes (collision, behavior, etc.)
 *    stored in metatile_attributes.bin files.
 *
 * The map layout is a 2D grid of metatile IDs. Each entry in the grid is a u16
 * that encodes three things:
 *   - Bits 0-9:  Metatile ID (which 16x16 tile to draw, max 1024 metatiles)
 *   - Bit 10:    Collision flag (1 = impassable, 0 = walkable)
 *   - Bits 12-15: Elevation/height (for bridges, ledges, etc.)
 *
 * VIRTUAL MAP (VMap):
 * The game doesn't just load the current map -- it creates a larger "virtual
 * map" buffer (gBackupMapData) that includes the current map PLUS a border
 * region (MAP_OFFSET tiles on each side). This border region is filled with
 * data from connected neighboring maps, enabling seamless scrolling between
 * maps without loading screens. The VMap is the authoritative data source
 * for all map queries (collision checks, metatile lookups, etc.)
 *
 * CONNECTIONS:
 * Maps can be connected in four cardinal directions. When the camera reaches
 * the edge of the current map, the engine detects which connected map should
 * appear and loads its data into the VMap border region. This creates the
 * illusion of one continuous world even though the game data is split into
 * many individual map files.
 *
 * TILESETS:
 * Each map uses exactly two tilesets: a primary and a secondary. The primary
 * tileset contains common tiles (grass, water, trees) shared across many maps.
 * The secondary tileset contains map-specific tiles (unique buildings, etc.)
 * Metatile IDs 0-511 reference the primary tileset; IDs 512+ reference the
 * secondary tileset.
 */
#include "global.h"
#include "gflib.h"
#include "overworld.h"
#include "script.h"
#include "new_menu_helpers.h"
#include "quest_log.h"
#include "fieldmap.h"
#include "mt_moon_gen.h"
#include "constants/layouts.h"

/* Bitfield tracking which cardinal directions have connected maps.
 * Used to determine whether to allow camera movement past map edges. */
struct ConnectionFlags
{
    u8 south:1;
    u8 north:1;
    u8 west:1;
    u8 east:1;
};

/* VMap (Virtual Map) - the combined map buffer that holds the current map plus
 * border data from connected maps. All map queries go through this structure. */
COMMON_DATA struct BackupMapLayout VMap = {0};

/* The actual metatile data buffer for the virtual map. VIRTUAL_MAP_SIZE is large
 * enough to hold the biggest possible map plus its border regions. Stored in
 * EWRAM (External Work RAM, 256KB) because it's too large for IWRAM (32KB). */
EWRAM_DATA u16 gBackupMapData[VIRTUAL_MAP_SIZE] = {};

/* The current map's header, containing pointers to layout data, events,
 * scripts, connections, and tileset references. Loaded when entering a map. */
EWRAM_DATA struct MapHeader gMapHeader = {};

/* Camera state tracking. When the camera moves to a connected map, the
 * x/y fields record the position delta for smooth scrolling transitions. */
EWRAM_DATA struct Camera gCamera = {};

/* Tracks which directions have valid map connections from the current map. */
static EWRAM_DATA struct ConnectionFlags gMapConnectionFlags = {};

/* Global palette tint mode, used by the Quest Log system to apply visual
 * effects (grayscale, sepia) to field map palettes during playback. */
EWRAM_DATA u8 gGlobalFieldTintMode = QL_TINT_NONE;

static const struct ConnectionFlags sDummyConnectionFlags = {};

static void InitMapLayoutData(struct MapHeader *);
static void InitBackupMapLayoutData(const u16 *, u16, u16);
static void InitBackupMapLayoutConnections(struct MapHeader *);
static void FillSouthConnection(struct MapHeader const *, struct MapHeader const *, s32);
static void FillNorthConnection(struct MapHeader const *, struct MapHeader const *, s32);
static void FillWestConnection(struct MapHeader const *, struct MapHeader const *, s32);
static void FillEastConnection(struct MapHeader const *, struct MapHeader const *, s32);
static void LoadSavedMapView(void);
static const struct MapConnection *GetIncomingConnection(u8, s32, s32);
static bool8 IsPosInIncomingConnectingMap(u8, s32, s32, const struct MapConnection *);
static bool8 IsCoordInIncomingConnectingMap(s32, s32, s32, s32);
static u32 GetAttributeByMetatileIdAndMapLayout(const struct MapLayout *, u16, u8);

#define GetBorderBlockAt(x, y) ({                                                                 \
    u16 block;                                                                                    \
    s32 xprime;                                                                                   \
    s32 yprime;                                                                                   \
                                                                                                  \
    const struct MapLayout *mapLayout = gMapHeader.mapLayout;                                     \
                                                                                                  \
    xprime = x - MAP_OFFSET;                                                                      \
    xprime += 8 * mapLayout->borderWidth;                                                         \
    xprime %= mapLayout->borderWidth;                                                             \
                                                                                                  \
    yprime = y - MAP_OFFSET;                                                                      \
    yprime += 8 * mapLayout->borderHeight;                                                        \
    yprime %= mapLayout->borderHeight;                                                            \
                                                                                                  \
    block = mapLayout->border[xprime + yprime * mapLayout->borderWidth] | MAPGRID_COLLISION_MASK; \
})

#define AreCoordsWithinMapGridBounds(x, y) (x >= 0 && x < VMap.Xsize && y >= 0 && y < VMap.Ysize)

#define GetMapGridBlockAt(x, y) (AreCoordsWithinMapGridBounds(x, y) ? VMap.map[x + VMap.Xsize * y] : GetBorderBlockAt(x, y))

/* Masks and shifts for extracting individual fields from metatile attribute words.
 * Each metatile has a 32-bit attribute word packed with multiple fields:
 *   Bits 0-8:   BEHAVIOR (e.g., "tall grass", "water", "warp tile", "ledge")
 *   Bits 9-13:  TERRAIN type (for encounter determination)
 *   Bits 14-17: Attribute 2 (context-dependent)
 *   Bits 18-23: Attribute 3 (context-dependent)
 *   Bits 24-26: ENCOUNTER_TYPE (grass, water, etc.)
 *   Bits 27-28: Attribute 5 (context-dependent)
 *   Bits 29-30: LAYER_TYPE (controls which BG layer the metatile renders on)
 *   Bit 31:     Attribute 7 (context-dependent)
 * This format is stored in each tileset's metatile_attributes.bin file. */
static const u32 sMetatileAttrMasks[METATILE_ATTRIBUTE_COUNT] = {
    [METATILE_ATTRIBUTE_BEHAVIOR]       = 0x000001ff, // Bits 0-8
    [METATILE_ATTRIBUTE_TERRAIN]        = 0x00003e00, // Bits 9-13
    [METATILE_ATTRIBUTE_2]              = 0x0003c000, // Bits 14-17
    [METATILE_ATTRIBUTE_3]              = 0x00fc0000, // Bits 18-23
    [METATILE_ATTRIBUTE_ENCOUNTER_TYPE] = 0x07000000, // Bits 24-26
    [METATILE_ATTRIBUTE_5]              = 0x18000000, // Bits 27-28
    [METATILE_ATTRIBUTE_LAYER_TYPE]     = 0x60000000, // Bits 29-30
    [METATILE_ATTRIBUTE_7]              = 0x80000000  // Bit  31
};

static const u8 sMetatileAttrShifts[METATILE_ATTRIBUTE_COUNT] = {
    [METATILE_ATTRIBUTE_BEHAVIOR]       = 0,
    [METATILE_ATTRIBUTE_TERRAIN]        = 9,
    [METATILE_ATTRIBUTE_2]              = 14,
    [METATILE_ATTRIBUTE_3]              = 18,
    [METATILE_ATTRIBUTE_ENCOUNTER_TYPE] = 24,
    [METATILE_ATTRIBUTE_5]              = 27,
    [METATILE_ATTRIBUTE_LAYER_TYPE]     = 29,
    [METATILE_ATTRIBUTE_7]              = 31
};

/**
 * FUNCTION: GetMapHeaderFromConnection
 *
 * PURPOSE: Given a map connection entry, look up and return the full MapHeader
 *          for the connected map. Map connections store only the group/number
 *          ID pair; this function resolves that to the actual header data.
 */
const struct MapHeader * GetMapHeaderFromConnection(const struct MapConnection * connection)
{
    return Overworld_GetMapHeaderByGroupAndId(connection->mapGroup, connection->mapNum);
}

/**
 * FUNCTION: InitMap
 *
 * PURPOSE: Initialize the map data for a fresh map entry (not loaded from save).
 *          This is called when entering a map for the first time via warping.
 *
 * HOW IT WORKS:
 * 1. Loads the map layout data and connected map data into the VMap buffer.
 * 2. If this is Mt. Moon, runs the procedural cave generator (a custom feature).
 * 3. Runs the map's on-load script (e.g., setting up NPCs, triggers).
 */
void InitMap(void)
{
    InitMapLayoutData(&gMapHeader);
    if (gMapHeader.mapLayoutId == LAYOUT_MT_MOON_1F || gMapHeader.mapLayoutId == LAYOUT_MT_MOON_B1F)
        GenerateMtMoonCave();
    RunOnLoadMapScript();
}

/**
 * FUNCTION: InitMapFromSavedGame
 *
 * PURPOSE: Initialize map data when loading a saved game. Unlike InitMap,
 *          this also restores any dynamic metatile changes the player made
 *          before saving (e.g., moved boulders, opened doors).
 */
void InitMapFromSavedGame(void)
{
    InitMapLayoutData(&gMapHeader);
    /* Regenerate Mt. Moon cave on save reload. Without this, the map reverts
     * to the original ROM layout but NPC positions and the player's saved
     * coordinates still reflect the procedurally generated layout, causing
     * NPCs to be stuck in walls and the player to spawn inside terrain. */
    if (gMapHeader.mapLayoutId == LAYOUT_MT_MOON_1F || gMapHeader.mapLayoutId == LAYOUT_MT_MOON_B1F)
        GenerateMtMoonCave();
    LoadSavedMapView();
    RunOnLoadMapScript();
}

/**
 * FUNCTION: InitMapLayoutData
 *
 * PURPOSE: Set up the Virtual Map (VMap) buffer with the current map's metatile
 *          data plus border data from all connected neighboring maps.
 *
 * HOW IT WORKS:
 * 1. Fills the entire VMap buffer with MAPGRID_UNDEFINED (a sentinel value
 *    meaning "no valid tile here"). Uses CpuFastFill16 for speed.
 * 2. Sets VMap dimensions to the map's width/height PLUS border padding
 *    (MAP_OFFSET_W and MAP_OFFSET_H, typically 15 and 14 tiles).
 * 3. Asserts the VMap fits within VIRTUAL_MAP_SIZE to prevent buffer overflow.
 * 4. Copies the actual map data into the center of the VMap buffer.
 * 5. Fills border regions with data from connected maps.
 *
 * The result is a single large metatile grid where the current map is centered
 * and surrounded by neighboring map data for seamless scrolling.
 */
static void InitMapLayoutData(struct MapHeader * mapHeader)
{
    const struct MapLayout * mapLayout = mapHeader->mapLayout;
    CpuFastFill16(MAPGRID_UNDEFINED, gBackupMapData, sizeof(gBackupMapData));
    VMap.map = gBackupMapData;
    VMap.Xsize = mapLayout->width + MAP_OFFSET_W;
    VMap.Ysize = mapLayout->height + MAP_OFFSET_H;
    AGB_ASSERT_EX(VMap.Xsize * VMap.Ysize <= VIRTUAL_MAP_SIZE, ABSPATH("fieldmap.c"), 158);
    InitBackupMapLayoutData(mapLayout->map, mapLayout->width, mapLayout->height);
    InitBackupMapLayoutConnections(mapHeader);
}

/**
 * FUNCTION: InitBackupMapLayoutData
 *
 * PURPOSE: Copy the raw map layout data into the center of the VMap buffer,
 *          offset by the border padding so connected maps can fill the edges.
 *
 * HOW IT WORKS:
 * The destination starts at VMap.map + (Xsize * 7 + MAP_OFFSET), which skips
 * past the top border rows (7 tiles) and the left border columns (MAP_OFFSET).
 * It then copies one row at a time, advancing the destination pointer by
 * the full VMap width (which includes border padding on both sides).
 */
static void InitBackupMapLayoutData(const u16 *map, u16 width, u16 height)
{
    s32 y;
    u16 *dest = VMap.map;
    dest += VMap.Xsize * 7 + MAP_OFFSET;

    for (y = 0; y < height; y++)
    {
        CpuCopy16(map, dest, width * sizeof(u16));
        dest += width + MAP_OFFSET_W;
        map += width;
    }
}

/**
 * FUNCTION: InitBackupMapLayoutConnections
 *
 * PURPOSE: Fill the border regions of the VMap with metatile data from
 *          connected neighboring maps in all four cardinal directions.
 *
 * HOW IT WORKS:
 * Iterates through all of the current map's connection entries. For each
 * connection direction (north, south, east, west), it calls the appropriate
 * Fill*Connection function to copy a strip of the connected map's layout
 * data into the VMap border. The connection offset field handles alignment
 * between maps of different sizes.
 */
static void InitBackupMapLayoutConnections(struct MapHeader *mapHeader)
{
    s32 count;
    const struct MapConnection *connection;
    s32 i;

    gMapConnectionFlags = sDummyConnectionFlags;

    /*
     * This null pointer check is new to FireRed.  It was kept in
     * Emerald, with the above struct assignment moved to after
     * this check.
     */
    if (mapHeader->connections)
    {
        count = mapHeader->connections->count;
        connection = mapHeader->connections->connections;
        for (i = 0; i < count; i++, connection++)
        {
            struct MapHeader const *cMap = GetMapHeaderFromConnection(connection);
            u32 offset = connection->offset;
            switch (connection->direction)
            {
            case CONNECTION_SOUTH:
                FillSouthConnection(mapHeader, cMap, offset);
                gMapConnectionFlags.south = TRUE;
                break;
            case CONNECTION_NORTH:
                FillNorthConnection(mapHeader, cMap, offset);
                gMapConnectionFlags.north = TRUE;
                break;
            case CONNECTION_WEST:
                FillWestConnection(mapHeader, cMap, offset);
                gMapConnectionFlags.west = TRUE;
                break;
            case CONNECTION_EAST:
                FillEastConnection(mapHeader, cMap, offset);
                gMapConnectionFlags.east = TRUE;
                break;
            }
        }
    }
}

/**
 * FUNCTION: FillConnection
 *
 * PURPOSE: Copy a rectangular region of metatile data from a connected map
 *          into the VMap buffer at the specified position.
 *
 * PARAMETERS:
 * @param x, y                - Destination position in the VMap buffer
 * @param connectedMapHeader  - The neighboring map to copy data from
 * @param x2, y2              - Source position within the connected map's layout
 * @param width, height       - Size of the region to copy (in metatiles)
 */
static void FillConnection(s32 x, s32 y, const struct MapHeader *connectedMapHeader, s32 x2, s32 y2, s32 width, s32 height)
{
    s32 i;
    const u16 *src;
    u16 *dest;
    s32 mapWidth;

    mapWidth = connectedMapHeader->mapLayout->width;
    src = &connectedMapHeader->mapLayout->map[mapWidth * y2 + x2];
    dest = &VMap.map[VMap.Xsize * y + x];

    for (i = 0; i < height; i++)
    {
        CpuCopy16(src, dest, width * 2);
        dest += VMap.Xsize;
        src += mapWidth;
    }
}

static void FillSouthConnection(struct MapHeader const *mapHeader, struct MapHeader const *connectedMapHeader, s32 offset)
{
    s32 x, y;
    s32 x2;
    s32 width;
    s32 cWidth;

    if (connectedMapHeader)
    {
        cWidth = connectedMapHeader->mapLayout->width;
        x = offset + MAP_OFFSET;
        y = mapHeader->mapLayout->height + MAP_OFFSET;
        if (x < 0)
        {
            x2 = -x;
            x += cWidth;
            if (x < VMap.Xsize)
                width = x;
            else
                width = VMap.Xsize;
            x = 0;
        }
        else
        {
            x2 = 0;
            if (x + cWidth < VMap.Xsize)
                width = cWidth;
            else
                width = VMap.Xsize - x;
        }

        FillConnection(
            x, y,
            connectedMapHeader,
            x2, /*y2*/ 0,
            width, /*height*/ MAP_OFFSET);
    }
}

static void FillNorthConnection(struct MapHeader const *mapHeader, struct MapHeader const *connectedMapHeader, s32 offset)
{
    s32 x;
    s32 x2, y2;
    s32 width;
    s32 cWidth, cHeight;

    if (connectedMapHeader)
    {
        cWidth = connectedMapHeader->mapLayout->width;
        cHeight = connectedMapHeader->mapLayout->height;
        x = offset + MAP_OFFSET;
        y2 = cHeight - MAP_OFFSET;
        if (x < 0)
        {
            x2 = -x;
            x += cWidth;
            if (x < VMap.Xsize)
                width = x;
            else
                width = VMap.Xsize;
            x = 0;
        }
        else
        {
            x2 = 0;
            if (x + cWidth < VMap.Xsize)
                width = cWidth;
            else
                width = VMap.Xsize - x;
        }

        FillConnection(
            x, /*y*/ 0,
            connectedMapHeader,
            x2, y2,
            width, /*height*/ MAP_OFFSET);

    }
}

static void FillWestConnection(struct MapHeader const *mapHeader, struct MapHeader const *connectedMapHeader, s32 offset)
{
    s32 y;
    s32 x2, y2;
    s32 height;
    s32 cWidth, cHeight;
    if (connectedMapHeader)
    {
        cWidth = connectedMapHeader->mapLayout->width;
        cHeight = connectedMapHeader->mapLayout->height;
        y = offset + MAP_OFFSET;
        x2 = cWidth - MAP_OFFSET;
        if (y < 0)
        {
            y2 = -y;
            if (y + cHeight < VMap.Ysize)
                height = y + cHeight;
            else
                height = VMap.Ysize;
            y = 0;
        }
        else
        {
            y2 = 0;
            if (y + cHeight < VMap.Ysize)
                height = cHeight;
            else
                height = VMap.Ysize - y;
        }

        FillConnection(
            /*x*/ 0, y,
            connectedMapHeader,
            x2, y2,
            /*width*/ MAP_OFFSET, height);
    }
}

static void FillEastConnection(struct MapHeader const *mapHeader, struct MapHeader const *connectedMapHeader, s32 offset)
{
    s32 x, y;
    s32 y2;
    s32 height;
    s32 cHeight;
    if (connectedMapHeader)
    {
        cHeight = connectedMapHeader->mapLayout->height;
        x = mapHeader->mapLayout->width + MAP_OFFSET;
        y = offset + MAP_OFFSET;
        if (y < 0)
        {
            y2 = -y;
            if (y + cHeight < VMap.Ysize)
                height = y + cHeight;
            else
                height = VMap.Ysize;
            y = 0;
        }
        else
        {
            y2 = 0;
            if (y + cHeight < VMap.Ysize)
                height = cHeight;
            else
                height = VMap.Ysize - y;
        }

        FillConnection(
            x, y,
            connectedMapHeader,
            /*x2*/ 0, y2,
            /*width*/ MAP_OFFSET + 1, height);
    }
}

/**
 * FUNCTION: MapGridGetElevationAt
 *
 * PURPOSE: Get the elevation/height value at a map position. Elevation is used
 *          for features like bridges where the player can walk both above and
 *          below the same tile, and for ledge jumping mechanics.
 *
 * HOW IT WORKS:
 * Reads the metatile entry from the VMap and extracts the elevation bits
 * (bits 12-15) by right-shifting. Returns 0 for undefined grid positions.
 */
u8 MapGridGetElevationAt(s32 x, s32 y)
{
    u16 block = GetMapGridBlockAt(x, y);

    if (block == MAPGRID_UNDEFINED)
        return 0;

    return block >> MAPGRID_ELEVATION_SHIFT;
}

/**
 * FUNCTION: MapGridGetCollisionAt
 *
 * PURPOSE: Check if a map tile is impassable (blocked). This is the primary
 *          collision detection function used by the movement system.
 *
 * HOW IT WORKS:
 * Reads the metatile entry and extracts bit 10 (MAPGRID_COLLISION_MASK).
 * Returns TRUE (1) if the tile is impassable, FALSE (0) if walkable.
 * Undefined tiles (outside the map) are treated as impassable.
 */
u8 MapGridGetCollisionAt(s32 x, s32 y)
{
    u16 block = GetMapGridBlockAt(x, y);

    if (block == MAPGRID_UNDEFINED)
        return TRUE;

    return (block & MAPGRID_COLLISION_MASK) >> MAPGRID_COLLISION_SHIFT;
}

/**
 * FUNCTION: MapGridGetMetatileIdAt
 *
 * PURPOSE: Get the metatile ID at a map position (bits 0-9 of the grid entry).
 *          This identifies which 16x16 tile graphic is at that position.
 *
 * HOW IT WORKS:
 * For positions within the map, extracts the metatile ID with MAPGRID_METATILE_ID_MASK.
 * For positions outside the map (in the border region), falls back to the map's
 * border pattern using GetBorderBlockAt, which tiles the border data.
 */
u32 MapGridGetMetatileIdAt(s32 x, s32 y)
{
    u16 block = GetMapGridBlockAt(x, y);

    if (block == MAPGRID_UNDEFINED)
        return GetBorderBlockAt(x, y) & MAPGRID_METATILE_ID_MASK;

    return block & MAPGRID_METATILE_ID_MASK;
}

/**
 * FUNCTION: ExtractMetatileAttribute
 *
 * PURPOSE: Extract a specific field from a 32-bit metatile attribute word
 *          using the masks and shifts defined in sMetatileAttrMasks/Shifts.
 *
 * HOW IT WORKS:
 * Applies a bitmask to isolate the desired bits, then right-shifts to get
 * the value. For example, to extract BEHAVIOR (bits 0-8): mask with 0x1FF
 * and shift by 0. To extract LAYER_TYPE (bits 29-30): mask with 0x60000000
 * and shift by 29. If attributeType >= METATILE_ATTRIBUTE_COUNT, returns
 * the raw attribute word unmodified (used for "get all attributes" queries).
 */
u32 ExtractMetatileAttribute(u32 attributes, u8 attributeType)
{
    if (attributeType >= METATILE_ATTRIBUTE_COUNT) // Check for METATILE_ATTRIBUTES_ALL
        return attributes;

    return (attributes & sMetatileAttrMasks[attributeType]) >> sMetatileAttrShifts[attributeType];
}

u32 MapGridGetMetatileAttributeAt(s16 x, s16 y, u8 attributeType)
{
    u16 metatileId = MapGridGetMetatileIdAt(x, y);
    return GetAttributeByMetatileIdAndMapLayout(gMapHeader.mapLayout, metatileId, attributeType);
}

u32 MapGridGetMetatileBehaviorAt(s16 x, s16 y)
{
    return MapGridGetMetatileAttributeAt(x, y, METATILE_ATTRIBUTE_BEHAVIOR);
}

u8 MapGridGetMetatileLayerTypeAt(s16 x, s16 y)
{
    return MapGridGetMetatileAttributeAt(x, y, METATILE_ATTRIBUTE_LAYER_TYPE);
}

/**
 * FUNCTION: MapGridSetMetatileIdAt
 *
 * PURPOSE: Change the metatile at a position while preserving elevation data.
 *          Used by scripts to dynamically modify the map (e.g., opening a door,
 *          moving a boulder with Strength, cutting a tree with Cut).
 *
 * HOW IT WORKS:
 * Preserves the elevation bits (MAPGRID_ELEVATION_MASK) from the existing entry
 * and replaces everything else with the new metatile value. This ensures that
 * changing a tile's appearance doesn't accidentally alter its elevation.
 */
void MapGridSetMetatileIdAt(s32 x, s32 y, u16 metatile)
{
    s32 i;
    if (AreCoordsWithinMapGridBounds(x, y))
    {
        i = x + y * VMap.Xsize;
        VMap.map[i] = (VMap.map[i] & MAPGRID_ELEVATION_MASK) | (metatile & ~MAPGRID_ELEVATION_MASK);
    }
}

void MapGridSetMetatileEntryAt(s32 x, s32 y, u16 metatile)
{
    s32 i;
    if (AreCoordsWithinMapGridBounds(x, y))
    {
        i = x + VMap.Xsize * y;
        VMap.map[i] = metatile;
    }
}

void MapGridSetMetatileImpassabilityAt(s32 x, s32 y, bool32 impassable)
{
    if (AreCoordsWithinMapGridBounds(x, y))
    {
        if (impassable)
            VMap.map[x + VMap.Xsize * y] |= MAPGRID_COLLISION_MASK;
        else
            VMap.map[x + VMap.Xsize * y] &= ~MAPGRID_COLLISION_MASK;
    }
}

/**
 * FUNCTION: GetAttributeByMetatileIdAndMapLayout
 *
 * PURPOSE: Look up a specific attribute of a metatile by its ID and the map
 *          layout's tileset references.
 *
 * HOW IT WORKS:
 * Metatile IDs below NUM_METATILES_IN_PRIMARY (typically 512) come from the
 * primary tileset. IDs from 512 up to NUM_METATILES_TOTAL come from the
 * secondary tileset (indexed by subtracting 512). Each tileset has a
 * metatileAttributes array where index = metatile ID within that set.
 * Returns 0xFF for invalid metatile IDs.
 */
static u32 GetAttributeByMetatileIdAndMapLayout(const struct MapLayout *mapLayout, u16 metatile, u8 attributeType)
{
    const u32 * attributes;

    if (metatile < NUM_METATILES_IN_PRIMARY)
    {
        attributes = mapLayout->primaryTileset->metatileAttributes;
        return ExtractMetatileAttribute(attributes[metatile], attributeType);
    }
    else if (metatile < NUM_METATILES_TOTAL)
    {
        attributes = mapLayout->secondaryTileset->metatileAttributes;
        return ExtractMetatileAttribute(attributes[metatile - NUM_METATILES_IN_PRIMARY], attributeType);
    }
    else
    {
        return 0xFF;
    }
}

/**
 * FUNCTION: SaveMapView
 *
 * PURPOSE: Save the currently visible portion of the VMap into the save block.
 *          This preserves dynamic map changes (moved boulders, etc.) so they
 *          persist across map transitions and save/load cycles.
 *
 * HOW IT WORKS:
 * Copies a MAP_OFFSET_W x MAP_OFFSET_H region of the VMap centered around
 * the player's position into gSaveBlock2Ptr->mapView. This region corresponds
 * roughly to the screen-visible area plus a small margin. Only the metatile
 * entries (not the full VMap) are saved, since connected map data can be
 * reconstructed from the map header's connection list.
 */
void SaveMapView(void)
{
    s32 i, j;
    s32 x, y;
    u16 *mapView;
    s32 width;
    mapView = gSaveBlock2Ptr->mapView;
    width = VMap.Xsize;
    x = gSaveBlock1Ptr->pos.x;
    y = gSaveBlock1Ptr->pos.y;
    for (i = y; i < y + MAP_OFFSET_H; i++)
    {
        for (j = x; j < x + MAP_OFFSET_W; j++)
            *mapView++ = gBackupMapData[width * i + j];
    }
}

static bool32 SavedMapViewIsEmpty(void)
{
    u16 i;
    u32 marker = 0;

#ifndef UBFIX
    // BUG: This loop extends past the bounds of the mapView array. Its size is only 0x100.
    for (i = 0; i < 0x200; i++)
        marker |= gSaveBlock2Ptr->mapView[i];
#else
    for (i = 0; i < NELEMS(gSaveBlock2Ptr->mapView); i++)
        marker |= gSaveBlock2Ptr->mapView[i];
#endif

    if (marker == 0)
        return TRUE;
    else
        return FALSE;
}

static void ClearSavedMapView(void)
{
    CpuFill16(0, gSaveBlock2Ptr->mapView, sizeof(gSaveBlock2Ptr->mapView));
}

static void LoadSavedMapView(void)
{
    s32 i, j;
    s32 x, y;
    u16 *mapView;
    s32 width;
    mapView = gSaveBlock2Ptr->mapView;
    if (!SavedMapViewIsEmpty())
    {
        width = VMap.Xsize;
        x = gSaveBlock1Ptr->pos.x;
        y = gSaveBlock1Ptr->pos.y;
        for (i = y; i < y + MAP_OFFSET_H; i++)
        {
            for (j = x; j < x + MAP_OFFSET_W; j++)
            {
                gBackupMapData[j + width * i] = *mapView;
                mapView++;
            }
        }
        ClearSavedMapView();
    }
}

static void MoveMapViewToBackup(u8 direction)
{
    s32 width;
    u16 *mapView;
    s32 x0, y0;
    s32 x2, y2;
    u16 *src, *dest;
    s32 srci, desti;
    s32 r9, r8;
    s32 x, y;
    s32 i, j;
    mapView = gSaveBlock2Ptr->mapView;
    width = VMap.Xsize;
    r9 = 0;
    r8 = 0;
    x0 = gSaveBlock1Ptr->pos.x;
    y0 = gSaveBlock1Ptr->pos.y;
    x2 = 15;
    y2 = 14;
    switch (direction)
    {
    case CONNECTION_NORTH:
        y0 += 1;
        y2 = MAP_OFFSET_H - 1;
        break;
    case CONNECTION_SOUTH:
        r8 = 1;
        y2 = MAP_OFFSET_H - 1;
        break;
    case CONNECTION_WEST:
        x0 += 1;
        x2 = MAP_OFFSET_W - 1;
        break;
    case CONNECTION_EAST:
        r9 = 1;
        x2 = MAP_OFFSET_W - 1;
        break;
    }
    for (y = 0; y < y2; y++)
    {
        i = 0;
        j = 0;
        for (x = 0; x < x2; x++)
        {
            desti = width * (y + y0);
            srci = (y + r8) * MAP_OFFSET_W + r9;
            src = &mapView[srci + i];
            dest = &gBackupMapData[x0 + desti + j];
            *dest = *src;
            i++;
            j++;
        }
    }
    ClearSavedMapView();
}

s32 GetMapBorderIdAt(s32 x, s32 y)
{
    if (GetMapGridBlockAt(x, y) == MAPGRID_UNDEFINED)
        return CONNECTION_INVALID;

    if (x >= VMap.Xsize - (MAP_OFFSET + 1))
    {
        if (!gMapConnectionFlags.east)
            return CONNECTION_INVALID;

        return CONNECTION_EAST;
    }

    if (x < MAP_OFFSET)
    {
        if (!gMapConnectionFlags.west)
            return CONNECTION_INVALID;

        return CONNECTION_WEST;
    }

    if (y >= VMap.Ysize - MAP_OFFSET)
    {
        if (!gMapConnectionFlags.south)
            return CONNECTION_INVALID;

        return CONNECTION_SOUTH;
    }

    if (y < MAP_OFFSET)
    {
        if (!gMapConnectionFlags.north)
            return CONNECTION_INVALID;

        return CONNECTION_NORTH;
    }

    return CONNECTION_NONE;
}

static s32 GetPostCameraMoveMapBorderId(s32 x, s32 y)
{
    return GetMapBorderIdAt(gSaveBlock1Ptr->pos.x + MAP_OFFSET + x, gSaveBlock1Ptr->pos.y + MAP_OFFSET + y);
}

bool32 CanCameraMoveInDirection(s32 direction)
{
    s32 x, y;
    x = gSaveBlock1Ptr->pos.x + MAP_OFFSET + gDirectionToVectors[direction].x;
    y = gSaveBlock1Ptr->pos.y + MAP_OFFSET + gDirectionToVectors[direction].y;

    if (GetMapBorderIdAt(x, y) == CONNECTION_INVALID)
        return FALSE;

    return TRUE;
}

static void SetPositionFromConnection(const struct MapConnection *connection, int direction, s32 x, s32 y)
{
    struct MapHeader const *mapHeader;
    mapHeader = GetMapHeaderFromConnection(connection);
    switch (direction)
    {
    case CONNECTION_EAST:
        gSaveBlock1Ptr->pos.x = -x;
        gSaveBlock1Ptr->pos.y -= connection->offset;
        break;
    case CONNECTION_WEST:
        gSaveBlock1Ptr->pos.x = mapHeader->mapLayout->width;
        gSaveBlock1Ptr->pos.y -= connection->offset;
        break;
    case CONNECTION_SOUTH:
        gSaveBlock1Ptr->pos.x -= connection->offset;
        gSaveBlock1Ptr->pos.y = -y;
        break;
    case CONNECTION_NORTH:
        gSaveBlock1Ptr->pos.x -= connection->offset;
        gSaveBlock1Ptr->pos.y = mapHeader->mapLayout->height;
        break;
    }
}

/**
 * FUNCTION: CameraMove
 *
 * PURPOSE: Move the camera (and player position) by the given delta, handling
 *          map boundary crossings into connected maps.
 *
 * HOW IT WORKS:
 * 1. Checks if the movement would cross into a connected map boundary.
 * 2. If staying within the current map (CONNECTION_NONE or CONNECTION_INVALID),
 *    simply updates the player position.
 * 3. If crossing into a connected map:
 *    a. Saves the current map view for dynamic tile preservation
 *    b. Looks up which connected map the player is moving into
 *    c. Updates the player position relative to the new map
 *    d. Triggers a map load transition (LoadMapFromCameraTransition)
 *    e. Records the camera delta for smooth scrolling animation
 *    f. Restores the saved map view into the new VMap
 *
 * RETURNS: TRUE if a map transition occurred, FALSE if staying on current map.
 */
bool8 CameraMove(s32 x, s32 y)
{
    s32 direction;
    const struct MapConnection *connection;
    s32 old_x, old_y;
    gCamera.active = FALSE;
    direction = GetPostCameraMoveMapBorderId(x, y);
    if (direction == CONNECTION_NONE || direction == CONNECTION_INVALID)
    {
        gSaveBlock1Ptr->pos.x += x;
        gSaveBlock1Ptr->pos.y += y;
    }
    else
    {
        SaveMapView();
        old_x = gSaveBlock1Ptr->pos.x;
        old_y = gSaveBlock1Ptr->pos.y;
        connection = GetIncomingConnection(direction, gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y);
        SetPositionFromConnection(connection, direction, x, y);
        LoadMapFromCameraTransition(connection->mapGroup, connection->mapNum);
        gCamera.active = TRUE;
        gCamera.x = old_x - gSaveBlock1Ptr->pos.x;
        gCamera.y = old_y - gSaveBlock1Ptr->pos.y;
        gSaveBlock1Ptr->pos.x += x;
        gSaveBlock1Ptr->pos.y += y;
        MoveMapViewToBackup(direction);
    }
    return gCamera.active;
}

const struct MapConnection *GetIncomingConnection(u8 direction, s32 x, s32 y)
{
    s32 count;
    const struct MapConnection *connection;
    const struct MapConnections *connections = gMapHeader.connections;
    s32 i;

#ifdef UBFIX // UB: Multiple possible null dereferences
    if (connections == NULL || connections->connections == NULL)
        return NULL;
#endif
    count = connections->count;
    connection = connections->connections;
    for (i = 0; i < count; i++, connection++)
    {
        if (connection->direction == direction && IsPosInIncomingConnectingMap(direction, x, y, connection) == TRUE)
            return connection;
    }
    return NULL;

}

static bool8 IsPosInIncomingConnectingMap(u8 direction, s32 x, s32 y, const struct MapConnection *connection)
{
    struct MapHeader const *mapHeader;
    mapHeader = GetMapHeaderFromConnection(connection);
    switch (direction)
    {
    case CONNECTION_SOUTH:
    case CONNECTION_NORTH:
        return IsCoordInIncomingConnectingMap(x, gMapHeader.mapLayout->width, mapHeader->mapLayout->width, connection->offset);
    case CONNECTION_WEST:
    case CONNECTION_EAST:
        return IsCoordInIncomingConnectingMap(y, gMapHeader.mapLayout->height, mapHeader->mapLayout->height, connection->offset);
    }
    return FALSE;
}

static bool8 IsCoordInIncomingConnectingMap(s32 coord, s32 srcMax, s32 destMax, s32 offset)
{
    s32 offset2 = max(offset, 0);

    if (destMax + offset < srcMax)
        srcMax = destMax + offset;

    if (offset2 <= coord && coord <= srcMax)
        return TRUE;

    return FALSE;
}

static bool32 IsCoordInConnectingMap(s32 coord, s32 max)
{
    if (coord >= 0 && coord < max)
        return TRUE;

    return FALSE;
}

static s32 IsPosInConnectingMap(const struct MapConnection *connection, s32 x, s32 y)
{
    struct MapHeader const *mapHeader;
    mapHeader = GetMapHeaderFromConnection(connection);
    switch (connection->direction)
    {
    case CONNECTION_SOUTH:
    case CONNECTION_NORTH:
        return IsCoordInConnectingMap(x - connection->offset, mapHeader->mapLayout->width);
    case CONNECTION_WEST:
    case CONNECTION_EAST:
        return IsCoordInConnectingMap(y - connection->offset, mapHeader->mapLayout->height);
    }
    return FALSE;
}

const struct MapConnection *GetMapConnectionAtPos(s16 x, s16 y)
{
    s32 count;
    const struct MapConnection *connection;
    s32 i;
    u8 direction;
    if (!gMapHeader.connections)
    {
        return NULL;
    }
    else
    {
        count = gMapHeader.connections->count;
        connection = gMapHeader.connections->connections;
        for (i = 0; i < count; i++, connection++)
        {
            direction = connection->direction;
            if ((direction == CONNECTION_DIVE || direction == CONNECTION_EMERGE)
                || (direction == CONNECTION_NORTH && y > MAP_OFFSET - 1)
                || (direction == CONNECTION_SOUTH && y < gMapHeader.mapLayout->height + MAP_OFFSET)
                || (direction == CONNECTION_WEST && x > MAP_OFFSET - 1)
                || (direction == CONNECTION_EAST && x < gMapHeader.mapLayout->width + MAP_OFFSET))
            {
                continue;
            }

            if (IsPosInConnectingMap(connection, x - MAP_OFFSET, y - MAP_OFFSET) == TRUE)
                return connection;
        }
    }
    return NULL;
}

void SetCameraFocusCoords(u16 x, u16 y)
{
    gSaveBlock1Ptr->pos.x = x - MAP_OFFSET;
    gSaveBlock1Ptr->pos.y = y - MAP_OFFSET;
}

void GetCameraFocusCoords(u16 *x, u16 *y)
{
    *x = gSaveBlock1Ptr->pos.x + MAP_OFFSET;
    *y = gSaveBlock1Ptr->pos.y + MAP_OFFSET;
}

// Unused
static void SetCameraCoords(u16 x, u16 y)
{
    gSaveBlock1Ptr->pos.x = x;
    gSaveBlock1Ptr->pos.y = y;
}

void GetCameraCoords(u16 *x, u16 *y)
{
    *x = gSaveBlock1Ptr->pos.x;
    *y = gSaveBlock1Ptr->pos.y;
}

/**
 * FUNCTION: CopyTilesetToVram
 *
 * PURPOSE: Load a tileset's tile graphics into VRAM for the map background.
 *
 * GBA CONTEXT:
 * Tileset graphics are the raw 8x8 pixel tile images that metatiles reference.
 * They're loaded into BG character base 2 (the third character block in VRAM).
 * Each tile is 32 bytes of 4bpp data (8x8 pixels, 4 bits per pixel = 32 bytes).
 * The offset parameter determines where in the character block to place them
 * (primary tiles at offset 0, secondary tiles after the primary set).
 *
 * If the tileset is compressed (LZ77), DecompressAndCopyTileDataToVram2 is used
 * which decompresses on-the-fly during DMA transfer.
 */
static void CopyTilesetToVram(struct Tileset const *tileset, u16 numTiles, u16 offset)
{
    if (tileset)
    {
        if (!tileset->isCompressed)
            LoadBgTiles(2, tileset->tiles, numTiles * 32, offset);
        else
            DecompressAndCopyTileDataToVram2(2, tileset->tiles, numTiles * 32, offset, 0);
    }
}

static void CopyTilesetToVramUsingHeap(struct Tileset const *tileset, u16 numTiles, u16 offset)
{
    if (tileset)
    {
        if (!tileset->isCompressed)
            LoadBgTiles(2, tileset->tiles, numTiles * 32, offset);
        else
            DecompressAndLoadBgGfxUsingHeap2(2, tileset->tiles, numTiles * 32, offset, 0);
    }
}

static void ApplyGlobalTintToPaletteEntries(u16 offset, u16 size)
{
    switch (gGlobalFieldTintMode)
    {
    case QL_TINT_NONE:
        return;
    case QL_TINT_GRAYSCALE:
        TintPalette_GrayScale(&gPlttBufferUnfaded[offset], size);
        break;
    case QL_TINT_SEPIA:
        TintPalette_SepiaTone(&gPlttBufferUnfaded[offset], size);
        break;
    case QL_TINT_BACKUP_GRAYSCALE:
        QuestLog_BackUpPalette(offset, size);
        TintPalette_GrayScale(&gPlttBufferUnfaded[offset], size);
        break;
    default:
        return;
    }
    CpuCopy16(&gPlttBufferUnfaded[offset], &gPlttBufferFaded[offset], PLTT_SIZEOF(size));
}

void ApplyGlobalTintToPaletteSlot(u8 slot, u8 count)
{
    switch (gGlobalFieldTintMode)
    {
    case QL_TINT_NONE:
        return;
    case QL_TINT_GRAYSCALE:
        TintPalette_GrayScale(&gPlttBufferUnfaded[BG_PLTT_ID(slot)], count * 16);
        break;
    case QL_TINT_SEPIA:
        TintPalette_SepiaTone(&gPlttBufferUnfaded[BG_PLTT_ID(slot)], count * 16);
        break;
    case QL_TINT_BACKUP_GRAYSCALE:
        QuestLog_BackUpPalette(BG_PLTT_ID(slot), count * 16);
        TintPalette_GrayScale(&gPlttBufferUnfaded[BG_PLTT_ID(slot)], count * 16);
        break;
    default:
        return;
    }
    CpuFastCopy(&gPlttBufferUnfaded[BG_PLTT_ID(slot)], &gPlttBufferFaded[BG_PLTT_ID(slot)], count * PLTT_SIZE_4BPP);
}

/**
 * FUNCTION: LoadTilesetPalette
 *
 * PURPOSE: Load a tileset's color palettes into palette RAM. Each tileset
 *          provides 16-color palettes that its tiles reference.
 *
 * GBA CONTEXT:
 * The GBA has 256 BG palette entries organized as 16 palettes of 16 colors each.
 * Primary tilesets use palettes 0-6 (NUM_PALS_IN_PRIMARY), and secondary tilesets
 * use palettes 7-12. Palette slot 0, color 0 in the primary tileset is forced to
 * RGB_BLACK because color 0 of palette 0 is the screen backdrop color.
 *
 * After loading, the global tint (grayscale/sepia for quest log) is applied if active.
 */
static void LoadTilesetPalette(struct Tileset const *tileset, u16 destOffset, u16 size)
{
    u16 black = RGB_BLACK;

    if (tileset)
    {
        if (tileset->isSecondary == FALSE)
        {
            LoadPalette(&black, destOffset, PLTT_SIZEOF(1));
            LoadPalette(tileset->palettes[0] + 1, destOffset + 1, size - PLTT_SIZEOF(1));
            ApplyGlobalTintToPaletteEntries(destOffset + 1, (size - 2) >> 1);
        }
        else if (tileset->isSecondary == TRUE)
        {
            LoadPalette(tileset->palettes[NUM_PALS_IN_PRIMARY], destOffset, size);
            ApplyGlobalTintToPaletteEntries(destOffset, size >> 1);
        }
        else
        {
            LoadCompressedPalette((const u32 *)tileset->palettes, destOffset, size);
            ApplyGlobalTintToPaletteEntries(destOffset, size >> 1);
        }
    }
}

void CopyPrimaryTilesetToVram(const struct MapLayout *mapLayout)
{
    CopyTilesetToVram(mapLayout->primaryTileset, NUM_TILES_IN_PRIMARY, 0);
}

void CopySecondaryTilesetToVram(const struct MapLayout *mapLayout)
{
    CopyTilesetToVram(mapLayout->secondaryTileset, NUM_TILES_TOTAL - NUM_TILES_IN_PRIMARY, NUM_TILES_IN_PRIMARY);
}

void CopySecondaryTilesetToVramUsingHeap(const struct MapLayout *mapLayout)
{
    CopyTilesetToVramUsingHeap(mapLayout->secondaryTileset, NUM_TILES_TOTAL - NUM_TILES_IN_PRIMARY, NUM_TILES_IN_PRIMARY);
}

static void LoadPrimaryTilesetPalette(const struct MapLayout *mapLayout)
{
    LoadTilesetPalette(mapLayout->primaryTileset, BG_PLTT_ID(0), NUM_PALS_IN_PRIMARY * PLTT_SIZE_4BPP);
}

void LoadSecondaryTilesetPalette(const struct MapLayout *mapLayout)
{
    LoadTilesetPalette(mapLayout->secondaryTileset, BG_PLTT_ID(NUM_PALS_IN_PRIMARY), (NUM_PALS_TOTAL - NUM_PALS_IN_PRIMARY) * PLTT_SIZE_4BPP);
}

void CopyMapTilesetsToVram(struct MapLayout const *mapLayout)
{
    if (mapLayout)
    {
        CopyTilesetToVramUsingHeap(mapLayout->primaryTileset, NUM_TILES_IN_PRIMARY, 0);
        CopyTilesetToVramUsingHeap(mapLayout->secondaryTileset, NUM_TILES_TOTAL - NUM_TILES_IN_PRIMARY, NUM_TILES_IN_PRIMARY);
    }
}

void LoadMapTilesetPalettes(struct MapLayout const *mapLayout)
{
    if (mapLayout)
    {
        LoadPrimaryTilesetPalette(mapLayout);
        LoadSecondaryTilesetPalette(mapLayout);
    }
}
