/* /api/health and the tag levels the health check and the webhook use. */
#include "esl_api_int.h"

/* A number from the query string (?silent_s=600&battery_mv=2400); 0 if absent. */
static uint16_t query_u16(const char *uri, const char *key)
{
    const char *q = strchr(uri, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            unsigned long v = strtoul(q + kl + 1, NULL, 10);
            return v > 65535 ? 65535 : (uint16_t)v;
        }
        q = strchr(q, '&');
    }
    return 0;
}

/* The ESL_HEALTH_* flags of one tag in the table. */
static uint8_t tag_health(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms, const esl_health_cfg_t *cfg)
{
    return esl_health_flags(t, esl_store_tag_poll_s(api->store, t->mac), now_ms, cfg);
}

int esl_api_health(const esl_api_t *api, uint64_t now_ms, const esl_health_cfg_t *cfg, uint8_t *prev,
                   esl_sb_t *out)
{
    unsigned counts[8] = {0}, tags = 0, problems = 0, changed = 0;
    esl_sb_stream(out, 200); /* one row per tag with a problem: can be larger than the buffer */
    esl_sb_puts(out, "{\"problems\":[");
    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
        esl_tag_t t;
        if (!esl_store_get_slot(api->store, s, &t)) {
            if (prev)
                prev[s] = 0;
            continue;
        }
        uint8_t f = tag_health(api, &t, now_ms, cfg);
        tags++;
        if (prev) {
            if (prev[s] != f)
                changed++;
            prev[s] = f;
        }
        if (!f)
            continue;
        char m[17];
        esl_mac_format(t.mac, m);
        esl_sb_printf(out, "%s{\"mac\":\"%s\",\"problems\":[", problems ? "," : "", m);
        bool first = true;
        for (unsigned b = 0; b < 8; b++) {
            if (!(f & (1u << b)))
                continue;
            counts[b]++;
            esl_sb_printf(out, "%s\"%s\"", first ? "" : ",", esl_health_flag_name((uint8_t)(1u << b)));
            first = false;
        }
        if (t.seen)
            esl_sb_printf(out, "],\"seen_s_ago\":%" PRIu64 ",\"battery_mv\":%u}", esl_tag_seen_ms_ago(&t, now_ms) / 1000,
                          (unsigned)t.battery_mv);
        else
            esl_sb_puts(out, "],\"seen_s_ago\":null,\"battery_mv\":null}");
        problems++;
    }
    /* the radio coprocessors that are gone: no tag hears from the AP through them */
    unsigned nl, lost = 0;
    const esl_radio_link_t *links = api_radio_links(api, &nl);
    esl_sb_puts(out, "],\"radio_problems\":[");
    for (unsigned i = 0; i < nl; i++) {
        const esl_radio_link_t *l = &links[i];
        if (!api_radio_is_lost(l, i, now_ms))
            continue;
        esl_sb_printf(out, "%s{\"radio\":%u,\"id\":", lost ? "," : "", i);
        if (l->id) {
            char id[17];
            esl_mac_format(l->id, id);
            esl_sb_printf(out, "\"%s\"", id);
        } else {
            esl_sb_puts(out, "null");
        }
        esl_sb_printf(out, ",\"via\":\"%s\",\"problems\":[\"radio_lost\"],\"lost_s\":%" PRIu64 "}",
                      l->uart ? "uart" : "wifi", api_radio_lost_s(l, now_ms));
        lost++;
    }
    /* other APs' tags that poll here and are turned away (#390): adopt them to take them over */
    esl_other_ap_t other[ESL_STORE_OTHER_APS];
    unsigned no = esl_store_other_aps(api->store, other, ESL_STORE_OTHER_APS);
    esl_sb_puts(out, "],\"other_aps\":[");
    for (unsigned i = 0; i < no; i++) {
        esl_sb_printf(out, "%s{\"ap_id\":\"%08" PRIX32 "\",\"tags\":%u,\"polls\":%" PRIu32 ",\"seen_s_ago\":%" PRIu64
                      ",\"lost_long\":%s,\"answered_s_ago\":", i ? "," : "", other[i].ap_id, other[i].tags, other[i].polls,
                      (now_ms - other[i].last_ms) / 1000, other[i].lost_long ? "true" : "false");
        if (other[i].answered_ms)
            esl_sb_printf(out, "%" PRIu64 "}", (now_ms - other[i].answered_ms) / 1000);
        else
            esl_sb_puts(out, "null}");
    }
    esl_sb_printf(out, "],\"ok\":%s,\"tags\":%u,\"with_problems\":%u,\"radio_lost\":%u",
                  problems || lost ? "false" : "true", tags, problems, lost);
    for (unsigned b = 0; b < 8; b++) {
        uint8_t flag = (uint8_t)(1u << b);
        if (strcmp(esl_health_flag_name(flag), "?") != 0)
            esl_sb_printf(out, ",\"%s\":%u", esl_health_flag_name(flag), counts[b]);
    }
    if (prev)
        esl_sb_printf(out, ",\"changed\":%u", changed);
    esl_sb_putc(out, '}');
    return 200;
}

esl_tag_level_t esl_api_tag_level(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms)
{
    esl_health_cfg_t cfg = {0, 0}; /* the defaults, as the dashboard asks GET /api/health */
    if (tag_health(api, t, now_ms, &cfg))
        return ESL_TAG_BAD;
    esl_tag_cfg_t ap, eff;
    esl_store_tag_defaults(api->store, &ap);
    ap = esl_store_tag_ap(api->store, t, &ap);
    esl_tag_effective_cfg(t, &ap, &eff); /* "poll_s" in GET /api/tags */
    uint64_t poll_s = eff.poll_s ? eff.poll_s : ESL_TAG_BUILTIN_POLL_S;
    uint64_t ago; /* "seen_s_ago": late also for a tag from NVS not heard since this boot */
    if (api_seen_s_ago(api, t, now_ms, &ago) && ago > poll_s + ESL_TAG_CHECKIN_GRACE_S)
        return ESL_TAG_BUSY;
    esl_tagfw_img_t img;
    uint16_t got;
    if (t->seen && esl_tagfw_get(api->tagfw, t->model, &img) && api_tag_fw_update(api, t, &img, &got) == FWU_UPDATING)
        return ESL_TAG_BUSY;
    if (esl_tag_pending(t) || esl_tag_cfg_pending(t, &ap))
        return ESL_TAG_BUSY;
    return ESL_TAG_OK;
}

static int get_health(const esl_api_t *api, const char *method, const char *uri, uint64_t now_ms, esl_sb_t *out)
{
    if (strcmp(method, "GET") != 0)
        return api_error(out, 405, "method not allowed");
    esl_health_cfg_t cfg = {query_u16(uri, "silent_s"), query_u16(uri, "battery_mv")};
    return esl_api_health(api, now_ms, &cfg, NULL, out);
}

int api_route_health(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return get_health(api, rq->method, rq->uri, rq->now_ms, out);
}
