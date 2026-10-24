/* sim_ap.c - the AP core (ap/components/esl_core) as the C6 runs it, with "NVS" in RAM
 * (the same blob formats as ap/main/store_nvs.c) so the AP can be switched off and
 * restarted, and the radio link to the simulated tag with optional losses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_api.h"
#include "esl_image.h"
#include "esl_persist.h"
#include "esl_radio.h"
#include "esl_render.h"
#include "esl_text.h"
#include "esl_sched.h"
#include "sim.h"

#define CAP 8

static esl_store_t st;
static esl_layouts_t layouts;
static esl_image_t images;
static uint8_t *image_mem;
static esl_sched_entry_t sched_entries[CAP];
static esl_sched_t sched;
static bool ap_up;
static esl_tagfw_t tagfw;
static uint8_t tagfw_flash[ESL_MODEL_COUNT - 1][ESL_TAGFW_SLOT];   /* the "tagfw" partition, kept over restarts */
static uint64_t ap_boot_ms;

sim_stats_t sim_stats;

/* ---- "NVS" ---- */

static struct {
    uint8_t content[CAP][ESL_PERSIST_BLOB_MAX];
    size_t content_len[CAP];
    uint8_t cfg[CAP][ESL_PERSIST_CFG_LEN];
    size_t cfg_len[CAP];
    bool has_poll_s;
    uint16_t poll_s;
    char layout[ESL_MODEL_COUNT][ESL_LAYOUT_MAX_SIZE];
    size_t layout_len[ESL_MODEL_COUNT];
    uint8_t sched[CAP][ESL_SCHED_BLOB_MAX];
    size_t sched_len[CAP];
    uint8_t image[CAP][16384];   /* pictures from PUT /api/tags/{mac}/image */
    size_t image_len[CAP];
} nvs;

static bool nvs_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    (void)ctx;
    nvs.content_len[slot] = esl_persist_encode(mac, c, nvs.content[slot]);
    return true;
}

/* As store_nvs.c: the store keeps no content in RAM */
static bool nvs_load(void *ctx, unsigned slot, uint64_t mac, esl_content_t *c)
{
    uint64_t m;
    (void)ctx;
    return nvs.content_len[slot] && esl_persist_decode(nvs.content[slot], nvs.content_len[slot], &m, c) && m == mac;
}

static void nvs_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    nvs.content_len[slot] = 0;
}

static bool nvs_image(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (len > sizeof(nvs.image[slot]))
        return false;
    if (data)
        memcpy(nvs.image[slot], data, len);
    nvs.image_len[slot] = data ? len : 0;
    return true;
}

static size_t nvs_load_image(void *ctx, unsigned slot, uint8_t *buf, size_t cap)
{
    (void)ctx;
    if (!nvs.image_len[slot] || nvs.image_len[slot] > cap)
        return 0;
    memcpy(buf, nvs.image[slot], nvs.image_len[slot]);
    return nvs.image_len[slot];
}

static void nvs_cfg(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    (void)ctx;
    nvs.cfg_len[slot] = cfg ? esl_persist_cfg_encode(mac, cfg, nvs.cfg[slot]) : 0;
}

static void nvs_next_poll(void *ctx, const uint16_t *next_poll_s)
{
    (void)ctx;
    nvs.has_poll_s = next_poll_s != NULL;
    nvs.poll_s = next_poll_s ? *next_poll_s : 0;
}

static void nvs_layout_save(void *ctx, uint8_t model, const char *data, size_t len)
{
    (void)ctx;
    memcpy(nvs.layout[model], data, len);
    nvs.layout_len[model] = len;
}

static void nvs_layout_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    nvs.layout_len[model] = 0;
}

static void nvs_sched_save(void *ctx, unsigned slot, const uint8_t *blob, size_t len)
{
    (void)ctx;
    memcpy(nvs.sched[slot], blob, len);
    nvs.sched_len[slot] = len;
}

