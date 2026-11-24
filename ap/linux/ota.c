/* ota.c - the AP updating itself (docs/linux-ap.md, "Updating"): POST /api/ota takes a new
 * openesl-ap binary, GET /api/ota says what runs.
 *
 * Nothing here can bring the old program back by itself, so a new one is a trial run under
 * openesl-ap-run (the script that starts it):
 *
 *   <bin>.new          the upload, until it has been checked
 *   <bin>.prev         the program that ran before (a hard link: <bin> exists at every moment)
 *   <bin>.prev.version its version
 *   <bin>.trial        "new" after an update; openesl-ap-run makes it "started" when it starts the
 *                      new program, and the program removes it once it is up (ota_confirm). A
 *                      program that dies, or does not confirm within OPENESL_TRIAL_S, is replaced by
 *                      <bin>.prev by the script.
 *
 * Without openesl-ap-run (OPENESL_GUARD unset) or in a container there is no way back, so
 * the update is refused with 501: replace the binary by hand, or pull a new image. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ap.h"

#define OTA_MAX (32u * 1024 * 1024)
#define PATH_CAP (PATH_MAX + 16)

static pthread_mutex_t ota_mutex = PTHREAD_MUTEX_INITIALIZER;
static char exe[PATH_MAX];
static bool confirmed = true;
static volatile bool restart_pending;

static void path_with(char *out, size_t cap, const char *suffix)
{
    snprintf(out, cap, "%s%s", exe, suffix);
}

static bool in_container(void)
{
    return access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0;
}

/* The shell script that runs the program sets this; nothing else does */
static bool guarded(void)
{
    const char *g = getenv("OPENESL_GUARD");
    return g && g[0] == '1';
}

static unsigned env_seconds(const char *name, unsigned def)
{
    const char *v = getenv(name);
    char *end;
    unsigned long n = v && *v ? strtoul(v, &end, 10) : 0;
    return v && *v && !*end ? (unsigned)n : def;
}

void ota_init(void)
{
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        exe[0] = 0;
        return;
    }
    exe[n] = 0;
    static const char gone[] = " (deleted)";
    size_t gl = sizeof(gone) - 1;
    if ((size_t)n > gl && !strcmp(exe + n - gl, gone))
        exe[n - gl] = 0;
    char trial[PATH_CAP];
    path_with(trial, sizeof(trial), ".trial");
    confirmed = access(trial, F_OK) != 0;
}

unsigned ota_confirm_delay_s(void)
{
    return env_seconds("OPENESL_CONFIRM_S", 10);
}

void ota_confirm(void)
{
    if (!exe[0] || confirmed)
        return;
    char trial[PATH_CAP];
    path_with(trial, sizeof(trial), ".trial");
    if (unlink(trial) == 0 || errno == ENOENT) {
        confirmed = true;
        ap_log("%s is up: the update is kept", OPENESL_VERSION);
    }
}

bool ota_restart_pending(void)
{
    return restart_pending;
}

/* ---- versions ---- */

/* 1 when a is newer than b, -1 when older, 0 when the same. "1.2.3", with "-rc1" or "+x" after
 * it making it older than the plain release; "dev" is not comparable (0 is returned and the
 * caller lets it through). */
static int version_cmp(const char *a, const char *b)
{
    for (;;) {
        char *ea, *eb;
        unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
        if (x != y)
            return x > y ? 1 : -1;
        if (*ea != '.' && *eb != '.') {
            bool sa = *ea != 0, sb = *eb != 0; /* a suffix makes it a pre-release */
            return sa == sb ? 0 : sa ? -1 : 1;
        }
        a = *ea == '.' ? ea + 1 : "0";
        b = *eb == '.' ? eb + 1 : "0";
    }
}

static bool numeric_version(const char *v)
{
    return v[0] >= '0' && v[0] <= '9';
}

/* ---- the new program ---- */

