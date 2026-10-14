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

uint16_t app_backoff_s(uint16_t interval_s, uint8_t misses)
{
    uint16_t s = interval_s;
    uint8_t i;

    if (misses < CFG_BACKOFF_AFTER || s >= CFG_MAX_POLL_S)
        return s;
    for (i = CFG_BACKOFF_AFTER; i <= misses; i++) {
        if (s >= CFG_MAX_POLL_S / 2)
            return CFG_MAX_POLL_S;
        s <<= 1;
    }
    return s;
}

/* Sends POLL and waits for a valid answer to us. true = reply is filled in. */
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

static uint16_t on_miss(void)
{
    uint16_t sleep;

    if (app.misses < 255)
        app.misses++;
    sleep = app_backoff_s(app.interval_s, app.misses);
    if (app.lost_s < 0xFFFF0000ul)
        app.lost_s += sleep;
    if (app.lost_s >= CFG_LOST_AP_S && !app.lost_shown) {
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
    app.shown_id = 0;
    app.interval_s = CFG_POLL_S;
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

    radio_init(CFG_CHANNEL);
    if (!draw_status(msg_waiting))
        app.poll_flags |= ESL_POLL_F_DRAWFAIL;
}

uint16_t app_poll_once(void)
{
    uint8_t payload[ESL_POLL_LEN];
    uint8_t flen;
    bool got;

    hal_wdt_kick();
    proto_build_poll(payload, FW_VERSION, battery_read_mv(), app.shown_id, app.poll_flags);
    flen = proto_build_frame(txbuf, app.seq++, CFG_PAN_ID, app.mac, payload, ESL_POLL_LEN);

    radio_on();
    got = exchange(flen);
    if (!got) {
        hal_delay_ms(CFG_RETRY_MIN_MS + (uint8_t)(rng_next() % (CFG_RETRY_MAX_MS - CFG_RETRY_MIN_MS + 1)));
        got = exchange(flen);
    }
    radio_off();

    if (!got)
        return on_miss();

    app.misses = 0;
    app.lost_s = 0;
    app.poll_flags &= (uint8_t)~ESL_POLL_F_BOOTED;
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
