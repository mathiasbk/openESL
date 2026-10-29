#include "esl_clock.h"
#include <stdio.h>
#include <string.h>

static volatile int32_t today = -1; /* one aligned word: read and written whole */

void esl_clock_set_today(int32_t days)
{
    today = days < 0 ? -1 : days;
}

bool esl_clock_today(int32_t *days)
{
    *days = today;
    return *days >= 0;
}

typedef struct {
    const char *name;
    uint8_t res;
} field_t;

enum { F_WEEKDAY, F_DATE, F_DATE_SHORT, F_WEEK, F_COUNT };
static const field_t fields[F_COUNT] = {
    {"weekday", ESL_CLOCK_DAY},
    {"date", ESL_CLOCK_DAY},
    {"date_short", ESL_CLOCK_DAY},
    {"week", ESL_CLOCK_WEEK},
};

/* The field whose {name} starts t (n bytes left), or -1; *len its length with the braces */
static int field_at(const uint8_t *t, size_t n, size_t *len)
{
    if (n < 3 || t[0] != '{')
        return -1;
    for (int f = 0; f < F_COUNT; f++) {
        size_t k = strlen(fields[f].name);
        if (n >= k + 2 && memcmp(t + 1, fields[f].name, k) == 0 && t[k + 1] == '}') {
            *len = k + 2;
            return f;
        }
    }
    return -1;
}

uint8_t esl_clock_res(const uint8_t *t, size_t n)
{
    uint8_t res = ESL_CLOCK_NONE;
    for (size_t i = 0; i < n; i++) {
        size_t len;
        int f = field_at(t + i, n - i, &len);
        if (f >= 0 && fields[f].res > res)
            res = fields[f].res;
    }
    return res;
}

/* Year, month (1-12) and day (1-31) of days since 1970-01-01 (the proleptic Gregorian
   calendar, by 400-year eras) */
static void civil(int32_t days, int32_t *y, unsigned *m, unsigned *d)
{
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                         /* 0..146096 */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* 0..399 */
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               /* 0..365, from March */
    unsigned mp = (5 * doy + 2) / 153;                                    /* 0..11, March = 0 */
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int32_t)yoe + era * 400 + (*m <= 2);
}

/* 0 = Monday .. 6 = Sunday (1970-01-01 was a Thursday) */
static unsigned weekday(int32_t days)
{
    int32_t w = (days + 3) % 7;
    return (unsigned)(w < 0 ? w + 7 : w);
}

/* ISO 8601 week: the week (Monday to Sunday) that holds its year's first Thursday is 1 */
static unsigned iso_week(int32_t days)
{
    int32_t thu = days - (int32_t)weekday(days) + 3; /* the Thursday of this week */
    int32_t y;
    unsigned m, d;
    civil(thu, &y, &m, &d);
    int32_t jan1 = thu;
    /* days from 1 January of the Thursday's year to it */
    static const uint16_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    jan1 -= (int32_t)(before[m - 1] + d - 1 + (leap && m > 2));
    return (unsigned)((thu - jan1) / 7 + 1);
}

static const char *const weekdays[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char *const months[12] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};

size_t esl_clock_fill(const uint8_t *t, size_t n, int32_t days, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o < cap;) {
        size_t len;
        int f = field_at(t + i, n - i, &len);
        if (f < 0) {
            out[o++] = t[i++];
            continue;
        }
        i += len;
        if (days < 0)
            continue;
        char v[32];
        int32_t y;
        unsigned m, d;
        civil(days, &y, &m, &d);
        if (f == F_WEEKDAY)
            snprintf(v, sizeof(v), "%s", weekdays[weekday(days)]);
        else if (f == F_DATE)
            snprintf(v, sizeof(v), "%u %s %ld", d, months[m - 1], (long)y);
        else if (f == F_DATE_SHORT)
            snprintf(v, sizeof(v), "%02u.%02u.%ld", d, m, (long)y);
        else
            snprintf(v, sizeof(v), "%u", iso_week(days));
        for (const char *p = v; *p && o < cap; p++)
            out[o++] = (uint8_t)*p;
    }
    return o;
}

uint32_t esl_clock_stamp(uint8_t res)
{
    int32_t days;
    uint32_t v;
    if (res == ESL_CLOCK_NONE)
        return 0;
    if (!esl_clock_today(&days) || days < 0)
        v = 0x0FFFFFFFu; /* unknown: drawn empty, and again once it is known */
    else if (res == ESL_CLOCK_WEEK)
        v = (uint32_t)(days - (int32_t)weekday(days)); /* its Monday */
    else
        v = (uint32_t)days;
    return ((uint32_t)res << 28) ^ (v & 0x0FFFFFFFu);
}
