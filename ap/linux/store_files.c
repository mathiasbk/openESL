/* store_files.c - what the C6 keeps in NVS (ap/main/store_nvs.c), as files in the data
 * directory, in the same blob formats (esl_persist.h):
 *
 *   content/<slot>  cfg/<slot>  seen/<slot>  image/<slot>  sched/<slot>
 *   layout/<model>  font/<slot>  tagfw/<model>.bin  tagfw/<model>.to
 *   next_poll  channel  ap_id  hours
 *
 * Every file is written whole to a temporary name of its own, synced, renamed over the old
 * one and the directory synced, so a power cut leaves the old file or the new one, never
 * half of one. seen/ (what was last heard from each tag) skips the syncs: losing an hour of
 * it costs nothing, and syncing hundreds of files at once would stall the card. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ap.h"
#include "esl_image.h"
#include "esl_persist.h"
#include "esl_radio.h"

static char root[512];
static pthread_mutex_t mutex;
/* esl_store_ops_t save_lock: content and settings are saved in the order they change the
 * table, as store_nvs.c does, so two writers cannot leave the older one in the file */
static pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;

static void save_lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&save_mutex);
}

static void save_unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&save_mutex);
}

/* The image buffers' lock: drawing under it takes the layouts' (store_lock) too */
static pthread_mutex_t image_mutex = PTHREAD_MUTEX_INITIALIZER;

static void image_lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&image_mutex);
}

static void image_unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&image_mutex);
}

void store_lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&mutex);
}

void store_unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&mutex);
}

/* ---- files ---- */

static void path_of(char *out, size_t cap, const char *dir, unsigned n, const char *ext)
{
    if (dir)
        snprintf(out, cap, "%s/%s/%u%s", root, dir, n, ext);
    else
        snprintf(out, cap, "%s/%s", root, ext);
}

static void sync_dir(const char *path)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = 0;
    int fd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static bool write_file_as(const char *path, const void *data, size_t len, bool durable)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);   /* its own name: two writers never share one */
    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) {
        ap_log("cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            ap_log("cannot write %s: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    if ((durable && fsync(fd) != 0) || close(fd) != 0 || rename(tmp, path) != 0) {
        ap_log("cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    if (durable)
        sync_dir(path);
    return true;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    return write_file_as(path, data, len, true);
}

/* The file's length, or -1 with errno: ENOENT when it is missing, EFBIG when it is larger
 * than cap, EIO when it cannot be read whole */
static long read_file(const char *path, void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap, f);
    bool bad = ferror(f), more = !bad && fgetc(f) != EOF;
    fclose(f);
    if (bad || more) {
        errno = bad ? EIO : EFBIG;
        return -1;
    }
    return (long)n;
}

void store_files_get_text(const char *name, char *buf, size_t cap)
{
    char p[560];
    path_of(p, sizeof(p), NULL, 0, name);
    long n = read_file(p, buf, cap - 1);
    buf[n > 0 ? n : 0] = 0;
}

bool store_files_put_text(const char *name, const char *text)
{
    char p[560];
    path_of(p, sizeof(p), NULL, 0, name);
    return write_file(p, text, strlen(text));
}

static void erase_file(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT)
        ap_log("cannot remove %s: %s", path, strerror(errno));
}

static bool save(const char *dir, unsigned n, const void *data, size_t len)
{
    char p[560];
    path_of(p, sizeof(p), dir, n, "");
    return write_file_as(p, data, len, strcmp(dir, "seen") != 0);
}

static long load(const char *dir, unsigned n, void *buf, size_t cap)
{
    char p[560];
    path_of(p, sizeof(p), dir, n, "");
    return read_file(p, buf, cap);
}

static void erase(const char *dir, unsigned n)
{
    char p[560];
    path_of(p, sizeof(p), dir, n, "");
    erase_file(p);
}

/* ---- the store (esl_store_ops_t) ---- */

static bool content_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    (void)ctx;
    return save("content", slot, blob, esl_persist_encode(mac, c, blob));
}

