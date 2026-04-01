/**
 * @file quest_log_objects.c
 * @brief Quest Log Object Event Serialization — Save/Restore NPC States for Playback
 *
 * FILE OVERVIEW:
 * The Quest Log (called "Adventure Log" in-game) is a FireRed/LeafGreen feature
 * that records and replays recent gameplay events. To replay a scene accurately,
 * the game must save and restore the exact state of every NPC and object on the
 * map — their positions, facing directions, animation states, visibility, etc.
 *
 * This file handles the serialization (recording) and deserialization (loading)
 * of all object events (NPCs, the player, and interactive objects) for the Quest
 * Log system. It copies each object's many state fields into a compact Quest Log
 * format for storage, and can reconstruct them for playback.
 *
 * The file also handles a special case: if the Quest Log playback moves the player
 * from water to land, it must stop the surfing animation and switch back to walking.
 *
 * GBA CONTEXT:
 * Object events on the GBA are tracked in a fixed-size array (gObjectEvents) in
 * RAM. Each entry has dozens of bitfields for state like visibility, animation,
 * facing direction, elevation, etc. The Quest Log system must snapshot all of
 * this data compactly because save RAM is limited.
 */
#include "global.h"
#include "quest_log.h"
#include "fieldmap.h"
#include "field_player_avatar.h"
#include "metatile_behavior.h"

/**
 * FUNCTION: QL_RecordObjects
 *
 * PURPOSE: Snapshots the current state of all object events (NPCs, player, etc.)
 *          into the Quest Log scene structure for later playback.
 *
 * HOW IT WORKS:
 * Iterates through all OBJECT_EVENTS_COUNT slots and copies each object's fields
 * one-by-one from the live gObjectEvents array into the compact questLog format.
 * The Quest Log struct uses a separate QuestLogObjectEvent layout that stores
 * only the fields needed for replay.
 *
 * PARAMETERS:
 * @param questLog — Pointer to the Quest Log scene to write into
 */
void QL_RecordObjects(struct QuestLogScene * questLog)
{
    u32 i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        questLog->objectEvents[i].active                         = gObjectEvents[i].active;
        questLog->objectEvents[i].triggerGroundEffectsOnStop     = gObjectEvents[i].triggerGroundEffectsOnStop;
        questLog->objectEvents[i].disableCoveringGroundEffects   = gObjectEvents[i].disableCoveringGroundEffects;
        questLog->objectEvents[i].landingJump                    = gObjectEvents[i].landingJump;
        questLog->objectEvents[i].frozen                         = gObjectEvents[i].frozen;
        questLog->objectEvents[i].facingDirectionLocked          = gObjectEvents[i].facingDirectionLocked;
        questLog->objectEvents[i].disableAnim                    = gObjectEvents[i].disableAnim;
        questLog->objectEvents[i].enableAnim                     = gObjectEvents[i].enableAnim;
        questLog->objectEvents[i].inanimate                      = gObjectEvents[i].inanimate;
        questLog->objectEvents[i].invisible                      = gObjectEvents[i].invisible;
        questLog->objectEvents[i].offScreen                      = gObjectEvents[i].offScreen;
        questLog->objectEvents[i].trackedByCamera                = gObjectEvents[i].trackedByCamera;
        questLog->objectEvents[i].isPlayer                       = gObjectEvents[i].isPlayer;
        questLog->objectEvents[i].spriteAnimPausedBackup         = gObjectEvents[i].spriteAnimPausedBackup;
        questLog->objectEvents[i].spriteAffineAnimPausedBackup   = gObjectEvents[i].spriteAffineAnimPausedBackup;
        questLog->objectEvents[i].disableJumpLandingGroundEffect = gObjectEvents[i].disableJumpLandingGroundEffect;
        questLog->objectEvents[i].fixedPriority                  = gObjectEvents[i].fixedPriority;
        questLog->objectEvents[i].facingDirection                = gObjectEvents[i].facingDirection;
        questLog->objectEvents[i].currentElevation               = gObjectEvents[i].currentElevation;
        questLog->objectEvents[i].previousElevation              = gObjectEvents[i].previousElevation;
        questLog->objectEvents[i].graphicsId                     = gObjectEvents[i].graphicsId;
        questLog->objectEvents[i].movementType                   = gObjectEvents[i].movementType;
        questLog->objectEvents[i].trainerType                    = gObjectEvents[i].trainerType;
        questLog->objectEvents[i].localId                        = gObjectEvents[i].localId;
        questLog->objectEvents[i].mapNum                         = gObjectEvents[i].mapNum;
        questLog->objectEvents[i].mapGroup                       = gObjectEvents[i].mapGroup;
        questLog->objectEvents[i].x                              = gObjectEvents[i].currentCoords.x;
        questLog->objectEvents[i].y                              = gObjectEvents[i].currentCoords.y;
        questLog->objectEvents[i].trainerRange_berryTreeId       = gObjectEvents[i].trainerRange_berryTreeId;
        questLog->objectEvents[i].previousMetatileBehavior       = gObjectEvents[i].previousMetatileBehavior;
        questLog->objectEvents[i].directionSequenceIndex         = gObjectEvents[i].directionSequenceIndex;
        questLog->objectEvents[i].animId                         = gObjectEvents[i].playerCopyableMovement;
    }
}

