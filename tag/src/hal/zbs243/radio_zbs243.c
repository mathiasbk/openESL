/*
 * radio_zbs243.c - the 802.15.4 radio in ZBS243.
 *
 * The init values and the register meanings are from Dmitry Grinberg's ZBS243
 * reverse engineering, as used in the OpenEPaperLink tag firmware (see zbs243.h
 * and docs/kildelogg.md). Most init values have no known meaning; they are
 * written as found. The code is written for this project and runs without
 * interrupts: the pending flags in RADIO_IRQ_PENDING and TCON2 are polled.
 *
 * Contract (see hal.h):
 *   - radio_tx sends the frame without FCS; the radio appends the FCS.
 *   - radio_rx drops frames with a wrong FCS (the radio checks it) and returns
 *     the length without FCS.
 */
#include "../hal.h"
#include "proto.h"
#include "../../config.h"
#include "zbs243.h"

#define FRAME_MAX   127

static __xdata uint8_t txbuf[1 + FRAME_MAX];
static __xdata uint8_t rxbuf[1 + FRAME_MAX + 2];
static uint8_t cur_channel = 11;
static uint16_t cur_pan = CFG_PAN_ID;
static bool radio_up;

/* Up to max_ms milliseconds measured with timer 0 (16 MHz / 12): ms_start, then ms_tick. */
static void ms_start(void)
{
    TCON &= (uint8_t)~0x30;
    TH0 = (uint8_t)((0x10000u - 1333u) >> 8);
    TL0 = (uint8_t)(0x10000u - 1333u);
    TCON |= 0x10;
}

/* True once per elapsed millisecond (and restarts the next one). */
static bool ms_tick(void)
{
    if (!(TCON & 0x20))
        return false;
    ms_start();
    return true;
}

static void wait_ms_flag_tcon2(uint8_t mask, uint8_t max_ms)
{
    ms_start();
    CFGPAGE = 4;
    while (!(TCON2 & mask)) {
        if (ms_tick() && !max_ms--)
            break;
    }
    CFGPAGE = 0;
    TCON &= (uint8_t)~0x30;
}

static void set_channel(uint8_t ch)
{
    /* Per-channel values for channels 11-28, as found */
    static const uint8_t __code set1[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22,
                                          0x22, 0x22, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33};
    static const uint8_t __code set2[] = {4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2};

    if (ch < 11 || ch > 26)
        ch = 11;
    RADIO_CHANNEL = ch;
    RADIO_COMMAND = RADIO_CMD_RECEIVE;
    RADIO_CH_SETTING1 = set1[ch - 11];
    RADIO_CH_SETTING2 = set2[ch - 11];
}

/* Frame filter: our extended address and PAN. No short address (0xFFFF). */
static uint8_t mac[8];

static void set_filter(void)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
        RADIO_OWN_MAC[i] = mac[7 - i];
    RADIO_PAN_HI = (uint8_t)(cur_pan >> 8);
    RADIO_PAN_LO = (uint8_t)cur_pan;
    RADIO_SHORT_HI = 0x00;
    RADIO_SHORT_LO = 0x00;
}

void radio_set_pan(uint16_t pan)
{
    cur_pan = pan;
    if (radio_up)
        set_filter();
}

void radio_init(uint8_t channel)
{
    cur_channel = channel;
    radio_on();
}

