#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_json.h"
#include "esl_layout.h"
#include "esl_store.h"

/* Data sources: content the AP fetches for itself (ESL_DATASOURCES, the ESP32-S3 boards).
 *
 * A source is something on the internet the AP reads every so often, such as the
 * weather forecast for a place. Each fetch gives a set of named values ("temp",
 * "symbol", ...); the source's fields are templates over them ("{temp}°",
 * "{wind} m/s"), which become the content of the source's tags in its layout, as if a
 * client had sent PUT /api/tags. The store's diffing does the rest: content that came
 * out the same is not drawn or sent again, so a tag only wakes up for a new forecast.
 *
 * This part is plain C and is tested on the PC: the configuration and its checks, the
 * API at /api/sources, each type's URL and how its answer becomes values, and the
 * content. main/datasrc.c fetches (esp_http_client) and runs it on a task.
 *
 * Types:
 *   "met"  the forecast from MET Norway (api.met.no locationforecast 2.0, compact),
 *          for a latitude and longitude. Its data is CC BY 4.0: credit "MET Norway"
 *          where the forecast is shown. Values: esl_met_keys below. */

#define ESL_SOURCE_MAX        8   /* sources per AP */
#define ESL_SOURCE_TAGS_MAX   32  /* tags per source */
#define ESL_SOURCE_ID_MAX     16  /* a-z 0-9 _ - */
#define ESL_SOURCE_PLACE_MAX  32  /* bytes of UTF-8 */
#define ESL_SOURCE_TPL_MAX    48  /* bytes of one field template */
#define ESL_SOURCE_EVERY_MIN  30  /* minutes between fetches: at least, at most and the default */
#define ESL_SOURCE_EVERY_MAX  1440
#define ESL_SOURCE_EVERY_DEF  30
#define ESL_SOURCE_ERR_MAX    96
#define ESL_SOURCE_BODY_MAX   (128 * 1024) /* biggest answer the AP reads */
#define ESL_SOURCE_CFG_MAX    16384        /* the configuration as JSON (NVS, GET /api/sources) */

typedef enum { ESL_SOURCE_MET = 1 } esl_source_type_t;

typedef struct {
    char id[ESL_SOURCE_ID_MAX + 1];
    uint8_t type;                         /* esl_source_type_t */
    int32_t lat, lon;                     /* "met": degrees * 10000 (MET takes 4 decimals) */
    char place[ESL_SOURCE_PLACE_MAX + 1]; /* "met": the place's name, value "place" */
    char lang[3];                         /* "nb" or "en": the words in value "desc" */
    uint16_t every_min;
    char layout[ESL_LAYOUT_NAME_MAX + 1]; /* a layout from the layout files, or "text" */
    uint8_t nfields;
    char fields[ESL_MAX_FIELDS][ESL_SOURCE_TPL_MAX + 1];
    uint8_t ntags;
    uint64_t tags[ESL_SOURCE_TAGS_MAX];
} esl_source_cfg_t;

/* The values one fetch gave: key -> UTF-8 text */
#define ESL_VALUES_MAX     24
#define ESL_VALUE_KEY_MAX  12
#define ESL_VALUE_MAX      40
typedef struct {
    uint8_t n;
    char key[ESL_VALUES_MAX][ESL_VALUE_KEY_MAX + 1];
    char val[ESL_VALUES_MAX][ESL_VALUE_MAX + 1];
} esl_values_t;

void esl_values_set(esl_values_t *v, const char *key, const char *val);
/* NULL when v has no such key */
const char *esl_values_get(const esl_values_t *v, const char *key);

/* What happened to a source, for GET /api/sources */
typedef struct {
    uint32_t fetched_at;  /* Unix s of the last answer with data (200 or 304), 0 = none yet */
    uint32_t tried_at;    /* Unix s of the last try */
    uint32_t data_at;     /* when the data itself was made ("met": updated_at), 0 = unknown */
    uint32_t next_at;     /* Unix s of the next fetch */
    uint32_t expires_at;  /* the answer's Expires: not fetched again before it */
    int16_t http;         /* status of the last try, 0 = no answer */
    uint8_t tags_set;     /* tags given content by the last run, and of them the ones it changed */
    uint8_t tags_changed;
    uint8_t tags_failed;
    uint8_t fails;        /* tries in a row without an answer (not shown) */
    char error[ESL_SOURCE_ERR_MAX]; /* "" when the last run went well */
    char last_modified[40];         /* the answer's Last-Modified, sent back as If-Modified-Since */
    bool reapply;                   /* the content is to be made again from the last answer (not shown) */
    esl_values_t values;
} esl_source_state_t;

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* Saves the configuration (the JSON GET /api/sources shows without "status"); false
     * when it could not be saved */
    bool (*persist)(void *ctx, const char *json, size_t len);
    /* Something to fetch now (new configuration, POST /api/sources/refresh); may be NULL */
    void (*wake)(void *ctx);
} esl_sources_ops_t;

