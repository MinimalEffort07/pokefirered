/**
 * @file data.c
 * @brief Battle Sprite Data Tables and Pokemon/Trainer Data Includes
 *
 * FILE OVERVIEW:
 * This file serves as the central hub for battle-related graphical data and
 * game data tables. It defines:
 *
 * 1. BATTLER SPRITE TABLES: Frame image tables for battle participant sprites
 *    (player's Pokemon, opponent's Pokemon, trainer back sprites)
 * 2. ANIMATION COMMANDS: Sprite animation sequences for battler sprites
 *    (normal, emerge from pokeball, return to pokeball, various effects)
 * 3. AFFINE ANIMATION COMMANDS: Rotation/scaling animations for battle sprites
 *    (grow, shrink, spin, tip, etc.)
 * 4. DATA INCLUDES: Pulls in the massive data tables for all Pokemon graphics,
 *    trainer graphics, species names, move names, and trainer parties
 *
 * GBA CONTEXT — SPRITE ANIMATION SYSTEM:
 * The GBA's sprite (OBJ) hardware doesn't have built-in animation support.
 * Animations are implemented in software using command tables:
 * - AnimCmd: Controls which frame to display and for how many ticks
 * - AffineAnimCmd: Controls rotation/scaling transformations via the GBA's
 *   OAM affine matrix hardware (4 hardware matrices available)
 *
 * MEMORY LAYOUT:
 * Battler sprites are stored in a heap buffer (gHeap + 0x8000) rather than
 * in static ROM data. This is because Pokemon sprites are decompressed from
 * LZ77-compressed ROM data into this RAM buffer at battle start. Each sprite
 * occupies MON_PIC_SIZE bytes (typically 64x64 pixels at 4bpp = 2048 bytes).
 */
#include "global.h"
#include "gflib.h"
#include "battle.h"
#include "data.h"
#include "graphics.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/battle_ai.h"
#include "constants/trainers.h"

/**
 * BATTLER_OFFSET macro
 *
 * Calculates the RAM address for battler sprite frame 'i'.
 * gHeap + 0x8000 is the base address where battle sprites are decompressed to.
 * Each sprite frame is MON_PIC_SIZE bytes (2048 = 0x800 bytes for a 64x64 4bpp image).
 * With 16 slots (4 battler positions x 4 frames each), this uses 32KB of heap space.
 */
#define BATTLER_OFFSET(i) (gHeap + 0x8000 + MON_PIC_SIZE * (i))

/**
 * Battler Picture Frame Tables
 *
 * In a double battle, there are 4 battler positions:
 * - PlayerLeft (slots 0-3): Player's left Pokemon (or only Pokemon in singles)
 * - OpponentLeft (slots 4-7): Opponent's left Pokemon
 * - PlayerRight (slots 8-11): Player's right Pokemon (doubles only)
 * - OpponentRight (slots 12-15): Opponent's right Pokemon (doubles only)
 *
 * Each position has 4 animation frames. The SpriteFrameImage struct contains
 * a pointer to the pixel data and its size (MON_PIC_SIZE = 0x800 bytes).
 */
const struct SpriteFrameImage gBattlerPicTable_PlayerLeft[] =
{
    BATTLER_OFFSET(0), MON_PIC_SIZE,
    BATTLER_OFFSET(1), MON_PIC_SIZE,
    BATTLER_OFFSET(2), MON_PIC_SIZE,
    BATTLER_OFFSET(3), MON_PIC_SIZE,
};

const struct SpriteFrameImage gBattlerPicTable_OpponentLeft[] =
{
    BATTLER_OFFSET(4), MON_PIC_SIZE,
    BATTLER_OFFSET(5), MON_PIC_SIZE,
    BATTLER_OFFSET(6), MON_PIC_SIZE,
    BATTLER_OFFSET(7), MON_PIC_SIZE,
};

const struct SpriteFrameImage gBattlerPicTable_PlayerRight[] =
{
    BATTLER_OFFSET(8),  MON_PIC_SIZE,
    BATTLER_OFFSET(9),  MON_PIC_SIZE,
    BATTLER_OFFSET(10), MON_PIC_SIZE,
    BATTLER_OFFSET(11), MON_PIC_SIZE,
};

const struct SpriteFrameImage gBattlerPicTable_OpponentRight[] =
{
    BATTLER_OFFSET(12), MON_PIC_SIZE,
    BATTLER_OFFSET(13), MON_PIC_SIZE,
    BATTLER_OFFSET(14), MON_PIC_SIZE,
    BATTLER_OFFSET(15), MON_PIC_SIZE,
};

