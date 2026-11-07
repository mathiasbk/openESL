/* upgrade.c - "esl-sim upgrade": tag firmware over the air from one firmware to another, the
 * way a tag in the field gets it (#307). The AP is today's core, here; the tag runs in two
 * programs built by upgrade.py (esl-sim-tag, tagstep.c): first FROM, whose bootloader
 * installs the update and saves what the chip keeps, then TO, which starts from that.
 * FROM is a release and TO today's firmware, or the other way round (going back).
 *
 *   esl-sim upgrade FROM TO TO_VERSION OUTDIR [same-store]
 *     FROM, TO     esl-sim-tag programs of the same model
 *     TO_VERSION   the version TO reports (major * 10000 + minor * 100 + patch), which the
 *                  update file carries
 *     OUTDIR       screens before, after and with new content, as PNG
 *     same-store   both keep their settings in the same record format (STORE_FORMAT in
 *                  tag/src/app.c), so TO must start with FROM's settings and need no CONFIG.
 *                  Without it, a record TO cannot read may cost the one CONFIG app.c allows.
 *
 * Checked: today's AP gives FROM the update; FROM's bootloader puts it into the chip; TO
 * starts from FROM's flash on the AP's channel with the same screen and its settings, confirms the update (the
 * AP counts the tag as updated, not failed), is not sent its image again, and draws new
 * content as the AP draws it. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "proto.h"
#include "sim.h"
#include "tagstep.h"

static int failures, checks;
static const char *resp;
static char mac[17];
static int tag_fd = -1;
static pid_t tag_pid;
static tagstep_status_t st;
static bool installed;   /* the tag program sent 'X' */

#define CHECK(cond, ...) check((cond), #cond, __LINE__, __VA_ARGS__)

static void check(int ok, const char *expr, int line, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (ok)
        return;
    failures++;
    printf("  FAIL upgrade.c:%d: %s\n        ", line, expr);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n        last reply: %.300s\n", resp ? resp : "");
}

static void step(const char *title)
{
    printf("[%8.1f s] %s\n", sim_now_ms / 1000.0, title);
}

static bool read_all(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(tag_fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void write_all(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(tag_fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            printf("  FAIL: the tag program is gone\n");
            exit(1);
        }
        p += w;
        n -= (size_t)w;
    }
}

/* Answers the tag's frames until its status comes ('S', or 'X' when it installed an update) */
static void wait_status(void)
{
    for (;;) {
        uint8_t kind, hdr[10], frame[256], reply[256], n;
        uint64_t t;
        if (!read_all(&kind, 1)) {
            printf("  FAIL: the tag program stopped\n");
            exit(1);
        }
        if (kind == 'S' || kind == 'X') {
            if (!read_all(&st, sizeof(st)))
                exit(1);
            if (st.now_ms > sim_now_ms)
                sim_now_ms = st.now_ms;
            installed |= kind == 'X';
            return;
        }
        if (kind != 'F' || !read_all(hdr, sizeof(hdr)) || !read_all(frame, hdr[9])) {
            printf("  FAIL: the tag program sent %d\n", kind);
            exit(1);
        }
        memcpy(&t, hdr, 8);
        if (t > sim_now_ms)
            sim_now_ms = t;
        n = sim_link(hdr[8], frame, hdr[9], reply);
        write_all(&n, 1);
        write_all(reply, n);
    }
}

static void tag_start(const char *prog, const char *state_in, const char *state_out)
{
    int sv[2];
    installed = false;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        perror("socketpair");
        exit(1);
    }
    fflush(stdout);
    tag_pid = fork();
    if (tag_pid < 0) {
        perror("fork");
        exit(1);
    }
    if (tag_pid == 0) {
        const char *argv[8] = {prog, "--fd", "3"};
        int n = 3;
        close(sv[0]);
        if (sv[1] != 3) {
            dup2(sv[1], 3);
            close(sv[1]);
        }
        if (state_in) {
            argv[n++] = "--state-in";
            argv[n++] = state_in;
        }
        if (state_out) {
            argv[n++] = "--state-out";
            argv[n++] = state_out;
        }
        execv(prog, (char *const *)argv);
        perror(prog);
        _exit(127);
    }
    close(sv[1]);
    tag_fd = sv[0];
    wait_status();   /* booted */
}

static void tag_stop(void)
{
    if (!installed) {
        char q = 'Q';
        write_all(&q, 1);
    }
    close(tag_fd);
    tag_fd = -1;
    int ws;
    waitpid(tag_pid, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "the tag program ended well (%d)", ws);
}

static void tag_cmd_t(char cmd, uint64_t t)
{
    write_all(&cmd, 1);
    write_all(&t, 8);
    wait_status();
}

/* Runs the tag for s simulated seconds (it stops early when it installs an update) */
static void run_for(double s)
{
    tag_cmd_t('R', sim_now_ms + (uint64_t)(s * 1000));
}

