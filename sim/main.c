/* main.c - esl-sim: the tag firmware and the AP core on the PC (sim.h).
 *
 *   esl-sim test [OUTDIR]       the end-to-end scenarios (e2e.c), screens as PNG in OUTDIR
 *   esl-sim serve [options]     the AP's USB serial API on a pseudo-terminal, with one
 *                               simulated tag polling it in (sped-up) real time:
 *       --speed N    N simulated seconds per real second (default 1)
 *       --png PATH   write the tag's screen to PATH after every drawing
 *       -v           print every radio exchange and API request
 *     Prints "PTY <path>" on the first line; then, for example:
 *       tools/esl-client/esl_client.py --serial <path> status
 *       tools/esl-client/esl_client.py --serial <path> serve      (API tab of the web page)
 *
 * The serial framing is the one in ap/main/serial_api.c:
 *   host -> AP:  @ESL <seq> <METHOD> <uri> <len>\n<len bytes body>
 *   AP -> host:  @ESL <seq> <status> <len> <len bytes body>\n
 * with log lines in between, as on the C6. */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "sim.h"

#ifndef SIM_ROOT
#define SIM_ROOT ".."
#endif

int e2e_run(const char *outdir, const char *root);

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
    return fd;
}

static int serve(int argc, char **argv)
{
    static char in[64 * 1024];
    size_t in_len = 0;
    double speed = 1, start;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png_path = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            sim_verbose = true;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (speed <= 0)
        speed = 1;
    pty_fd = open_pty();
    if (pty_fd < 0) {
        perror("pty");
        return 1;
    }
    printf("PTY %s\n", ptsname(pty_fd));
    printf("tag %016llX (%s\"), %g simulated s per s\n", (unsigned long long)sim_tag_mac, sim_tag_model, speed);
    fflush(stdout);

    sim_on_draw = on_draw;
    sim_tag_flash_erase();
    sim_ap_boot();
    sim_tag_boot();
    start = real_s();

    for (;;) {
        uint64_t target = (uint64_t)((real_s() - start) * speed * 1000);
        sim_tag_run_until(target);

        double wait_ms = (sim_tag_next_wake_ms() - sim_now_ms) / speed;
        struct pollfd pf = {pty_fd, POLLIN, 0};
        int r = poll(&pf, 1, wait_ms > 200 ? 200 : (int)wait_ms + 1);
        if (r <= 0 || !(pf.revents & POLLIN))
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
    fprintf(stderr, "usage: %s test [OUTDIR] | serve [--speed N] [--png PATH] [-v]\n", argv[0]);
    return 2;
}
