#include "app.h"
#include "config.h"
#include "draw.h"
#include "status.h"
#include "epd.h"
#include "ota.h"
#include "image.h"
#include "hal/hal.h"

#define RX_TRIES 3       /* other tags may be in the window; read up to 3 frames */
#define LINE_MAX 24

app_state_t __xdata app;

static uint8_t __xdata txbuf[ESL_MAC_MAX_FRAME];
static uint8_t __xdata rxbuf[ESL_MAC_MAX_FRAME];
static proto_reply_t __xdata reply;
static draw_scene_t __xdata scene;
static uint8_t __xdata lines[4][LINE_MAX];
static uint8_t __xdata line_len[4];
static uint8_t rx_off, rx_plen;      /* payload in rxbuf after a valid answer */

/* ---- Helpers ---- */

static uint16_t rng_next(void)
{
    uint16_t x = app.rng;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    app.rng = x;
    return x;
}

static uint8_t put_str(uint8_t *dst, uint8_t pos, const char *s)
{
    while (*s && pos < LINE_MAX)
        dst[pos++] = (uint8_t)*s++;
    return pos;
}

static uint8_t put_hex(uint8_t *dst, uint8_t pos, uint8_t v)
{
    static const char __code hex[] = "0123456789ABCDEF";
    if (pos + 2 > LINE_MAX)
        return pos;
    dst[pos++] = hex[v >> 4];
    dst[pos++] = hex[v & 15];
    return pos;
}

/* Start screen and "waiting"/"no AP" screens (requirement T9): name, MAC, firmware version, status. */
static bool draw_status(const char *status)
{
    const uint8_t *lp[4];
    uint8_t i, n;

    line_len[0] = put_str(lines[0], 0, "OpenESL");
    n = put_str(lines[1], 0, "MAC ");
    for (i = MAC_ADDR_LEN; i > 0; i--)       /* show the most significant byte first */
        n = put_hex(lines[1], n, app.mac[i - 1]);
    line_len[1] = n;
    line_len[2] = put_str(lines[2], 0, "FW " FW_VERSION_STR);
    line_len[3] = put_str(lines[3], 0, status);
    for (i = 0; i < 4; i++)
        lp[i] = lines[i];
    status_build(&scene, lp, line_len, 4);
    return epd_draw(&scene);
}

static const char __code msg_waiting[] = "Waiting for content";
static const char __code msg_lost[] = "No AP found";

/* ---- Settings from the AP (CONFIG) ---- */

static uint16_t max_poll_s(void)
{
    return app.cfg.max_poll_s ? app.cfg.max_poll_s : CFG_MAX_POLL_S;
}

static uint16_t base_poll_s(void)
{
    return app.cfg.poll_s ? app.cfg.poll_s : CFG_POLL_S;
}

static uint16_t long_after_min(void)
{
    return app.cfg.long_after_min ? app.cfg.long_after_min : CFG_LONG_AFTER_MIN;
}

static uint16_t long_poll_s(void)
{
    return app.cfg.long_poll_s ? app.cfg.long_poll_s : CFG_LONG_POLL_S;
}

/*
 * Flash (requirement T12): a restarted tag uses the AP's settings before it reaches the AP.
 * One record through hal_store_* (hal.h):
 *   0 STORE_FORMAT   1-2 cfg ver   3-4 poll_s   5-6 max_poll_s   7-8 lost_ap_s
 *   9-10 long_poll_s   11-12 long_after_min   13 cfg channel   14 the channel the tag last heard the AP on
 * The settings are all of app_cfg_t, ver included: the tag reports ver in POLL, so a
 * stale or missing copy only costs one extra CONFIG from the AP. The screen itself comes
 * back from the image slot (image.h).
 * Written only when something changes, and with the radio off.
 */
#define STORE_FORMAT     4        /* 3: without the channel, 1: without the long backoff; 2 was the firmware that drew content */
#define STORE_LEN        15

static uint8_t __xdata store_rec[STORE_LEN];

