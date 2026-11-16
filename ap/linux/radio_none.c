/* radio_none.c - --radio none: no radio of the AP's own, for an AP whose tags are all served
 * by network radios (--netradio, docs/linux-ap.md). The radio loop still runs, for the
 * scheduler and the channel moves: it waits here, never hears a frame, and a move goes to
 * the network radio on the AP's channel (netradio_set_channel). */
#define _DEFAULT_SOURCE
#include <poll.h>
#include <stdio.h>
#include "ap.h"

static int none_start(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    (void)l;
    (void)channel;
    (void)pan;
    (void)short_addr;
    (void)promiscuous;
    return 0;
}

static int none_open(esl_link_t *l, const char *arg)
{
    if (*arg) {
        fprintf(stderr, "openesl-ap: --radio none takes nothing after it, not none:%s\n", arg);
        return -1;
    }
    l->name = "no";
    l->start = none_start;
    l->set_channel = netradio_set_channel;
    return 0;
}

static int none_recv(esl_link_t *l, esl_link_rx_t *f, int timeout_ms)
{
    (void)l;
    (void)f;
    poll(NULL, 0, timeout_ms);
    return 0;
}

const radio_backend_t radio_none = {
    "none", "none             no radio of its own: only network radios (--netradio)", none_open, none_recv, NULL,
};
