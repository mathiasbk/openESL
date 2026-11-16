/* radio_sim.c - --radio sim[:N[,ms]]: N simulated tags (1..64, default 2) in the program,
 * for trying the API and the dashboard without a radio, and for the tests (test.py).
 * Each polls every ms milliseconds (default 2000), alternately the 2.9" and the 1.54"
 * model, and behaves as a tag does on the air in the parts the AP sees: CONFIG is
 * acknowledged in the next POLL, an image is fetched block by block with IMG_REQ, checked
 * against its CRC and then reported as the content_id. MACs are 00000000FAC7xxxx, as the
 * C6's stub tags. The full tag firmware on a simulated radio is sim/ (esl-sim). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ap.h"
#include "esl_crc32.h"
#include "esl_proto.h"

#define SIM_MAX 64
#define SIM_IMG_MAX 8192

typedef struct {
    uint64_t mac;
    uint8_t model;
    uint8_t seq;
    uint16_t cfg_ver;
    uint32_t cid;          /* content_id shown */
    int64_t due_us;        /* next POLL */
    /* an image being fetched */
    uint32_t img_id;
    uint16_t img_size;
    uint16_t img_pos;
    uint8_t img[SIM_IMG_MAX];
    bool want_block;       /* an IMG_REQ goes out next */
} sim_tag_t;

typedef struct {
    sim_tag_t tags[SIM_MAX];
    unsigned n;
    int64_t period_us;
    uint8_t channel;
    uint16_t pan;
} sim_t;

static sim_t sim;

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

/* MAC header tag -> AP: data, PAN compression, dst short (the AP), src long (the tag) */
static size_t header(sim_tag_t *t, uint8_t *f)
{
    f[0] = 0x41, f[1] = 0xC8, f[2] = t->seq++;
    put16(f + 3, sim.pan);
    put16(f + 5, ESL_AP_SHORT_ADDR);
    for (int i = 0; i < 8; i++)
        f[7 + i] = (uint8_t)(t->mac >> (8 * i));
    return ESL_MHR_LEN;
}

static size_t poll_frame(sim_tag_t *t, uint8_t *f)
{
    size_t h = header(t, f);
    uint8_t *p = f + h;
    memset(p, 0, ESL_POLL_LEN_LINK);
    p[ESL_OFF_TYPE] = ESL_MSG_POLL;
    p[ESL_OFF_VER] = ESL_PROTO_VER;
    put16(p + ESL_POLL_OFF_FW, 1000);
    put16(p + ESL_POLL_OFF_BATT, 2950);
    put32(p + ESL_POLL_OFF_CID, t->cid);
    p[ESL_POLL_OFF_FLAGS] = ESL_POLL_F_IMAGE;
    p[ESL_POLL_OFF_MODEL] = t->model;
    put16(p + ESL_POLL_OFF_CFG_VER, t->cfg_ver);
    return h + ESL_POLL_LEN_LINK;
}

static size_t img_req_frame(sim_tag_t *t, uint8_t *f)
{
    size_t h = header(t, f);
    uint8_t *p = f + h;
    memset(p, 0, ESL_IREQ_LEN);
    p[ESL_OFF_TYPE] = ESL_MSG_IMG_REQ;
    p[ESL_OFF_VER] = ESL_PROTO_VER;
    p[ESL_IREQ_OFF_MODEL] = t->model;
    p[ESL_IREQ_OFF_MAX] = ESL_IMG_CHUNK_MAX;
    put32(p + ESL_IREQ_OFF_ID, t->img_id);
    put16(p + ESL_IREQ_OFF_POS, t->img_pos);
    return h + ESL_IREQ_LEN;
}

/* ---- esl_link_t ---- */

static int sim_start(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    (void)l, (void)short_addr, (void)promiscuous;
    sim.channel = channel;
    sim.pan = pan;
    return 0;
}

static int sim_set_channel(esl_link_t *l, uint8_t channel)
{
    (void)l;
    sim.channel = channel;
    return 0;
}

