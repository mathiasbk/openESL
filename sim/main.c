/* main.c - esl-sim: the tag firmware and the AP core on the PC (sim.h).
 *
 *   esl-sim test [OUTDIR]       the end-to-end scenarios (e2e.c), screens as PNG in OUTDIR
 *   esl-sim serve [options]     the AP's USB serial API on a pseudo-terminal, with
 *                               simulated tags polling it in (sped-up) real time:
 *       --speed N    N simulated seconds per real second (default 1)
 *       --png PATH   write the first tag's screen to PATH after every drawing
 *       --tags N     N tags (default 1, at most 16). The first runs in this program; each
 *                    other one is "esl-sim tag" in a program of its own, with its own MAC,
 *                    battery, temperature and signal
 *       --sims A,B   the esl-sim programs for the other tags, taken in turn (one per tag
 *                    model, build/<model>/esl-sim); default this program
 *       -v           print every radio exchange and API request
 *     Prints "PTY <path>" on the first line; then, for example:
 *       tools/esl-client/esl_client.py --serial <path> status
 *       tools/esl-client/esl_client.py --serial <path> serve      (API tab of the web page)
 *   esl-sim upgrade ...         tag firmware over the air from one firmware to another
 *                               (upgrade.c; run by upgrade.py)
 *   esl-sim tag --fd N ...      one more tag for "serve" (started by it): its frames go over
 *                               the socket on fd N to the AP there
 *
 * The serial framing is the one in ap/main/serial_api.c:
 *   host -> AP:  @ESL <seq> <METHOD> <uri> <len>\n<len bytes body>
 *   AP -> host:  @ESL <seq> <status> <len> <len bytes body>\n
 * with log lines in between, as on the C6.
 *
 * Between "serve" and a tag program, each frame goes as 4 bytes (0xE5, channel, the signal
 * the AP measures as int8 dBm, length) and the frame; the reply as 1 byte length and the
 * reply (0 = no reply). */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "sim.h"
#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifndef SIM_ROOT
#define SIM_ROOT ".."
#endif

int e2e_run(const char *outdir, const char *root);
int upgrade_run(int argc, char **argv);

static int pty_fd = -1;
static const char *png_path;

static double real_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pty_write(const char *p, size_t n, bool must)
{
    while (n) {
        ssize_t w = write(pty_fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
        } else if (w < 0 && errno == EAGAIN && must) {
            struct pollfd pf = {pty_fd, POLLOUT, 0};
            poll(&pf, 1, 100);
        } else {
            return;   /* nobody is reading: log lines are dropped */
        }
    }
}

/* A log line on the port, as ESP_LOGI writes them on the C6 */
static void pty_log(const char *fmt, ...)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "I (%llu) sim: ", (unsigned long long)sim_now_ms);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    pty_write(line, (size_t)n, false);
    if (sim_verbose) {
        fwrite(line, 1, (size_t)n, stdout);
        fflush(stdout);
    }
}

static void save_png(void)
{
    sim_tag_save_png(png_path);
}

static void on_draw(void)
{
    pty_log("tag drew its screen, content_id %u", sim_tag_shown_id());
    if (png_path)
        sim_tag_save_png(png_path);
}

