/* /api/tag-firmware: tag firmware files for OTA (docs/tag-ota.md). */
#include "esl_api_int.h"

/* ?key=N in the query string; false if absent or not a number. */
static bool query_size(const char *uri, const char *key, size_t *v)
{
    const char *q = strchr(uri, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (strncmp(q, key, kl) == 0 && q[kl] == '=' && q[kl + 1] >= '0' && q[kl + 1] <= '9') {
            *v = (size_t)strtoul(q + kl + 1, NULL, 10);
            return true;
        }
        q = strchr(q, '&');
    }
    return false;
}

bool esl_api_tagfw_piece_uri(const char *uri, size_t body_len, size_t done, char *buf, size_t cap)
{
    const size_t fbase = sizeof(PATH_TAGFW) - 1;
    if (strcspn(uri, "?") != fbase || memcmp(uri, PATH_TAGFW, fbase) != 0)
        return false;
    size_t offset = 0, total = body_len, tl = 0;
    bool has_off = query_size(uri, "offset", &offset), has_total = query_size(uri, "total", &total);
    if (has_off != has_total)
        return false;
    const char *to = api_query_str(uri, "to", &tl);
    int n = snprintf(buf, cap, PATH_TAGFW "?offset=%lu&total=%lu%s%.*s", (unsigned long)(offset + done),
                     (unsigned long)total, to ? "&to=" : "", to ? (int)tl : 0, to ? to : "");
    return n > 0 && (size_t)n < cap;
}

static void write_tagfw_model(const esl_api_t *api, esl_sb_t *out, uint8_t model)
{
    esl_tagfw_img_t img;
    esl_tagfw_to_t to;
    if (!esl_tagfw_get(api->tagfw, model, &img) || !esl_tagfw_get_to(api->tagfw, model, &to)) {
        esl_sb_printf(out, "{\"model\":\"%s\",\"version\":null}", esl_model_name(model));
        return;
    }
    /* the seen tags of the model; progress counts the bytes of those it is for */
    unsigned n[FWU_UPDATING + 1] = {0};
    uint32_t received = 0, total = 0;
    for (unsigned s = 0, slots = esl_store_slots(api->store); s < slots; s++) {
        esl_tag_t t;
        uint16_t got;
        if (!esl_store_get_slot(api->store, s, &t) || !t.seen || t.model != model)
            continue;
        fwu_t u = api_tag_fw_update(api, &t, &img, &got);
        n[u]++;
        if (u == FWU_UPDATED && esl_tagfw_for(api->tagfw, model, t.mac)) {
            received += img.size;
            total += img.size;
        } else if (u == FWU_UPDATING) {
            received += got;
            total += img.size;
        }
    }
    esl_sb_putc(out, '{');
    esl_tagfw_write_fields(out, model, &img);
    esl_sb_puts(out, ",\"to\":");
    if (to.all) {
        esl_sb_puts(out, "\"all\"");
    } else {
        esl_sb_putc(out, '[');
        for (unsigned i = 0; i < to.n; i++) {
            char m[17];
            esl_mac_format(to.mac[i], m);
            esl_sb_printf(out, "%s\"%s\"", i ? "," : "", m);
        }
        esl_sb_putc(out, ']');
    }
    esl_sb_printf(out,
                  ",\"tags\":{\"updated\":%u,\"pending\":%u,\"failed\":%u,\"no_ota\":%u,\"not_chosen\":%u},"
                  "\"progress\":{\"received\":%" PRIu32 ",\"total\":%" PRIu32 "}}",
                  n[FWU_UPDATED], n[FWU_UPDATING], n[FWU_FAILED], n[FWU_NO_OTA], n[FWU_NOT_CHOSEN], received, total);
}

static int handle_tagfw(const esl_api_t *api, const char *method, const char *uri, size_t plen,
                        const char *body, size_t body_len, esl_sb_t *out)
{
    const size_t base = sizeof(PATH_TAGFW) - 1;

    if (plen == base || (plen == base + 1 && uri[base] == '/')) {
        if (strcmp(method, "GET") == 0) {
            esl_sb_printf(out, "{\"max_size\":%u,\"part_max\":%u,\"images\":[", (unsigned)(ESL_FWF_HDR_LEN + ESL_FW_MAX),
                          (unsigned)ESL_API_MAX_BODY);
            for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
                if (m > 1)
                    esl_sb_putc(out, ',');
                write_tagfw_model(api, out, m);
            }
            esl_sb_puts(out, "]}");
            return 200;
        }
        if (strcmp(method, "PUT") != 0)
            return api_error(out, 405, "method not allowed");
        size_t offset = 0, total = body_len;
        bool has_off = query_size(uri, "offset", &offset), has_total = query_size(uri, "total", &total);
        if (has_off != has_total)
            return api_error(out, 400, "give both offset and total, or neither");
        if (!body || body_len == 0)
            return api_error(out, 400, "missing body: the tag firmware file (openesl-tag-<model>-<version>-ota.bin)");
        if (total > ESL_FWF_HDR_LEN + ESL_FW_MAX)
            return api_error(out, 413, "the file is larger than a tag image can be");
        uint64_t only;
        if (!api_query_to(uri, &only))
            return api_error(out, 400, "to must be all or a tag's MAC (16 hex digits)");
        return esl_tagfw_part(api->tagfw, offset, total, (const uint8_t *)body, body_len, only, out);
    }
    if (uri[base] != '/')
        return api_error(out, 404, "not found");
    size_t ml = plen - base - 1;
    if (ml > 0 && uri[base + ml] == '/')
        ml--;
    uint8_t model = esl_model_parse(&uri[base + 1], ml);
    if (model == ESL_MODEL_UNKNOWN)
        return api_error(out, 404, "unknown model (use 2.9 or 1.54)");
    if (strcmp(method, "GET") == 0) {
        if (!esl_tagfw_get(api->tagfw, model, NULL))
            return api_error(out, 404, "no tag firmware for this model");
        write_tagfw_model(api, out, model);
        return 200;
    }
    if (strcmp(method, "POST") == 0) {
        uint64_t mac;
        size_t tl;
        if (!api_query_str(uri, "to", &tl) || !api_query_to(uri, &mac))
            return api_error(out, 400, "give to=all or to= a tag's MAC (16 hex digits)");
        int status = esl_tagfw_choose(api->tagfw, model, mac);
        if (status == 404)
            return api_error(out, 404, "no tag firmware for this model");
        if (status == 409)
            return api_error(out, 409, "too many tags chosen one by one: give it to all of them (to=all)");
        write_tagfw_model(api, out, model);
        return 200;
    }
    if (strcmp(method, "DELETE") == 0) {
        if (!esl_tagfw_delete(api->tagfw, model))
            return api_error(out, 404, "no tag firmware for this model");
        esl_sb_printf(out, "{\"model\":\"%s\",\"deleted\":true}", esl_model_name(model));
        return 200;
    }
    return api_error(out, 405, "method not allowed");
}

int api_route_tagfw(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return handle_tagfw(api, rq->method, rq->uri, rq->plen, rq->body, rq->body_len, out);
}