static void app_store_load(void)
{
    uint8_t n = hal_store_read(store_rec, sizeof(store_rec));

    if (n >= STORE_LEN && store_rec[0] == STORE_FORMAT) {
        app.cfg.ver = ESL_GET_U16(store_rec + 1);
        app.cfg.poll_s = ESL_GET_U16(store_rec + 3);
        app.cfg.max_poll_s = ESL_GET_U16(store_rec + 5);
        app.cfg.lost_ap_s = ESL_GET_U16(store_rec + 7);
        app.cfg.long_poll_s = ESL_GET_U16(store_rec + 9);
        app.cfg.long_after_min = ESL_GET_U16(store_rec + 11);
        app.cfg.channel = store_rec[13];
        app.channel = store_rec[14];
    } else {
        app.cfg.ver = 0;
        app.cfg.poll_s = 0;
        app.cfg.max_poll_s = 0;
        app.cfg.lost_ap_s = 0;
        app.cfg.long_poll_s = 0;
        app.cfg.long_after_min = 0;
        app.cfg.channel = 0;
        app.channel = 0;
    }
    if (app.cfg.channel < ESL_CHANNEL_MIN || app.cfg.channel > ESL_CHANNEL_MAX)
        app.cfg.channel = 0;
    if (app.channel < ESL_CHANNEL_MIN || app.channel > ESL_CHANNEL_MAX)
        app.channel = CFG_CHANNEL;
}

static void app_store_save(void)
{
    store_rec[0] = STORE_FORMAT;
    ESL_PUT_U16(store_rec + 1, app.cfg.ver);
    ESL_PUT_U16(store_rec + 3, app.cfg.poll_s);
    ESL_PUT_U16(store_rec + 5, app.cfg.max_poll_s);
    ESL_PUT_U16(store_rec + 7, app.cfg.lost_ap_s);
    ESL_PUT_U16(store_rec + 9, app.cfg.long_poll_s);
    ESL_PUT_U16(store_rec + 11, app.cfg.long_after_min);
    store_rec[13] = app.cfg.channel;
    store_rec[14] = app.channel;
    /* a failed write only costs a CONFIG after a restart */
    hal_store_write(store_rec, STORE_LEN);
}

static uint16_t at_least(uint16_t v, uint16_t min)
{
    return (v && v < min) ? min : v;
}

/* Called with the radio off (hal_store_write needs that). */
static void apply_config(void)
{
    uint16_t poll = at_least(reply.cfg_poll_s, ESL_CFG_POLL_MIN_S);
    uint16_t max = at_least(reply.cfg_max_poll_s, ESL_CFG_POLL_MIN_S);
    uint16_t lost = at_least(reply.cfg_lost_ap_s, ESL_CFG_LOST_AP_MIN_S);
    uint16_t lpoll = at_least(reply.cfg_long_poll_s, ESL_CFG_LONG_POLL_MIN_S);
    uint16_t lafter = at_least(reply.cfg_long_after_min, ESL_CFG_LONG_AFTER_MIN_MIN);
    bool changed = app.cfg.ver != reply.cfg_ver || app.cfg.poll_s != poll ||
                   app.cfg.max_poll_s != max || app.cfg.lost_ap_s != lost ||
                   app.cfg.long_poll_s != lpoll || app.cfg.long_after_min != lafter ||
                   app.cfg.channel != reply.cfg_channel;

    app.cfg.ver = reply.cfg_ver;
    app.cfg.poll_s = poll;
    app.cfg.max_poll_s = max;
    app.cfg.lost_ap_s = lost;
    app.cfg.long_poll_s = lpoll;
    app.cfg.long_after_min = lafter;
    app.cfg.channel = reply.cfg_channel;
    app.interval_s = base_poll_s();
    if (changed || app.channel_moved)
        app_store_save();
}

uint16_t app_backoff_s(uint16_t interval_s, uint8_t misses)
{
    uint16_t s = interval_s;
    uint16_t max = max_poll_s();
    uint8_t i;

    if (misses < CFG_BACKOFF_AFTER || s >= max)
        return s;
    for (i = CFG_BACKOFF_AFTER; i <= misses; i++) {
        if (s >= max / 2)
            return max;
        s <<= 1;
    }
    return s;
}

uint16_t app_long_poll_s(uint32_t lost_s)
{
    uint16_t after = long_after_min();

    if (after == ESL_CFG_LONG_NEVER || lost_s < (uint32_t)after * 60u)
        return 0;
    return long_poll_s();
}

/* Sends the frame in txbuf and waits for a valid answer to us. true = reply is filled in. */
static bool exchange(uint8_t flen)
{
    uint8_t t, off;
    int8_t n, plen;

    if (!radio_tx(txbuf, flen))
        return false;
    for (t = 0; t < RX_TRIES; t++) {
        n = radio_rx(rxbuf, sizeof(rxbuf), CFG_RX_TIMEOUT_MS);
        if (n < 0)
            return false;
        plen = proto_check_frame(rxbuf, (uint8_t)n, CFG_PAN_ID, app.mac, &off);
        if (plen < 0)
            continue;
        if (proto_parse_reply(rxbuf + off, (uint8_t)plen, &reply) == PROTO_OK) {
            rx_off = off;
            rx_plen = (uint8_t)plen;
            app.rx_rssi = radio_rx_rssi();     /* the signal of this frame, for the next POLL */
            app.rx_lqi = radio_rx_lqi();
            return true;
        }
    }
    return false;
}