void radio_on(void)
{
    /*
     * Read the MAC before the radio init: the flash DMA in hal_mac_read writes
     * SETTINGS, which would clear the radio bits the init sets there.
     */
    hal_mac_read(mac);
    hal_delay_ms(1);
    RADIO_UNK_F0 |= 0x80;
    hal_delay_ms(1);
    CFGPAGE = 0;
    RESET &= (uint8_t)~0x04;
    RESET |= 0x04;               /* pulse the radio reset */
    RESET &= (uint8_t)~0x04;
    hal_delay_ms(1);

    RADIO_POWER_CTL |= 0x04;

    CFGPAGE = 4;
    SETTINGS |= 0x02;
    RADIO_INITSEQ0 = 0x02;
    RADIO_INITSEQ1 = 0xFA;
    RADIO_INITSEQ2 = 0xDD;
    SETTINGS |= 0x04;
    RADIO_INITSEQ3 = 0x01;
    RADIO_INITSEQ4 = 0xFA;
    RADIO_INITSEQ5 = 0xDD;
    CFGPAGE = 0;

    RADIO_COMMAND = 0xC1;

    RADIO_UNK_C1X = 0x02;
    RADIO_CAL_C2 = 0xF7;
    RADIO_CAL_C3 = 0x05;
    RADIO_CAL_C4 = 0x35;
    RADIO_CAL_C5 = 0x24;
    RADIO_CAL_C6 = 0x33;
    RADIO_CAL_C7 = 0x70;
    RADIO_UNK_CA = 0x58;
    RADIO_CH_SETTING2 = 0x02;
    RADIO_UNK_CD = (RADIO_UNK_CD & (uint8_t)~0x07) | 0x01;
    RADIO_TX_POWER = 0;
    RADIO_CAL_CF = 0x30;
    RADIO_CAL_D0 = 0x00;
    RADIO_CAL_D1 = 0x49;
    RADIO_CAL_D2 = 0x06;
    RADIO_UNK_D7 = 0x43;
    RADIO_UNK_E2 = 0x08;
    RADIO_UNK_83 = 0xE5;
    RADIO_UNK_C8 = 0x7F;
    RADIO_CAL_81 = 0xF0;
    RADIO_FLAGS |= 0x08;
    RADIO_UNK_D8 = 0x00;
    RADIO_CAL_9D = 0x3F;
    RADIO_CAL_A1 = 0x04;
    RADIO_CAL_94 = 0x7F;
    RADIO_UNK_CE = 0x55;
    RADIO_CAL_D3 = 0x30;
    RADIO_CAL_D4 = 0xCC;
    RADIO_CAL_D5 = 0xF6;
    RADIO_CAL_86 = 0x40;
    RADIO_CAL_95 = 0x08;
    RADIO_CAL_96 = 0xD3;
    RADIO_CAL_70 = 0x01;
    RADIO_CAL_71 = 0x40;

    RADIO_UNK_A6 = 0x00;
    RADIO_COMMAND = 0xC8;
    RADIO_COMMAND = RADIO_CMD_FLUSH_TX;
    RADIO_COMMAND = RADIO_CMD_FLUSH_RX;
    RADIO_IRQ_ENABLE = 0x70;     /* the pending bits we poll: 0x40, 0x20, 0x10 */
    RADIO_FLAGS |= 0x08;

    /* The init sets TCON2 0x08 when done; clear it, or the next flash read ends at once. */
    wait_ms_flag_tcon2(0x08, 10);
    CFGPAGE = 4;
    TCON2 &= (uint8_t)~0x48;
    SETTINGS &= (uint8_t)~0x10;
    CFGPAGE = 0;

    RADIO_TX_POWER = 7;          /* highest setting, +8 dBm */
    radio_up = true;
    set_filter();
    set_channel(cur_channel);
    RADIO_IRQ_PENDING = 0;
}

void radio_off(void)
{
    if (!radio_up)
        return;
    RADIO_IRQ_PENDING = 0;
    RADIO_UNK_C1 &= (uint8_t)~0x81;     /* in page 0, as the OEPL power-down does */
    CFGPAGE = 4;
    RADIO_COMMAND = 0xCA;
    RADIO_COMMAND = 0xC5;
    CFGPAGE = 0;
    radio_up = false;
}

