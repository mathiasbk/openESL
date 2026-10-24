/* e2e.c - end-to-end scenarios: the real tag firmware against the real AP core, driven
 * through the API, with simulated time. The cases follow the phase 5 test matrix in
 * docs/fremdriftsplan.md (and the hardware results there), plus CONFIG, layout files and
 * T12. One tag runs through all of them in order, as one long day on the jig.
 * Every screen worth looking at is saved as PNG in the output folder. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_crc32.h"
#include "esl_render.h"
#include "proto.h"
#include "sim.h"

static int failures, checks;
static const char *outdir;
static char mac[17];
static const char *resp;

#define CHECK(cond, ...) check((cond), #cond, __LINE__, __VA_ARGS__)

static void check(int ok, const char *expr, int line, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (ok)
        return;
    failures++;
    printf("  FAIL e2e.c:%d: %s\n        ", line, expr);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n        last reply: %.300s\n", resp ? resp : "");
}

static double now_s(void) { return sim_now_ms / 1000.0; }

static void step(const char *title)
{
    printf("\n== %s (t = %.0f s)\n", title, now_s());
}

static void note(const char *fmt, ...)
{
    va_list ap;
    printf("   ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static char *read_file(const char *path, size_t *len);
static void ref_add(const char *fmt, ...);

/* The screen the tag drew against the one the AP draws for the same content (esl_render):
   they must be the same bytes. Images are at most 1024 x 256 with two planes. */
static uint8_t img_tag[1024 * 32 * 2], img_ap[1024 * 32 * 2];

static int first_diff(size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (img_tag[i] != img_ap[i])
            return (int)i;
    return -1;
}

static void png(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", outdir, name);
    CHECK(sim_tag_save_png(path), "could not write %s", path);
    note("screen -> %s", path);
    if (sim_tag_shown_id()) {
        size_t nt = sim_tag_screen(img_tag, sizeof(img_tag)), na = sim_ap_render_tag(sim_tag_mac, img_ap, sizeof(img_ap));
        CHECK(nt && nt == na && first_diff(nt) < 0, "the AP draws the same screen (%s): %zu/%zu bytes, first difference at %d",
              name, nt, na, nt == na ? first_diff(nt) : -1);
    }
}

/* Every layout of a layout file, and the built-in ones, with a few sets of fields and every
   flag combination, drawn by esl_render; the CRC of each screen goes to the reference
   (ref_check). */
static void render_compare_file(const char *path, const char *label)
{
    static const char *const sets[][6] = {
        { "Kramp", "Spann sort 12 l", "1626-160602/439", "17,25", "13,80", "x" },
        { "Kärcher Prof.", "Nedre kuleledd, kat. 3/2 som brytes", "inkl. mva 3910,36", "3128,29", "2502,63", "æø" },
        { "8713331002910", "4006381333931", "12345", "99,90", "79,92", "0" },
        { "", "a", "", "", "", "" },
        { "Åpent 9-21 ÆØÅ üé", "Kaffe filtermalt 500 g", "kr 99,80/kg", "Tilbud ut uken", "1", "2" },
        { "Lettmelk 1 L", "24,90", "kr/l 24,90", "", "", "" },
        { "Etlangtordutenmellomromsomikkefårplass", "Ja", "Nei", "123456789012", "x", "y" },
        /* a short last word that must go to the next line, and Code 128 with odd lengths */
        { "Sement grå 500 kg til mur", "Kabel 3x2,5 mm 100 m på rull", "AB-12345", "7", "1234,50", "Vi har det" },
        { "Lim til tre", "Sekk 25 kg", "abcdefghijk", "1 299,-", "Ny", "x-y-z 1 2 3" },
    };
    char err[200];
    size_t len;
    char *json = read_file(path, &len);
    int count, cases = 0, bad = 0;
    uint8_t layouts[2 + 16];
    int nl = 0;

    if (!json) {
        note("%s: not found, skipped", path);
        return;
    }
    count = sim_ap_compile(json, len, err, sizeof(err));
    free(json);
    CHECK(count > 0, "%s compiles: %s", path, err);
    layouts[nl++] = 1;
    layouts[nl++] = 2;
    for (int i = 0; i < count; i++)
        layouts[nl++] = (uint8_t)(0x10 + i);
    for (int li = 0; li < nl; li++)
        for (unsigned si = 0; si < sizeof(sets) / sizeof(sets[0]); si++)
            for (uint8_t flags = 0; flags < 4; flags++) {
                int nf = layouts[li] == 1 ? 3 : layouts[li] == 2 ? 4 : 6; /* as the API limits them */
                size_t na = sim_ap_render(layouts[li], flags, nf, sets[si], img_ap, sizeof(img_ap));
                cases++;
                ref_add("%s %02x %u %u %zu %08lx\n", label, layouts[li], si, flags, na,
                        (unsigned long)(na ? esl_crc32(img_ap, na) : 0));
                if (!na && ++bad <= 5)
                    note("NOT DRAWN %s layout 0x%02x set %u flags %u", path, layouts[li], si, flags);
            }
    CHECK(bad == 0, "%s: %d of %d screens not drawn", path, bad, cases);
    note("%s: %d layouts, %d screens drawn", path, count, cases);
}

