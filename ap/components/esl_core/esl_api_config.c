/* /api/config: the AP's settings, the radio channel and the transmit power. */
#include "esl_api_int.h"
#include "esl_clock.h"

_Static_assert(ESL_CFG_POLL_MIN_S == 10 && ESL_CFG_POLL_MAX_S == 3600 && ESL_CFG_LOST_AP_MIN_S == 60,
               "update the config error messages");
_Static_assert(ESL_NTP_MAX == 63 && ESL_TZ_MAX == 63, "update the ntp and tz error messages");

/* "tx_power": a whole number of dBm from ESL_TX_POWER_MIN to ESL_TX_POWER_MAX. */
static bool parse_dbm(const json_t *j, int v, int8_t *out)
{
    const esl_jtok_t *t = &j->tok[v];
    int i = t->start, n = 0;
    bool neg = i < t->end && j->js[i] == '-';
    if (t->type != ESL_JSON_NUMBER || t->end - i - neg < 1 || t->end - i - neg > 2)
        return false;
    for (i += neg; i < t->end; i++) {
        if (j->js[i] < '0' || j->js[i] > '9')
            return false;
        n = n * 10 + (j->js[i] - '0');
    }
    n = neg ? -n : n;
    if (n < ESL_TX_POWER_MIN || n > ESL_TX_POWER_MAX)
        return false;
    *out = (int8_t)n;
    return true;
}

bool esl_ntp_name_ok(const char *s, size_t len)
{
    if (len < 1 || len > ESL_NTP_MAX || s[0] == '-' || s[0] == '.')
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '.')
            return false;
    return true;
}

bool esl_tz_ok(const char *s, size_t len)
{
    if (len < 1 || len > ESL_TZ_MAX)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr("+-,./:<>", s[i]))
            return false;
    /* the POSIX form, a name and its offset: "Europe/Oslo" would be taken as UTC */
    size_t i = 0;
    if (s[0] == '<') {
        while (++i < len && s[i] != '>')
            if (!isalnum((unsigned char)s[i]) && s[i] != '+' && s[i] != '-')
                return false;
        if (i < 2 || i >= len)
            return false;
        i++;
    } else {
        while (i < len && isalpha((unsigned char)s[i]))
            i++;
        if (i < 3)
            return false;
    }
    if (i < len && (s[i] == '+' || s[i] == '-'))
        i++;
    return i < len && isdigit((unsigned char)s[i]);
}

/* A string or null at key k (index i) into buf (cap bytes): its length, 0 for null, -1 when it
   is neither or does not fit, -2 when check (may be NULL) turns a non-empty one down. */
static int opt_str(const char *body, const esl_jtok_t *tok, int i, char *buf, size_t cap,
                   bool (*check)(const char *s, size_t len))
{
    buf[0] = '\0';
    if (tok[i].type == ESL_JSON_NULL)
        return 0;
    int n = tok[i].type == ESL_JSON_STRING ? esl_json_str_decode(body, &tok[i], buf, cap - 1) : -1;
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n && check && !check(buf, (size_t)n) ? -2 : n;
}

/* PUT/GET /api/config: AP-wide default interval, the webhook, the radio's transmit
 * power, the clock, night mode and the update window (#415, esl_store.h). poll_s 0 = tags keep their built-in interval, null = back to
 * the AP's built-in default (menuconfig). webhook null or "" = off. tx_power in dBm,
 * null = back to the built-in value. ntp null or "" = the server set in menuconfig;
 * tz null or "" = UTC. lang "en" or "nb" for the date fields, null = not chosen (English). Keys left out
 * stay as they are; one of them must be there. */
/* "channel_now" skips the wait for a move the tags were told of; a channel they have not heard
   of would leave them on the old one (docs/protocol.md, "Changing the channel"). The built-in
   channel is fine: every tag goes back to it when it loses the AP. */
static bool channel_now_ok(const esl_channel_state_t *cs, uint8_t channel)
{
    return channel == cs->builtin || channel == cs->channel || channel == cs->next;
}

