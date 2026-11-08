#include "esl_source.h"
#include <stdio.h>
#include <string.h>

/* Data source type "countdown": the days to (or since) a date, made on the AP with no
 * network: Christmas, a holiday, a birthday, the days since a date. With "yearly" it
 * counts to the next time the date comes round, and {years} gives how many years it will
 * be (an age, an anniversary). With a "time", the last day counts down in hours and
 * minutes. The values are made again every every_min minutes; a tag only wakes up when
 * its text changed (at local midnight for a date alone). */

static const char *const cd_keys[] = {"place", "left", "days", "weeks", "hours", "since", "date", "weekday", "years",
                                      NULL};
/* Three lines without words of one language: the name, the time left and the day */
static const char *const cd_fields[] = {"{place}", "{left}", "{weekday} {date}", NULL};
static const char *const cd_cfg_keys[] = {"date", "time", "yearly", NULL};

/* ---- dates ---- */

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static unsigned mdays(int y, unsigned m)
{
    static const uint8_t n[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : n[m - 1];
}

/* Days since 1970-01-01 of a date, and the other way */
static int32_t days_of(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

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

/* A yearly date in year y: 29 February is 28 February in other years */
static int32_t days_in_year(int y, unsigned m, unsigned d)
{
    return days_of(y, m, m == 2 && d == 29 && !leap(y) ? 28 : d);
}

static bool num(const char *s, unsigned n, unsigned *out)
{
    *out = 0;
    for (unsigned i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        *out = *out * 10 + (unsigned)(s[i] - '0');
    }
    return true;
}

/* ---- the configuration ---- */

static bool cd_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "date");
    const char *s = t >= 0 ? js + tok[t].start : "";
    unsigned y, m, d;
    if (t < 0 || tok[t].type != ESL_JSON_STRING || tok[t].end - tok[t].start != 10 || s[4] != '-' || s[7] != '-' ||
        !num(s, 4, &y) || !num(s + 5, 2, &m) || !num(s + 8, 2, &d) || y < 1900 || y > 2100 || m < 1 || m > 12 ||
        d < 1 || d > mdays((int)y, m)) {
        snprintf(err, errlen, "date: a date \"YYYY-MM-DD\" from 1900 to 2100");
        return false;
    }
    c->countdown.year = (uint16_t)y;
    c->countdown.month = (uint8_t)m;
    c->countdown.day = (uint8_t)d;
    c->countdown.minute = -1;
    t = esl_json_obj_get(js, tok, obj, "time");
    if (t >= 0) {
        unsigned h, mi;
        s = js + tok[t].start;
        if (tok[t].type != ESL_JSON_STRING || tok[t].end - tok[t].start != 5 || s[2] != ':' || !num(s, 2, &h) ||
            !num(s + 3, 2, &mi) || h > 23 || mi > 59) {
            snprintf(err, errlen, "time: a local time \"HH:MM\"");
            return false;
        }
        c->countdown.minute = (int16_t)(h * 60 + mi);
    }
    c->countdown.yearly = false;
    t = esl_json_obj_get(js, tok, obj, "yearly");
    if (t >= 0) {
        if (tok[t].type != ESL_JSON_TRUE && tok[t].type != ESL_JSON_FALSE) {
            snprintf(err, errlen, "yearly: true or false");
            return false;
        }
        c->countdown.yearly = tok[t].type == ESL_JSON_TRUE;
    }
    return true;
}

static void cd_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_printf(out, ",\"date\":\"%04u-%02u-%02u\"", (unsigned)c->countdown.year, (unsigned)c->countdown.month,
                  (unsigned)c->countdown.day);
    if (c->countdown.minute >= 0)
        esl_sb_printf(out, ",\"time\":\"%02d:%02d\"", c->countdown.minute / 60, c->countdown.minute % 60);
    esl_sb_printf(out, ",\"yearly\":%s", c->countdown.yearly ? "true" : "false");
}

/* Nothing is fetched: the "URL" only names the data, so the state is kept while the date
   stays the same */
static size_t cd_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "countdown:%04u-%02u-%02u/%d/%d", (unsigned)c->countdown.year,
                     (unsigned)c->countdown.month, (unsigned)c->countdown.day, (int)c->countdown.minute,
                     (int)c->countdown.yearly);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ---- the values ---- */

