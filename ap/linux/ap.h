#pragma once
/* openesl-ap: the AP core (ap/components/esl_core) as a Linux program (docs/linux-ap.md).
 *
 *   main.c        options, start-up, the radio loop and the scheduler tick
 *   store_files.c what the C6 keeps in NVS, as files in the data directory
 *   http.c        the API, the dashboard and the status line over HTTP
 *   radio_*.c     the radio backends, chosen at run time with --radio
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_api.h"
#include "esl_layout.h"
#include "esl_link.h"
#include "esl_sched.h"
#include "esl_store.h"
#include "esl_tagfw.h"

#define AP_MAX_TAGS 1000

/* ---- clock ---- */

int64_t ap_clock_us(void);  /* monotonic, since an arbitrary start; the clock of esl_link_rx_t.rx_us */
uint64_t ap_now_ms(void);   /* monotonic ms since the program started: the core's now_ms */

/* ---- log ---- */

extern int ap_verbose;
void ap_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ap_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- the AP's state, shared by the radio loop and the HTTP server ---- */

typedef struct {
    esl_store_t store;
    esl_layouts_t layouts;
    esl_sched_t sched;
    esl_tagfw_t tagfw;
    esl_api_changes_t changes;
    esl_api_lock_t api_lock;
    esl_api_t api;
    esl_link_t link;
    uint16_t pan;
} ap_t;

extern ap_t ap;

/* ---- store_files.c ---- */

/* Opens (and creates) the data directory and loads everything kept there into ap's store,
 * layouts, schedule and tag firmware; channel is the built-in one (--channel), which a
 * channel set through the API replaces. False with a message on stderr if the directory
 * cannot be used. */
bool store_files_open(const char *dir, unsigned max_tags, uint16_t next_poll_s, uint8_t channel);
/* The lock every core object's ops use: one mutex, so the radio loop and the HTTP server
 * never see half a change. */
void store_lock(void *ctx);
void store_unlock(void *ctx);
/* The AP id's settings for the API (esl_api_t.apid), and their days: starts them once the
 * clock is known and ends them; called now and then (every minute) */
esl_apid_cfg_t *store_files_apid(void);
void store_files_tick(void);
/* Syncs the data directory's file system: after esl_store_save_seen(), whose files are
 * written without a sync each */
void store_files_sync(void);
/* A setting kept as a line of text in <data>/name: "" when there is none */
void store_files_get_text(const char *name, char *buf, size_t cap);
bool store_files_put_text(const char *name, const char *text);

/* ---- http.c ---- */

/* Starts the server on addr ("[host]:port"); false with a message if it cannot listen. */
bool http_start(const char *addr);
/* The lock the API is handled under: what it reads (ap.api's links) changes under it too */
void http_api_lock(void);
void http_api_unlock(void);
/* A listening TCP socket on addr ("[host]:port"), or -1 with a message naming --opt */
int ap_listen(const char *opt, const char *addr);

/* ---- netradio.c ---- */

/* Network radios beside the AP's own (docs/linux-ap.md, "Network radios"): C6 boards running
 * ap/netradio/, which connect to addr ("[host]:port") and serve channels of their own in
 * places 1..3; the AP's own radio is place 0 on the AP's channel. Fills in
 * ap.api's links and radio_ops. False with a message if it cannot start. */
bool netradio_start(const char *addr);
/* The AP's own radio got this frame (now_ms on ap_now_ms()), before it is answered: the
 * radio that last heard a tag serves it. Nothing without network radios. */
void netradio_heard(const uint8_t *psdu, size_t len, uint64_t now_ms);

/* ---- radio backends ---- */

typedef struct {
    const char *scheme;  /* "sim", "uart", ...: --radio <scheme>[:<arg>] */
    const char *help;
    /* Opens the radio; arg is what follows "scheme:" ("" when nothing). Fills in l's
     * name, send, set_channel, set_tx_power and ctx. 0 = ok, else a message on stderr. */
    int (*open)(esl_link_t *l, const char *arg);
    /* Waits up to timeout_ms for one received frame: 1 = f filled in (rx_us on
     * ap_clock_us()), 0 = none in time, -1 = the radio failed. */
    int (*recv)(esl_link_t *l, esl_link_rx_t *f, int timeout_ms);
    void (*close)(esl_link_t *l);
} radio_backend_t;

extern const radio_backend_t radio_sim;
extern const radio_backend_t radio_coproc;
extern const radio_backend_t radio_spimodem;