/* The CRC-32 of every screen above, one line per screen, against sim/reference/<model>.crc.
   The file was made while the tag still drew content with its own code, and every screen
   in it was byte for byte what the tag drew. SIM_REF_UPDATE=1 writes it instead (after a
   change to the drawing that is meant to change screens; the diff shows which). */
static char *ref;
static size_t ref_len, ref_cap;

static void ref_add(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (ref_cap - ref_len < 128) {
        ref_cap = ref_cap ? ref_cap * 2 : 16384;
        ref = realloc(ref, ref_cap);
        if (!ref)
            abort();
    }
    va_start(ap, fmt);
    n = vsnprintf(ref + ref_len, ref_cap - ref_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        ref_len += (size_t)n;
}

static void ref_check(const char *root)
{
    char path[512];
    size_t len, line = 1;
    char *want;

    snprintf(path, sizeof(path), "%s/sim/reference/%s.crc", root, sim_tag_model);
    const char *upd = getenv("SIM_REF_UPDATE");
    if (upd && strcmp(upd, "1") == 0) {
        FILE *f = fopen(path, "wb");
        CHECK(f && fwrite(ref, 1, ref_len, f) == ref_len && fclose(f) == 0, "could not write %s", path);
        note("%s written", path);
        return;
    }
    want = read_file(path, &len);
    CHECK(want != NULL, "%s is missing (SIM_REF_UPDATE=1 writes it)", path);
    if (!want)
        return;
    /* a checkout with CRLF line ends (Git for Windows) holds the same lines */
    size_t k = 0;
    for (size_t i = 0; i < len; i++)
        if (want[i] != '\r')
            want[k++] = want[i];
    len = k;
    for (size_t i = 0; i < len && i < ref_len && want[i] == ref[i]; i++)
        if (want[i] == '\n')
            line++;
    CHECK(len == ref_len && memcmp(want, ref, len) == 0,
          "the screens differ from %s from line %zu (\"file layout set flags bytes crc\"); "
          "if the change is meant, SIM_REF_UPDATE=1 rewrites it", path, line);
    if (len == ref_len && memcmp(want, ref, len) == 0)
        note("%s: all %zu screens as the reference", path, line - 1);
    free(want);
}

static void render_compare(const char *root)
{
    char path[512];
    step("0. The AP draws every layout as the reference says (esl_render)");
    ref_len = 0;
    snprintf(path, sizeof(path), "%s/tag/designs/layouts/%s.json", root, sim_tag_model);
    render_compare_file(path, "designs");
    snprintf(path, sizeof(path), "%s/ap/test/layouts/%s.json", root, sim_tag_model);
    render_compare_file(path, "ap-test");
    snprintf(path, sizeof(path), "%s/sim/layouts/%s.json", root, sim_tag_model);
    render_compare_file(path, "sim");
    ref_check(root);
}

static int api(const char *method, const char *suffix, const char *body)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "/api/tags/%s%s", mac, suffix);
    return sim_api(method, uri, body, &resp);
}

