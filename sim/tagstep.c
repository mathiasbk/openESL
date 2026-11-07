/* tagstep.c - esl-sim-tag: one simulated tag in a program of its own, driven step by step by
 * "esl-sim upgrade" over a socket (tagstep.h). It has no AP of its own: sim_link sends each
 * frame to the AP of the other program.
 *
 *   esl-sim-tag --fd N [--state-in FILE] [--state-out FILE]
 *     --state-in    start from what a chip kept over a firmware update (sim_tag_state_save),
 *                   instead of a new tag with empty flash
 *     --state-out   when the bootloader has installed new firmware, save what the chip keeps
 *                   there, send 'X' and stop: the new firmware goes on in the next program
 *
 * upgrade.py builds it from the tag firmware of a release (tag/, shared/ of that release)
 * and from today's, with today's sim_tag.c, tagstep.c and png.c. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sim.h"
#include "tagstep.h"

static int fd = -1;
static const char *state_out;
static uint8_t *expect;
static uint32_t expect_len;

static void read_all(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            exit(0);   /* the AP side stopped */
        p += r;
        n -= (size_t)r;
    }
}

static void write_all(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            exit(0);
        p += w;
        n -= (size_t)w;
    }
}

uint8_t sim_link(uint8_t channel, const uint8_t *frame, uint8_t len, uint8_t *reply)
{
    uint8_t hdr[11] = {'F'}, n;
    memcpy(hdr + 1, &sim_now_ms, 8);
    hdr[9] = channel;
    hdr[10] = len;
    write_all(hdr, sizeof(hdr));
    write_all(frame, len);
    read_all(&n, 1);
    if (n)
        read_all(reply, n);
    return n;
}

static void send_status(char kind)
{
    tagstep_status_t st = {
        .now_ms = sim_now_ms,
        .next_wake_ms = sim_tag_next_wake_ms(),
        .last_draw_ms = sim_tag_last_draw_ms(),
        .shown_id = sim_tag_shown_id(),
        .draws = sim_tag_draws(),
        .restarts = sim_tag_restarts(),
        .fw_version = sim_tag_fw_version(),
        .last_sleep_s = sim_tag_last_sleep_s(),
        .interval_s = sim_tag_interval_s(),
        .model_code = sim_tag_model_code(),
        .ota_confirmed = sim_tag_ota_confirmed(),
        .misses = sim_tag_misses(),
        .app_is = expect ? sim_tag_app_is(expect, expect_len) : -1,
    };
    st.cfg_len = (uint8_t)sim_tag_cfg(st.cfg, sizeof(st.cfg));
    write_all(&kind, 1);
    write_all(&st, sizeof(st));
}

static void on_restart(void)
{
    if (!sim_tag_state_save(state_out)) {
        perror(state_out);
        exit(1);
    }
    send_status('X');
    exit(0);
}

int main(int argc, char **argv)
{
    const char *state_in = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fd"))
            fd = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--state-in"))
            state_in = argv[i + 1];
        else if (!strcmp(argv[i], "--state-out"))
            state_out = argv[i + 1];
        else
            break;
    }
    if (fd < 0 || argc % 2 == 0) {
        fprintf(stderr, "usage: %s --fd N [--state-in FILE] [--state-out FILE] (started by esl-sim upgrade)\n", argv[0]);
        return 2;
    }
    sim_verbose = getenv("SIM_VERBOSE") != NULL;
    if (state_out)
        sim_on_restart = on_restart;
    sim_tag_flash_erase();
    if (state_in && !sim_tag_state_load(state_in)) {
        fprintf(stderr, "%s: not a saved tag state for this model\n", state_in);
        return 1;
    }
    sim_tag_boot();
    send_status('S');
    for (;;) {
        char cmd;
        uint64_t t;
        uint32_t n;
        uint16_t pn;
        char path[512];
        static uint8_t screen[64 * 1024];
        read_all(&cmd, 1);
        switch (cmd) {
        case 'R':
            read_all(&t, 8);
            sim_tag_run_until(t);
            break;
        case 'D':
            read_all(&t, 8);
            sim_tag_run_until_draw(t);
            break;
        case 'A':
            read_all(&n, 4);
            free(expect);
            expect = malloc(n ? n : 1);
            if (!expect)
                return 1;
            read_all(expect, n);
            expect_len = n;
            break;
        case 'P':
            read_all(&pn, 2);
            if (pn >= sizeof(path))
                return 1;
            read_all(path, pn);
            path[pn] = 0;
            if (!sim_tag_save_png(path))
                perror(path);
            break;
        case 'G':
            n = (uint32_t)sim_tag_screen(screen, sizeof(screen));
            send_status('S');
            write_all(&n, 4);
            write_all(screen, n);
            continue;
        case 'Q':
            return 0;
        default:
            fprintf(stderr, "esl-sim-tag: unknown command %d\n", cmd);
            return 1;
        }
        send_status('S');
    }
}
