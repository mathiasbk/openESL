#include "esl_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data source type "fx": the exchange rate of a currency, from Norges Bank's open data
 * (data.norges-bank.no, free, no key; credit "Norges Bank" where it is shown). Norges Bank
 * gives each currency's price in NOK once each business day, at about 16:00 Norway's time
 * (the ECB's rates of 14:15). Priced in another currency than NOK ("in"), the AP asks for
 * both currencies and divides, day by day: USD in EUR is USD's NOK price over EUR's.
 *
 * The answer is CSV, one line per day and currency, oldest first:
 *   FREQ;Frequency;BASE_CUR;...;UNIT_MULT;Unit Multiplier;...;TIME_PERIOD;OBS_VALUE
 *   B;Business;JPY;Japanese yen;NOK;...;2;Hundreds;...;2026-11-12;5.9691
 * UNIT_MULT 2 means the price is for 100 of the currency. The columns are found by name. */

static const char *const fx_keys[] = {"cur",   "in",   "pair", "unit", "rate", "rate2", "rate4", "chg",
                                      "pct",   "pctp", "hi",   "lo",   "date", "day",   "days",  "period",
                                      "chart", NULL};
/* The default templates, for the built-in text layout: without words of one language */
static const char *const fx_fields[] = {"{pair} {date}", "{rate}", "{chg} ({pct} %)", "{lo} - {hi}", NULL};
static const char *const fx_cfg_keys[] = {"currency", "in", "days", "unit", NULL};

#define FX_DAYS_MIN 5
#define FX_DAYS_MAX 30 /* 30 points of a chart fit in ESL_VALUE_LONG */
#define FX_DAYS_DEF 30
#define FX_ROWS     (FX_DAYS_MAX + 2)

/* Three capital letters, into out (4 bytes) */
static bool code(const char *js, const esl_jtok_t *t, char *out)
{
    if (t->type != ESL_JSON_STRING || t->end - t->start != 3)
        return false;
    for (int i = 0; i < 3; i++) {
        char ch = js[t->start + i];
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        if (ch < 'A' || ch > 'Z')
            return false;
        out[i] = ch;
    }
    out[3] = '\0';
    return true;
}

static bool fx_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "currency");
    if (t < 0 || !code(js, &tok[t], c->fx.cur)) {
        snprintf(err, errlen, "currency: a currency code of 3 letters, e.g. \"USD\"");
        return false;
    }
    memcpy(c->fx.in, "NOK", 4);
    t = esl_json_obj_get(js, tok, obj, "in");
    if (t >= 0 && !code(js, &tok[t], c->fx.in)) {
        snprintf(err, errlen, "in: the currency code of 3 letters the price is given in (\"NOK\" by default)");
        return false;
    }
    if (!strcmp(c->fx.cur, c->fx.in)) {
        snprintf(err, errlen, "in: another currency than currency");
        return false;
    }
    c->fx.days = FX_DAYS_DEF;
    t = esl_json_obj_get(js, tok, obj, "days");
    if (t >= 0) {
        double d;
        if (!esl_source_num(js, &tok[t], &d) || d != (double)(long)d || d < FX_DAYS_MIN || d > FX_DAYS_MAX) {
            snprintf(err, errlen, "days: a whole number from %d to %d", FX_DAYS_MIN, FX_DAYS_MAX);
            return false;
        }
        c->fx.days = (uint8_t)d;
    }
    c->fx.unit = 0;
    t = esl_json_obj_get(js, tok, obj, "unit");
    if (t >= 0) {
        double d;
        if (!esl_source_num(js, &tok[t], &d) || (d != 1 && d != 100)) {
            snprintf(err, errlen, "unit: 1 or 100 (left out: 100 when one is worth less than 0.1)");
            return false;
        }
        c->fx.unit = (uint8_t)d;
    }
    return true;
}

static void fx_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_printf(out, ",\"currency\":\"%s\",\"in\":\"%s\",\"days\":%u", c->fx.cur, c->fx.in, (unsigned)c->fx.days);
    if (c->fx.unit)
        esl_sb_printf(out, ",\"unit\":%u", (unsigned)c->fx.unit);
}

#define BASE "https://data.norges-bank.no/api/data/EXR/B."

/* The series to ask for: cur's and in's NOK prices (NOK itself has none) */
static size_t fx_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    bool cn = !strcmp(c->fx.cur, "NOK"), in = !strcmp(c->fx.in, "NOK");
    int n = snprintf(buf, cap, BASE "%s%s%s.NOK.SP?format=csv&lastNObservations=%u&locale=en", cn ? "" : c->fx.cur,
                     cn || in ? "" : "+", in ? "" : c->fx.in, (unsigned)c->fx.days + 1);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ---- the answer ---- */

