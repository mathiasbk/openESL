#include "esl_radio.h"
#include "esl_crc32.h"
#include <string.h>

size_t esl_radio_handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                        int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev)
{
    esl_rx_frame_t f;
    esl_poll_t poll;
    esl_layout_req_t req;
    uint8_t reply[ESL_MAX_PAYLOAD];
    size_t n;

    if (!esl_frame_parse_from_tag(psdu, psdu_len, &f) || f.dst_pan != pan || f.dst_short != ESL_AP_SHORT_ADDR)
        return 0;
    memset(&poll, 0, sizeof(poll));
    memset(&req, 0, sizeof(req));

    if (esl_layout_req_parse(f.payload, f.payload_len, &req)) {
        n = esl_layouts_block(ls, req.model, req.layout_ver, req.pos, req.max, esl_store_next_poll(st), reply);
    } else if (esl_poll_parse(f.payload, f.payload_len, &poll)) {
        /* The layout names and version are copied out first: the store and the
         * layout files may share one lock, and it is not taken twice. */
        esl_layout_env_t env;
        esl_layouts_env(ls, poll.model, &env);
        n = esl_store_on_poll(st, f.src_mac, &poll, rssi, now_ms, &env, reply);
        if (n == 0)
            n = esl_layouts_block(ls, poll.model, 0, 0, ESL_LAYOUT_CHUNK_MAX, esl_store_next_poll(st), reply);
    } else {
        return 0;
    }

    if (ev) {
        ev->mac = f.src_mac;
        ev->msg_type = f.payload[ESL_OFF_TYPE];
        ev->poll = poll;
        ev->req = req;
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
    t->layouts = true;
}

size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_POLL_LEN_LAYOUT];
    uint8_t flags = t->flags;
    if (t->layouts && t->layout_rejected)
        flags |= ESL_POLL_F_LAYOUTFAIL;
    esl_poll_t p = {t->fw, t->battery_mv, t->shown, flags, t->model, t->layouts, t->layout_ver};
    size_t n = esl_poll_build(payload, &p);
    t->seq++;
    t->poll_again = false;
    return esl_frame_build_from_tag(psdu, t->seq, pan, ESL_AP_SHORT_ADDR, t->mac, payload, n);
}

size_t esl_faketag_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_LREQ_LEN];
    if (!t->dl_ver || t->dl_got >= t->dl_size)
        return 0;
    esl_layout_req_t r = {t->model, ESL_LAYOUT_CHUNK_MAX, t->dl_ver, t->dl_got};
    size_t n = esl_layout_req_build(payload, &r);
    t->seq++;
    return esl_frame_build_from_tag(psdu, t->seq, pan, ESL_AP_SHORT_ADDR, t->mac, payload, n);
}

/* The same structure check as layout_file_install() on the tag. */
static bool file_ok(const esl_faketag_t *t, const uint8_t *b, uint16_t len)
{
    if (len < ESL_LBIN_HDR_LEN || b[0] != ESL_LBIN_FORMAT || b[1] != t->model || b[2] == 0 ||
        b[2] > ESL_LAYOUT_FILE_MAX)
        return false;
    size_t pos = ESL_LBIN_HDR_LEN;
    for (unsigned k = 0; k < b[2]; k++) {
        if (pos >= len)
            return false;
        size_t n = b[pos++];
        pos += n * ESL_LE_SIZE;
        if (pos > len)
            return false;
    }
    return pos == len;
}

const uint8_t *esl_faketag_layout(const esl_faketag_t *t, uint8_t n, uint8_t *nelem)
{
    if (!t->layout_ver || n >= t->layout[2])
        return NULL;
    size_t pos = ESL_LBIN_HDR_LEN;
    for (;;) {
        *nelem = t->layout[pos++];
        if (!n--)
            return &t->layout[pos];
        pos += (size_t)*nelem * ESL_LE_SIZE;
    }
}

static void layout_changed(esl_faketag_t *t)
{
    if (t->shown && t->content.layout >= ESL_LAYOUT_FILE)
        t->shown = 0;
    t->poll_again = true;
}

static void on_layout(esl_faketag_t *t, const esl_reply_t *r)
{
    uint32_t v = r->layout_ver;
    t->layout_blocks++;
    if (v == 0) {
        t->dl_ver = 0;
        t->layout_rejected = 0;
        if (t->layout_ver) {
            t->layout_ver = 0;
            layout_changed(t);
        }
        return;
    }
    if (v == t->layout_ver || v == t->layout_rejected)
        return;
    if (v != t->dl_ver || r->layout_size != t->dl_size) {
        if (r->layout_size > ESL_LBIN_MAX || r->layout_size < ESL_LBIN_HDR_LEN) {
            t->dl_ver = 0;
            t->layout_rejected = v;
            t->poll_again = true;
            return;
        }
        if (t->layout_ver) {
            t->layout_ver = 0;
            layout_changed(t);
        }
        t->dl_ver = v;
        t->dl_size = r->layout_size;
        t->dl_got = 0;
    }
    if (r->layout_pos <= t->dl_got && r->layout_pos + r->layout_len > t->dl_got) {
        size_t skip = t->dl_got - r->layout_pos;
        memcpy(&t->layout[t->dl_got], r->layout_data + skip, r->layout_len - skip);
        t->dl_got = (uint16_t)(r->layout_pos + r->layout_len);
    }
    if (t->dl_got >= t->dl_size) {
        uint32_t crc = esl_crc32(t->layout, t->dl_size);
        t->dl_ver = 0;
        if ((crc ? crc : 1) == v && file_ok(t, t->layout, t->dl_size)) {
            t->layout_ver = v;
            t->layout_size = t->dl_size;
            t->layout_rejected = 0;
            t->layout_installs++;
        } else {
            t->layout_rejected = v;
        }
        layout_changed(t);
    }
}

uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len)
{
    esl_ap_frame_t f;
    esl_reply_t r;
    if (!esl_frame_parse_to_tag(psdu, psdu_len, &f) || f.dst_mac != t->mac || f.seq != t->seq ||
        f.src_short != ESL_AP_SHORT_ADDR || !esl_reply_parse(f.payload, f.payload_len, &r) ||
        (r.type == ESL_MSG_LAYOUT && !t->layouts)) {
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
        t->drawn_elements = 0;
        if (r.content.layout >= ESL_LAYOUT_FILE) {
            uint8_t n;
            if (esl_faketag_layout(t, (uint8_t)(r.content.layout - ESL_LAYOUT_FILE), &n))
                t->drawn_elements = n;
            else
                t->missing_layouts++; /* the firmware falls back to the built-in text layout */
        }
    } else if (r.type == ESL_MSG_LAYOUT) {
        on_layout(t, &r);
    } else {
        t->idles++;
    }
    return r.type;
}

uint8_t esl_faketag_round(esl_faketag_t *t, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, int8_t rssi,
                          uint64_t now_ms)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_poll(t, pan, rx);
    size_t m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, NULL);
    uint8_t type = m ? esl_faketag_receive(t, tx, m) : 0;
    if (type != ESL_MSG_LAYOUT)
        return type;
    for (int req = 0; req < 16 && (n = esl_faketag_request(t, pan, rx)) != 0; req++) {
        m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, NULL);
        if (!m || esl_faketag_receive(t, tx, m) != ESL_MSG_LAYOUT)
            return type;
    }
    if (t->poll_again) {
        n = esl_faketag_poll(t, pan, rx);
        m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, NULL);
        uint8_t again = m ? esl_faketag_receive(t, tx, m) : 0;
        if (again)
            type = again;
    }
    return type;
}
