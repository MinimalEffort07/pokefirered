/**
 * @file quest_log_battle.c
 * @brief Quest Log Battle Event Recording — Logging Wins for the Adventure Log
 *
 * FILE OVERVIEW:
 * This file records battle outcomes into the Quest Log (Adventure Log) system.
 * When the player wins or catches a Pokemon in battle, the game logs information
 * about the battle so it can be displayed in the Quest Log and, in some cases,
 * replayed as an event scene.
 *
 * Different event types are logged depending on the opponent:
 *   - Wild Pokemon: records the species defeated or caught
 *   - Regular Trainers: records the trainer ID and ending HP fraction
 *   - Gym Leaders: logged as a special "defeated gym leader" event
 *   - Elite Four / Champion: logged with their own event types
 *   - Link Battles: records opponent names and battle outcome
 *
 * GAME LOGIC:
 * The HP fraction at the end of a trainer battle determines the flavor text
 * shown in the Quest Log (e.g., "handily defeated" vs "barely managed to win").
 * Three tiers: full/high HP, moderate HP (< 2/3), and low HP (< 1/3).
 */
#include "global.h"
#include "gflib.h"
#include "battle.h"
#include "battle_anim.h"
#include "link.h"
#include "overworld.h"
#include "quest_log.h"
#include "constants/trainers.h"

static void GetLinkMultiBattlePlayerIndexes(s32 *, s32 *);

/**
 * FUNCTION: TrySetQuestLogBattleEvent
 *
 * PURPOSE: After a battle ends, records the outcome in the Quest Log if applicable.
 *
 * HOW IT WORKS:
 * Only records for non-link, non-tutorial battles that the player won or caught.
 * For trainer battles, determines the event type by trainer class (gym leader,
 * champion, E4, or regular trainer), then logs the trainer ID, species involved,
 * map section, and an HP fraction tier. For wild battles, records whether the
 * wild Pokemon was defeated or caught.
 */
void TrySetQuestLogBattleEvent(void)
{
    if (!(gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_OLD_MAN_TUTORIAL | BATTLE_TYPE_POKEDUDE)) && (gBattleOutcome == B_OUTCOME_WON || gBattleOutcome == B_OUTCOME_CAUGHT))
    {
        // Why allocate both of these? Only one will ever be used at a time
        struct QuestLogEvent_TrainerBattle * trainerData = Alloc(sizeof(*trainerData));
        struct QuestLogEvent_WildBattle * wildData = Alloc(sizeof(*wildData));
        u16 eventId;
        u16 playerEndingHP;
        u16 playerMaxHP;

        if (gBattleTypeFlags & BATTLE_TYPE_TRAINER)
        {
            switch (gTrainers[gTrainerBattleOpponent_A].trainerClass)
            {
            case TRAINER_CLASS_LEADER:
                eventId = QL_EVENT_DEFEATED_GYM_LEADER;
                break;
            case TRAINER_CLASS_CHAMPION:
                eventId = QL_EVENT_DEFEATED_CHAMPION;
                break;
            case TRAINER_CLASS_ELITE_FOUR:
                eventId = QL_EVENT_DEFEATED_E4_MEMBER;
                break;
            default:
                eventId = QL_EVENT_DEFEATED_TRAINER;
                break;
            }
            trainerData->trainerId = gTrainerBattleOpponent_A;
            if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
            {
                trainerData->speciesOpponent = gBattleResults.lastOpponentSpecies;
                
                // Decide which of the pokemon on the player's side to mention as the victor
                if (GetBattlerSide(gBattleStruct->lastAttackerToFaintOpponent) == B_SIDE_PLAYER)
                    trainerData->speciesPlayer = gBattleMons[gBattleStruct->lastAttackerToFaintOpponent].species;
                else if (gBattleMons[GetBattlerAtPosition(0)].hp != 0)
                    trainerData->speciesPlayer = gBattleMons[GetBattlerAtPosition(0)].species;
                else
                    trainerData->speciesPlayer = gBattleMons[GetBattlerAtPosition(2)].species;

                playerEndingHP = gBattleMons[GetBattlerAtPosition(0)].hp + gBattleMons[GetBattlerAtPosition(2)].hp;
                playerMaxHP = gBattleMons[GetBattlerAtPosition(0)].maxHP + gBattleMons[GetBattlerAtPosition(2)].maxHP;
            }
            else
            {
                trainerData->speciesOpponent = gBattleResults.lastOpponentSpecies;
                trainerData->speciesPlayer = gBattleMons[GetBattlerAtPosition(0)].species;
                playerEndingHP = gBattleMons[GetBattlerAtPosition(0)].hp;
                playerMaxHP = gBattleMons[GetBattlerAtPosition(0)].maxHP;
            }
            trainerData->mapSec = GetCurrentRegionMapSectionId();

            // Calculate fractional HP loss (determines flavor text, e.g. "handily" vs "somehow" defeated trainer)
            trainerData->hpFractionId = 0;
            if (playerEndingHP < playerMaxHP / 3 * 2)
                trainerData->hpFractionId++;
            if (playerEndingHP < playerMaxHP / 3)
                trainerData->hpFractionId++;

            SetQuestLogEvent(eventId, (const u16 *)trainerData);
        }
        else
        {
            if (gBattleOutcome == B_OUTCOME_WON)
            {
                wildData->defeatedSpecies = GetMonData(gEnemyParty, MON_DATA_SPECIES);
                wildData->caughtSpecies = SPECIES_NONE;
            }
            else // gBattleOutcome == B_OUTCOME_CAUGHT
            {
                wildData->defeatedSpecies = SPECIES_NONE;
                wildData->caughtSpecies = GetMonData(gEnemyParty, MON_DATA_SPECIES);
            }
            wildData->mapSec = GetCurrentRegionMapSectionId();
            SetQuestLogEvent(QL_EVENT_DEFEATED_WILD_MON, (const u16 *)wildData);
        }
        Free(trainerData);
        Free(wildData);
    }
}

