/*
 * radio_zbs243.c - the 802.15.4 radio in ZBS243.
 *
 * STUB. The radio registers are the biggest unknown in the project (phase 3, the "gate").
 * They are filled in from experiments, community research and a backup of the
 * original firmware, with the C6 sniffer as the reference. Every register fact is
 * recorded in docs/kildelogg.md with its source.
 *
 * Contract (see hal.h):
 *   - radio_tx sends the frame without FCS; it does not add FCS itself, so this
 *     file must do it with proto_fcs().
 *   - radio_rx drops frames with a wrong FCS and returns the length without FCS.
 */
#include "../hal.h"

void radio_init(uint8_t channel)
{
    (void)channel;   /* TODO phase 3: channel 11-26 -> register */
}

void radio_on(void)
{
    /* TODO phase 3 */
}

void radio_off(void)
{
    /* TODO phase 3 */
}

bool radio_tx(const uint8_t *frame, uint8_t len)
{
    (void)frame;
    (void)len;
    return false;    /* TODO phase 3 */
}

int8_t radio_rx(uint8_t *buf, uint8_t maxlen, uint8_t timeout_ms)
{
    (void)buf;
    (void)maxlen;
    hal_delay_ms(timeout_ms);
    return -1;       /* TODO phase 3 */
}
