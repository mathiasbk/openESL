#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esl_store.h"

/* Health of the tags (krav A6, "operational reliability"): which tags need
 * attention, from the same table GET /api/tags shows. One byte of flags per
 * tag; GET /api/health lists them, and the AP posts the same list to a
 * webhook (esl_notify_t) whenever it changes. */

#define ESL_HEALTH_SILENT        0x01 /* seen, but not for longer than silent_s */
#define ESL_HEALTH_UNSEEN        0x02 /* not seen since the AP started, and that is longer ago than silent_s */
#define ESL_HEALTH_DRAW_FAILED   0x04 /* its last POLL said the last drawing failed */
#define ESL_HEALTH_LOW_BATTERY   0x08 /* battery below battery_mv (a tag that reports 0 cannot measure it) */
/* 0x10 was layout_failed (the tag rejected the layout file), before the AP drew images */
#define ESL_HEALTH_CONTENT_ERROR 0x20 /* its content is held back (esl_tag_content_err) */

#define ESL_HEALTH_BATTERY_MV_DEFAULT 2500 /* a CR2450 is nearly empty here */
#define ESL_HEALTH_SILENT_X 3              /* default silent_s: this many poll intervals */
#define ESL_HEALTH_TAG_DEFAULT_POLL_S ESL_TAG_BUILTIN_POLL_S /* what a tag polls with when the AP sends 0 */

typedef struct {
    uint16_t silent_s;   /* seconds without a POLL before a tag is silent; 0 = ESL_HEALTH_SILENT_X x its interval */
    uint16_t battery_mv; /* below this is low; 0 = ESL_HEALTH_BATTERY_MV_DEFAULT */
} esl_health_cfg_t;

/* The seconds without a POLL after which the tag counts as silent. */
uint32_t esl_health_silent_s(const esl_health_cfg_t *cfg, uint16_t interval_s);

/* The ESL_HEALTH_* flags of one tag. interval_s is what the AP sends it
 * (esl_store_tag_poll_s). */
uint8_t esl_health_flags(const esl_tag_t *t, uint16_t interval_s, uint64_t now_ms, const esl_health_cfg_t *cfg);

/* The name of one flag, for the API ("silent", "draw_failed", ...). */
const char *esl_health_flag_name(uint8_t flag);

/* The webhook: where the AP posts the health report when it changes. */
#define ESL_WEBHOOK_URL_MAX 127

typedef struct {
    char url[ESL_WEBHOOK_URL_MAX + 1]; /* empty = off */
    void *ctx;
    void (*persist)(void *ctx, const char *url); /* NULL: not stored */
} esl_notify_t;

/* Checks a webhook URL: http://host[:port][/path], printable ASCII, at most
 * ESL_WEBHOOK_URL_MAX bytes. */
bool esl_webhook_url_ok(const char *url, size_t len);