static bool nvs_sched_load(void *ctx, unsigned slot, uint8_t *blob, size_t *len)
{
    (void)ctx;
    if (!nvs.sched_len[slot] || nvs.sched_len[slot] > *len)
        return false;
    memcpy(blob, nvs.sched[slot], nvs.sched_len[slot]);
    *len = nvs.sched_len[slot];
    return true;
}

static void nvs_sched_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    nvs.sched_len[slot] = 0;
}

/* ---- the tag firmware store (the "tagfw" partition) ---- */

static bool fw_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    memset(tagfw_flash[model - 1], 0xFF, ESL_TAGFW_SLOT);
    return true;
}

static bool fw_write(void *ctx, uint8_t model, uint32_t off, const void *data, size_t len)
{
    (void)ctx;
    if (off + len > ESL_TAGFW_SLOT)
        return false;
    memcpy(tagfw_flash[model - 1] + off, data, len);
    return true;
}

static bool fw_read(void *ctx, uint8_t model, uint32_t off, void *data, size_t len)
{
    (void)ctx;
    if (off + len > ESL_TAGFW_SLOT)
        return false;
    memcpy(data, tagfw_flash[model - 1] + off, len);
    return true;
}

void sim_ap_boot(void)
{
    static const esl_tagfw_ops_t fops = {NULL, NULL, NULL, fw_erase, fw_write, fw_read, NULL, NULL};
    static const esl_sched_ops_t sops = {NULL, NULL, NULL, nvs_sched_save, nvs_sched_load, nvs_sched_erase};
    static const esl_store_ops_t ops = {NULL, NULL, NULL, nvs_save, nvs_erase, nvs_cfg, nvs_next_poll, NULL, NULL, nvs_load, NULL, NULL, NULL,
                                        nvs_image, nvs_load_image};
    static const esl_layout_ops_t lops = {NULL, NULL, NULL, nvs_layout_save, nvs_layout_erase, NULL};
    static bool first = true;

    if (first) {
        memset(&nvs, 0, sizeof(nvs));
        memset(tagfw_flash, 0xFF, sizeof(tagfw_flash));
    }
    first = false;
    esl_store_free(&st);
    esl_store_init(&st, CAP, &ops, 0);   /* CONFIG_ESL_NEXT_POLL_S default: the tag keeps its own */
    esl_layouts_init(&layouts, &lops);
    /* as store_nvs_init() and layouts_nvs_init() */
    if (nvs.has_poll_s)
        esl_store_restore_next_poll(&st, nvs.poll_s);
    for (unsigned slot = 0; slot < CAP; slot++) {
        uint64_t mac;
        esl_content_t c;
        esl_tag_cfg_t cfg;
        if (nvs.content_len[slot] && esl_persist_decode(nvs.content[slot], nvs.content_len[slot], &mac, &c))
            esl_store_restore(&st, slot, mac, &c);
        if (nvs.cfg_len[slot] && esl_persist_cfg_decode(nvs.cfg[slot], nvs.cfg_len[slot], &mac, &cfg))
            esl_store_restore_cfg(&st, slot, mac, &cfg);
    }
    esl_sched_init(&sched, sched_entries, CAP, &sops);   /* as sched_start(): the time is unknown again */
    for (unsigned slot = 0; slot < CAP; slot++)
        if (nvs.sched_len[slot])
            esl_sched_restore(&sched, slot, nvs.sched[slot], nvs.sched_len[slot]);
    for (uint8_t model = 1; model < ESL_MODEL_COUNT; model++)
        if (nvs.layout_len[model])
            esl_layouts_restore(&layouts, model, nvs.layout[model], nvs.layout_len[model]);
    esl_tagfw_init(&tagfw, &fops);
    esl_radio_set_tagfw(&tagfw);
    /* as app_main: the image buffers from the heap, empty after a restart */
    if (!image_mem)
        image_mem = malloc(esl_image_mem_size());
    esl_image_init(&images, image_mem, NULL);
    esl_radio_set_image(&images);
    ap_boot_ms = sim_now_ms;
    ap_up = true;
    if (sim_verbose)
        printf("  [%7.1f s] AP started\n", sim_now_ms / 1000.0);
}

