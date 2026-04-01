/**
 * @file wonder_news.c
 * @brief Wonder News Reward System — Berry Rewards for Sharing Mystery Gift News
 *
 * FILE OVERVIEW:
 * Wonder News was a feature (primarily used in Japanese releases) where players
 * could receive and share news bulletins via the Mystery Gift system. As a reward
 * for receiving or forwarding Wonder News to other players, the game gives the
 * player random berries.
 *
 * The reward system has several anti-farming mechanisms:
 *   - Maximum of 5 rewards before a 500-step cooldown
 *   - Every 4th "sent" reward is a "big" reward (better berry range)
 *   - Separate tracking for received vs. sent rewards
 *
 * GAME LOGIC:
 * Berry rewards are randomly selected from different ranges depending on how
 * the news was obtained:
 *   - Received from a friend: random berry from Razz to Nomel range
 *   - Sent to another player: random berry from Cheri to Iapapa range
 * The system uses ITEM_TO_BERRY() to convert item IDs to berry IDs for storage.
 *
 * NOTE: The original Japanese filename is "menews_jisan.c" (visible in the
 * assert at the bottom), where "menews" = Mystery Event News, "jisan" = reward.
 */
#include "global.h"
#include "mystery_gift.h"
#include "random.h"
#include "event_data.h"
#include "wonder_news.h"
#include "constants/items.h"

/* Every 4th reward for sending Wonder News to a link partner is a "big" reward */
#define MAX_SENT_REWARD 4

/* Maximum rewards before the 500-step cooldown activates */
#define MAX_REWARD 5

static u32 GetRewardItem(struct WonderNewsMetadata *);
static u32 GetRewardType(struct WonderNewsMetadata *);
static void IncrementRewardCounter(struct WonderNewsMetadata *);
static void IncrementSentRewardCounter(struct WonderNewsMetadata *);
static void ResetSentRewardCounter(struct WonderNewsMetadata *);

/**
 * FUNCTION: WonderNews_SetReward
 *
 * PURPOSE: Sets up a pending reward based on how Wonder News was obtained.
 *          Randomly selects a berry from the appropriate range.
 *
 * PARAMETERS:
 * @param newsType — How the news was obtained (WONDER_NEWS_RECV_FRIEND,
 *                   WONDER_NEWS_RECV_WIRELESS, or WONDER_NEWS_SENT)
 */
void WonderNews_SetReward(u32 newsType)
{
    struct WonderNewsMetadata *data = GetSavedWonderNewsMetadata();

    data->newsType = newsType;
    switch (newsType)
    {
    case WONDER_NEWS_NONE:
        break;
    case WONDER_NEWS_RECV_FRIEND:
    case WONDER_NEWS_RECV_WIRELESS:
        // Random berry between ITEM_RAZZ_BERRY and ITEM_NOMEL_BERRY
        data->berry = (Random() % 15) + ITEM_TO_BERRY(ITEM_RAZZ_BERRY);
        break;
    case WONDER_NEWS_SENT:
        // Random berry between ITEM_CHERI_BERRY and ITEM_IAPAPA_BERRY
        data->berry = (Random() % 15) + ITEM_TO_BERRY(ITEM_CHERI_BERRY);
        break;
    }
}

/**
 * FUNCTION: WonderNews_Reset
 *
 * PURPOSE: Clears all Wonder News reward state — news type, counters, berry,
 *          and step counter. Called when Wonder News data is invalidated.
 */
void WonderNews_Reset(void)
{
    struct WonderNewsMetadata *data = GetSavedWonderNewsMetadata();

    data->newsType = WONDER_NEWS_NONE;
    data->sentRewardCounter = 0;
    data->rewardCounter = 0;
    data->berry = 0;
    VarSet(VAR_WONDER_NEWS_STEP_COUNTER, 0);
}

/**
 * FUNCTION: WonderNews_IncrementStepCounter
 *
 * PURPOSE: Called each step the player takes. If the reward limit has been reached,
 *          counts steps until 500 are taken, then resets the reward counter to
 *          allow the player to receive rewards again.
 */
