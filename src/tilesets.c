/**
 * @file tilesets.c
 * @brief Tileset Data Includes — Map Tile Graphics, Metatiles, and Headers
 *
 * FILE OVERVIEW:
 * This file acts as a central aggregation point for all tileset data used by the
 * game's map rendering system. It contains no executable code — instead, it pulls
 * in three categories of pre-built data via #include:
 *
 *   1. Graphics — the raw pixel data for each 8x8 tile (stored in 4bpp format)
 *   2. Metatiles — definitions of how 8x8 tiles combine into 16x16 "metatiles"
 *      (the basic building blocks of map layouts)
 *   3. Headers — metadata for each tileset (which graphics it uses, its metatile
 *      definitions, animation callbacks, etc.)
 *
 * GBA CONTEXT:
 * The GBA's background system renders maps using 8x8 pixel tiles stored in VRAM.
 * Pokemon games add a layer of abstraction called "metatiles" — each metatile is
 * a 2x2 grid of regular tiles that forms a single logical map cell (like a grass
 * patch, a wall segment, or a floor tile). This two-level system lets designers
 * build complex maps from a relatively small set of base tiles.
 *
 * Tilesets come in pairs: a "primary" tileset (shared across many maps in a region)
 * and a "secondary" tileset (unique to a specific map or area). Together they
 * provide all the visual tiles needed to render any given map.
 */
#include "global.h"
#include "tilesets.h"
#include "tileset_anims.h"

/* Pull in the raw tile graphics (pixel data for every tileset's 8x8 tiles) */
#include "data/tilesets/graphics.h"
/* Pull in metatile definitions (how 8x8 tiles combine into 16x16 map cells) */
#include "data/tilesets/metatiles.h"
/* Pull in tileset header structures (metadata linking graphics, metatiles, and callbacks) */
#include "data/tilesets/headers.h"
