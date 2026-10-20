#include "esl_health.h"
#include <string.h>

uint32_t esl_health_silent_s(const esl_health_cfg_t *cfg, uint16_t interval_s)
{
    if (cfg && cfg->silent_s)
        return cfg->silent_s;
    if (!interval_s)
        interval_s = ESL_HEALTH_TAG_DEFAULT_POLL_S;
    return (uint32_t)ESL_HEALTH_SILENT_X * interval_s;
}

uint8_t esl_health_flags(const esl_tag_t *t, uint16_t interval_s, uint32_t layout_ver, uint64_t now_ms,
                         const esl_health_cfg_t *cfg)
{
    uint8_t f = 0;
    uint64_t silent_ms = (uint64_t)esl_health_silent_s(cfg, interval_s) * 1000;
    uint16_t low = cfg && cfg->battery_mv ? cfg->battery_mv : ESL_HEALTH_BATTERY_MV_DEFAULT;

    if (!t->seen) {
        /* It is in the table from NVS; give it the same time as a seen tag
           before it counts as gone */
        if (now_ms >= silent_ms)
            f |= ESL_HEALTH_UNSEEN;
        return f | ((t->has_content && esl_tag_pending(t) && t->content_err) ? ESL_HEALTH_CONTENT_ERROR : 0);
    }
    if (now_ms - t->last_seen_ms >= silent_ms)
        f |= ESL_HEALTH_SILENT;
    if (t->poll_flags & ESL_POLL_FLAG_DRAW_FAILED)
        f |= ESL_HEALTH_DRAW_FAILED;
    if (t->battery_mv && t->battery_mv < low)
        f |= ESL_HEALTH_LOW_BATTERY;
    if (t->layout_capable && !t->image_capable && layout_ver && t->layout_failed == layout_ver)
        f |= ESL_HEALTH_LAYOUT_FAILED;
    if (t->has_content && esl_tag_pending(t) && t->content_err)
        f |= ESL_HEALTH_CONTENT_ERROR;
    return f;
}

const char *esl_health_flag_name(uint8_t flag)
{
    switch (flag) {
    case ESL_HEALTH_SILENT:        return "silent";
    case ESL_HEALTH_UNSEEN:        return "unseen";
    case ESL_HEALTH_DRAW_FAILED:   return "draw_failed";
    case ESL_HEALTH_LOW_BATTERY:   return "low_battery";
    case ESL_HEALTH_LAYOUT_FAILED: return "layout_failed";
    case ESL_HEALTH_CONTENT_ERROR: return "content_error";
    default:                       return "?";
    }
}

bool esl_webhook_url_ok(const char *url, size_t len)
{
    static const char http[] = "http://";
    if (len > ESL_WEBHOOK_URL_MAX || len < sizeof(http) || memcmp(url, http, sizeof(http) - 1) != 0)
        return false;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)url[i] <= ' ' || (unsigned char)url[i] >= 0x7F || url[i] == '"' || url[i] == '\\')
            return false;
    return url[sizeof(http) - 1] != '/'; /* a host must follow */
}
