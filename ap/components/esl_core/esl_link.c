#include "esl_link.h"

size_t esl_link_handle(esl_link_t *l, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const esl_link_rx_t *f,
                       uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev, int64_t (*clock_us)(void), bool *sent)
{
    esl_radio_event_t own;
    if (!ev)
        ev = &own;
    if (sent)
        *sent = false;
    l->stats.rx++;
    size_t len = esl_radio_handle(st, ls, pan, f->psdu, f->len, f->rssi, now_ms, tx, ev);
    if (!len)
        return 0; /* not a POLL, FW_REQ or IMG_REQ for us */
    if (ev->msg_type == ESL_MSG_POLL)
        l->stats.polls++;
    else
        l->stats.blocks++;
    l->stats.last_us = -1; /* esl_link_sending() sets it */
    bool ok = l->send(l, tx, len, f) == 0;
    if (ok)
        l->stats.replies++;
    else
        l->stats.tx_failed++;
    if (l->stats.last_us < 0)
        l->stats.last_us = clock_us ? clock_us() - f->rx_us : 0;
    if (l->stats.last_us > l->stats.worst_us)
        l->stats.worst_us = l->stats.last_us;
    if (l->stats.last_us > ESL_LINK_SLOW_US)
        l->stats.slow++;
    if (sent)
        *sent = ok;
    esl_radio_after(st, ls, ev, now_ms); /* an image to draw, now that the reply is out */
    return len;
}

void esl_link_sending(esl_link_t *l, const esl_link_rx_t *req, int64_t now_us)
{
    if (l->stats.last_us < 0)
        l->stats.last_us = now_us - req->rx_us;
}

void esl_link_lost(esl_link_t *l)
{
    if (l->stats.replies)
        l->stats.replies--;
    l->stats.tx_failed++;
}
