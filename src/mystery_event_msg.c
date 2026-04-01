/**
 * @file mystery_event_msg.c
 * @brief Mystery Gift / Mystery Event Text Strings
 *
 * FILE OVERVIEW:
 * This file defines the text messages displayed to the player when they receive
 * items, Pokemon, or other rewards through the Mystery Gift / Mystery Event
 * system. Mystery Gift was a feature that let players receive special content
 * via wireless communication or the e-Reader peripheral.
 *
 * GBA CONTEXT:
 * All text strings in GBA Pokemon games use a custom character encoding (not
 * ASCII). The _() macro converts human-readable strings into the game's internal
 * encoding at compile time. Placeholders like {STR_VAR_1} and {STR_VAR_2} are
 * filled in at runtime with dynamic values (e.g., a Pokemon name or berry name).
 */
#include "global.h"

/* Message shown when the player receives a berry via Mystery Gift */
const u8 gText_MysteryGiftBerry[] = _("Obtained a {STR_VAR_2} BERRY!\nDad has it at PETALBURG GYM.");
const u8 gText_MysteryGiftBerryTransform[] = _("The {STR_VAR_1} BERRY transformed into\none {STR_VAR_2} BERRY.");
const u8 gText_MysteryGiftBerryObtained[] = _("The {STR_VAR_1} BERRY has already been\nobtained.");
const u8 gText_MysteryGiftSpecialRibbon[] = _("A special RIBBON was awarded to\nyour party POKéMON.");
const u8 gText_MysteryGiftNationalDex[] = _("The POKéDEX has been upgraded\nwith the NATIONAL MODE.");
const u8 gText_MysteryGiftRareWord[] = _("A rare word has been added.");
const u8 gText_MysteryGiftSentOver[] = _("{STR_VAR_1} was sent over!");
const u8 gText_MysteryGiftFullParty[] = _("Your party is full.\n{STR_VAR_1} could not be sent over.");
const u8 gText_MysteryGiftNewTrainer[] = _("A new TRAINER has arrived in\nHOENN.");
const u8 gText_MysteryGiftNewAdversaryInBattleTower[] = _("バトルタワーに　あらたな\nたいせんしゃが　あらわれた！");
const u8 gText_MysteryGiftCantBeUsed[] = _("This data can't be used in\nthis version.");
