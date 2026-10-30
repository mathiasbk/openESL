#include "esl_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data source type "met": the forecast from MET Norway (api.met.no locationforecast 2.0,
 * compact) for a latitude and longitude. Its data is CC BY 4.0: credit "MET Norway" where
 * the forecast is shown. MET's terms (at least 30 minutes between fetches, never before
 * Expires, 4 decimals) are kept by every_min below, esl_source.c and main/datasrc.c. */

/* The values it gives, in this order (docs/data-sources.md) */
static const char *const met_keys[] = {"place", "temp",  "symbol", "desc", "min", "max", "wind", "dir",
                                       "rain",  "rain1h", "humidity", "h3", "t3", "s3", "h6", "t6",
                                       "s6",    "tmin", "tmax", "tsym", "updated", NULL};

/* The default templates: those of the "weather" template (templates/home) */
static const char *const met_fields[] = {"{place}", "{temp}", "{symbol}", "{desc}", "{min}° / {max}°",
                                         "{wind} m/s  {rain} mm", NULL};

/* Its own keys in a source's configuration */
static const char *const met_cfg_keys[] = {"lat", "lon", NULL};

/* ---- the configuration ---- */

static bool met_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    static const char *const ll[] = {"lat", "lon"};
    static const double lim[] = {90, 180};
    int32_t *dst[] = {&c->met.lat, &c->met.lon};
    for (int i = 0; i < 2; i++) {
        double d;
        int t = esl_json_obj_get(js, tok, obj, ll[i]);
        if (t < 0 || !esl_source_num(js, &tok[t], &d) || d < -lim[i] || d > lim[i]) {
            snprintf(err, errlen, "%s: a number from -%d to %d (degrees)", ll[i], (int)lim[i], (int)lim[i]);
            return false;
        }
        *dst[i] = (int32_t)esl_source_round(d * 10000); /* MET takes 4 decimals */
    }
    return true;
}

/* Degrees * 10000 as a number with 4 decimals, after prefix */
static int deg(char *buf, size_t cap, const char *prefix, int32_t v)
{
    uint32_t a = (uint32_t)(v < 0 ? -(int64_t)v : v);
    return snprintf(buf, cap, "%s%s%lu.%04lu", prefix, v < 0 ? "-" : "", (unsigned long)(a / 10000),
                    (unsigned long)(a % 10000));
}

static void met_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    char b[32];
    deg(b, sizeof(b), ",\"lat\":", c->met.lat);
    esl_sb_puts(out, b);
    deg(b, sizeof(b), ",\"lon\":", c->met.lon);
    esl_sb_puts(out, b);
}

static size_t met_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    char la[24], lo[24];
    deg(la, sizeof(la), "", c->met.lat);
    deg(lo, sizeof(lo), "", c->met.lon);
    int n = snprintf(buf, cap, "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%s&lon=%s", la, lo);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ---- the answer: locationforecast 2.0 compact ---- */

typedef struct {
    uint32_t t;
    double temp, wind, dir, hum, rain1, rain6;
    bool has_temp, has_wind, has_dir, has_hum, has_rain1, has_rain6;
    char sym1[32], sym6[32], sym12[32];
} met_entry_t;

/* The value at obj.k1.k2... (NULL ends the keys), or -1 */
static int path(const char *js, const esl_jtok_t *tok, int obj, const char *k1, const char *k2, const char *k3)
{
    const char *keys[] = {k1, k2, k3};
    for (int i = 0; i < 3 && keys[i] && obj >= 0; i++)
        obj = tok[obj].type == ESL_JSON_OBJECT ? esl_json_obj_get(js, tok, obj, keys[i]) : -1;
    return obj;
}

static bool num_at(const char *js, const esl_jtok_t *tok, int obj, const char *key, double *out)
{
    int t = obj >= 0 && tok[obj].type == ESL_JSON_OBJECT ? esl_json_obj_get(js, tok, obj, key) : -1;
    return t >= 0 && esl_source_num(js, &tok[t], out);
}

static void sym_at(const char *js, const esl_jtok_t *tok, int data, const char *period, char *out, size_t cap)
{
    int t = path(js, tok, data, period, "summary", "symbol_code");
    out[0] = '\0';
    if (t >= 0 && tok[t].type == ESL_JSON_STRING) {
        int n = esl_json_str_decode(js, &tok[t], out, cap - 1);
        out[n > 0 ? n : 0] = '\0';
    }
}

