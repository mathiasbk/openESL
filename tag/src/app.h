/*
 * app.h - the tag's state machine, with no hardware code.
 *
 *   app_init()      read the MAC, draw the start screen
 *   app_poll_once() one round: POLL -> IDLE/CONFIG, or IMG (the screen the AP drew,
 *                   IMG_REQ -> IMG until it is whole, then shown) or FW (new firmware,
 *                   FW_REQ -> FW). Returns how many seconds the tag should sleep.
 *
 * main.c just does: hal_init(); app_init(); for (;;) power_sleep_s(app_poll_once());
 * The tests in test/ run the same code against a fake radio and display.
 */
#ifndef APP_H
#define APP_H

#include "compat.h"
#include "proto_tag.h"

/* Settings from the AP's CONFIG message. 0 = the built-in value in config.h. */
typedef struct {
    uint16_t ver;             /* cfg_ver from CONFIG, sent back in every POLL */
    uint16_t poll_s;          /* normal interval (CFG_POLL_S) */
    uint16_t max_poll_s;      /* longest interval without an answer (CFG_MAX_POLL_S) */
    uint16_t lost_ap_s;       /* "No AP found" after this long (CFG_LOST_AP_S), ESL_CFG_LOST_AP_NEVER = never */
    uint16_t long_poll_s;     /* interval once the AP has been gone for long_after_min (CFG_LONG_POLL_S) */
    uint16_t long_after_min;  /* (CFG_LONG_AFTER_MIN), ESL_CFG_LONG_NEVER = never */
} app_cfg_t;

typedef struct {
    uint8_t mac[MAC_ADDR_LEN];
    uint32_t shown_id;        /* image_id currently shown, 0 = nothing */
    uint16_t interval_s;      /* interval from the AP (or default) */
    uint8_t misses;           /* rounds in a row without an answer */
    uint32_t lost_s;          /* seconds slept since the last answer */
    uint16_t slept_s;         /* the sleep the last round asked for, counted into lost_s at the next miss */
    uint8_t lost_shown;       /* "No AP found" is drawn */
    uint8_t poll_flags;       /* ESL_POLL_F_* for the next POLL */
    uint8_t rx_rssi;          /* the radio's RSSI and LQI of the AP's last answer, sent in POLL (0 = none yet) */
    uint8_t rx_lqi;
    uint8_t seq;
    uint16_t rng;
    app_cfg_t cfg;            /* kept in flash, see app_cfg_load/app_cfg_save in app.c */
} app_state_t;

extern app_state_t __xdata app;

void app_init(void);
uint16_t app_poll_once(void);

/* Interval after misses in a row (requirement T11), capped at the configured
   max_poll_s (CFG_MAX_POLL_S by default). Exported for the tests. */
uint16_t app_backoff_s(uint16_t interval_s, uint8_t misses);
/* The interval once the AP has been gone for lost_s seconds: long_poll_s after
   long_after_min, else 0 (backoff as usual). Exported for the tests. */
uint16_t app_long_poll_s(uint32_t lost_s);

#endif /* APP_H */
