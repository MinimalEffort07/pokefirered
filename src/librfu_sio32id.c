/**
 * @file librfu_sio32id.c
 * @brief RFU Wireless Adapter ID Check — 32-bit Serial I/O Identification Protocol
 *
 * FILE OVERVIEW:
 * This file implements the hardware identification protocol for the GBA Wireless
 * Adapter (RFU — Radio Frequency Unit). Before wireless communication can begin,
 * the GBA must verify that the device connected to the serial port is actually an
 * RFU adapter by exchanging a specific handshake sequence.
 *
 * The protocol works by sending the ASCII bytes for "NINTENDO" one pair at a time
 * over the 32-bit serial I/O interface, and checking that the adapter echoes back
 * the expected responses. If all 4 exchanges succeed, the adapter returns its
 * device ID (RFU_ID = 0x00008001).
 *
 * GBA CONTEXT:
 * The GBA's serial port (link cable connector) can operate in several modes.
 * This code uses SIO_32BIT_MODE, which transfers 32 bits per transaction.
 * The GBA can be either "master" (drives the clock) or "slave" (receives the
 * clock). REG_SIOCNT controls the serial port configuration. REG_SIODATA32
 * is the 32-bit data register for sending/receiving. REG_IME/REG_IE control
 * the interrupt system — serial transfers trigger interrupts when complete.
 *
 * The handshake uses a timer (selected by gSTWIStatus->timerSelect) as a delay
 * between retry attempts. Timer registers come in pairs: REG_TMCNT_L (counter
 * value) and REG_TMCNT_H (control: prescaler, enable bit).
 */
#include "librfu.h"

static void Sio32IDIntr(void);
static void Sio32IDInit(void);
static s32 Sio32IDMain(void);

/* State tracking for the 32-bit SIO identification handshake */
struct RfuSIO32Id
{
    u8 MS_mode;    /* Master/Slave mode (AGB_CLK_MASTER or AGB_CLK_SLAVE) */
    u8 state;      /* Protocol state machine step */
    u16 count;     /* Number of successful handshake exchanges (needs 4) */
    u16 send_id;   /* Current value being sent to the adapter */
    u16 recv_id;   /* Expected receive value (complement of what was sent) */
    u16 unk8;      /* Unused padding */
    u16 lastId;    /* The adapter's ID once handshake completes (RFU_ID on success) */
};

COMMON_DATA struct RfuSIO32Id gRfuSIO32Id = {0};

/* The handshake sequence: "IN", "TN", "NE", "DO" — spells "NINTENDO" when read
 * as pairs of ASCII bytes. Each 16-bit value is sent one at a time. */
static const u16 Sio32ConnectionData[] = { 0x494e, 0x544e, 0x4e45, 0x4f44 }; /* "NINTENDO" */
static const char Sio32IDLib_Var[] = "Sio32ID_030820";

/**
 * FUNCTION: AgbRFU_checkID
 *
 * PURPOSE: Attempts to identify an RFU Wireless Adapter connected to the serial port.
 *
 * HOW IT WORKS:
 * Sets up 32-bit serial I/O mode and repeatedly runs the handshake protocol.
 * Each attempt calls Sio32IDMain() to progress the state machine. Between
 * attempts, a hardware timer introduces a short delay. The adapter gets
 * maxTries * 8 attempts to respond before giving up.
 *
 * GBA CONTEXT:
 * REG_IME (Interrupt Master Enable) is toggled to safely modify REG_IE
 * (Interrupt Enable). This pattern — disable interrupts, modify IE, re-enable —
 * prevents race conditions where an interrupt could fire while the enable
 * mask is in a half-modified state.
 *
 * PARAMETERS:
 * @param maxTries — Number of retry cycles (each cycle = 8 actual attempts)
 *
 * RETURNS: The adapter ID (positive) on success, 0 if no response, -1 if
 *          interrupts are disabled.
 */
s32 AgbRFU_checkID(u8 maxTries)
{
    u16 ieBak;
    vu16 *regTMCNTL;
    s32 id;

    // Interrupts must be enabled
    if (REG_IME == 0)
        return -1;
    ieBak = REG_IE;
    gSTWIStatus->state = 10;
    STWI_set_Callback_ID(Sio32IDIntr);
    Sio32IDInit();
    regTMCNTL = &REG_TMCNT_L(gSTWIStatus->timerSelect);
    maxTries *= 8;
    while (--maxTries != 0xFF)
    {
        id = Sio32IDMain();
        if (id != 0)
            break;
        regTMCNTL[1] = 0;
        regTMCNTL[0] = 0;
        regTMCNTL[1] = TIMER_1024CLK | TIMER_ENABLE;
        while (regTMCNTL[0] < 32)
            ;
        regTMCNTL[1] = 0;
        regTMCNTL[0] = 0;
    }
    REG_IME = 0;
    REG_IE = ieBak;
    REG_IME = 1;
    gSTWIStatus->state = 0;
    STWI_set_Callback_ID(NULL);
    return id;
}

