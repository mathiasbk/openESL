#include "esl_radio.h"
#include "esl_crc32.h"
#include "esl_render.h"
#include "cmac.h"
#include <string.h>

static esl_tagfw_t *tagfw;
static esl_radio_sec_t *sec;
static esl_image_t *images;
static bool draw_later;

void esl_radio_set_tagfw(esl_tagfw_t *fw)
{
    tagfw = fw;
}

void esl_radio_set_image(esl_image_t *im)
{
    images = im;
}

void esl_radio_set_sec(esl_radio_sec_t *s)
{
    sec = s;
}

esl_radio_sec_t *esl_radio_sec(void)
{
    return sec;
}

bool esl_radio_sec_has_key(const esl_radio_sec_t *s)
{
    uint8_t any = 0;
    if (!s)
        return false;
    for (int i = 0; i < ESL_SEC_KEY_LEN; i++)
        any |= s->key[i];
    return any != 0;
}

void esl_radio_tag_key(const esl_radio_sec_t *s, uint64_t mac, uint8_t key[ESL_SEC_KEY_LEN])
{
    uint8_t m[9];
    cmac_t c;
    m[0] = ESL_SEC_KDF_LABEL;
    for (int i = 0; i < 8; i++)
        m[1 + i] = (uint8_t)(mac >> (8 * i));
    cmac_start(&c, s->key);
    cmac_add(&c, m, sizeof(m));
    cmac_finish(&c, key);
}

/* A message from the tag with ESL_PROTO_VER_SEC (p, len bytes): true when its MIC is right
   for the tag's key (into key) and its counter (into ctr) new; the message without the
   trailer, as ESL_PROTO_VER, is then in msg (*mlen bytes). */
static bool sec_check(esl_store_t *st, uint64_t mac, const uint8_t *p, size_t len, uint8_t *key, uint32_t *ctr,
                      uint8_t *msg, size_t *mlen)
{
    uint8_t mic[ESL_SEC_MIC_LEN];
    if (!esl_radio_sec_has_key(sec) || len < 2 + ESL_SEC_TRAILER_TAG || len > ESL_MAX_PAYLOAD)
        return false;
    size_t n = len - ESL_SEC_TRAILER_TAG;
    *ctr = (uint32_t)p[n] | (uint32_t)p[n + 1] << 8 | (uint32_t)p[n + 2] << 16 | (uint32_t)p[n + 3] << 24;
    esl_radio_tag_key(sec, mac, key);
    sec_mic(key, ESL_SEC_DIR_TAG, *ctr, p, (uint8_t)n, mic);
    uint8_t diff = 0;
    for (int i = 0; i < ESL_SEC_MIC_LEN; i++)
        diff |= (uint8_t)(mic[i] ^ p[n + ESL_SEC_CTR_LEN + i]);
    if (diff) {
        esl_store_sec_bad(st, mac);
        return false;
    }
    if (!esl_store_sec_ok(st, mac, *ctr))
        return false;
    memcpy(msg, p, n);
    msg[ESL_OFF_VER] = ESL_PROTO_VER;
    *mlen = n;
    return true;
}

/* PAIR with the tag's key, or zeros (forget), into reply */
static size_t pair_build(uint64_t mac, bool forget, uint16_t next_poll_s, uint8_t *reply)
{
    reply[ESL_OFF_TYPE] = ESL_MSG_PAIR;
    reply[ESL_OFF_VER] = ESL_PROTO_VER;
    reply[ESL_OFF_NEXT_POLL] = (uint8_t)next_poll_s;
    reply[ESL_OFF_NEXT_POLL + 1] = (uint8_t)(next_poll_s >> 8);
    if (forget)
        memset(reply + ESL_PAIR_OFF_KEY, 0, ESL_SEC_KEY_LEN);
    else
        esl_radio_tag_key(sec, mac, reply + ESL_PAIR_OFF_KEY);
    return ESL_PAIR_LEN;
}

void esl_radio_set_draw_later(bool on)
{
    draw_later = on;
}

