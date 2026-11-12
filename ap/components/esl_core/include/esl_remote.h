#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_health.h"
#include "esl_json.h"

/* A SoluM ESL remote (SLT-RC004, FCC ID E2X-RC) as a wireless button (docs/solum-remote.md).
 *
 * The remote has three buttons (W, left, right) and an on/off switch. Each press sends a
 * burst of ten identical, unencrypted IEEE 802.15.4 frames on channel 11, about 160 ms
 * in all (as measured on the air):
 *
 *   41 88 | seq | FF FF | FF FF | EF BE | 19 | button | 00 | FCS
 *   FCF     seq   PAN     dst     src      payload
 *
 * a data frame without ACK, broadcast on PAN 0xFFFF from short address 0xBEEF. seq goes up
 * by one per press, and every frame of a burst has the same. button is 1 (W), 2 (left) or
 * 3 (right). A remote has no address of its own, so all remotes look alike.
 *
 * The AP hears the remote when its radio is on channel 11: either the AP runs on 11, or
 * "listen" is on and the radio visits 11 for ESL_REMOTE_VISIT_MS every ESL_REMOTE_PERIOD_MS
 * (ap/main/radio.c), below. Running the AP on channel 11 costs nothing; the visits cost
 * the tags some battery, since about a third of their POLLs need the retry.
 *
 * esl_remote_frame() turns the frames of one burst into one press and keeps the last
 * ESL_REMOTE_LOG presses for GET /api/remote; the glue (ap/main/remote.c) posts each press
 * to the remote's webhook. The radio task and the API both touch it: they hold lock. */

#define ESL_REMOTE_CHANNEL   11
#define ESL_REMOTE_LOG       8
/* The frames of one burst come within about 160 ms: a frame with the seq of the last press
   within this time is the same press */
#define ESL_REMOTE_SAME_MS   1000
/* The visits to channel 11 while the AP runs on another one. Each visit is longer than the
   gap between two frames of a burst, and the visits come often enough that one always
   falls on a frame: checked for frames 8 to 12 ms apart and a pause of 60 to 100 ms
   between the halves (tools/remote_visits.py). The radio is away 14/48 of the time; a tag
   whose POLL falls in a visit gets its answer to the retry 22-30 ms later, which is before
   the next visit. */
#define ESL_REMOTE_PERIOD_MS 48
#define ESL_REMOTE_VISIT_MS  14

enum { ESL_REMOTE_W = 1, ESL_REMOTE_LEFT = 2, ESL_REMOTE_RIGHT = 3 };

typedef struct {
    uint32_t id;     /* 1, 2, ... since boot */
    uint8_t button;  /* ESL_REMOTE_W ... */
    int8_t rssi;     /* of the first frame heard */
    uint64_t at_ms;  /* ms since boot */
} esl_remote_press_t;

typedef struct esl_remote esl_remote_t;
struct esl_remote {
    /* settings, kept by the glue in NVS */
    bool listen;                 /* visit channel 11 when the AP runs on another one */
    bool can_listen;             /* the radio can visit another channel (the C6's own radio) */
    char webhook[ESL_WEBHOOK_URL_MAX + 1]; /* where each press is posted; "" = nowhere */
    int webhook_status;          /* the last post: its HTTP status, -1 = no answer, 0 = none yet */
    /* Saves and applies new settings: webhook NULL = unchanged. NULL: not saved */
    void (*set)(esl_remote_t *r, bool listen, const char *webhook);
    /* Taken around every read and write of the log and the settings; NULL: no lock */
    void (*lock)(esl_remote_t *r, bool take);
    void *ctx;
    uint32_t boot;               /* a random number per start, so a client tells ids of two starts apart */
    /* what was heard */
    uint32_t frames;             /* remote frames heard */
    uint32_t next_id;
    unsigned n;                  /* presses in log, newest last */
    esl_remote_press_t log[ESL_REMOTE_LOG];
    bool have_seq;
    uint8_t last_seq;
    uint64_t last_ms;            /* the last frame of the last press */
};

/* The button of a remote frame (psdu of len bytes, FCS included or not) and its seq; false
   when it is not one */
bool esl_remote_parse(const uint8_t *psdu, size_t len, uint8_t *button, uint8_t *seq);

/* One received frame: true when it is the first frame of a new press, which is then logged
   and copied to *press (may be NULL). Takes r->lock. */
bool esl_remote_frame(esl_remote_t *r, const uint8_t *psdu, size_t len, int8_t rssi, uint64_t now_ms,
                      esl_remote_press_t *press);

/* "w", "left", "right"; NULL for another number */
const char *esl_remote_button_name(uint8_t button);

/* The webhook's body for one press: {"event":"remote","button":"w","id":3,"boot":...,"rssi":-60} */
void esl_remote_press_json(const esl_remote_t *r, const esl_remote_press_t *p, esl_sb_t *out);
