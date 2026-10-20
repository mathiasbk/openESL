#include "esl_coproc.h"
#include <string.h>

uint16_t esl_coproc_crc16(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t esl_coproc_frame(uint8_t *out, uint8_t type, const void *payload, size_t len)
{
    if (len > ESL_COPROC_PAYLOAD_MAX)
        return 0;
    out[0] = ESL_COPROC_SYNC;
    out[1] = type;
    out[2] = (uint8_t)len;
    if (len)
        memcpy(&out[3], payload, len);
    uint16_t crc = esl_coproc_crc16(0xFFFF, &out[1], len + 2);
    out[3 + len] = (uint8_t)crc;
    out[4 + len] = (uint8_t)(crc >> 8);
    return len + ESL_COPROC_OVERHEAD;
}

/* ---- little-endian helpers ---- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

/* ---- CONFIG: channel, pan, short, tx_power, flags (7 bytes) ---- */

#define CONFIG_LEN 7

static void put_config(uint8_t *p, const esl_coproc_config_t *c)
{
    p[0] = c->channel;
    put16(&p[1], c->pan);
    put16(&p[3], c->short_addr);
    p[5] = (uint8_t)c->tx_power;
    p[6] = c->flags;
}

static void get_config(const uint8_t *p, esl_coproc_config_t *c)
{
    c->channel = p[0];
    c->pan = get16(&p[1]);
    c->short_addr = get16(&p[3]);
    c->tx_power = (int8_t)p[5];
    c->flags = p[6];
}

size_t esl_coproc_encode_config(uint8_t *out, const esl_coproc_config_t *c)
{
    uint8_t p[CONFIG_LEN];
    put_config(p, c);
    return esl_coproc_frame(out, ESL_COPROC_CONFIG, p, sizeof(p));
}

bool esl_coproc_decode_config(const uint8_t *p, size_t len, esl_coproc_config_t *c)
{
    if (len < CONFIG_LEN)
        return false;
    get_config(p, c);
    return c->channel >= 11 && c->channel <= 26;
}

/* ---- HELLO: version, configured, fw, ieee_addr, config (19 bytes) ---- */

#define HELLO_LEN (4 + 8 + CONFIG_LEN)

size_t esl_coproc_encode_hello(uint8_t *out, const esl_coproc_hello_t *h)
{
    uint8_t p[HELLO_LEN];
    p[0] = h->version;
    p[1] = h->configured;
    put16(&p[2], h->fw);
    put32(&p[4], (uint32_t)h->ieee_addr);
    put32(&p[8], (uint32_t)(h->ieee_addr >> 32));
    put_config(&p[12], &h->cfg);
    return esl_coproc_frame(out, ESL_COPROC_HELLO, p, sizeof(p));
}

bool esl_coproc_decode_hello(const uint8_t *p, size_t len, esl_coproc_hello_t *h)
{
    /* a newer radio may add fields at the end */
    if (len < HELLO_LEN)
        return false;
    h->version = p[0];
    h->configured = p[1];
    h->fw = get16(&p[2]);
    h->ieee_addr = get32(&p[4]) | (uint64_t)get32(&p[8]) << 32;
    get_config(&p[12], &h->cfg);
    return true;
}

/* ---- RX: ref, rssi, channel, PSDU ---- */

size_t esl_coproc_encode_rx(uint8_t *out, const esl_coproc_rx_t *rx)
{
    uint8_t p[3 + ESL_COPROC_PSDU_MAX];
    if (rx->len > ESL_COPROC_PSDU_MAX)
        return 0;
    p[0] = rx->ref;
    p[1] = (uint8_t)rx->rssi;
    p[2] = rx->channel;
    memcpy(&p[3], rx->psdu, rx->len);
    return esl_coproc_frame(out, ESL_COPROC_RX, p, 3u + rx->len);
}

bool esl_coproc_decode_rx(const uint8_t *p, size_t len, esl_coproc_rx_t *rx)
{
    if (len < 3 || len - 3 > ESL_COPROC_PSDU_MAX)
        return false;
    rx->ref = p[0];
    rx->rssi = (int8_t)p[1];
    rx->channel = p[2];
    rx->len = (uint8_t)(len - 3);
    memcpy(rx->psdu, &p[3], rx->len);
    return true;
}

/* ---- TX: ref, PSDU (length incl. FCS; the radio fills in the FCS) ---- */

size_t esl_coproc_encode_tx(uint8_t *out, uint8_t ref, const uint8_t *psdu, size_t len)
{
    uint8_t p[1 + ESL_COPROC_PSDU_MAX];
    if (len < 3 || len > ESL_COPROC_PSDU_MAX)
        return 0;
    p[0] = ref;
    memcpy(&p[1], psdu, len);
    return esl_coproc_frame(out, ESL_COPROC_TX, p, 1 + len);
}

bool esl_coproc_decode_tx(const uint8_t *p, size_t len, uint8_t *ref, const uint8_t **psdu, size_t *psdu_len)
{
    if (len < 1 + 3 || len - 1 > ESL_COPROC_PSDU_MAX)
        return false;
    *ref = p[0];
    *psdu = &p[1];
    *psdu_len = len - 1;
    return true;
}

/* ---- TX_DONE: ref, status, resends, rx_to_tx_us (5 bytes) ---- */

size_t esl_coproc_encode_tx_done(uint8_t *out, const esl_coproc_tx_done_t *d)
{
    uint8_t p[5] = {d->ref, d->status, d->resends};
    put16(&p[3], d->rx_to_tx_us);
    return esl_coproc_frame(out, ESL_COPROC_TX_DONE, p, sizeof(p));
}

bool esl_coproc_decode_tx_done(const uint8_t *p, size_t len, esl_coproc_tx_done_t *d)
{
    if (len < 5)
        return false;
    d->ref = p[0];
    d->status = p[1];
    d->resends = p[2];
    d->rx_to_tx_us = get16(&p[3]);
    return true;
}

/* ---- STATS: 8 x u32 ---- */

#define STATS_N 8

size_t esl_coproc_encode_stats(uint8_t *out, const esl_coproc_stats_t *s)
{
    const uint32_t v[STATS_N] = {s->rx, s->tx, s->tx_failed, s->tx_resent, s->tx_late,
                                 s->dropped, s->link_errors, s->uptime_s};
    uint8_t p[4 * STATS_N];
    for (int i = 0; i < STATS_N; i++)
        put32(&p[4 * i], v[i]);
    return esl_coproc_frame(out, ESL_COPROC_STATS, p, sizeof(p));
}

bool esl_coproc_decode_stats(const uint8_t *p, size_t len, esl_coproc_stats_t *s)
{
    if (len < 4 * STATS_N)
        return false;
    uint32_t *v[STATS_N] = {&s->rx, &s->tx, &s->tx_failed, &s->tx_resent, &s->tx_late,
                            &s->dropped, &s->link_errors, &s->uptime_s};
    for (int i = 0; i < STATS_N; i++)
        *v[i] = get32(&p[4 * i]);
    return true;
}

/* ---- SCREEN: per line colour, length, text ---- */

size_t esl_coproc_encode_screen(uint8_t *out, const esl_coproc_screen_t *s)
{
    uint8_t p[ESL_COPROC_SCREEN_LINES * (2 + ESL_COPROC_SCREEN_TEXT_MAX)];
    size_t n = 0;
    for (int i = 0; i < ESL_COPROC_SCREEN_LINES; i++) {
        size_t len = 0; /* strnlen is not C11 */
        while (len < ESL_COPROC_SCREEN_TEXT_MAX && s->text[i][len])
            len++;
        p[n++] = s->color[i];
        p[n++] = (uint8_t)len;
        memcpy(&p[n], s->text[i], len);
        n += len;
    }
    return esl_coproc_frame(out, ESL_COPROC_SCREEN, p, n);
}

bool esl_coproc_decode_screen(const uint8_t *p, size_t len, esl_coproc_screen_t *s)
{
    size_t n = 0;
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < ESL_COPROC_SCREEN_LINES; i++) {
        if (n + 2 > len)
            return false;
        s->color[i] = p[n];
        size_t l = p[n + 1];
        n += 2;
        if (l > ESL_COPROC_SCREEN_TEXT_MAX || n + l > len)
            return false;
        memcpy(s->text[i], &p[n], l);
        n += l;
    }
    return true;
}