/* A move away from the built-in channel needs tags and a radio that pass it on: 0 = fine, or
   the 409 with why ("force" moves past tags with old firmware, not past an old radio) */
static int channel_move_ok(const esl_channel_state_t *cs, uint8_t channel, bool force, esl_sb_t *out)
{
    if (channel != cs->channel && (cs->taken >> channel & 1))
        return api_error(out, 409, "another radio of this AP is on that channel: pick another one");
    if (channel == cs->builtin || channel == cs->channel)
        return 0;
    if (cs->radio_old)
        return api_error(out, 409, "the radio on WiFi runs firmware that cannot tell the tags of a new channel: "
                                   "update the radio first");
    if (cs->unheard && !force) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "%u tag%s %s not polled since the AP started, so the AP does not know if %s can follow a "
                 "channel change: wait until %s %s (at most %u min), or send \"force\": true",
                 cs->unheard, cs->unheard == 1 ? "" : "s", cs->unheard == 1 ? "has" : "have",
                 cs->unheard == 1 ? "it" : "they", cs->unheard == 1 ? "it" : "they",
                 cs->unheard == 1 ? "has" : "have", (unsigned)((cs->unheard_s + 59) / 60));
        return api_error(out, 409, msg);
    }
    if (cs->old_tags && !force) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "%u tag%s run%s firmware that cannot follow a channel change: update %s first, "
                 "or send \"force\": true to leave %s behind",
                 cs->old_tags, cs->old_tags == 1 ? "" : "s", cs->old_tags == 1 ? "s" : "",
                 cs->old_tags == 1 ? "it" : "them", cs->old_tags == 1 ? "it" : "them");
        return api_error(out, 409, msg);
    }
    return 0;
}

/* An AP id as 8 hex digits ("1A2B3C4D"); not 0 or FFFFFFFF */
static bool parse_ap_id(const char *body, const esl_jtok_t *t, uint32_t *id)
{
    if (t->type != ESL_JSON_STRING || t->end - t->start != 8)
        return false;
    uint32_t v = 0;
    for (int i = t->start; i < t->end; i++) {
        char ch = body[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 :
                ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        if (d < 0)
            return false;
        v = v << 4 | (uint32_t)d;
    }
    if (!v || v == 0xFFFFFFFFul)
        return false;
    *id = v;
    return true;
}

/* "HH:MM" (00:00 to 23:59) at key k of object obj to minutes after midnight; false when it
   is missing or not that */
static bool parse_hhmm(const json_t *j, int obj, const char *k, uint16_t *min)
{
    int v = esl_json_obj_get(j->js, j->tok, obj, k);
    if (v < 0 || j->tok[v].type != ESL_JSON_STRING || j->tok[v].end - j->tok[v].start != 5)
        return false;
    const char *s = j->js + j->tok[v].start;
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) || s[2] != ':' ||
        !isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4]))
        return false;
    unsigned h = (unsigned)(s[0] - '0') * 10 + (unsigned)(s[1] - '0'), m = (unsigned)(s[3] - '0') * 10 + (unsigned)(s[4] - '0');
    if (h > 23 || m > 59)
        return false;
    *min = (uint16_t)(h * 60 + m);
    return true;
}

/* "night": null or {"from":"22:00","to":"06:00","poll_s":1800} into h (night mode off for null) */
static bool parse_night(const json_t *j, int v, esl_hours_t *h)
{
    h->night_from = h->night_to = h->night_poll_s = 0;
    if (j->tok[v].type == ESL_JSON_NULL)
        return true;
    uint16_t from, to, poll_s;
    if (j->tok[v].type != ESL_JSON_OBJECT || !parse_hhmm(j, v, "from", &from) || !parse_hhmm(j, v, "to", &to) ||
        from == to || api_parse_uint(j, v, "poll_s", ESL_NIGHT_POLL_MIN_S, ESL_NIGHT_POLL_MAX_S, &poll_s) < 0 || !poll_s)
        return false;
    h->night_from = from;
    h->night_to = to;
    h->night_poll_s = poll_s;
    return true;
}

