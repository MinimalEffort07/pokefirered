/**
 * =pokemon_firered BATTLE AI SCRIPT COMMANDS=
 *
 * FILE OVERVIEW:
 * This file implements the AI (Artificial Intelligence) decision engine for
 * opponent trainers in battle. Rather than using hardcoded C logic, the GBA
 * Pokemon games use a custom SCRIPTING LANGUAGE for AI behavior. This file
 * is the INTERPRETER (virtual machine) that executes those scripts.
 *
 * ARCHITECTURE -- THE AI SCRIPTING VIRTUAL MACHINE:
 * The AI system works like a tiny programming language:
 *   1. Each trainer has AI "flags" that select which AI scripts to run
 *   2. Scripts are stored as byte arrays in ROM (see battle_ai_scripts.s)
 *   3. A script pointer (sAIScriptPtr) walks through the bytecode
 *   4. Each byte is an OPCODE (command number) that indexes into sBattleAICmdTable
 *   5. The corresponding C function executes, reads its arguments from subsequent
 *      bytes, and advances sAIScriptPtr past the full instruction
 *
 * THE SCORING SYSTEM:
 * The AI evaluates each of the battler's 4 moves by running scripts that
 * adjust a SCORE for each move (starting at 100). After all scripts run,
 * the move with the highest score is chosen. If there are ties, one is
 * picked randomly. Scripts can increase or decrease scores based on
 * conditions like type effectiveness, HP levels, status conditions, etc.
 *
 * WHY SCRIPTING INSTEAD OF C CODE?
 * Using a scripting VM lets Game Freak's designers tweak AI behavior
 * without recompiling C code. Different trainers can have different AI
 * "personalities" just by pointing to different scripts. This is a common
 * pattern in game development called "data-driven design."
 */
#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "util.h"
#include "item.h"
#include "random.h"
#include "battle_ai_script_commands.h"
#include "constants/abilities.h"
#include "constants/battle_ai.h"
#include "constants/battle_move_effects.h"
#include "constants/moves.h"

/* AI action flags — these are bit flags (each uses one bit) that can be
 * combined with bitwise OR to indicate what the AI has decided to do.
 * Using bit flags means multiple states can be active simultaneously. */
#define AI_ACTION_DONE          0x0001  /* Bit 0: AI has finished evaluating this move */
#define AI_ACTION_FLEE          0x0002  /* Bit 1: AI wants to flee (Safari Zone) */
#define AI_ACTION_WATCH         0x0004  /* Bit 2: AI wants to watch (Safari Zone) */
#define AI_ACTION_DO_NOT_ATTACK 0x0008  /* Bit 3: AI should not attack this turn */
#define AI_ACTION_UNK5          0x0010  /* Bit 4: Unknown/unused */
#define AI_ACTION_UNK6          0x0020  /* Bit 5: Unknown/unused */
#define AI_ACTION_UNK7          0x0040  /* Bit 6: Unknown/unused */
#define AI_ACTION_UNK8          0x0080  /* Bit 7: Unknown/unused */

/* Convenience macros to access the AI's working memory.
 * AI_THINKING_STRUCT holds the current evaluation state (scores, flags, etc.)
 * BATTLE_HISTORY tracks what the AI has observed about the player's Pokemon
 * (moves used, abilities revealed, held items seen). */
#define AI_THINKING_STRUCT (gBattleResources->ai)
#define BATTLE_HISTORY (gBattleResources->battleHistory)

/* AI processing states — this is a simple state machine that controls
 * the flow of AI script execution for each move being evaluated. */
enum
{
    AIState_SettingUp,            /* Preparing to evaluate the next move */
    AIState_Processing,           /* Currently executing AI script commands */
    AIState_FinishedProcessing,   /* All moves have been evaluated */
    AIState_DoNotProcess          /* Skip processing (used for matching only) */
};

/*
 * sAIScriptPtr — the "program counter" of the AI virtual machine.
 *
 * This pointer walks through AI script bytecode stored in ROM. Each AI command
 * reads its opcode and arguments from sAIScriptPtr, then advances the pointer
 * past the instruction. The number of bytes to skip varies per command:
 *   - A simple command with no args: advance by 1 byte (just the opcode)
 *   - A conditional with a byte arg + 4-byte jump target: advance by 6 bytes
 *   - Commands with status word args: advance by 10 bytes
 *
 * EWRAM_DATA means this is stored in EWRAM (External Work RAM, 256KB at
 * 0x02000000), the GBA's general-purpose writable memory. Static variables
 * go here because the GBA has limited IWRAM (32KB fast internal RAM).
 *
 * Refer to battle_ai_scripts.s for the actual AI scripts this interprets.
 */
static EWRAM_DATA const u8 *sAIScriptPtr = NULL;

/* Table of pointers to AI script bytecode in ROM, indexed by AI logic ID.
 * Each entry corresponds to a different AI behavior script (check bad moves,
 * try to faint, check viability, etc.). */
extern u8 *gBattleAI_ScriptsTable[];

static void Cmd_if_random_less_than(void);
static void Cmd_if_random_greater_than(void);
static void Cmd_if_random_equal(void);
static void Cmd_if_random_not_equal(void);
static void Cmd_score(void);
static void Cmd_if_hp_less_than(void);
static void Cmd_if_hp_more_than(void);
static void Cmd_if_hp_equal(void);
static void Cmd_if_hp_not_equal(void);
static void Cmd_if_status(void);
static void Cmd_if_not_status(void);
static void Cmd_if_status2(void);
static void Cmd_if_not_status2(void);
static void Cmd_if_status3(void);
static void Cmd_if_not_status3(void);
static void Cmd_if_side_affecting(void);
static void Cmd_if_not_side_affecting(void);
static void Cmd_if_less_than(void);
static void Cmd_if_more_than(void);
static void Cmd_if_equal(void);
static void Cmd_if_not_equal(void);
static void Cmd_if_less_than_ptr(void);
static void Cmd_if_more_than_ptr(void);
static void Cmd_if_equal_ptr(void);
static void Cmd_if_not_equal_ptr(void);
static void Cmd_if_move(void);
static void Cmd_if_not_move(void);
static void Cmd_if_in_bytes(void);
static void Cmd_if_not_in_bytes(void);
static void Cmd_if_in_hwords(void);
static void Cmd_if_not_in_hwords(void);
static void Cmd_if_user_has_attacking_move(void);
static void Cmd_if_user_has_no_attacking_moves(void);
static void Cmd_get_turn_count(void);
static void Cmd_get_type(void);
static void Cmd_get_considered_move_power(void);
static void Cmd_get_how_powerful_move_is(void);
static void Cmd_get_last_used_battler_move(void);
static void Cmd_if_equal_(void);
static void Cmd_if_not_equal_(void);
static void Cmd_if_would_go_first(void);
static void Cmd_if_would_not_go_first(void);
static void Cmd_nullsub_2A(void);
static void Cmd_nullsub_2B(void);
static void Cmd_count_alive_pokemon(void);
static void Cmd_get_considered_move(void);
static void Cmd_get_considered_move_effect(void);
static void Cmd_get_ability(void);
static void Cmd_get_highest_type_effectiveness(void);
static void Cmd_if_type_effectiveness(void);
static void Cmd_nullsub_32(void);
static void Cmd_nullsub_33(void);
static void Cmd_if_status_in_party(void);
static void Cmd_if_status_not_in_party(void);
static void Cmd_get_weather(void);
static void Cmd_if_effect(void);
static void Cmd_if_not_effect(void);
static void Cmd_if_stat_level_less_than(void);
static void Cmd_if_stat_level_more_than(void);
static void Cmd_if_stat_level_equal(void);
static void Cmd_if_stat_level_not_equal(void);
static void Cmd_if_can_faint(void);
static void Cmd_if_cant_faint(void);
static void Cmd_if_has_move(void);
static void Cmd_if_doesnt_have_move(void);
static void Cmd_if_has_move_with_effect(void);
static void Cmd_if_doesnt_have_move_with_effect(void);
static void Cmd_if_any_move_disabled_or_encored(void);
static void Cmd_if_curr_move_disabled_or_encored(void);
static void Cmd_flee(void);
static void Cmd_if_random_safari_flee(void);
static void Cmd_watch(void);
static void Cmd_get_hold_effect(void);
static void Cmd_get_gender(void);
static void Cmd_is_first_turn_for(void);
static void Cmd_get_stockpile_count(void);
static void Cmd_is_double_battle(void);
static void Cmd_get_used_held_item(void);
static void Cmd_get_move_type_from_result(void);
static void Cmd_get_move_power_from_result(void);
static void Cmd_get_move_effect_from_result(void);
static void Cmd_get_protect_count(void);
static void Cmd_nullsub_52(void);
static void Cmd_nullsub_53(void);
static void Cmd_nullsub_54(void);
static void Cmd_nullsub_55(void);
static void Cmd_nullsub_56(void);
static void Cmd_nullsub_57(void);
static void Cmd_call(void);
static void Cmd_goto(void);
static void Cmd_end(void);
static void Cmd_if_level_compare(void);
static void Cmd_if_target_taunted(void);
static void Cmd_if_target_not_taunted(void);

static void RecordLastUsedMoveByTarget(void);
static void BattleAI_DoAIProcessing(void);
static void AIStackPushVar(const u8 *ptr);
static bool8 AIStackPop(void);

/* Function pointer type for AI script commands. Every command takes no
 * parameters (it reads arguments directly from sAIScriptPtr) and returns
 * nothing (it modifies AI_THINKING_STRUCT and advances sAIScriptPtr). */
typedef void (*BattleAICmdFunc)(void);

/**
 * THE AI COMMAND TABLE — the heart of the scripting VM.
 *
 * Each entry maps an opcode number (array index) to a C function.
 * When the VM reads byte 0x04 from the script, it calls sBattleAICmdTable[4],
 * which is Cmd_score. This is the same dispatch pattern used by the main
 * battle script engine (battle_script_commands.c) — it's how Pokemon games
 * implement their custom scripting languages on the GBA.
 *
 * Commands fall into several categories:
 *   0x00-0x03: Random number conditionals (add randomness to AI decisions)
 *   0x04:      Score adjustment (the core mechanism for choosing moves)
 *   0x05-0x08: HP comparison conditionals
 *   0x09-0x10: Status condition checks (paralysis, sleep, confusion, etc.)
 *   0x11-0x18: General comparison/branching
 *   0x19-0x20: Move and moveset queries
 *   0x21-0x30: Getter commands (retrieve battle state into funcResult)
 *   0x31-0x3E: Type effectiveness and stat checks
 *   0x3F-0x44: Move and effect queries
 *   0x45-0x47: Safari Zone special actions (flee, watch)
 *   0x48-0x57: Miscellaneous getters and null stubs
 *   0x58-0x5A: Control flow (call, goto, end — like subroutines)
 *   0x5B-0x5D: Level comparison and taunt checks
 */
