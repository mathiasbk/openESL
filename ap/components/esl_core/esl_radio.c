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

esl_tagfw_t *esl_radio_tagfw(void)
{
    return tagfw;
}

esl_image_t *esl_radio_image(void)
{
    return images;
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

/* The id of the image the store knows for the tag's content and layout file, 0 when
   it has not been drawn (esl_radio_replay: nothing is drawn) */
static uint32_t known_image(esl_store_t *st, esl_layouts_t *ls, uint64_t mac)
{
    esl_tag_t t;
    uint32_t ver = 0;
    if (!esl_store_get(st, mac, &t) || !t.has_content || t.content_err || !t.image_id)
        return 0;
    if (!esl_layouts_info(ls, t.model, &ver, NULL, NULL))
        ver = 0;
    return t.image_layout_ver == ver ? t.image_id : 0;
}

/* esl_radio_handle(), or with replay esl_radio_replay(): the store learns the same,
   but no image is drawn or sent (the reply goes nowhere). */
static size_t handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                     int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev, bool replay)
{
    esl_rx_frame_t f;
    esl_poll_t poll;
    esl_fw_req_t freq;
    esl_img_req_t ireq;
    uint8_t reply[ESL_MAX_PAYLOAD];
    size_t n;

    if (!esl_frame_parse_from_tag(psdu, psdu_len, &f) || f.dst_pan != pan || f.dst_short != ESL_AP_SHORT_ADDR)
        return 0;
    memset(&poll, 0, sizeof(poll));

    if (esl_fw_req_parse(f.payload, f.payload_len, &freq)) {
        uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
        esl_store_fw_taken(st, f.src_mac, freq.ver);
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
        if (images && !replay)
            n = esl_image_block(images, st, ls, f.src_mac, ireq.id, ireq.pos, ireq.max, next, now_ms, reply);
        else
            n = esl_idle_build(reply, next);
    } else if (esl_poll_parse(f.payload, f.payload_len, &poll)) {
        /* The layout names and version are copied out first: the store and the
         * layout files may share one lock, and it is not taken twice. */
        esl_layout_env_t env;
        esl_layouts_env(ls, poll.model, &env);
        n = esl_store_on_poll(st, f.src_mac, &poll, rssi, now_ms, &env, reply);
        /* Content not checked against this model's file yet (from the schedule, or set
           before the model was known) is, before its image is drawn */
        esl_store_check_tag(st, ls, f.src_mac);
        /* A tag that shows what the AP draws: its content's image, when it shows
         * another one (id 0 still notes a refusal, ESL_POLL_F_DRAWFAIL) */
        bool offered = false;
        if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && (poll.flags & ESL_POLL_F_IMAGE) && images && replay) {
            offered = esl_store_image_offer(st, f.src_mac, &poll, known_image(st, ls, f.src_mac));
        } else if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && (poll.flags & ESL_POLL_F_IMAGE) && images) {
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
        if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && !offered) {
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
        ev->reply_type = reply[ESL_OFF_TYPE];
    }
    /* The tag's sequence number is echoed so it can match the reply. */
    return esl_frame_build_to_tag(tx, f.seq, pan, ESL_AP_SHORT_ADDR, f.src_mac, reply, n);
}

size_t esl_radio_handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                        int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev)
{
    return handle(st, ls, pan, psdu, psdu_len, rssi, now_ms, tx, ev, false);
}

void esl_radio_replay(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                      int8_t rssi, uint64_t now_ms)
{
    uint8_t tx[ESL_PSDU_MAX];
    handle(st, ls, pan, psdu, psdu_len, rssi, now_ms, tx, NULL, true);
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
    t->images = true;
    t->img_rle = true;
    t->img_2d = true;
    t->interval_s = ESL_FAKETAG_POLL_S;
}

size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu)
{
    uint8_t payload[ESL_POLL_LEN_CFG];
    uint8_t flags = t->flags;
    if (t->ota)
        flags |= ESL_POLL_F_OTA;
    if (t->ota && t->fw_failed)
        flags |= ESL_POLL_F_FWFAIL;
    if (t->images)
        flags |= ESL_POLL_F_IMAGE;
    if (t->images && t->img_rle)
        flags |= ESL_POLL_F_IMG_RLE;
    if (t->images && t->img_2d)
        flags |= ESL_POLL_F_IMG_2D;
    if (t->images && t->img_failed)
        flags |= ESL_POLL_F_DRAWFAIL;
    esl_poll_t p = {t->fw, t->battery_mv, t->shown, flags, t->model, !t->legacy, t->cfg_ver};
    size_t n = esl_poll_build(payload, &p);
    t->seq++;
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
    if (r->fw_ver != t->fw_dl_ver || r->size != t->fw_dl_size || r->fw_crc != t->fw_dl_crc) {
        if (r->size == 0 || r->size > ESL_FW_MAX || (r->size & 0x3FF)) {
            t->fw_failed = r->fw_ver;
            return;
        }
        t->fw_dl_ver = r->fw_ver;
        t->fw_dl_size = r->size;
        t->fw_dl_crc = r->fw_crc;
        t->fw_dl_got = 0;
        t->fw_dl_sum = 0;
        t->fw_failed = 0;
    }
    if (r->pos <= t->fw_dl_got && r->pos + r->len > t->fw_dl_got) {
        size_t skip = t->fw_dl_got - r->pos;
        t->fw_dl_sum = esl_crc32_update(t->fw_dl_sum, r->data + skip, r->len - skip);
        t->fw_dl_got = (uint16_t)(r->pos + r->len);
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
    t->img_size_sent = (uint8_t)r->len;
    if (!t->images || r->img_id == 0 || r->img_id == t->img_failed)
        return;
    if (r->img_id == t->shown)
        return;                         /* shows it already (drawn from its slot) */
    if (r->img_id != t->img_dl_id || r->size != t->img_dl_size) {
        bool raw = r->img_fmt == ESL_IMG_FMT_RAW && r->size == esl_render_size(t->model);
        bool rle = t->img_rle && r->img_fmt == ESL_IMG_FMT_RLE && r->size < esl_render_size(t->model);
        bool c2d = t->img_2d && r->img_fmt == ESL_IMG_FMT_2D && r->size < esl_render_size(t->model);
        if (r->fw_model != t->model || !(raw || rle || c2d)) {
            t->img_failed = r->img_id;
            t->img_dl_id = 0;
            return;
        }
        t->img_dl_id = r->img_id;
        t->img_dl_size = r->size;
        t->img_dl_got = 0;
        t->img_dl_sum = 0;
    }
    if (r->pos <= t->img_dl_got && r->pos + r->len > t->img_dl_got) {
        size_t skip = t->img_dl_got - r->pos;
        t->img_dl_sum = esl_crc32_update(t->img_dl_sum, r->data + skip, r->len - skip);
        t->img_dl_got = (uint16_t)(r->pos + r->len);
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

uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len)
{
    esl_ap_frame_t f;
    esl_reply_t r;
    if (!esl_frame_parse_to_tag(psdu, psdu_len, &f) || f.dst_mac != t->mac || f.seq != t->seq ||
        f.src_short != ESL_AP_SHORT_ADDR || !esl_reply_parse(f.payload, f.payload_len, &r) ||
        (t->legacy && r.type == ESL_MSG_CONFIG)) {
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
    }
    return type;
}
