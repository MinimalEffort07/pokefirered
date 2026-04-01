/**
 * @file mystery_gift_link.c
 * @brief Mystery Gift Link Layer — Reliable Data Transfer over GBA Link Cable/Wireless
 *
 * FILE OVERVIEW:
 * This file implements the data transfer protocol used by the Mystery Gift system
 * to reliably send and receive large data blocks between two GBA systems. It sits
 * between the raw link hardware layer (link.c / link_rfu_*.c) and the Mystery Gift
 * application layer (mystery_gift_client.c / mystery_gift_server.c).
 *
 * The protocol handles:
 *   - Splitting large payloads into 252-byte chunks (the link block size limit)
 *   - Sending a header first (identifier, CRC16 checksum, total size)
 *   - Reassembling chunks on the receiving end
 *   - CRC16 verification after reassembly to detect data corruption
 *   - Fatal error handling if sizes exceed buffer limits or IDs don't match
 *
 * GBA CONTEXT:
 * The GBA link cable can only send small blocks at a time. gBlockRecvBuffer and
 * gBlockSendBuffer are the hardware-level transfer buffers. SendBlock() queues
 * data for transmission, and GetBlockReceivedStatus() checks if data has arrived.
 * Each transfer is acknowledged before the next block is sent, ensuring reliable
 * in-order delivery. Internal functions use the MGL_ prefix (MysteryGiftLink).
 */
#include "global.h"
#include "util.h"
#include "link.h"
#include "link_rfu.h"
#include "mystery_gift_server.h"

struct SendRecvHeader
{
    u16 ident;
    u16 crc;
    u16 size;
};

static u32 MGL_Receive(struct MysteryGiftLink *);
static u32 MGL_Send(struct MysteryGiftLink *);

u32 MysteryGiftLink_Recv(struct MysteryGiftLink * link)
{
    return link->recvFunc(link);
}

u32 MysteryGiftLink_Send(struct MysteryGiftLink * link)
{
    return link->sendFunc(link);
}

void MysteryGiftLink_Init(struct MysteryGiftLink * link, u32 sendPlayerId, u32 recvPlayerId)
{
    link->sendPlayerId = sendPlayerId;
    link->recvPlayerId = recvPlayerId;
    link->state = 0;
    link->sendCRC = 0;
    link->sendSize = 0;
    link->sendCounter = 0;
    link->recvCRC = 0;
    link->recvSize = 0;
    link->recvCounter = 0;
    link->sendBuffer = NULL;
    link->recvBuffer = NULL;
    link->sendFunc = MGL_Send;
    link->recvFunc = MGL_Receive;
}

void MysteryGiftLink_InitSend(struct MysteryGiftLink * link, u32 ident, const void *src, u32 size)
{
    link->state = 0;
    link->sendIdent = ident;
    link->sendCounter = 0;
    link->sendCRC = 0;
    if (size != 0)
        link->sendSize = size;
    else
        link->sendSize = MG_LINK_BUFFER_SIZE;
    link->sendBuffer = src;
}

void MysteryGiftLink_InitRecv(struct MysteryGiftLink * link, u32 ident, void *dest)
{
    link->state = 0;
    link->recvIdent = ident;
    link->recvCounter = 0;
    link->recvCRC = 0;
    link->recvSize = 0;
    link->recvBuffer = dest;
}

static void MGL_ReceiveBlock(u32 playerId, void *dest, size_t size)
{
    memcpy(dest, gBlockRecvBuffer[playerId], size);
}

static bool32 MGL_HasReceived(u32 playerId)
{
    if ((GetBlockReceivedStatus() >> playerId) & 1)
        return TRUE;
    else
        return FALSE;
}

static void MGL_ResetReceived(u32 playerId)
{
    ResetBlockReceivedFlag(playerId);
}

static bool32 MGL_Receive(struct MysteryGiftLink * link)
{
    struct SendRecvHeader header;

    switch (link->state)
    {
    case 0:
        if (MGL_HasReceived(link->recvPlayerId))
        {
            MGL_ReceiveBlock(link->recvPlayerId, &header, sizeof(header));
            link->recvSize = header.size;
            link->recvCRC = header.crc;
            if (link->recvSize > MG_LINK_BUFFER_SIZE)
            {
                LinkRfu_FatalError();
                return FALSE;
            }
            else if (link->recvIdent != header.ident)
            {
                LinkRfu_FatalError();
                return FALSE;
            }
            else
            {
                link->recvCounter = 0;
                MGL_ResetReceived(link->recvPlayerId);
                link->state++;
            }
        }
        break;
    case 1:
        if (MGL_HasReceived(link->recvPlayerId))
        {
            size_t blocksize = link->recvCounter * 252;
            if (link->recvSize - blocksize <= 252)
            {
                MGL_ReceiveBlock(link->recvPlayerId, link->recvBuffer + blocksize, link->recvSize - blocksize);
                link->recvCounter++;
                link->state++;
            }
            else
            {
                MGL_ReceiveBlock(link->recvPlayerId, link->recvBuffer + blocksize, 252);
                link->recvCounter++;
            }
            MGL_ResetReceived(link->recvPlayerId);
        }
        break;
    case 2:
        if (CalcCRC16WithTable(link->recvBuffer, link->recvSize) != link->recvCRC)
        {
            LinkRfu_FatalError();
            return FALSE;
        }
        else
        {
            link->state = 0;
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static bool32 MGL_Send(struct MysteryGiftLink * link)
{
    struct SendRecvHeader header;

    switch (link->state)
    {
    case 0:
        if (IsLinkTaskFinished())
        {
            header.ident = link->sendIdent;
            header.size = link->sendSize;
            header.crc = CalcCRC16WithTable(link->sendBuffer, link->sendSize);
            link->sendCRC = header.crc;
            link->sendCounter = 0;
            SendBlock(0, &header, sizeof(header));
            link->state++;
        }
        break;
    case 1:
        if (IsLinkTaskFinished())
        {
            if (MGL_HasReceived(link->sendPlayerId))
            {
                size_t blocksize;
                MGL_ResetReceived(link->sendPlayerId);
                blocksize = 252 * link->sendCounter;
                if (link->sendSize - blocksize <= 252)
                {
                    SendBlock(0, link->sendBuffer + blocksize, link->sendSize - blocksize);
                    link->sendCounter++;
                    link->state++;
                }
                else
                {
                    SendBlock(0, link->sendBuffer + blocksize, 252);
                    link->sendCounter++;
                }
            }
        }
        break;
    case 2:
        if (IsLinkTaskFinished())
        {
            if (CalcCRC16WithTable(link->sendBuffer, link->sendSize) != link->sendCRC)
                LinkRfu_FatalError();
            else
                link->state++;
        }
        break;
    case 3:
        if (MGL_HasReceived(link->sendPlayerId))
        {
            MGL_ResetReceived(link->sendPlayerId);
            link->state = 0;
            return TRUE;
        }
        break;
    }

    return FALSE;
}
