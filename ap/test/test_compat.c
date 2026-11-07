/* Compatibility with earlier releases (#307, compat/README.md): every compat/v*.txt
 * holds what the AP core of that release left in NVS and how it answered its tags after a
 * restart from it (compat/gen_golden.c). Today's core must read the same NVS to the same
 * content_id, settings and "seen" record, so no tag is sent its content again after an
 * update of the AP, and answer the same POLLs with the same frames, so no tag gets a reply
 * its firmware does not know. A tag that shows an image may be sent a new image when the
 * drawing changed; that is printed, not failed. */
#define _POSIX_C_SOURCE 200809L /* opendir, strdup */
#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esl_api.h"
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_persist.h"
#include "esl_proto.h"
#include "esl_radio.h"
#include "esl_store.h"
#include "tinytest.h"

#define CAP 16
#define PAN 0x4553
#define COMPAT_DIR "compat"

static esl_store_t st;
static esl_layouts_t layouts;
static esl_image_t images;
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

/* Hex digits to bytes, at most cap; the count, or 0 on anything that is not hex. */
static size_t unhex(const char *s, uint8_t *buf, size_t cap)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap)
        return 0;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return 0;
        buf[i] = (uint8_t)v;
    }
    return n / 2;
}

static void boot(void)
{
    static uint8_t *mem;
    if (!mem)
        mem = malloc(esl_image_mem_size());
    esl_image_init(&images, mem, NULL);
    esl_radio_set_image(&images);
    esl_layouts_init(&layouts, NULL);
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
}

static unsigned redrawn;

/* The reply today's AP gave (got) against the one the release gave (want), for one POLL. */
static void check_reply(const char *file, const uint8_t *poll, size_t poll_len, const uint8_t *got, size_t got_len,
                        const uint8_t *want, size_t want_len)
{
    if (got_len == want_len && memcmp(got, want, want_len) == 0) {
        CHECK(1);
        return;
    }
    esl_rx_frame_t rx;
    esl_poll_t p;
    esl_ap_frame_t g, w;
    esl_reply_t gr, wr;
    bool ok = esl_frame_parse_from_tag(poll, poll_len, &rx) && esl_poll_parse(rx.payload, rx.payload_len, &p) &&
              esl_frame_parse_to_tag(got, got_len, &g) && esl_reply_parse(g.payload, g.payload_len, &gr) &&
              esl_frame_parse_to_tag(want, want_len, &w) && esl_reply_parse(w.payload, w.payload_len, &wr);
    char mac[17];
    esl_mac_format(ok ? rx.src_mac : 0, mac);
    /* the drawing changed: a tag that shows the AP's images is sent the new one */
    if (ok && (p.flags & ESL_POLL_F_IMAGE) && wr.type == ESL_MSG_IDLE && gr.type == ESL_MSG_IMG &&
        g.seq == w.seq && gr.next_poll_s == wr.next_poll_s) {
        printf("\n  note %s: %s is sent a new image (its content draws differently now)", file, mac);
        redrawn++;
        CHECK(1);
        return;
    }
    tt_checks++;
    tt_failures++;
    printf("\n  FAIL %s: tag %s gets another reply than from that release (type %u, was %u)\n", file, mac,
           ok ? gr.type : 0, ok ? wr.type : 0);
}