static void Sio32IDInit(void)
{
    REG_IME = 0;
    REG_IE &= ~((8 << gSTWIStatus->timerSelect) | INTR_FLAG_SERIAL);
    REG_IME = 1;
    REG_RCNT = 0;
    REG_SIOCNT = SIO_32BIT_MODE;
    REG_SIOCNT |= SIO_INTR_ENABLE | SIO_ENABLE;
    CpuFill32(0, &gRfuSIO32Id, sizeof(struct RfuSIO32Id));
    REG_IF = INTR_FLAG_SERIAL;
}

static s32 Sio32IDMain(void)
{
    switch (gRfuSIO32Id.state)
    {
    case 0:
        gRfuSIO32Id.MS_mode = AGB_CLK_MASTER;
        REG_SIOCNT |= SIO_38400_BPS;
        REG_IME = 0;
        REG_IE |= INTR_FLAG_SERIAL;
        REG_IME = 1;
        gRfuSIO32Id.state = 1;
        *(vu8 *)&REG_SIOCNT |= SIO_ENABLE;
        break;
    case 1:
        if (gRfuSIO32Id.lastId == 0)
        {
            if (gRfuSIO32Id.MS_mode == AGB_CLK_MASTER)
            {
                if (gRfuSIO32Id.count == 0)
                {
                    REG_IME = 0;
                    REG_SIOCNT |= SIO_ENABLE;
                    REG_IME = 1;
                }
            }
            else if (gRfuSIO32Id.send_id != RFU_ID && !gRfuSIO32Id.count)
            {
                REG_IME = 0;
                REG_IE &= ~INTR_FLAG_SERIAL;
                REG_IME = 1;
                REG_SIOCNT = 0;
                REG_SIOCNT = SIO_32BIT_MODE;
                REG_IF = INTR_FLAG_SERIAL;
                REG_SIOCNT |= SIO_INTR_ENABLE | SIO_ENABLE;
                REG_IME = 0;
                REG_IE |= INTR_FLAG_SERIAL;
                REG_IME = 1;
            }
            break;
        }
        else
        {
            gRfuSIO32Id.state = 2;
            // fallthrough
        }
    default:
        return gRfuSIO32Id.lastId;
    }
    return 0;
}

static void Sio32IDIntr(void)
{
    u32 regSIODATA32;
    u16 delay;
    u32 rfuSIO32IdUnk0_times_16;
    u16 negRfuSIO32IdUnk6;

    regSIODATA32 = REG_SIODATA32;
    if (gRfuSIO32Id.MS_mode != AGB_CLK_MASTER)
        REG_SIOCNT |= SIO_ENABLE;
    rfuSIO32IdUnk0_times_16 = (regSIODATA32 << (16 * gRfuSIO32Id.MS_mode)) >> 16;
    regSIODATA32 = (regSIODATA32 << 16 * (1 - gRfuSIO32Id.MS_mode)) >> 16;
    if (gRfuSIO32Id.lastId == 0)
    {
        u16 backup = rfuSIO32IdUnk0_times_16;
        if (backup == gRfuSIO32Id.recv_id)
        {
            if (gRfuSIO32Id.count < 4)
            {
                backup = (u16)~gRfuSIO32Id.send_id;
                if (gRfuSIO32Id.recv_id == backup)
                {
                    if (regSIODATA32 == (u16)~gRfuSIO32Id.recv_id)
                        ++gRfuSIO32Id.count;
                }
            }
            else
                gRfuSIO32Id.lastId = regSIODATA32;
        }
        else
        {
            gRfuSIO32Id.count = 0;
        }
    }
    if (gRfuSIO32Id.count < 4)
        gRfuSIO32Id.send_id = *(gRfuSIO32Id.count + Sio32ConnectionData);
    else
        gRfuSIO32Id.send_id = RFU_ID;
    gRfuSIO32Id.recv_id = ~regSIODATA32;
    REG_SIODATA32 = (gRfuSIO32Id.send_id << 16 * (1 - gRfuSIO32Id.MS_mode))
                  + (gRfuSIO32Id.recv_id << 16 * gRfuSIO32Id.MS_mode);
    if (gRfuSIO32Id.MS_mode == AGB_CLK_MASTER && (gRfuSIO32Id.count != 0 || regSIODATA32 == 0x494e))
    {
        for (delay = 0; delay < 600; ++delay)
            ;
        if (gRfuSIO32Id.lastId == 0)
            REG_SIOCNT |= SIO_ENABLE;
    }
}
