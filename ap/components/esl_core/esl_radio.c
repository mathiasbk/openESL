#include "esl_radio.h"
#include "esl_crc32.h"
#include "esl_render.h"
#include <string.h>

static esl_tagfw_t *tagfw;
static esl_image_t *images;

void esl_radio_set_tagfw(esl_tagfw_t *fw)
{
    tagfw = fw;
}

void esl_radio_set_image(esl_image_t *im)
{
    images = im;
}

/* How far a tag has the image, from the FW block it gets (for the dashboard). offer:
   the block at 0 that comes with every POLL while there is an image for the tag; a tag
   part way through ignores it and asks for the next block, so it only counts for a
   download that has not started. */
static void note_fw_sent(esl_store_t *st, uint64_t mac, const uint8_t *reply, size_t n, bool offer)
{
    if (n < ESL_FW_HDR_LEN || reply[ESL_OFF_TYPE] != ESL_MSG_FW)
        return;
    uint16_t ver = (uint16_t)(reply[ESL_FW_OFF_VER] | reply[ESL_FW_OFF_VER + 1] << 8);
    uint16_t pos = (uint16_t)(reply[ESL_FW_OFF_POS] | reply[ESL_FW_OFF_POS + 1] << 8);
    esl_store_fw_sent(st, mac, ver, (uint16_t)(pos + (n - ESL_FW_HDR_LEN)), offer);
}

size_t esl_radio_handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                        int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev)
{
    esl_rx_frame_t f;
    esl_poll_t poll;
    esl_layout_req_t req;
    esl_fw_req_t freq;
    esl_img_req_t ireq;
    uint8_t reply[ESL_MAX_PAYLOAD];
    size_t n;

    if (!esl_frame_parse_from_tag(psdu, psdu_len, &f) || f.dst_pan != pan || f.dst_short != ESL_AP_SHORT_ADDR)
        return 0;
    memset(&poll, 0, sizeof(poll));
    memset(&req, 0, sizeof(req));

    if (esl_layout_req_parse(f.payload, f.payload_len, &req)) {
        esl_store_offer_taken(st, f.src_mac, false, req.layout_ver);
        n = esl_layouts_block(ls, req.model, req.layout_ver, req.pos, req.max, esl_store_tag_poll_s(st, f.src_mac),
                              reply);
    } else if (esl_fw_req_parse(f.payload, f.payload_len, &freq)) {
        uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
        esl_store_offer_taken(st, f.src_mac, true, freq.ver);
        n = 0;
        if (esl_tagfw_for(tagfw, freq.model, f.src_mac))
            n = esl_tagfw_block(tagfw, freq.model, freq.ver, freq.crc, freq.pos, freq.max, next, reply);
        if (n == 0)
            n = esl_idle_build(reply, next); /* the image is gone, or not for it: the tag stops */
        else
            note_fw_sent(st, f.src_mac, reply, n, false);
    } else if (esl_img_req_parse(f.payload, f.payload_len, &ireq)) {
        uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
        esl_store_image_taken(st, f.src_mac, ireq.id);
        if (images)
            n = esl_image_block(images, st, ls, f.src_mac, ireq.id, ireq.pos, ireq.max, next, now_ms, reply);
        else
            n = esl_idle_build(reply, next);
    } else if (esl_poll_parse(f.payload, f.payload_len, &poll)) {
        /* The layout names and version are copied out first: the store and the
         * layout files may share one lock, and it is not taken twice. */
        esl_layout_env_t env;
        esl_layouts_env(ls, poll.model, &env);
        n = esl_store_on_poll(st, f.src_mac, &poll, rssi, now_ms, &env, reply);
        if (n == 0) {
            n = esl_layouts_block(ls, poll.model, 0, 0, ESL_LAYOUT_CHUNK_MAX, esl_store_tag_poll_s(st, f.src_mac),
                                  reply);
            /* A file that fits in this one block: the tag needs no LAYOUT_REQ to take it */
            esl_reply_t lr;
            if (esl_reply_parse(reply, n, &lr) && lr.type == ESL_MSG_LAYOUT && lr.layout_ver &&
                lr.layout_pos + lr.layout_len >= lr.layout_size)
                esl_store_offer_taken(st, f.src_mac, false, lr.layout_ver);
        }
        /* A tag that shows what the AP draws: its content's image, when it shows
         * another one (id 0 still notes a refusal, ESL_POLL_F_DRAWFAIL) */
        if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && (poll.flags & ESL_POLL_F_IMAGE) && images) {
            bool busy;
            uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
            uint32_t id = esl_image_prepare(images, st, ls, f.src_mac, poll.content_id, now_ms, &busy);
            if (esl_store_image_offer(st, f.src_mac, &poll, id))
                n = esl_image_block(images, st, ls, f.src_mac, id, 0, ESL_IMG_CHUNK_MAX, next, now_ms, reply);
            else if (busy)
                n = esl_idle_build(reply, ESL_IMAGE_BUSY_S); /* every buffer is taken: back in a moment */
        }
        /* Nothing else for the tag: new firmware, if there is some for it (ver 0
         * still notes a rejection, ESL_POLL_F_FWFAIL) */
        esl_tagfw_img_t img;
        if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE) {
            bool has = esl_tagfw_get(tagfw, poll.model, &img) && esl_tagfw_for(tagfw, poll.model, f.src_mac);
            if (esl_store_fw_offer(st, f.src_mac, &poll, has ? img.ver : 0)) {
                size_t m = esl_tagfw_block(tagfw, poll.model, 0, 0, 0, ESL_FW_CHUNK_MAX,
                                           esl_store_tag_poll_s(st, f.src_mac), reply);
                if (m) {
                    n = m;
                    note_fw_sent(st, f.src_mac, reply, n, true);
                }
            }
        }
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
    t->interval_s = ESL_FAKETAG_POLL_S;
}