static bool met_entry(const char *js, const esl_jtok_t *tok, int e, met_entry_t *m)
{
    memset(m, 0, sizeof(*m));
    int t = tok[e].type == ESL_JSON_OBJECT ? esl_json_obj_get(js, tok, e, "time") : -1;
    if (t < 0 || tok[t].type != ESL_JSON_STRING)
        return false;
    m->t = esl_iso_time(js + tok[t].start, (size_t)(tok[t].end - tok[t].start));
    int data = esl_json_obj_get(js, tok, e, "data");
    if (!m->t || data < 0)
        return false;
    int det = path(js, tok, data, "instant", "details", NULL);
    m->has_temp = num_at(js, tok, det, "air_temperature", &m->temp);
    m->has_wind = num_at(js, tok, det, "wind_speed", &m->wind);
    m->has_dir = num_at(js, tok, det, "wind_from_direction", &m->dir);
    m->has_hum = num_at(js, tok, det, "relative_humidity", &m->hum);
    m->has_rain1 = num_at(js, tok, path(js, tok, data, "next_1_hours", "details", NULL), "precipitation_amount", &m->rain1);
    m->has_rain6 = num_at(js, tok, path(js, tok, data, "next_6_hours", "details", NULL), "precipitation_amount", &m->rain6);
    sym_at(js, tok, data, "next_1_hours", m->sym1, sizeof(m->sym1));
    sym_at(js, tok, data, "next_6_hours", m->sym6, sizeof(m->sym6));
    sym_at(js, tok, data, "next_12_hours", m->sym12, sizeof(m->sym12));
    return true;
}

/* The weather for the coming hours: MET's symbol code for the shortest period it has */
static const char *met_sym(const met_entry_t *m)
{
    return m->sym1[0] ? m->sym1 : m->sym6[0] ? m->sym6 : m->sym12;
}

/* MET's symbol code ("lightrainshowers_day") as one of the icon names of the weather
 * template: sun, moon, partsun, partmoon, cloud, fog, rain, sleet, snow, thunder */
static const char *met_icon(const char *code)
{
    bool night = strstr(code, "_night") != NULL;
    if (strstr(code, "thunder"))
        return "thunder";
    if (strstr(code, "sleet"))
        return "sleet";
    if (strstr(code, "snow"))
        return "snow";
    if (strstr(code, "rain"))
        return "rain";
    if (!strncmp(code, "fog", 3))
        return "fog";
    if (!strncmp(code, "cloudy", 6))
        return "cloud";
    if (!strncmp(code, "partlycloudy", 12))
        return night ? "partmoon" : "partsun";
    if (!strncmp(code, "clearsky", 8) || !strncmp(code, "fair", 4))
        return night ? "moon" : "sun";
    return "";
}

static const struct {
    const char *code, *nb, *en;
} met_words[] = {
    {"clearsky", "Klarvær", "Clear sky"},
    {"fair", "Lettskyet", "Fair"},
    {"partlycloudy", "Delvis skyet", "Partly cloudy"},
    {"cloudy", "Skyet", "Cloudy"},
    {"fog", "Tåke", "Fog"},
    {"lightrainshowers", "Lette regnbyger", "Light rain showers"},
    {"heavyrainshowers", "Kraftige regnbyger", "Heavy rain showers"},
    {"rainshowers", "Regnbyger", "Rain showers"},
    {"lightrain", "Lett regn", "Light rain"},
    {"heavyrain", "Kraftig regn", "Heavy rain"},
    {"rain", "Regn", "Rain"},
    {"lightsleetshowers", "Lette sluddbyger", "Light sleet showers"},
    {"heavysleetshowers", "Kraftige sluddbyger", "Heavy sleet showers"},
    {"sleetshowers", "Sluddbyger", "Sleet showers"},
    {"lightsleet", "Lett sludd", "Light sleet"},
    {"heavysleet", "Kraftig sludd", "Heavy sleet"},
    {"sleet", "Sludd", "Sleet"},
    {"lightsnowshowers", "Lette snøbyger", "Light snow showers"},
    {"heavysnowshowers", "Kraftige snøbyger", "Heavy snow showers"},
    {"snowshowers", "Snøbyger", "Snow showers"},
    {"lightsnow", "Lett snø", "Light snow"},
    {"heavysnow", "Kraftig snø", "Heavy snow"},
    {"snow", "Snø", "Snow"},
};

/* The weather in words: "Lette regnbyger og torden" */
static void met_desc(const char *code, bool en, char *out, size_t cap)
{
    size_t n = strcspn(code, "_");
    bool thunder = n > 10 && !memcmp(code + n - 10, "andthunder", 10);
    if (thunder)
        n -= 10;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(met_words) / sizeof(met_words[0]); i++)
        if (strlen(met_words[i].code) == n && !memcmp(met_words[i].code, code, n)) {
            snprintf(out, cap, "%s%s", en ? met_words[i].en : met_words[i].nb,
                     thunder ? (en ? " and thunder" : " og torden") : "");
            return;
        }
}