/* A reply from the AP: what the tag it is for does with it */
static int sim_send(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req)
{
    esl_link_sending(l, req, ap_clock_us());
    if (len < ESL_MHR_LEN + 2 + ESL_FCS_LEN)
        return 0;
    uint64_t mac = 0;
    for (int i = 0; i < 8; i++)
        mac |= (uint64_t)psdu[5 + i] << (8 * i);
    const uint8_t *p = psdu + ESL_MHR_LEN;
    size_t pl = len - ESL_MHR_LEN - ESL_FCS_LEN;
    for (unsigned i = 0; i < sim.n; i++) {
        sim_tag_t *t = &sim.tags[i];
        if (t->mac != mac)
            continue;
        if (p[ESL_OFF_TYPE] == ESL_MSG_CONFIG && pl >= ESL_CONFIG_OFF_VER + 2)
            t->cfg_ver = get16(p + ESL_CONFIG_OFF_VER);
        if (p[ESL_OFF_TYPE] == ESL_MSG_IMG && pl > ESL_IMG_OFF_DATA) {
            uint32_t id = get32(p + ESL_IMG_OFF_ID);
            uint16_t size = get16(p + ESL_IMG_OFF_SIZE), pos = get16(p + ESL_IMG_OFF_POS);
            size_t n = pl - ESL_IMG_OFF_DATA;
            if (size > SIM_IMG_MAX || pos + n > size || p[ESL_IMG_OFF_MODEL] != t->model)
                return 0;
            if (id != t->img_id || pos == 0) {
                t->img_id = id;
                t->img_size = size;
                t->img_pos = 0;
            }
            if (pos != t->img_pos)
                return 0;
            memcpy(t->img + pos, p + ESL_IMG_OFF_DATA, n);
            t->img_pos = (uint16_t)(pos + n);
            if (t->img_pos < t->img_size) {
                t->want_block = true;
            } else {
                uint32_t crc = esl_crc32(t->img, t->img_size);
                if ((crc ? crc : 1) == id)
                    t->cid = id;
                else
                    ap_log("sim tag %u: image %08lX fails its CRC", i, (unsigned long)id);
                t->img_id = 0;
            }
        }
        break;
    }
    return 0;
}

static int sim_recv(esl_link_t *l, esl_link_rx_t *f, int timeout_ms)
{
    (void)l;
    int64_t now = ap_clock_us(), until = now + (int64_t)timeout_ms * 1000;
    for (;;) {
        sim_tag_t *next = NULL;
        for (unsigned i = 0; i < sim.n; i++) {
            sim_tag_t *t = &sim.tags[i];
            if (t->want_block) { /* the next block right away, as a tag does */
                t->want_block = false;
                f->len = (uint8_t)(img_req_frame(t, f->psdu) + ESL_FCS_LEN);
                goto got;
            }
            if (!next || t->due_us < next->due_us)
                next = t;
        }
        if (next && next->due_us <= now) {
            next->due_us = now + sim.period_us;
            f->len = (uint8_t)(poll_frame(next, f->psdu) + ESL_FCS_LEN);
            goto got;
        }
        int64_t wake = next && next->due_us < until ? next->due_us : until;
        if (wake <= now)
            return 0;
        usleep((useconds_t)(wake - now));
        now = ap_clock_us();
        if (now >= until && !(next && next->due_us <= now))
            return 0;
    }
got:
    f->psdu[f->len - 2] = f->psdu[f->len - 1] = 0;   /* FCS: the radio checks it, esl_radio_handle does not */
    f->rssi = -50;
    f->channel = sim.channel;
    f->rx_us = ap_clock_us();
    f->ref = 0;
    return 1;
}

static int sim_open(esl_link_t *l, const char *arg)
{
    unsigned n = 2, ms = 2000;
    if (*arg && sscanf(arg, "%u,%u", &n, &ms) < 1) {
        fprintf(stderr, "openesl-ap: --radio sim:N[,ms], not sim:%s\n", arg);
        return -1;
    }
    if (n < 1 || n > SIM_MAX || ms < 50) {
        fprintf(stderr, "openesl-ap: sim: 1-%d tags, a poll every 50 ms or more\n", SIM_MAX);
        return -1;
    }
    memset(&sim, 0, sizeof(sim));
    sim.n = n;
    sim.period_us = (int64_t)ms * 1000;
    int64_t now = ap_clock_us();
    for (unsigned i = 0; i < n; i++) {
        sim.tags[i].mac = 0x00000000FAC70001ull + i;
        sim.tags[i].model = (uint8_t)(i % 2 ? 2 : 1);
        sim.tags[i].due_us = now + (int64_t)(i + 1) * sim.period_us / (n + 1);  /* spread over a period */
    }
    l->name = "sim";
    l->start = sim_start;
    l->send = sim_send;
    l->set_channel = sim_set_channel;
    l->ctx = &sim;
    return 0;
}

const radio_backend_t radio_sim = {
    "sim", "sim[:N[,ms]]     N simulated tags polling every ms (2, 2000)", sim_open, sim_recv, NULL,
};