/* GET /api/tags/{mac}: 200 and the fields read below */
static int get_tag(void)
{
    return api("GET", "", NULL);
}

/* PUT content; returns its content_id (0 on error) */
static uint32_t put_content(const char *json, int expect_changed)
{
    int status = api("PUT", "", json);
    CHECK(status == 200, "PUT /api/tags/%s -> %d", mac, status);
    CHECK(sim_json_bool("changed") == expect_changed, "changed should be %d", expect_changed);
    return (uint32_t)sim_json_num("content_id");
}

/* The id of the image the AP draws for the tag's content (GET /api/tags/{mac} "image_id"),
   known once the AP has drawn it for the tag; 0 before. */
static uint32_t image_id(void)
{
    get_tag();
    long id = sim_json_num("image_id");
    return id > 0 ? (uint32_t)id : 0;
}

/* The tag shows the image of its current content: the AP draws the screen and the tag
   only shows it (protokoll.md "Image transfer"), so what the tag reports is the image id. */
static bool shows_image(void)
{
    uint32_t id = image_id();
    return id && sim_tag_shown_id() == id;
}

static void run_for(double s)
{
    sim_tag_run_until(sim_now_ms + (uint64_t)(s * 1000));
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = (size_t)n;
    }
    return buf;
}

static const char PRICE_1[] = "{\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"24,90\",\"kr/l 24,90\"]}";
static const char PRICE_2[] =
    "{\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"21,90\",\"Tilbud\"],\"price_highlight\":true}";
static const char PRICE_3[] = "{\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"19,90\",\"Kampanje\"]}";
static const char PRICE_4[] = "{\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"22,90\",\"kr/l 22,90\"]}";
static const char SHELF_1[] = "{\"layout\":\"shelf\",\"fields\":[\"Kaffe filtermalt 500 g\",\"49,90\","
                              "\"kr 99,80/kg\",\"Tilbud ut uken\"]}";
static const char SHELF_2[] = "{\"layout\":\"shelf\",\"fields\":[\"Kaffe filtermalt 500 g\",\"44,90\","
                              "\"kr 89,80/kg\",\"Ny pris\"]}";
static const char TEXT_1[] = "{\"layout\":\"text\",\"fields\":[\"Velkommen\",\"Man-fre 9-21\",\"Lor 9-18\"],"
                             "\"frame\":true}";

