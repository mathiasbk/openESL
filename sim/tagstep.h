/* tagstep.h - between "esl-sim upgrade" (upgrade.c, the AP) and esl-sim-tag (tagstep.c, one
 * tag in a program of its own), over a socket. The tag program can be built from the tag
 * firmware of an older release, so the upgrade test runs a real firmware update from it to
 * today's, and back.
 *
 * Unlike "serve", time is not real: the AP side says how far the tag runs, and every frame
 * carries the tag's simulated time, so a run gives the same result every time.
 *
 *   AP -> tag:  'R' u64 t      run until t (ms)
 *               'D' u64 t      run until the tag draws, or until t
 *               'A' u32 n, n bytes   the application image the AP sent: status.app_is says
 *                                    whether the chip's code flash holds it
 *               'P' u16 n, n bytes   save the screen as PNG to that path
 *               'G'            the screen: the status, then u32 n and n bytes (sim_tag_screen)
 *               'Q'            stop
 *   tag -> AP:  'S' status     after each command, and once after the tag has booted
 *               'F' u64 now, u8 channel, u8 n, n bytes   a frame; the AP answers with
 *                                    u8 n and n bytes (0 = no reply)
 *               'X' status     the bootloader has installed new firmware; with --state-out
 *                              the program has saved what the chip keeps and stops
 * Both programs are built from today's sim/, so the status goes as the struct. */
#ifndef TAGSTEP_H
#define TAGSTEP_H

#include <stdint.h>

typedef struct {
    uint64_t now_ms, next_wake_ms, last_draw_ms;
    uint32_t shown_id;
    int32_t draws, restarts;
    uint16_t fw_version, last_sleep_s, interval_s;
    uint8_t model_code, ota_confirmed, misses;
    int8_t app_is;        /* -1 = no 'A' yet */
    uint8_t cfg_len;      /* app.cfg as bytes (sim_tag_cfg), in the layout of the firmware */
    uint8_t cfg[32];
} tagstep_status_t;

#endif