static const BattleAICmdFunc sBattleAICmdTable[] =
{
    Cmd_if_random_less_than,              // 0x0
    Cmd_if_random_greater_than,           // 0x1
    Cmd_if_random_equal,                  // 0x2
    Cmd_if_random_not_equal,              // 0x3
    Cmd_score,                            // 0x4
    Cmd_if_hp_less_than,                  // 0x5
    Cmd_if_hp_more_than,                  // 0x6
    Cmd_if_hp_equal,                      // 0x7
    Cmd_if_hp_not_equal,                  // 0x8
    Cmd_if_status,                        // 0x9
    Cmd_if_not_status,                    // 0xA
    Cmd_if_status2,                       // 0xB
    Cmd_if_not_status2,                   // 0xC
    Cmd_if_status3,                       // 0xD
    Cmd_if_not_status3,                   // 0xE
    Cmd_if_side_affecting,                // 0xF
    Cmd_if_not_side_affecting,            // 0x10
    Cmd_if_less_than,                     // 0x11
    Cmd_if_more_than,                     // 0x12
    Cmd_if_equal,                         // 0x13
    Cmd_if_not_equal,                     // 0x14
    Cmd_if_less_than_ptr,                 // 0x15
    Cmd_if_more_than_ptr,                 // 0x16
    Cmd_if_equal_ptr,                     // 0x17
    Cmd_if_not_equal_ptr,                 // 0x18
    Cmd_if_move,                          // 0x19
    Cmd_if_not_move,                      // 0x1A
    Cmd_if_in_bytes,                      // 0x1B
    Cmd_if_not_in_bytes,                  // 0x1C
    Cmd_if_in_hwords,                     // 0x1D
    Cmd_if_not_in_hwords,                 // 0x1E
    Cmd_if_user_has_attacking_move,       // 0x1F
    Cmd_if_user_has_no_attacking_moves,   // 0x20
    Cmd_get_turn_count,                   // 0x21
    Cmd_get_type,                         // 0x22
    Cmd_get_considered_move_power,        // 0x23
    Cmd_get_how_powerful_move_is,         // 0x24
    Cmd_get_last_used_battler_move,       // 0x25
    Cmd_if_equal_,                        // 0x26
    Cmd_if_not_equal_,                    // 0x27
    Cmd_if_would_go_first,                // 0x28
    Cmd_if_would_not_go_first,            // 0x29
    Cmd_nullsub_2A,                       // 0x2A
    Cmd_nullsub_2B,                       // 0x2B
    Cmd_count_alive_pokemon,              // 0x2C
    Cmd_get_considered_move,              // 0x2D
    Cmd_get_considered_move_effect,       // 0x2E
    Cmd_get_ability,                      // 0x2F
    Cmd_get_highest_type_effectiveness,   // 0x30
    Cmd_if_type_effectiveness,            // 0x31
    Cmd_nullsub_32,                       // 0x32
    Cmd_nullsub_33,                       // 0x33
    Cmd_if_status_in_party,               // 0x34
    Cmd_if_status_not_in_party,           // 0x35
    Cmd_get_weather,                      // 0x36
    Cmd_if_effect,                        // 0x37
    Cmd_if_not_effect,                    // 0x38
    Cmd_if_stat_level_less_than,          // 0x39
    Cmd_if_stat_level_more_than,          // 0x3A
    Cmd_if_stat_level_equal,              // 0x3B
    Cmd_if_stat_level_not_equal,          // 0x3C
    Cmd_if_can_faint,                     // 0x3D
    Cmd_if_cant_faint,                    // 0x3E
    Cmd_if_has_move,                      // 0x3F
    Cmd_if_doesnt_have_move,              // 0x40
    Cmd_if_has_move_with_effect,          // 0x41
    Cmd_if_doesnt_have_move_with_effect,  // 0x42
    Cmd_if_any_move_disabled_or_encored,  // 0x43
    Cmd_if_curr_move_disabled_or_encored, // 0x44
    Cmd_flee,                             // 0x45
    Cmd_if_random_safari_flee,            // 0x46
    Cmd_watch,                            // 0x47
    Cmd_get_hold_effect,                  // 0x48
    Cmd_get_gender,                       // 0x49
    Cmd_is_first_turn_for,                // 0x4A
    Cmd_get_stockpile_count,              // 0x4B
    Cmd_is_double_battle,                 // 0x4C
    Cmd_get_used_held_item,               // 0x4D
    Cmd_get_move_type_from_result,        // 0x4E
    Cmd_get_move_power_from_result,       // 0x4F
    Cmd_get_move_effect_from_result,      // 0x50
    Cmd_get_protect_count,                // 0x51
    Cmd_nullsub_52,                       // 0x52
    Cmd_nullsub_53,                       // 0x53
    Cmd_nullsub_54,                       // 0x54
    Cmd_nullsub_55,                       // 0x55
    Cmd_nullsub_56,                       // 0x56
    Cmd_nullsub_57,                       // 0x57
    Cmd_call,                             // 0x58
    Cmd_goto,                             // 0x59
    Cmd_end,                              // 0x5A
    Cmd_if_level_compare,                 // 0x5B
    Cmd_if_target_taunted,                // 0x5C
    Cmd_if_target_not_taunted,            // 0x5D
};

/**
 * Moves whose effects make them "discouraged" for power comparison purposes.
 *
 * When the AI evaluates which move is "most powerful," it ignores moves with
 * these effects because they have serious drawbacks (self-KO, charging turns,
 * recharge turns, stat drops, etc.). This prevents the AI from always picking
 * Explosion just because it has the highest base power.
 *
 * The list is terminated by 0xFFFF, a common sentinel value pattern in GBA
 * games — the loop checks for this to know when to stop iterating.
 */
static const u16 sDiscouragedPowerfulMoveEffects[] =
{
    EFFECT_EXPLOSION,     /* Self-destructs (Explosion, Self-Destruct) */
    EFFECT_DREAM_EATER,   /* Only works on sleeping targets */
    EFFECT_RAZOR_WIND,    /* Requires a charging turn */
    EFFECT_SKY_ATTACK,    /* Requires a charging turn */
    EFFECT_RECHARGE,      /* Must recharge after use (Hyper Beam) */
    EFFECT_SKULL_BASH,    /* Requires a charging turn */
    EFFECT_SOLAR_BEAM,    /* Requires a charging turn (unless sunny) */
    EFFECT_SPIT_UP,       /* Requires Stockpile stacks */
    EFFECT_FOCUS_PUNCH,   /* Fails if hit before attacking */
    EFFECT_SUPERPOWER,    /* Lowers own Attack and Defense */
    EFFECT_ERUPTION,      /* Power depends on current HP */
    EFFECT_OVERHEAT,      /* Sharply lowers own Special Attack */
    0xFFFF                /* Sentinel: marks end of list */
};

/**
 * FUNCTION: BattleAI_HandleItemUseBeforeAISetup
 *
 * PURPOSE: Initializes the AI's battle history and loads the trainer's usable
 * items before setting up the main AI decision-making data.
 *
 * HOW IT WORKS:
 * 1. Clears all battle history (previously observed moves, abilities, items)
 * 2. If this is a regular trainer battle (not Safari, Link, Battle Tower, etc.),
 *    copies the trainer's item list from ROM into the AI's working memory
 * 3. Calls BattleAI_SetupAIData to prepare the scoring system
 *
 * GAME LOGIC:
 * Trainers in Pokemon can use items during battle (like Full Restores, X items).
 * Each trainer's data in ROM includes up to MAX_TRAINER_ITEMS items they can use.
 * This function loads those items so the AI can decide when to use them later
 * (handled separately in battle_ai_switch_items.c).
 */
void BattleAI_HandleItemUseBeforeAISetup(void)
{
    s32 i;
    u8 *data = (u8 *)BATTLE_HISTORY;

    /* Clear the entire battle history struct by treating it as a raw byte array.
     * This is a common GBA pattern — memset wasn't always available or efficient,
     * so developers would manually zero memory byte-by-byte. */
    for (i = 0; i < sizeof(struct BattleHistory); i++)
        data[i] = 0;

    /* Items are allowed to use in ONLY regular trainer battles.
     * The AI doesn't use items in Safari Zone, Link battles, Battle Tower,
     * e-Reader battles, or Secret Base battles — those have special rules. */
    if ((gBattleTypeFlags & BATTLE_TYPE_TRAINER)
        && (gTrainerBattleOpponent_A != TRAINER_SECRET_BASE)
        && !(gBattleTypeFlags & (BATTLE_TYPE_TRAINER_TOWER | BATTLE_TYPE_EREADER_TRAINER | BATTLE_TYPE_BATTLE_TOWER | BATTLE_TYPE_SAFARI | BATTLE_TYPE_LINK))
        )
    {
        /* Copy non-empty items from the trainer's ROM data into the AI's
         * battle history. itemsNo tracks how many items have been loaded. */
        for (i = 0; i < MAX_TRAINER_ITEMS; i++)
        {
            if (gTrainers[gTrainerBattleOpponent_A].items[i] != 0)
            {
                BATTLE_HISTORY->trainerItems[BATTLE_HISTORY->itemsNo] = gTrainers[gTrainerBattleOpponent_A].items[i];
                BATTLE_HISTORY->itemsNo++;
            }
        }
    }

    BattleAI_SetupAIData();
}

/**
 * FUNCTION: BattleAI_SetupAIData
 *
 * PURPOSE: Initializes the AI scoring system and selects which AI scripts
 * to run based on the battle type and trainer data.
 *
 * HOW IT WORKS:
 * 1. Clears all AI thinking data and sets every move's score to 100
 * 2. Zeroes out scores for moves that can't be used (no PP, disabled, etc.)
 * 3. Generates random damage roll values for each move (simulating the
 *    game's built-in damage variance of 85-100%)
 * 4. Sets up attacker/target battler IDs
 * 5. Selects which AI scripts to run based on battle type
 *
 * GAME LOGIC:
 * The base score of 100 is a neutral starting point. AI scripts then add
 * or subtract from this based on how good/bad each move seems. A move that
 * ends up at 110 is preferred over one at 90. The simulated RNG values
 * (84-100) let the AI estimate damage ranges without rolling new randoms.
 */
