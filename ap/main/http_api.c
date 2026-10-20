#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#if CONFIG_ESL_WEB_DASHBOARD
#include "webui.h"
#endif

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
#if CONFIG_ESL_OTA
    if (status != 401 && status != 429)
        ota_confirm_running(); /* the API answers: new firmware is kept */
#endif
    return status;
}

int api_handle_tagfw_put(const esl_api_t *api, const char *uri, const char *auth, size_t len, api_read_fn read,
                         void *ctx, char *piece, size_t piece_cap, char *resp, size_t resp_cap, esl_sb_t *out,
                         size_t *got)
{
    char puri[ESL_API_PIECE_URI_MAX];
    int status = 200;
    *got = 0;
    esl_sb_init(out, resp, resp_cap);
    while (status == 200 && *got < len) {
        size_t n = len - *got < piece_cap ? len - *got : piece_cap;
        if (!read(ctx, piece, n))
            return -1;
        if (!esl_api_tagfw_piece_uri(uri, len, *got, puri, sizeof(puri)))
            return -1; /* the caller checked it */
        esl_sb_init(out, resp, resp_cap);
        status = api_handle(api, "PUT", puri, auth, piece, n, (uint64_t)esp_timer_get_time() / 1000, out);
        *got += n;
    }
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
    if (out->overflow) {
        static const char msg[] = "{\"error\":\"reply too large\"}";
        ESP_LOGE(TAG, "%s: reply larger than its buffer", req->uri);
        return send_json(req, 500, msg, sizeof(msg) - 1);
    }
    httpd_resp_set_status(req, esl_api_status_line(status));
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, out->buf, (ssize_t)out->len);
}

/* The buffer a request's reply is written into. Only a batch (PUT /api/tags)
 * needs the whole ESL_API_STREAM_BUF (23 KB): what grows with the table is
 * streamed, the largest other reply a GET gives is a layout file, and a PUT or
 * DELETE of one thing answers in at most a kilobyte or two. Asking the heap for no
 * more than that keeps a fragmented heap from answering "out of memory": the
 * dashboard's five GETs every 10 s, and a tag firmware part (16 KB body). */
#define GET_BUF   (ESL_LAYOUT_MAX_SIZE + 2048)
#define SMALL_BUF 4096

static size_t reply_buf_size(const char *method, const char *uri)
{
    if (strcmp(method, "GET") == 0)
        return GET_BUF;
    size_t n = strcspn(uri, "?");
    if (n && uri[n - 1] == '/')
        n--;
    if (strcmp(method, "PUT") == 0 && n == sizeof("/api/tags") - 1 && memcmp(uri, "/api/tags", n) == 0)
        return ESL_API_STREAM_BUF;
    return SMALL_BUF;
}

