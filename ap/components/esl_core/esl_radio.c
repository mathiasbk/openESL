#include "esl_radio.h"
#include <string.h>

size_t esl_radio_handle(esl_store_t *st, uint16_t pan, const uint8_t *psdu, size_t psdu_len, int8_t rssi,
                        uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev)
{
    esl_rx_frame_t f;
    esl_poll_t poll;
    uint8_t reply[ESL_MAX_PAYLOAD];

    if (!esl_frame_parse_from_tag(psdu, psdu_len, &f) || f.dst_pan != pan || f.dst_short != ESL_AP_SHORT_ADDR)
        return 0;
    if (!esl_poll_parse(f.payload, f.payload_len, &poll))
        return 0;

    size_t n = esl_store_on_poll(st, f.src_mac, &poll, rssi, now_ms, reply);
    if (ev) {
        ev->mac = f.src_mac;
        ev->poll = poll;
        ev->reply_type = reply[ESL_OFF_TYPE];
    }
    /* The tag's sequence number is echoed so it can match the reply. */
    return esl_frame_build_to_tag(tx, f.seq, pan, ESL_AP_SHORT_ADDR, f.src_mac, reply, n);
}

/* ---- fake tag ---------------------------------------------------------------- */

void esl_faketag_init(esl_faketag_t *t, uint64_t mac, uint16_t fw, uint16_t battery_mv, uint8_t model)
{
    memset(t, 0, sizeof(*t));
    t->mac = mac;
    t->fw = fw;
    t->battery_mv = battery_mv;
    t->model = model;
    t->flags = ESL_POLL_F_BOOTED;
}

size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_POLL_LEN];
    esl_poll_t p = {t->fw, t->battery_mv, t->shown, t->flags, t->model};
    esl_poll_build(payload, &p);
    t->seq++;
    return esl_frame_build_from_tag(psdu, t->seq, pan, ESL_AP_SHORT_ADDR, t->mac, payload, sizeof(payload));
}

uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len)
{
    esl_ap_frame_t f;
    esl_reply_t r;
    if (!esl_frame_parse_to_tag(psdu, psdu_len, &f) || f.dst_mac != t->mac || f.seq != t->seq ||
        f.src_short != ESL_AP_SHORT_ADDR || !esl_reply_parse(f.payload, f.payload_len, &r)) {
        t->rejected++;
        return 0;
    }
    t->flags = 0;
    if (r.next_poll_s)
        t->next_poll_s = r.next_poll_s;
    if (r.type == ESL_MSG_CONTENT) {
        t->content = r.content;
        t->shown = r.content_id;
        t->contents++;
    } else {
        t->idles++;
    }
    return r.type;
}