static void check_file(const char *name)
{
    char path[300];
    snprintf(path, sizeof(path), COMPAT_DIR "/%s", name);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    boot();
    esl_api_t api = {0};
    api.store = &st;
    api.token = "t";
    api.fw_version = "0.0.0";
    api.layouts = &layouts;
    esl_sb_init(&out, resp, sizeof(resp));

    static char line[2 * ESL_LAYOUT_MAX_SIZE + 64];
    static uint8_t buf[ESL_LAYOUT_MAX_SIZE];
    unsigned slot = 0, contents = 0, polls = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char kind[16], a[2 * ESL_LAYOUT_MAX_SIZE + 1], b[256], c[256];
        int n = sscanf(line, "%15s %s %255s %255s", kind, a, b, c);
        if (n < 2 || kind[0] == '#')
            continue;
        uint64_t mac;
        if (strcmp(kind, "layout") == 0) {
            /* as layouts_nvs_init() in main/store_nvs.c */
            const char *h = strchr(line + 7, ' ');
            size_t len = h ? unhex(h + 1, buf, sizeof(buf)) : 0;
            CHECK(len > 0 && esl_layouts_restore(&layouts, (uint8_t)atoi(a), (const char *)buf, len));
        } else if (strcmp(kind, "content") == 0) {
            /* as restore_content(); the slot does not matter, each tag gets its own */
            esl_content_t ct;
            size_t len = unhex(a, buf, sizeof(buf));
            bool ok = esl_persist_decode(buf, len, &mac, &ct);
            CHECK(ok);
            if (!ok)
                continue;
            CHECK_INT(esl_content_id(&ct), strtoul(b, NULL, 16));
            CHECK(esl_store_restore(&st, slot++, mac, &ct));
            /* and the API says the same content_id, so the tag is not sent it again */
            char uri[40], want[32];
            snprintf(uri, sizeof(uri), "/api/tags/%016" PRIX64, mac);
            CHECK_INT(esl_api_handle(&api, "GET", uri, "Bearer t", NULL, 0, 0, &out), 200);
            snprintf(want, sizeof(want), "\"content_id\":%lu", strtoul(b, NULL, 16));
            CHECK(strstr(resp, want) != NULL);
            contents++;
        } else if (strcmp(kind, "cfg") == 0) {
            esl_tag_cfg_t cfg;
            size_t len = unhex(a, buf, sizeof(buf));
            unsigned v[5];
            int k = sscanf(line, "%*s %*s %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4]);
            bool ok = esl_persist_cfg_decode(buf, len, &mac, &cfg);
            CHECK(ok && k >= 3);
            if (!ok)
                continue;
            CHECK_INT(cfg.poll_s, v[0]);
            CHECK_INT(cfg.max_poll_s, v[1]);
            CHECK_INT(cfg.lost_ap_s, v[2]);
            if (k == 5) {
                CHECK_INT(cfg.long_poll_s, v[3]);
                CHECK_INT(cfg.long_after_min, v[4]);
            } else { /* stored before 0.9.0: the long backoff at its defaults */
                CHECK_INT(cfg.long_poll_s, 0);
                CHECK_INT(cfg.long_after_min, 0);
            }
            int s = esl_store_slot_of(&st, mac);
            CHECK(esl_store_restore_cfg(&st, s >= 0 ? (unsigned)s : slot++, mac, &cfg));
        } else if (strcmp(kind, "seen") == 0) {
            esl_seen_t seen;
            size_t len = unhex(a, buf, sizeof(buf));
            unsigned model, fw, mv;
            int rssi;
            unsigned long unix_s;
            int k = sscanf(line, "%*s %*s %u %u %u %d %lu", &model, &fw, &mv, &rssi, &unix_s);
            bool ok = esl_persist_seen_decode(buf, len, &mac, &seen);
            CHECK(ok && k == 5);
            if (!ok)
                continue;
            CHECK_INT(seen.model, model);
            CHECK_INT(seen.fw, fw);
            CHECK_INT(seen.battery_mv, mv);
            CHECK_INT(seen.rssi, rssi);
            CHECK_INT(seen.unix_s, unix_s);
            int s = esl_store_slot_of(&st, mac);
            CHECK(esl_store_restore_seen(&st, s >= 0 ? (unsigned)s : slot++, mac, &seen));
        } else if (strcmp(kind, "poll") == 0 && n == 4) {
            uint8_t rx[ESL_PSDU_MAX], want[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
            size_t rl = unhex(a, rx, sizeof(rx)), wl = unhex(b, want, sizeof(want));
            CHECK(rl > 0);
            size_t m = esl_radio_handle(&st, &layouts, PAN, rx, rl, -60, strtoull(c, NULL, 10), tx, NULL);
            check_reply(name, rx, rl, tx, m, want, wl);
            polls++;
        }
    }
    fclose(f);
    CHECK(contents > 0);
    CHECK(polls > 0);
    esl_store_free(&st);
    esl_layouts_free(&layouts);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void test_compat(void)
{
    DIR *d = opendir(COMPAT_DIR);
    CHECK(d != NULL);
    if (!d)
        return;
    char *names[64];
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == 'v' && l > 4 && strcmp(e->d_name + l - 4, ".txt") == 0)
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(names[0]), by_name);
    CHECK(n >= 1);
    for (unsigned i = 0; i < n; i++) {
        check_file(names[i]);
        free(names[i]);
    }
    if (redrawn)
        printf("\n  %u tags would be sent a new image after the update\n%10s", redrawn, "");
}