void BattleAI_SetupAIData(void)
{
    s32 i;
    u8 *data = (u8 *)AI_THINKING_STRUCT;
    u8 moveLimitations;

    /* Clear the AI thinking struct, then set all 4 move scores to 100.
     * 100 is the "neutral" score — scripts will adjust up or down from here. */
    for (i = 0; i < sizeof(struct AI_ThinkingStruct); i++)
        data[i] = 0;

    for (i = 0; i < MAX_MON_MOVES; i++)
        AI_THINKING_STRUCT->score[i] = 100;

    /* Check which moves are limited (no PP, disabled, taunted, encored, etc.).
     * Returns a bitmask where each bit corresponds to a move slot.
     * 0xFF means check ALL limitation types. */
    moveLimitations = CheckMoveLimitations(gActiveBattler, 0, 0xFF);

    /* Zero out scores for unusable moves and generate simulated damage rolls.
     * gBitTable[i] produces a bitmask with only bit i set (1, 2, 4, 8).
     * If that bit is set in moveLimitations, the move can't be used.
     * simulatedRNG generates values 84-100, simulating Pokemon's built-in
     * damage roll where moves deal 85-100% of calculated damage. */
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (gBitTable[i] & moveLimitations)
            AI_THINKING_STRUCT->score[i] = 0;

        AI_THINKING_STRUCT->simulatedRNG[i] = 100 - (Random() % 16);
    }

    /* Reset the AI call stack (used for call/goto/end subroutine support). */
    gBattleResources->AI_ScriptsStack->size = 0;
    gBattlerAttacker = gActiveBattler;

    /* In double battles, randomly pick one of the two opposing positions.
     * BIT_FLANK (bit 1) distinguishes left vs. right position on each side.
     * If that battler has fainted (absent), flip to the other position.
     * In singles, the target is simply the opponent (XOR with BIT_SIDE flips
     * the side bit, turning player->enemy or enemy->player). */
    if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
    {
        gBattlerTarget = (Random() & BIT_FLANK);

        if (gAbsentBattlerFlags & gBitTable[gBattlerTarget])
            gBattlerTarget ^= BIT_FLANK;
    }
    else
    {
        gBattlerTarget = gBattlerAttacker ^ BIT_SIDE;
    }

    /* Select which AI scripts to run based on battle type.
     * Each battle type uses different AI complexity:
     *   - Safari Zone: only flee/watch logic
     *   - Roamer: special roaming behavior
     *   - Wild scripted: basic "avoid bad moves" only
     *   - Legendaries: smarter (check bad moves + try to faint + check viability)
     *   - Battle Tower/etc: also smarter AI
     *   - Regular trainers: use the trainer's custom aiFlags from ROM data */
    if (gBattleTypeFlags & BATTLE_TYPE_SAFARI)
    {
        AI_THINKING_STRUCT->aiFlags = AI_SCRIPT_SAFARI;
        return;
    }
    else if (gBattleTypeFlags & BATTLE_TYPE_ROAMER)
    {
        AI_THINKING_STRUCT->aiFlags = AI_SCRIPT_ROAMING;
        return;
    }
    else if (!(gBattleTypeFlags & (BATTLE_TYPE_TRAINER_TOWER | BATTLE_TYPE_EREADER_TRAINER | BATTLE_TYPE_BATTLE_TOWER)) && (gTrainerBattleOpponent_A != TRAINER_SECRET_BASE))
    {
        if (gBattleTypeFlags & BATTLE_TYPE_WILD_SCRIPTED)
        {
            AI_THINKING_STRUCT->aiFlags = AI_SCRIPT_CHECK_BAD_MOVE;
            return;
        }
        else if (gBattleTypeFlags & BATTLE_TYPE_LEGENDARY_FRLG)
        {
            AI_THINKING_STRUCT->aiFlags = (AI_SCRIPT_CHECK_BAD_MOVE | AI_SCRIPT_TRY_TO_FAINT | AI_SCRIPT_CHECK_VIABILITY);
            return;
        }
    }
    else
    {
        AI_THINKING_STRUCT->aiFlags = (AI_SCRIPT_CHECK_BAD_MOVE | AI_SCRIPT_TRY_TO_FAINT | AI_SCRIPT_CHECK_VIABILITY);
        return;
    }
    /* For regular trainer battles, use the trainer-specific AI flags defined
     * in the trainer's ROM data. Different trainers can be smarter or dumber
     * depending on which flags are set. */
    AI_THINKING_STRUCT->aiFlags = gTrainers[gTrainerBattleOpponent_A].aiFlags;
}

/**
 * FUNCTION: BattleAI_ChooseMoveOrAction
 *
 * PURPOSE: The main entry point for AI move selection — runs all applicable
 * AI scripts and returns the index of the chosen move (0-3) or a special
 * action code (flee/watch).
 *
 * HOW IT WORKS:
 * 1. Records what move the target (player) last used for future AI reference
 * 2. Iterates through each AI flag bit — each set bit triggers a different
 *    AI script that adjusts move scores
 * 3. After all scripts have run, finds the move(s) with the highest score
 * 4. If there's a tie, randomly picks among the tied moves
 *
 * GAME LOGIC:
 * This is where the AI's "personality" comes together. A simple trainer
 * might only have AI_SCRIPT_CHECK_BAD_MOVE enabled (avoid obviously bad
 * choices), while an Elite Four member might have multiple scripts that
 * check type effectiveness, try to KO, and consider stat boosts. The more
 * scripts that run, the "smarter" the AI appears.
 *
 * RETURNS: Move slot index (0-3), AI_CHOICE_FLEE, or AI_CHOICE_WATCH
 */
u8 BattleAI_ChooseMoveOrAction(void)
{
    u8 currentMoveArray[MAX_MON_MOVES];
    u8 consideredMoveArray[MAX_MON_MOVES];
    u8 numOfBestMoves;
    s32 i;

    /* Record what the player used last turn so AI scripts can reference it. */
    RecordLastUsedMoveByTarget();

    /* Process each AI script. aiFlags is a bitmask where each bit enables
     * a different AI script. We check bit 0, run that script if set, then
     * right-shift to check the next bit. aiLogicId tracks which script
     * index we're on (indexes into gBattleAI_ScriptsTable). */
    while (AI_THINKING_STRUCT->aiFlags != 0)
    {
        if (AI_THINKING_STRUCT->aiFlags & 1)
        {
            AI_THINKING_STRUCT->aiState = AIState_SettingUp;
            BattleAI_DoAIProcessing();
        }
        AI_THINKING_STRUCT->aiFlags >>= 1;  /* Shift to check next flag bit */
        AI_THINKING_STRUCT->aiLogicId++;     /* Move to next script index */
        AI_THINKING_STRUCT->movesetIndex = 0; /* Reset to evaluate move 0 again */
    }

    /* Safari Zone special actions — the AI might flee or watch instead of attacking. */
    if (AI_THINKING_STRUCT->aiAction & AI_ACTION_FLEE)
        return AI_CHOICE_FLEE;
    if (AI_THINKING_STRUCT->aiAction & AI_ACTION_WATCH)
        return AI_CHOICE_WATCH;

    /* Find the highest-scoring move(s). Start by assuming move 0 is best. */
    numOfBestMoves = 1;
    currentMoveArray[0] = AI_THINKING_STRUCT->score[0];
    consideredMoveArray[0] = 0;

    /* Compare each subsequent move's score against the current best. */
    for (i = 1; i < MAX_MON_MOVES; i++)
    {
        if (currentMoveArray[0] < AI_THINKING_STRUCT->score[i])
        {
            /* Found a new best move — reset the "best" list to just this one. */
            numOfBestMoves = 1;
            currentMoveArray[0] = AI_THINKING_STRUCT->score[i];
            consideredMoveArray[0] = i;
        }
        if (currentMoveArray[0] == AI_THINKING_STRUCT->score[i])
        {
            /* Tied with the current best — add it to the tie list. */
            currentMoveArray[numOfBestMoves] = AI_THINKING_STRUCT->score[i];
            consideredMoveArray[numOfBestMoves++] = i;
        }
    }

    /* If multiple moves are tied for best score, pick one randomly.
     * This prevents the AI from being completely predictable. */
    return consideredMoveArray[Random() % numOfBestMoves];
}

/**
 * FUNCTION: BattleAI_DoAIProcessing
 *
 * PURPOSE: The inner loop of the AI virtual machine — executes one AI script
 * against all 4 move slots of the current Pokemon.
 *
 * HOW IT WORKS:
 * This is a state machine with 3 meaningful states:
 *   SettingUp: Load the script pointer and current move for evaluation
 *   Processing: Execute script commands one at a time until AI_ACTION_DONE
 *   FinishedProcessing: Exit the loop
 *
 * For each move slot (0-3), the script runs completely (from start to "end"
 * command), adjusting that move's score. Then the state resets to SettingUp
 * for the next move, and the same script runs again for that move.
 *
 * GAME LOGIC:
 * This is the "CPU" of the AI VM. The key line is:
 *   sBattleAICmdTable[*sAIScriptPtr]()
 * It reads one byte from the script, uses it as an index into the command
 * table, and calls the corresponding function. That function processes its
 * arguments and advances sAIScriptPtr, so the next iteration reads the
 * next command. This continues until a Cmd_end sets AI_ACTION_DONE.
 */