/**
 * Trainer Back Sprite Frame Tables
 *
 * These are for the trainer sprites shown from behind during battle (the player's
 * perspective). Unlike Pokemon sprites which are decompressed to RAM, trainer sprites
 * are stored as static ROM data and referenced directly.
 *
 * Each frame is 0x0800 bytes (2KB = 64x32 pixels at 4bpp, or a 64x64 image in parts).
 * Red and Leaf (the player characters) have 5 frames for their throwing animation,
 * while others have 4 frames.
 */
const struct SpriteFrameImage gTrainerBackPicTable_Red[] =
{
    gTrainerBackPic_Red, 0x0800,
    gTrainerBackPic_Red + 0x0800, 0x0800,   /* Frame 2: offset by one frame size */
    gTrainerBackPic_Red + 0x1000, 0x0800,   /* Frame 3: offset by two frame sizes */
    gTrainerBackPic_Red + 0x1800, 0x0800,   /* Frame 4 */
    gTrainerBackPic_Red + 0x2000, 0x0800,   /* Frame 5: extra frame for throw anim */
};

const struct SpriteFrameImage gTrainerBackPicTable_Leaf[] =
{
    gTrainerBackPic_Leaf, 0x0800,
    gTrainerBackPic_Leaf + 0x0800, 0x0800,
    gTrainerBackPic_Leaf + 0x1000, 0x0800,
    gTrainerBackPic_Leaf + 0x1800, 0x0800,
    gTrainerBackPic_Leaf + 0x2000, 0x0800,
};

const struct SpriteFrameImage gTrainerBackPicTable_Pokedude[] =
{
    gTrainerBackPic_Pokedude, 0x0800,
    gTrainerBackPic_Pokedude + 0x0800, 0x0800,
    gTrainerBackPic_Pokedude + 0x1000, 0x0800,
    gTrainerBackPic_Pokedude + 0x1800, 0x0800,
};

const struct SpriteFrameImage gTrainerBackPicTable_OldMan[] =
{
    gTrainerBackPic_OldMan, 0x0800,
    gTrainerBackPic_OldMan + 0x0800, 0x0800,
    gTrainerBackPic_OldMan + 0x1000, 0x0800,
    gTrainerBackPic_OldMan + 0x1800, 0x0800,
};

const struct SpriteFrameImage gTrainerBackPicTable_RSBrendan[] =
{
    gTrainerBackPic_RSBrendan, 0x0800,
    gTrainerBackPic_RSBrendan + 0x0800, 0x0800,
    gTrainerBackPic_RSBrendan + 0x1000, 0x0800,
    gTrainerBackPic_RSBrendan + 0x1800, 0x0800,
};

const struct SpriteFrameImage gTrainerBackPicTable_RSMay[] =
{
    gTrainerBackPic_RSMay, 0x0800,
    gTrainerBackPic_RSMay + 0x0800, 0x0800,
    gTrainerBackPic_RSMay + 0x1000, 0x0800,
    gTrainerBackPic_RSMay + 0x1800, 0x0800,
};

/**
 * BASIC SPRITE ANIMATION COMMANDS
 *
 * These are simple "show frame N forever" animations used for static display.
 * ANIMCMD_FRAME(frameNum, duration): Display frame 'frameNum' for 'duration' ticks
 *   (0 duration = display indefinitely until changed)
 * ANIMCMD_END: Terminates the animation sequence
 */
static const union AnimCmd sAnim_GeneralFrame0[] =
{
    ANIMCMD_FRAME(0, 0),  /* Show frame 0 forever */
    ANIMCMD_END,
};

static const union AnimCmd sAnim_GeneralFrame3[] =
{
    ANIMCMD_FRAME(3, 0),  /* Show frame 3 forever */
    ANIMCMD_END,
};