/**
 * FUNCTION: QL_LoadObjects
 *
 * PURPOSE: Restores all object events from a Quest Log snapshot for playback.
 *
 * HOW IT WORKS:
 * 1. Zeroes out the entire gObjectEvents array with CpuFill16
 * 2. Copies all saved fields from the Quest Log back into gObjectEvents
 * 3. Cross-references each object's localId against the map's ObjectEventTemplates
 *    to restore initial coordinates and movement ranges (not saved in Quest Log)
 * 4. Recalculates the metatile behavior at each object's current position
 * 5. Attempts to reconstruct previousCoords by checking adjacent tiles for a
 *    matching metatile behavior (since exact previous coords weren't saved)
 * 6. Copies the reconstructed state into the save block for consistency
 *
 * PARAMETERS:
 * @param questLog  — The Quest Log scene containing saved object states
 * @param templates — The current map's object event templates (for initial coords)
 */
void QL_LoadObjects(const struct QuestLogScene * questLog, const struct ObjectEventTemplate * templates)
{
    u32 i, j;
    const struct QuestLogObjectEvent * questLogObjectEvents = questLog->objectEvents;

    CpuFill16(0, gObjectEvents, sizeof(gObjectEvents));

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        gObjectEvents[i].active                         = questLogObjectEvents[i].active;
        gObjectEvents[i].triggerGroundEffectsOnStop     = questLogObjectEvents[i].triggerGroundEffectsOnStop;
        gObjectEvents[i].disableCoveringGroundEffects   = questLogObjectEvents[i].disableCoveringGroundEffects;
        gObjectEvents[i].landingJump                    = questLogObjectEvents[i].landingJump;
        gObjectEvents[i].frozen                         = questLogObjectEvents[i].frozen;
        gObjectEvents[i].facingDirectionLocked          = questLogObjectEvents[i].facingDirectionLocked;
        gObjectEvents[i].disableAnim                    = questLogObjectEvents[i].disableAnim;
        gObjectEvents[i].enableAnim                     = questLogObjectEvents[i].enableAnim;
        gObjectEvents[i].inanimate                      = questLogObjectEvents[i].inanimate;
        gObjectEvents[i].invisible                      = questLogObjectEvents[i].invisible;
        gObjectEvents[i].offScreen                      = questLogObjectEvents[i].offScreen;
        gObjectEvents[i].trackedByCamera                = questLogObjectEvents[i].trackedByCamera;
        gObjectEvents[i].isPlayer                       = questLogObjectEvents[i].isPlayer;
        gObjectEvents[i].spriteAnimPausedBackup         = questLogObjectEvents[i].spriteAnimPausedBackup;
        gObjectEvents[i].spriteAffineAnimPausedBackup   = questLogObjectEvents[i].spriteAffineAnimPausedBackup;
        gObjectEvents[i].disableJumpLandingGroundEffect = questLogObjectEvents[i].disableJumpLandingGroundEffect;
        gObjectEvents[i].fixedPriority                  = questLogObjectEvents[i].fixedPriority;
        gObjectEvents[i].facingDirection                = questLogObjectEvents[i].facingDirection;
        gObjectEvents[i].currentElevation               = questLogObjectEvents[i].currentElevation;
        gObjectEvents[i].previousElevation              = questLogObjectEvents[i].previousElevation;
        gObjectEvents[i].graphicsId                     = questLogObjectEvents[i].graphicsId;
        gObjectEvents[i].movementType                   = questLogObjectEvents[i].movementType;
        gObjectEvents[i].trainerType                    = questLogObjectEvents[i].trainerType;
        gObjectEvents[i].localId                        = questLogObjectEvents[i].localId;
        gObjectEvents[i].mapNum                         = questLogObjectEvents[i].mapNum;
        gObjectEvents[i].mapGroup                       = questLogObjectEvents[i].mapGroup;
        gObjectEvents[i].currentCoords.x                = questLogObjectEvents[i].x;
        gObjectEvents[i].currentCoords.y                = questLogObjectEvents[i].y;
        gObjectEvents[i].trainerRange_berryTreeId       = questLogObjectEvents[i].trainerRange_berryTreeId;
        gObjectEvents[i].previousMetatileBehavior       = questLogObjectEvents[i].previousMetatileBehavior;
        gObjectEvents[i].directionSequenceIndex         = questLogObjectEvents[i].directionSequenceIndex;
        gObjectEvents[i].playerCopyableMovement         = questLogObjectEvents[i].animId;

        for (j = 0; j < OBJECT_EVENT_TEMPLATES_COUNT; j++)
        {
            if (gObjectEvents[i].localId == templates[j].localId)
            {
                gObjectEvents[i].initialCoords.x = templates[j].x + MAP_OFFSET;
                gObjectEvents[i].initialCoords.y = templates[j].y + MAP_OFFSET;
                gObjectEvents[i].rangeX = templates[j].objUnion.normal.movementRangeX;
                gObjectEvents[i].rangeY = templates[j].objUnion.normal.movementRangeY;
            }
        }

        gObjectEvents[i].currentMetatileBehavior = MapGridGetMetatileBehaviorAt(gObjectEvents[i].currentCoords.x, gObjectEvents[i].currentCoords.y);
        if (gObjectEvents[i].previousMetatileBehavior == MapGridGetMetatileBehaviorAt((s16)(gObjectEvents[i].currentCoords.x), (s16)(gObjectEvents[i].currentCoords.y)))
        {
            gObjectEvents[i].previousCoords.x = gObjectEvents[i].currentCoords.x;
            gObjectEvents[i].previousCoords.y = gObjectEvents[i].currentCoords.y;
        }
        else if (gObjectEvents[i].previousMetatileBehavior == MapGridGetMetatileBehaviorAt((s16)(gObjectEvents[i].currentCoords.x - 1), (s16)(gObjectEvents[i].currentCoords.y)))
        {
            gObjectEvents[i].previousCoords.x = gObjectEvents[i].currentCoords.x - 1;
            gObjectEvents[i].previousCoords.y = gObjectEvents[i].currentCoords.y;
        }
        else if (gObjectEvents[i].previousMetatileBehavior == MapGridGetMetatileBehaviorAt((s16)(gObjectEvents[i].currentCoords.x + 1), (s16)(gObjectEvents[i].currentCoords.y)))
        {
            gObjectEvents[i].previousCoords.x = gObjectEvents[i].currentCoords.x + 1;
            gObjectEvents[i].previousCoords.y = gObjectEvents[i].currentCoords.y;
        }
        else if (gObjectEvents[i].previousMetatileBehavior == MapGridGetMetatileBehaviorAt((s16)(gObjectEvents[i].currentCoords.x), (s16)(gObjectEvents[i].currentCoords.y - 1)))
        {
            gObjectEvents[i].previousCoords.x = gObjectEvents[i].currentCoords.x;
            gObjectEvents[i].previousCoords.y = gObjectEvents[i].currentCoords.y - 1;
        }
        else if (gObjectEvents[i].previousMetatileBehavior == MapGridGetMetatileBehaviorAt((s16)(gObjectEvents[i].currentCoords.x), (s16)(gObjectEvents[i].currentCoords.y + 1)))
        {
            gObjectEvents[i].previousCoords.x = gObjectEvents[i].currentCoords.x;
            gObjectEvents[i].previousCoords.y = gObjectEvents[i].currentCoords.y + 1;
        }
    }

    CpuCopy16(gObjectEvents, gSaveBlock1Ptr->objectEvents, sizeof(gObjectEvents));
}

/**
 * FUNCTION: QL_TryStopSurfing
 *
 * PURPOSE: During Quest Log playback, checks if the player has moved from water
 *          to land and stops the surfing animation if so.
 *
 * HOW IT WORKS:
 * Only acts during Quest Log playback (QL_STATE_PLAYBACK). Gets the player's
 * destination tile and checks if it's a non-surfable metatile. If the player
 * is currently flagged as surfing but standing on land, transitions them to
 * the on-foot state and destroys the surf Pokemon sprite that follows behind.
 */
void QL_TryStopSurfing(void)
{
    if (gQuestLogState == QL_STATE_PLAYBACK)
    {
        s16 x, y;

        PlayerGetDestCoords(&x, &y);
        if (!MetatileBehavior_IsSurfable(MapGridGetMetatileBehaviorAt(x, y)) && TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING))
        {
            struct ObjectEvent * objectEvent = &gObjectEvents[gPlayerAvatar.objectEventId];
            SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT);
            DestroySprite(&gSprites[objectEvent->fieldEffectSpriteId]);
        }
    }
}
