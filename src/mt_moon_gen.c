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

#define CAVE_W 48
#define CAVE_H 40
#define CAVE_SIZE (CAVE_W * CAVE_H)

// Full u16 metatile entries (metatile | collision<<10 | elevation<<12)
#define TILE_FLOOR          0x3281
#define TILE_ENTRANCE       0x3287
#define TILE_EXIT           0x3285

// Wall tiles by cardinal adjacency
#define TILE_W_NONE         0x0691  // interior
#define TILE_W_BELOW        0x0711  // cliff face
#define TILE_W_ABOVE        0x0689  // wall top
#define TILE_W_LEFT         0x0690  // floor to left
#define TILE_W_RIGHT        0x0692  // floor to right
#define TILE_W_ABOVE_LEFT   0x0688
#define TILE_W_ABOVE_RIGHT  0x068a
#define TILE_W_BELOW_LEFT   0x0683
#define TILE_W_BELOW_RIGHT  0x0683
#define TILE_W_ABOVE_LR     0x069f
#define TILE_W_BELOW_LR     0x0699
#define TILE_W_LEFT_RIGHT   0x0682
#define TILE_W_ABOVE_BELOW  0x0682
#define TILE_W_SURROUNDED   0x0682  // use thin wall tile instead of 717 (has signpost behavior)

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
        case 0x1: return TILE_W_RIGHT;
        case 0x2: return TILE_W_LEFT;
        case 0x3: return TILE_W_LEFT_RIGHT;
        case 0x4: return TILE_W_BELOW;
        case 0x5: return TILE_W_BELOW_RIGHT;
        case 0x6: return TILE_W_BELOW_LEFT;
        case 0x7: return TILE_W_BELOW_LR;
        case 0x8: return TILE_W_ABOVE;
        case 0x9: return TILE_W_ABOVE_RIGHT;
        case 0xA: return TILE_W_ABOVE_LEFT;
        case 0xB: return TILE_W_ABOVE_LR;
        case 0xC: return TILE_W_ABOVE_BELOW;
        default:  return TILE_W_SURROUNDED;
        }
    }

    // No cardinal floor - check diagonals for inner corners
    // Only use edge tile ~25% of the time for visual variety, otherwise plain interior
    {
        bool8 fNW = IsFloor(x - 1, y - 1);
        bool8 fNE = IsFloor(x + 1, y - 1);
        bool8 fSW = IsFloor(x - 1, y + 1);
        bool8 fSE = IsFloor(x + 1, y + 1);

        if (fSW || fSE || fNW || fNE)
        {
            if ((Random() % 4) == 0)
            {
                if (fSW || fSE) return TILE_W_BELOW;
                return TILE_W_ABOVE;
            }
        }
    }

    return TILE_W_NONE;
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

    // Then write the generated cave grid over it
    for (y = 0; y < CAVE_H; y++)
        for (x = 0; x < CAVE_W; x++)
        {
            u16 tile = GRID(x, y) ? TILE_FLOOR : GetWallTile(x, y);
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
