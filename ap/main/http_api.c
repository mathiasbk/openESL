#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

/* esp_http_server glue: reads the request and hands it to esl_api_handle().
 * The server runs all handlers in one task; api_handle() also keeps them apart
 * from requests on the serial port. */

static const char *TAG = "http";

static SemaphoreHandle_t api_mutex;

void api_handle_init(void)
{
    static StaticSemaphore_t buf;
    api_mutex = xSemaphoreCreateMutexStatic(&buf);
}

int api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
               const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out)
{
    xSemaphoreTake(api_mutex, portMAX_DELAY);
    int status = esl_api_handle(api, method, uri, auth, body, body_len, now_ms, out);
    xSemaphoreGive(api_mutex);
    return status;
}

int api_handle_health(const esl_api_t *api, uint64_t now_ms, uint8_t *prev, esl_sb_t *out)
{
    xSemaphoreTake(api_mutex, portMAX_DELAY);
    int status = esl_api_health(api, now_ms, NULL, prev, out);
    xSemaphoreGive(api_mutex);
    return status;
}

static esp_err_t send_json(httpd_req_t *req, int status, const char *body, size_t len)
{
    httpd_resp_set_status(req, esl_api_status_line(status));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (ssize_t)len);
}

/* Replies that grow with the tag table (GET /api/tags, the status page) are
 * streamed: the API fills a buffer of ESL_API_STREAM_BUF bytes and hands each
 * full one here as a chunk, so the reply never has to fit in RAM at once. */
typedef struct {
    httpd_req_t *req;
    const char *type;
} sink_t;

static bool http_sink(esl_sb_t *sb)
{
    sink_t *s = sb->ctx;
    if (sb->sent == 0) {
        httpd_resp_set_status(s->req, esl_api_status_line(sb->status));
        httpd_resp_set_type(s->req, s->type);
    }
    return httpd_resp_send_chunk(s->req, sb->buf, (ssize_t)sb->len) == ESP_OK;
}

/* Sends what the API wrote: the rest of a streamed reply, or the whole one. */
static esp_err_t send_reply(httpd_req_t *req, int status, esl_sb_t *out, const char *type)
{
    if (out->sent) {
        /* The head has gone out with out->status; nothing can replace it */
        if (status != out->status || out->overflow) {
            ESP_LOGE(TAG, "%s: streamed reply failed (%d)", req->uri, status);
            return ESP_FAIL; /* closes the connection: the client sees a cut body */
        }
        esl_sb_flush(out);
        if (out->overflow)
            return ESP_FAIL;
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    httpd_resp_set_status(req, esl_api_status_line(status));
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, out->buf, (ssize_t)out->len);
}

static const char *method_name(int method)
{
    switch (method) {
    case HTTP_GET:    return "GET";
    case HTTP_PUT:    return "PUT";
    case HTTP_DELETE: return "DELETE";
    default:          return "?";
    }
}

static esp_err_t api_handler(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    const char *method = method_name(req->method);
    size_t len = req->content_len;
    char *body = NULL, *auth = NULL, *resp = NULL;
    esp_err_t ret = ESP_OK;

    if (len > ESL_API_MAX_BODY) {
        static const char msg[] = "{\"error\":\"body too large\"}";
        ESP_LOGW(TAG, "%s %s -> 413 (%u bytes)", method, req->uri, (unsigned)len);
        return send_json(req, 413, msg, sizeof(msg) - 1);
    }

    if (len) {
        body = malloc(len);
        if (!body)
            goto oom;
        size_t got = 0;
        while (got < len) {
            int r = httpd_req_recv(req, body + got, len - got);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (r <= 0) {
                free(body);
                return ESP_FAIL; /* closes the connection */
            }
            got += (size_t)r;
        }
    }

    size_t al = httpd_req_get_hdr_value_len(req, "Authorization");
    if (al) {
        auth = malloc(al + 1);
        if (!auth || httpd_req_get_hdr_value_str(req, "Authorization", auth, al + 1) != ESP_OK) {
            free(auth);
            auth = NULL;
        }
    }

    resp = malloc(ESL_API_STREAM_BUF);
    if (!resp)
        goto oom;
    esl_sb_t out;
    sink_t sink = {req, "application/json"};
    esl_sb_init(&out, resp, ESL_API_STREAM_BUF);
    esl_sb_set_sink(&out, http_sink, &sink);
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    int status = api_handle(api, method, req->uri, auth, body, len, now_ms, &out);
    ESP_LOGI(TAG, "%s %s -> %d", method, req->uri, status);
    ret = send_reply(req, status, &out, "application/json");
    goto done;

oom:
    ESP_LOGE(TAG, "%s %s: out of memory", method, req->uri);
    {
        static const char msg[] = "{\"error\":\"out of memory\"}";
        ret = send_json(req, 500, msg, sizeof(msg) - 1);
    }
done:
    free(body);
    free(auth);
    free(resp);
    return ret;
}

