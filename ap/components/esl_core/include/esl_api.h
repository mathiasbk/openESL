#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"
#include "esl_layout.h"
#include "esl_link.h"
#include "esl_sched.h"
#include "esl_store.h"
#include "esl_wifi.h"
#include "esl_health.h"
#include "esl_tagfw.h"
#include "esl_source.h"
#include "esl_radio.h"

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

/* The 802.15.4 transmit power (ESL_TX_POWER_MIN..MAX, esl_link.h), as "tx_power" in
 * GET/PUT /api/config. The radio code owns it: set() applies dbm to the radio and
 * saves it, or with builtin, goes back to the menuconfig value and forgets the saved
 * one. Called under the API's mutex. */
typedef struct {
    int8_t dbm;             /* now */
    int8_t builtin;         /* menuconfig (ESL_TX_POWER_DBM) */
    void (*set)(void *ctx, int8_t dbm, bool builtin);
    void *ctx;
} esl_tx_power_t;
/* The AP's clock settings, "ntp" and "tz" in GET/PUT /api/config, kept by the glue
 * (main/sched.c) in NVS. */
#define ESL_NTP_MAX 63 /* an NTP server's name */
#define ESL_TZ_MAX  63 /* a POSIX TZ string, "CET-1CEST,M3.5.0,M10.5.0/3" */
typedef struct {
    char ntp[ESL_NTP_MAX + 1]; /* the NTP server in use; "" = none */
    char tz[ESL_TZ_MAX + 1];   /* the time zone for local time; "" = UTC */
    const char *builtin_ntp;   /* the server set in menuconfig, which ntp "" goes back to */
    void *ctx;
    /* Saves and uses them at once. ntp: NULL = unchanged, "" = back to builtin_ntp;
       tz: NULL = unchanged, "" = UTC. Fills in ntp and tz above. */
    void (*set)(void *ctx, const char *ntp, const char *tz);
} esl_clock_cfg_t;

/* An NTP server's name: 1 to ESL_NTP_MAX letters, digits, '-' and '.' (a host name
 * or an IPv4 address). A POSIX TZ: up to ESL_TZ_MAX of letters, digits and + - , . / : < >,
 * starting with a name (3 or more letters, or <...>) and its offset ("CET-1...", "<+0530>-5:30");
 * a tzdata name such as "Europe/Oslo" is turned down, since the AP would take it as UTC. */
bool esl_ntp_name_ok(const char *s, size_t len);
bool esl_tz_ok(const char *s, size_t len);

/* GET /api/tags?since= (#270): which tags changed since a client last read the list.
 * Each such read takes a fingerprint (CRC) of every tag's row and stamps the rows that
 * differ from the last read with a new number, seq; the reply gives the client
 * "<boot>-<seq>" to send next time, and holds only the tags stamped after the one it
 * sent. One entry per slot of the tag table, allocated a chunk at a time like the table.
 * boot is set by the glue at start (a random number), so a since from before a restart
 * gets the whole list. Touched only under the API's mutex. */
typedef struct {
    uint32_t crc;  /* the row's fingerprint at the last read; 0 = no tag there */
    uint32_t seq;  /* the read that saw it change */
    uint32_t mac;  /* the low 32 bits of that tag's MAC: another tag in a reused slot means one went */
} esl_api_change_t;
typedef struct {
    uint32_t boot;
    uint32_t seq;
    uint32_t removed_seq; /* the last read that found a tag gone: an older since gets the whole list */
    esl_api_change_t *chunk[ESL_STORE_MAX_CHUNKS];
} esl_api_changes_t;
void esl_api_changes_free(esl_api_changes_t *ch);

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
    const esl_link_stats_t *radio_stats; /* the AP's own radio link (radio.c), as "replies" in GET /api/tags; NULL: left out */
    esl_tx_power_t *tx_power; /* the radio's transmit power (GET/PUT /api/config "tx_power"); NULL: left out */
    esl_sources_t *sources; /* data sources (esl_source.h); NULL: /api/sources answers 404 */
    esl_clock_cfg_t *clock; /* NTP server and time zone (GET/PUT /api/config); NULL: left out, and PUT answers 422 */
    esl_api_changes_t *changes; /* GET /api/tags?since= (above); NULL: since is ignored, the whole list */
    esl_radio_sec_t *sec;   /* radio security (esl_radio_set_sec): "radio_secure" in /api/config, /api/radio-key,
                               /api/tags/{mac}/pair and "secure" per tag; NULL: none of them */
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
 * layout file. ESL_API_RESP_SIZE_ is n batch replies; a tag in the list takes
 * ESL_API_LIST_TAG, with its signal history (esl_store.h) at 11 bytes a POLL. */
#define ESL_API_RESP_SIZE_(n) (512 + 448 * (n))
#define ESL_API_LIST_TAG (640 + 11 * ESL_LINK_HIST)
#define ESL_API_LIST_SIZE_(n) (512 + ESL_API_LIST_TAG * (n))
#define ESL_API_RESP_SIZE(max_tags)                                                        \
    (ESL_API_LIST_SIZE_(max_tags) > ESL_API_RESP_SIZE_(ESL_API_MAX_BATCH) ? ESL_API_LIST_SIZE_(max_tags) \
                                                                          : ESL_API_RESP_SIZE_(ESL_API_MAX_BATCH))
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