int e2e_run(const char *out, const char *root)
{
    uint32_t cid, cid2;
    int draws;
    uint64_t t0, w;
    sim_stats_t s0;

    outdir = out;
    snprintf(mac, sizeof(mac), "%016llX", (unsigned long long)sim_tag_mac);
    printf("End-to-end simulation: tag %s (%s\"), tag firmware tag/src + AP core ap/components/esl_core\n", mac,
           sim_tag_model);

    render_compare(root);

    /* ------------------------------------------------------------------ */
    step("1. New tag, unknown to the AP");
    sim_tag_flash_erase();
    sim_ap_boot();
    sim_tag_boot();
    CHECK(sim_tag_draws() == 1, "start screen drawn once, got %d", sim_tag_draws());
    png("01-start");
    run_for(1);
    CHECK(get_tag() == 200, "the AP knows the tag after its first POLL");
    CHECK(sim_json_num("seen_s_ago") >= 0 && sim_json_num("seen_s_ago") <= 1, "seen just now");
    CHECK(sim_json_num("content_id") == 0 && sim_json_num("shown_content_id") == 0, "no content yet");
    CHECK(sim_json_bool("pending") == 0, "nothing pending");
    {
        char want[32];
        snprintf(want, sizeof(want), "\"model\":\"%s\"", sim_tag_model);
        CHECK(strstr(resp, want) != NULL, "model %s reported", sim_tag_model);
    }
    CHECK(sim_stats.idle == 1, "the AP answered IDLE");

    /* ------------------------------------------------------------------ */
    step("2. New content through the API");
    t0 = sim_now_ms;
    cid = put_content(PRICE_1, 1);
    CHECK(sim_json_bool("pending") == 1, "pending until the tag shows it");
    CHECK(sim_tag_run_until_draw(t0 + 120000), "drawn within 2 min (G1)");
    CHECK(shows_image(), "the tag shows the image of content %u, got %u", cid, sim_tag_shown_id());
    note("drawn %.1f s after the PUT (G1: at most 120 s); image %u in %u IMG_REQ", (sim_tag_last_draw_ms() - t0) / 1000.0,
         image_id(), sim_stats.img_reqs);
    get_tag();
    CHECK(sim_json_bool("pending") == 1, "the API confirms only at the next POLL");
    run_for(61);
    get_tag();
    CHECK(sim_json_num("shown_content_id") == (long)cid && sim_json_num("shown_image_id") == (long)image_id() &&
              sim_json_bool("pending") == 0,
          "confirmed one poll later");
    note("confirmed by the API %.1f s after the PUT", (sim_now_ms - t0) / 1000.0);
    png("02-price");

    /* ------------------------------------------------------------------ */
    step("3. Same content again");
    draws = sim_tag_draws();
    s0 = sim_stats;
    cid2 = put_content(PRICE_1, 0);
    CHECK(cid2 == cid, "same content_id");
    CHECK(sim_json_bool("pending") == 0, "nothing pending");
    run_for(300);
    CHECK(sim_tag_draws() == draws, "no redraw in 5 min");
    CHECK(sim_stats.image == s0.image && sim_stats.idle >= s0.idle + 4, "IDLE every poll, no image");

    /* ------------------------------------------------------------------ */
    step("4a. The first IMG block lost once: the retry in the same wake-up gets it");
    t0 = sim_now_ms;
    s0 = sim_stats;
    cid = put_content(PRICE_2, 1);
    sim_drop_reply(ESL_MSG_IMG, 1);
    CHECK(sim_tag_run_until_draw(t0 + 61000), "drawn at the next poll");
    CHECK(shows_image(), "shows the new content");
    CHECK(sim_tag_misses() == 0, "not counted as a miss");
    CHECK(sim_stats.unanswered == s0.unanswered + 1 && sim_stats.image == s0.image + 1,
          "one lost reply, then the image (a lost reply is not counted)");
    png("04a-highlight");

    step("4b. The first IMG block lost in both tries: the next wake-up gets it");
    run_for(61);
    t0 = sim_now_ms;
    cid = put_content(PRICE_3, 1);
    sim_drop_reply(ESL_MSG_IMG, 2);
    w = sim_tag_next_wake_ms();
    sim_tag_run_until(w);
    CHECK(sim_tag_misses() == 1 && !shows_image(), "a miss, old content still shown");
    CHECK(sim_tag_last_sleep_s() == 60, "no backoff after one miss, slept %u", sim_tag_last_sleep_s());
    get_tag();
    CHECK(sim_json_bool("pending") == 1, "still pending in the API");
    CHECK(sim_tag_run_until_draw(w + 61000), "drawn one interval later");
    CHECK(shows_image() && sim_tag_misses() == 0, "shows it, misses back to 0");
    note("drawn %.1f s after the PUT", (sim_tag_last_draw_ms() - t0) / 1000.0);

    step("4c. The POLL that confirms it is lost: the API waits for the next one");
    sim_drop_uplink(2);
    sim_tag_run_until(sim_tag_next_wake_ms());
    get_tag();
    CHECK(sim_json_bool("pending") == 1, "not confirmed yet");
    sim_tag_run_until(sim_tag_next_wake_ms());
    get_tag();
    CHECK(sim_json_bool("pending") == 0 && sim_json_num("shown_content_id") == (long)cid, "confirmed");

    step("4d. The AP dies while sending the image and is back after 3 min");
    t0 = sim_now_ms;
    cid = put_content(PRICE_4, 1);
    sim_die_on_drop = true;
    sim_drop_reply(ESL_MSG_IMG, 1);
    sim_tag_run_until(sim_tag_next_wake_ms());
    sim_die_on_drop = false;
    CHECK(!sim_ap_up() && sim_tag_shown_id() != cid, "the AP is off, the tag has not got it");
    run_for(180);
    sim_ap_boot();
    get_tag();
    CHECK(sim_json_num("content_id") == (long)cid, "the content survived the restart (NVS)");
    w = sim_now_ms;
    CHECK(sim_tag_run_until_draw(w + 600000), "the tag gets it after the restart");
    CHECK(shows_image(), "shows it (the AP drew it again after the restart, same id)");
    note("drawn %.1f s after the AP came back (%.1f s after the PUT)", (sim_tag_last_draw_ms() - w) / 1000.0,
         (sim_tag_last_draw_ms() - t0) / 1000.0);
    run_for(61);
    CHECK(sim_tag_last_sleep_s() == 60, "back to 60 s");

    /* ------------------------------------------------------------------ */
    step("5. Layout file over the radio, content with a named layout");
    {
        char path[512], uri[64];
        size_t len = 0;
        char *json;
        long ver;
        int status;
        snprintf(path, sizeof(path), "%s/tag/designs/layouts/%s.json", root, sim_tag_model);
        json = read_file(path, &len);
        CHECK(json != NULL, "read %s", path);
        snprintf(uri, sizeof(uri), "/api/layouts/%s", sim_tag_model);
        status = sim_api("PUT", uri, json ? json : "", &resp);
        free(json);
        CHECK(status == 200, "PUT %s -> %d", uri, status);
        ver = sim_json_num("version");
        s0 = sim_stats;
        t0 = sim_now_ms;
        cid = put_content(SHELF_1, 1);
        CHECK(sim_tag_run_until_draw(t0 + 61000), "drawn at the next poll");
        CHECK(shows_image(), "shows the shelf layout, drawn by the AP from file version %ld", ver);
        note("the AP drew the named layout; %u IMG_REQ", sim_stats.img_reqs - s0.img_reqs);
        run_for(61);
        get_tag();
        CHECK(sim_json_bool("pending") == 0, "confirmed");
        png("05-shelf");
    }

    /* ------------------------------------------------------------------ */
    step("6. Settings (CONFIG): poll_s 120, then back to the default");
    s0 = sim_stats;
    CHECK(api("PUT", "/config", "{\"poll_s\":120}") == 200, "PUT config");
    sim_tag_run_until(sim_tag_next_wake_ms());
    CHECK(sim_stats.config == s0.config + 1, "CONFIG at the next poll");
    CHECK(sim_tag_last_sleep_s() == 120, "the tag sleeps 120 s, got %u", sim_tag_last_sleep_s());
    sim_tag_run_until(sim_tag_next_wake_ms());
    get_tag();
    CHECK(sim_json_bool("config_pending") == 0 && sim_json_num("poll_s") == 120, "confirmed");
    CHECK(api("PUT", "/config", "{}") == 200, "PUT config {}");
    sim_tag_run_until(sim_tag_next_wake_ms());
    CHECK(sim_tag_last_sleep_s() == 60, "back to 60 s, got %u", sim_tag_last_sleep_s());
    run_for(61);
    get_tag();
    CHECK(sim_json_bool("config_pending") == 0, "confirmed");

    /* ------------------------------------------------------------------ */
    step("7. Tag restart with the AP off: the content comes back from flash (T12)");
    cid = sim_tag_shown_id();
    draws = sim_tag_draws();
    sim_ap_off();
    sim_tag_boot();
    CHECK(sim_tag_draws() == draws + 1 && sim_tag_shown_id() == cid, "drew the shelf again without the AP");
    png("07-restart");
    run_for(30);
    sim_ap_boot();
    draws = sim_tag_draws();
    s0 = sim_stats;
    run_for(180);
    CHECK(sim_tag_draws() == draws, "no redraw when the AP comes back");
    CHECK(sim_stats.image == s0.image, "no image");
    get_tag();
    CHECK(sim_json_bool("pending") == 0, "the API sees it as shown");

    /* ------------------------------------------------------------------ */
    step("8. The AP gone for an hour with new content waiting");
    cid = put_content(SHELF_2, 1);
    sim_ap_off();
    draws = sim_tag_draws();
    t0 = sim_now_ms;
    {
        unsigned longest = 0;
        while (sim_tag_next_wake_ms() <= t0 + 3600000) {
            sim_tag_run_until(sim_tag_next_wake_ms());
            if (sim_tag_last_sleep_s() > longest)
                longest = sim_tag_last_sleep_s();
        }
        CHECK(longest == 600, "the backoff reaches 600 s (T11), got %u", longest);
        note("%u misses in an hour, longest sleep %u s", sim_tag_misses(), longest);
    }
    run_for((double)(t0 + 3600000 - sim_now_ms) / 1000.0);
    CHECK(sim_tag_draws() == draws, "nothing drawn (\"No AP found\" only after 2 h)");
    sim_ap_boot();
    w = sim_now_ms;
    CHECK(sim_tag_run_until_draw(w + 601000), "found the AP within 600 s");
    CHECK(shows_image(), "shows the new content");
    note("drawn %.1f s after the AP came back", (sim_tag_last_draw_ms() - w) / 1000.0);
    CHECK(sim_tag_last_sleep_s() == 60, "back to 60 s, got %u", sim_tag_last_sleep_s());
    png("08-after-hour");

    /* ------------------------------------------------------------------ */
    step("9. The AP gone for more than 2 h: \"No AP found\" (T9), then back");
    run_for(61);
    sim_ap_off();
    t0 = sim_now_ms;
    CHECK(sim_tag_run_until_draw(t0 + 7200000 + 601000), "drew the no-AP screen");
    CHECK(sim_tag_shown_id() == 0, "no content id while showing it");
    /* 2 h after the last answer, which came at most one interval (60 s) before t0 */
    CHECK(sim_tag_last_draw_ms() >= t0 + 7140000 && sim_tag_last_draw_ms() <= t0 + 7201000,
          "\"No AP found\" 2 h after the last answer (T9), got %.0f s after the AP went",
          (sim_tag_last_draw_ms() - t0) / 1000.0);
    note("\"No AP found\" %.0f s after the AP went", (sim_tag_last_draw_ms() - t0) / 1000.0);
    png("09-no-ap");
    s0 = sim_stats;
    sim_ap_boot();
    w = sim_now_ms;
    CHECK(sim_tag_run_until_draw(w + 601000), "content again when the AP is back");
    CHECK(shows_image(), "shows the image of %u again, got %u", cid, sim_tag_shown_id());
    CHECK(sim_stats.img_reqs == s0.img_reqs, "drawn from the tag's own flash, no download");
    note("drawn %.1f s after the AP came back", (sim_tag_last_draw_ms() - w) / 1000.0);

    /* ------------------------------------------------------------------ */
    step("10. Tag deleted on the AP (unknown tag)");
    run_for(61);
    CHECK(api("DELETE", "", NULL) == 200, "DELETE");
    draws = sim_tag_draws();
    cid = sim_tag_shown_id();
    run_for(121);
    CHECK(get_tag() == 200 && sim_json_num("content_id") == 0, "registered again, with no content");
    CHECK(sim_tag_draws() == draws && sim_tag_shown_id() == cid, "the tag keeps what it shows");
    t0 = sim_now_ms;
    cid = put_content(TEXT_1, 1);
    CHECK(sim_tag_run_until_draw(t0 + 61000), "a new PUT reaches it");
    CHECK(shows_image(), "shows it");
    run_for(61);
    get_tag();
    CHECK(sim_json_bool("pending") == 0, "confirmed");
    png("final");

    /* ------------------------------------------------------------------ */
    step("11. A price from a set time, with a 5-minute interval");
    CHECK(sim_api("PUT", "/api/config", "{\"poll_s\":300}", &resp) == 200, "PUT /api/config poll_s 300");
    run_for(61);
    run_for(301);
    CHECK(sim_tag_last_sleep_s() == 300, "the tag polls every 300 s, got %u", sim_tag_last_sleep_s());
    CHECK(sim_api("PUT", "/api/time", "{\"time\":1792065600}", &resp) == 200, "PUT /api/time");
    {
        /* more than one interval ahead, so the tag polls once before it is due */
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"17,90\",\"Kampanje\"],\"at\":%u}",
                 1792065600u + 400);
        CHECK(api("PUT", "", body) == 200 && sim_json_bool("scheduled") == 1, "scheduled for 400 s from now");
    }
    t0 = sim_now_ms;
    CHECK(sim_tag_run_until_draw(t0 + 700000), "drew the scheduled price");
    CHECK(sim_tag_last_draw_ms() >= t0 + 400000 && sim_tag_last_draw_ms() <= t0 + 410000,
          "drawn just after it was due, not at the next 300 s poll: got %.1f s", (sim_tag_last_draw_ms() - t0) / 1000.0);
    note("due after 400 s, drawn after %.1f s", (sim_tag_last_draw_ms() - t0) / 1000.0);
    run_for(1);
    CHECK(sim_tag_last_sleep_s() == 300, "back to 300 s, got %u", sim_tag_last_sleep_s());
    png("11-scheduled");

    /* ------------------------------------------------------------------ */
    step("12. New firmware over the air (T15): download, install, restart, confirm");
    {
        /* A made-up 4 KB image as build.py packs it; the "tag" only copies it */
        static uint8_t file[ESL_FWF_HDR_LEN + 4096];
        uint8_t *img = file + ESL_FWF_HDR_LEN;
        uint16_t ver = (uint16_t)(sim_tag_fw_version() + 1);
        uint32_t crc = 0xFFFFFFFFu;
        char uri[96];
        for (int i = 0; i < 4096; i++) {
            img[i] = (uint8_t)(i * 31 + 7);
            crc ^= img[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        crc = ~crc;
        ESL_PUT_U32(file + ESL_FWF_OFF_MAGIC, ESL_FWF_MAGIC);
        file[ESL_FWF_OFF_FORMAT] = ESL_FWF_FORMAT;
        file[ESL_FWF_OFF_MODEL] = sim_tag_model_code();
        file[ESL_FWF_OFF_VER] = (uint8_t)ver;
        file[ESL_FWF_OFF_VER + 1] = (uint8_t)(ver >> 8);
        ESL_PUT_U32(file + ESL_FWF_OFF_SIZE, 4096u);
        ESL_PUT_U32(file + ESL_FWF_OFF_CRC, crc);
        /* in two parts, as esl-client does with bigger images */
        snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=0&total=%u", (unsigned)sizeof(file));
        CHECK(sim_api_bin("PUT", uri, file, 2000, &resp) == 200, "first part");
        snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=2000&total=%u", (unsigned)sizeof(file));
        CHECK(sim_api_bin("PUT", uri, file + 2000, sizeof(file) - 2000, &resp) == 200, "last part");
        note("uploaded %u bytes: %.120s", (unsigned)sizeof(file), resp);

        uint32_t shown = sim_tag_shown_id();
        int restarts = sim_tag_restarts();
        run_for(301);
        CHECK(sim_tag_restarts() == restarts + 1, "the tag downloaded the image and restarted (%d)", sim_tag_restarts());
        CHECK(sim_tag_app_is(img, 4096), "the bootloader copied the image into the chip");
        CHECK(sim_tag_shown_id() == shown, "the price is back on the screen after the restart (T12)");
        CHECK(sim_tag_ota_confirmed(), "the new image talked to the AP and is kept");
        note("restarted at %.0f s, confirmed", now_s());
        /* The simulated tag still reports its old version: a mismatch with the image's,
           so it reports the update as failed and the AP stops offering it */
        restarts = sim_tag_restarts();
        run_for(601);
        CHECK(sim_tag_restarts() == restarts, "not taken again");
        CHECK(sim_api("GET", "/api/tag-firmware", NULL, &resp) == 200 && strstr(resp, "\"failed\":1"),
              "the API counts it as failed");
        snprintf(uri, sizeof(uri), "/api/tag-firmware/%s", sim_tag_model);
        CHECK(sim_api("DELETE", uri, NULL, &resp) == 200, "DELETE the image");
        restarts = sim_tag_restarts();
        run_for(601);
        CHECK(sim_tag_restarts() == restarts, "no more updates");
        CHECK(sim_tag_last_sleep_s() == 300, "polls every 300 s again, got %u", sim_tag_last_sleep_s());
    }

    /* ------------------------------------------------------------------ */
    step("13. A picture from a client (PUT /api/tags/{mac}/image)");
    {
        uint16_t w, h;
        bool red;
        uint8_t *pix, *file;
        size_t len = 0, n;
        char path[512];
        esl_render_model(sim_tag_model_code(), &w, &h, &red);
        /* a ring and a bar code-like stripe, black on white, as a client would draw them */
        pix = malloc((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int dx = x - w / 3, dy = y - h / 2, r2 = dx * dx + dy * dy, r = h / 3;
                bool ink = (r2 < r * r && r2 > (r - 8) * (r - 8)) || (x > w * 2 / 3 && (x / 3) % 2 && y > 10 && y < h - 10);
                pix[y * w + x] = ink ? 0 : 255;
            }
        snprintf(path, sizeof(path), "%s/13-picture-sent.png", outdir);
        CHECK(png_write_gray(path, pix, w, h), "write %s", path);
        file = (uint8_t *)read_file(path, &len);
        CHECK(file != NULL, "read %s", path);
        t0 = sim_now_ms;
        s0 = sim_stats;
        CHECK(api("PUT", "/image", "x") == 415, "not a PNG: 415");
        char uri[96];
        snprintf(uri, sizeof(uri), "/api/tags/%s/image", mac);
        CHECK(sim_api_bin("PUT", uri, file, len, &resp) == 200 && sim_json_bool("changed") == 1, "PUT the PNG: %.100s", resp);
        cid = (uint32_t)sim_json_num("content_id");
        get_tag();
        CHECK(strstr(resp, "\"layout\":\"image\",\"fields\":[]") != NULL, "the API shows it as a picture");
        CHECK(sim_tag_run_until_draw(t0 + 320000), "the tag draws it at its next poll");
        CHECK(shows_image(), "the tag shows the picture");
        note("drawn %.1f s after the PUT, %u IMG_REQ", (sim_tag_last_draw_ms() - t0) / 1000.0,
             sim_stats.img_reqs - s0.img_reqs);
        png("13-picture");
        /* pixel for pixel what was sent */
        n = sim_tag_screen(img_tag, sizeof(img_tag));
        size_t cb = ESL_RENDER_COL_BYTES(h), wrong = 0;
        for (int y = 0; y < h && n; y++)
            for (int x = 0; x < w; x++)
                wrong += !(img_tag[x * cb + y / 8] & (0x80 >> (y % 8))) != (pix[y * w + x] != 0);
        CHECK(n && wrong == 0, "the tag's screen is the picture (%zu pixels differ)", wrong);
        /* a restart of the AP keeps it, and the tag is not sent it again */
        draws = sim_tag_draws();
        sim_ap_boot();
        run_for(601);
        CHECK(sim_tag_draws() == draws && shows_image(), "kept over an AP restart, not sent again");
        get_tag();
        CHECK(sim_json_num("content_id") == (long)cid && sim_json_bool("pending") == 0, "confirmed");
        /* text again replaces it */
        t0 = sim_now_ms;
        put_content(PRICE_1, 1);
        CHECK(sim_tag_run_until_draw(t0 + 320000) && shows_image(), "a price replaces the picture");
        free(file);
        free(pix);
    }

    printf("\n%d checks, %d failed. Simulated time: %.1f h\n", checks, failures, now_s() / 3600.0);
    return failures ? 1 : 0;
}