/* ---- /setup: WiFi and token (krav A1) ---------------------------------------- */

#define SETUP_MAX_BODY 1024

static esp_err_t send_page(httpd_req_t *req, int status, esl_sb_t *out)
{
    httpd_resp_set_status(req, esl_api_status_line(status));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out->buf, (ssize_t)out->len);
}

static esp_err_t setup_get(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    char q[16];
    const char *msg = NULL;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "scan=1")) {
        if (api->wifi->scan)
            api->wifi->scan(api->wifi->ctx);
        msg = "Searching for networks. Reload the page in a few seconds.";
    }
    char *page = malloc(ESL_WIFI_PAGE_SIZE);
    esl_wifi_status_t *ws = malloc(sizeof(*ws));
    esp_err_t ret;
    if (!page || !ws) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        ret = httpd_resp_sendstr(req, "out of memory");
    } else {
        esl_sb_t out;
        esl_sb_init(&out, page, ESL_WIFI_PAGE_SIZE);
        api->wifi->status(api->wifi->ctx, ws);
        xSemaphoreTake(api_mutex, portMAX_DELAY);
        bool token_set = api->token && api->token[0];
        xSemaphoreGive(api_mutex);
        esl_wifi_page(&out, ws, token_set, api->fw_version, msg);
        ret = send_page(req, 200, &out);
    }
    free(page);
    free(ws);
    return ret;
}

static esp_err_t setup_post(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    size_t len = req->content_len;
    if (len > SETUP_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "too large");
    }
    char *body = malloc(len + 1);
    char *page = malloc(ESL_WIFI_PAGE_SIZE);
    esp_err_t ret = ESP_OK;
    if (!body || !page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        ret = httpd_resp_sendstr(req, "out of memory");
        goto done;
    }
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0) {
            ret = ESP_FAIL;
            goto done;
        }
        got += (size_t)r;
    }
    esl_sb_t out;
    esl_sb_init(&out, page, ESL_WIFI_PAGE_SIZE);
    /* Under the API mutex: it may change the token the API checks. */
    xSemaphoreTake(api_mutex, portMAX_DELAY);
    int status = esl_wifi_setup_post(api->wifi, api->token, api->fw_version, body, len, &out);
    xSemaphoreGive(api_mutex);
    ESP_LOGI(TAG, "POST /setup -> %d", status);
    ret = send_page(req, status, &out);
done:
    free(body);
    free(page);
    return ret;
}

#if CONFIG_ESL_STATUS_PAGE
/* GET /: the status page (krav A6). Read-only and without a token. */
static esp_err_t page_handler(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    if (api->wifi) {
        /* Nothing saved yet: whoever is on the setup network wants the form. */
        esl_wifi_status_t *ws = malloc(sizeof(*ws));
        bool unset = false;
        if (ws) {
            api->wifi->status(api->wifi->ctx, ws);
            unset = ws->mode == ESL_WIFI_SETUP;
            free(ws);
        }
        if (unset) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/setup");
            return httpd_resp_send(req, NULL, 0);
        }
    }
    char *page = malloc(ESL_API_STREAM_BUF);
    if (!page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory");
    }
    esl_sb_t out;
    sink_t sink = {req, "text/html; charset=utf-8"};
    esl_sb_init(&out, page, ESL_API_STREAM_BUF);
    esl_sb_set_sink(&out, http_sink, &sink);
    int status = esl_api_status_page(api, (uint64_t)esp_timer_get_time() / 1000, &out);
    esp_err_t ret = send_reply(req, status, &out, status == 200 ? sink.type : "text/plain");
    free(page);
    return ret;
}
#endif

esp_err_t http_api_start(const esl_api_t *api)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK)
        return err;

    static const httpd_method_t methods[] = {HTTP_GET, HTTP_PUT, HTTP_DELETE};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        httpd_uri_t uri = {
            .uri = "/api/*",
            .method = methods[i],
            .handler = api_handler,
            .user_ctx = (void *)api,
        };
        err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK)
            return err;
    }
    if (api->wifi) {
        httpd_uri_t sg = {.uri = "/setup", .method = HTTP_GET, .handler = setup_get, .user_ctx = (void *)api};
        httpd_uri_t sp = {.uri = "/setup", .method = HTTP_POST, .handler = setup_post, .user_ctx = (void *)api};
        if ((err = httpd_register_uri_handler(server, &sg)) != ESP_OK ||
            (err = httpd_register_uri_handler(server, &sp)) != ESP_OK)
            return err;
    }
#if CONFIG_ESL_STATUS_PAGE
    httpd_uri_t page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = page_handler,
        .user_ctx = (void *)api,
    };
    err = httpd_register_uri_handler(server, &page);
    if (err != ESP_OK)
        return err;
#endif
    ESP_LOGI(TAG, "HTTP API on port %d", cfg.server_port);
    return ESP_OK;
}
