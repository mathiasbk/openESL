#include "esl_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data source type "btc": the price of bitcoin, from CoinGecko's public API (free, no key;
 * its terms ask for "Powered by CoinGecko" where the data is shown). One answer gives the
 * price at 00:00 UTC of each of the last days and, last, the price now:
 *   {"prices":[[1792022400000,774898.10],...,[1794561530000,658464.45]],"market_caps":[...],
 *    "total_volumes":[...]}
 * in any currency CoinGecko has ("nok", "usd", "eur", "sek", ...). */

static const char *const btc_keys[] = {"cur",  "price", "pct",    "today", "pctp",  "hi", "lo",
                                       "date", "updated", "days", "period", "chart", NULL};
/* The default templates, for the built-in text layout: without words of one language */
static const char *const btc_fields[] = {"BTC {cur} {updated}", "{price}", "{pct} %", "{lo} - {hi}", NULL};
static const char *const btc_cfg_keys[] = {"currency", "days", NULL};

#define BTC_DAYS_MIN 7
#define BTC_DAYS_MAX 30 /* 30 days and now: 31 points of a chart fit in ESL_VALUE_LONG */
#define BTC_DAYS_DEF 30
#define BTC_POINTS   (BTC_DAYS_MAX + 2)

static bool btc_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "currency");
    memcpy(c->btc.cur, "nok", 4);
    if (t >= 0) {
        int n = tok[t].end - tok[t].start;
        bool ok = tok[t].type == ESL_JSON_STRING && n >= 3 && n <= 5;
        for (int i = 0; ok && i < n; i++) {
            char ch = js[tok[t].start + i];
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
            ok = ch >= 'a' && ch <= 'z';
            c->btc.cur[i] = ch;
        }
        if (!ok) {
            snprintf(err, errlen, "currency: what the price is given in, as CoinGecko names it (\"nok\", \"usd\", ...)");
            return false;
        }
        c->btc.cur[n] = '\0';
    }
    c->btc.days = BTC_DAYS_DEF;
    t = esl_json_obj_get(js, tok, obj, "days");
    if (t >= 0) {
        double d;
        if (!esl_source_num(js, &tok[t], &d) || d != (double)(long)d || d < BTC_DAYS_MIN || d > BTC_DAYS_MAX) {
            snprintf(err, errlen, "days: a whole number from %d to %d", BTC_DAYS_MIN, BTC_DAYS_MAX);
            return false;
        }
        c->btc.days = (uint8_t)d;
    }
    return true;
}

static void btc_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_printf(out, ",\"currency\":\"%s\",\"days\":%u", c->btc.cur, (unsigned)c->btc.days);
}

static size_t btc_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap,
                     "https://api.coingecko.com/api/v3/coins/bitcoin/market_chart?vs_currency=%s&days=%u&interval=daily",
                     c->btc.cur, (unsigned)c->btc.days);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* A price: whole with spaces between thousands from 100 on, else two decimals */
static void fmt_price(double d, bool en, char *out, size_t cap)
{
    double a = d < 0 ? -d : d;
    esl_source_fmt(d, a >= 100 ? 0 : 2, en, true, false, out, cap);
}

