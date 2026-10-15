#include "app.h"
#include "config.h"
#include "draw.h"
#include "layout.h"
#include "epd.h"
#include "hal/hal.h"

#define RX_TRIES 3       /* other tags may be in the window; read up to 3 frames */
#define LINE_MAX 24

app_state_t __xdata app;

static uint8_t __xdata txbuf[ESL_MAC_MAX_FRAME];
static uint8_t __xdata rxbuf[ESL_MAC_MAX_FRAME];
static uint8_t __xdata content[ESL_MAX_PAYLOAD];   /* payload currently shown */
static proto_reply_t __xdata reply;
static proto_reply_t __xdata shown;
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
    layout_build_lines(&scene, lp, line_len, 4);
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

/*
 * app.cfg in flash (requirement T12), so a restarted tag uses the AP's settings
 * even before it reaches the AP. One record through hal_store_* (hal.h):
 *   0 CFG_STORE_FORMAT   1-2 ver   3-4 poll_s   5-6 max_poll_s   7-8 lost_ap_s
 * all of app_cfg_t, ver included: the tag reports ver in POLL, so a stale or
 * missing copy only costs one extra CONFIG from the AP. Written only when the
 * settings change (the AP sends CONFIG only then), and with the radio off.
 * The layout file is not kept here (768 bytes > HAL_STORE_MAX).
 */
#define CFG_STORE_FORMAT 1
#define CFG_STORE_LEN    9

static uint8_t __xdata cfg_rec[CFG_STORE_LEN];

static void app_cfg_load(void)
{
    if (hal_store_read(cfg_rec, CFG_STORE_LEN) == CFG_STORE_LEN && cfg_rec[0] == CFG_STORE_FORMAT) {
        app.cfg.ver = ESL_GET_U16(cfg_rec + 1);
        app.cfg.poll_s = ESL_GET_U16(cfg_rec + 3);
        app.cfg.max_poll_s = ESL_GET_U16(cfg_rec + 5);
        app.cfg.lost_ap_s = ESL_GET_U16(cfg_rec + 7);
    } else {
        app.cfg.ver = 0;
        app.cfg.poll_s = 0;
        app.cfg.max_poll_s = 0;
        app.cfg.lost_ap_s = 0;
    }
}

static void app_cfg_save(void)
{
    cfg_rec[0] = CFG_STORE_FORMAT;
    ESL_PUT_U16(cfg_rec + 1, app.cfg.ver);
    ESL_PUT_U16(cfg_rec + 3, app.cfg.poll_s);
    ESL_PUT_U16(cfg_rec + 5, app.cfg.max_poll_s);
    ESL_PUT_U16(cfg_rec + 7, app.cfg.lost_ap_s);
    hal_store_write(cfg_rec, CFG_STORE_LEN);   /* a failed write only costs a CONFIG after a restart */
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
    bool changed = app.cfg.ver != reply.cfg_ver || app.cfg.poll_s != poll ||
                   app.cfg.max_poll_s != max || app.cfg.lost_ap_s != lost;

    app.cfg.ver = reply.cfg_ver;
    app.cfg.poll_s = poll;
    app.cfg.max_poll_s = max;
    app.cfg.lost_ap_s = lost;
    app.interval_s = base_poll_s();
    if (changed)
        app_cfg_save();
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
            return true;
        }
    }
    return false;
}

/* exchange(), and once more after a random pause if nothing came (protokoll.md). */
static bool exchange_retry(uint8_t flen)
{
    if (exchange(flen))
        return true;
    hal_delay_ms(CFG_RETRY_MIN_MS + (uint8_t)(rng_next() % (CFG_RETRY_MAX_MS - CFG_RETRY_MIN_MS + 1)));
    return exchange(flen);
}

/* Builds POLL straight into the payload part of txbuf (proto_build_frame copies it onto itself). */
static uint8_t build_poll_frame(void)
{
    uint8_t *pl = txbuf + ESL_MAC_HDR_LEN;
    uint8_t flags = app.poll_flags;

    if (app.lay_rejected)
        flags |= ESL_POLL_F_LAYOUTFAIL;
    proto_build_poll(pl, FW_VERSION, battery_read_mv(), app.shown_id, flags,
                     app.cfg.ver, layout_file_ver);
    return proto_build_frame(txbuf, app.seq++, CFG_PAN_ID, app.mac, pl, ESL_POLL_LEN_LAYOUT);
}

/* The layout file changed: content drawn with one of its layouts must be fetched again,
   because the AP resolves layout names against the file it has now. */
static void layout_changed(void)
{
    if (app.shown_id && shown.layout >= ESL_LAYOUT_FILE)
        app.shown_id = 0;
}

/*
 * reply is a LAYOUT block. Stores it and asks for the rest of the file in the same round
 * (docs/protokoll.md part 1, "Layout transfer"). Returns true when the file is complete
 * (installed or rejected) or dropped, so the caller polls once more for content.
 * Returns false when there is nothing to do or the AP stopped answering; the download
 * then continues from dl_got the next time the AP offers the same version.
 */
