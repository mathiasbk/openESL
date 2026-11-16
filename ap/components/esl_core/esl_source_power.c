#include "esl_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data source type "power": today's electricity spot price in a Norwegian price area, from
 * hvakosterstrommen.no (free, no key; credit "hvakosterstrommen.no" where it is shown).
 * The answer for a day is a list of intervals (an hour, or 15 minutes) with their price in
 * NOK per kWh without VAT. The URL holds Norway's date, so the AP asks for today's file.
 * The prices are Norwegian, so the dates and times shown are Norway's (CET/CEST), whatever
 * time zone the AP has. A day's file does not change, so datasrc.c fetches it once
 * (".dated") and makes the values ("now", "next", ...) again from it at the start of each
 * quarter (".align"), when "now" turns over. */

static const char *const power_keys[] = {"area", "now",   "nowore", "next",  "min",   "minat", "max",
                                         "maxat", "avg",  "level",  "date",  NULL};
/* The default templates: four lines for the built-in text layout, without words of one
   language ({level} follows "lang") */
static const char *const power_fields[] = {"{area} {date}", "{now} kr/kWh", "{level}", "{min} - {max} kr", NULL};
static const char *const power_cfg_keys[] = {"area", "vat", NULL};

/* ---- the configuration ---- */

static bool power_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err,
                        size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "area");
    size_t n = t >= 0 ? (size_t)(tok[t].end - tok[t].start) : 0;
    if (t < 0 || tok[t].type != ESL_JSON_STRING || n != 3 || memcmp(js + tok[t].start, "NO", 2) != 0 ||
        js[tok[t].start + 2] < '1' || js[tok[t].start + 2] > '5') {
        snprintf(err, errlen, "area: the price area, \"NO1\" to \"NO5\"");
        return false;
    }
    c->power.area = (uint8_t)(js[tok[t].start + 2] - '0');
    /* NO4 (Northern Norway) has no VAT on electricity */
    c->power.vat = c->power.area != 4;
    t = esl_json_obj_get(js, tok, obj, "vat");
    if (t >= 0) {
        if (tok[t].type != ESL_JSON_TRUE && tok[t].type != ESL_JSON_FALSE) {
            snprintf(err, errlen, "vat: true or false");
            return false;
        }
        c->power.vat = tok[t].type == ESL_JSON_TRUE;
    }
    return true;
}

static void power_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_printf(out, ",\"area\":\"NO%u\",\"vat\":%s", (unsigned)c->power.area, c->power.vat ? "true" : "false");
}

#define BASE "https://www.hvakosterstrommen.no/api/v1/prices/"

/* What the source fetches, without the date (it names the source's data) */
static size_t power_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, BASE "NO%u", (unsigned)c->power.area);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* Year, month and day of a count of days since 1970-01-01 */
static void civil(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

/* Days since 1970-01-01 of the 31st of a month from March to December */
static int32_t day31(int y, unsigned m)
{
    int32_t era = y / 400; /* y > 0 */
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m - 3) + 2) / 5 + 30;
    return era * 146097 + (int32_t)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

/* The last Sunday of March or October (1970-01-01 was a Thursday: (day + 4) % 7 is 0 on a Sunday) */
static int32_t last_sunday(int y, unsigned m)
{
    int32_t day = day31(y, m);
    return day - (day + 4) % 7;
}

/* Norway's offset from UTC at t: +2 h from 01:00 UTC on the last Sunday of March to 01:00 UTC
   on the last Sunday of October, else +1 h (the EU rule) */
static int32_t norway(uint32_t t)
{
    int y;
    unsigned m, d;
    civil((int32_t)(t / 86400), &y, &m, &d);
    uint32_t a = (uint32_t)last_sunday(y, 3) * 86400 + 3600, b = (uint32_t)last_sunday(y, 10) * 86400 + 3600;
    return t >= a && t < b ? 7200 : 3600;
}

