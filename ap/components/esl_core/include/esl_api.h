#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"
#include "esl_store.h"

/* The HTTP API from docs/protokoll.md part 2, independent of the HTTP server.
 * The ESP-IDF glue (main/http_api.c) reads the request and hands it here; the
 * host tests call it directly. */

#define ESL_API_MAX_BATCH  50
#define ESL_API_MAX_BODY   24576
#define ESL_API_MAX_TOKENS 1024

typedef struct {
    esl_store_t *store;
    const char *token;      /* bearer token; empty or NULL rejects every request */
    const char *fw_version; /* reported in GET /api/tags */
    uint8_t channel;
} esl_api_t;

/* Response buffer that always fits a full GET /api/tags or batch reply. */
#define ESL_API_RESP_SIZE(max_tags) (512 + 320 * ((max_tags) > ESL_API_MAX_BATCH ? (max_tags) : ESL_API_MAX_BATCH))

/* Handles one request. method is "GET"/"PUT"/"DELETE", uri may contain a query
 * string, auth is the Authorization header (or NULL), now_ms is time since
 * boot. Writes a JSON body to out and returns the HTTP status code. */
int esl_api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
                   const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out);

/* "200 OK", "401 Unauthorized", ... for esp_http_server. */
const char *esl_api_status_line(int status);