static bool cd_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                      esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    (void)body, (void)len, (void)err, (void)errlen;
    bool en = !strcmp(c->lang, "en");
    int32_t o = off ? off(now) : 0;
    int64_t local = (int64_t)now + o; /* local seconds since 1970 */
    int32_t today = (int32_t)(local / 86400);
    int y0;
    unsigned m0, d0;
    civil(today, &y0, &m0, &d0);
    int minute = c->countdown.minute;
    /* the target day, and the moment (local s) it is reached: its time, or its start */
    int ty = c->countdown.year;
    int32_t day = days_of(ty, c->countdown.month, c->countdown.day);
    if (c->countdown.yearly) {
        ty = y0;
        day = days_in_year(ty, c->countdown.month, c->countdown.day);
        int64_t at = (int64_t)day * 86400 + (minute >= 0 ? minute * 60 : 0);
        /* passed this year: next year's (a date alone holds for its whole day) */
        if (day < today || (day == today && minute >= 0 && at <= local)) {
            ty++;
            day = days_in_year(ty, c->countdown.month, c->countdown.day);
        }
    }
    int64_t at = (int64_t)day * 86400 + (minute >= 0 ? minute * 60 : 0);
    int32_t diff = day - today;
    int64_t secs = at - local;
    char buf[ESL_VALUE_MAX + 1];
    v->n = 0;
    esl_values_set(v, "place", c->place);

    if (diff > 1)
        snprintf(buf, sizeof(buf), en ? "in %ld days" : "om %ld dager", (long)diff);
    else if (diff == 1)
        snprintf(buf, sizeof(buf), "%s", en ? "tomorrow" : "i morgen");
    else if (diff == 0 && minute >= 0 && secs > 0) {
        long mins = (long)((secs + 59) / 60), h = mins / 60;
        if (h)
            snprintf(buf, sizeof(buf), en ? "in %ld h %ld min" : "om %ld t %ld min", h, mins % 60);
        else
            snprintf(buf, sizeof(buf), en ? "in %ld min" : "om %ld min", mins);
    } else if (diff == 0)
        snprintf(buf, sizeof(buf), "%s", en ? "today" : "i dag");
    else if (diff == -1)
        snprintf(buf, sizeof(buf), "%s", en ? "yesterday" : "i går");
    else
        snprintf(buf, sizeof(buf), en ? "%ld days ago" : "for %ld dager siden", (long)-diff);
    esl_values_set(v, "left", buf);

    long left = diff > 0 ? (long)diff : 0;
    snprintf(buf, sizeof(buf), "%ld", left);
    esl_values_set(v, "days", buf);
    snprintf(buf, sizeof(buf), "%ld", left / 7);
    esl_values_set(v, "weeks", buf);
    snprintf(buf, sizeof(buf), "%ld", secs > 0 ? (long)(secs / 3600) : 0L);
    esl_values_set(v, "hours", buf);
    snprintf(buf, sizeof(buf), "%ld", diff < 0 ? (long)-diff : 0L);
    esl_values_set(v, "since", buf);

    int y;
    unsigned m, d;
    civil(day, &y, &m, &d);
    snprintf(buf, sizeof(buf), "%02u.%02u.%04d", d, m, y);
    esl_values_set(v, "date", buf);
    static const char *const wd_nb[7] = {"torsdag", "fredag", "lørdag", "søndag", "mandag", "tirsdag", "onsdag"};
    static const char *const wd_en[7] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
    int w = (int)(((day % 7) + 7) % 7); /* 1970-01-01 was a Thursday */
    esl_values_set(v, "weekday", en ? wd_en[w] : wd_nb[w]);
    if (c->countdown.yearly) {
        snprintf(buf, sizeof(buf), "%d", ty - (int)c->countdown.year);
        esl_values_set(v, "years", buf);
    }
    *data_at = now;
    return true;
}

const esl_source_def_t esl_source_countdown = {
    .name = "countdown",
    .type = ESL_SOURCE_COUNTDOWN,
    .about = "the days to or since a date, made on the AP",
    .keys = cd_keys,
    .fields = cd_fields,
    .cfg_keys = cd_cfg_keys,
    .every_min = 1,
    .own_pace = true,
    .local = true, /* nothing to fetch */
    .parse = cd_parse,
    .write = cd_write,
    .url = cd_url,
    .values = cd_values,
};