/* exchange(), and once more after a random pause if nothing came (protokoll.md). The
   retry is the same frame, byte for byte; retried says it was needed and answered. */
static bool retried;

static bool exchange_retry(uint8_t flen)
{
    retried = false;
    if (exchange(flen))
        return true;
    hal_delay_ms(CFG_RETRY_MIN_MS + (uint8_t)(rng_next() % (CFG_RETRY_MAX_MS - CFG_RETRY_MIN_MS + 1)));
    retried = exchange(flen);
    return retried;
}

/* Builds POLL straight into the payload part of txbuf (proto_build_frame copies it onto itself). */
static uint8_t build_poll_frame(void)
{
    uint8_t *pl = txbuf + ESL_MAC_HDR_LEN;
    uint8_t flags = app.poll_flags | ota_poll_flags() | image_poll_flags();

    proto_build_poll(pl, FW_VERSION, battery_read_mv(), app.shown_id, flags, app.cfg.ver,
                     app.rx_rssi, app.rx_lqi, app.misses, app.link_flags);
    return proto_build_frame(txbuf, app.seq++, CFG_PAN_ID, app.mac, pl, ESL_POLL_LEN_LINK);
}

/*
 * reply is a FW block (docs/tag-ota.md): writes it to the SPI flash and asks for the
 * next ones in the same round, up to CFG_FW_MAX_REQ. A request that gets no answer is
 * sent again, up to CFG_FW_MAX_MISS times per round. True when the image is complete and
 * checked; the download otherwise goes on at the next FW the AP sends.
 */
static bool fw_transfer(void)
{
    uint8_t req = 0, miss = 0, r, n;

    ota_open();
    r = ota_block(&reply, rxbuf + rx_off + reply.data_off);
    while (r == OTA_MORE && ++req <= CFG_FW_MAX_REQ) {
        if ((req & 15) == 0)
            hal_wdt_kick();
        n = proto_build_fw_req(txbuf + ESL_MAC_HDR_LEN, ota.dl_ver, ota.dl_got, ota.dl_crc, ESL_FW_CHUNK_MAX);
        if (!exchange_retry(proto_build_frame(txbuf, app.seq++, CFG_PAN_ID, app.mac,
                                              txbuf + ESL_MAC_HDR_LEN, n))) {
            if (++miss > CFG_FW_MAX_MISS)
                break;
            continue;
        }
        if (reply.type != ESL_MSG_FW)
            break;                     /* the AP no longer has it */
        r = ota_block(&reply, rxbuf + rx_off + reply.data_off);
    }
    ota_close();
    return r == OTA_DONE;
}

/*
 * reply is an IMG block (docs/protokoll.md part 1, "Image transfer"): writes it to the
 * SPI flash and asks for the next ones in the same round, up to CFG_IMG_MAX_REQ, with
 * lost answers handled as for FW. Returns IMAGE_DONE when the image is whole (or was
 * in the slot already), IMAGE_STOP when the tag refused it; an unfinished download
 * goes on at the next IMG the AP sends.
 */
static uint8_t image_transfer(void)
{
    uint8_t req = 0, miss = 0, r, n;

    image_open();
    r = image_block(&reply, rxbuf + rx_off + reply.data_off);
    while (r == IMAGE_MORE && ++req <= CFG_IMG_MAX_REQ) {
        if ((req & 15) == 0)
            hal_wdt_kick();
        n = proto_build_img_req(txbuf + ESL_MAC_HDR_LEN, image.dl_id, image.dl_got, ESL_IMG_CHUNK_MAX);
        if (!exchange_retry(proto_build_frame(txbuf, app.seq++, CFG_PAN_ID, app.mac,
                                              txbuf + ESL_MAC_HDR_LEN, n))) {
            if (++miss > CFG_FW_MAX_MISS)
                break;
            continue;
        }
        if (reply.type != ESL_MSG_IMG)
            break;                     /* the AP no longer has it */
        r = image_block(&reply, rxbuf + rx_off + reply.data_off);
    }
    image_close();
    return r;
}