static int open_pty(void)
{
    struct termios t;
    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0 || grantpt(fd) || unlockpt(fd))
        return -1;
    /* Keep the slave open ourselves, so the master does not see a hangup between clients,
       and make it raw (no echo, no line editing) like the C6's USB port */
    int slave = open(ptsname(fd), O_RDWR | O_NOCTTY);
    if (slave < 0 || tcgetattr(slave, &t))
        return -1;
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);   /* not for the tag programs (--tags) */
    fcntl(slave, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* ---- more tags, each in a program of its own ("esl-sim tag") ---- */

#define MAX_TAGS 16   /* CAP in sim_ap.c */
#define LINK_MAGIC 0xE5

/* What each tag reports and how the AP hears it, so a dashboard shows some variety: the first
   one is the tag of "serve" as it always was; one runs low on battery, one is in a fridge,
   two are far away */
static const struct {
    uint16_t mv;
    int8_t temp_c, rssi;
} profiles[MAX_TAGS] = {
    {2950, 20, -55}, {3010, 21, -48}, {2620, 19, -71}, {2310, 22, -63},
    {2890, 4, -66},  {2780, 18, -89}, {3000, 20, -58}, {2700, 23, -77},
    {2980, 20, -52}, {2850, 5, -68},  {2450, 21, -60}, {2930, 19, -84},
    {3020, 20, -45}, {2800, 22, -73}, {2900, 20, -57}, {2660, 18, -80},
};

static bool read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* "esl-sim tag": sim_tag_link to the AP of "serve" over the socket. With no AP there any
   more (serve stopped), the tag stops too. */
static int link_fd = -1;
static int8_t link_rssi = -55;

static uint8_t remote_link(uint8_t channel, const uint8_t *frame, uint8_t len, uint8_t *reply)
{
    uint8_t hdr[4] = {LINK_MAGIC, channel, (uint8_t)link_rssi, len}, n;
    if (!write_all(link_fd, hdr, 4) || !write_all(link_fd, frame, len) || !read_all(link_fd, &n, 1) ||
        (n && !read_all(link_fd, reply, n)))
        exit(0);
    return n;
}

static int tag_main(int argc, char **argv)
{
    double speed = 1, start;
    for (int i = 2; i < argc; i++) {
        if (i + 1 >= argc) {
            fprintf(stderr, "%s needs a value\n", argv[i]);
            return 2;
        }
        const char *o = argv[i], *v = argv[++i];
        if (!strcmp(o, "--fd"))
            link_fd = atoi(v);
        else if (!strcmp(o, "--mac"))
            sim_tag_mac = strtoull(v, NULL, 16);
        else if (!strcmp(o, "--mv"))
            sim_tag_battery_mv = (uint16_t)atoi(v);
        else if (!strcmp(o, "--temp"))
            sim_tag_temp_c = (int8_t)atoi(v);
        else if (!strcmp(o, "--rssi"))
            link_rssi = (int8_t)atoi(v);
        else if (!strcmp(o, "--speed"))
            speed = atof(v);
        else if (!strcmp(o, "--png"))
            png_path = v;
        else {
            fprintf(stderr, "unknown option %s\n", o);
            return 2;
        }
    }
    if (link_fd < 0) {
        fprintf(stderr, "esl-sim tag is started by esl-sim serve --tags N\n");
        return 2;
    }
    if (speed <= 0)
        speed = 1;
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    sim_tag_link = remote_link;
    if (png_path)
        sim_on_draw = save_png;
    sim_tag_flash_erase();
    sim_tag_boot();
    start = real_s();
    for (;;) {
        sim_tag_run_until((uint64_t)((real_s() - start) * speed * 1000));
        double wait_ms = (sim_tag_next_wake_ms() - sim_now_ms) / speed;
        /* serve never writes unless asked, so anything to read here is its end */
        struct pollfd pf = {link_fd, POLLIN, 0};
        if (poll(&pf, 1, wait_ms > 200 ? 200 : (int)wait_ms + 1) > 0)
            return 0;
    }
}

typedef struct {
    int fd;
    pid_t pid;
} child_t;

static child_t children[MAX_TAGS];
static int n_children;

static void stop_children(void)
{
    for (int i = 0; i < n_children; i++)
        if (children[i].pid > 0)
            kill(children[i].pid, SIGTERM);
}

static void on_signal(int sig)
{
    stop_children();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Starts tag number i (1 ...) as "sim tag" from the program sim */
static bool start_child(const char *sim, int i, double speed)
{
    int sv[2];
    char mac[24], mv[8], temp[8], rssi[8], sp[32];
    if (access(sim, X_OK)) {
        fprintf(stderr, "%s: not a program (--sims)\n", sim);
        return false;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        perror("socketpair");
        return false;
    }
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);   /* the AP's end: not for the other tag programs */
    snprintf(mac, sizeof(mac), "%016llX", (unsigned long long)(sim_tag_mac + (uint64_t)i));
    snprintf(mv, sizeof(mv), "%u", profiles[i].mv);
    snprintf(temp, sizeof(temp), "%d", profiles[i].temp_c);
    snprintf(rssi, sizeof(rssi), "%d", profiles[i].rssi);
    snprintf(sp, sizeof(sp), "%g", speed);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return false;
    }
    if (pid == 0) {
        close(sv[0]);
        if (sv[1] != 3) {
            dup2(sv[1], 3);
            close(sv[1]);
        }
        execl(sim, sim, "tag", "--fd", "3", "--mac", mac, "--mv", mv, "--temp", temp, "--rssi", rssi, "--speed", sp,
              (char *)NULL);
        perror(sim);
        _exit(127);
    }
    close(sv[1]);
    children[n_children].fd = sv[0];
    children[n_children].pid = pid;
    n_children++;
    printf("tag %s from %s: %s mV, %s C, %s dBm\n", mac, sim, mv, temp, rssi);
    return true;
}

/* A frame from tag program c: through the AP here, the reply back */
static void child_frame(child_t *c)
{
    uint8_t hdr[4], frame[256], reply[256], n;
    if (!read_all(c->fd, hdr, 4) || hdr[0] != LINK_MAGIC || !read_all(c->fd, frame, hdr[3])) {
        close(c->fd);   /* it stopped */
        c->fd = -1;
        waitpid(c->pid, NULL, WNOHANG);
        c->pid = 0;
        return;
    }
    sim_link_rssi = (int8_t)hdr[2];
    n = sim_link(hdr[1], frame, hdr[3], reply);
    sim_link_rssi = profiles[0].rssi;
    write_all(c->fd, &n, 1);
    if (n)
        write_all(c->fd, reply, n);
}