/* A whole number, never "-0" */
static void fmt_int(char *out, size_t cap, double d)
{
    long v = esl_source_round(d);
    snprintf(out, cap, "%ld", v);
}

/* mm of rain: one decimal under 10 with a decimal comma ("0,4"), whole above, "0" for none */
static void fmt_mm(char *out, size_t cap, double d)
{
    long t = esl_source_round(d * 10);
    if (t <= 0)
        snprintf(out, cap, "0");
    else if (t < 100)
        snprintf(out, cap, "%ld,%ld", t / 10, t % 10);
    else
        snprintf(out, cap, "%ld", (t + 5) / 10);
}

static const char *compass(double deg, bool en)
{
    static const char *const nb[] = {"N", "NØ", "Ø", "SØ", "S", "SV", "V", "NV"};
    static const char *const enw[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    long d = esl_source_round(deg) % 360;
    int i = (int)(((d < 0 ? d + 360 : d) * 2 + 45) / 90) % 8;
    return en ? enw[i] : nb[i];
}

/* MET's answer is 30-40 KB; more than the tokenizer takes (64 KB) is cut after the last
 * whole entry of the time series that fits, which still leaves days of forecast */
static size_t met_cut(const char *body, size_t len)
{
    static const char sep[] = "},{\"time\"";
    size_t max = ESL_JSON_MAX_LEN - 8, cut = 0;
    for (size_t i = 0; i + sizeof(sep) - 1 <= len && i < max; i++)
        if (body[i] == '}' && !memcmp(body + i, sep, sizeof(sep) - 1))
            cut = i + 1;
    return cut;
}

static bool met_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                       esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    char *copy = NULL;
    if (len > ESL_JSON_MAX_LEN) {
        size_t cut = met_cut(body, len);
        copy = cut ? malloc(cut + 4) : NULL;
        if (!copy) {
            snprintf(err, errlen, cut ? "out of memory" : "the answer is too big");
            return false;
        }
        memcpy(copy, body, cut);
        memcpy(copy + cut, "]}}", 3);
        body = copy;
        len = cut + 3;
    }
    /* every token but the last follows one of { [ , : */
    unsigned max_tok = 2;
    for (size_t i = 0; i < len; i++)
        max_tok += body[i] == '{' || body[i] == '[' || body[i] == ',' || body[i] == ':';
    esl_jtok_t *tok = malloc(sizeof(*tok) * max_tok);
    bool ok = false;
    met_entry_t *ent = NULL;
    if (!tok) {
        snprintf(err, errlen, "out of memory");
        goto done;
    }
    if (esl_json_parse(body, len, tok, max_tok) < 0 || tok[0].type != ESL_JSON_OBJECT) {
        snprintf(err, errlen, "the answer is not JSON");
        goto done;
    }
    int props = esl_json_obj_get(body, tok, 0, "properties");
    int ts = path(body, tok, props, "timeseries", NULL, NULL);
    if (ts < 0 || tok[ts].type != ESL_JSON_ARRAY || tok[ts].size == 0) {
        snprintf(err, errlen, "the answer has no properties.timeseries");
        goto done;
    }
    int up = path(body, tok, props, "meta", "updated_at", NULL);
    *data_at = up >= 0 && tok[up].type == ESL_JSON_STRING
                   ? esl_iso_time(body + tok[up].start, (size_t)(tok[up].end - tok[up].start))
                   : 0;

    /* the entries of the next two days or so: hourly first, then every 6 hours */
    unsigned n = 0, cap = tok[ts].size < 80 ? tok[ts].size : 80;
    ent = malloc(sizeof(*ent) * cap);
    if (!ent) {
        snprintf(err, errlen, "out of memory");
        goto done;
    }
    int cur = -1;
    for (int k = ts + 1, i = 0; i < tok[ts].size && n < cap; i++, k = tok[k].next) {
        if (!met_entry(body, tok, k, &ent[n]))
            continue;
        if (ent[n].t <= now || cur < 0)
            cur = (int)n;
        n++;
    }
    if (cur < 0) {
        snprintf(err, errlen, "the answer's time series has no entries");
        goto done;
    }
    const met_entry_t *m = &ent[cur];
    if (now > m->t + 3 * 3600) {
        snprintf(err, errlen, "the forecast is old (it ends before now)");
        goto done;
    }
    bool en = !strcmp(c->lang, "en");
    char buf[ESL_VALUE_MAX + 1];
    v->n = 0;
    esl_values_set(v, "place", c->place);
    if (m->has_temp) {
        fmt_int(buf, sizeof(buf), m->temp);
        esl_values_set(v, "temp", buf);
    }
    esl_values_set(v, "symbol", met_icon(met_sym(m)));
    met_desc(met_sym(m), en, buf, sizeof(buf));
    esl_values_set(v, "desc", buf);

    /* min and max over the next 24 hours */
    bool any = false;
    double lo = 0, hi = 0;
    for (unsigned i = (unsigned)cur; i < n && ent[i].t < m->t + 24 * 3600; i++)
        if (ent[i].has_temp) {
            if (!any || ent[i].temp < lo)
                lo = ent[i].temp;
            if (!any || ent[i].temp > hi)
                hi = ent[i].temp;
            any = true;
        }
    if (any) {
        fmt_int(buf, sizeof(buf), lo);
        esl_values_set(v, "min", buf);
        fmt_int(buf, sizeof(buf), hi);
        esl_values_set(v, "max", buf);
    }
    if (m->has_wind) {
        fmt_int(buf, sizeof(buf), m->wind);
        esl_values_set(v, "wind", buf);
    }
    if (m->has_dir)
        esl_values_set(v, "dir", compass(m->dir, en));
    fmt_mm(buf, sizeof(buf), m->has_rain6 ? m->rain6 : 0);
    esl_values_set(v, "rain", buf);
    fmt_mm(buf, sizeof(buf), m->has_rain1 ? m->rain1 : 0);
    esl_values_set(v, "rain1h", buf);
    if (m->has_hum) {
        fmt_int(buf, sizeof(buf), m->hum);
        esl_values_set(v, "humidity", buf);
    }

    /* in 3 and 6 hours: the hour (local time), the temperature and the icon */
    static const char *const later[2][3] = {{"h3", "t3", "s3"}, {"h6", "t6", "s6"}};
    for (int k = 0; k < 2; k++) {
        uint32_t want = m->t + (uint32_t)(k + 1) * 3 * 3600;
        unsigned i = (unsigned)cur;
        while (i < n && ent[i].t < want)
            i++;
        if (i == n || !ent[i].has_temp)
            continue;
        uint32_t local = ent[i].t + (uint32_t)(off ? off(ent[i].t) : 0);
        snprintf(buf, sizeof(buf), "%02u", (unsigned)(local % 86400 / 3600));
        esl_values_set(v, later[k][0], buf);
        fmt_int(buf, sizeof(buf), ent[i].temp);
        esl_values_set(v, later[k][1], buf);
        esl_values_set(v, later[k][2], met_icon(met_sym(&ent[i])));
    }

    /* tomorrow (local time): its lowest and highest, and the weather around noon */
    uint32_t tomorrow = (now + (uint32_t)(off ? off(now) : 0)) / 86400 + 1;
    int noon = -1;
    unsigned best = 99;
    any = false;
    for (unsigned i = 0; i < n; i++) {
        uint32_t local = ent[i].t + (uint32_t)(off ? off(ent[i].t) : 0);
        if (local / 86400 != tomorrow)
            continue;
        unsigned h = local % 86400 / 3600, d = h > 12 ? h - 12 : 12 - h;
        if (ent[i].has_temp) {
            if (!any || ent[i].temp < lo)
                lo = ent[i].temp;
            if (!any || ent[i].temp > hi)
                hi = ent[i].temp;
            any = true;
        }
        if (d < best && met_sym(&ent[i])[0]) {
            best = d;
            noon = (int)i;
        }
    }
    if (any) {
        fmt_int(buf, sizeof(buf), lo);
        esl_values_set(v, "tmin", buf);
        fmt_int(buf, sizeof(buf), hi);
        esl_values_set(v, "tmax", buf);
    }
    if (noon >= 0)
        esl_values_set(v, "tsym", met_icon(ent[noon].sym6[0] ? ent[noon].sym6 : met_sym(&ent[noon])));
    if (*data_at) {
        uint32_t local = *data_at + (uint32_t)(off ? off(*data_at) : 0);
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(local % 86400 / 3600), (unsigned)(local % 3600 / 60));
        esl_values_set(v, "updated", buf);
    }
    ok = true;
done:
    free(ent);
    free(tok);
    free(copy);
    return ok;
}

const esl_source_def_t esl_source_met = {
    .name = "met",
    .type = ESL_SOURCE_MET,
    .about = "the forecast from MET Norway",
    .keys = met_keys,
    .fields = met_fields,
    .cfg_keys = met_cfg_keys,
    .every_min = 30, /* MET's terms */
    .parse = met_parse,
    .write = met_write,
    .url = met_url,
    .values = met_values,
};
