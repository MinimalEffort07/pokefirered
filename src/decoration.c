/**
 * @file decoration.c
 * @brief Decoration Data Includes — Secret Base / Room Decoration Definitions
 *
 * FILE OVERVIEW:
 * This file aggregates all decoration-related data for the game. Decorations are
 * items the player can place in their Secret Base (a feature from Ruby/Sapphire/
 * Emerald that has limited presence in FireRed). The data is organized into three
 * include files:
 *
 *   1. Tiles — the graphical tile data for each decoration (how it looks on the map)
 *   2. Descriptions — text strings describing each decoration in menus
 *   3. Headers — master table linking each decoration ID to its tiles, description,
 *      size, placement rules, and other properties
 *
 * Like tilesets.c, this file contains no executable code — it simply pulls in
 * pre-defined data tables that other systems reference by decoration ID.
 */
#include "global.h"
#include "decoration.h"
#include "constants/decorations.h"

/* Tile graphics data for each decoration (how they visually appear on the map) */
#include "data/decoration/tiles.h"
/* Text descriptions shown to the player when viewing decorations in menus */
#include "data/decoration/description.h"
/* Master decoration table: links each decoration ID to its properties */
#include "data/decoration/header.h"