static void log_oom(const char *what, size_t want)
{
    ESP_LOGE(TAG, "%s: out of memory for %u bytes (free %u, largest block %u)", what, (unsigned)want,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* httpd_req_recv, but a client that sends nothing through RECV_TRIES receive
 * timeouts (recv_wait_timeout, 5 s each) is given up on: the server runs every
 * handler in one task, so waiting for it forever would hang every other request. */
#define RECV_TRIES 3

static int recv_some(httpd_req_t *req, char *buf, size_t len)
{
    int r = HTTPD_SOCK_ERR_TIMEOUT;
    for (int i = 0; i < RECV_TRIES && r == HTTPD_SOCK_ERR_TIMEOUT; i++)
        r = httpd_req_recv(req, buf, len);
    return r;
}

static const char *method_name(int method)
{
    switch (method) {
    case HTTP_GET:    return "GET";
    case HTTP_POST:   return "POST";
    case HTTP_PUT:    return "PUT";
    case HTTP_DELETE: return "DELETE";
    default:          return "?";
    }
}

/* All n bytes of the body, or false */
static bool recv_all(void *ctx, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv_some(ctx, buf + got, n - got);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

static char *get_auth(httpd_req_t *req)
{
    char *auth = NULL;
    size_t al = httpd_req_get_hdr_value_len(req, "Authorization");
    if (al) {
        auth = malloc(al + 1);
        if (!auth || httpd_req_get_hdr_value_str(req, "Authorization", auth, al + 1) != ESP_OK) {
            free(auth);
            auth = NULL;
        }
    }
    return auth;
}

/* PUT /api/tag-firmware: the body goes on in TAGFW_PIECE pieces (api_handle_tagfw_put),
 * so a client's 16 KB part needs no 16 KB block of RAM, which the C6 often lacks. */
#define TAGFW_PIECE 2048
#define TAGFW_RESP  512

static esp_err_t tagfw_put(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    size_t len = req->content_len;
    char *buf = malloc(TAGFW_PIECE + TAGFW_RESP);
    if (!buf) {
        log_oom(req->uri, TAGFW_PIECE + TAGFW_RESP);
        static const char msg[] = "{\"error\":\"out of memory\"}";
        return send_json(req, 500, msg, sizeof(msg) - 1);
    }
    char *auth = get_auth(req);
    esl_sb_t out;
    size_t got;
    int status = api_handle_tagfw_put(api, req->uri, auth, len, recv_all, req, buf, TAGFW_PIECE, buf + TAGFW_PIECE,
                                      TAGFW_RESP, &out, &got);
    free(auth);
    esp_err_t ret;
    if (status < 0) {
        ESP_LOGW(TAG, "PUT %s: body stopped arriving", req->uri);
        ret = ESP_FAIL; /* closes the connection */
    } else {
        ESP_LOGI(TAG, "PUT %s -> %d (%u bytes)", req->uri, status, (unsigned)got);
        /* Drain the rest so the reply gets through, within reason */
        size_t rest = len - got;
        while (rest && rest <= 4 * TAGFW_PIECE) {
            size_t n = rest < TAGFW_PIECE ? rest : TAGFW_PIECE;
            if (!recv_all(req, buf, n))
                break;
            rest -= n;
        }
        ret = send_json(req, status, out.buf, out.len);
    }
    free(buf);
    return ret;
}

static esp_err_t api_handler(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    const char *method = method_name(req->method);
    size_t len = req->content_len;
    char *body = NULL, *auth = NULL, *resp = NULL;
    esp_err_t ret = ESP_OK;

    char puri[ESL_API_PIECE_URI_MAX];
    if (req->method == HTTP_PUT && len && esl_api_tagfw_piece_uri(req->uri, len, 0, puri, sizeof(puri)))
        return tagfw_put(req);

    if (len > ESL_API_MAX_BODY) {
        static const char msg[] = "{\"error\":\"body too large\"}";
        ESP_LOGW(TAG, "%s %s -> 413 (%u bytes)", method, req->uri, (unsigned)len);
        return send_json(req, 413, msg, sizeof(msg) - 1);
    }

    if (len) {
        body = malloc(len);
        if (!body) {
            log_oom(req->uri, len);
            goto oom;
        }
        if (!recv_all(req, body, len)) {
            free(body);
            return ESP_FAIL; /* closes the connection */
        }
    }

    auth = get_auth(req);

    size_t cap = reply_buf_size(method, req->uri);
    resp = malloc(cap);
    if (!resp) {
        log_oom(req->uri, cap);
        goto oom;
    }
    esl_sb_t out;
    sink_t sink = {req, "application/json"};
    esl_sb_init(&out, resp, cap);
    esl_sb_set_sink(&out, http_sink, &sink);
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    int status = api_handle(api, method, req->uri, auth, body, len, now_ms, &out);
    ESP_LOGI(TAG, "%s %s -> %d", method, req->uri, status);
    ret = send_reply(req, status, &out, "application/json");
    goto done;

oom:
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

#if CONFIG_ESL_OTA
/* ---- /api/ota: new AP firmware (krav A10) ------------------------------------- */

/* Checks the token like the rest of the API, with the same lockout (the
 * server runs every handler in one task, so api->lock needs no mutex here);
 * false after sending 401 or 429. */
static bool ota_auth(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    char auth[160] = ""; /* "Bearer " + a token of up to 128 bytes */
    size_t al = httpd_req_get_hdr_value_len(req, "Authorization");
    if (al && al < sizeof(auth))
        httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));
    unsigned wait_s;
    int status = esl_api_auth_check(api, al ? auth : NULL, (uint64_t)esp_timer_get_time() / 1000, &wait_s);
    if (!status)
        return true;
    char msg[80];
    int n = status == 429 ? snprintf(msg, sizeof(msg), "{\"error\":\"too many wrong tokens: locked for %u s\"}", wait_s)
                          : snprintf(msg, sizeof(msg), "{\"error\":\"unauthorized\"}");
    if (status == 401)
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    send_json(req, status, msg, (size_t)n);
    return false;
}

static esp_err_t ota_get(httpd_req_t *req)
{
    if (!ota_auth(req))
        return ESP_OK;
    ota_confirm_running();
    char buf[256];
    esl_sb_t out;
    esl_sb_init(&out, buf, sizeof(buf));
    int status = ota_info(&out);
    return send_json(req, status, out.buf, out.len);
}

/* The body is the image, whole (no query) or one part (?offset=N&total=T);
 * it goes to flash in OTA_CHUNK pieces, so no buffer holds the image. */
#define OTA_CHUNK 4096

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!ota_auth(req))
        return ESP_OK;
    size_t offset, total, len = req->content_len;
    char buf[256];
    esl_sb_t out;
    esl_sb_init(&out, buf, sizeof(buf));
    if (!ota_parse_query(req->uri, &offset, &total)) {
        static const char msg[] = "{\"error\":\"give both offset and total, or neither\"}";
        return send_json(req, 422, msg, sizeof(msg) - 1);
    }
    if (!total)
        total = len; /* the whole image in one request */
    if (!len) {
        static const char msg[] = "{\"error\":\"empty body\"}";
        return send_json(req, 422, msg, sizeof(msg) - 1);
    }
    char *chunk = malloc(OTA_CHUNK);
    if (!chunk) {
        static const char msg[] = "{\"error\":\"out of memory\"}";
        return send_json(req, 500, msg, sizeof(msg) - 1);
    }
    int status = 200;
    size_t got = 0;
    while (status == 200 && got < len) {
        size_t want = len - got < OTA_CHUNK ? len - got : OTA_CHUNK;
        int r = recv_some(req, chunk, want);
        if (r <= 0) {
            ESP_LOGW(TAG, "POST %s: body stopped arriving", req->uri);
            ota_part(0, 0, chunk, 0, &out); /* offset 0 drops the open upload, and size 0 opens no new one */
            free(chunk);
            return ESP_FAIL;
        }
        esl_sb_init(&out, buf, sizeof(buf));
        status = ota_part(offset + got, total, chunk, (size_t)r, &out);
        got += (size_t)r;
    }
    free(chunk);
    ESP_LOGI(TAG, "POST %s -> %d (%u bytes)", req->uri, status, (unsigned)got);
    if (status != 200 && got < len) {
        /* Drain the rest so the reply gets through, within reason */
        size_t rest = len - got;
        char junk[128];
        while (rest && rest <= 4 * OTA_CHUNK) {
            int r = recv_some(req, junk, rest < sizeof(junk) ? rest : sizeof(junk));
            if (r <= 0)
                break;
            rest -= (size_t)r;
        }
    }
    return send_json(req, status, out.buf, out.len);
}
#endif

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
        int r = recv_some(req, body + got, len - got);
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
    int status = esl_wifi_setup_post(api->wifi, api->token, api->fw_version, body, len, api->lock,
                                     (uint64_t)esp_timer_get_time() / 1000, &out);
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
    char *page = malloc(GET_BUF); /* streamed: grows with the table in pieces of this size */
    if (!page) {
        log_oom("/", GET_BUF);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory");
    }
    esl_sb_t out;
    sink_t sink = {req, "text/html; charset=utf-8"};
    esl_sb_init(&out, page, GET_BUF);
    esl_sb_set_sink(&out, http_sink, &sink);
    int status = esl_api_status_page(api, (uint64_t)esp_timer_get_time() / 1000, &out);
    esp_err_t ret = send_reply(req, status, &out, status == 200 ? sink.type : "text/plain");
    free(page);
    return ret;
}
#endif

