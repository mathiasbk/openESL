/* main.c - openesl-ap: options, start-up, and the radio loop, which answers every frame
 * with esl_link_handle() as ap/main/radio.c does on the C6 and ticks the scheduler. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ap.h"
#include "esl_radio.h"

#ifndef OPENESL_VERSION
#define OPENESL_VERSION "dev"
#endif

ap_t ap;
int ap_verbose;
static volatile sig_atomic_t stop;

static const radio_backend_t *const backends[] = {&radio_sim, NULL};

/* ---- clock and log ---- */

int64_t ap_clock_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t start_us;

uint64_t ap_now_ms(void)
{
    return (uint64_t)(ap_clock_us() - start_us) / 1000;
}

static void vlog(const char *fmt, va_list ap_)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    vfprintf(stderr, fmt, ap_);
    fputc('\n', stderr);
}

void ap_log(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vlog(fmt, a);
    va_end(a);
}

void ap_debug(const char *fmt, ...)
{
    va_list a;
    if (!ap_verbose)
        return;
    va_start(a, fmt);
    vlog(fmt, a);
    va_end(a);
}

/* ---- options ---- */

typedef struct {
    char data[256];
    char listen[128];
    char radio[256];
    char token[ESL_API_MAX_TOKENS];
    unsigned channel;
    unsigned pan;
    unsigned next_poll_s;
    unsigned max_tags;
} opts_t;

static void usage(FILE *f)
{
    fprintf(f,
            "usage: openesl-ap [options]\n"
            "  --data DIR        where the AP keeps its tags, layouts and settings (/var/lib/openesl)\n"
            "  --listen ADDR     HTTP API and dashboard, [host]:port (:80)\n"
            "  --radio SPEC      the 802.15.4 radio (sim):\n");
    for (const radio_backend_t *const *b = backends; *b; b++)
        fprintf(f, "                      %s\n", (*b)->help);
    fprintf(f,
            "  --channel N       built-in channel 11-26 (25); the API can move the AP to another\n"
            "  --pan HEX         PAN id (4553)\n"
            "  --token TOKEN     API token (default: made at the first start, kept in DIR/token)\n"
            "  --next-poll S     default poll interval sent to tags, 0 = the tag keeps its own (0)\n"
            "  --max-tags N      (%d)\n"
            "  --config FILE     read options from FILE first: one \"name = value\" per line, # comments\n"
            "  -v, --verbose     log every request and frame\n"
            "  --version\n",
            AP_MAX_TAGS);
}

static bool num(unsigned *out, const char *val, int base, unsigned lo, unsigned hi)
{
    char *end;
    unsigned long v = strtoul(val, &end, base);
    if (!*val || *end || v < lo || v > hi)
        return false;
    *out = (unsigned)v;
    return true;
}

static bool set_opt(opts_t *o, const char *name, const char *val)
{
#define STR(field) snprintf(o->field, sizeof(o->field), "%s", val), true
#define NUM(field, base, lo, hi) num(&o->field, val, base, lo, hi)
    if (!strcmp(name, "data"))
        return STR(data);
    if (!strcmp(name, "listen"))
        return STR(listen);
    if (!strcmp(name, "radio"))
        return STR(radio);
    if (!strcmp(name, "token"))
        return STR(token);
    if (!strcmp(name, "channel"))
        return NUM(channel, 10, 11, 26);
    if (!strcmp(name, "pan"))
        return NUM(pan, 16, 0, 0xFFFE);
    if (!strcmp(name, "next-poll"))
        return NUM(next_poll_s, 10, 0, 3600);
    if (!strcmp(name, "max-tags"))
        return NUM(max_tags, 10, 1, AP_MAX_TAGS);
    if (!strcmp(name, "verbose"))
        return ap_verbose = val[0] && strcmp(val, "0") && strcmp(val, "false"), true;
#undef STR
#undef NUM
    return false;
}