/* ---- drawing (esl_render) ---- */

static esl_layout_bin_t cmp_bin;
static bool cmp_has_file;
uint8_t sim_tag_model_code(void);

int sim_ap_compile(const char *json, size_t len, char *err, size_t errlen)
{
    cmp_has_file = esl_layout_compile(json, len, sim_tag_model_code(), &cmp_bin, err, errlen);
    return cmp_has_file ? cmp_bin.count : 0;
}

static bool content_from(esl_content_t *c, uint8_t layout, uint8_t flags, int n, const char *const *fields)
{
    uint8_t latin[256];
    if (esl_content_init(c, layout, flags) != ESL_CONTENT_OK)
        return false;
    for (int i = 0; i < n; i++) {
        size_t l = esl_utf8_to_latin1(fields[i], strlen(fields[i]), latin, sizeof(latin), NULL);
        if (esl_content_add_field(c, latin, l) != ESL_CONTENT_OK)
            return false;
    }
    esl_content_trim(c);
    return true;
}

size_t sim_ap_render(uint8_t layout, uint8_t flags, int n, const char *const *fields, uint8_t *out, size_t cap)
{
    esl_content_t c;
    if (!content_from(&c, layout, flags, n, fields))
        return 0;
    return esl_render(sim_tag_model_code(), cmp_has_file ? &cmp_bin : NULL, &c, out, cap);
}

size_t sim_ap_render_tag(uint64_t mac, uint8_t *out, size_t cap)
{
    esl_tag_t t;
    esl_content_t c;
    const esl_layout_t *f;
    if (!esl_store_get(&st, mac, &t) || !t.has_content || t.model == 0 || t.model >= ESL_MODEL_COUNT ||
        !esl_store_get_content(&st, mac, &c))
        return 0;
    if (c.layout == ESL_LAYOUT_IMAGE) /* a picture: as the AP keeps it */
        return esl_image_screen(&st, &layouts, mac, t.model, &c, out, cap);
    f = layouts.models[t.model - 1];
    return esl_render(t.model, f ? &f->bin : NULL, &c, out, cap);
}

void sim_ap_off(void)
{
    ap_up = false;
    if (sim_verbose)
        printf("  [%7.1f s] AP off\n", sim_now_ms / 1000.0);
}

bool sim_ap_up(void) { return ap_up; }

static uint64_t ap_now_ms(void)
{
    return sim_now_ms - ap_boot_ms;
}

/* ---- API ---- */

static char resp[ESL_API_RESP_SIZE(CAP)];

int sim_api(const char *method, const char *uri, const char *body, const char **out_body)
{
    return sim_api_bin(method, uri, body, body ? strlen(body) : 0, out_body);
}

int sim_api_bin(const char *method, const char *uri, const void *body, size_t len, const char **out_body)
{
    esl_api_t api = {&st, SIM_TOKEN, "sim", ESL_DEFAULT_CHANNEL, &layouts, "poweron", NULL, &sched, NULL, NULL, &tagfw, false, NULL};
    esl_sb_t out;
    int status;

    esl_sb_init(&out, resp, sizeof(resp));
    if (!ap_up) {
        esl_sb_puts(&out, "{\"error\":\"AP is off\"}");
        status = 503;
    } else {
        esl_sched_tick(&sched, &st, ap_now_ms());
        status = esl_api_handle(&api, method, uri, "Bearer " SIM_TOKEN, body, len, ap_now_ms(), &out);
    }
    if (out_body)
        *out_body = resp;
    return status;
}

static int json_find(const char *key, esl_jtok_t *tok, unsigned max)
{
    if (esl_json_parse(resp, strlen(resp), tok, max) < 0)
        return -1;
    return esl_json_obj_get(resp, tok, 0, key);
}