/* Today's file (Norway's date) */
static size_t power_url_at(const esl_source_cfg_t *c, uint32_t now, esl_utc_offset_fn off, char *buf, size_t cap)
{
    int y;
    unsigned m, d;
    (void)off;
    civil((int32_t)((now + (uint32_t)norway(now)) / 86400), &y, &m, &d);
    int n = snprintf(buf, cap, BASE "%d/%02u-%02u_NO%u.json", y, m, d, (unsigned)c->power.area);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ---- the answer ---- */

/* "2026-11-08T13:15:00+01:00" (an offset, or Z) as Unix s; 0 when it is not one */
static uint32_t time_off(const char *s, size_t n)
{
    if (n == 20)
        return esl_iso_time(s, n);
    if (n != 25 || (s[19] != '+' && s[19] != '-') || s[22] != ':')
        return 0;
    for (int i = 20; i < 25; i++)
        if (i != 22 && (s[i] < '0' || s[i] > '9'))
            return 0;
    char z[21];
    memcpy(z, s, 19);
    z[19] = 'Z';
    z[20] = '\0';
    uint32_t t = esl_iso_time(z, 20);
    int oh = (s[20] - '0') * 10 + (s[21] - '0'), om = (s[23] - '0') * 10 + (s[24] - '0');
    if (!t || oh < 0 || oh > 14 || om < 0 || om > 59)
        return 0;
    int32_t o = (oh * 60 + om) * 60;
    return s[19] == '+' ? t - (uint32_t)o : t + (uint32_t)o;
}

typedef struct {
    uint32_t start, end;
    double nok; /* per kWh, with VAT when the source has it */
} span_t;

/* Kroner with two decimals, with a decimal comma in Norwegian */
static void fmt_kr(char *out, size_t cap, double kr, bool en)
{
    long ore = esl_source_round(kr * 100);
    unsigned long a = (unsigned long)(ore < 0 ? -ore : ore);
    snprintf(out, cap, "%s%lu%c%02lu", ore < 0 ? "-" : "", a / 100, en ? '.' : ',', a % 100);
}

static void fmt_hm(char *out, size_t cap, uint32_t t)
{
    uint32_t l = t + (uint32_t)norway(t);
    snprintf(out, cap, "%02u:%02u", (unsigned)(l % 86400 / 3600), (unsigned)(l % 3600 / 60));
}

#define SPANS_MAX 100 /* 96 quarters, and 100 on the day summer time ends */

static bool power_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now,
                         esl_utc_offset_fn off, esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    (void)data_at, (void)off;
    unsigned max_tok = 2;
    for (size_t i = 0; i < len; i++)
        max_tok += body[i] == '{' || body[i] == '[' || body[i] == ',' || body[i] == ':';
    esl_jtok_t *tok = len <= ESL_JSON_MAX_LEN ? malloc(sizeof(*tok) * max_tok) : NULL;
    span_t *sp = malloc(sizeof(*sp) * SPANS_MAX);
    bool ok = false;
    unsigned n = 0;
    if (!tok || !sp) {
        snprintf(err, errlen, tok || len > ESL_JSON_MAX_LEN ? "out of memory" : "the answer is too big");
        goto done;
    }
    if (esl_json_parse(body, len, tok, max_tok) < 0 || tok[0].type != ESL_JSON_ARRAY) {
        snprintf(err, errlen, "the answer is not a JSON list of prices");
        goto done;
    }
    for (int k = 1, i = 0; i < tok[0].size && n < SPANS_MAX; i++, k = tok[k].next) {
        if (tok[k].type != ESL_JSON_OBJECT)
            continue;
        int p = esl_json_obj_get(body, tok, k, "NOK_per_kWh");
        int s = esl_json_obj_get(body, tok, k, "time_start");
        int e = esl_json_obj_get(body, tok, k, "time_end");
        double nok;
        if (p < 0 || s < 0 || e < 0 || tok[s].type != ESL_JSON_STRING || tok[e].type != ESL_JSON_STRING ||
            !esl_source_num(body, &tok[p], &nok))
            continue;
        span_t *x = &sp[n];
        x->start = time_off(body + tok[s].start, (size_t)(tok[s].end - tok[s].start));
        x->end = time_off(body + tok[e].start, (size_t)(tok[e].end - tok[e].start));
        if (!x->start || x->end <= x->start)
            continue;
        x->nok = c->power.vat ? nok * 1.25 : nok;
        n++;
    }
    if (!n) {
        snprintf(err, errlen, "the answer has no prices");
        goto done;
    }
    int cur = -1, nxt = -1;
    unsigned lo = 0, hi = 0;
    double sum = 0;
    for (unsigned i = 0; i < n; i++) {
        if (sp[i].start <= now && now < sp[i].end)
            cur = (int)i;
        if (sp[i].nok < sp[lo].nok)
            lo = i;
        if (sp[i].nok > sp[hi].nok)
            hi = i;
        sum += sp[i].nok;
    }
    if (cur < 0) {
        snprintf(err, errlen, "the answer has no price for now (it is for another day)");
        goto done;
    }
    for (unsigned i = 0; i < n; i++)
        if (sp[i].start == sp[cur].end)
            nxt = (int)i;
    bool en = !strcmp(c->lang, "en");
    double avg = sum / n;
    char buf[ESL_VALUE_MAX + 1];
    v->n = 0;
    snprintf(buf, sizeof(buf), "NO%u", (unsigned)c->power.area);
    esl_values_set(v, "area", buf);
    fmt_kr(buf, sizeof(buf), sp[cur].nok, en);
    esl_values_set(v, "now", buf);
    snprintf(buf, sizeof(buf), "%ld", esl_source_round(sp[cur].nok * 100));
    esl_values_set(v, "nowore", buf);
    if (nxt >= 0) {
        fmt_kr(buf, sizeof(buf), sp[nxt].nok, en);
        esl_values_set(v, "next", buf);
    }
    fmt_kr(buf, sizeof(buf), sp[lo].nok, en);
    esl_values_set(v, "min", buf);
    fmt_hm(buf, sizeof(buf), sp[lo].start);
    esl_values_set(v, "minat", buf);
    fmt_kr(buf, sizeof(buf), sp[hi].nok, en);
    esl_values_set(v, "max", buf);
    fmt_hm(buf, sizeof(buf), sp[hi].start);
    esl_values_set(v, "maxat", buf);
    fmt_kr(buf, sizeof(buf), avg, en);
    esl_values_set(v, "avg", buf);
    /* against the day's average: a tenth below is low, a tenth above is high */
    double r = avg > 0 ? sp[cur].nok / avg : 1;
    esl_values_set(v, "level", r < 0.9 ? (en ? "low" : "lav") : r > 1.1 ? (en ? "high" : "høy") : "normal");
    uint32_t l = sp[cur].start + (uint32_t)norway(sp[cur].start);
    int y;
    unsigned m, d;
    civil((int32_t)(l / 86400), &y, &m, &d);
    snprintf(buf, sizeof(buf), "%02u.%02u.", d, m);
    esl_values_set(v, "date", buf);
    ok = true;
done:
    free(sp);
    free(tok);
    return ok;
}

const esl_source_def_t esl_source_power = {
    .name = "power",
    .type = ESL_SOURCE_POWER,
    .about = "today's electricity price in Norway from hvakosterstrommen.no",
    .keys = power_keys,
    .fields = power_fields,
    .cfg_keys = power_cfg_keys,
    .every_min = 15, /* runs at :00, :15, :30, :45 (UTC, so Norway's too) */
    .own_pace = true, /* "now" follows the clock, not the server's Expires */
    .dated = true,    /* a day's file is fetched once, then the values are made from it */
    .align = true,    /* runs at the start of each quarter, when "now" turns over */
    .parse = power_parse,
    .write = power_write,
    .url = power_url,
    .url_at = power_url_at,
    .values = power_values,
};