/* "update_window": null or {"from":"06:00","to":"08:00"} */
static bool parse_window(const json_t *j, int v, esl_hours_t *h)
{
    h->window = false;
    h->window_from = h->window_to = 0;
    if (j->tok[v].type == ESL_JSON_NULL)
        return true;
    h->window = j->tok[v].type == ESL_JSON_OBJECT && parse_hhmm(j, v, "from", &h->window_from) &&
                parse_hhmm(j, v, "to", &h->window_to) && h->window_from != h->window_to;
    return h->window;
}

/* {"from":"HH:MM","to":"HH:MM" (the object is left open) */
static void put_span(esl_sb_t *out, uint16_t from, uint16_t to)
{
    esl_sb_printf(out, "{\"from\":\"%02u:%02u\",\"to\":\"%02u:%02u\"", (unsigned)(from / 60), (unsigned)(from % 60),
                  (unsigned)(to / 60), (unsigned)(to % 60));
}

static void set_adopt(const esl_api_t *api, bool all, uint32_t id)
{
    esl_store_set_adopt(api->store, all, id);
    api->apid->set_adopt(api->apid->ctx, all, id);
}

static int ap_config(const esl_api_t *api, const char *method, const char *body, size_t len, uint64_t now_ms,
                     esl_sb_t *out)
{
    bool put = strcmp(method, "PUT") == 0;
    if (put) {
        esl_jtok_t *tok = NULL;
        int status = api_parse_json(body, len, &tok, out);
        if (status == 0) {
            json_t j = {body, tok};
            int v = esl_json_obj_get(body, tok, 0, "poll_s");
            int w = esl_json_obj_get(body, tok, 0, "webhook");
            int x = esl_json_obj_get(body, tok, 0, "tx_power");
            int n = esl_json_obj_get(body, tok, 0, "ntp");
            int z = esl_json_obj_get(body, tok, 0, "tz");
            int lg = esl_json_obj_get(body, tok, 0, "lang");
            int c = esl_json_obj_get(body, tok, 0, "channel");
            int cn = esl_json_obj_get(body, tok, 0, "channel_now");
            int cf = esl_json_obj_get(body, tok, 0, "force");
            int ai = esl_json_obj_get(body, tok, 0, "ap_id");
            int ad = esl_json_obj_get(body, tok, 0, "adopt");
            int ng = esl_json_obj_get(body, tok, 0, "night");
            int uw = esl_json_obj_get(body, tok, 0, "update_window");
            esl_hours_t hours;
            esl_store_get_hours(api->store, &hours);
            uint32_t new_id = 0, adopt_id = 0;
            bool adopt_all = ad >= 0 && tok[ad].type == ESL_JSON_STRING && tok[ad].end - tok[ad].start == 3 &&
                             !memcmp(body + tok[ad].start, "all", 3);
            esl_channel_state_t cs;
            esl_store_channel_state(api->store, now_ms, &cs);
            uint16_t poll_s, channel = 0;
            int8_t dbm = 0;
            char url[ESL_WEBHOOK_URL_MAX + 1], ntp[ESL_NTP_MAX + 1], tz[ESL_TZ_MAX + 1];
            int ul = 0, lang = -1;
            if (lg >= 0 && tok[lg].type == ESL_JSON_STRING)
                lang = esl_lang_parse(body + tok[lg].start, (size_t)(tok[lg].end - tok[lg].start));
            if (v < 0 && w < 0 && x < 0 && n < 0 && z < 0 && c < 0 && lg < 0 && ai < 0 && ad < 0 && ng < 0 && uw < 0)
                status = api_error(out, 422, "poll_s, webhook, tx_power, ntp, tz, lang, channel, ap_id, adopt, night or "
                                             "update_window is required (null resets it)");
            else if ((ng >= 0 || uw >= 0) && !api->store->ops.unix_now)
                status = api_error(out, 422, "this AP has no clock: no night mode or update window");
            else if (ng >= 0 && !parse_night(&j, ng, &hours))
                status = api_error(out, 422, "night must be null or {\"from\": \"HH:MM\", \"to\": \"HH:MM\", \"poll_s\": "
                                             "seconds from 60 to 3600}, from and to not the same");
            else if (uw >= 0 && !parse_window(&j, uw, &hours))
                status = api_error(out, 422, "update_window must be null or {\"from\": \"HH:MM\", \"to\": \"HH:MM\"}, "
                                             "from and to not the same");
            else if ((ai >= 0 || ad >= 0) && !api->apid)
                status = api_error(out, 422, "this AP has no AP id");
            else if (ai >= 0 && !parse_ap_id(body, &tok[ai], &new_id))
                status = api_error(out, 422, "ap_id must be 8 hex digits, not 00000000 or FFFFFFFF");
            else if (ad >= 0 && tok[ad].type != ESL_JSON_NULL && !adopt_all && !parse_ap_id(body, &tok[ad], &adopt_id))
                status = api_error(out, 422, "adopt must be null, \"all\" or an AP id of 8 hex digits");
            else if (adopt_id && adopt_id == (ai >= 0 ? new_id : api->store->ap_id))
                status = api_error(out, 422, "adopt must be the id of another AP, not this AP's own");
            else if (v >= 0 && tok[v].type != ESL_JSON_NULL &&
                     (api_parse_uint(&j, 0, "poll_s", 0, ESL_CFG_POLL_MAX_S, &poll_s) < 0 ||
                      (poll_s && poll_s < ESL_CFG_POLL_MIN_S)))
                status = api_error(out, 422, "poll_s must be null, 0 or a whole number of seconds from 10 to 3600");
            else if (w >= 0 && !api->notify)
                status = api_error(out, 422, "this AP has no webhook");
            else if (w >= 0 && tok[w].type != ESL_JSON_NULL &&
                     (tok[w].type != ESL_JSON_STRING ||
                      (ul = esl_json_str_decode(body, &tok[w], url, sizeof(url) - 1)) < 0 ||
                      (ul && !esl_webhook_url_ok(url, (size_t)ul))))
                status = api_error(out, 422, "webhook must be null or an http://host[:port][/path] URL of at most 127 characters");
            else if (x >= 0 && !api->tx_power)
                status = api_error(out, 422, "this AP cannot set its transmit power");
            else if (x >= 0 && tok[x].type != ESL_JSON_NULL && !parse_dbm(&j, x, &dbm))
                status = api_error(out, 422, "tx_power must be null or a whole number of dBm from -24 to 20");
            else if ((n >= 0 || z >= 0) && !api->clock)
                status = api_error(out, 422, "this AP has no clock settings");
            else if (n >= 0 && opt_str(body, tok, n, ntp, sizeof(ntp), esl_ntp_name_ok) < 0)
                status = api_error(out, 422, "ntp must be null or a server name of at most 63 letters, digits, '-' and '.'");
            else if (z >= 0 && opt_str(body, tok, z, tz, sizeof(tz), esl_tz_ok) < 0)
                status = api_error(out, 422, "tz must be null or a POSIX time zone of at most 63 characters, "
                                             "such as CET-1CEST,M3.5.0,M10.5.0/3");
            else if (lg >= 0 && (!api->clock || !api->clock->set_lang))
                status = api_error(out, 422, "this AP has no language setting");
            else if (lg >= 0 && tok[lg].type != ESL_JSON_NULL && (tok[lg].type != ESL_JSON_STRING || lang < 0))
                status = api_error(out, 422, "lang must be null, \"en\" or \"nb\"");
            else if (c >= 0 && !api->channel_settable)
                status = api_error(out, 422, "this AP cannot change its radio channel");
            else if (c >= 0 && tok[c].type != ESL_JSON_NULL &&
                     api_parse_uint(&j, 0, "channel", ESL_CHANNEL_MIN, ESL_CHANNEL_MAX, &channel) < 0)
                status = api_error(out, 422, "channel must be null or a whole number from 11 to 26");
            else if (cn >= 0 && (c < 0 || (tok[cn].type != ESL_JSON_TRUE && tok[cn].type != ESL_JSON_FALSE)))
                status = api_error(out, 422, "channel_now must be true or false, with channel");
            else if (cf >= 0 && (c < 0 || (tok[cf].type != ESL_JSON_TRUE && tok[cf].type != ESL_JSON_FALSE)))
                status = api_error(out, 422, "force must be true or false, with channel");
            else if (cn >= 0 && tok[cn].type == ESL_JSON_TRUE &&
                     !channel_now_ok(&cs, channel ? (uint8_t)channel : cs.builtin))
                status = api_error(out, 409, "channel_now moves only to the channel the tags were told of: "
                                             "set the channel first, then move once they have it");
            else if (c >= 0 && (status = channel_move_ok(&cs, channel ? (uint8_t)channel : cs.builtin,
                                                         cf >= 0 && tok[cf].type == ESL_JSON_TRUE, out)))
                ;
            else if (ai >= 0 && new_id != api->store->ap_id &&
                     !api->apid->set_id(api->apid->ctx, new_id, api->store->ap_id))
                status = api_error(out, 500, "the AP id could not be saved"); /* last: nothing else is set yet */
            else {
                if (ng >= 0 || uw >= 0)
                    esl_store_set_hours(api->store, &hours);
                if (v >= 0 && tok[v].type == ESL_JSON_NULL)
                    esl_store_reset_next_poll(api->store);
                else if (v >= 0)
                    esl_store_set_next_poll(api->store, poll_s);
                if (w >= 0) {
                    url[ul] = '\0';
                    strcpy(api->notify->url, url);
                    if (api->notify->persist)
                        api->notify->persist(api->notify->ctx, url);
                }
                if (x >= 0) {
                    bool reset = tok[x].type == ESL_JSON_NULL;
                    esl_tx_power_t *tp = api->tx_power;
                    tp->dbm = reset ? tp->builtin : dbm;
                    if (tp->set)
                        tp->set(tp->ctx, tp->dbm, reset);
                }
                if (n >= 0 || z >= 0)
                    api->clock->set(api->clock->ctx, n >= 0 ? ntp : NULL, z >= 0 ? tz : NULL);
                if (lg >= 0)
                    api->clock->set_lang(api->clock->ctx, lang < 0 ? "" : esl_lang_name((uint8_t)lang)); /* null: not chosen */
                if (ai >= 0 && new_id != api->store->ap_id) {
                    /* the tags of this AP carry the old id: still answered, and moved to the new one */
                    esl_store_set_prev_id(api->store, api->store->ap_id);
                    esl_store_set_ap_id(api->store, new_id);
                }
                if (ad >= 0)
                    set_adopt(api, adopt_all, adopt_id);
                if (c >= 0)
                    esl_store_move_channel(api->store, (uint8_t)channel, cn >= 0 && tok[cn].type == ESL_JSON_TRUE,
                                           cf >= 0 && tok[cf].type == ESL_JSON_TRUE, now_ms);
            }
        }
        free(tok);
        if (status)
            return status;
    } else if (strcmp(method, "GET") != 0) {
        return api_error(out, 405, "method not allowed");
    }
    esl_sb_printf(out, "{\"poll_s\":%u,\"builtin_poll_s\":%u,\"webhook\":", (unsigned)esl_store_get_next_poll(api->store),
                  (unsigned)api->store->next_poll_builtin);
    if (api->notify && api->notify->url[0])
        esl_sb_json_str(out, api->notify->url);
    else
        esl_sb_puts(out, "null");
    /* night mode and update window (#415), and whether they apply now (not while the time
       is unknown), on an AP with a clock */
    if (api->store->ops.unix_now) {
        esl_hours_t h;
        esl_store_get_hours(api->store, &h);
        uint32_t unix_s = api->store->ops.unix_now(api->store->ops.ctx);
        esl_sb_puts(out, ",\"night\":");
        if (h.night_poll_s) {
            put_span(out, h.night_from, h.night_to);
            esl_sb_printf(out, ",\"poll_s\":%u}", (unsigned)h.night_poll_s);
        } else {
            esl_sb_puts(out, "null");
        }
        esl_sb_puts(out, ",\"update_window\":");
        if (h.window) {
            put_span(out, h.window_from, h.window_to);
            esl_sb_putc(out, '}');
        } else {
            esl_sb_puts(out, "null");
        }
        esl_sb_printf(out, ",\"night_now\":%s,\"update_window_open\":%s", esl_hours_night_now(&h, unix_s) ? "true" : "false",
                      esl_hours_window_closed(&h, unix_s) ? "false" : "true");
    }
    if (api->tx_power)
        esl_sb_printf(out, ",\"tx_power\":%d,\"builtin_tx_power\":%d", api->tx_power->dbm, api->tx_power->builtin);
    /* the channel, and a move the tags are being told about: how many of the tags heard
       lately still have to confirm it, and the most it waits for them */
    if (api->channel_settable) {
        esl_channel_state_t ch;
        esl_store_channel_state(api->store, now_ms, &ch);
        esl_sb_printf(out, ",\"channel\":%u,\"builtin_channel\":%u", (unsigned)ch.channel, (unsigned)ch.builtin);
        if (ch.next)
            esl_sb_printf(out, ",\"channel_next\":%u,\"channel_waiting\":%u,\"channel_due_s\":%" PRIu32,
                          (unsigned)ch.next, ch.waiting, ch.due_s);
        else
            esl_sb_puts(out, ",\"channel_next\":null");
        /* what stops a move: tags heard lately with firmware from before 0.9.0, an old radio on WiFi */
        esl_sb_printf(out, ",\"channel_old_tags\":%u,\"channel_unheard_tags\":%u,\"channel_radio_old\":%s"
                      ",\"channel_stopped\":%u", ch.old_tags, ch.unheard, ch.radio_old ? "true" : "false", ch.stopped);
    }
    if (api->apid && api->store->ap_id) {
        esl_sb_printf(out, ",\"ap_id\":\"%08" PRIX32 "\"", api->store->ap_id);
        if (api->store->prev_id)
            esl_sb_printf(out, ",\"prev_ap_id\":\"%08" PRIX32 "\"", api->store->prev_id);
        else
            esl_sb_puts(out, ",\"prev_ap_id\":null");
        if (api->store->adopt_all)
            esl_sb_puts(out, ",\"adopt\":\"all\"");
        else if (api->store->adopt_id)
            esl_sb_printf(out, ",\"adopt\":\"%08" PRIX32 "\"", api->store->adopt_id);
        else
            esl_sb_puts(out, ",\"adopt\":null");
        uint32_t until = (api->store->adopt_all || api->store->adopt_id) ? api->apid->adopt_until : 0;
        if (until > 1)
            esl_sb_printf(out, ",\"adopt_until\":%" PRIu32, until);
        else
            esl_sb_puts(out, ",\"adopt_until\":null");
    } else if (api->apid) {
        esl_sb_puts(out, ",\"ap_id\":null,\"prev_ap_id\":null,\"adopt\":null,\"adopt_until\":null");
    }
    if (api->clock) {
        const esl_clock_cfg_t *c = api->clock;
        const char *keys[] = {"ntp", "builtin_ntp", "tz"};
        const char *vals[] = {c->ntp, c->builtin_ntp, c->tz};
        for (unsigned i = 0; i < 3; i++) {
            esl_sb_printf(out, ",\"%s\":", keys[i]);
            if (vals[i] && vals[i][0])
                esl_sb_json_str(out, vals[i]);
            else
                esl_sb_puts(out, "null");
        }
        if (c->set_lang) /* null until chosen: the AP writes English */
            esl_sb_printf(out, c->lang[0] ? ",\"lang\":\"%s\"" : ",\"lang\":null", c->lang);
    }
    esl_sb_putc(out, '}');
    return 200;
}

int api_route_config(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return ap_config(api, rq->method, rq->body, rq->body_len, rq->now_ms, out);
}
