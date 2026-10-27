#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

/* Data sources (ESL_DATASOURCES): fetches each source's data when it is due, makes the
 * content of its tags from it and sets it in the store (esl_source.h). The configuration
 * is kept in NVS cfg/sources. The last answer of each source is kept (in PSRAM) and used
 * again when only the configuration changed, or for a refresh before its Expires, and
 * sent with If-Modified-Since, so MET's servers are asked as their terms say: at most as
 * often as the answer's Expires allows, with a User-Agent that names the program. */

static const char *TAG = "datasrc";

#define NS "cfg"
#define KEY "sources"
#define URL_MAX ESL_SOURCE_URL_MAX
#define IDLE_S 300 /* longest wait between looks at the clock and the sources */

static esl_sources_t *sources; /* about 16 KB: in PSRAM */
static SemaphoreHandle_t mutex;
static TaskHandle_t task;
static esl_store_t *store;
static esl_layouts_t *layouts;
static esl_sched_t *sched;

/* The last answers, by the URL they came from (not by the source's index, which changes when
   a source before it is removed) */
static struct {
    char url[URL_MAX];
    char *body;
    size_t len;
} cache[ESL_SOURCE_MAX];

static void lock(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void unlock(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(mutex);
}

static bool persist(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = len ? nvs_set_blob(h, KEY, json, len) : nvs_erase_key(h, KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving %s/%s failed: %s", NS, KEY, esp_err_to_name(err));
    return err == ESP_OK;
}

static void wake(void *ctx)
{
    (void)ctx;
    if (task)
        xTaskNotifyGive(task);
}

/* The local time's offset from UTC (TZ, set from the AP's settings) */
static int32_t utc_offset(uint32_t t)
{
    time_t tt = (time_t)t;
    struct tm l, g;
    localtime_r(&tt, &l);
    gmtime_r(&tt, &g);
    int32_t days = l.tm_year != g.tm_year ? (l.tm_year > g.tm_year ? 1 : -1) : l.tm_yday - g.tm_yday;
    return days * 86400 + (l.tm_hour - g.tm_hour) * 3600 + (l.tm_min - g.tm_min) * 60;
}

/* The answer's Expires and Last-Modified */
typedef struct {
    char expires[40];
    char last_modified[40];
} heads_t;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    heads_t *h = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h) {
        if (!strcasecmp(e->header_key, "Expires"))
            snprintf(h->expires, sizeof(h->expires), "%s", e->header_value);
        else if (!strcasecmp(e->header_key, "Last-Modified"))
            snprintf(h->last_modified, sizeof(h->last_modified), "%s", e->header_value);
    }
    return ESP_OK;
}

/* The length of the host in an https:// URL, for messages */
static int host_len(const char *url)
{
    const char *h = url + 8, *e = strchr(h, '/');
    return e ? (int)(e - h) : (int)strlen(h);
}

/* GETs url into a new buffer (*body, *len): the HTTP status, 0 when there was no answer
   (err says why) */