bool radio_tx(const uint8_t *frame, uint8_t len)
{
    uint8_t i, cause;
    uint16_t wait;

    if (!radio_up || len > FRAME_MAX - 2)
        return false;
    txbuf[0] = len + 2;          /* PHY length includes the FCS the radio appends */
    for (i = 0; i < len; i++)
        txbuf[1 + i] = frame[i];

    RADIO_IRQ_PENDING = 0;
    CFGPAGE = 4;
    RADIO_TXLEN = len;
    RADIO_TXPTRL = (uint8_t)(uint16_t)txbuf;
    RADIO_TXPTRH = (uint8_t)((uint16_t)txbuf >> 8);
    RADIO_COMMAND = RADIO_CMD_LOAD_TX;
    TRIGGER |= 0x02;             /* TX DMA */
    CFGPAGE = 0;

    for (wait = 0xFFFF; wait; wait--)
        if (RADIO_RF_STATE & 0x80)
            break;
    if (!wait)
        return false;

    /* The radio raises 0x40 when the frame (and the ACK window) is over. */
    ms_start();
    i = 10;
    cause = 0;
    while (!(cause & 0x40)) {
        cause |= RADIO_IRQ_PENDING;
        if (ms_tick() && !i--) {
            TCON &= (uint8_t)~0x30;
            return false;
        }
    }
    TCON &= (uint8_t)~0x30;
    RADIO_IRQ_PENDING = 0;
    CFGPAGE = 4;
    TCON2 &= (uint8_t)~0x02;
    CFGPAGE = 0;
    RADIO_COMMAND = RADIO_CMD_RECEIVE;
    return true;
}

/*
 * Copies one frame from the RX FIFO with DMA into rxbuf + 1. Returns the length
 * without FCS, or -1.
 */
static __xdata uint8_t rx_len;

/* Starts the RX DMA of rx_len + 1 bytes into rxbuf + 1. No locals: see fetch_frame. */
static void rx_dma_start(void)
{
    CFGPAGE = 4;
    RADIO_RXLEN = rx_len - 1;
    RADIO_RXPTRL = (uint8_t)(uint16_t)(rxbuf + 1);
    RADIO_RXPTRH = (uint8_t)((uint16_t)(rxbuf + 1) >> 8);
    TRIGGER |= 0x04;
    TCON2 &= (uint8_t)~0x04;
    CFGPAGE = 0;
}

static int8_t fetch_frame(uint8_t *buf, uint8_t maxlen)
{
    uint8_t i, n;

    /*
     * RADIO_GOTLEN is a read port on the RX FIFO: the first read gives the PHY
     * length byte (frame + 2), and every further read pops the next frame byte
     * (seen on the tag: 0x29, then 0x41 = the first frame byte). So read it once,
     * first thing, and never poll it.
     */
    rx_len = RADIO_GOTLEN;
    if (rx_len < 3 || rx_len >= 0x80 || (uint8_t)(RADIO_RX_FIRST + 1) != rx_len) {
        RADIO_COMMAND = RADIO_CMD_FLUSH_RX;
        return -1;
    }
    /*
     * Keep register variables out of code that runs with CFGPAGE = 4: with the
     * length in a register across that section, the frame came out one byte short
     * (the length read was right, the computed size was one less). Not understood;
     * the length now lives in xdata.
     */
    rx_dma_start();
    wait_ms_flag_tcon2(0x04, 2);
    CFGPAGE = 4;
    TCON2 &= (uint8_t)~0x04;
    CFGPAGE = 0;
    RADIO_COMMAND = RADIO_CMD_FLUSH_RX;

    /*
     * Measured on the tag: rxbuf[1 .. len - 2] is the frame without FCS, then
     * rxbuf[len - 1] is the RSSI and rxbuf[len] has 0x80 = FCS correct | LQI.
     */
    if (!(rxbuf[rx_len] & 0x80))
        return -1;               /* FCS wrong */
    n = rx_len - 2;
    if (n > maxlen)
        return -1;
    for (i = 0; i < n; i++)
        buf[i] = rxbuf[1 + i];
    return (int8_t)n;
}

int8_t radio_rx(uint8_t *buf, uint8_t maxlen, uint8_t timeout_ms)
{
    uint8_t cause;
    int8_t n;

    if (!radio_up)
        return -1;
    ms_start();
    for (;;) {
        cause = RADIO_IRQ_PENDING;
        if (cause) {
            RADIO_IRQ_PENDING = 0;
            if (cause & 0x20) {
                n = fetch_frame(buf, maxlen);
                if (n >= 0) {
                    TCON &= (uint8_t)~0x30;
                    return n;
                }
                ms_start();      /* fetch_frame used timer 0 */
            }
        }
        if (ms_tick() && !timeout_ms--)
            break;
    }
    TCON &= (uint8_t)~0x30;
    return -1;
}
