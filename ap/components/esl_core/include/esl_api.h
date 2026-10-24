#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"
#include "esl_layout.h"
#include "esl_sched.h"
#include "esl_store.h"
#include "esl_wifi.h"
#include "esl_health.h"
#include "esl_tagfw.h"

/* The HTTP API from docs/protokoll.md part 2, independent of the HTTP server.
 * The ESP-IDF glue (main/http_api.c) reads the request and hands it here; the
 * host tests call it directly. */

/* The network radio's link as the AP sees it (ap/main/radio_net.c), shown as "radio" in
 * GET /api/tags. The driver keeps it up to date. */
typedef struct {
    bool up;                /* a radio is connected now */
    uint32_t ip;            /* its address, host order */
    uint64_t seen_ms;       /* when it last sent anything (ms since boot) */
    uint16_t ver;           /* its firmware, as its HELLO says (0 before HELLO) */
} esl_radio_link_t;

#define ESL_API_MAX_BATCH  50
#define ESL_API_MAX_BODY   24576
#define ESL_API_MAX_TOKENS 1024

typedef struct {
    esl_store_t *store;
    const char *token;      /* bearer token; empty or NULL rejects every request */
    const char *fw_version; /* reported in GET /api/tags */
    uint8_t channel;
    esl_layouts_t *layouts; /* NULL: the /api/layouts endpoints answer 404 */
    const char *reset_reason; /* why the AP last restarted, as "reset" in GET /api/tags; NULL leaves it out */
    const esl_wifi_ops_t *wifi; /* NULL: /api/wifi answers 404 */
    esl_sched_t *sched;     /* NULL: no scheduled content; /api/time and /api/schedule answer 404 */
    esl_notify_t *notify;   /* the webhook (GET/PUT /api/config "webhook"); NULL: no webhook, shown as null */
    struct esl_api_lock *lock; /* lockout after wrong tokens (below); NULL: none, for a transport without a token */
    esl_tagfw_t *tagfw;     /* tag firmware images (docs/tag-ota.md); NULL: /api/tag-firmware answers 404 */
    bool dashboard;         /* the AP serves the dashboard at /dashboard: the status page links to it */
    const esl_radio_link_t *link; /* the network radio (radio_net.c); NULL: no "radio" in GET /api/tags */
} esl_api_t;

/* Against guessing the token: after ESL_API_AUTH_TRIES wrong tokens in a row the
 * API answers 429 to every request for ESL_API_AUTH_LOCK_S seconds, without
 * looking at the token; a right token resets the count. One per transport,
 * touched only by that transport's task. */
typedef struct esl_api_lock {
    uint8_t fails;
    uint64_t locked_until_ms;
} esl_api_lock_t;
#define ESL_API_AUTH_TRIES  5
#define ESL_API_AUTH_LOCK_S 60

/* Response buffer that always fits a full GET /api/tags, a batch reply and a
 * layout file. */
#define ESL_API_RESP_SIZE_(n) (512 + 448 * (n))
#define ESL_API_RESP_SIZE(max_tags) \
    ESL_API_RESP_SIZE_((max_tags) > ESL_API_MAX_BATCH ? (max_tags) : ESL_API_MAX_BATCH)
/* Buffer for a glue that streams (esl_sb_set_sink): every reply that is not
 * streamed (a batch reply, a layout file, errors) fits in it, and GET
 * /api/tags and the status page, which grow with the tag table, come through
 * the sink in pieces of this size. */
#define ESL_API_STREAM_BUF ESL_API_RESP_SIZE_(ESL_API_MAX_BATCH)

/* True if auth is "Bearer <token>" (case-insensitive scheme), in constant
 * time; false when the AP has no token. For transports that handle a request
 * themselves (main/ota.c). */
bool esl_api_auth_ok(const char *token, const char *auth);

/* esl_api_auth_ok() with api->lock: 0 when the request may go on, 401 for a
 * wrong token, 429 while locked (then *wait_s is the seconds left). */
int esl_api_auth_check(const esl_api_t *api, const char *auth, uint64_t now_ms, unsigned *wait_s);
/* The two halves, for a check that compares the token itself (the setup
 * form): esl_api_lock_wait() is the seconds left of a lock, 0 when there is
 * none (or lock is NULL); esl_api_lock_note() counts a right or wrong token.
 * Both are safe with lock NULL. */