/* As store_nvs.c: the store keeps no content in RAM */
static bool content_load(void *ctx, unsigned slot, uint64_t mac, esl_content_t *c)
{
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    uint64_t m;
    (void)ctx;
    long n = load("content", slot, blob, sizeof(blob));
    return n > 0 && esl_persist_decode(blob, (size_t)n, &m, c) && m == mac;
}

static void content_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    erase("content", slot);
    erase("image", slot);
}

static void cfg_save(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    uint8_t blob[ESL_PERSIST_CFG_LEN];
    (void)ctx;
    if (cfg)
        save("cfg", slot, blob, esl_persist_cfg_encode(mac, cfg, blob));
    else
        erase("cfg", slot);
}

static void seen_save(void *ctx, unsigned slot, uint64_t mac, const esl_seen_t *s)
{
    uint8_t blob[ESL_PERSIST_SEEN_LEN];
    (void)ctx;
    if (s)
        save("seen", slot, blob, esl_persist_seen_encode(mac, s, blob));
    else
        erase("seen", slot);
}

static void next_poll_save(void *ctx, const uint16_t *next_poll_s)
{
    char p[560];
    (void)ctx;
    path_of(p, sizeof(p), NULL, 0, "next_poll");
    if (next_poll_s) {
        uint8_t b[2] = {(uint8_t)*next_poll_s, (uint8_t)(*next_poll_s >> 8)};
        write_file(p, b, 2);
    } else {
        erase_file(p);
    }
}

static void hours_save(void *ctx, const esl_hours_t *h)
{
    char p[560];
    uint8_t b[ESL_HOURS_LEN];
    (void)ctx;
    path_of(p, sizeof(p), NULL, 0, "hours");
    if (h)
        write_file(p, b, esl_hours_encode(h, b));
    else
        erase_file(p);
}

static void channel_save(void *ctx, uint8_t channel, uint8_t next)
{
    char p[560];
    uint8_t b[2] = {channel, next};
    (void)ctx;
    path_of(p, sizeof(p), NULL, 0, "channel");
    write_file(p, b, 2);
}

static uint32_t unix_now(void *ctx)
{
    (void)ctx;
    return esl_sched_now(&ap.sched, ap_now_ms(), NULL);
}

static bool image_save(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!data) {
        erase("image", slot);
        return true;
    }
    return save("image", slot, data, len);
}

static size_t image_load(void *ctx, unsigned slot, uint8_t *buf, size_t cap)
{
    (void)ctx;
    long n = load("image", slot, buf, cap);
    return n > 0 ? (size_t)n : 0;
}

/* ---- layouts and fonts (esl_layout_ops_t) ---- */

static void layout_save(void *ctx, uint8_t model, const char *data, size_t len)
{
    (void)ctx;
    save("layout", model, data, len);
}

static void layout_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    erase("layout", model);
}

static bool font_save(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!data) {
        erase("font", slot);
        return true;
    }
    return save("font", slot, data, len);
}

/* ---- scheduled content (esl_sched_ops_t) ---- */

static void sched_save(void *ctx, unsigned slot, const uint8_t *blob, size_t len)
{
    (void)ctx;
    save("sched", slot, blob, len);
}

static bool sched_load(void *ctx, unsigned slot, uint8_t *blob, size_t *len)
{
    (void)ctx;
    long n = load("sched", slot, blob, *len);
    if (n <= 0)
        return false;
    *len = (size_t)n;
    return true;
}

static void sched_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    erase("sched", slot);
}

/* The host's time zone at t, for the next time of a recurring entry (#415) */
static int32_t sched_utc_offset_at(void *ctx, uint32_t t)
{
    (void)ctx;
    time_t tt = (time_t)t;
    struct tm l;
    return localtime_r(&tt, &l) ? (int32_t)l.tm_gmtoff : 0;
}

/* ---- tag firmware (esl_tagfw_ops_t): one file of ESL_TAGFW_SLOT bytes per model ---- */

static int fw_open(uint8_t model, int flags)
{
    char p[560];
    path_of(p, sizeof(p), "tagfw", model, ".bin");
    return open(p, flags, 0600);
}