/**
 * AFFINE ANIMATION COMMANDS FOR BATTLE SPRITES
 *
 * These control rotation and scaling of battler sprites using the GBA's hardware
 * affine transformation system. The GBA OAM supports 32 affine-capable sprites
 * sharing 32 affine parameter sets (matrices).
 *
 * AFFINEANIMCMD_FRAME(xScale, yScale, rotation, duration):
 * - xScale, yScale: 8.8 fixed-point scale values (0x100 = 1.0x = normal size)
 *   Positive = grow each frame, negative = shrink. The value is ADDED each frame.
 * - rotation: Amount of rotation per frame (positive = counterclockwise)
 *   One full rotation = 256 units
 * - duration: How many frames to apply this transformation (0 = set immediately)
 *
 * For the initial frame (duration=0), xScale/yScale set the ABSOLUTE scale.
 * For subsequent frames (duration>0), they set the DELTA (change per frame).
 */
// Many of these affine anims seem to go unused, and
// instead SetSpriteRotScale is used to manipulate
// the battler sprites directly (for instance, in AnimTask_SwitchOutShrinkMon).
// Those with explicit indexes are referenced elsewhere.

/* Normal state: 1.0x scale (0x100), no rotation */
static const union AffineAnimCmd sAffineAnim_Battler_Normal[] =
{
    AFFINEANIMCMD_FRAME(0x100, 0x100, 0, 0),  /* Set to exactly 1.0x scale */
    AFFINEANIMCMD_END,
};

/* Horizontally flipped: -1.0x X scale mirrors the sprite */
static const union AffineAnimCmd sAffineAnim_Battler_Flipped[] =
{
    AFFINEANIMCMD_FRAME(-0x100, 0x0100, 0, 0),  /* Negative X = mirror horizontally */
    AFFINEANIMCMD_END,
};

/* Emerge from pokeball: Start small (0x28 = ~15% size), grow over 12 frames */
static const union AffineAnimCmd sAffineAnim_Battler_Emerge[] =
{
    AFFINEANIMCMD_FRAME(0x28, 0x28, 0, 0),     /* Start at ~15% scale */
    AFFINEANIMCMD_FRAME(0x12, 0x12, 0, 12),    /* Grow by 0x12 per frame for 12 frames */
    AFFINEANIMCMD_END,
};

/* Return to pokeball: Slowly shrink and disappear */
static const union AffineAnimCmd sAffineAnim_Battler_Return[] =
{
    AFFINEANIMCMD_FRAME(-0x2, -0x2, 0, 18),    /* Slow shrink for 18 frames */
    AFFINEANIMCMD_FRAME(-0x10, -0x10, 0, 15),  /* Faster shrink for 15 frames */
    AFFINEANIMCMD_END,
};

/* Looping horizontal squish: Used for certain flinch/hit effects */
static const union AffineAnimCmd sAffineAnim_Battler_HorizontalSquishLoop[] =
{
    AFFINEANIMCMD_FRAME(0xA0, 0x100, 0, 0),   /* Start squished horizontally */
    AFFINEANIMCMD_FRAME( 0x4,   0x0, 0, 8),   /* Stretch wider for 8 frames */
    AFFINEANIMCMD_FRAME(-0x4,   0x0, 0, 8),   /* Squish back for 8 frames */
    AFFINEANIMCMD_JUMP(1),                      /* Loop back to the stretch step */
};

/* Grow slightly: Subtle enlarge effect */
static const union AffineAnimCmd sAffineAnim_Battler_Grow[] =
{
    AFFINEANIMCMD_FRAME(0x2, 0x2, 0, 20),
    AFFINEANIMCMD_END,
};

/* Shrink slightly: Subtle shrink effect */
static const union AffineAnimCmd sAffineAnim_Battler_Shrink[] =
{
    AFFINEANIMCMD_FRAME(-0x2, -0x2, 0, 20),
    AFFINEANIMCMD_END,
};

/* Shrink from big to small: Used for size transition effects */
static const union AffineAnimCmd sAffineAnim_Battler_BigToSmall[] =
{
    AFFINEANIMCMD_FRAME(0x100, 0x100, 0, 0),    /* Start at normal size */
    AFFINEANIMCMD_FRAME(-0x10, -0x10, 0, 9),    /* Rapidly shrink for 9 frames */
    AFFINEANIMCMD_END,
};

/* Grow large over many frames: Slow dramatic growth */
static const union AffineAnimCmd sAffineAnim_Battler_GrowLarge[] =
{
    AFFINEANIMCMD_FRAME(0x4, 0x4, 0, 63),       /* Grow slowly for 63 frames (~1 second) */
    AFFINEANIMCMD_END,
};