static void BattleAI_DoAIProcessing(void)
{
    while (AI_THINKING_STRUCT->aiState != AIState_FinishedProcessing)
    {
        switch (AI_THINKING_STRUCT->aiState)
        {
        case AIState_DoNotProcess: /* Dead state — needed for compiler match. */
            break;
        case AIState_SettingUp:
            /* Point the script pointer to the start of the current AI script.
             * aiLogicId selects which script (0 = check bad moves, etc.). */
            sAIScriptPtr = gBattleAI_ScriptsTable[AI_THINKING_STRUCT->aiLogicId];

            /* Load the move to evaluate. If it has no PP, set it to 0
             * (empty/invalid) so the script will skip it immediately. */
            if (gBattleMons[gBattlerAttacker].pp[AI_THINKING_STRUCT->movesetIndex] == 0)
            {
                AI_THINKING_STRUCT->moveConsidered = 0;
            }
            else
            {
                AI_THINKING_STRUCT->moveConsidered = gBattleMons[gBattlerAttacker].moves[AI_THINKING_STRUCT->movesetIndex];
            }
            AI_THINKING_STRUCT->aiState++; /* Transition to Processing */
            break;
        case AIState_Processing:
            if (AI_THINKING_STRUCT->moveConsidered != 0)
            {
                /* THE HEART OF THE VM: read the opcode byte at sAIScriptPtr,
                 * look it up in the command table, and call that function.
                 * Each function will read its own arguments from sAIScriptPtr
                 * and advance the pointer when done. */
                sBattleAICmdTable[*sAIScriptPtr]();
            }
            else
            {
                /* Empty move slot (no PP or no move) — score it at 0 and
                 * mark as done so the AI never selects it. */
                AI_THINKING_STRUCT->score[AI_THINKING_STRUCT->movesetIndex] = 0;
                AI_THINKING_STRUCT->aiAction |= AI_ACTION_DONE;
            }
            if (AI_THINKING_STRUCT->aiAction & AI_ACTION_DONE)
            {
                AI_THINKING_STRUCT->movesetIndex++;

                /* If more moves remain and attacking is allowed, go back to
                 * SettingUp to evaluate the next move with the same script. */
                if (AI_THINKING_STRUCT->movesetIndex < MAX_MON_MOVES && (AI_THINKING_STRUCT->aiAction & AI_ACTION_DO_NOT_ATTACK) == 0)
                    AI_THINKING_STRUCT->aiState = AIState_SettingUp;
                else
                    AI_THINKING_STRUCT->aiState++; /* Done — advance to FinishedProcessing */

                /* Clear AI_ACTION_DONE but preserve other persistent flags
                 * (flee, watch, do-not-attack). The AND mask keeps all bits
                 * EXCEPT bit 0 (AI_ACTION_DONE). */
                AI_THINKING_STRUCT->aiAction &= (AI_ACTION_FLEE | AI_ACTION_WATCH | AI_ACTION_DO_NOT_ATTACK |
                AI_ACTION_UNK5 | AI_ACTION_UNK6 | AI_ACTION_UNK7 | AI_ACTION_UNK8);
            }
            break;
        }
    }
}

/**
 * FUNCTION: RecordLastUsedMoveByTarget
 *
 * PURPOSE: Records the most recent move used by the target (player's Pokemon)
 * into the AI's battle history so future AI scripts can reference it.
 *
 * GAME LOGIC:
 * The AI tracks up to 8 moves the player has used. This lets AI scripts check
 * "has the player used move X?" and make decisions accordingly (e.g., if the
 * player used Swords Dance, the AI might prioritize phasing moves).
 * gBattlerTarget >> 1 divides by 2, mapping battler IDs to party side indices
 * (battlers 0,1 = player side index 0; battlers 2,3 = enemy side index 1).
 */
static void RecordLastUsedMoveByTarget(void)
{
    s32 i;

    for (i = 0; i < 8; i++)
    {
        if (BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i] == 0)
        {
            BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i] = gLastMoves[gBattlerTarget];
            return;
        }
    }
}

/**
 * FUNCTION: ClearBattlerMoveHistory (unused)
 *
 * PURPOSE: Clears all recorded moves for a given battler. Not called anywhere
 * in the game — possibly leftover from development or reserved for future use.
 */
static void ClearBattlerMoveHistory(u8 battlerId)
{
    s32 i;

    for (i = 0; i < 8; i++)
        BATTLE_HISTORY->usedMoves[battlerId / 2][i] = MOVE_NONE;
}

/**
 * FUNCTION: RecordAbilityBattle
 *
 * PURPOSE: Records a Pokemon's ability into battle history when it activates,
 * so the AI can reference it in future turns.
 *
 * GAME LOGIC:
 * The AI doesn't automatically know the player's ability — it only learns it
 * when it activates in battle (e.g., Intimidate triggers on switch-in). This
 * simulates "fair" AI that doesn't cheat by reading hidden data. Only records
 * for the player's side (side 0), since the AI already knows its own abilities.
 */
void RecordAbilityBattle(u8 battlerId, u8 abilityId)
{
    if (GetBattlerSide(battlerId) == 0)
        BATTLE_HISTORY->abilities[GET_BATTLER_SIDE(battlerId)] = abilityId;
}

/**
 * FUNCTION: RecordItemEffectBattle
 *
 * PURPOSE: Records a held item's effect when it activates in battle, so the
 * AI can consider it in future decisions.
 *
 * GAME LOGIC:
 * Like abilities, the AI only learns about the player's held items when they
 * activate (e.g., Leftovers healing, Focus Band preventing fainting). This
 * prevents the AI from "cheating" by knowing items it hasn't observed.
 */
void RecordItemEffectBattle(u8 battlerId, u8 itemEffect)
{
    if (GetBattlerSide(battlerId) == 0)
        BATTLE_HISTORY->itemEffects[GET_BATTLER_SIDE(battlerId)] = itemEffect;
}

/* ========================================================================
 * AI SCRIPT COMMAND IMPLEMENTATIONS
 * ========================================================================
 *
 * Each function below implements one AI script opcode. They all follow
 * the same pattern:
 *
 * READING ARGUMENTS:
 *   sAIScriptPtr[0] = the opcode itself (already consumed by the dispatcher)
 *   sAIScriptPtr[1], [2], etc. = arguments encoded in the script bytecode
 *   T1_READ_PTR(ptr) = reads a 4-byte pointer from unaligned memory
 *   T1_READ_16(ptr)  = reads a 2-byte value from unaligned memory
 *   T1_READ_32(ptr)  = reads a 4-byte value from unaligned memory
 *
 * ADVANCING THE POINTER:
 *   After processing, sAIScriptPtr advances by the total instruction size.
 *   For conditional commands, there are two paths:
 *     - Condition TRUE:  sAIScriptPtr = T1_READ_PTR(...) — jump to target
 *     - Condition FALSE: sAIScriptPtr += N — skip past this instruction
 *
 * AI_USER vs AI_TARGET:
 *   sAIScriptPtr[1] often specifies which battler to query:
 *     AI_USER   = the AI's own Pokemon (gBattlerAttacker)
 *     AI_TARGET = the opponent's Pokemon (gBattlerTarget)
 *
 * INSTRUCTION ENCODING EXAMPLE (Cmd_if_random_less_than):
 *   Byte 0: opcode (0x00)
 *   Byte 1: threshold value (0-255)
 *   Bytes 2-5: 4-byte jump target pointer (if condition is true)
 *   Total: 6 bytes (skip 6 on false, jump to target on true)
 * ======================================================================== */

/**
 * FUNCTION: Cmd_if_random_less_than
 *
 * PURPOSE: Jumps to a target script address if a random number (0-255) is
 * less than the threshold value. Used to add controlled randomness to AI
 * decisions — e.g., "30% chance to use this strategy."
 *
 * SCRIPT ENCODING: [opcode] [threshold] [4-byte jump ptr] = 6 bytes
 */
static void Cmd_if_random_less_than(void)
{
    if (Random() % 256 < sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);  /* Condition true: jump */
    else
        sAIScriptPtr += 6;  /* Condition false: skip to next instruction */
}

