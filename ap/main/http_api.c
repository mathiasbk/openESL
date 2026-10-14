#include <stdlib.h>
#include "app.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/* esp_http_server glue: reads the request and hands it to esl_api_handle().
 * The server runs all handlers in one task, so requests never overlap. */

static const char *TAG = "http";

static esp_err_t send_json(httpd_req_t *req, int status, const char *body, size_t len)
{
    httpd_resp_set_status(req, esl_api_status_line(status));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (ssize_t)len);
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

    size_t cap = ESL_API_RESP_SIZE(CONFIG_ESL_MAX_TAGS);
    resp = malloc(cap);
    if (!resp)
        goto oom;
    esl_sb_t out;
    esl_sb_init(&out, resp, cap);
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    int status = esl_api_handle(api, method, req->uri, auth, body, len, now_ms, &out);
    ESP_LOGI(TAG, "%s %s -> %d", method, req->uri, status);
    ret = send_json(req, status, out.buf, out.len);
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

#if CONFIG_ESL_STATUS_PAGE
/* GET /: the status page (krav A6). Read-only and without a token. */
static esp_err_t page_handler(httpd_req_t *req)
{
    const esl_api_t *api = req->user_ctx;
    size_t cap = ESL_API_PAGE_SIZE(CONFIG_ESL_MAX_TAGS);
    char *page = malloc(cap);
    if (!page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory");
    }
    esl_sb_t out;
    esl_sb_init(&out, page, cap);
    int status = esl_api_status_page(api, (uint64_t)esp_timer_get_time() / 1000, &out);
    httpd_resp_set_status(req, esl_api_status_line(status));
    httpd_resp_set_type(req, status == 200 ? "text/html; charset=utf-8" : "text/plain");
    esp_err_t ret = httpd_resp_send(req, out.buf, (ssize_t)out.len);
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
