/**
 * @file mystery_gift_scripts.c
 * @brief Mystery Gift Communication Scripts — Client/Server Command Sequences
 *
 * FILE OVERVIEW:
 * This file defines the scripted command sequences that drive Mystery Gift
 * communication between two GBA systems (a "server" distributing gifts and
 * a "client" receiving them). These are not game event scripts — they are
 * structured command arrays that the Mystery Gift state machine interprets
 * to orchestrate the multi-step wireless exchange.
 *
 * The protocol handles:
 *   - Wonder Card distribution (special event cards with rewards)
 *   - Wonder News distribution (news bulletins with berry rewards)
 *   - Error handling (communication errors, duplicate cards, cancellation)
 *   - Card replacement prompts (asking the player to toss an old card)
 *
 * ARCHITECTURE:
 * Each script is an array of command structs (MysteryGiftClientCmd or
 * MysteryGiftServerCmd). Commands include sending/receiving data over the
 * link, loading saved data, checking conditions, branching to other scripts,
 * and returning with a result message. The client and server scripts work
 * in tandem — the server tells the client which script to run next.
 */
#include "global.h"
#include "mystery_gift_server.h"
#include "mystery_gift_client.h"
#include "constants/mystery_gift.h"

extern const struct MysteryGiftServerCmd gServerScript_ClientCanceledCard[];

/* Unreferenced text — may have been used in a stamp-collecting Mystery Gift variant */
static const u8 sText_CollectedAllStamps[] = _("You have collected all STAMPs!\nWant to input a CARD as a prize?");

//==================
// Client scripts
//==================

const struct MysteryGiftClientCmd gMysteryGiftClientScript_Init[] = {
    {CLI_RECV, MG_LINKID_CLIENT_SCRIPT},
    {CLI_COPY_RECV}
};

static const struct MysteryGiftClientCmd sClientScript_SendGameData[] = {
    {CLI_LOAD_GAME_DATA},
    {CLI_SEND_LOADED},
    {CLI_RECV, MG_LINKID_CLIENT_SCRIPT},
    {CLI_COPY_RECV}
};

static const struct MysteryGiftClientCmd sClientScript_CantAccept[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_CANT_ACCEPT}
};

static const struct MysteryGiftClientCmd sClientScript_CommError[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_COMM_ERROR}
};

static const struct MysteryGiftClientCmd sClientScript_NothingSent[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_NOTHING_SENT}
};

static const struct MysteryGiftClientCmd sClientScript_SaveCard[] = {
    {CLI_RECV, MG_LINKID_CARD},
    {CLI_SAVE_CARD},
    {CLI_RECV, MG_LINKID_RAM_SCRIPT},
    {CLI_SAVE_RAM_SCRIPT},
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_CARD_RECEIVED}
};

static const struct MysteryGiftClientCmd sClientScript_SaveNews[] = {
    {CLI_RECV, MG_LINKID_NEWS},
    {CLI_SAVE_NEWS},
    {CLI_SEND_LOADED}, // Send whether or not the News was saved (read by sServerScript_SendNews)
    {CLI_RECV, MG_LINKID_CLIENT_SCRIPT},
    {CLI_COPY_RECV}
};

static const struct MysteryGiftClientCmd sClientScript_HadNews[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_HAD_NEWS}
};

static const struct MysteryGiftClientCmd sClientScript_NewsReceived[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_NEWS_RECEIVED}
};

static const struct MysteryGiftClientCmd sClientScript_AskToss[] = {
    {CLI_ASK_TOSS},
    {CLI_LOAD_TOSS_RESPONSE},
    {CLI_SEND_LOADED},
    {CLI_RECV, MG_LINKID_CLIENT_SCRIPT},
    {CLI_COPY_RECV}
};

static const struct MysteryGiftClientCmd sClientScript_Canceled[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_COMM_CANCELED}
};

static const struct MysteryGiftClientCmd sClientScript_HadCard[] = {
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_HAD_CARD}
};

static const struct MysteryGiftClientCmd sClientScript_DynamicSuccess[] = {
    {CLI_RECV, MG_LINKID_DYNAMIC_MSG},
    {CLI_COPY_MSG},
    {CLI_SEND_READY_END},
    {CLI_RETURN, CLI_MSG_BUFFER_SUCCESS}
};

//==================
// Server scripts
//==================

static const struct MysteryGiftServerCmd sServerScript_CantSend[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_CantAccept)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_CANT_SEND_GIFT_1}
};

static const struct MysteryGiftServerCmd sServerScript_CommError[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_CommError)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_COMM_ERROR}
};

static const struct MysteryGiftServerCmd sServerScript_ClientCanceledNews[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_Canceled)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_CLIENT_CANCELED}
};

static const struct MysteryGiftServerCmd sServerScript_HasNews[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_HadNews)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_HAS_NEWS}
};

static const struct MysteryGiftServerCmd sServerScript_SendNews[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_SaveNews)},
    {SVR_SEND},
    {SVR_LOAD_NEWS},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_RESPONSE},
    {SVR_READ_RESPONSE},
    {SVR_GOTO_IF_EQ, TRUE, sServerScript_HasNews}, // Wonder News was not saved
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_NewsReceived)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_NEWS_SENT}
};

static const struct MysteryGiftServerCmd sServerScript_SendCard[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_SaveCard)},
    {SVR_SEND},
    {SVR_LOAD_CARD},
    {SVR_SEND},
    {SVR_LOAD_RAM_SCRIPT},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_CARD_SENT}
};

static const struct MysteryGiftServerCmd sServerScript_TossPrompt[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_AskToss)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_RESPONSE},
    {SVR_READ_RESPONSE},
    {SVR_GOTO_IF_EQ, FALSE, sServerScript_SendCard},    // Tossed old card, send new one
    {SVR_GOTO, .ptr = gServerScript_ClientCanceledCard} // Kept old card, cancel new one
};

static const struct MysteryGiftServerCmd sServerScript_HasCard[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_HadCard)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_HAS_CARD}
};

static const struct MysteryGiftServerCmd sServerScript_NothingSent[] = {
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_NothingSent)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_READY_END},
    {SVR_RETURN, SVR_MSG_NOTHING_SENT}
};

const struct MysteryGiftServerCmd gMysteryGiftServerScript_SendWonderNews[] = {
    {SVR_COPY_SAVED_NEWS},
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_SendGameData)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_GAME_DATA},
    {SVR_COPY_GAME_DATA},
    {SVR_CHECK_GAME_DATA},
    {SVR_GOTO_IF_EQ, FALSE, sServerScript_CantSend},
    {SVR_GOTO, .ptr = sServerScript_SendNews},
};

const struct MysteryGiftServerCmd gMysteryGiftServerScript_SendWonderCard[] = {
    {SVR_COPY_SAVED_CARD},
    {SVR_COPY_SAVED_RAM_SCRIPT},
    {SVR_LOAD_CLIENT_SCRIPT, PTR_ARG(sClientScript_SendGameData)},
    {SVR_SEND},
    {SVR_RECV, MG_LINKID_GAME_DATA},
    {SVR_COPY_GAME_DATA},
    {SVR_CHECK_GAME_DATA},
    {SVR_GOTO_IF_EQ, FALSE, sServerScript_CantSend},
    {SVR_CHECK_EXISTING_CARD},
    {SVR_GOTO_IF_EQ, HAS_DIFF_CARD, sServerScript_TossPrompt},
    {SVR_GOTO_IF_EQ, HAS_NO_CARD, sServerScript_SendCard},
    {SVR_GOTO, .ptr = sServerScript_HasCard} // HAS_SAME_CARD
};