/* Draws the image in the slot (radio off). true = it is on the screen, and the tag
   reports its id. */
static bool show_image(void)
{
    if (!image_show()) {
        app.poll_flags |= ESL_POLL_F_DRAWFAIL;
        return false;
    }
    app.shown_id = image.slot_id;
    app.poll_flags &= (uint8_t)~ESL_POLL_F_DRAWFAIL;
    return true;
}

/*
 * The AP has been gone for lost_ap_s (#98): the picture on the screen stays, with the
 * "no AP" mark in its corner (status.h), drawn again from the slot. True when it is up;
 * false when there is no picture, the slot no longer holds it (a new one was half
 * downloaded when the AP went) or the panel did not answer: then the "No AP found"
 * screen is drawn instead. One refresh now and one when the AP is back, none per miss.
 */
static bool show_marked(void)
{
    image.mark = 0;
    if (!app.shown_id || app.shown_id != image.slot_id)
        return false;
    image.mark = 1;
    if (image_show())
        return true;
    image.mark = 0;
    return false;
}

static uint16_t on_miss(void)
{
    uint16_t sleep, longer;
    uint16_t lost = app.cfg.lost_ap_s ? app.cfg.lost_ap_s : CFG_LOST_AP_S;

    if (app.misses < 255)
        app.misses++;
    /* lost_s is the time that has passed: the sleeps before this round, not the next one */
    if (app.lost_s < 0xFFFF0000ul)
        app.lost_s += app.slept_s;
    sleep = app_backoff_s(app.interval_s, app.misses);
    longer = app_long_poll_s(app.lost_s);   /* gone for a day: save the battery (#261) */
    if (longer > sleep)
        sleep = longer;
    if (lost == ESL_CFG_LOST_AP_NEVER || app.lost_shown)
        return sleep;
    if (app.lost_s >= lost) {
        if (!show_marked()) {
            draw_status(msg_lost);
            app.shown_id = 0; /* the AP offers the image again when it is back */
        }
        app.lost_shown = 1;
    } else if (sleep > lost - app.lost_s) {
        sleep = (uint16_t)(lost - app.lost_s);   /* wake up when the time is up (T9) */
    }
    return sleep;
}

/* The channel the AP says it is on, or moves to (CONFIG) */
static uint8_t announced_channel(void)
{
    return app.cfg.channel ? app.cfg.channel : CFG_CHANNEL;
}

/* The round's POLL (flen, still in txbuf) again on channel ch: the same frame as its retry,
   so every channel tried costs one more send and listening window, nothing more */
static void tune(uint8_t ch)
{
    radio_off();
    radio_init(ch);
    radio_on();
}

/* One POLL, with its retry, on another channel; true = answered there (reply is filled in) */
static bool try_channel(uint8_t ch, uint8_t flen)
{
    tune(ch);
    return exchange_retry(flen);
}

/*
 * No answer on the tag's channel (radio on): one POLL, with its retry, on the channel
 * the AP announced, then on the built-in one from the CFG_BUILTIN_AFTER-th miss in a row,
 * each when it is another (docs/protokoll.md, "Changing the channel"). An AP that moved is
 * found at the first miss; the built-in channel finds one that went back to it before the
 * tag heard. True = reply is filled in and the tag stays there.
 */
static bool try_announced(uint8_t flen)
{
    uint8_t ch = announced_channel();

    if (ch == app.channel || !try_channel(ch, flen)) {
        ch = CFG_CHANNEL;
        /* the built-in channel only after a few misses: another AP there (every AP answers
           every tag) would keep a tag that only lost one reply to noise */
        if (app.misses + 1 < CFG_BUILTIN_AFTER || ch == app.channel || ch == announced_channel() ||
            !try_channel(ch, flen)) {
            radio_init(app.channel); /* the next round starts where the tag was */
            return false;
        }
    }
    app.channel = ch;
    app.channel_moved = 1;
    return true;
}

static const uint8_t __code scan_list[CFG_SCAN_COUNT] = { CFG_SCAN_CHANNELS };

/*
 * The AP is not on the tag's, the announced or the built-in channel (#258): the third
 * round in a row without an answer looks once on each of the first CFG_SCAN_FIRST channels
 * of CFG_SCAN_CHANNELS, every later one on the next channel of the list only, round and
 * round through all of 11-26, so a long outage costs one listening window more per round.
 * The round's POLL once on each, without the retry. True = reply is filled in and the tag
 * stays on that channel.
 */