static bool layout_transfer(void)
{
    uint8_t req = 0, n, i;
    uint16_t pos;
    uint32_t ver;

    for (;;) {
        ver = reply.content_id;
        if (ver == 0) {                     /* the AP has no file for this model */
            app.dl_ver = 0;
            app.lay_rejected = 0;
            if (!layout_file_ver)
                return false;
            layout_file_clear();
            layout_changed();
            return true;
        }
        if (ver == layout_file_ver || ver == app.lay_rejected)
            return false;
        if (ver != app.dl_ver || reply.lay_size != app.dl_size) {
            /* A new file, or the file changed during the download: start from the beginning */
            if (reply.lay_size > ESL_LBIN_MAX || reply.lay_size < ESL_LBIN_HDR_LEN) {
                app.dl_ver = 0;
                app.lay_rejected = ver;     /* too big for this tag; the AP should never offer it */
                return true;
            }
            if (layout_file_ver) {          /* the download overwrites the installed file */
                layout_file_clear();
                layout_changed();
            }
            app.dl_ver = ver;
            app.dl_size = reply.lay_size;
            app.dl_got = 0;
        }
        n = reply.field_len[0];
        pos = reply.lay_pos;
        if (pos <= app.dl_got && pos + n > app.dl_got) {
            const uint8_t *d = rxbuf + rx_off + reply.field_off[0];
            for (i = (uint8_t)(app.dl_got - pos); i < n; i++)
                layout_file[pos + i] = d[i];
            app.dl_got = pos + n;
        }
        if (app.dl_got >= app.dl_size) {
            app.dl_ver = 0;
            app.lay_rejected = layout_file_install(app.dl_size, ver) ? 0 : ver;
            return true;
        }
        if (++req > CFG_LAYOUT_MAX_REQ)
            return false;
        n = proto_build_layout_req(txbuf + ESL_MAC_HDR_LEN, ver, app.dl_got, ESL_LAYOUT_CHUNK_MAX);
        if (!exchange_retry(proto_build_frame(txbuf, app.seq++, CFG_PAN_ID, app.mac,
                                              txbuf + ESL_MAC_HDR_LEN, n)) ||
            reply.type != ESL_MSG_LAYOUT)
            return false;
    }
}

static uint16_t on_miss(void)
{
    uint16_t sleep;
    uint16_t lost = app.cfg.lost_ap_s ? app.cfg.lost_ap_s : CFG_LOST_AP_S;

    if (app.misses < 255)
        app.misses++;
    sleep = app_backoff_s(app.interval_s, app.misses);
    if (app.lost_s < 0xFFFF0000ul)
        app.lost_s += sleep;
    if (lost != ESL_CFG_LOST_AP_NEVER && app.lost_s >= lost && !app.lost_shown) {
        draw_status(msg_lost);
        app.shown_id = 0;     /* the AP resends the content when it is back */
        app.lost_shown = 1;
    }
    return sleep;
}

static void show_content(void)
{
    uint8_t i;

    for (i = 0; i < rx_plen; i++)
        content[i] = rxbuf[rx_off + i];
    if (proto_parse_reply(content, rx_plen, &shown) != PROTO_OK)
        return;
    layout_build(&scene, &shown, content);
    if (epd_draw(&scene)) {
        app.shown_id = shown.content_id;
        app.poll_flags &= (uint8_t)~ESL_POLL_F_DRAWFAIL;
    } else {
        app.poll_flags |= ESL_POLL_F_DRAWFAIL;
    }
}

/* ---- Public ---- */

void app_init(void)
{
    uint8_t i;

    hal_mac_read(app.mac);
    app_cfg_load();
    app.shown_id = 0;
    app.interval_s = base_poll_s();
    app.misses = 0;
    app.lost_s = 0;
    app.lost_shown = 0;
    app.poll_flags = ESL_POLL_F_BOOTED;
    app.seq = 0;
    app.rng = 0xACE1;
    for (i = 0; i < MAC_ADDR_LEN; i++)
        app.rng ^= (uint16_t)app.mac[i] << ((i & 1) ? 8 : 0);
    if (!app.rng)
        app.rng = 1;
    app.dl_ver = 0;
    app.dl_size = 0;
    app.dl_got = 0;
    app.lay_rejected = 0;
    layout_file_clear();     /* not kept in flash yet: fetched again after every restart */
    shown.layout = 0;

    radio_init(CFG_CHANNEL);
    if (!draw_status(msg_waiting))
        app.poll_flags |= ESL_POLL_F_DRAWFAIL;
}

uint16_t app_poll_once(void)
{
    bool got;

    hal_wdt_kick();
    radio_on();
    got = exchange_retry(build_poll_frame());
    if (got && reply.type == ESL_MSG_LAYOUT) {
        if (reply.next_poll_s)
            app.interval_s = reply.next_poll_s;
        /* After a finished download, poll again at once: the AP answers with the content
           for the new file. Anything else this round means "nothing to draw". */
        if (!layout_transfer() || !exchange_retry(build_poll_frame()) || reply.type == ESL_MSG_LAYOUT) {
            reply.type = ESL_MSG_LAYOUT;
            reply.next_poll_s = 0;
        }
        hal_wdt_kick();
    }
    radio_off();

    if (!got)
        return on_miss();

    app.misses = 0;
    app.lost_s = 0;
    app.poll_flags &= (uint8_t)~ESL_POLL_F_BOOTED;
    if (reply.type == ESL_MSG_CONFIG)
        apply_config();              /* new base settings first, next_poll_s below as for any reply */
    if (reply.next_poll_s)
        app.interval_s = reply.next_poll_s;

    if (reply.type == ESL_MSG_CONTENT && reply.content_id != app.shown_id)
        show_content();
    else if (app.lost_shown && app.shown_id == 0)
        draw_status(msg_waiting);    /* the AP is back but has no content for us */
    app.lost_shown = 0;
    hal_wdt_kick();
    return app.interval_s;
}