size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_POLL_LEN_LAYOUT];
    uint8_t flags = t->flags;
    if (t->layouts && t->layout_rejected)
        flags |= ESL_POLL_F_LAYOUTFAIL;
    if (t->ota)
        flags |= ESL_POLL_F_OTA;
    if (t->ota && t->fw_failed)
        flags |= ESL_POLL_F_FWFAIL;
    if (t->images)
        flags |= ESL_POLL_F_IMAGE;
    if (t->images && t->img_rle)
        flags |= ESL_POLL_F_IMG_RLE;
    if (t->images && t->img_failed)
        flags |= ESL_POLL_F_DRAWFAIL;
    esl_poll_t p = {t->fw, t->battery_mv, t->shown, flags, t->model,
                    !t->legacy, t->cfg_ver, t->layouts && !t->legacy, t->layout_ver};
    size_t n = esl_poll_build(payload, &p);
    t->seq++;
    t->poll_again = false;
    return esl_frame_build_from_tag(psdu, t->seq, pan, ESL_AP_SHORT_ADDR, t->mac, payload, n);
}

size_t esl_faketag_fw_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_FREQ_LEN];
    if (!t->fw_dl_ver || t->fw_dl_got >= t->fw_dl_size)
        return 0;
    esl_fw_req_t r = {t->model, ESL_FW_CHUNK_MAX, t->fw_dl_ver, t->fw_dl_got, t->fw_dl_crc};
    size_t n = esl_fw_req_build(payload, &r);
    t->seq++;
    return esl_frame_build_from_tag(psdu, t->seq, pan, ESL_AP_SHORT_ADDR, t->mac, payload, n);
}

/* A FW block: the same checks as ota_block() on the tag (tag/src/ota.c). Only a running
   CRC is kept; a whole image "installs" by taking its version, as after the restart. */
static void on_fw(esl_faketag_t *t, const esl_reply_t *r)
{
    t->fw_blocks++;
    if (!t->ota || r->fw_model != t->model || r->fw_ver == t->fw || r->fw_ver == t->fw_failed)
        return;
    if (r->fw_ver != t->fw_dl_ver || r->layout_size != t->fw_dl_size || r->fw_crc != t->fw_dl_crc) {
        if (r->layout_size == 0 || r->layout_size > ESL_FW_MAX || (r->layout_size & 0x3FF)) {
            t->fw_failed = r->fw_ver;
            return;
        }
        t->fw_dl_ver = r->fw_ver;
        t->fw_dl_size = r->layout_size;
        t->fw_dl_crc = r->fw_crc;
        t->fw_dl_got = 0;
        t->fw_dl_sum = 0;
        t->fw_failed = 0;
    }
    if (r->layout_pos <= t->fw_dl_got && r->layout_pos + r->layout_len > t->fw_dl_got) {
        size_t skip = t->fw_dl_got - r->layout_pos;
        t->fw_dl_sum = esl_crc32_update(t->fw_dl_sum, r->layout_data + skip, r->layout_len - skip);
        t->fw_dl_got = (uint16_t)(r->layout_pos + r->layout_len);
    }
    if (t->fw_dl_got >= t->fw_dl_size) {
        if (t->fw_dl_sum == t->fw_dl_crc && !t->fw_bad) {
            t->fw = t->fw_dl_ver;           /* installed and restarted */
            t->flags = ESL_POLL_F_BOOTED;
            t->fw_installs++;
        } else {
            t->fw_failed = t->fw_dl_ver;    /* bad CRC, or (fw_bad) rolled back */
        }
        t->fw_dl_ver = 0;
    }
}