static bool scan(uint8_t flen)
{
    uint8_t steps = CFG_SCAN_COUNT, tries = 1, ch;

    if (app.misses + 1 < CFG_SCAN_AFTER)
        return false;
    if (app.misses + 1 == CFG_SCAN_AFTER) {
        app.scan_next = 0;
        tries = CFG_SCAN_FIRST;
    }
    while (tries && steps--) {
        ch = scan_list[app.scan_next];
        if (++app.scan_next == CFG_SCAN_COUNT)
            app.scan_next = 0;
        if (ch == app.channel || ch == announced_channel() || ch == CFG_CHANNEL)
            continue;                /* tried this round already */
        tries--;
        tune(ch);
        if (exchange(flen)) {
            app.channel = ch;
            app.channel_moved = 1;
            return true;
        }
    }
    radio_init(app.channel);
    return false;
}

/* ---- Public ---- */

void app_init(void)
{
    uint8_t i;

    hal_mac_read(app.mac);
    app_store_load();
    app.channel_moved = 0;
    app.scan_next = 0;
    app.shown_id = 0;
    app.interval_s = base_poll_s();
    app.misses = 0;
    app.lost_s = 0;
    app.slept_s = 0;
    app.lost_shown = 0;
    app.poll_flags = ESL_POLL_F_BOOTED;
    app.rx_rssi = 0;
    app.rx_lqi = 0;
    app.link_flags = 0;
    app.seq = 0;
    app.rng = 0xACE1;
    for (i = 0; i < MAC_ADDR_LEN; i++)
        app.rng ^= (uint16_t)app.mac[i] << ((i & 1) ? 8 : 0);
    if (!app.rng)
        app.rng = 1;
    ota_init();
    image_init(ota.present);

    radio_init(app.channel);
    /* The last image if flash has it, else the start screen (T9) */
    if (!(image.slot_id && show_image()) && !draw_status(msg_waiting))
        app.poll_flags |= ESL_POLL_F_DRAWFAIL;
}

static uint16_t poll_round(void)
{
    bool got, fw_done = false;
    uint8_t img = IMAGE_MORE;        /* nothing to show */
    uint8_t flen;

    hal_wdt_kick();
    radio_on();
    flen = build_poll_frame();
    got = exchange_retry(flen) || try_announced(flen) || scan(flen);
    if (got)                         /* the next POLL tells the AP this one needed its retry */
        app.link_flags = retried ? ESL_POLL_LF_RETRY : 0;
    if (got && reply.type == ESL_MSG_FW) {
        /* The AP sends firmware only when nothing else is waiting: nothing to draw */
        fw_done = fw_transfer();
        reply.type = ESL_MSG_FW;
        hal_wdt_kick();
    } else if (got && reply.type == ESL_MSG_IMG) {
        img = image_transfer();
        reply.type = ESL_MSG_IMG;
        hal_wdt_kick();
    }
    radio_off();

    if (!got)
        return on_miss();
    ota_confirm();                   /* a new image that reaches the AP stays */
    if (fw_done)
        ota_install();               /* restarts into the bootloader */

    app.misses = 0;
    app.lost_s = 0;
    app.poll_flags &= (uint8_t)~ESL_POLL_F_BOOTED;
    if (reply.type == ESL_MSG_CONFIG)
        apply_config();              /* new base settings first, next_poll_s below as for any reply */
    else if (app.channel_moved)
        app_store_save();            /* the new channel, for the next start */
    app.channel_moved = 0;
    if (reply.next_poll_s)
        app.interval_s = reply.next_poll_s;

    if (img == IMAGE_DONE) {
        image.mark = 0;              /* the new picture (or the same one again) without the mark */
        show_image();
    } else {
        if (img == IMAGE_STOP)
            app.poll_flags |= ESL_POLL_F_DRAWFAIL;   /* refused: the AP sees it and stops offering it */
        if (image.mark && !(reply.type == ESL_MSG_IMG && img == IMAGE_MORE)) {
            /* The AP is back with nothing new to send: the picture again, without the mark.
               While a new one is coming in, the marked one stays until it is whole. */
            image.mark = 0;
            if (app.shown_id != image.slot_id || !show_image()) {
                draw_status(msg_waiting);
                app.shown_id = 0;
            }
        } else if (app.lost_shown && app.shown_id == 0) {
            draw_status(msg_waiting);    /* the AP is back but has no content for us */
        }
    }
    app.lost_shown = 0;
    hal_wdt_kick();
    return app.interval_s;
}

uint16_t app_poll_once(void)
{
    app.slept_s = poll_round();
    return app.slept_s;
}