typedef struct esl_sources {
    uint8_t n;
    esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    esl_source_state_t state[ESL_SOURCE_MAX];
    esl_sources_ops_t ops;
    /* esl_sources_api, set by esl_sources_init: esl_api.c calls it through this, so an AP
       without data sources (the C6) does not link this part */
    int (*api)(struct esl_sources *s, const char *method, const char *path, size_t plen, const char *body,
               size_t len, uint32_t now, esl_sb_t *out);
    /* esl_sources_release, the same way */
    bool (*release)(struct esl_sources *s, uint64_t mac);
} esl_sources_t;

/* ops may be NULL (no locking, nothing saved). Starts with no sources. */
void esl_sources_init(esl_sources_t *s, const esl_sources_ops_t *ops);

/* The tag was given other content (PUT /api/tags, a picture): it leaves its source, so the
 * next run does not overwrite that, and a source left without tags goes. The configuration
 * is saved. False when the tag was in no source. */
bool esl_sources_release(esl_sources_t *s, uint64_t mac);

/* Checks a configuration ({"sources": [...]}) into cfg (ESL_SOURCE_MAX of them) and *n.
 * False with a message in err naming the place, e.g. "sources[1].lat: ...". */
bool esl_sources_parse(const char *js, size_t len, esl_source_cfg_t *cfg, uint8_t *n, char *err, size_t errlen);

/* Loads the saved configuration at boot. Does not call persist. */
bool esl_sources_restore(esl_sources_t *s, const char *js, size_t len);

/* Writes one source's configuration as the JSON object the API takes */
void esl_source_write_cfg(const esl_source_cfg_t *c, esl_sb_t *out);

/* /api/sources (docs/protokoll.md part 2): GET, PUT and POST /api/sources/refresh.
 * path is the URI's path from /api/sources on. now is Unix s (0 when the time is not
 * known). Returns the HTTP status. */
int esl_sources_api(esl_sources_t *s, const char *method, const char *path, size_t plen, const char *body,
                    size_t len, uint32_t now, esl_sb_t *out);

/* ---- for the task that fetches (main/datasrc.c) ---- */

/* The index of a source that is due at now, with a copy of its configuration and
 * Last-Modified, or -1. *fetch is true when its data is to be fetched (next_at came), false
 * when only its content is to be made again from the last answer (a new configuration
 * with the same URL, POST /api/sources/refresh before the answer's Expires); a caller
 * without that answer fetches anyway. Handed out once: next_at moves on and reapply is
 * cleared. */
int esl_sources_due(esl_sources_t *s, uint32_t now, esl_source_cfg_t *cfg, char *last_modified, size_t lm_cap,
                    bool *fetch);

/* Seconds until the next fetch is due (at most max_s) */
uint32_t esl_sources_wait(esl_sources_t *s, uint32_t now, uint32_t max_s);

/* The URL of a source's data into buf (cap bytes); 0 when it does not fit */
size_t esl_source_url(const esl_source_cfg_t *c, char *buf, size_t cap);

/* The local time's offset from UTC in s at Unix time t (time zones, summer time) */
typedef int32_t (*esl_utc_offset_fn)(uint32_t t);

/* Values from an answer (body, len bytes) of source c's type, at Unix time now. False
 * with a message in err when it cannot be read. */
bool esl_source_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                       esl_values_t *v, uint32_t *data_at, char *err, size_t errlen);

/* The content of source c's tags from values v: the templates filled in, in Latin-1.
 * False with a message when a field is too long or a template names no value. */
bool esl_source_content(const esl_source_cfg_t *c, const esl_values_t *v, esl_content_t *out, char *err,
                        size_t errlen);

/* Gives each of c's tags the content from v (esl_store_set): ls checks the layout is in
 * the file for the tag's model. Counts in st (tags_set, _changed, _failed, error). */
void esl_source_apply(const esl_source_cfg_t *c, const esl_values_t *v, esl_store_t *store, esl_layouts_t *ls,
                      esl_source_state_t *st);

/* Records the result of a fetch of source i (same id as cfg, else ignored): status is
 * the HTTP status (0 = no answer), expires and last_modified from the answer's headers
 * (0 / NULL when it had none); values and run (the apply counts and error) are copied
 * when values is not NULL. */
void esl_sources_done(esl_sources_t *s, int i, const char *id, uint32_t now, int status, uint32_t expires,
                      const char *last_modified, const esl_values_t *values, uint32_t data_at,
                      const esl_source_state_t *run, const char *error);

/* An HTTP date ("Mon, 26 Oct 2026 16:30:00 GMT") as Unix s, 0 when it is not one */
uint32_t esl_http_date(const char *s);
/* An ISO 8601 UTC time ("2026-10-26T16:00:00Z") as Unix s, 0 when it is not one */
uint32_t esl_iso_time(const char *s, size_t len);

/* The values a "met" source gives, in this order (docs/protokoll.md part 2). */
extern const char *const esl_met_keys[];