size_t esl_faketag_img_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_IREQ_LEN];
    if (!t->img_dl_id || t->img_dl_got >= t->img_dl_size)
        return 0;
    esl_img_req_t r = {t->model, ESL_IMG_CHUNK_MAX, t->img_dl_id, t->img_dl_got};
    size_t n = esl_img_req_build(payload, &r);
    t->seq++;
    return esl_frame_build_from_tag(psdu, t->seq, pan, ESL_AP_SHORT_ADDR, t->mac, payload, n);
}

/* An IMG block: the same checks as image_block() on the tag (tag/src/image.c). Only a
   running CRC is kept; a whole image whose CRC is its id is "shown". */
static void on_img(esl_faketag_t *t, const esl_reply_t *r)
{
    t->img_blocks++;
    t->img_size_sent = (uint8_t)r->layout_len;
    if (!t->images || r->img_id == 0 || r->img_id == t->img_failed)
        return;
    if (r->img_id == t->shown)
        return;                         /* shows it already (drawn from its slot) */
    if (r->img_id != t->img_dl_id || r->layout_size != t->img_dl_size) {
        bool raw = r->img_fmt == ESL_IMG_FMT_RAW && r->layout_size == esl_render_size(t->model);
        bool rle = t->img_rle && r->img_fmt == ESL_IMG_FMT_RLE && r->layout_size < esl_render_size(t->model);
        if (r->fw_model != t->model || !(raw || rle)) {
            t->img_failed = r->img_id;
            t->img_dl_id = 0;
            return;
        }
        t->img_dl_id = r->img_id;
        t->img_dl_size = r->layout_size;
        t->img_dl_got = 0;
        t->img_dl_sum = 0;
    }
    if (r->layout_pos <= t->img_dl_got && r->layout_pos + r->layout_len > t->img_dl_got) {
        size_t skip = t->img_dl_got - r->layout_pos;
        t->img_dl_sum = esl_crc32_update(t->img_dl_sum, r->layout_data + skip, r->layout_len - skip);
        t->img_dl_got = (uint16_t)(r->layout_pos + r->layout_len);
    }
    if (t->img_dl_got >= t->img_dl_size) {
        uint32_t sum = t->img_dl_sum ? t->img_dl_sum : 1;
        if (sum == t->img_dl_id && !t->img_bad) {
            t->shown = t->img_dl_id;
            t->img_failed = 0;
            t->img_shown++;
        } else {
            t->img_failed = t->img_dl_id;   /* bad CRC, or (img_bad) the panel failed */
        }
        t->img_dl_id = 0;
    }
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
        b[2] > ESL_LAYOUT_FILE_MAX || b[ESL_LBIN_OFF_NTEXT] > ESL_LBIN_TEXT_MAX)
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
    for (unsigned k = 0; k < b[ESL_LBIN_OFF_NTEXT]; k++) {
        if (pos >= len)
            return false;
        size_t n = b[pos++];
        pos += n;
        if (n == 0 || n > ESL_LBIN_TEXT_LEN_MAX || pos > len)
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
        (r.type == ESL_MSG_LAYOUT && (!t->layouts || t->legacy)) || (t->legacy && r.type == ESL_MSG_CONFIG)) {
        t->rejected++;
        return 0;
    }
    t->flags = 0;
    if (r.type == ESL_MSG_FW) {
        on_fw(t, &r);
    } else if (r.type == ESL_MSG_IMG) {
        on_img(t, &r);
    } else if (r.type == ESL_MSG_CONFIG) {
        /* new base settings first, then next_poll_s as for any reply */
        t->cfg = r.cfg;
        t->cfg_ver = r.cfg_ver;
        t->interval_s = r.cfg.poll_s ? r.cfg.poll_s : ESL_FAKETAG_POLL_S;
        t->configs++;
    } else if (r.type == ESL_MSG_CONTENT) {
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
    if (r.next_poll_s) {
        t->next_poll_s = r.next_poll_s;
        t->interval_s = r.next_poll_s;
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
    if (type == ESL_MSG_FW) {
        for (int req = 0; req < ESL_FAKETAG_FW_REQS && (n = esl_faketag_fw_request(t, pan, rx)) != 0; req++) {
            m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, NULL);
            if (!m || esl_faketag_receive(t, tx, m) != ESL_MSG_FW)
                break;
        }
        return type;
    }
    if (type == ESL_MSG_IMG) {
        for (int req = 0; req < ESL_FAKETAG_IMG_REQS && (n = esl_faketag_img_request(t, pan, rx)) != 0; req++) {
            m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, NULL);
            if (!m || esl_faketag_receive(t, tx, m) != ESL_MSG_IMG)
                break;
        }
        return type;
    }
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
