/* Writes the golden file for one release (ap/test/compat/README.md): built by
 * make_golden.py against the AP core of that release, never against today's.
 *
 * An AP of that release gets a layout file, content and settings over its API, and its
 * tags (fake tags, as in test_radio.c) poll until they have everything. What its NVS then
 * holds is written out with what that release reads back from it (content_id, settings,
 * the "seen" record). Then the AP restarts from that NVS and every tag polls once more:
 * each POLL and the AP's answer are written out too. test_compat.c gives today's AP the
 * same NVS and the same POLLs and checks it gets the same.
 *
 * Only API that has not changed since 0.8.2 is used; what came later is behind
 * HAS_NO_LINK (0.9.0: 14-byte POLL of older tags) and HAS_LONG (0.9.0: long_poll_s and
 * long_after_min), which make_golden.py sets from the release. The radio channel in NVS is
 * not covered yet.
 *
 * Some tags get new content or settings once they have everything, and do not poll
 * again until the AP has restarted, as a tag out of range during an update: their POLL
 * after the restart is answered with CONFIG or IMG instead of IDLE. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_api.h"
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_persist.h"
#include "esl_proto.h"
#include "esl_radio.h"
#include "esl_store.h"

#define CAP 16
#define PAN 0x4553
#define AUTH "Bearer t"

/* ---- fake NVS, as main/store_nvs.c lays it out --------------------------- */
static struct {
    uint8_t blob[CAP][ESL_PERSIST_BLOB_MAX];
    size_t len[CAP];
    uint8_t cfg[CAP][ESL_PERSIST_CFG_LEN];
    size_t cfg_len[CAP];
    uint8_t seen[CAP][ESL_PERSIST_SEEN_LEN];
    size_t seen_len[CAP];
    char *layout[ESL_MODEL_COUNT];
    size_t layout_len[ESL_MODEL_COUNT];
} nvs;

static bool nvs_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    (void)ctx;
    nvs.len[slot] = esl_persist_encode(mac, c, nvs.blob[slot]);
    return true;
}

static void nvs_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    nvs.len[slot] = 0;
}

static void nvs_cfg(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    (void)ctx;
    nvs.cfg_len[slot] = cfg ? esl_persist_cfg_encode(mac, cfg, nvs.cfg[slot]) : 0;
}

static void nvs_seen(void *ctx, unsigned slot, uint64_t mac, const esl_seen_t *s)
{
    (void)ctx;
    nvs.seen_len[slot] = s ? esl_persist_seen_encode(mac, s, nvs.seen[slot]) : 0;
}

static uint32_t unix_now(void *ctx)
{
    (void)ctx;
    return 1793000000u; /* fixed, so the seen record is the same every run */
}

static void layout_save(void *ctx, uint8_t model, const char *data, size_t len)
{
    (void)ctx;
    free(nvs.layout[model]);
    nvs.layout[model] = malloc(len);
    memcpy(nvs.layout[model], data, len);
    nvs.layout_len[model] = len;
}

static void layout_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    nvs.layout_len[model] = 0;
}

static const esl_store_ops_t store_ops = {
    .persist_save = nvs_save, .persist_erase = nvs_erase, .persist_cfg = nvs_cfg,
    .persist_seen = nvs_seen, .unix_now = unix_now,
};
static const esl_layout_ops_t layout_ops = {.persist_save = layout_save, .persist_erase = layout_erase};

static esl_store_t st;
static esl_layouts_t layouts;
static esl_image_t images;
static esl_api_t api;
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;
static FILE *gold;

static void boot(bool from_nvs)
{
    static uint8_t *mem;
    if (!mem)
        mem = malloc(esl_image_mem_size());
    esl_image_init(&images, mem, NULL);
    esl_radio_set_image(&images);
    esl_layouts_init(&layouts, &layout_ops);
    esl_store_free(&st);
    esl_store_init(&st, CAP, &store_ops, 0);
    if (!from_nvs)
        return;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++)
        if (nvs.layout_len[m] && !esl_layouts_restore(&layouts, m, nvs.layout[m], nvs.layout_len[m]))
            fprintf(stderr, "the layout file for model %u does not load again\n", m);
    for (unsigned s = 0; s < CAP; s++) {
        uint64_t mac;
        esl_content_t c;
        esl_tag_cfg_t cfg;
        esl_seen_t seen;
        if (nvs.len[s] && esl_persist_decode(nvs.blob[s], nvs.len[s], &mac, &c))
            esl_store_restore(&st, s, mac, &c);
        if (nvs.cfg_len[s] && esl_persist_cfg_decode(nvs.cfg[s], nvs.cfg_len[s], &mac, &cfg))
            esl_store_restore_cfg(&st, s, mac, &cfg);
        if (nvs.seen_len[s] && esl_persist_seen_decode(nvs.seen[s], nvs.seen_len[s], &mac, &seen))
            esl_store_restore_seen(&st, s, mac, &seen);
    }
}