static void Cmd_if_random_greater_than(void)
{
    if (Random() % 256 > sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_random_equal(void)
{
    if (Random() % 256 == sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_random_not_equal(void)
{
    if (Random() % 256 != sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

/**
 * FUNCTION: Cmd_score
 *
 * PURPOSE: THE MOST IMPORTANT AI COMMAND — adjusts the score of the
 * currently considered move. This is how AI scripts express preferences.
 *
 * HOW IT WORKS:
 * sAIScriptPtr[1] is a SIGNED byte (-128 to +127) added to the move's score.
 * Positive values make the move more likely to be chosen; negative values
 * make it less likely. If the score drops below 0, it's clamped to 0.
 *
 * EXAMPLE: If the AI script detects the move is super effective against
 * the target, it might execute "score +10" to boost that move's priority.
 * If the target is immune, it might do "score -100" to virtually eliminate it.
 *
 * SCRIPT ENCODING: [opcode 0x04] [signed_adjustment] = 2 bytes
 */
static void Cmd_score(void)
{
    /* Add the signed adjustment to the current move's score. */
    AI_THINKING_STRUCT->score[AI_THINKING_STRUCT->movesetIndex] += sAIScriptPtr[1];

    /* Clamp to 0 — negative scores don't make sense. */
    if (AI_THINKING_STRUCT->score[AI_THINKING_STRUCT->movesetIndex] < 0)
        AI_THINKING_STRUCT->score[AI_THINKING_STRUCT->movesetIndex] = 0;

    sAIScriptPtr += 2;
}

/**
 * FUNCTION: Cmd_if_hp_less_than
 *
 * PURPOSE: Jumps if the specified battler's HP is below a percentage threshold.
 * Used for strategies like "if my HP is below 25%, use a healing move."
 *
 * SCRIPT ENCODING: [opcode] [AI_USER/AI_TARGET] [percentage] [4-byte ptr] = 7 bytes
 * The HP percentage is calculated as: (currentHP * 100) / maxHP
 */
static void Cmd_if_hp_less_than(void)
{
    u16 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    /* Calculate HP as a percentage (0-100) and compare to the threshold. */
    if ((u32)(100 * gBattleMons[battlerId].hp / gBattleMons[battlerId].maxHP) < sAIScriptPtr[2])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    else
        sAIScriptPtr += 7;
}

static void Cmd_if_hp_more_than(void)
{
    u16 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if ((u32)(100 * gBattleMons[battlerId].hp / gBattleMons[battlerId].maxHP) > sAIScriptPtr[2])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    else
        sAIScriptPtr += 7;
}

static void Cmd_if_hp_equal(void)
{
    u16 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if ((u32)(100 * gBattleMons[battlerId].hp / gBattleMons[battlerId].maxHP) == sAIScriptPtr[2])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    else
        sAIScriptPtr += 7;
}

static void Cmd_if_hp_not_equal(void)
{
    u16 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if ((u32)(100 * gBattleMons[battlerId].hp / gBattleMons[battlerId].maxHP) != sAIScriptPtr[2])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    else
        sAIScriptPtr += 7;
}

/**
 * FUNCTION: Cmd_if_status
 *
 * PURPOSE: Jumps if the specified battler has a particular primary status
 * condition (status1 = sleep, poison, burn, paralysis, freeze, toxic).
 *
 * GAME LOGIC:
 * Pokemon has two categories of status:
 *   status1: "primary" statuses that persist until cured (sleep, poison, etc.)
 *   status2: "volatile" statuses that clear on switch-out (confusion, etc.)
 * The status argument is a 32-bit bitmask — the & operator checks if any of
 * the specified status bits are set.
 *
 * SCRIPT ENCODING: [opcode] [user/target] [4-byte status mask] [4-byte ptr] = 10 bytes
 */
static void Cmd_if_status(void)
{
    u16 battlerId;
    u32 status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    /* Read the 4-byte status bitmask from the script. */
    status = T1_READ_32(sAIScriptPtr + 2);

    /* Check if any of the specified status bits are active. */
    if (gBattleMons[battlerId].status1 & status)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_not_status(void)
{
    u16 battlerId;
    u32 status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    status = T1_READ_32(sAIScriptPtr + 2);

    if (!(gBattleMons[battlerId].status1 & status))
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_status2(void)
{
    u16 battlerId;
    u32 status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    status = T1_READ_32(sAIScriptPtr + 2);

    if ((gBattleMons[battlerId].status2 & status))
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_not_status2(void)
{
    u16 battlerId;
    u32 status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    status = T1_READ_32(sAIScriptPtr + 2);

    if (!(gBattleMons[battlerId].status2 & status))
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_status3(void)
{
    u16 battlerId;
    u32 status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    status = T1_READ_32(sAIScriptPtr + 2);

    if (gStatuses3[battlerId] & status)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_not_status3(void)
{
    u16 battlerId;
    u32 status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    status = T1_READ_32(sAIScriptPtr + 2);

    if (!(gStatuses3[battlerId] & status))
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_side_affecting(void)
{
    u16 battlerId;
    u32 side, status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    side = GET_BATTLER_SIDE(battlerId);
    status = T1_READ_32(sAIScriptPtr + 2);

    if (gSideStatuses[side] & status)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_not_side_affecting(void)
{
    u16 battlerId;
    u32 side, status;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    side = GET_BATTLER_SIDE(battlerId);
    status = T1_READ_32(sAIScriptPtr + 2);

    if (!(gSideStatuses[side] & status))
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
    else
        sAIScriptPtr += 10;
}

static void Cmd_if_less_than(void)
{
    if (AI_THINKING_STRUCT->funcResult < sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_more_than(void)
{
    if (AI_THINKING_STRUCT->funcResult > sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_equal(void)
{
    if (AI_THINKING_STRUCT->funcResult == sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_not_equal(void)
{
    if (AI_THINKING_STRUCT->funcResult != sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_less_than_ptr(void)
{
    const u8 *value = T1_READ_PTR(sAIScriptPtr + 1);

    if (AI_THINKING_STRUCT->funcResult < *value)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
    else
        sAIScriptPtr += 9;
}

static void Cmd_if_more_than_ptr(void)
{
    const u8 *value = T1_READ_PTR(sAIScriptPtr + 1);

    if (AI_THINKING_STRUCT->funcResult > *value)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
    else
        sAIScriptPtr += 9;
}

static void Cmd_if_equal_ptr(void)
{
    const u8 *value = T1_READ_PTR(sAIScriptPtr + 1);

    if (AI_THINKING_STRUCT->funcResult == *value)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
    else
        sAIScriptPtr += 9;
}

static void Cmd_if_not_equal_ptr(void)
{
    const u8 *value = T1_READ_PTR(sAIScriptPtr + 1);

    if (AI_THINKING_STRUCT->funcResult != *value)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
    else
        sAIScriptPtr += 9;
}

static void Cmd_if_move(void)
{
    u16 move = T1_READ_16(sAIScriptPtr + 1);

    if (AI_THINKING_STRUCT->moveConsidered == move)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    else
        sAIScriptPtr += 7;
}

static void Cmd_if_not_move(void)
{
    u16 move = T1_READ_16(sAIScriptPtr + 1);

    if (AI_THINKING_STRUCT->moveConsidered != move)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    else
        sAIScriptPtr += 7;
}

static void Cmd_if_in_bytes(void)
{
    const u8 *ptr = T1_READ_PTR(sAIScriptPtr + 1);

    while (*ptr != 0xFF)
    {
        if (AI_THINKING_STRUCT->funcResult == *ptr)
        {
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
            return;
        }
        ptr++;
    }
    sAIScriptPtr += 9;
}

static void Cmd_if_not_in_bytes(void)
{
    const u8 *ptr = T1_READ_PTR(sAIScriptPtr + 1);

    while (*ptr != 0xFF)
    {
        if (AI_THINKING_STRUCT->funcResult == *ptr)
        {
            sAIScriptPtr += 9;
            return;
        }
        ptr++;
    }
    sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
}

static void Cmd_if_in_hwords(void)
{
    const u16 *ptr = (const u16 *)T1_READ_PTR(sAIScriptPtr + 1);

    while (*ptr != 0xFFFF)
    {
        if (AI_THINKING_STRUCT->funcResult == *ptr)
        {
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
            return;
        }
        ptr++;
    }
    sAIScriptPtr += 9;
}

static void Cmd_if_not_in_hwords(void)
{
    const u16 *ptr = (const u16 *)T1_READ_PTR(sAIScriptPtr + 1);

    while (*ptr != 0xFFFF)
    {
        if (AI_THINKING_STRUCT->funcResult == *ptr)
        {
            sAIScriptPtr += 9;
            return;
        }
        ptr++;
    }
    sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 5);
}

static void Cmd_if_user_has_attacking_move(void)
{
    s32 i;

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (gBattleMons[gBattlerAttacker].moves[i] != 0
            && gBattleMoves[gBattleMons[gBattlerAttacker].moves[i]].power != 0)
            break;
    }

    if (i == MAX_MON_MOVES)
        sAIScriptPtr += 5;
    else
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);
}

static void Cmd_if_user_has_no_attacking_moves(void)
{
    s32 i;

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (gBattleMons[gBattlerAttacker].moves[i] != 0
         && gBattleMoves[gBattleMons[gBattlerAttacker].moves[i]].power != 0)
            break;
    }

    if (i != MAX_MON_MOVES)
        sAIScriptPtr += 5;
    else
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);
}

static void Cmd_get_turn_count(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleResults.battleTurnCounter;
    sAIScriptPtr += 1;
}

static void Cmd_get_type(void)
{
    switch (sAIScriptPtr[1])
    {
    case AI_TYPE1_USER:
        AI_THINKING_STRUCT->funcResult = gBattleMons[gBattlerAttacker].type1;
        break;
    case AI_TYPE1_TARGET:
        AI_THINKING_STRUCT->funcResult = gBattleMons[gBattlerTarget].type1;
        break;
    case AI_TYPE2_USER:
        AI_THINKING_STRUCT->funcResult = gBattleMons[gBattlerAttacker].type2;
        break;
    case AI_TYPE2_TARGET:
        AI_THINKING_STRUCT->funcResult = gBattleMons[gBattlerTarget].type2;
        break;
    case AI_TYPE_MOVE:
        AI_THINKING_STRUCT->funcResult = gBattleMoves[AI_THINKING_STRUCT->moveConsidered].type;
        break;
    }
    sAIScriptPtr += 2;
}

static void Cmd_get_considered_move_power(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleMoves[AI_THINKING_STRUCT->moveConsidered].power;
    sAIScriptPtr += 1;
}

/**
 * FUNCTION: Cmd_get_how_powerful_move_is
 *
 * PURPOSE: Determines whether the currently considered move is the most
 * powerful attacking option available, excluding "discouraged" moves
 * with undesirable side effects.
 *
 * HOW IT WORKS:
 * 1. First checks if the current move itself has a discouraged effect
 *    (if so, returns MOVE_POWER_DISCOURAGED immediately)
 * 2. Calculates estimated damage for ALL non-discouraged attacking moves
 * 3. Compares the current move's damage against all others
 * 4. Returns MOVE_MOST_POWERFUL if nothing deals more damage,
 *    or MOVE_NOT_MOST_POWERFUL if a better option exists
 *
 * GAME LOGIC:
 * This lets AI scripts make decisions like: "if this move is the strongest
 * option, boost its score" or "if there's a stronger move, reduce this
 * one's score." The damage calculation uses AI_CalcDmg (which accounts for
 * Attack/Defense stats and base power) plus TypeCalc (type effectiveness),
 * multiplied by the simulated random damage roll (84-100%).
 */
static void Cmd_get_how_powerful_move_is(void)
{
    s32 i, checkedMove;
    s32 moveDmgs[MAX_MON_MOVES];

    /* Check if the current move has a discouraged effect. */
    for (i = 0; sDiscouragedPowerfulMoveEffects[i] != 0xFFFF; i++)
    {
        if (gBattleMoves[AI_THINKING_STRUCT->moveConsidered].effect == sDiscouragedPowerfulMoveEffects[i])
            break;
    }

    /* Only evaluate if the move has power > 1 and isn't discouraged.
     * Power of 0 = status move, power of 1 = variable power moves. */
    if (gBattleMoves[AI_THINKING_STRUCT->moveConsidered].power > 1
        && sDiscouragedPowerfulMoveEffects[i] == 0xFFFF)
    {
        /* Reset damage calculation globals to neutral state. */
        gDynamicBasePower = 0;
        gBattleStruct->dynamicMoveType = 0;
        gBattleScripting.dmgMultiplier = 1;
        gMoveResultFlags = 0;
        gCritMultiplier = 1;

        /* Calculate estimated damage for each move in the moveset. */
        for (checkedMove = 0; checkedMove < MAX_MON_MOVES; checkedMove++)
        {
            /* Skip discouraged moves in the comparison. */
            for (i = 0; sDiscouragedPowerfulMoveEffects[i] != 0xFFFF; i++)
            {
                if (gBattleMoves[gBattleMons[gBattlerAttacker].moves[checkedMove]].effect == sDiscouragedPowerfulMoveEffects[i])
                    break;
            }

            if (gBattleMons[gBattlerAttacker].moves[checkedMove] != MOVE_NONE
                && sDiscouragedPowerfulMoveEffects[i] == 0xFFFF
                && gBattleMoves[gBattleMons[gBattlerAttacker].moves[checkedMove]].power > 1)
            {
                /* Calculate damage: base damage * type effectiveness * RNG roll.
                 * AI_CalcDmg considers stats, abilities, and base power.
                 * TypeCalc applies STAB and type effectiveness multipliers. */
                gCurrentMove = gBattleMons[gBattlerAttacker].moves[checkedMove];
                AI_CalcDmg(gBattlerAttacker, gBattlerTarget);
                TypeCalc(gCurrentMove, gBattlerAttacker, gBattlerTarget);
                moveDmgs[checkedMove] = gBattleMoveDamage * AI_THINKING_STRUCT->simulatedRNG[checkedMove] / 100;
                if (moveDmgs[checkedMove] == 0)
                    moveDmgs[checkedMove] = 1;  /* Minimum 1 damage */
            }
            else
            {
                moveDmgs[checkedMove] = 0;  /* Non-attacking or discouraged */
            }
        }

        /* Check if any other move deals more damage than the current one. */
        for (checkedMove = 0; checkedMove < MAX_MON_MOVES; checkedMove++)
        {
            if (moveDmgs[checkedMove] > moveDmgs[AI_THINKING_STRUCT->movesetIndex])
                break;
        }

        if (checkedMove == MAX_MON_MOVES)
            AI_THINKING_STRUCT->funcResult = MOVE_MOST_POWERFUL;      /* No move beats it */
        else
            AI_THINKING_STRUCT->funcResult = MOVE_NOT_MOST_POWERFUL;  /* Something is stronger */
    }
    else
    {
        AI_THINKING_STRUCT->funcResult = MOVE_POWER_DISCOURAGED;  /* Has bad side effects */
    }

    sAIScriptPtr++;
}

static void Cmd_get_last_used_battler_move(void)
{
    if (sAIScriptPtr[1] == AI_USER)
        AI_THINKING_STRUCT->funcResult = gLastMoves[gBattlerAttacker];
    else
        AI_THINKING_STRUCT->funcResult = gLastMoves[gBattlerTarget];

    sAIScriptPtr += 2;
}

static void Cmd_if_equal_(void) // Same as if_equal.
{
    if (sAIScriptPtr[1] == AI_THINKING_STRUCT->funcResult)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_not_equal_(void) // Same as if_not_equal.
{
    if (sAIScriptPtr[1] != AI_THINKING_STRUCT->funcResult)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_would_go_first(void)
{
    if (GetWhoStrikesFirst(gBattlerAttacker, gBattlerTarget, TRUE) == sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_would_not_go_first(void)
{
    if (GetWhoStrikesFirst(gBattlerAttacker, gBattlerTarget, TRUE) != sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_nullsub_2A(void)
{
}

static void Cmd_nullsub_2B(void)
{
}

static void Cmd_count_alive_pokemon(void)
{
    u8 battlerId;
    u8 battlerOnField1, battlerOnField2;
    struct Pokemon *party;
    s32 i;

    AI_THINKING_STRUCT->funcResult = 0;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (GetBattlerSide(battlerId) == B_SIDE_PLAYER)
        party = gPlayerParty;
    else
        party = gEnemyParty;

    if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
    {
        u32 position;
        battlerOnField1 = gBattlerPartyIndexes[battlerId];
        position = GetBattlerPosition(battlerId) ^ BIT_FLANK;
        battlerOnField2 = gBattlerPartyIndexes[GetBattlerAtPosition(position)];
    }
    else // In singles there's only one battlerId by side.
    {
        battlerOnField1 = gBattlerPartyIndexes[battlerId];
        battlerOnField2 = gBattlerPartyIndexes[battlerId];
    }

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (i != battlerOnField1 && i != battlerOnField2
         && GetMonData(&party[i], MON_DATA_HP) != 0
         && GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE
         && GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_EGG)
        {
            AI_THINKING_STRUCT->funcResult++;
        }
    }

    sAIScriptPtr += 2;
}

static void Cmd_get_considered_move(void)
{
    AI_THINKING_STRUCT->funcResult = AI_THINKING_STRUCT->moveConsidered;
    sAIScriptPtr += 1;
}

static void Cmd_get_considered_move_effect(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleMoves[AI_THINKING_STRUCT->moveConsidered].effect;
    sAIScriptPtr += 1;
}

/**
 * FUNCTION: Cmd_get_ability
 *
 * PURPOSE: Gets the ability of the specified battler, with special handling
 * for how much the AI "knows" about the opponent's ability.
 *
 * HOW IT WORKS:
 * For the AI's own Pokemon: simply reads the actual ability (it knows itself).
 * For the opponent (player's Pokemon), the AI uses a knowledge hierarchy:
 *   1. If the ability was already observed and recorded → use that
 *   2. If it's a trapping ability (Shadow Tag, etc.) → the AI notices it
 *   3. Otherwise, look up what abilities the species CAN have:
 *      - If only one possible ability → use that
 *      - If two possible abilities → randomly guess (50/50)
 *
 * GAME LOGIC:
 * This is one of the most interesting AI design decisions. The AI doesn't
 * cheat by reading the opponent's data directly — it uses observation and
 * species knowledge to make educated guesses. This makes the AI feel more
 * "fair" to the player, since a human opponent would also have to guess.
 */
static void Cmd_get_ability(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (GetBattlerSide(battlerId) == AI_TARGET)
    {
        u16 side = GET_BATTLER_SIDE(battlerId);

        /* Priority 1: Use previously observed ability (from battle history). */
        if (BATTLE_HISTORY->abilities[side] != 0)
        {
            AI_THINKING_STRUCT->funcResult = BATTLE_HISTORY->abilities[side];
            sAIScriptPtr += 2;
            return;
        }

        /* Priority 2: Trapping abilities are always "visible" since
         * the player can't switch out — the AI notices this. */
        if (gBattleMons[battlerId].ability == ABILITY_SHADOW_TAG
        || gBattleMons[battlerId].ability == ABILITY_MAGNET_PULL
        || gBattleMons[battlerId].ability == ABILITY_ARENA_TRAP)
        {
            AI_THINKING_STRUCT->funcResult = gBattleMons[battlerId].ability;
            sAIScriptPtr += 2;
            return;
        }

        /* Priority 3: Guess based on species data. Each Pokemon species
         * can have up to 2 possible abilities. */
        if (gSpeciesInfo[gBattleMons[battlerId].species].abilities[0] != ABILITY_NONE)
        {
            if (gSpeciesInfo[gBattleMons[battlerId].species].abilities[1] != ABILITY_NONE)
            {
                /* Two possible abilities — randomly guess which one.
                 * This means the AI might make wrong assumptions! */
                if (Random() % 2)
                    AI_THINKING_STRUCT->funcResult = gSpeciesInfo[gBattleMons[battlerId].species].abilities[0];
                else
                    AI_THINKING_STRUCT->funcResult = gSpeciesInfo[gBattleMons[battlerId].species].abilities[1];
            }
            else
            {
                /* Only one possible ability — use it with certainty. */
                AI_THINKING_STRUCT->funcResult = gSpeciesInfo[gBattleMons[battlerId].species].abilities[0];
            }
        }
        else
        {
            /* Dead code — no Pokemon has ability slot 2 without ability slot 1. */
            AI_THINKING_STRUCT->funcResult = gSpeciesInfo[gBattleMons[battlerId].species].abilities[1];
        }
    }
    else
    {
        /* The AI knows its own ability directly — no guessing needed. */
        AI_THINKING_STRUCT->funcResult = gBattleMons[battlerId].ability;
    }

    sAIScriptPtr += 2;
}

static void Cmd_get_highest_type_effectiveness(void)
{
    s32 i;
    u8 *dynamicMoveType;

    gDynamicBasePower = 0;
    dynamicMoveType = &gBattleStruct->dynamicMoveType;
    *dynamicMoveType = 0;
    gBattleScripting.dmgMultiplier = 1;
    gMoveResultFlags = 0;
    gCritMultiplier = 1;
    AI_THINKING_STRUCT->funcResult = 0;

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        gBattleMoveDamage = 40;
        gCurrentMove = gBattleMons[gBattlerAttacker].moves[i];

        if (gCurrentMove != MOVE_NONE)
        {
            TypeCalc(gCurrentMove, gBattlerAttacker, gBattlerTarget);

            if (gBattleMoveDamage == 120) // Super effective STAB.
                gBattleMoveDamage = AI_EFFECTIVENESS_x2;
            if (gBattleMoveDamage == 240)
                gBattleMoveDamage = AI_EFFECTIVENESS_x4;
            if (gBattleMoveDamage == 30) // Not very effective STAB.
                gBattleMoveDamage = AI_EFFECTIVENESS_x0_5;
            if (gBattleMoveDamage == 15)
                gBattleMoveDamage = AI_EFFECTIVENESS_x0_25;

            if (gMoveResultFlags & MOVE_RESULT_DOESNT_AFFECT_FOE)
                gBattleMoveDamage = AI_EFFECTIVENESS_x0;

            if (AI_THINKING_STRUCT->funcResult < gBattleMoveDamage)
                AI_THINKING_STRUCT->funcResult = gBattleMoveDamage;
        }
    }

    sAIScriptPtr += 1;
}

static void Cmd_if_type_effectiveness(void)
{
    u8 damageVar;

    gDynamicBasePower = 0;
    gBattleStruct->dynamicMoveType = 0;
    gBattleScripting.dmgMultiplier = 1;
    gMoveResultFlags = 0;
    gCritMultiplier = 1;

    gBattleMoveDamage = AI_EFFECTIVENESS_x1;
    gCurrentMove = AI_THINKING_STRUCT->moveConsidered;

    TypeCalc(gCurrentMove, gBattlerAttacker, gBattlerTarget);

    if (gBattleMoveDamage == 120) // Super effective STAB.
        gBattleMoveDamage = AI_EFFECTIVENESS_x2;
    if (gBattleMoveDamage == 240)
        gBattleMoveDamage = AI_EFFECTIVENESS_x4;
    if (gBattleMoveDamage == 30) // Not very effective STAB.
        gBattleMoveDamage = AI_EFFECTIVENESS_x0_5;
    if (gBattleMoveDamage == 15)
        gBattleMoveDamage = AI_EFFECTIVENESS_x0_25;

    if (gMoveResultFlags & MOVE_RESULT_DOESNT_AFFECT_FOE)
        gBattleMoveDamage = AI_EFFECTIVENESS_x0;

    // Store gBattleMoveDamage in a u8 variable because sAIScriptPtr[1] is a u8.
    damageVar = gBattleMoveDamage;

    if (damageVar == sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_nullsub_32(void)
{
}

static void Cmd_nullsub_33(void)
{
}

static void Cmd_if_status_in_party(void)
{
    struct Pokemon *party;
    struct Pokemon *partyPtr;
    int i;
    u32 statusToCompareTo;
    // u8 battlerId

    // for whatever reason, game freak put the party pointer into 2 variables instead of 1
    // it's possible at some point the switch encompassed the whole function and used each respective variable creating largely duplicate code.
    switch (sAIScriptPtr[1])
    {
    case 1:
        party = partyPtr = gEnemyParty;
        break;
    default:
        party = partyPtr = gPlayerParty;
        break;
    }

    /* Emerald's fixed version below
    switch (sAIScriptPtr[1])
    {
    case AI_USER:
        battlerId = gBattlerAttacker;
        break;
    default:
        battlerId = gBattlerTarget;
        break;
    }

    party = (GetBattlerSide(battlerId) == B_SIDE_PLAYER) ? gPlayerParty : gEnemyParty;
    */

    statusToCompareTo = T1_READ_32(sAIScriptPtr + 2);

    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 species = GetMonData(&party[i], MON_DATA_SPECIES);
        u16 hp = GetMonData(&party[i], MON_DATA_HP);
        u32 status = GetMonData(&party[i], MON_DATA_STATUS);

        if (species != SPECIES_NONE && species != SPECIES_EGG && hp != 0 && status == statusToCompareTo)
        {
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
            return;
        }
    }

    sAIScriptPtr += 10;
}

// bugged, doesnt return properly. also unused
static void Cmd_if_status_not_in_party(void)
{
    struct Pokemon *party;
    struct Pokemon *partyPtr;
    int i;
    u32 statusToCompareTo;
    //u8 battlerId

    switch (sAIScriptPtr[1])
    {
    case 1:
        party = partyPtr = gEnemyParty;
        break;
    default:
        party = partyPtr = gPlayerParty;
        break;
    }

    statusToCompareTo = T1_READ_32(sAIScriptPtr + 2);

    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 species = GetMonData(&party[i], MON_DATA_SPECIES);
        u16 hp = GetMonData(&party[i], MON_DATA_HP);
        u32 status = GetMonData(&party[i], MON_DATA_STATUS);

        // everytime the status is found, the AI's logic jumps further and further past its intended destination. this results in a broken AI macro and is probably why it is unused.
        if (species != SPECIES_NONE && species != SPECIES_EGG && hp != 0 && status == statusToCompareTo)
        {
            sAIScriptPtr += 10; // doesnt return?
            #ifdef UBFIX
            return;
            #endif
        }
    }
    sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 6);
}

enum
{
    WEATHER_TYPE_SUNNY,
    WEATHER_TYPE_RAIN,
    WEATHER_TYPE_SANDSTORM,
    WEATHER_TYPE_HAIL,
};

extern u16 gBattleWeather;

static void Cmd_get_weather(void)
{
    if (gBattleWeather & B_WEATHER_RAIN)
        AI_THINKING_STRUCT->funcResult = WEATHER_TYPE_RAIN;
    if (gBattleWeather & B_WEATHER_SANDSTORM)
        AI_THINKING_STRUCT->funcResult = WEATHER_TYPE_SANDSTORM;
    if (gBattleWeather & B_WEATHER_SUN)
        AI_THINKING_STRUCT->funcResult = WEATHER_TYPE_SUNNY;
    if (gBattleWeather & B_WEATHER_HAIL_TEMPORARY)
        AI_THINKING_STRUCT->funcResult = WEATHER_TYPE_HAIL;

    sAIScriptPtr += 1;
}

static void Cmd_if_effect(void)
{
    if (gBattleMoves[AI_THINKING_STRUCT->moveConsidered].effect == sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_not_effect(void)
{
    if (gBattleMoves[AI_THINKING_STRUCT->moveConsidered].effect != sAIScriptPtr[1])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
    else
        sAIScriptPtr += 6;
}

static void Cmd_if_stat_level_less_than(void)
{
    u32 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (gBattleMons[battlerId].statStages[sAIScriptPtr[2]] < sAIScriptPtr[3])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
    else
        sAIScriptPtr += 8;
}

static void Cmd_if_stat_level_more_than(void)
{
    u32 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (gBattleMons[battlerId].statStages[sAIScriptPtr[2]] > sAIScriptPtr[3])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
    else
        sAIScriptPtr += 8;
}

static void Cmd_if_stat_level_equal(void)
{
    u32 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (gBattleMons[battlerId].statStages[sAIScriptPtr[2]] == sAIScriptPtr[3])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
    else
        sAIScriptPtr += 8;
}

static void Cmd_if_stat_level_not_equal(void)
{
    u32 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (gBattleMons[battlerId].statStages[sAIScriptPtr[2]] != sAIScriptPtr[3])
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
    else
        sAIScriptPtr += 8;
}

/**
 * FUNCTION: Cmd_if_can_faint
 *
 * PURPOSE: Jumps if the currently considered move can KO (knock out / faint)
 * the target in one hit. This is crucial for the AI_SCRIPT_TRY_TO_FAINT
 * behavior — if the AI can finish off the opponent, it should prioritize that.
 *
 * HOW IT WORKS:
 * 1. Status moves (power < 2) can't KO, so skip immediately
 * 2. Calculate the estimated damage including type effectiveness and RNG
 * 3. If estimated damage >= target's remaining HP, the move can KO
 *
 * GAME LOGIC:
 * The damage calculation resets all globals to neutral first (no dynamic
 * base power, no critical hits, 1x multiplier). This gives the AI a
 * conservative estimate — if it thinks it can KO without crits, it probably can.
 */
static void Cmd_if_can_faint(void)
{
    /* Status moves can't KO — skip immediately. */
    if (gBattleMoves[AI_THINKING_STRUCT->moveConsidered].power < 2)
    {
        sAIScriptPtr += 5;
        return;
    }

    /* Reset damage calculation state to neutral/conservative values. */
    gDynamicBasePower = 0;
    gBattleStruct->dynamicMoveType = 0;
    gBattleScripting.dmgMultiplier = 1;
    gMoveResultFlags = 0;
    gCritMultiplier = 1;
    gCurrentMove = AI_THINKING_STRUCT->moveConsidered;

    /* Calculate base damage, then apply type effectiveness. */
    AI_CalcDmg(gBattlerAttacker, gBattlerTarget);
    TypeCalc(gCurrentMove, gBattlerAttacker, gBattlerTarget);

    /* Apply the simulated random damage roll (84-100%). */
    gBattleMoveDamage = gBattleMoveDamage * AI_THINKING_STRUCT->simulatedRNG[AI_THINKING_STRUCT->movesetIndex] / 100;

    /* Moves always deal at least 1 damage. */
    if (gBattleMoveDamage == 0)
        gBattleMoveDamage = 1;

    /* Can this move KO the target? */
    if (gBattleMons[gBattlerTarget].hp <= gBattleMoveDamage)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);  /* Yes — jump to KO strategy */
    else
        sAIScriptPtr += 5;  /* No — continue script */
}

static void Cmd_if_cant_faint(void)
{
    if (gBattleMoves[AI_THINKING_STRUCT->moveConsidered].power < 2)
    {
        sAIScriptPtr += 5;
        return;
    }

    gDynamicBasePower = 0;
    gBattleStruct->dynamicMoveType = 0;
    gBattleScripting.dmgMultiplier = 1;
    gMoveResultFlags = 0;
    gCritMultiplier = 1;
    gCurrentMove = AI_THINKING_STRUCT->moveConsidered;
    AI_CalcDmg(gBattlerAttacker, gBattlerTarget);
    TypeCalc(gCurrentMove, gBattlerAttacker, gBattlerTarget);

    gBattleMoveDamage = gBattleMoveDamage * AI_THINKING_STRUCT->simulatedRNG[AI_THINKING_STRUCT->movesetIndex] / 100;

    // This macro is missing the damage 0 = 1 assumption.

    if (gBattleMons[gBattlerTarget].hp > gBattleMoveDamage)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);
    else
        sAIScriptPtr += 5;
}

static void Cmd_if_has_move(void)
{
    int i;
    const u16 *movePtr = (u16 *)(sAIScriptPtr + 2);

    switch (sAIScriptPtr[1])
    {
    case AI_USER:
    case AI_USER_PARTNER:
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            if (gBattleMons[gBattlerAttacker].moves[i] == *movePtr)
                break;
        }
        if (i == MAX_MON_MOVES)
            sAIScriptPtr += 8;
        else
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
        break;
    case AI_TARGET:
    case AI_TARGET_PARTNER:
        for (i = 0; i < 8; i++)
        {
            if (BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i] == *movePtr)
                break;
        }
        if (i == 8)
            sAIScriptPtr += 8;
        else
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
        break;
    }
}

static void Cmd_if_doesnt_have_move(void)
{
    int i;
    const u16 *movePtr = (u16 *)(sAIScriptPtr + 2);

    switch (sAIScriptPtr[1])
    {
    case AI_USER:
    case AI_USER_PARTNER:
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            if (gBattleMons[gBattlerAttacker].moves[i] == *movePtr)
                break;
        }
        if (i != MAX_MON_MOVES)
            sAIScriptPtr += 8;
        else
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
        break;
    case AI_TARGET:
    case AI_TARGET_PARTNER:
        for (i = 0; i < 8; i++)
        {
            if (BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i] == *movePtr)
                break;
        }
        if (i != 8)
            sAIScriptPtr += 8;
        else
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 4);
        break;
    }
}

static void Cmd_if_has_move_with_effect(void)
{
    int i;

    switch (sAIScriptPtr[1])
    {
    case AI_USER:
    case AI_USER_PARTNER:
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            if (gBattleMons[gBattlerAttacker].moves[i] != 0 && gBattleMoves[gBattleMons[gBattlerAttacker].moves[i]].effect == sAIScriptPtr[2])
                break;
        }
        if (i != MAX_MON_MOVES)
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
        else
            sAIScriptPtr += 7;
        break;
    case AI_TARGET:
    case AI_TARGET_PARTNER:
        for (i = 0; i < 8; i++)
        {
            if (gBattleMons[gBattlerAttacker].moves[i] != 0 && gBattleMoves[BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i]].effect == sAIScriptPtr[2])
                break;
        }
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    }
}

static void Cmd_if_doesnt_have_move_with_effect(void)
{
    int i;

    switch (sAIScriptPtr[1])
    {
    case AI_USER:
    case AI_USER_PARTNER:
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            if (gBattleMons[gBattlerAttacker].moves[i] != 0 && gBattleMoves[gBattleMons[gBattlerAttacker].moves[i]].effect == sAIScriptPtr[2])
                break;
        }
        if (i != MAX_MON_MOVES)
            sAIScriptPtr += 7;
        else
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
        break;
    case AI_TARGET:
    case AI_TARGET_PARTNER:
        for (i = 0; i < 8; i++)
        {
            if (BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i] != 0 && gBattleMoves[BATTLE_HISTORY->usedMoves[gBattlerTarget >> 1][i]].effect == sAIScriptPtr[2])
                break;
        }
        sAIScriptPtr += 7;
    }
}

static void Cmd_if_any_move_disabled_or_encored(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (sAIScriptPtr[2] == 0)
    {
        if (gDisableStructs[battlerId].disabledMove == MOVE_NONE)
            sAIScriptPtr += 7;
        else
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
    }
    else if (sAIScriptPtr[2] != 1)
    {
        sAIScriptPtr += 7;
    }
    else
    {
        if (gDisableStructs[battlerId].encoredMove != MOVE_NONE)
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 3);
        else
            sAIScriptPtr += 7;
    }
}

static void Cmd_if_curr_move_disabled_or_encored(void)
{
    switch (sAIScriptPtr[1])
    {
    case 0:
        if (gDisableStructs[gActiveBattler].disabledMove == AI_THINKING_STRUCT->moveConsidered)
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
        else
            sAIScriptPtr += 6;
        break;
    case 1:
        if (gDisableStructs[gActiveBattler].encoredMove == AI_THINKING_STRUCT->moveConsidered)
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
        else
            sAIScriptPtr += 6;
        break;
    default:
        sAIScriptPtr += 6;
        break;
    }
}

static void Cmd_flee(void)
{
    AI_THINKING_STRUCT->aiAction |= (AI_ACTION_DONE | AI_ACTION_FLEE | AI_ACTION_DO_NOT_ATTACK); // what matters is AI_ACTION_FLEE being enabled.
}

/**
 * FUNCTION: Cmd_if_random_safari_flee
 *
 * PURPOSE: Determines if a wild Pokemon in the Safari Zone will flee this turn.
 *
 * GAME LOGIC:
 * Safari Zone has unique mechanics — the player throws bait or rocks at wild
 * Pokemon. Rocks make Pokemon angrier (more likely to flee but easier to catch).
 * Bait makes them calmer (less likely to flee but harder to catch).
 *   - After throwing rocks: flee rate doubles (max 20)
 *   - After throwing bait: flee rate quarters (min 1)
 *   - Otherwise: uses base escape factor
 * The final rate is multiplied by 5, then checked against a random 0-99.
 * So a base factor of 5 → 25% flee chance; factor of 10 → 50% flee chance.
 */
static void Cmd_if_random_safari_flee(void)
{
    u8 safariFleeRate;

    if (gBattleStruct->safariRockThrowCounter)
    {
        safariFleeRate = gBattleStruct->safariEscapeFactor * 2;
        if (safariFleeRate > 20)
            safariFleeRate = 20;
    }
    else if (gBattleStruct->safariBaitThrowCounter != 0)
    {
        safariFleeRate = gBattleStruct->safariEscapeFactor / 4;
        if (safariFleeRate == 0)
            safariFleeRate = 1;
    }
    else
        safariFleeRate = gBattleStruct->safariEscapeFactor;
    safariFleeRate *= 5;
    if ((u8)(Random() % 100) < safariFleeRate)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);  /* Pokemon flees */
    else
        sAIScriptPtr += 5;  /* Pokemon stays */
}

static void Cmd_watch(void)
{
    AI_THINKING_STRUCT->aiAction |= (AI_ACTION_DONE | AI_ACTION_WATCH | AI_ACTION_DO_NOT_ATTACK); // what matters is AI_ACTION_WATCH being enabled.
}

static void Cmd_get_hold_effect(void)
{
    u8 battlerId;
    u16 side;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    if (GetBattlerSide(battlerId) == B_SIDE_PLAYER)
    {
        side = GET_BATTLER_SIDE(battlerId);
        AI_THINKING_STRUCT->funcResult = BATTLE_HISTORY->itemEffects[side];
    }
    else
        AI_THINKING_STRUCT->funcResult = ItemId_GetHoldEffect(gBattleMons[battlerId].item);

    sAIScriptPtr += 2;
}

static void Cmd_get_gender(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    AI_THINKING_STRUCT->funcResult = GetGenderFromSpeciesAndPersonality(gBattleMons[battlerId].species, gBattleMons[battlerId].personality);

    sAIScriptPtr += 2;
}

static void Cmd_is_first_turn_for(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    AI_THINKING_STRUCT->funcResult = gDisableStructs[battlerId].isFirstTurn;

    sAIScriptPtr += 2;
}

static void Cmd_get_stockpile_count(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    AI_THINKING_STRUCT->funcResult = gDisableStructs[battlerId].stockpileCounter;

    sAIScriptPtr += 2;
}

static void Cmd_is_double_battle(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleTypeFlags & BATTLE_TYPE_DOUBLE;

    sAIScriptPtr += 1;
}

static void Cmd_get_used_held_item(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    AI_THINKING_STRUCT->funcResult = ((u8 *)gBattleStruct->usedHeldItems)[battlerId * 2];
    sAIScriptPtr += 2;
}

static void Cmd_get_move_type_from_result(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleMoves[AI_THINKING_STRUCT->funcResult].type;

    sAIScriptPtr += 1;
}

static void Cmd_get_move_power_from_result(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleMoves[AI_THINKING_STRUCT->funcResult].power;

    sAIScriptPtr += 1;
}

static void Cmd_get_move_effect_from_result(void)
{
    AI_THINKING_STRUCT->funcResult = gBattleMoves[AI_THINKING_STRUCT->funcResult].effect;

    sAIScriptPtr += 1;
}

static void Cmd_get_protect_count(void)
{
    u8 battlerId;

    if (sAIScriptPtr[1] == AI_USER)
        battlerId = gBattlerAttacker;
    else
        battlerId = gBattlerTarget;

    AI_THINKING_STRUCT->funcResult = gDisableStructs[battlerId].protectUses;

    sAIScriptPtr += 2;
}

static void Cmd_nullsub_52(void)
{
}

static void Cmd_nullsub_53(void)
{
}

static void Cmd_nullsub_54(void)
{
}

static void Cmd_nullsub_55(void)
{
}

static void Cmd_nullsub_56(void)
{
}

static void Cmd_nullsub_57(void)
{
}

/**
 * FUNCTION: Cmd_call
 *
 * PURPOSE: Calls a sub-script (like a function call in a programming language).
 * Pushes the return address onto the AI stack and jumps to the target script.
 *
 * GAME LOGIC:
 * This allows AI scripts to share common logic as "subroutines." The return
 * address (current position + 5, past the call instruction) is saved so
 * Cmd_end can return to it. This is exactly how function calls work in
 * assembly language — push return address, then jump.
 */
static void Cmd_call(void)
{
    AIStackPushVar(sAIScriptPtr + 5);  /* Save return address */
    sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);  /* Jump to subroutine */
}

/**
 * FUNCTION: Cmd_goto
 *
 * PURPOSE: Unconditional jump — sets the script pointer to a new address.
 * Unlike Cmd_call, this doesn't save a return address (it's a one-way jump).
 */
static void Cmd_goto(void)
{
    sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);
}

/**
 * FUNCTION: Cmd_end
 *
 * PURPOSE: Ends the current script or returns from a subroutine call.
 * If the AI stack has a saved return address (from Cmd_call), pops it
 * and continues executing from there. If the stack is empty, this script
 * is truly done — sets AI_ACTION_DONE so the VM moves to the next move.
 */
static void Cmd_end(void)
{
    if (AIStackPop() == FALSE)
        AI_THINKING_STRUCT->aiAction |= AI_ACTION_DONE;
}

static void Cmd_if_level_compare(void)
{
    switch (sAIScriptPtr[1])
    {
    case 0: // greater than
        if (gBattleMons[gBattlerAttacker].level > gBattleMons[gBattlerTarget].level)
        {
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
            return;
        }
        sAIScriptPtr += 6;
        return;
    case 1: // less than
        if (gBattleMons[gBattlerAttacker].level < gBattleMons[gBattlerTarget].level)
        {
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
            return;
        }
        sAIScriptPtr += 6;
        return;
    case 2: // equal
        if (gBattleMons[gBattlerAttacker].level == gBattleMons[gBattlerTarget].level)
        {
            sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 2);
            return;
        }
        sAIScriptPtr += 6;
        return;
    }
}

static void Cmd_if_target_taunted(void)
{
    if (gDisableStructs[gBattlerTarget].tauntTimer != 0)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);
    else
        sAIScriptPtr += 5;
}

static void Cmd_if_target_not_taunted(void)
{
    if (gDisableStructs[gBattlerTarget].tauntTimer == 0)
        sAIScriptPtr = T1_READ_PTR(sAIScriptPtr + 1);
    else
        sAIScriptPtr += 5;
}

/* ========================================================================
 * AI SCRIPT CALL STACK
 * ========================================================================
 * These functions implement a simple call stack for AI script subroutines.
 * This works exactly like a CPU's call stack in miniature:
 *   - Push: save a return address before jumping to a subroutine
 *   - Pop:  restore the return address when the subroutine ends
 * The stack size limits how deeply subroutines can nest.
 * ======================================================================== */

/**
 * FUNCTION: AIStackPushVar
 *
 * PURPOSE: Pushes a script pointer onto the AI call stack (saves a return address).
 */
static void AIStackPushVar(const u8 *var)
{
    gBattleResources->AI_ScriptsStack->ptr[gBattleResources->AI_ScriptsStack->size++] = var;
}

/**
 * FUNCTION: AIStackPushVar_cursor (unused)
 *
 * PURPOSE: Pushes the CURRENT script position onto the stack. Not used in
 * the final game — possibly was used during development for different
 * control flow patterns.
 */
static void AIStackPushVar_cursor(void)
{
    gBattleResources->AI_ScriptsStack->ptr[gBattleResources->AI_ScriptsStack->size++] = sAIScriptPtr;
}

/**
 * FUNCTION: AIStackPop
 *
 * PURPOSE: Pops a return address from the AI call stack and restores it
 * as the current script pointer. Returns TRUE if there was an address
 * to pop, FALSE if the stack was empty (meaning the top-level script ended).
 */
static bool8 AIStackPop(void)
{
    if (gBattleResources->AI_ScriptsStack->size != 0)
    {
        --gBattleResources->AI_ScriptsStack->size;
        sAIScriptPtr = gBattleResources->AI_ScriptsStack->ptr[gBattleResources->AI_ScriptsStack->size];
        return TRUE;
    }
    else
        return FALSE;
}