unsigned esl_api_lock_wait(const esl_api_lock_t *lock, uint64_t now_ms);
void esl_api_lock_note(esl_api_lock_t *lock, bool ok, uint64_t now_ms);

/* Handles one request. method is "GET"/"PUT"/"DELETE", uri may contain a query
 * string, auth is the Authorization header (or NULL), now_ms is time since
 * boot. Writes a JSON body to out and returns the HTTP status code. */
int esl_api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
                   const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out);

/* PUT /api/tag-firmware in pieces, for a transport that will not hold a whole part in
 * one block of RAM (a 16 KB block is often not to be had on the ESP32-C6): the URI under
 * which the bytes from done on of a body of body_len bytes sent to uri go to
 * esl_api_handle() as a part of their own (?offset=&total=, and to= as it was), in buf
 * (cap bytes). False when uri is not PUT /api/tag-firmware with offset and total both
 * given or both left out (the whole file in one body); such a request goes whole. */
#define ESL_API_PIECE_URI_MAX 96
bool esl_api_tagfw_piece_uri(const char *uri, size_t body_len, size_t done, char *buf, size_t cap);

/* PUT /api/tags/{mac}/image, for a transport that streams the body: a PNG can be larger
 * than ESL_API_MAX_BODY (up to ESL_API_MAX_IMAGE_BODY) and is decoded as it arrives
 * (esl_png.h), so it never has to be in RAM whole. esl_api_image_put_uri() tells such a
 * request; esl_api_image_put() handles it like esl_api_handle() (the token too), pulling
 * the len bytes of the body through read (exactly n bytes, or false). It may stop
 * before the end of the body (an error, or bytes after the PNG's end): the rest is the
 * caller's to drop. esl_api_handle() takes the same request with the body in RAM.
 * The caller holds the lock it keeps around esl_api_handle(); hold (may be NULL) lets
 * it go (false) while the body comes in and is decoded, so a slow client does not hold
 * up other requests, and takes it again (true) to store the picture. */
#define ESL_API_MAX_IMAGE_BODY (512 * 1024)
bool esl_api_image_put_uri(const char *method, const char *uri);
int esl_api_image_put(const esl_api_t *api, const char *uri, const char *auth, size_t len,
                      bool (*read)(void *ctx, uint8_t *buf, size_t n), void *ctx, void (*hold)(bool take),
                      uint64_t now_ms, esl_sb_t *out);

/* GET /api/health as JSON: every tag with a problem (esl_health_flags) and
 * counts per problem. prev (the store's max bytes, may be NULL) holds the flags of the
 * last report: the tags whose flags differ are listed under "changed", and
 * every tag's new flags are written back to it. Returns the HTTP status;
 * streams like GET /api/tags. */
int esl_api_health(const esl_api_t *api, uint64_t now_ms, const esl_health_cfg_t *cfg, uint8_t *prev,
                   esl_sb_t *out);

/* What the dashboard shows for a tag (tagState in web/js/dashdata.js): BAD when
 * GET /api/health lists a problem for it, BUSY when its poll is late or it is
 * still getting firmware, content or settings, OK otherwise. The AP's status
 * screen counts its tags by this. */
typedef enum { ESL_TAG_OK, ESL_TAG_BUSY, ESL_TAG_BAD } esl_tag_level_t;
#define ESL_TAG_CHECKIN_GRACE_S 15 /* CHECKIN_GRACE_S in dashdata.js: late after poll_s + this */
esl_tag_level_t esl_api_tag_level(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms);

/* The status page at / (krav A6): an HTML table with the same data as
 * GET /api/tags. Needs no token, since it only reads. Returns the HTTP status. */
int esl_api_status_page(const esl_api_t *api, uint64_t now_ms, esl_sb_t *out);
/* Buffer that always fits the status page. */
#define ESL_API_PAGE_SIZE(max_tags) (2816 + 1024 * (max_tags))

/* "200 OK", "401 Unauthorized", ... for esp_http_server. */
const char *esl_api_status_line(int status);