static int serve(int argc, char **argv)
{
    static char in[64 * 1024];
    size_t in_len = 0;
    double speed = 1, start;
    int n_tags = 1;
    char *sims = NULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png_path = argv[++i];
        else if (!strcmp(argv[i], "--tags") && i + 1 < argc)
            n_tags = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sims") && i + 1 < argc)
            sims = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            sim_verbose = true;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (speed <= 0)
        speed = 1;
    if (n_tags < 1 || n_tags > MAX_TAGS) {
        fprintf(stderr, "--tags: 1 to %d\n", MAX_TAGS);
        return 2;
    }
    pty_fd = open_pty();
    if (pty_fd < 0) {
        perror("pty");
        return 1;
    }
    printf("PTY %s\n", ptsname(pty_fd));
    printf("tag %016llX (model %s), %g simulated s per s\n", (unsigned long long)sim_tag_mac, sim_tag_model, speed);

    /* The other tags, from the programs in --sims in turn */
    const char *list[MAX_TAGS];
    int n_list = 0;
    for (char *p = sims ? strtok(sims, ",") : NULL; p && n_list < MAX_TAGS; p = strtok(NULL, ","))
        list[n_list++] = p;
    if (!n_list)
        list[n_list++] = argv[0];
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    atexit(stop_children);
    for (int i = 1; i < n_tags; i++)
        if (!start_child(list[(i - 1) % n_list], i, speed))
            return 1;
    fflush(stdout);

    sim_on_draw = on_draw;
    sim_link_rssi = profiles[0].rssi;
    sim_tag_flash_erase();
    sim_ap_boot();
    sim_tag_boot();
    start = real_s();

    for (;;) {
        uint64_t target = (uint64_t)((real_s() - start) * speed * 1000);
        sim_tag_run_until(target);

        double wait_ms = (sim_tag_next_wake_ms() - sim_now_ms) / speed;
        struct pollfd pf[1 + MAX_TAGS] = {{pty_fd, POLLIN, 0}};
        int map[1 + MAX_TAGS], n_pf = 1;
        for (int i = 0; i < n_children; i++)
            if (children[i].fd >= 0) {
                pf[n_pf].fd = children[i].fd;
                pf[n_pf].events = POLLIN;
                map[n_pf++] = i;
            }
        int r = poll(pf, (nfds_t)n_pf, wait_ms > 200 ? 200 : (int)wait_ms + 1);
        if (r <= 0)
            continue;
        for (int i = 1; i < n_pf; i++)
            if (pf[i].revents) {
                sim_tag_run_until((uint64_t)((real_s() - start) * speed * 1000));
                child_frame(&children[map[i]]);
            }
        if (!(pf[0].revents & POLLIN))
            continue;
        ssize_t n = read(pty_fd, in + in_len, sizeof(in) - in_len);
        if (n <= 0)
            continue;
        in_len += (size_t)n;

        /* Handle every complete request in the buffer */
        for (;;) {
            char *nl = memchr(in, '\n', in_len);
            char seq[16], method[8], uri[256];
            unsigned len;
            size_t head;
            if (!nl)
                break;
            head = (size_t)(nl - in) + 1;
            *nl = 0;
            if (strncmp(in, "@ESL ", 5) || sscanf(in, "@ESL %15s %7s %255s %u", seq, method, uri, &len) != 4) {
                memmove(in, in + head, in_len - head);
                in_len -= head;
                continue;
            }
            if (in_len - head < len) {
                *nl = '\n';
                if (head + len > sizeof(in)) {   /* cannot ever fit: drop it */
                    in_len = 0;
                }
                break;
            }
            static char body[64 * 1024];
            memcpy(body, in + head, len);
            body[len] = 0;
            memmove(in, in + head + len, in_len - head - len);
            in_len -= head + len;

            sim_tag_run_until((uint64_t)((real_s() - start) * speed * 1000));
            const char *resp;
            /* the body as it came: a picture or firmware holds zero bytes */
            int status = sim_api_bin(method, uri, len ? body : NULL, len, &resp);
            char hdr[64];
            int hn = snprintf(hdr, sizeof(hdr), "@ESL %s %d %u ", seq, status, (unsigned)strlen(resp));
            pty_write(hdr, (size_t)hn, true);
            pty_write(resp, strlen(resp), true);
            pty_write("\n", 1, true);
            pty_log("%s %s -> %d", method, uri, status);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "test")) {
        const char *out = argc >= 3 ? argv[2] : ".";
        sim_verbose = getenv("SIM_VERBOSE") != NULL;
        return e2e_run(out, getenv("SIM_ROOT") ? getenv("SIM_ROOT") : SIM_ROOT);
    }
    if (argc >= 2 && !strcmp(argv[1], "serve"))
        return serve(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "tag"))
        return tag_main(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "upgrade"))
        return upgrade_run(argc, argv);
    fprintf(stderr, "usage: %s test [OUTDIR] | serve [--speed N] [--png PATH] [--tags N] [--sims A,B] [-v] |\n"
                    "       upgrade FROM TO TO_VERSION OUTDIR (sim/upgrade.py)\n", argv[0]);
    return 2;
}