static bool run_until_draw(double s)
{
    int32_t draws = st.draws;
    tag_cmd_t('D', sim_now_ms + (uint64_t)(s * 1000));
    return st.draws != draws;
}

static void tag_png(const char *dir, const char *name)
{
    char path[512];
    uint16_t n = (uint16_t)snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    char cmd = 'P';
    write_all(&cmd, 1);
    write_all(&n, 2);
    write_all(path, n);
    wait_status();
}

static void tag_expect_app(const uint8_t *img, uint32_t n)
{
    char cmd = 'A';
    write_all(&cmd, 1);
    write_all(&n, 4);
    write_all(img, n);
    wait_status();
}

/* The tag's screen is what the AP draws for its content */
static bool screen_matches(void)
{
    static uint8_t tag[64 * 1024], ap[64 * 1024];
    uint32_t n;
    char cmd = 'G';
    write_all(&cmd, 1);
    wait_status();
    if (!read_all(&n, 4) || n > sizeof(tag) || !read_all(tag, n))
        exit(1);
    size_t m = sim_ap_render_tag(strtoull(mac, NULL, 16), ap, sizeof(ap));
    return n && m == n && !memcmp(tag, ap, n);
}

static int api(const char *method, const char *suffix, const char *body)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "/api/tags/%s%s", mac, suffix);
    return sim_api(method, uri, body, &resp);
}

/* The tag shows the image of its content (GET /api/tags/{mac} "image_id") */
static bool shows_content(void)
{
    long id = api("GET", "", NULL) == 200 ? sim_json_num("image_id") : -1;
    return id > 0 && st.shown_id == (uint32_t)id;
}

/* A count of GET /api/tag-firmware for the tag's model ("updated", "failed", ...) */
static long fw_count(const char *model, const char *key)
{
    char want[32], k[24];
    if (sim_api("GET", "/api/tag-firmware", NULL, &resp) != 200)
        return -1;
    snprintf(want, sizeof(want), "\"model\":\"%s\"", model);
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(resp, want), *q = p ? strstr(p, k) : NULL, *end = p ? strstr(p + 1, "\"model\":") : NULL;
    if (!q || (end && q > end))
        return -1;
    return strtol(q + strlen(k), NULL, 10);
}

static const char PRICE_1[] = "{\"layout\":\"price\",\"fields\":[\"Milk 1 L\",\"24.90\",\"per l 24.90\"]}";
static const char PRICE_2[] = "{\"layout\":\"price\",\"fields\":[\"Milk 1 L\",\"19.90\",\"Offer\"]}";