/* Runs `path --version` for up to 5 s and takes the version from "openesl-ap <version>" */
static bool probe_version(const char *path, char *ver, size_t cap, char *why, size_t whycap)
{
    int p[2];
    if (pipe(p) != 0) {
        snprintf(why, whycap, "pipe: %s", strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        snprintf(why, whycap, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execl(path, path, "--version", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    char out[128];
    size_t n = 0;
    struct pollfd pf = {p[0], POLLIN, 0};
    int64_t end = ap_clock_us() + 5000000;
    while (n < sizeof(out) - 1) {
        int64_t left = (end - ap_clock_us()) / 1000;
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
            break;
        ssize_t r = read(p[0], out + n, sizeof(out) - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    out[n] = 0;
    close(p[0]);
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
    static const char tag[] = "openesl-ap ";
    if (strncmp(out, tag, sizeof(tag) - 1) != 0) {
        snprintf(why, whycap, "it does not run as openesl-ap here (--version printed nothing usable)");
        return false;
    }
    const char *v = out + sizeof(tag) - 1;
    size_t vl = strcspn(v, "\r\n ");
    if (!vl || vl >= cap) {
        snprintf(why, whycap, "no version in it");
        return false;
    }
    memcpy(ver, v, vl);
    ver[vl] = 0;
    return true;
}

/* The first 20 bytes of an ELF file: what makes it for this machine */
static bool elf_matches_self(const unsigned char *h)
{
    unsigned char me[20];
    int fd = open("/proc/self/exe", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, me, sizeof(me)) : -1;
    if (fd >= 0)
        close(fd);
    if (n != (ssize_t)sizeof(me) || memcmp(me, "\x7f" "ELF", 4) != 0)
        return false;
    /* class, byte order, version, and e_machine */
    return !memcmp(h, me, 7) && !memcmp(h + 18, me + 18, 2);
}

static void fsync_dir(void)
{
    char dir[PATH_CAP];
    snprintf(dir, sizeof(dir), "%s", exe);
    char *s = strrchr(dir, '/');
    if (s)
        *s = 0;
    int fd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static bool dir_writable(void)
{
    char dir[PATH_CAP];
    snprintf(dir, sizeof(dir), "%s", exe);
    char *s = strrchr(dir, '/');
    if (s && s != dir)
        *s = 0;
    else if (s)
        s[1] = 0;
    return access(dir, W_OK) == 0;
}

static int fail(char *msg, size_t cap, int status, const char *text)
{
    snprintf(msg, cap, "{\"error\":\"%s\"}", text);
    return status;
}

static bool query_flag(const char *query, const char *name)
{
    size_t nl = strlen(name);
    for (const char *q = query; q && *q; q = strchr(q, '&') ? strchr(q, '&') + 1 : NULL) {
        if (!strncmp(q, name, nl) && (q[nl] == '=' ? q[nl + 1] == '1' : false))
            return true;
    }
    return false;
}

int ota_update(const char *query, long len, ota_read_fn rd, void *ctx, char *msg, size_t cap)
{
    if (!exe[0])
        return fail(msg, cap, 500, "cannot tell where the program is");
    if (in_container())
        return fail(msg, cap, 501, "this AP runs in a container: pull a new image");
    if (!guarded())
        return fail(msg, cap, 501,
                    "this AP was not started by openesl-ap-run, which brings the old version back if the new one "
                    "fails: replace the binary by hand, or start it with openesl-ap-run (docs/linux-ap.md)");
    if (len <= 0)
        return fail(msg, cap, 422, "empty body");
    if ((unsigned long)len > OTA_MAX)
        return fail(msg, cap, 413, "the file is too large for the AP");
    if (!dir_writable())
        return fail(msg, cap, 500, "the folder of the program is not writable by the user running it");
    if (pthread_mutex_trylock(&ota_mutex) != 0)
        return fail(msg, cap, 409, "an update is already in progress");
    int status;
    char np[PATH_CAP], prev[PATH_CAP], prevv[PATH_CAP], trial[PATH_CAP];
    path_with(np, sizeof(np), ".new");
    path_with(prev, sizeof(prev), ".prev");
    path_with(prevv, sizeof(prevv), ".prev.version");
    path_with(trial, sizeof(trial), ".trial");
    int fd = -1;
    char *buf = malloc(64 * 1024);
    if (restart_pending) {
        status = fail(msg, cap, 409, "the AP is restarting into an update");
        goto out;
    }
    if (!confirmed) {
        status = fail(msg, cap, 409, "the running version is not confirmed yet: wait for it, or the old one comes back");
        goto out;
    }
    if (!buf) {
        status = fail(msg, cap, 500, "out of memory");
        goto out;
    }
    unlink(np);
    fd = open(np, O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (fd < 0) {
        status = fail(msg, cap, 500, "cannot write next to the program");
        goto out;
    }
    unsigned char head[20];
    size_t got = 0, headn = 0;
    while (got < (size_t)len) {
        size_t want = (size_t)len - got < 64 * 1024 ? (size_t)len - got : 64 * 1024;
        ssize_t r = rd(ctx, buf, want);
        if (r <= 0) {
            status = 0; /* the body stopped arriving: nothing to answer */
            goto drop;
        }
        for (ssize_t i = 0; i < r && headn < sizeof(head); i++)
            head[headn++] = (unsigned char)buf[i];
        if (write(fd, buf, (size_t)r) != r) {
            status = fail(msg, cap, 500, "cannot write next to the program (disk full?)");
            goto drop;
        }
        got += (size_t)r;
    }
    if (headn < sizeof(head) || !elf_matches_self(head)) {
        status = fail(msg, cap, 422, "not an openesl-ap for this machine (another architecture, or not a program)");
        goto drop;
    }
    if (fsync(fd) != 0 || fchmod(fd, 0755) != 0 || close(fd) != 0) {
        fd = -1;
        status = fail(msg, cap, 500, "cannot write next to the program");
        goto drop;
    }
    fd = -1;
    char ver[64], why[160], err[256];
    if (!probe_version(np, ver, sizeof(ver), why, sizeof(why))) {
        snprintf(err, sizeof(err), "the new file is not usable: %s", why);
        status = fail(msg, cap, 422, err);
        goto drop;
    }
    bool force = query_flag(query, "force");
    if (!force && numeric_version(ver) && numeric_version(OPENESL_VERSION) && version_cmp(ver, OPENESL_VERSION) <= 0) {
        snprintf(err, sizeof(err), "%s is not newer than the running %s (add ?force=1 to install it anyway)", ver,
                 OPENESL_VERSION);
        status = fail(msg, cap, 409, err);
        goto drop;
    }

    /* the swap: <bin> never goes missing. The old program is linked as .prev first, then the new
       one is renamed over <bin>, which is atomic; the trial marker goes in before either */
    unlink(prev);
    unlink(prevv);
    FILE *f = fopen(prevv, "w");
    if (f) {
        fprintf(f, "%s\n", OPENESL_VERSION);
        fclose(f);
    }
    FILE *t = fopen(trial, "w");
    if (!t || fputs("new\n", t) < 0 || fflush(t) != 0 || fsync(fileno(t)) != 0) {
        if (t)
            fclose(t);
        unlink(trial);
        status = fail(msg, cap, 500, "cannot write next to the program");
        goto drop;
    }
    fclose(t);
    if (link(exe, prev) != 0 || rename(np, exe) != 0) {
        int e = errno;
        unlink(trial);
        unlink(prev);
        unlink(prevv);
        snprintf(err, sizeof(err), "cannot replace the program: %s", strerror(e));
        status = fail(msg, cap, 500, err);
        goto drop;
    }
    fsync_dir();
    ap_log("update: %s -> %s written, restarting into it", OPENESL_VERSION, ver);
    restart_pending = true;
    snprintf(msg, cap, "{\"version\":\"%s\",\"previous\":\"%s\",\"restarting\":true}", ver, OPENESL_VERSION);
    status = 200;
    goto out;
drop:
    if (fd >= 0)
        close(fd);
    fd = -1;
    unlink(np);
out:
    free(buf);
    pthread_mutex_unlock(&ota_mutex);
    return status;
}

/* The machine this build is for, as the release names it (openesl-ap-linux-<arch>-<version>): the
 * dashboard offers the file for it */
#if defined(__x86_64__)
#define OTA_ARCH "x86_64"
#elif defined(__aarch64__)
#define OTA_ARCH "aarch64"
#elif defined(__arm__)
#define OTA_ARCH "armv7"
#else
#define OTA_ARCH NULL
#endif

void ota_info(char *out, size_t cap)
{
    char prev[PATH_CAP], prevv[PATH_CAP], pv[64] = "";
    path_with(prev, sizeof(prev), ".prev");
    path_with(prevv, sizeof(prevv), ".prev.version");
    bool have_prev = exe[0] && access(prev, F_OK) == 0;
    FILE *f = have_prev ? fopen(prevv, "r") : NULL;
    if (f) {
        if (fgets(pv, sizeof(pv), f))
            pv[strcspn(pv, "\r\n\"\\")] = 0;
        fclose(f);
    }
    const char *why = in_container() ? "container" : !guarded() ? "unguarded" : !exe[0] ? "unknown" : NULL;
    const char *arch = OTA_ARCH;
    int n = snprintf(out, cap, "{\"slot\":\"binary\",\"version\":\"%s\",\"slot_size\":%u,\"other_slot\":%s,",
                     OPENESL_VERSION, OTA_MAX, have_prev ? "\"prev\"" : "null");
    n += snprintf(out + n, cap - (size_t)n, "\"arch\":%s%s%s,", arch ? "\"" : "", arch ? arch : "null", arch ? "\"" : "");
    n += snprintf(out + n, cap - (size_t)n, "\"other_version\":%s%s%s,\"confirmed\":%s,", pv[0] ? "\"" : "null",
                  pv, pv[0] ? "\"" : "", confirmed ? "true" : "false");
    snprintf(out + n, cap - (size_t)n, "\"self_update\":%s,\"self_update_blocked\":%s%s%s,\"uploading\":%s}",
             why ? "false" : "true", why ? "\"" : "null", why ? why : "", why ? "\"" : "",
             pthread_mutex_trylock(&ota_mutex) == 0 ? (pthread_mutex_unlock(&ota_mutex), "false") : "true");
}