static bool read_config(opts_t *o, const char *file)
{
    FILE *f = fopen(file, "r");
    char line[1200];
    unsigned n = 0;
    if (!f) {
        fprintf(stderr, "openesl-ap: %s: %s\n", file, strerror(errno));
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        n++;
        char *s = line + strspn(line, " \t");
        if (*s == '#' || *s == '\n' || !*s)
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "openesl-ap: %s:%u: no \"=\"\n", file, n);
            fclose(f);
            return false;
        }
        char *name = s, *val = eq + 1, *e = eq;
        while (e > name && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        *e = 0;
        val += strspn(val, " \t");
        val[strcspn(val, "\r\n")] = 0;
        for (e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t');)
            *--e = 0;
        if (!set_opt(o, name, val)) {
            fprintf(stderr, "openesl-ap: %s:%u: bad option %s = %s\n", file, n, name, val);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

static int parse_args(opts_t *o, int argc, char **argv)
{
    /* --config first, so the command line wins over the file */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config")) {
            if (i + 1 >= argc || !read_config(o, argv[i + 1]))
                return 2;
        }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            exit(0);
        }
        if (!strcmp(a, "--version")) {
            printf("openesl-ap %s\n", OPENESL_VERSION);
            exit(0);
        }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            ap_verbose = 1;
            continue;
        }
        if (strncmp(a, "--", 2) != 0 || i + 1 >= argc) {
            usage(stderr);
            return 2;
        }
        if (!strcmp(a, "--config")) {
            i++;
            continue;
        }
        if (!set_opt(o, a + 2, argv[i + 1])) {
            fprintf(stderr, "openesl-ap: bad option %s %s\n", a, argv[i + 1]);
            return 2;
        }
        i++;
    }
    return 0;
}

/* The API token: --token, or the one kept in the data directory, made at the first start */
static bool load_token(opts_t *o)
{
    char path[300];
    if (o->token[0])
        return true;
    snprintf(path, sizeof(path), "%s/token", o->data);
    FILE *f = fopen(path, "r");
    if (f) {
        bool ok = fgets(o->token, sizeof(o->token), f) != NULL;
        fclose(f);
        o->token[strcspn(o->token, "\r\n")] = 0;
        if (ok && o->token[0])
            return true;
    }
    uint8_t rnd[16];
    FILE *r = fopen("/dev/urandom", "rb");
    bool ok = r && fread(rnd, sizeof(rnd), 1, r) == 1;
    if (r)
        fclose(r);
    if (!ok) {
        fprintf(stderr, "openesl-ap: no random bytes for a token: give one with --token\n");
        return false;
    }
    for (size_t i = 0; i < sizeof(rnd); i++)
        snprintf(o->token + 2 * i, 3, "%02x", rnd[i]);
    /* readable by this user only from the start */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f || fprintf(f, "%s\n", o->token) < 0 || fclose(f) != 0) {
        fprintf(stderr, "openesl-ap: cannot write %s: %s\n", path, strerror(errno));
        if (f == NULL && fd >= 0)
            close(fd);
        return false;
    }
    ap_log("new API token in %s", path);
    return true;
}

/* ---- the radio loop ---- */

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

static void log_frame(const esl_link_rx_t *f, const esl_radio_event_t *ev, size_t n)
{
    char m[17];
    esl_mac_format(ev->mac, m);
    if (ev->msg_type == ESL_MSG_POLL)
        ap_log("%s poll id=%08lX fw=%u bat=%u model=%u rssi=%d -> %s in %lld us", m, (unsigned long)ev->poll.content_id,
               ev->poll.fw_ver, ev->poll.battery_mv, ev->poll.model, f->rssi, n ? msg_name(ev->reply_type) : "nothing",
               (long long)ap.link.stats.last_us);
    else
        ap_debug("%s %s -> %s", m, msg_name(ev->msg_type), n ? msg_name(ev->reply_type) : "nothing");
}

/* The system clock is taken as right (NTP on the host) once it is past 2024; before that
 * the scheduled content waits for PUT /api/time, as on a C6 without NTP. */