#if CONFIG_ESL_WEB_DASHBOARD
/* GET /dashboard and /ui/...: the web page's dashboard, gzipped in flash
 * (webui.h). No token: the page asks for it and sends it with each API call. */
static esp_err_t webui_handler(httpd_req_t *req)
{
    char path[64];
    strlcpy(path, req->uri, sizeof(path));
    char *q = strchr(path, '?');
    if (q)
        *q = 0;
    for (const webui_file_t *f = webui_files; f->path; f++) {
        if (strcmp(f->path, path) != 0)
            continue;
        httpd_resp_set_type(req, f->type);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        /* new firmware brings new files; the browser asks again after an update */
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, (const char *)f->gz, (ssize_t)f->len);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}
#endif

#if CONFIG_ESL_WIFI_CAPTIVE
/* Any other GET while the setup network is open, asked for by a name that is
 * not the AP's own address (a phone's connectivity probe, which captive.c's
 * DNS sent here): a redirect to the form, which makes the phone show its
 * "sign in to network" page. Everything else is 404 as before. */
static esp_err_t captive_handler(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    char host[48] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    bool setup_open = false;
    if (api->wifi && strcmp(host, "192.168.4.1") != 0) {
        esl_wifi_status_t *ws = malloc(sizeof(*ws));
        if (ws) {
            api->wifi->status(api->wifi->ctx, ws);
            setup_open = ws->setup_ssid[0] != 0;
            free(ws);
        }
    }
    if (!setup_open) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}