static bool fw_erase(void *ctx, uint8_t model)
{
    static uint8_t ff[4096];
    (void)ctx;
    memset(ff, 0xFF, sizeof(ff));
    int fd = fw_open(model, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    bool ok = true;
    for (uint32_t off = 0; ok && off < ESL_TAGFW_SLOT; off += sizeof(ff))
        ok = write(fd, ff, sizeof(ff)) == (ssize_t)sizeof(ff);
    return close(fd) == 0 && ok;
}

static bool fw_write(void *ctx, uint8_t model, uint32_t off, const void *data, size_t len)
{
    (void)ctx;
    if (off + len > ESL_TAGFW_SLOT)
        return false;
    int fd = fw_open(model, O_WRONLY);
    if (fd < 0)
        return false;
    bool ok = pwrite(fd, data, len, off) == (ssize_t)len && fsync(fd) == 0;
    return close(fd) == 0 && ok;
}

static bool fw_read(void *ctx, uint8_t model, uint32_t off, void *data, size_t len)
{
    (void)ctx;
    if (off + len > ESL_TAGFW_SLOT)
        return false;
    memset(data, 0xFF, len);
    int fd = fw_open(model, O_RDONLY);
    if (fd < 0)
        return true;   /* never written: erased flash */
    bool ok = pread(fd, data, len, off) >= 0;
    close(fd);
    return ok;
}

static void fw_save_to(void *ctx, uint8_t model, const void *to, size_t len)
{
    char p[560];
    (void)ctx;
    path_of(p, sizeof(p), "tagfw", model, ".to");
    if (to)
        write_file(p, to, len);
    else
        erase_file(p);
}

static bool fw_load_to(void *ctx, uint8_t model, void *to, size_t len)
{
    char p[560];
    (void)ctx;
    path_of(p, sizeof(p), "tagfw", model, ".to");
    return read_file(p, to, len) == (long)len;
}

/* ---- the AP id (#390, docs/protocol.md "AP id"), as store_nvs.c ----
 *
 * ap_id is made once, at random, the first time the AP starts with this data directory,
 * and sent to the tags in CONFIG: a tag that has it is then answered only by this AP (or
 * one that adopts it). A new id with an empty tag table adopts every lost tag for
 * ADOPT_S, so an AP moved to new storage gets its old tags back.
 *   prev  = u32 id, u32 end: the AP's own id before PUT "ap_id", still answered until end
 *   adopt = u32 id (0 = every id), u32 end
 * Both ends are unix s; 1 = the days start once the clock is known, and with no clock at
 * all they are counted from the start of the program. */
#define ADOPT_S ((uint32_t)ESL_API_ADOPT_DAYS * 24 * 3600)

static esl_apid_cfg_t apid;
static uint32_t prev_end;
static uint64_t adopt_from_ms, prev_from_ms; /* ap_now_ms() when set, for the no-clock fallback */

static bool save_pair(const char *name, uint32_t a, uint32_t b)
{
    char p[560];
    uint8_t buf[8];
    path_of(p, sizeof(p), NULL, 0, name);
    if (!b) {
        erase_file(p);
        return true;
    }
    for (int i = 0; i < 4; i++) {
        buf[i] = (uint8_t)(a >> (8 * i));
        buf[4 + i] = (uint8_t)(b >> (8 * i));
    }
    return write_file(p, buf, 8);
}

static bool load_pair(const char *name, uint32_t *a, uint32_t *b)
{
    char p[560];
    uint8_t buf[8];
    path_of(p, sizeof(p), NULL, 0, name);
    if (read_file(p, buf, 8) != 8)
        return false;
    *a = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
    *b = buf[4] | buf[5] << 8 | buf[6] << 16 | (uint32_t)buf[7] << 24;
    return true;
}

static bool save_id(uint32_t id)
{
    char p[560];
    uint8_t b[4] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24)};
    path_of(p, sizeof(p), NULL, 0, "ap_id");
    return write_file(p, b, 4);
}

static void adopt_save(uint32_t end, uint32_t id)
{
    save_pair("adopt", end ? id : 0, end);
    apid.adopt_until = end;
}

static void prev_save(uint32_t id, uint32_t end)
{
    save_pair("prev", id, end);
    prev_end = end;
}