static void sync_clock(void)
{
    time_t now = time(NULL);
    if (now >= 1704067200)
        esl_sched_set_time(&ap.sched, (uint32_t)now, ap_now_ms(), ESL_TIME_NTP);
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/* Slow disk work away from the radio loop, which must answer a tag within its listening
 * window: the AP id's days and the clock every minute, what was last heard from each tag
 * every hour (as seen_task on the C6) */
static void *housekeeping(void *arg)
{
    (void)arg;
    uint64_t last_seen = ap_now_ms();
    while (!stop) {
        for (int i = 0; i < 60 && !stop; i++)
            sleep(1);
        sync_clock();
        store_files_tick();
        if (ap_now_ms() - last_seen >= 3600000) {
            last_seen = ap_now_ms();
            unsigned n = esl_store_save_seen(&ap.store);
            store_files_sync();
            ap_debug("saved %u tags' last poll", n);
        }
    }
    return NULL;
}

static int radio_loop(const radio_backend_t *b)
{
    uint8_t tx[ESL_PSDU_MAX];
    uint64_t last_tick = 0;
    while (!stop) {
        esl_link_rx_t f;
        int r = b->recv(&ap.link, &f, 200);
        if (r < 0)
            return 1;
        uint64_t now = ap_now_ms();
        if (r > 0) {
            esl_radio_event_t ev;
            memset(&ev, 0, sizeof(ev));
            size_t n = esl_link_handle(&ap.link, &ap.store, &ap.layouts, ap.pan, &f, now, tx, &ev, ap_clock_us, NULL);
            if (ev.msg_type)
                log_frame(&f, &ev, n);
        }
        if (now - last_tick >= 1000) {
            last_tick = now;
            esl_sched_tick(&ap.sched, &ap.store, now);
            uint8_t ch = esl_store_channel_step(&ap.store, now);
            if (ch) {
                int err = ap.link.set_channel ? ap.link.set_channel(&ap.link, ch) : -1;
                ap_log("moved to channel %u: %s", ch, err ? "failed" : "ok");
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    static opts_t o = {"/var/lib/openesl", ":80", "sim", "", ESL_DEFAULT_CHANNEL, ESL_DEFAULT_PAN_ID, 0, AP_MAX_TAGS};
    int rc = parse_args(&o, argc, argv);
    if (rc)
        return rc;
    start_us = ap_clock_us();
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* the radio backend */
    const radio_backend_t *b = NULL;
    const char *arg = strchr(o.radio, ':');
    size_t sl = arg ? (size_t)(arg - o.radio) : strlen(o.radio);
    for (const radio_backend_t *const *p = backends; *p; p++)
        if (strlen((*p)->scheme) == sl && !strncmp((*p)->scheme, o.radio, sl))
            b = *p;
    if (!b) {
        fprintf(stderr, "openesl-ap: no radio \"%.*s\"\n", (int)sl, o.radio);
        usage(stderr);
        return 2;
    }

    if (mkdir(o.data, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "openesl-ap: cannot create %s: %s\n", o.data, strerror(errno));
        return 1;
    }
    if (!load_token(&o) || !store_files_open(o.data, o.max_tags, (uint16_t)o.next_poll_s, (uint8_t)o.channel))
        return 1;
    sync_clock();
    ap.pan = (uint16_t)o.pan;

    memset(&ap.link, 0, sizeof(ap.link));
    if (b->open(&ap.link, arg ? arg + 1 : "") != 0)
        return 1;
    esl_channel_state_t cs;
    esl_store_channel_state(&ap.store, 0, &cs);
    if (ap.link.start(&ap.link, cs.channel, ap.pan, ESL_AP_SHORT_ADDR, false) != 0) {
        fprintf(stderr, "openesl-ap: the radio did not start\n");
        return 1;
    }

    /* the API: as app_main, without WiFi, OTA, data sources or network radios */
    uint8_t rnd[4] = {0};
    FILE *r = fopen("/dev/urandom", "rb");
    if (r) {
        if (fread(rnd, sizeof(rnd), 1, r) != 1)
            memset(rnd, 0, sizeof(rnd));
        fclose(r);
    }
    ap.changes.boot = (uint32_t)rnd[0] | rnd[1] << 8 | rnd[2] << 16 | (uint32_t)rnd[3] << 24 | 1;
    ap.api.store = &ap.store;
    ap.api.token = o.token;
    ap.api.fw_version = OPENESL_VERSION;
    ap.api.layouts = &ap.layouts;
    ap.api.reset_reason = "poweron";
    ap.api.sched = &ap.sched;
    ap.api.lock = &ap.api_lock;
    ap.api.tagfw = &ap.tagfw;
    ap.api.radio_stats = &ap.link.stats;
    ap.api.channel_settable = ap.link.set_channel != NULL;
    ap.api.changes = &ap.changes;
    ap.api.apid = store_files_apid();
    if (!http_start(o.listen))
        return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    ap_log("openesl-ap %s: %s radio on channel %u, PAN 0x%04X", OPENESL_VERSION, ap.link.name, cs.channel, ap.pan);
    pthread_t hk;
    bool hk_ok = pthread_create(&hk, NULL, housekeeping, NULL) == 0;
    rc = radio_loop(b);
    stop = 1;
    if (hk_ok)
        pthread_join(hk, NULL);
    esl_store_save_seen(&ap.store);
    store_files_sync();
    if (b->close)
        b->close(&ap.link);
    ap_log("stopped");
    return rc;
}