typedef struct {
    char date[11];  /* "2026-11-12" */
    double cur, in; /* the NOK price of one of cur and of in; 0 = not given (NOK: 1) */
} fx_row_t;

/* Field i (from 0) of a line of n bytes split by ';', into *f and *fl; false when it has fewer */
static bool field(const char *line, size_t n, unsigned i, const char **f, size_t *fl)
{
    const char *p = line, *end = line + n;
    for (unsigned k = 0; k < i; k++) {
        const char *q = memchr(p, ';', (size_t)(end - p));
        if (!q)
            return false;
        p = q + 1;
    }
    const char *q = memchr(p, ';', (size_t)(end - p));
    *f = p;
    *fl = (size_t)((q ? q : end) - p);
    return true;
}

static bool is(const char *f, size_t fl, const char *s)
{
    return fl == strlen(s) && memcmp(f, s, fl) == 0;
}

/* A decimal number of the CSV ("9.5325"); false when it is not one */
static bool number(const char *f, size_t fl, double *out)
{
    char buf[24];
    if (!fl || fl >= sizeof(buf))
        return false;
    memcpy(buf, f, fl);
    buf[fl] = '\0';
    char *end;
    *out = strtod(buf, &end);
    return end == buf + fl && *out > 0;
}

static bool fx_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                      esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    (void)now, (void)off;
    int col_cur = -1, col_mult = -1, col_time = -1, col_val = -1;
    fx_row_t row[FX_ROWS];
    unsigned nrow = 0;
    bool cn = !strcmp(c->fx.cur, "NOK"), in_nok = !strcmp(c->fx.in, "NOK");

    for (const char *p = body, *end = body + len; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = (size_t)((nl ? nl : end) - p);
        const char *line = p;
        p = nl ? nl + 1 : end;
        if (n && line[n - 1] == '\r')
            n--;
        if (!n)
            continue;
        const char *f;
        size_t fl;
        if (col_val < 0) {
            /* the header */
            for (unsigned i = 0; field(line, n, i, &f, &fl); i++) {
                if (is(f, fl, "BASE_CUR"))
                    col_cur = (int)i;
                else if (is(f, fl, "UNIT_MULT"))
                    col_mult = (int)i;
                else if (is(f, fl, "TIME_PERIOD"))
                    col_time = (int)i;
                else if (is(f, fl, "OBS_VALUE"))
                    col_val = (int)i;
            }
            if (col_cur < 0 || col_time < 0 || col_val < 0) {
                snprintf(err, errlen, "the answer is not Norges Bank's exchange rates in CSV");
                return false;
            }
            continue;
        }
        char cur[4] = "", date[11] = "";
        double val, mult = 1;
        if (!field(line, n, (unsigned)col_cur, &f, &fl) || fl != 3)
            continue;
        memcpy(cur, f, 3);
        if (!field(line, n, (unsigned)col_time, &f, &fl) || fl != 10)
            continue;
        memcpy(date, f, 10);
        if (!field(line, n, (unsigned)col_val, &f, &fl) || !number(f, fl, &val))
            continue;
        if (col_mult >= 0 && field(line, n, (unsigned)col_mult, &f, &fl) && fl == 1 && f[0] >= '0' && f[0] <= '4')
            for (char k = '0'; k < f[0]; k++)
                mult *= 10;
        bool is_cur = !strcmp(cur, c->fx.cur), is_in = !strcmp(cur, c->fx.in);
        if (!is_cur && !is_in)
            continue;
        /* the row of that date: the dates come in order within each currency */
        unsigned r;
        for (r = 0; r < nrow && strcmp(row[r].date, date); r++)
            ;
        if (r == nrow) {
            if (nrow == FX_ROWS)
                continue;
            memset(&row[nrow], 0, sizeof(row[0]));
            memcpy(row[nrow].date, date, 11);
            nrow++;
        }
        if (is_cur)
            row[r].cur = val / mult;
        else
            row[r].in = val / mult;
    }
    if (col_val < 0) {
        snprintf(err, errlen, "the answer is empty");
        return false;
    }

    /* the days with both prices, oldest first (dates sort as text) */
    double rate[FX_ROWS];
    char date[FX_ROWS][11];
    unsigned n = 0;
    for (unsigned k = 0; k < nrow; k++) {
        unsigned best = nrow;
        for (unsigned r = 0; r < nrow; r++)
            if (row[r].date[0] && (best == nrow || strcmp(row[r].date, row[best].date) < 0))
                best = r;
        double a = cn ? 1 : row[best].cur, b = in_nok ? 1 : row[best].in;
        if (a > 0 && b > 0) {
            rate[n] = a / b;
            memcpy(date[n], row[best].date, 11);
            n++;
        }
        row[best].date[0] = '\0';
    }
    if (!n) {
        snprintf(err, errlen, "the answer has no rates for %s in %s", c->fx.cur, c->fx.in);
        return false;
    }

    /* the chart is the last days rates; the one before them gives the first change */
    unsigned first = n > c->fx.days ? n - c->fx.days : 0;
    unsigned unit = c->fx.unit ? c->fx.unit : rate[n - 1] < 0.1 ? 100 : 1;
    for (unsigned i = 0; i < n; i++)
        rate[i] *= unit;
    bool en = !strcmp(c->lang, "en");
    double now_r = rate[n - 1], hi = rate[first], lo = rate[first];
    for (unsigned i = first; i < n; i++) {
        if (rate[i] > hi)
            hi = rate[i];
        if (rate[i] < lo)
            lo = rate[i];
    }
    unsigned dec = now_r < 10 ? 4 : 2;
    char buf[ESL_VALUE_LONG + 1];
    v->n = 0;
    esl_values_set(v, "cur", c->fx.cur);
    esl_values_set(v, "in", c->fx.in);
    if (unit == 100)
        snprintf(buf, sizeof(buf), "100 %s/%s", c->fx.cur, c->fx.in);
    else
        snprintf(buf, sizeof(buf), "%s/%s", c->fx.cur, c->fx.in);
    esl_values_set(v, "pair", buf);
    snprintf(buf, sizeof(buf), "%u", unit);
    esl_values_set(v, "unit", buf);
    esl_source_fmt(now_r, dec, en, true, false, buf, sizeof(buf));
    esl_values_set(v, "rate", buf);
    esl_source_fmt(now_r, 2, en, true, false, buf, sizeof(buf));
    esl_values_set(v, "rate2", buf);
    esl_source_fmt(now_r, 4, en, true, false, buf, sizeof(buf));
    esl_values_set(v, "rate4", buf);
    if (n >= 2) {
        double prev = rate[n - 2];
        esl_source_fmt(now_r - prev, dec, en, true, true, buf, sizeof(buf));
        esl_values_set(v, "chg", buf);
        esl_source_fmt((now_r - prev) * 100 / prev, 1, en, false, true, buf, sizeof(buf));
        esl_values_set(v, "pct", buf);
    }
    esl_source_fmt((now_r - rate[first]) * 100 / rate[first], 1, en, false, true, buf, sizeof(buf));
    esl_values_set(v, "pctp", buf);
    esl_source_fmt(hi, dec, en, true, false, buf, sizeof(buf));
    esl_values_set(v, "hi", buf);
    esl_source_fmt(lo, dec, en, true, false, buf, sizeof(buf));
    esl_values_set(v, "lo", buf);
    const char *d = date[n - 1];
    snprintf(buf, sizeof(buf), "%.2s.%.2s.", d + 8, d + 5);
    esl_values_set(v, "date", buf);
    /* the day of the rate, in words when it is today or yesterday (local) */
    {
        int32_t today = (int32_t)((now + (uint32_t)(off ? off(now) : 0)) / 86400);
        char iso[21];
        snprintf(iso, sizeof(iso), "%.10sT00:00:00Z", d);
        uint32_t t = esl_iso_time(iso, 20);
        int32_t day = (int32_t)(t / 86400);
        if (t && now && day == today)
            esl_values_set(v, "day", en ? "today" : "i dag");
        else if (t && now && day == today - 1)
            esl_values_set(v, "day", en ? "yesterday" : "i går");
        else
            esl_values_set(v, "day", buf);
        *data_at = t;
    }
    snprintf(buf, sizeof(buf), "%u", n - first);
    esl_values_set(v, "days", buf);
    snprintf(buf, sizeof(buf), en ? "%u days" : "%u dager", n - first);
    esl_values_set(v, "period", buf);
    esl_source_series(rate + first, n - first, buf, sizeof(buf));
    esl_values_set(v, "chart", buf);
    return true;
}

const esl_source_def_t esl_source_fx = {
    .name = "fx",
    .type = ESL_SOURCE_FX,
    .about = "a currency's exchange rate from Norges Bank",
    .keys = fx_keys,
    .fields = fx_fields,
    .cfg_keys = fx_cfg_keys,
    .every_min = 60,  /* a new rate once a business day; no Expires in the answer */
    .own_pace = true,
    .parse = fx_parse,
    .write = fx_write,
    .url = fx_url,
    .values = fx_values,
};