static int fetch(const char *url, const char *if_modified, heads_t *h, char **body, size_t *len, char *err,
                 size_t errlen)
{
    char ua[80];
    snprintf(ua, sizeof(ua), "openESL/%s github.com/mathiasbk/openESL", esp_app_get_description()->version);
    esp_http_client_config_t cfg = {
        .url = url,
        .user_agent = ua,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = h,
        .buffer_size = 2048,
    };
    *body = NULL;
    *len = 0;
    memset(h, 0, sizeof(*h));
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, errlen, "no memory for the HTTP client");
        return 0;
    }
    if (if_modified && if_modified[0])
        esp_http_client_set_header(c, "If-Modified-Since", if_modified);
    int status = 0;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        if (status == 200 || status == 203) {
            char *b = heap_caps_malloc(ESL_SOURCE_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!b)
                b = malloc(ESL_SOURCE_BODY_MAX);
            size_t n = 0;
            int r = 0;
            while (b && n < ESL_SOURCE_BODY_MAX && (r = esp_http_client_read(c, b + n, ESL_SOURCE_BODY_MAX - n)) > 0)
                n += (size_t)r;
            if (!b) {
                snprintf(err, errlen, "no memory for the answer");
                status = 0;
            } else if (r < 0 || n == ESL_SOURCE_BODY_MAX) {
                snprintf(err, errlen, r < 0 ? "the answer was cut off" : "the answer is over %d KB",
                         ESL_SOURCE_BODY_MAX / 1024);
                free(b);
                status = 0;
            } else {
                char *small = realloc(b, n ? n : 1); /* keep only what it takes */
                *body = small ? small : b;
                *len = n;
            }
        }
    } else {
        snprintf(err, errlen, "no answer from %.*s: %s", host_len(url), url + 8, esp_err_to_name(e == ESP_OK ? ESP_FAIL : e));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static int cache_find(const char *url)
{
    for (int k = 0; k < ESL_SOURCE_MAX; k++)
        if (cache[k].body && !strcmp(cache[k].url, url))
            return k;
    return -1;
}

/* Whether a source of the configuration now has this URL */
static bool url_in_use(const char *url)
{
    char u[URL_MAX];
    bool used = false;
    lock(NULL);
    for (unsigned i = 0; i < sources->n && !used; i++)
        used = esl_source_url(&sources->cfg[i], u, sizeof(u)) && !strcmp(u, url);
    unlock(NULL);
    return used;
}

/* The slot for a new answer from url: its own, an empty one, or one no source uses any more */
static int cache_slot(const char *url)
{
    int k = cache_find(url);
    for (int j = 0; k < 0 && j < ESL_SOURCE_MAX; j++)
        if (!cache[j].body)
            k = j;
    for (int j = 0; k < 0 && j < ESL_SOURCE_MAX; j++)
        if (!url_in_use(cache[j].url))
            k = j;
    return k < 0 ? 0 : k;
}

static void run(int i, const esl_source_cfg_t *cfg, const char *lm, bool want_fetch, uint32_t now)
{
    static esl_values_t values;
    static esl_source_state_t result;
    static heads_t heads;
    char url[URL_MAX], err[ESL_SOURCE_ERR_MAX] = "";
    uint32_t expires = 0, data_at = 0;
    int status = 0;

    if (!esl_source_url(cfg, url, sizeof(url))) {
        esl_sources_done(sources, i, cfg->id, now, 0, 0, NULL, NULL, 0, NULL, "no URL");
        return;
    }
    int k = cache_find(url);
    bool cached = k >= 0;
    if (want_fetch || !cached) {
        char *body;
        size_t len;
        status = fetch(url, cached ? lm : NULL, &heads, &body, &len, err, sizeof(err));
        expires = esl_http_date(heads.expires);
        /* MET's terms: spread the requests, not all at the Expires every AP for this place got */
        if (expires)
            expires += esp_random() % 300;
        if (status == 200 || status == 203) {
            /* 203: the data is good, but this version of the API is deprecated (it ends about a
               month later): shown as the source's error until then */
            if (status == 203)
                snprintf(err, sizeof(err), "%.*s says this API version is deprecated (HTTP 203): update the AP",
                         host_len(url), url + 8);
            k = cache_slot(url);
            free(cache[k].body);
            snprintf(cache[k].url, sizeof(cache[k].url), "%s", url);
            cache[k].body = body;
            cache[k].len = len;
        } else if (status == 304 && cached) {
            /* not changed: the last answer again, for the time that has passed since */
        } else {
            if (status)
                snprintf(err, sizeof(err), "HTTP %d from %.*s", status, host_len(url), url + 8);
            ESP_LOGW(TAG, "%s: %s", cfg->id, err);
            esl_sources_done(sources, i, cfg->id, now, status, expires, NULL, NULL, 0, NULL, err);
            return;
        }
    }
    if (!esl_source_values(cfg, cache[k].body, cache[k].len, now, utc_offset, &values, &data_at, err, sizeof(err))) {
        ESP_LOGW(TAG, "%s: %s", cfg->id, err);
        esl_sources_done(sources, i, cfg->id, now, status, expires, heads.last_modified, NULL, 0, NULL, err);
        return;
    }
    esl_source_apply(cfg, &values, store, layouts, &result);
    if (result.error[0])
        ESP_LOGW(TAG, "%s: %s", cfg->id, result.error);
    ESP_LOGI(TAG, "%s: %s, %u tag(s), %u changed", cfg->id,
             status == 200 || status == 203 ? "new data" : status == 304 ? "not changed" : "made again",
             (unsigned)result.tags_set, (unsigned)result.tags_changed);
    if (err[0])
        ESP_LOGW(TAG, "%s: %s", cfg->id, err);
    esl_sources_done(sources, i, cfg->id, now, status, expires, heads.last_modified, &values, data_at, &result,
                     err[0] ? err : NULL);
}

static void datasrc_task(void *arg)
{
    (void)arg;
    static esl_source_cfg_t cfg;
    char lm[40];
    for (;;) {
        uint32_t now = esl_sched_now(sched, (uint64_t)esp_timer_get_time() / 1000, NULL);
        uint32_t wait = 10; /* the time is not known yet */
        if (now) {
            bool want_fetch;
            int i = esl_sources_due(sources, now, &cfg, lm, sizeof(lm), &want_fetch);
            if (i >= 0) {
                run(i, &cfg, lm, want_fetch, now);
                continue;
            }
            wait = esl_sources_wait(sources, now, IDLE_S);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((wait ? wait : 1) * 1000));
    }
}

esl_sources_t *datasrc_start(esl_store_t *st, esl_layouts_t *ls, esl_sched_t *sc)
{
    static const esl_sources_ops_t ops = {NULL, lock, unlock, persist, wake};
    store = st;
    layouts = ls;
    sched = sc;
    mutex = xSemaphoreCreateMutex();
    sources = heap_caps_malloc(sizeof(*sources), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sources)
        sources = malloc(sizeof(*sources));
    if (!sc || !mutex || !sources) {
        ESP_LOGE(TAG, "not started: %s", sc ? "out of memory" : "no clock (ESL_MAX_SCHEDULED is 0)");
        return NULL;
    }
    esl_sources_init(sources, &ops);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_blob(h, KEY, NULL, &len) == ESP_OK && len) {
            char *js = malloc(len);
            if (js && nvs_get_blob(h, KEY, js, &len) == ESP_OK && !esl_sources_restore(sources, js, len))
                ESP_LOGW(TAG, "ignoring bad configuration in NVS %s/%s", NS, KEY);
            free(js);
        }
        nvs_close(h);
    }
    if (xTaskCreate(datasrc_task, "datasrc", 8192, NULL, 3, &task) != pdPASS) {
        ESP_LOGE(TAG, "no task: no data sources");
        return NULL;
    }
    ESP_LOGI(TAG, "%u data source(s)", (unsigned)sources->n);
    return sources;
}