#endif

esp_err_t http_api_start(const esl_api_t *api)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    /* A browser keeps connections open, and the dashboard's page loads its files over
       several at once: let a new one close the oldest instead of being refused when
       all are taken. Each open socket also holds RAM in lwIP. */
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 15; /* two for /api/ota, four for the /api wildcard, two for /setup, /, two for the dashboard, and the rest */

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK)
        return err;

#if CONFIG_ESL_OTA
    /* Before the wildcard: the first registered match wins */
    httpd_uri_t ota_uris[] = {
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post, .user_ctx = (void *)api},
        {.uri = "/api/ota", .method = HTTP_GET, .handler = ota_get, .user_ctx = (void *)api},
    };
    for (size_t i = 0; i < sizeof(ota_uris) / sizeof(ota_uris[0]); i++) {
        err = httpd_register_uri_handler(server, &ota_uris[i]);
        if (err != ESP_OK)
            return err;
    }
#endif
    static const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE};
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
#if CONFIG_ESL_WEB_DASHBOARD
    httpd_uri_t ui[] = {
        {.uri = "/dashboard", .method = HTTP_GET, .handler = webui_handler, .user_ctx = (void *)api},
        {.uri = "/ui/*", .method = HTTP_GET, .handler = webui_handler, .user_ctx = (void *)api},
    };
    for (size_t i = 0; i < sizeof(ui) / sizeof(ui[0]); i++) {
        err = httpd_register_uri_handler(server, &ui[i]);
        if (err != ESP_OK)
            return err;
    }
#endif
#if CONFIG_ESL_WIFI_CAPTIVE
    if (api->wifi) {
        /* Last: the first registered match wins, so this only gets what nothing else took */
        httpd_uri_t rest = {.uri = "/*", .method = HTTP_GET, .handler = captive_handler, .user_ctx = (void *)api};
        err = httpd_register_uri_handler(server, &rest);
        if (err != ESP_OK)
            return err;
    }
#endif
    ESP_LOGI(TAG, "HTTP API on port %d", cfg.server_port);
    return ESP_OK;
}