/* Tip right: Small tilt animation (player side) */
static const union AffineAnimCmd sAffineAnim_Battler_TipRight[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0, -3, 5),       /* Rotate -3 per frame for 5 frames (tilt right) */
    AFFINEANIMCMD_FRAME(0x0, 0x0,  3, 5),       /* Rotate +3 per frame for 5 frames (tilt back) */
    AFFINEANIMCMD_END,
};

/**
 * Player-side affine animation table.
 * Indexed by BATTLER_AFFINE_* constants. The sprite engine selects the
 * appropriate animation based on the current battle action (sending out,
 * returning, being hit, etc.)
 */
const union AffineAnimCmd *const gAffineAnims_BattleSpritePlayerSide[] =
{
    [BATTLER_AFFINE_NORMAL] = sAffineAnim_Battler_Normal,
    [BATTLER_AFFINE_EMERGE] = sAffineAnim_Battler_Emerge,
    [BATTLER_AFFINE_RETURN] = sAffineAnim_Battler_Return,
    sAffineAnim_Battler_HorizontalSquishLoop,
    sAffineAnim_Battler_Grow,
    sAffineAnim_Battler_Shrink,
    sAffineAnim_Battler_GrowLarge,
    sAffineAnim_Battler_TipRight,
    sAffineAnim_Battler_BigToSmall,
};

/* Spin while shrinking: Used for opponent fainting effects */
static const union AffineAnimCmd sAffineAnim_Battler_SpinShrink[] =
{
    AFFINEANIMCMD_FRAME(-0x4, -0x4, 4, 63),     /* Shrink and rotate for 63 frames */
    AFFINEANIMCMD_END,
};

/* Tip left: Mirror of TipRight for opponent side */
static const union AffineAnimCmd sAffineAnim_Battler_TipLeft[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0,  3, 5),
    AFFINEANIMCMD_FRAME(0x0, 0x0, -3, 5),
    AFFINEANIMCMD_END,
};

/* Rotate up then back: A wobble/recoil effect */
static const union AffineAnimCmd sAffineAnim_Battler_RotateUpAndBack[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0, -5, 20),      /* Tilt one direction */
    AFFINEANIMCMD_FRAME(0x0, 0x0,  0, 20),      /* Hold for 20 frames */
    AFFINEANIMCMD_FRAME(0x0, 0x0,  5, 20),      /* Tilt back to original */
    AFFINEANIMCMD_END,
};

/* Full spin: Complete rotation for special effects */
static const union AffineAnimCmd sAffineAnim_Battler_Spin[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0, 9, 110),      /* Spin for ~110 frames */
    AFFINEANIMCMD_END,
};

/**
 * Opponent-side affine animation table.
 * Similar to the player-side table but with different effects for TipLeft
 * (mirror of TipRight) and additional spin/rotation effects.
 */
const union AffineAnimCmd *const gAffineAnims_BattleSpriteOpponentSide[] =
{
    [BATTLER_AFFINE_NORMAL] = sAffineAnim_Battler_Normal,
    [BATTLER_AFFINE_EMERGE] = sAffineAnim_Battler_Emerge,
    [BATTLER_AFFINE_RETURN] = sAffineAnim_Battler_Return,
    sAffineAnim_Battler_HorizontalSquishLoop,
    sAffineAnim_Battler_Grow,
    sAffineAnim_Battler_Shrink,
    sAffineAnim_Battler_SpinShrink,
    sAffineAnim_Battler_TipLeft,
    sAffineAnim_Battler_RotateUpAndBack,
    sAffineAnim_Battler_BigToSmall,
    sAffineAnim_Battler_Spin,
};

/**
 * Contest-side affine animation table.
 * The "Normal" state uses the flipped (mirrored) version — contests display
 * Pokemon facing the opposite direction compared to battles.
 */
const union AffineAnimCmd *const gAffineAnims_BattleSpriteContest[] =
{
    [BATTLER_AFFINE_NORMAL] = sAffineAnim_Battler_Flipped,  /* Mirrored for contests */
    [BATTLER_AFFINE_EMERGE] = sAffineAnim_Battler_Emerge,
    [BATTLER_AFFINE_RETURN] = sAffineAnim_Battler_Return,
    sAffineAnim_Battler_HorizontalSquishLoop,
    sAffineAnim_Battler_Grow,
    sAffineAnim_Battler_Shrink,
    sAffineAnim_Battler_SpinShrink,
    sAffineAnim_Battler_TipLeft,
    sAffineAnim_Battler_RotateUpAndBack,
    sAffineAnim_Battler_BigToSmall,
    sAffineAnim_Battler_Spin,
};

