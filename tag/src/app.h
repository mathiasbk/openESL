/*
 * app.h - the tag's state machine, with no hardware code.
 *
 *   app_init()      read the MAC, draw the start screen
 *   app_poll_once() one round: POLL -> IDLE/CONTENT -> draw on new content.
 *                   Returns how many seconds the tag should sleep.
 *
 * main.c just does: hal_init(); app_init(); for (;;) power_sleep_s(app_poll_once());
 * The tests in test/ run the same code against a fake radio and display.
 */
#ifndef APP_H
#define APP_H

#include "compat.h"
#include "proto_tag.h"

typedef struct {
    uint8_t mac[MAC_ADDR_LEN];
    uint32_t shown_id;        /* content_id currently shown, 0 = nothing */
    uint16_t interval_s;      /* interval from the AP (or default) */
    uint8_t misses;           /* rounds in a row without an answer */
    uint32_t lost_s;          /* seconds since the last answer */
    uint8_t lost_shown;       /* "No AP found" is drawn */
    uint8_t poll_flags;       /* ESL_POLL_F_* for the next POLL */
    uint8_t seq;
    uint16_t rng;
} app_state_t;

extern app_state_t __xdata app;

void app_init(void);
uint16_t app_poll_once(void);

/* Interval after misses in a row (requirement T11). Exported for the tests. */
uint16_t app_backoff_s(uint16_t interval_s, uint8_t misses);

#endif /* APP_H */