void esl_radio_after(esl_store_t *st, esl_layouts_t *ls, const esl_radio_event_t *ev, uint64_t now_ms)
{
    if (!draw_later || !ev || (ev->msg_type != ESL_MSG_POLL && ev->msg_type != ESL_MSG_IMG_REQ))
        return;
    if (ev->msg_type == ESL_MSG_POLL)
        esl_store_check_tag(st, ls, ev->mac);
    if (images)
        esl_image_draw_later(images, st, ls, now_ms);
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
    if (!esl_store_get(st, mac, &t) || !t.has_content || t.content_err || !t.image_id)
        return 0;
    return t.image_layout_ver == esl_layouts_image_ver(ls, t.model, t.layout_hash) ? t.image_id : 0;
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

    /* Security: a message with a trailer is checked and read without it; with require, a
       plain one is answered only to pair the tag */
    uint8_t key[ESL_SEC_KEY_LEN], msg[ESL_MAX_PAYLOAD];
    uint32_t ctr = 0;
    bool secure = f.payload_len > ESL_OFF_VER && f.payload[ESL_OFF_VER] == ESL_PROTO_VER_SEC;
    if (secure) {
        size_t mlen;
        if (!sec_check(st, f.src_mac, f.payload, f.payload_len, key, &ctr, msg, &mlen))
            return 0;
        f.payload = msg;
        f.payload_len = mlen;
    } else if (sec && sec->require &&
               (f.payload_len < ESL_IDLE_LEN || f.payload[ESL_OFF_TYPE] != ESL_MSG_POLL ||
                esl_store_pair_op(st, f.src_mac, false) != ESL_PAIR_OP_PAIR)) {
        /* a tag that was told to forget its key did; any other plain frame changes nothing,
           so nobody can make a paired tag look unpaired */
        if (esl_store_pair_op(st, f.src_mac, false) == ESL_PAIR_OP_FORGET)
            esl_store_sec_heard(st, f.src_mac, false);
        return 0;
    }

    /* signed blocks are shorter, so the frame stays within ESL_SEC_MAX_REPLY */
    const uint8_t cut = secure ? ESL_SEC_CHUNK_CUT : 0;
    if (esl_fw_req_parse(f.payload, f.payload_len, &freq)) {
        if (freq.max > ESL_FW_CHUNK_MAX - cut)
            freq.max = ESL_FW_CHUNK_MAX - cut;
        uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
        esl_store_fw_taken(st, f.src_mac, freq.ver);
        n = 0;
        if (esl_tagfw_for(tagfw, freq.model, f.src_mac))
            n = esl_tagfw_block(tagfw, freq.model, freq.ver, freq.crc, freq.pos, freq.max, next, reply);
        if (n == 0)
            n = esl_idle_build(reply, next); /* the image is gone, or not for it: the tag stops */
        else
            note_fw_sent(st, f.src_mac, reply, n, false);
        esl_blocks_poll_soon(reply, n); /* back soon for the rest when it stops part way */
    } else if (esl_img_req_parse(f.payload, f.payload_len, &ireq)) {
        if (ireq.max > ESL_IMG_CHUNK_MAX - cut)
            ireq.max = ESL_IMG_CHUNK_MAX - cut;
        uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
        esl_store_image_taken(st, f.src_mac, ireq.id);
        if (images && !replay && draw_later)
            n = esl_image_block_later(images, st, ls, f.src_mac, ireq.id, ireq.pos, ireq.max, next, now_ms, reply);
        else if (images && !replay)
            n = esl_image_block(images, st, ls, f.src_mac, ireq.id, ireq.pos, ireq.max, next, now_ms, reply);
        else
            n = esl_idle_build(reply, next);
        esl_blocks_poll_soon(reply, n);
    } else if (esl_poll_parse(f.payload, f.payload_len, &poll)) {
        /* The layout names and version are copied out first: the store and the
         * layout files may share one lock, and it is not taken twice. */
        esl_layout_env_t env;
        esl_layouts_env(ls, poll.model, &env);
        n = esl_store_on_poll(st, f.src_mac, &poll, rssi, now_ms, &env, reply);
        /* Pairing (the status is noted all the same): a plain PAIR with its key to a tag
           without one, an authenticated one of zeros to make it forget. It comes back soon,
           to show that it took it; images and firmware wait for that. */
        uint8_t op = replay || !esl_radio_sec_has_key(sec) ? ESL_PAIR_OP_NONE : esl_store_pair_op(st, f.src_mac, false);
        if ((op == ESL_PAIR_OP_PAIR && !secure) || (op == ESL_PAIR_OP_FORGET && secure)) {
            esl_store_pair_op(st, f.src_mac, true);
            n = pair_build(f.src_mac, op == ESL_PAIR_OP_FORGET, ESL_BLOCKS_POLL_S, reply);
        }
        /* Content not checked against this model's file yet (from the schedule, or set
           before the model was known) is, before its image is drawn */
        if (!draw_later || replay)
            esl_store_check_tag(st, ls, f.src_mac); /* else after the reply (esl_radio_after) */
        /* A tag that shows what the AP draws: its content's image, when it shows
         * another one (id 0 still notes a refusal, ESL_POLL_F_DRAWFAIL) */
        bool offered = false;
        if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && (poll.flags & ESL_POLL_F_IMAGE) && images && replay) {
            offered = esl_store_image_offer(st, f.src_mac, &poll, known_image(st, ls, f.src_mac));
        } else if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && (poll.flags & ESL_POLL_F_IMAGE) && images) {
            bool busy;
            uint16_t next = esl_store_tag_poll_s(st, f.src_mac);
            uint32_t id = draw_later ? esl_image_prepare_later(images, st, ls, f.src_mac, poll.content_id, now_ms, &busy)
                                     : esl_image_prepare(images, st, ls, f.src_mac, poll.content_id, now_ms, &busy);
            if (esl_store_image_offer(st, f.src_mac, &poll, id))
                n = esl_image_block(images, st, ls, f.src_mac, id, 0, ESL_IMG_CHUNK_MAX - cut, next, now_ms, reply);
            else if (busy)
                n = esl_idle_build(reply, ESL_IMAGE_BUSY_S); /* every buffer is taken: back in a moment */
        }
        /* Nothing else for the tag: new firmware, if there is some for it (ver 0
         * still notes a rejection, ESL_POLL_F_FWFAIL) */
        esl_tagfw_img_t img;
        if (reply[ESL_OFF_TYPE] == ESL_MSG_IDLE && !offered) {
            bool has = esl_tagfw_get(tagfw, poll.model, &img) && esl_tagfw_for(tagfw, poll.model, f.src_mac);
            if (esl_store_fw_offer(st, f.src_mac, &poll, has ? img.ver : 0)) {
                size_t m = esl_tagfw_block(tagfw, poll.model, 0, 0, 0, ESL_FW_CHUNK_MAX - cut,
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

    esl_store_sec_heard(st, f.src_mac, secure);
    if (secure) {
        if (n + ESL_SEC_TRAILER_AP > ESL_SEC_MAX_REPLY)
            return 0;
        reply[ESL_OFF_VER] = ESL_PROTO_VER_SEC;
        sec_mic(key, ESL_SEC_DIR_AP, ctr, reply, (uint8_t)n, reply + n);
        n += ESL_SEC_TRAILER_AP;
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
    uint8_t payload[ESL_POLL_LEN_LINK];
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
    esl_poll_t p = {t->fw, t->battery_mv, t->shown, flags, t->model, !t->legacy, t->cfg_ver, !t->legacy, 0, 0, 0, 0};
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
    esl_radio_event_t ev;
    size_t n = esl_faketag_poll(t, pan, rx);
    size_t m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, &ev);
    uint8_t type = m ? esl_faketag_receive(t, tx, m) : 0;
    if (m)
        esl_radio_after(st, ls, &ev, now_ms); /* as esl_link_handle() once the reply is out */
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
            m = esl_radio_handle(st, ls, pan, rx, n, rssi, now_ms, tx, &ev);
            uint8_t got = m ? esl_faketag_receive(t, tx, m) : 0;
            if (m)
                esl_radio_after(st, ls, &ev, now_ms);
            if (got != ESL_MSG_IMG)
                break;
        }
    }
    return type;
}