void WonderNews_IncrementStepCounter(void)
{
    u16 *stepCounter = GetVarPointer(VAR_WONDER_NEWS_STEP_COUNTER);
    struct WonderNewsMetadata *data = GetSavedWonderNewsMetadata();

    // If the player has reached the reward limit, start counting steps.
    // When they reach 500 steps reset the reward counter to allow them to
    // receive rewards again.
    if (data->rewardCounter >= MAX_REWARD && ++(*stepCounter) >= 500)
    {
        data->rewardCounter = 0;
        *stepCounter = 0;
    }
}

/**
 * FUNCTION: WonderNews_GetRewardInfo
 *
 * PURPOSE: Determines what reward (if any) the player should receive and stores
 *          the reward item ID in gSpecialVar_Result.
 *
 * RETURNS: The reward type (NEWS_REWARD_NONE, _WAITING, _AT_MAX, _RECV_SMALL,
 *          _RECV_BIG, _SENT_SMALL, or _SENT_BIG). The calling code uses this
 *          to display the appropriate message.
 */
u16 WonderNews_GetRewardInfo(void)
{
    u16 *result = &gSpecialVar_Result;
    struct WonderNewsMetadata *data = GetSavedWonderNewsMetadata();
    u16 rewardType;

    if (!IsMysteryGiftEnabled() || !ValidateSavedWonderNews())
        return NEWS_REWARD_NONE;

    rewardType = GetRewardType(data);

    switch (rewardType)
    {
    case NEWS_REWARD_RECV_SMALL:
    case NEWS_REWARD_RECV_BIG:
        *result = GetRewardItem(data);
        break;
    case NEWS_REWARD_SENT_SMALL:
        *result = GetRewardItem(data);
        IncrementSentRewardCounter(data);
        break;
    case NEWS_REWARD_SENT_BIG:
        *result = GetRewardItem(data);
        ResetSentRewardCounter(data);
        break;
    case NEWS_REWARD_NONE:
    case NEWS_REWARD_WAITING:
    case NEWS_REWARD_AT_MAX:
        break;
    }

    return rewardType;
}

static u32 GetRewardItem(struct WonderNewsMetadata *data)
{
    u32 itemId;

    data->newsType = WONDER_NEWS_NONE;
    itemId = data->berry + FIRST_BERRY_INDEX - 1;
    data->berry = 0;
    IncrementRewardCounter(data);
    return itemId;
}

static void ResetSentRewardCounter(struct WonderNewsMetadata *data)
{
    data->sentRewardCounter = 0;
}

// Track number of times a reward was received (or attmepted to receive) for sending Wonder News to a link partner.
static void IncrementSentRewardCounter(struct WonderNewsMetadata *data)
{
    data->sentRewardCounter++;
    if (data->sentRewardCounter > MAX_SENT_REWARD)
        data->sentRewardCounter = MAX_SENT_REWARD;
}

static void IncrementRewardCounter(struct WonderNewsMetadata *data)
{
    data->rewardCounter++;
    if (data->rewardCounter > MAX_REWARD)
        data->rewardCounter = MAX_REWARD;
}

static u32 GetRewardType(struct WonderNewsMetadata *data)
{
    if (data->rewardCounter == MAX_REWARD)
        return NEWS_REWARD_AT_MAX;

    switch (data->newsType)
    {
    case WONDER_NEWS_NONE:
        return NEWS_REWARD_WAITING;
    case WONDER_NEWS_RECV_FRIEND:
        return NEWS_REWARD_RECV_SMALL;
    case WONDER_NEWS_RECV_WIRELESS:
        return NEWS_REWARD_RECV_BIG;
    case WONDER_NEWS_SENT:
        if (data->sentRewardCounter < MAX_SENT_REWARD - 1)
            return NEWS_REWARD_SENT_SMALL;
        return NEWS_REWARD_SENT_BIG;
    default:
        AGB_ASSERT_EX(0, ABSPATH("menews_jisan.c"), 383);
        return NEWS_REWARD_NONE;
    }
}