/* ---- parser ---- */

enum { S_SYNC, S_TYPE, S_LEN, S_PAYLOAD, S_CRC0, S_CRC1 };

void esl_coproc_parser_init(esl_coproc_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool esl_coproc_parser_feed(esl_coproc_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case S_SYNC:
        if (b == ESL_COPROC_SYNC)
            p->state = S_TYPE;
        else
            p->skipped++;
        return false;
    case S_TYPE:
        p->type = b;
        p->state = S_LEN;
        return false;
    case S_LEN:
        if (b > ESL_COPROC_PAYLOAD_MAX) {
            p->too_long++;
            p->state = S_SYNC;
            return false;
        }
        p->len = b;
        p->got = 0;
        p->state = b ? S_PAYLOAD : S_CRC0;
        return false;
    case S_PAYLOAD:
        p->payload[p->got++] = b;
        if (p->got == p->len)
            p->state = S_CRC0;
        return false;
    case S_CRC0:
        p->crc = b;
        p->state = S_CRC1;
        return false;
    default: {
        p->crc |= (uint16_t)b << 8;
        p->state = S_SYNC;
        uint8_t hdr[2] = {p->type, p->len};
        uint16_t crc = esl_coproc_crc16(esl_coproc_crc16(0xFFFF, hdr, 2), p->payload, p->len);
        if (crc != p->crc) {
            p->bad_crc++;
            return false;
        }
        return true;
    }
    }
}
