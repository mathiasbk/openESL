#pragma once
/* Private to the esl_api*.c files: what they share. The public API is esl_api.h. */
#include "esl_api.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_render.h"
#include "esl_text.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The AP board (ap/boards/<board>/, set by ap/CMakeLists.txt); "host" in the host tests and sim */
#ifndef ESL_BOARD
#define ESL_BOARD "host"
#endif
#ifndef ESL_BOARD_NAME
#define ESL_BOARD_NAME "host"
#endif

/* The paths, as ap/test/openapi/check_openapi.py reads them */
#define PATH_TAGS "/api/tags"
#define PATH_LAYOUTS "/api/layouts"
#define PATH_FONTS "/api/fonts"
#define PATH_CONFIG "/api/config"
#define PATH_WIFI "/api/wifi"
#define PATH_TIME "/api/time"
#define PATH_SCHEDULE "/api/schedule"
#define PATH_HEALTH "/api/health"
#define PATH_OTA "/api/ota"
#define PATH_TAGFW "/api/tag-firmware"
#define PATH_SOURCES "/api/sources"
#define PATH_RADIOS "/api/radios"

typedef struct {
    const char *js;
    const esl_jtok_t *tok;
} json_t;

/* Room for an error message built with snprintf */
#define MSG_MAX 192

typedef enum { FWU_NONE, FWU_UPDATED, FWU_NO_OTA, FWU_FAILED, FWU_NOT_CHOSEN, FWU_UPDATING } fwu_t;

/* One request, as esl_api_handle() got it; plen is the length of the path without the query */
typedef struct {
    const char *method;
    const char *uri;
    size_t plen;
    const char *body;
    size_t body_len;
    uint64_t now_ms;
} esl_api_req_t;

/* A route's handler (the table in esl_api.c): the status, with the reply in out */
typedef int esl_api_route_fn(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out);

esl_api_route_fn api_route_tags;      /* esl_api_tags.c */
esl_api_route_fn api_route_layouts;   /* esl_api_layouts.c */
esl_api_route_fn api_route_fonts;     /* esl_api_layouts.c */
esl_api_route_fn api_route_tagfw;     /* esl_api_tagfw.c */
esl_api_route_fn api_route_config;    /* esl_api_config.c */
esl_api_route_fn api_route_health;    /* esl_api_health.c */
esl_api_route_fn api_route_wifi;      /* esl_api_wifi.c */
esl_api_route_fn api_route_radios;    /* esl_api_radios.c */
esl_api_route_fn api_route_time;      /* esl_api_time.c */
esl_api_route_fn api_route_schedule;  /* esl_api_time.c */

/* esl_api_util.c */
void api_sb_reset(esl_sb_t *sb);
int api_error_at(esl_sb_t *out, int status, const char *msg, int index);
int api_error(esl_sb_t *out, int status, const char *msg);
void api_leave_source(const esl_api_t *api, uint64_t mac);
int api_parse_bool(const json_t *j, int obj, const char *key, bool *out);
int api_parse_uint(const json_t *j, int obj, const char *key, unsigned min, unsigned max, uint16_t *out);
int api_parse_json(const char *body, size_t len, esl_jtok_t **tok, esl_sb_t *out);
const char *api_query_str(const char *uri, const char *key, size_t *len);
bool api_query_to(const char *uri, uint64_t *mac);

/* esl_api_tags.c */
void api_write_set_result(esl_sb_t *out, uint64_t mac, const esl_set_result_t *r);
void api_write_content(esl_sb_t *out, const esl_content_t *c);
fwu_t api_tag_fw_update(const esl_api_t *api, const esl_tag_t *t, const esl_tagfw_img_t *img, uint16_t *got);
bool api_seen_s_ago(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms, uint64_t *s);
const char *api_store_err_text(esl_store_err_t err);

/* esl_api_radios.c */
const esl_radio_link_t *api_radio_links(const esl_api_t *api, unsigned *n);
uint64_t api_radio_lost_s(const esl_radio_link_t *l, uint64_t now_ms);
bool api_radio_is_lost(const esl_radio_link_t *l, unsigned i, uint64_t now_ms);
void api_write_link_state(esl_sb_t *out, const esl_radio_link_t *l, unsigned i, uint64_t now_ms);
void api_write_radios(const esl_api_t *api, esl_sb_t *out, uint8_t ap_channel, uint64_t now_ms);
bool api_several_radios(const esl_api_t *api);

/* esl_api_image.c: GET and PUT /api/tags/{mac}/image */
int api_tag_image(const esl_api_t *api, uint64_t mac, const esl_api_req_t *rq, esl_sb_t *out);