long sim_json_num(const char *key)
{
    static esl_jtok_t tok[512];
    int i = json_find(key, tok, 512);
    if (i < 0 || tok[i].type != ESL_JSON_NUMBER)
        return -1;
    return strtol(resp + tok[i].start, NULL, 10);
}

int sim_json_bool(const char *key)
{
    static esl_jtok_t tok[512];
    int i = json_find(key, tok, 512);
    if (i < 0)
        return -1;
    return tok[i].type == ESL_JSON_TRUE ? 1 : tok[i].type == ESL_JSON_FALSE ? 0 : -1;
}

/* ---- radio link ---- */

static int drop_up;
static int drop_reply;
static uint8_t drop_type;
bool sim_die_on_drop;

void sim_drop_uplink(int n) { drop_up = n; }

void sim_drop_reply(uint8_t msg_type, int n)
{
    drop_type = msg_type;
    drop_reply = n;
}

static const char *msg_name(uint8_t t)
{
    switch (t) {
    case ESL_MSG_POLL: return "POLL";
    case ESL_MSG_IDLE: return "IDLE";
    case ESL_MSG_CONFIG: return "CONFIG";
    case ESL_MSG_FW: return "FW";
    case ESL_MSG_FW_REQ: return "FW_REQ";
    case ESL_MSG_IMG: return "IMG";
    case ESL_MSG_IMG_REQ: return "IMG_REQ";
    default: return "?";
    }
}

uint8_t sim_link(const uint8_t *frame, uint8_t len, uint8_t *reply)
{
    uint8_t psdu[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    esl_radio_event_t ev;
    size_t n;

    if (len + ESL_FCS_LEN > ESL_PSDU_MAX)
        return 0;
    if (!ap_up || drop_up > 0) {
        if (ap_up)
            drop_up--;
        sim_stats.unanswered++;
        if (sim_verbose)
            printf("  [%7.1f s] tag -> %s: %s\n", sim_now_ms / 1000.0,
                   msg_name(len > ESL_MHR_LEN ? frame[ESL_MHR_LEN] : 0), ap_up ? "lost" : "no AP");
        return 0;
    }
    memcpy(psdu, frame, len);
    psdu[len] = psdu[len + 1] = 0;   /* FCS: checked by the radio hardware, not by esl_radio_handle */
    memset(&ev, 0, sizeof(ev));
    /* the AP's sched task ticks once a second; here only time between frames matters */
    esl_sched_tick(&sched, &st, ap_now_ms());
    n = esl_radio_handle(&st, &layouts, ESL_DEFAULT_PAN_ID, psdu, len + ESL_FCS_LEN, -55, ap_now_ms(), tx, &ev);
    if (ev.msg_type == ESL_MSG_POLL)
        sim_stats.polls++;
    else if (ev.msg_type == ESL_MSG_IMG_REQ)
        sim_stats.img_reqs++;
    if (n <= ESL_FCS_LEN) {
        sim_stats.unanswered++;
        return 0;
    }
    if (drop_reply > 0 && (!drop_type || drop_type == ev.reply_type)) {
        drop_reply--;
        sim_stats.unanswered++;
        if (sim_die_on_drop)
            sim_ap_off();
        if (sim_verbose)
            printf("  [%7.1f s] tag -> %s, AP -> %s: reply lost\n", sim_now_ms / 1000.0, msg_name(ev.msg_type),
                   msg_name(ev.reply_type));
        return 0;
    }
    switch (ev.reply_type) {
    case ESL_MSG_IDLE: sim_stats.idle++; break;
    case ESL_MSG_CONFIG: sim_stats.config++; break;
    case ESL_MSG_IMG: if (ev.msg_type == ESL_MSG_POLL) sim_stats.image++; break;
    }
    if (sim_verbose)
        printf("  [%7.1f s] tag -> %s, AP -> %s\n", sim_now_ms / 1000.0, msg_name(ev.msg_type),
               msg_name(ev.reply_type));
    n -= ESL_FCS_LEN;
    memcpy(reply, tx, n);
    return (uint8_t)n;
}