int upgrade_run(int argc, char **argv)
{
    if (argc != 6 && !(argc == 7 && !strcmp(argv[6], "same-store"))) {
        fprintf(stderr, "usage: %s upgrade FROM TO TO_VERSION OUTDIR [same-store]\n", argv[0]);
        return 2;
    }
    bool same_store = argc == 7;
    const char *from = argv[2], *to = argv[3], *out = argv[5];
    uint16_t to_ver = (uint16_t)atoi(argv[4]);
    char state[512];
    snprintf(state, sizeof(state), "%s/chip.state", out);
    snprintf(mac, sizeof(mac), "%016llX", (unsigned long long)sim_tag_mac);
    sim_verbose = getenv("SIM_VERBOSE") != NULL;

    step("1. A new tag on the old firmware, today's AP");
    sim_ap_boot();
    tag_start(from, NULL, state);
    uint16_t from_ver = st.fw_version;
    uint8_t model = st.model_code;
    char model_name[16];
    run_for(130);
    CHECK(api("GET", "", NULL) == 200 && sim_json_num("fw") == from_ver, "registered with firmware %u", from_ver);
    const char *p = strstr(resp, "\"model\":\"");
    CHECK(p != NULL, "the AP knows its model");
    snprintf(model_name, sizeof(model_name), "%.*s", p ? (int)strcspn(p + 9, "\"") : 0, p ? p + 9 : "");
    printf("  model %s, firmware %u -> %u\n", model_name, from_ver, to_ver);
    CHECK(from_ver != to_ver, "the versions differ, or the tag would not take it");

    step("2. Content and its own settings (poll_s 120)");
    CHECK(api("PUT", "/config", "{\"poll_s\":120}") == 200, "PUT config");
    CHECK(api("PUT", "", PRICE_1) == 200, "PUT content");
    CHECK(run_until_draw(320), "drawn");
    run_for(250);
    CHECK(shows_content(), "shows its content");
    CHECK(api("GET", "", NULL) == 200 && sim_json_bool("pending") == 0 && sim_json_bool("config_pending") == 0,
          "content and settings confirmed");
    CHECK(st.last_sleep_s == 120, "sleeps 120 s, got %u", st.last_sleep_s);
    CHECK(screen_matches(), "the screen is what the AP draws");
    tag_png(out, "1-before");
    uint32_t shown = st.shown_id;

    step("3. Firmware over the air (PUT /api/tag-firmware): the old bootloader installs it");
    /* A made-up 4 KB application as build.py packs it: the simulated chip only keeps it, the
       next program runs TO in its place */
    static uint8_t file[ESL_FWF_HDR_LEN + 4096];
    uint8_t *img = file + ESL_FWF_HDR_LEN;
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 4096; i++) {
        img[i] = (uint8_t)(i * 13 + to_ver);
        crc ^= img[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    ESL_PUT_U32(file + ESL_FWF_OFF_MAGIC, ESL_FWF_MAGIC);
    file[ESL_FWF_OFF_FORMAT] = ESL_FWF_FORMAT;
    file[ESL_FWF_OFF_MODEL] = model;
    file[ESL_FWF_OFF_VER] = (uint8_t)to_ver;
    file[ESL_FWF_OFF_VER + 1] = (uint8_t)(to_ver >> 8);
    ESL_PUT_U32(file + ESL_FWF_OFF_SIZE, 4096u);
    ESL_PUT_U32(file + ESL_FWF_OFF_CRC, crc);
    char uri[96];
    snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=0&total=%u", (unsigned)sizeof(file));
    CHECK(sim_api_bin("PUT", uri, file, sizeof(file), &resp) == 200, "PUT /api/tag-firmware");
    tag_expect_app(img, 4096);
    CHECK(st.app_is == 0, "not in the chip yet");
    sim_stats_t s0 = sim_stats;
    for (int i = 0; i < 20 && !installed; i++)
        run_for(60);
    CHECK(installed, "the tag downloaded the update and restarted");
    if (!installed) {
        tag_stop();
        printf("upgrade: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    CHECK(st.app_is == 1, "the bootloader copied it into the chip");
    printf("  installed at %.0f s\n", sim_now_ms / 1000.0);
    tag_stop();

    step("4. The new firmware starts from what the chip kept");
    s0 = sim_stats;
    tag_start(to, state, NULL);
    CHECK(st.fw_version == to_ver, "runs firmware %u, got %u", to_ver, st.fw_version);
    tag_expect_app(img, 4096);
    CHECK(st.app_is == 1, "the chip holds the update");
    CHECK(st.shown_id == shown, "the same screen after the restart (T12): %u, was %u", st.shown_id, shown);
    run_for(250);
    CHECK(st.ota_confirmed, "talked to the AP and kept the update (no rollback)");
    CHECK(st.restarts == 0, "no further restart (%d)", st.restarts);
    CHECK(api("GET", "", NULL) == 200 && sim_json_num("fw") == to_ver, "the AP sees firmware %u", to_ver);
    CHECK(fw_count(model_name, "updated") == 1 && fw_count(model_name, "failed") == 0 &&
              fw_count(model_name, "pending") == 0,
          "the AP counts it as updated");
    CHECK(shows_content(), "still shows its content");
    CHECK(sim_stats.image == s0.image, "not sent its image again (%u)", sim_stats.image - s0.image);
    CHECK(sim_stats.unanswered == s0.unanswered, "found the AP at once (%u unanswered)",
          sim_stats.unanswered - s0.unanswered);
    if (same_store)
        CHECK(sim_stats.config == s0.config, "kept its own settings: no CONFIG (%u)", sim_stats.config - s0.config);
    else
        CHECK(sim_stats.config <= s0.config + 1, "at most one CONFIG for settings it cannot read (%u)",
              sim_stats.config - s0.config);
    printf("  %u CONFIG after the restart\n", sim_stats.config - s0.config);
    CHECK(st.last_sleep_s == 120, "its own settings: sleeps 120 s, got %u", st.last_sleep_s);
    CHECK(api("GET", "", NULL) == 200 && sim_json_bool("config_pending") == 0 && sim_json_num("poll_s") == 120,
          "settings still confirmed");
    CHECK(screen_matches(), "the screen is what the AP draws");
    tag_png(out, "2-after");

    step("5. New content on the new firmware");
    CHECK(api("PUT", "", PRICE_2) == 200, "PUT content");
    CHECK(run_until_draw(320), "drawn");
    run_for(250);
    CHECK(shows_content(), "shows the new content");
    CHECK(api("GET", "", NULL) == 200 && sim_json_bool("pending") == 0, "confirmed");
    CHECK(screen_matches(), "the screen is what the AP draws");
    tag_png(out, "3-new-content");
    snprintf(uri, sizeof(uri), "/api/tag-firmware/%s", model_name);
    CHECK(sim_api("DELETE", uri, NULL, &resp) == 200, "DELETE the update");
    tag_stop();
    remove(state);

    printf("upgrade %u -> %u: %d checks, %d failed\n", from_ver, to_ver, checks, failures);
    return failures ? 1 : 0;
}