/**
 * Monster Picture Frame Animations
 * Simple animations that just display a specific frame.
 * Used to select which of the 4 possible animation frames to show.
 */
static const union AnimCmd sAnim_MonPic_0[] =
{
    ANIMCMD_FRAME(0, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_MonPic_1[] =
{
    ANIMCMD_FRAME(1, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_MonPic_2[] =
{
    ANIMCMD_FRAME(2, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_MonPic_3[] =
{
    ANIMCMD_FRAME(3, 0),
    ANIMCMD_END,
};

/* Master animation table for Pokemon pictures — indexed by frame number */
const union AnimCmd *const gAnims_MonPic[] =
{
    sAnim_MonPic_0,
    sAnim_MonPic_1,
    sAnim_MonPic_2,
    sAnim_MonPic_3,
};

/**
 * DATA TABLE HELPER MACROS
 *
 * These macros simplify the creation of the massive data tables included below.
 * Each macro maps a species/trainer name to its graphics data.
 *
 * SPECIES_SPRITE: Maps species to its sprite sheet data and size (0x800 = 2KB)
 * SPECIES_PAL: Maps species to its normal color palette
 * SPECIES_SHINY_PAL: Maps species to its shiny color palette (using an offset tag)
 * TRAINER_SPRITE: Maps trainer class to its front sprite and size
 * TRAINER_PAL: Maps trainer class to its color palette
 */
#define SPECIES_SPRITE(species, sprite) [SPECIES_##species] = {sprite, 0x800, SPECIES_##species}
#define SPECIES_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species}
#define SPECIES_SHINY_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species + SPECIES_SHINY_TAG}

#define TRAINER_SPRITE(trainerPic, sprite, size) [TRAINER_PIC_##trainerPic] = {sprite, size, TRAINER_PIC_##trainerPic}
#define TRAINER_PAL(trainerPic, pal) [TRAINER_PIC_##trainerPic] = {pal, TRAINER_PIC_##trainerPic}

/*
 * MASSIVE DATA TABLE INCLUDES
 *
 * These #include directives pull in the auto-generated data tables that define
 * every Pokemon's and trainer's graphical data. Each include adds hundreds or
 * thousands of array entries. The tables include:
 *
 * Pokemon Graphics:
 * - front_pic_coordinates.h: X/Y offset and size for each Pokemon's front sprite
 * - front_pic_table.h: Pointers to compressed front sprite data for all ~400 Pokemon
 * - back_pic_coordinates.h: Same for back (battle) sprites
 * - back_pic_table.h: Pointers to compressed back sprite data
 * - palette_table.h: Normal color palette for each Pokemon
 * - shiny_palette_table.h: Shiny (alternate color) palette for each Pokemon
 * - enemy_mon_elevation.h: Y-axis offset for floating/flying Pokemon in battle
 *
 * Trainer Graphics:
 * - front_pic_anims.h: Animation sequences for trainer front sprites
 * - front_pic_tables.h: Sprite data tables for all trainer classes
 * - back_pic_anims.h: Animation sequences for trainer back sprites
 * - back_pic_tables.h: Sprite data tables for trainer back pictures
 *
 * Game Data:
 * - trainer_parties.h: Every trainer's Pokemon team composition
 * - trainer_class_names.h: Display names for trainer classes ("Bug Catcher", etc.)
 * - trainers.h: Master trainer data (class, name, items, AI flags, party)
 * - species_names.h: Display names for all Pokemon species
 * - move_names.h: Display names for all moves
 */
#include "data/pokemon_graphics/front_pic_coordinates.h"
#include "data/pokemon_graphics/front_pic_table.h"
#include "data/pokemon_graphics/back_pic_coordinates.h"
#include "data/pokemon_graphics/back_pic_table.h"
#include "data/pokemon_graphics/palette_table.h"
#include "data/pokemon_graphics/shiny_palette_table.h"

#include "data/trainer_graphics/front_pic_anims.h"
#include "data/trainer_graphics/front_pic_tables.h"
#include "data/trainer_graphics/back_pic_anims.h"
#include "data/trainer_graphics/back_pic_tables.h"

#include "data/pokemon_graphics/enemy_mon_elevation.h"

#include "data/trainer_parties.h"
#include "data/text/trainer_class_names.h"
#include "data/trainers.h"
#include "data/text/species_names.h"
#include "data/text/move_names.h"