static int call(const char *method, const char *uri, const char *body, size_t len)
{
    int status = esl_api_handle(&api, method, uri, AUTH, body, len, 0, &out);
    if (status != 200) {
        fprintf(stderr, "%s %s: %d %s\n", method, uri, status, resp);
        exit(1);
    }
    return status;
}

static void hex(const char *tag, const void *data, size_t len)
{
    const uint8_t *p = data;
    fputs(tag, gold);
    fputc(' ', gold);
    for (size_t i = 0; i < len; i++)
        fprintf(gold, "%02X", p[i]);
}

typedef struct {
    uint64_t mac;
    uint8_t model;
    uint16_t fw;
    int kind; /* 0: today's tag, 1: 14-byte POLL (before 0.9.0), 2: 12-byte POLL, no images */
    const char *content, *config;
    const char *later_content, *later_config; /* set after it has everything; it polls again only after the restart */
} tag_spec_t;

static const tag_spec_t specs[] = {
    {0xC0FFEE01, ESL_MODEL_29, 600, 1, "{\"layout\":\"price\",\"fields\":[\"Milk 1 L\",\"24.90\",\"NOK 24.90/L\"],"
                                       "\"price_highlight\":true}", "{\"poll_s\":120,\"lost_ap_s\":65535}",
     NULL, "{\"poll_s\":180,\"lost_ap_s\":65535}"},
    {0xC0FFEE02, ESL_MODEL_154, 902, 0, "{\"layout\":\"text\",\"fields\":[\"Opening hours\",\"Mon-Fri 9-21\"],"
                                        "\"frame\":true}",
#ifdef HAS_LONG
     "{\"poll_s\":300,\"max_poll_s\":1800,\"long_poll_s\":3600,\"long_after_min\":60}",
#else
     "{\"poll_s\":300,\"max_poll_s\":1800}",
#endif
     "{\"layout\":\"text\",\"fields\":[\"Opening hours\",\"Mon-Sat 9-21\"],\"frame\":true}", NULL},
    {0xC0FFEE03, ESL_MODEL_29, 600, 1, "{\"layout\":\"shelf\",\"fields\":[\"Coffee\",\"59.90\",\"500 g\",\"Organic\"]}",
     NULL, NULL, NULL},
    {0xC0FFEE04, ESL_MODEL_29, 1, 2, "{\"layout\":\"text\",\"fields\":[\"Old tag\"]}", "{\"poll_s\":900}", NULL,
     "{\"poll_s\":600}"},
    {0xC0FFEE05, ESL_MODEL_154, 902, 0, NULL, NULL, NULL, NULL},
};
#define NTAGS (sizeof(specs) / sizeof(specs[0]))

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: gen_golden RELEASE LAYOUT-2.9.json OUT\n");
        return 2;
    }
    static char file[ESL_LAYOUT_MAX_SIZE];
    FILE *f = fopen(argv[2], "rb");
    size_t flen = f ? fread(file, 1, sizeof(file), f) : 0;
    if (f)
        fclose(f);
    gold = fopen(argv[3], "w");
    if (!flen || !gold) {
        perror("gen_golden");
        return 1;
    }
    api.store = &st;
    api.token = "t";
    api.fw_version = "0.0.0";
    api.layouts = &layouts;
    esl_sb_init(&out, resp, sizeof(resp));

    boot(false);
    call("PUT", "/api/layouts/2.9", file, flen);

    esl_faketag_t t[NTAGS];
    uint64_t now = 1000;
    for (unsigned i = 0; i < NTAGS; i++) {
        esl_faketag_init(&t[i], specs[i].mac, specs[i].fw, 2900, specs[i].model);
#ifdef HAS_NO_LINK /* before 0.9.0 every tag sent the 14-byte POLL */
        t[i].no_link = specs[i].kind == 1;
#endif
        t[i].legacy = specs[i].kind == 2;
        t[i].images = specs[i].kind != 2;
        esl_faketag_round(&t[i], &st, &layouts, PAN, -60, now += 100);
    }
    for (unsigned i = 0; i < NTAGS; i++) {
        char uri[64];
        snprintf(uri, sizeof(uri), "/api/tags/%016" PRIX64, specs[i].mac);
        if (specs[i].content)
            call("PUT", uri, specs[i].content, strlen(specs[i].content));
        strcat(uri, "/config");
        if (specs[i].config)
            call("PUT", uri, specs[i].config, strlen(specs[i].config));
    }
    /* every tag polls until it has it all (two IDLE in a row) */
    for (unsigned i = 0; i < NTAGS; i++) {
        unsigned idle = 0;
        for (unsigned r = 0; r < 20 && idle < 2; r++) {
            uint8_t m = esl_faketag_round(&t[i], &st, &layouts, PAN, -60, now += 1000);
            idle = m == ESL_MSG_IDLE ? idle + 1 : 0;
        }
        if (idle < 2) {
            fprintf(stderr, "tag %u never settled\n", i);
            return 1;
        }
    }
    /* new content or settings for tags that are away until the AP has restarted */
    for (unsigned i = 0; i < NTAGS; i++) {
        char uri[64];
        snprintf(uri, sizeof(uri), "/api/tags/%016" PRIX64, specs[i].mac);
        if (specs[i].later_content)
            call("PUT", uri, specs[i].later_content, strlen(specs[i].later_content));
        strcat(uri, "/config");
        if (specs[i].later_config)
            call("PUT", uri, specs[i].later_config, strlen(specs[i].later_config));
    }
    esl_store_save_seen(&st);

    fprintf(gold, "# The AP core of %s: its NVS after content and settings over the API (gen_golden.c)\n", argv[1]);
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++)
        if (nvs.layout_len[m]) {
            fprintf(gold, "layout %u", m);
            hex("", nvs.layout[m], nvs.layout_len[m]);
            fputc('\n', gold);
        }
    for (unsigned s = 0; s < CAP; s++) {
        uint64_t mac;
        esl_content_t c;
        esl_tag_cfg_t cfg;
        esl_seen_t seen;
        if (nvs.len[s] && esl_persist_decode(nvs.blob[s], nvs.len[s], &mac, &c)) {
            hex("content", nvs.blob[s], nvs.len[s]);
            fprintf(gold, " %08" PRIX32 "\n", esl_content_id(&c));
        }
        if (nvs.cfg_len[s] && esl_persist_cfg_decode(nvs.cfg[s], nvs.cfg_len[s], &mac, &cfg)) {
            hex("cfg", nvs.cfg[s], nvs.cfg_len[s]);
#ifdef HAS_LONG
            fprintf(gold, " %u %u %u %u %u\n", cfg.poll_s, cfg.max_poll_s, cfg.lost_ap_s, cfg.long_poll_s,
                    cfg.long_after_min);
#else
            fprintf(gold, " %u %u %u - -\n", cfg.poll_s, cfg.max_poll_s, cfg.lost_ap_s);
#endif
        }
        if (nvs.seen_len[s] && esl_persist_seen_decode(nvs.seen[s], nvs.seen_len[s], &mac, &seen)) {
            hex("seen", nvs.seen[s], nvs.seen_len[s]);
            fprintf(gold, " %u %u %u %d %" PRIu32 "\n", seen.model, seen.fw, seen.battery_mv, seen.rssi, seen.unix_s);
        }
    }

    /* the AP restarts from its NVS; each tag polls once, as it would after an update of the AP */
    boot(true);
    for (unsigned i = 0; i < NTAGS; i++) {
        uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
        size_t n = esl_faketag_poll(&t[i], PAN, rx);
        size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, now += 1000, tx, NULL);
        hex("poll", rx, n);
        hex("", tx, m);
        fprintf(gold, " %" PRIu64 "\n", now);
        if (m)
            esl_faketag_receive(&t[i], tx, m);
    }
    fclose(gold);
    return 0;
}