/* 1: the clock is known now, the days start; -1: they are up; 0: neither */
static int days_check(uint32_t end, uint32_t now, uint64_t from_ms)
{
    if (!end)
        return 0;
    if (!now)
        return end == 1 && ap_now_ms() - from_ms >= (uint64_t)ADOPT_S * 1000 ? -1 : 0;
    if (end == 1)
        return 1;
    return now >= end ? -1 : 0;
}

void store_files_sync(void)
{
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        syncfs(fd);
        close(fd);
    }
}

void store_files_tick(void)
{
    store_lock(NULL);
    uint32_t now = unix_now(NULL);
    bool on = ap.store.adopt_all || ap.store.adopt_id; /* turned off meanwhile: not started again */
    int a = days_check(apid.adopt_until, now, adopt_from_ms);
    if (a < 0 || (apid.adopt_until && !on)) {
        esl_store_set_adopt(&ap.store, false, 0);
        adopt_save(0, 0);
        ap_log("adopt ended");
    } else if (a > 0) {
        adopt_save(now + ADOPT_S, ap.store.adopt_all ? 0 : ap.store.adopt_id);
    }
    int p = days_check(prev_end, now, prev_from_ms);
    if (p < 0) {
        esl_store_set_prev_id(&ap.store, 0);
        prev_save(0, 0);
        ap_log("the previous AP id is no longer answered");
    } else if (p > 0) {
        prev_save(ap.store.prev_id, now + ADOPT_S);
    }
    store_unlock(NULL);
}

/* PUT /api/config "ap_id": saved before the store changes */
static bool api_set_id(void *ctx, uint32_t id, uint32_t prev)
{
    (void)ctx;
    store_lock(NULL); /* store_files_tick writes the same, from the housekeeping thread */
    uint32_t now = unix_now(NULL);
    bool ok = save_pair("prev", prev, prev ? (now ? now + ADOPT_S : 1) : 0) && save_id(id);
    if (ok) {
        prev_end = prev ? (now ? now + ADOPT_S : 1) : 0;
        prev_from_ms = ap_now_ms();
        ap_log("AP id set to %08lX", (unsigned long)id);
    }
    store_unlock(NULL);
    return ok;
}

static void api_set_adopt(void *ctx, bool all, uint32_t id)
{
    (void)ctx;
    store_lock(NULL);
    uint32_t now = unix_now(NULL);
    if (!all && !id)
        adopt_save(0, 0);
    else
        adopt_save(now ? now + ADOPT_S : 1, all ? 0 : id);
    adopt_from_ms = ap_now_ms();
    store_unlock(NULL);
    ap_log(all ? "adopting lost tags of any AP" : id ? "adopting lost tags of one AP" : "adopt off");
}

esl_apid_cfg_t *store_files_apid(void)
{
    return &apid;
}

static void ap_id_init(void)
{
    char p[560];
    uint8_t b[4];
    uint32_t id = 0, adopt_id = 0, end = 0, prev = 0, pend = 0;
    apid.set_id = api_set_id;
    apid.set_adopt = api_set_adopt;
    path_of(p, sizeof(p), NULL, 0, "ap_id");
    errno = 0;
    long n = read_file(p, b, sizeof(b));
    if (n < 0 && errno != ENOENT) {
        /* the id may be there but cannot be read (permissions after a move, an I/O error, a
           cut file): a new one would turn this AP's own tags away, so this start runs without
           one, as store_nvs.c does (every tag is answered, as before #390) */
        ap_log("cannot read %s (%s): running without an AP id", p, errno ? strerror(errno) : "not 4 bytes");
        return;
    }
    if (n == 4)
        id = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    else if (n >= 0) {
        ap_log("%s is %ld bytes, not 4: running without an AP id", p, n);
        return;
    }
    if (!load_pair("adopt", &adopt_id, &end))
        adopt_id = end = 0;
    if (!load_pair("prev", &prev, &pend))
        prev = pend = 0;
    if (!id || id == 0xFFFFFFFFul) {
        FILE *r = fopen("/dev/urandom", "rb");
        do
            id = 0;
        while (r && fread(&id, sizeof(id), 1, r) == 1 && (!id || id == 0xFFFFFFFFul));
        if (r)
            fclose(r);
        if (!id || !save_id(id)) {
            /* not kept: a new one at every start would turn the AP's own tags away */
            ap_log("no AP id: every tag is answered");
            return;
        }
        ap_log("new AP id %08lX", (unsigned long)id);
        if (!esl_store_count(&ap.store)) {
            ap_log("WARNING: new AP id with an empty tag table: adopting lost tags of any AP on this channel for "
                   "%u days, as a new C6 does (PUT /api/config {\"adopt\":null} stops it)",
                   (unsigned)ESL_API_ADOPT_DAYS);
            adopt_save(1, 0);
            end = 1;
            adopt_id = 0;
        }
    }
    esl_store_set_ap_id(&ap.store, id);
    apid.adopt_until = end;
    if (end)
        esl_store_set_adopt(&ap.store, !adopt_id, adopt_id);
    if (prev && pend && prev != id) {
        prev_end = pend;
        esl_store_set_prev_id(&ap.store, prev);
    }
}