/**
 * FUNCTION: TrySetQuestLogLinkBattleEvent
 *
 * PURPOSE: Records a link battle (multiplayer) outcome in the Quest Log.
 *
 * HOW IT WORKS:
 * Only runs for link battles. Records the outcome (won/lost/drew), opponent
 * names, and the battle type (single, double, multi, or Union Room). For multi
 * battles, records the partner's name and both opponents' names.
 */
void TrySetQuestLogLinkBattleEvent(void)
{
    s32 partnerIdx;
    s32 opponentIdxs[2];
    u16 eventId;
    s32 i;
    bool32 inUnionRoom;

    if (gBattleTypeFlags & BATTLE_TYPE_LINK)
    {
        struct QuestLogEvent_LinkBattle * data = Alloc(sizeof(*data));
        data->outcome = gBattleOutcome - 1; // 0 = won, 1 = lost, 2 = drew
        if (gBattleTypeFlags & BATTLE_TYPE_MULTI)
        {
            eventId = QL_EVENT_LINK_BATTLED_MULTI;
            GetLinkMultiBattlePlayerIndexes(&partnerIdx, opponentIdxs);
            for (i = 0; i < PLAYER_NAME_LENGTH; i++)
            {
                data->playerNames[0][i] = gLinkPlayers[partnerIdx].name[i];
                data->playerNames[1][i] = gLinkPlayers[opponentIdxs[0]].name[i];
                data->playerNames[2][i] = gLinkPlayers[opponentIdxs[1]].name[i];
            }
        }
        else
        {
            if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
                eventId = QL_EVENT_LINK_BATTLED_DOUBLE;
            else
            {
                inUnionRoom = InUnionRoom();
                eventId = QL_EVENT_LINK_BATTLED_SINGLE;
                
                if (inUnionRoom == TRUE)
                    eventId = QL_EVENT_LINK_BATTLED_UNION;
            }

            for (i = 0; i < PLAYER_NAME_LENGTH; i++)
                data->playerNames[0][i] = gLinkPlayers[gBattleStruct->multiplayerId ^ 1].name[i];
        }
        SetQuestLogEvent(eventId, (const u16 *)data);
        Free(data);
    }
}

/**
 * FUNCTION: GetLinkMultiBattlePlayerIndexes
 *
 * PURPOSE: Identifies which link players are the partner and which are opponents
 *          in a multi battle (2v2 with 4 human players).
 *
 * HOW IT WORKS:
 * In multi battles, each player has a 2-bit ID. The partner's ID is the player's
 * ID XORed with 2 (swapping the high bit). The function scans all 4 battler
 * slots to find which index matches the partner ID, and which are opponents.
 */
static void GetLinkMultiBattlePlayerIndexes(s32 * partnerIdx, s32 * opponentIdxs)
{
    s32 i;
    s32 numOpponentsFound = 0;
    u8 partnerId = gLinkPlayers[gBattleStruct->multiplayerId].id ^ 2;
    for (i = 0; i < MAX_BATTLERS_COUNT; i++)
    {
        if (partnerId == gLinkPlayers[i].id)
            *partnerIdx = i;
        else if (i != gBattleStruct->multiplayerId)
            opponentIdxs[numOpponentsFound++] = i;
    }
}