static bool btc_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                       esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    (void)now;
    unsigned max_tok = 2;
    for (size_t i = 0; i < len; i++)
        max_tok += body[i] == '{' || body[i] == '[' || body[i] == ',' || body[i] == ':';
    esl_jtok_t *tok = len <= ESL_JSON_MAX_LEN ? malloc(sizeof(*tok) * max_tok) : NULL;
    double price[BTC_POINTS];
    uint32_t at[BTC_POINTS];
    unsigned n = 0, skipped = 0;
    bool ok = false;
    if (!tok) {
        snprintf(err, errlen, len > ESL_JSON_MAX_LEN ? "the answer is too big" : "out of memory");
        return false;
    }
    int p;
    if (esl_json_parse(body, len, tok, max_tok) < 0 || tok[0].type != ESL_JSON_OBJECT ||
        (p = esl_json_obj_get(body, tok, 0, "prices")) < 0 || tok[p].type != ESL_JSON_ARRAY) {
        snprintf(err, errlen, "the answer is not CoinGecko's prices");
        goto done;
    }
    /* the last BTC_POINTS of them */
    if (tok[p].size > BTC_POINTS)
        skipped = (unsigned)tok[p].size - BTC_POINTS;
    for (int k = p + 1, i = 0; i < tok[p].size; i++, k = tok[k].next) {
        double t, x;
        if ((unsigned)i < skipped || tok[k].type != ESL_JSON_ARRAY || tok[k].size != 2 ||
            !esl_source_num(body, &tok[k + 1], &t) || !esl_source_num(body, &tok[tok[k + 1].next], &x) || x <= 0)
            continue;
        price[n] = x;
        at[n] = (uint32_t)(t / 1000);
        n++;
    }
    if (n < 2) {
        snprintf(err, errlen, "the answer has no prices");
        goto done;
    }
    bool en = !strcmp(c->lang, "en");
    double now_p = price[n - 1], hi = price[0], lo = price[0];
    for (unsigned i = 0; i < n; i++) {
        if (price[i] > hi)
            hi = price[i];
        if (price[i] < lo)
            lo = price[i];
    }
    char buf[ESL_VALUE_LONG + 1];
    v->n = 0;
    for (unsigned i = 0; c->btc.cur[i] && i < sizeof(buf) - 1; i++) {
        char ch = c->btc.cur[i];
        buf[i] = (char)(ch - 'a' + 'A');
        buf[i + 1] = '\0';
    }
    esl_values_set(v, "cur", buf);
    fmt_price(now_p, en, buf, sizeof(buf));
    esl_values_set(v, "price", buf);
    /* since the day's first price (00:00 UTC), and over the whole chart */
    esl_source_fmt((now_p - price[n - 2]) * 100 / price[n - 2], 1, en, false, true, buf, sizeof(buf));
    esl_values_set(v, "pct", buf);
    esl_values_set(v, "today", en ? "today" : "i dag");
    esl_source_fmt((now_p - price[0]) * 100 / price[0], 1, en, false, true, buf, sizeof(buf));
    esl_values_set(v, "pctp", buf);
    fmt_price(hi, en, buf, sizeof(buf));
    esl_values_set(v, "hi", buf);
    fmt_price(lo, en, buf, sizeof(buf));
    esl_values_set(v, "lo", buf);
    uint32_t local = at[n - 1] + (uint32_t)(off ? off(at[n - 1]) : 0);
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(local % 86400 / 3600), (unsigned)(local % 3600 / 60));
    esl_values_set(v, "updated", buf);
    {
        /* the day, from days since 1970-01-01 */
        int32_t z = (int32_t)(local / 86400) + 719468;
        int32_t era = z / 146097;
        unsigned doe = (unsigned)(z - era * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
        snprintf(buf, sizeof(buf), "%02u.%02u.", d, m);
        esl_values_set(v, "date", buf);
    }
    *data_at = at[n - 1];
    snprintf(buf, sizeof(buf), "%u", n - 1);
    esl_values_set(v, "days", buf);
    snprintf(buf, sizeof(buf), en ? "%u days" : "%u dager", n - 1);
    esl_values_set(v, "period", buf);
    esl_source_series(price, n, buf, sizeof(buf));
    esl_values_set(v, "chart", buf);
    ok = true;
done:
    free(tok);
    return ok;
}

const esl_source_def_t esl_source_btc = {
    .name = "btc",
    .type = ESL_SOURCE_BTC,
    .about = "the price of bitcoin from CoinGecko",
    .keys = btc_keys,
    .fields = btc_fields,
    .cfg_keys = btc_cfg_keys,
    .every_min = 15, /* CoinGecko's free API takes a few calls a minute; the price moves all day */
    .own_pace = true, /* its answers expire after 30 s */
    .parse = btc_parse,
    .write = btc_write,
    .url = btc_url,
    .values = btc_values,
};