/* ---- start ---- */

static bool mkdirs(void)
{
    static const char *dirs[] = {"", "content", "cfg", "seen", "image", "sched", "layout", "font", "tagfw"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char p[560];
        snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
        if (mkdir(p, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "openesl-ap: cannot create %s: %s\n", p, strerror(errno));
            return false;
        }
    }
    return true;
}

bool store_files_open(const char *dir, unsigned max_tags, uint16_t next_poll_s, uint8_t channel)
{
    static const esl_store_ops_t ops = {
        .lock = store_lock, .unlock = store_unlock, .save_lock = save_lock, .save_unlock = save_unlock,
        .persist_save = content_save, .persist_erase = content_erase, .persist_cfg = cfg_save,
        .persist_next_poll = next_poll_save, .persist_seen = seen_save, .unix_now = unix_now,
        .load_content = content_load, .persist_image = image_save, .load_image = image_load,
        .persist_channel = channel_save, .persist_hours = hours_save,
    };
    static const esl_layout_ops_t lops = {
        .lock = store_lock, .unlock = store_unlock,
        .persist_save = layout_save, .persist_erase = layout_erase, .font_save = font_save,
    };
    static const esl_sched_ops_t sops = {
        .lock = store_lock, .unlock = store_unlock, .save = sched_save, .load = sched_load, .erase = sched_erase,
        .utc_offset_at = sched_utc_offset_at,
    };
    static const esl_tagfw_ops_t fops = {
        .lock = store_lock, .unlock = store_unlock, .erase = fw_erase, .write = fw_write, .read = fw_read,
        .save_to = fw_save_to, .load_to = fw_load_to,
    };
    static esl_sched_entry_t sched_entries[AP_MAX_TAGS];
    static uint8_t *image_mem;
    static esl_image_t images;
    pthread_mutexattr_t attr;
    uint8_t buf[ESL_LAYOUT_MAX_SIZE > ESL_PERSIST_BLOB_MAX ? ESL_LAYOUT_MAX_SIZE : ESL_PERSIST_BLOB_MAX];
    char p[560];
    long n;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attr);
    snprintf(root, sizeof(root), "%s", dir);
    if (!mkdirs())
        return false;
    if (max_tags > AP_MAX_TAGS)
        max_tags = AP_MAX_TAGS;

    /* as store_nvs_init(): the tags, their settings and what was last heard from them */
    esl_store_init(&ap.store, max_tags, &ops, next_poll_s);
    path_of(p, sizeof(p), NULL, 0, "next_poll");
    if (read_file(p, buf, 2) == 2)
        esl_store_restore_next_poll(&ap.store, (uint16_t)(buf[0] | buf[1] << 8));
    path_of(p, sizeof(p), NULL, 0, "hours");
    {
        esl_hours_t h;
        if ((n = read_file(p, buf, sizeof(buf))) > 0 && esl_hours_decode(buf, (size_t)n, &h))
            esl_store_restore_hours(&ap.store, &h);
    }
    unsigned tags = 0;
    for (unsigned slot = 0; slot < max_tags; slot++) {
        uint64_t mac;
        esl_content_t c;
        esl_tag_cfg_t cfg;
        esl_seen_t s;
        if ((n = load("content", slot, buf, sizeof(buf))) > 0 && esl_persist_decode(buf, (size_t)n, &mac, &c) &&
            esl_store_restore(&ap.store, slot, mac, &c))
            tags++;
        if ((n = load("cfg", slot, buf, sizeof(buf))) > 0 && esl_persist_cfg_decode(buf, (size_t)n, &mac, &cfg))
            esl_store_restore_cfg(&ap.store, slot, mac, &cfg);
        if ((n = load("seen", slot, buf, sizeof(buf))) > 0 && esl_persist_seen_decode(buf, (size_t)n, &mac, &s))
            esl_store_restore_seen(&ap.store, slot, mac, &s);
    }
    esl_store_set_builtin_channel(&ap.store, channel);
    path_of(p, sizeof(p), NULL, 0, "channel");
    if (read_file(p, buf, 2) == 2) {
        /* as store_nvs.c: a channel out of range is dropped, a bad planned move too */
        uint8_t ch = buf[0] >= ESL_CHANNEL_MIN && buf[0] <= ESL_CHANNEL_MAX ? buf[0] : 0;
        uint8_t nch = buf[1] & ~ESL_CHANNEL_FORCED; /* a forced move keeps its flag */
        uint8_t next = nch >= ESL_CHANNEL_MIN && nch <= ESL_CHANNEL_MAX ? buf[1] : 0;
        if ((buf[0] && ch != buf[0]) || (buf[1] && next != buf[1]))
            ap_log("ignoring a bad stored channel (%u, next %u)", buf[0], buf[1]);
        esl_store_restore_channel(&ap.store, ch, next, 0);   /* after the tags */
    }
    ap_id_init();

    /* the font files before the layout files, which may name them */
    esl_layouts_init(&ap.layouts, &lops);
    for (unsigned slot = 0; slot < ESL_FONT_USER_MAX; slot++) {
        path_of(p, sizeof(p), "font", slot, "");
        struct stat sb;
        if (stat(p, &sb) != 0 || sb.st_size <= 0)
            continue;
        uint8_t *fb = malloc((size_t)sb.st_size);
        if (fb && (n = read_file(p, fb, (size_t)sb.st_size)) > 0 && esl_layouts_font_restore(&ap.layouts, slot, fb, (size_t)n))
            ap_debug("font file %u loaded (%ld bytes)", slot, n);
        else
            ap_log("ignoring bad stored font file %u", slot);
        free(fb);
    }
    for (uint8_t model = 1; model < ESL_MODEL_COUNT; model++)
        if ((n = load("layout", model, buf, sizeof(buf))) > 0)
            esl_layouts_restore(&ap.layouts, model, (const char *)buf, (size_t)n);
    for (uint8_t model = 1; model < ESL_MODEL_COUNT; model++)
        esl_store_check_layout(&ap.store, &ap.layouts, model, NULL, NULL);

    esl_sched_init(&ap.sched, sched_entries, max_tags, &sops);
    for (unsigned slot = 0; slot < max_tags; slot++) {
        uint8_t blob[ESL_SCHED_BLOB_MAX];
        if ((n = load("sched", slot, blob, sizeof(blob))) > 0)
            esl_sched_restore(&ap.sched, slot, blob, (size_t)n);
    }

    esl_tagfw_init(&ap.tagfw, &fops);
    esl_radio_set_tagfw(&ap.tagfw);
    /* as app_main: the image buffers, empty after a restart */
    image_mem = malloc(esl_image_mem_size());
    if (!image_mem) {
        fprintf(stderr, "openesl-ap: no memory for the image buffers\n");
        return false;
    }
    /* their own lock: network radios draw in a thread of their own (netradio.c) */
    static const esl_image_ops_t iops = {.lock = image_lock, .unlock = image_unlock};
    esl_image_init(&images, image_mem, &iops);
    esl_radio_set_image(&images);
    ap_log("data in %s: %u tags with content", root, tags);
    return true;
}
