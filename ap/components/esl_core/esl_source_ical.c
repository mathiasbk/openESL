#include "esl_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data source type "ical": the next events of a calendar (an iCalendar .ics address, such as
 * the secret address of a Google, Outlook, iCloud or Nextcloud calendar), as values in the
 * fields of a layout: a meeting room's door sign, the family's week on the fridge.
 *
 * Calendars are often bigger than ESL_SOURCE_BODY_MAX, so the answer is read as it comes
 * (stream_new, stream_feed, stream_end): line by line, keeping only the next KEEP
 * occurrences. Repeating events (RRULE) are expanded here, with EXDATE and moved single
 * occurrences (RECURRENCE-ID). What is kept is a short text, the "body" that is cached and
 * given to values():
 *     X <the calendar's name>
 *     <start> <end> <all day 0/1> <summary>      one line per occurrence, Unix s, by start
 *
 * Times with a TZID, and floating times, are read in the AP's own time zone: a calendar in
 * another zone shows its events at the wrong hours. Times in UTC ("Z") are right anywhere.
 * Rules this does not follow (BYSETPOS, BYWEEKNO, BYYEARDAY, BYHOUR and the like) give the
 * event's first occurrence only. */

#define KEEP      24                   /* occurrences kept, by start */
#define SHOWN     6                    /* {day1} to {day6} */
#define OVR_MAX   32                   /* moved occurrences (RECURRENCE-ID) remembered */
#define EXD_MAX   16                   /* EXDATEs of one event */
#define LINE_MAX  1024                 /* bytes of one unfolded line (the rest is left out) */
#define RULE_MAX  192                  /* bytes of an RRULE */
#define HORIZON_S (400 * 86400)        /* occurrences further ahead are not kept */
#define PERIODS   4000                 /* periods of one rule looked at, at most */
#define BUDGET    2000000L             /* candidate days of all rules in one calendar */

static const char *const ical_keys[] = {"title", "today", "day1",  "time1", "what1", "day2", "time2",
                                        "what2", "day3",  "time3", "what3", "day4", "time4", "what4",
                                        "day5",  "time5", "what5", "day6",  "time6", "what6", NULL};
static const char *const ical_fields[] = {"{title}", "{day1} {time1} {what1}", "{day2} {time2} {what2}",
                                          "{day3} {time3} {what3}", NULL};
static const char *const ical_cfg_keys[] = {"url", NULL};

static bool ical_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "url");
    char u[ESL_SOURCE_URL_MAX + 1];
    int n = t >= 0 && tok[t].type == ESL_JSON_STRING ? esl_json_str_decode(js, &tok[t], u, sizeof(u) - 1) : -1;
    if (n >= 0) {
        u[n] = '\0';
        /* webcal:// is how calendars offer a subscription: the same address over https */
        if (!strncmp(u, "webcal://", 9)) {
            char h[ESL_SOURCE_URL_MAX + 2];
            snprintf(h, sizeof(h), "https://%s", u + 9);
            n = (int)strlen(h) < ESL_SOURCE_URL_MAX ? (int)strlen(h) : -1;
            if (n >= 0)
                memcpy(u, h, (size_t)n + 1);
        }
    }
    if (n < 0 || n >= ESL_SOURCE_URL_MAX || !esl_source_url_ok(u)) {
        snprintf(err, errlen,
                 "url: an http://, https:// or webcal:// URL of an iCalendar file, up to %d bytes, without spaces or a login",
                 ESL_SOURCE_URL_MAX - 1);
        return false;
    }
    memcpy(c->ical.url, u, (size_t)n + 1);
    return true;
}

static void ical_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_puts(out, ",\"url\":");
    esl_sb_json_str(out, c->ical.url);
}

static size_t ical_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s", c->ical.url);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

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

/* 0 = Monday ... 6 = Sunday (1970-01-01 was a Thursday) */
static int wday(int32_t z)
{
    return (int)(((z % 7) + 7 + 3) % 7);
}

static int64_t fdiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* ---- reading the calendar ---- */

enum { F_UTC, F_LOCAL, F_DATE }; /* how a time is given: in UTC, local, a date (all day) */

typedef struct {
    int64_t wall; /* s since 1970 on the clock it is given in */
    uint8_t frame;
} dt_t;

typedef struct {
    int64_t start, end; /* Unix s */
    uint32_t uid;
    bool allday;
    bool single; /* a moved occurrence (it has a RECURRENCE-ID) */
    char what[ESL_VALUE_MAX * 2 + 1]; /* fit() to ESL_VALUE_MAX */
} occ_t;

typedef struct {
    uint8_t freq; /* 0: none, 1 daily, 2 weekly, 3 monthly, 4 yearly */
    bool bad;     /* a part this does not follow: the first occurrence only */
    uint16_t interval;
    uint32_t count;
    bool has_until;
    int64_t until; /* Unix s */
    uint8_t nday;
    struct {
        int8_t ord; /* 0: every such day; 1, 2, -1: the first, second, last in the month */
        uint8_t wd;
    } day[14];
    uint8_t nmday;
    int8_t mday[8];
    uint16_t months; /* bit m (1-12); 0: any */
} rule_t;

typedef struct {
    const esl_source_cfg_t *cfg;
    uint32_t now;
    esl_utc_offset_fn off;
    char line[LINE_MAX];
    size_t len;
    bool eol, calendar;
    int depth, ev_depth;
    long budget;
    char title[ESL_VALUE_MAX + 1];
    /* the event being read */
    struct {
        bool on, has_start, has_end, has_dur, has_rid, cancelled;
        dt_t start, end;
        int64_t dur;
        uint32_t uid;
        int64_t rid;
        char rule[RULE_MAX];
        int64_t exd[EXD_MAX];
        uint8_t nexd;
        char what[ESL_VALUE_MAX * 2 + 1];
    } ev;
    occ_t keep[KEEP];
    unsigned nkeep;
    struct {
        uint32_t uid;
        int64_t rid;
    } ovr[OVR_MAX];
    unsigned novr;
} ical_t;

static int64_t to_utc(const ical_t *s, dt_t t)
{
    if (t.frame == F_UTC || !s->off)
        return t.wall;
    /* the offset at the moment itself: two rounds settle a summer time switch */
    int64_t u = t.wall - s->off((uint32_t)t.wall);
    return t.wall - s->off((uint32_t)(t.wall - s->off((uint32_t)u)));
}

static bool digits(const char *p, unsigned n, unsigned *out)
{
    *out = 0;
    for (unsigned i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        *out = *out * 10 + (unsigned)(p[i] - '0');
    }
    return true;
}

/* "20261108", "20261108T090000", "20261108T090000Z" */
static bool parse_dt(const char *v, size_t n, bool date, dt_t *t)
{
    unsigned y, m, d, hh = 0, mi = 0, ss = 0;
    if (n < 8 || !digits(v, 4, &y) || !digits(v + 4, 2, &m) || !digits(v + 6, 2, &d) || y < 1900 || y > 2200 ||
        m < 1 || m > 12 || d < 1 || d > mdays((int)y, m))
        return false;
    t->frame = F_DATE;
    if (n >= 15 && v[8] == 'T' && !date) {
        if (!digits(v + 9, 2, &hh) || !digits(v + 11, 2, &mi) || !digits(v + 13, 2, &ss) || hh > 23 || mi > 59 ||
            ss > 60)
            return false;
        t->frame = n >= 16 && v[15] == 'Z' ? F_UTC : F_LOCAL;
    }
    t->wall = (int64_t)days_of((int)y, m, d) * 86400 + hh * 3600 + mi * 60 + (ss > 59 ? 59 : ss);
    return true;
}

/* "PT1H30M", "P1D", "P2W", "-PT15M" (s) */
static bool parse_dur(const char *v, size_t n, int64_t *out)
{
    size_t i = 0;
    bool neg = false;
    if (i < n && (v[i] == '+' || v[i] == '-'))
        neg = v[i++] == '-';
    if (i >= n || v[i++] != 'P')
        return false;
    int64_t s = 0, num = 0;
    bool any = false;
    for (; i < n; i++) {
        char c = v[i];
        if (c >= '0' && c <= '9') {
            if (num > 100000000)
                return false;
            num = num * 10 + (c - '0');
            any = true;
            continue;
        }
        static const char units[] = "WDHMS";
        static const int64_t secs[] = {604800, 86400, 3600, 60, 1};
        const char *u = c ? strchr(units, c) : NULL;
        if (c == 'T')
            continue;
        if (!u)
            return false;
        s += num * secs[u - units];
        num = 0;
    }
    *out = neg ? -s : s;
    return any;
}

static uint32_t fnv(const char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h ? h : 1;
}

/* The text of a property: \n, \, and \; undone, control characters and bad UTF-8 left out,
   white space made single; into out (cap with the NUL) */
static void text_of(const char *v, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c == '\\' && i + 1 < n) {
            c = (unsigned char)v[++i];
            if (c == 'n' || c == 'N')
                c = ' ';
        }
        if (c < 0x20 || c == 0x7f)
            c = ' ';
        if (c == ' ' && (!k || out[k - 1] == ' '))
            continue;
        if (c >= 0x80) {
            /* a whole UTF-8 character or nothing */
            size_t w = (c & 0xe0) == 0xc0 && c >= 0xc2 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 && c <= 0xf4 ? 4 : 0;
            bool ok = w && i + w <= n && k + w < cap;
            for (size_t j = 1; ok && j < w; j++)
                ok = ((unsigned char)v[i + j] & 0xc0) == 0x80;
            if (ok) {
                memcpy(out + k, v + i, w);
                k += w;
            }
            i += w ? w - 1 : 0;
            continue;
        }
        out[k++] = (char)c;
    }
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = '\0';
}

/* A text cut to fit a value: at a character's start, with "..." */
static void fit(char *s)
{
    size_t n = strlen(s);
    if (n <= ESL_VALUE_MAX)
        return;
    n = ESL_VALUE_MAX - 3;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
        n--;
    while (n && s[n - 1] == ' ')
        n--;
    memcpy(s + n, "...", 4);
}

/* ---- repeating events ---- */

static int weekday_of(const char *p)
{
    static const char *const names[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    for (int i = 0; i < 7; i++)
        if (p[0] == names[i][0] && p[1] == names[i][1])
            return i;
    return -1;
}

/* The integer at *p (with a sign), moving past it */
static bool number(const char **p, const char *end, long *out)
{
    const char *q = *p;
    bool neg = q < end && (*q == '-' || *q == '+') ? *q++ == '-' : false;
    long v = 0;
    if (q >= end || *q < '0' || *q > '9')
        return false;
    while (q < end && *q >= '0' && *q <= '9' && v < 100000000)
        v = v * 10 + (*q++ - '0');
    *p = q;
    *out = neg ? -v : v;
    return true;
}

static void parse_rule(const ical_t *s, const char *r, rule_t *ru)
{
    memset(ru, 0, sizeof(*ru));
    ru->interval = 1;
    const char *p = r, *end = r + strlen(r);
    while (p < end) {
        const char *semi = memchr(p, ';', (size_t)(end - p)), *e = semi ? semi : end;
        const char *eq = memchr(p, '=', (size_t)(e - p));
        if (!eq) {
            ru->bad = true;
            break;
        }
        size_t kn = (size_t)(eq - p);
        const char *v = eq + 1;
        long num;
#define KEY(k) (kn == sizeof(k) - 1 && !memcmp(p, k, kn))
        if (KEY("FREQ")) {
            static const char *const f[] = {"DAILY", "WEEKLY", "MONTHLY", "YEARLY"};
            for (int i = 0; i < 4; i++)
                if ((size_t)(e - v) == strlen(f[i]) && !memcmp(v, f[i], strlen(f[i])))
                    ru->freq = (uint8_t)(i + 1);
            if (!ru->freq)
                ru->bad = true; /* SECONDLY, MINUTELY, HOURLY */
        } else if (KEY("INTERVAL")) {
            if (!number(&v, e, &num) || v != e || num < 1 || num > 1000)
                ru->bad = true;
            else
                ru->interval = (uint16_t)num;
        } else if (KEY("COUNT")) {
            if (!number(&v, e, &num) || v != e || num < 1)
                ru->bad = true;
            else
                ru->count = (uint32_t)num;
        } else if (KEY("UNTIL")) {
            dt_t t;
            if (!parse_dt(v, (size_t)(e - v), false, &t)) {
                ru->bad = true;
            } else {
                if (t.frame == F_DATE) {
                    t.wall += 86399; /* the whole last day */
                    t.frame = F_LOCAL;
                }
                ru->has_until = true;
                ru->until = to_utc(s, t);
            }
        } else if (KEY("BYDAY")) {
            while (v < e && !ru->bad) {
                long ord = 0;
                if (*v == '-' || *v == '+' || (*v >= '0' && *v <= '9'))
                    if (!number(&v, e, &ord) || ord == 0 || ord < -5 || ord > 5)
                        ru->bad = true;
                int wd = e - v >= 2 ? weekday_of(v) : -1;
                if (wd < 0 || ru->nday >= sizeof(ru->day) / sizeof(ru->day[0])) {
                    ru->bad = true;
                    break;
                }
                ru->day[ru->nday].ord = (int8_t)ord;
                ru->day[ru->nday++].wd = (uint8_t)wd;
                v += 2;
                if (v < e && *v++ != ',')
                    ru->bad = true;
            }
        } else if (KEY("BYMONTHDAY")) {
            while (v < e && !ru->bad) {
                if (!number(&v, e, &num) || num == 0 || num < -31 || num > 31 || ru->nmday >= 8)
                    ru->bad = true;
                else
                    ru->mday[ru->nmday++] = (int8_t)num;
                if (v < e && *v++ != ',')
                    ru->bad = true;
            }
        } else if (KEY("BYMONTH")) {
            while (v < e && !ru->bad) {
                if (!number(&v, e, &num) || num < 1 || num > 12)
                    ru->bad = true;
                else
                    ru->months |= (uint16_t)(1u << num);
                if (v < e && *v++ != ',')
                    ru->bad = true;
            }
        } else if (!KEY("WKST")) {
            ru->bad = true; /* BYSETPOS, BYWEEKNO, BYYEARDAY, BYHOUR, ... */
        }
#undef KEY
        p = semi ? semi + 1 : end;
    }
    if (!ru->freq)
        ru->bad = true;
    /* an ordinal weekday only means something in a month (or a year with BYMONTH) */
    for (unsigned i = 0; i < ru->nday; i++)
        if (ru->day[i].ord && (ru->freq == 1 || ru->freq == 2 || (ru->freq == 4 && !ru->months)))
            ru->bad = true;
    if (ru->freq == 4 && ru->nday && !ru->months)
        ru->bad = true; /* every Monday of a year */
}

static bool day_in(const rule_t *ru, int wd)
{
    for (unsigned i = 0; i < ru->nday; i++)
        if (ru->day[i].wd == wd)
            return true;
    return false;
}

static bool mday_in(const rule_t *ru, int y, unsigned m, unsigned d)
{
    for (unsigned i = 0; i < ru->nmday; i++) {
        int md = ru->mday[i] > 0 ? ru->mday[i] : (int)mdays(y, m) + 1 + ru->mday[i];
        if (md == (int)d)
            return true;
    }
    return false;
}

static int cmp32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y;
}

/* The days of month m of year y the rule gives (day0: the first occurrence's day of the
   month), sorted; their number */
static unsigned month_days(const rule_t *ru, int y, unsigned m, unsigned day0, int32_t *out)
{
    unsigned n = 0, len = mdays(y, m);
    int32_t first = days_of(y, m, 1);
    if (ru->nday) {
        for (unsigned i = 0; i < ru->nday; i++) {
            int wd = ru->day[i].wd, ord = ru->day[i].ord;
            unsigned d1 = (unsigned)((wd - wday(first) + 7) % 7) + 1; /* the first such day */
            unsigned cnt = (len - d1) / 7 + 1;
            for (unsigned k = 0; k < cnt; k++) {
                unsigned d = d1 + 7 * k;
                bool take = !ord || (ord > 0 && (unsigned)ord == k + 1) || (ord < 0 && (int)cnt + ord == (int)k);
                if (take && (!ru->nmday || mday_in(ru, y, m, d)) && n < 31)
                    out[n++] = first + (int32_t)d - 1;
            }
        }
    } else if (ru->nmday) {
        for (unsigned d = 1; d <= len; d++)
            if (mday_in(ru, y, m, d))
                out[n++] = first + (int32_t)d - 1;
    } else if (day0 <= len) {
        out[n++] = first + (int32_t)day0 - 1;
    }
    qsort(out, n, sizeof(out[0]), cmp32);
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++)
        if (!k || out[k - 1] != out[i])
            out[k++] = out[i];
    return k;
}

static void keep(ical_t *s, int64_t start, int64_t end, bool allday)
{
    if (end <= (int64_t)s->now || start > (int64_t)s->now + HORIZON_S)
        return;
    for (unsigned i = 0; i < s->ev.nexd; i++)
        if (s->ev.exd[i] == start)
            return;
    unsigned at = s->nkeep;
    if (s->nkeep == KEEP) {
        /* full: in place of the latest, when this one is earlier */
        at = 0;
        for (unsigned i = 1; i < KEEP; i++)
            if (s->keep[i].start > s->keep[at].start)
                at = i;
        if (s->keep[at].start <= start)
            return;
    } else {
        s->nkeep++;
    }
    occ_t *o = &s->keep[at];
    o->start = start;
    o->end = end;
    o->allday = allday;
    o->uid = s->ev.uid;
    o->single = s->ev.has_rid;
    snprintf(o->what, sizeof(o->what), "%s", s->ev.what);
    fit(o->what);
}

/* One occurrence at wall time w (the event's own clock) */
static void occurrence(ical_t *s, int64_t w, int64_t dur)
{
    dt_t t = {w, s->ev.start.frame};
    if (t.frame == F_DATE) {
        dt_t e = {w + dur, F_LOCAL};
        t.frame = F_LOCAL;
        keep(s, to_utc(s, t), to_utc(s, e), true);
    } else {
        int64_t u = to_utc(s, t);
        keep(s, u, u + dur, false);
    }
}

static void expand(ical_t *s, const rule_t *ru, int64_t dur)
{
    dt_t st = s->ev.start;
    int32_t z0 = (int32_t)fdiv(st.wall, 86400);
    int64_t sod = st.wall - (int64_t)z0 * 86400;
    int y0;
    unsigned m0, d0;
    civil(z0, &y0, &m0, &d0);
    /* where to begin: with no COUNT, whole periods before now are skipped */
    int64_t nowz = fdiv((int64_t)s->now + (st.frame == F_UTC || !s->off ? 0 : s->off(s->now)), 86400);
    int64_t back = nowz - z0 - dur / 86400 - 2;
    int64_t k = 0;
    if (!ru->count && back > 0) {
        int64_t per = ru->freq == 1 ? 1 : ru->freq == 2 ? 7 : ru->freq == 3 ? 31 : 366;
        int64_t periods = ru->freq == 3 ? back / 31 : ru->freq == 4 ? back / 366 : back / per;
        k = periods / ru->interval;
    }
    uint32_t n = 0;
    int32_t days[31 * 12];
    for (unsigned steps = 0; steps < PERIODS && s->budget > 0; steps++, k++) {
        unsigned nd = 0;
        int64_t step = k * ru->interval;
        if (ru->freq == 1) {
            int32_t z = z0 + (int32_t)step;
            int y;
            unsigned m, d;
            civil(z, &y, &m, &d);
            if ((!ru->nday || day_in(ru, wday(z))) && (!ru->months || ru->months & (1u << m)) &&
                (!ru->nmday || mday_in(ru, y, m, d)))
                days[nd++] = z;
        } else if (ru->freq == 2) {
            int32_t ws = z0 - wday(z0) + (int32_t)(7 * step);
            for (int wd = 0; wd < 7; wd++) {
                int y;
                unsigned m, d;
                civil(ws + wd, &y, &m, &d);
                if ((ru->nday ? day_in(ru, wd) : wd == wday(z0)) && (!ru->months || ru->months & (1u << m)))
                    days[nd++] = ws + wd;
            }
        } else if (ru->freq == 3) {
            int64_t mm = (int64_t)(m0 - 1) + step;
            int y = y0 + (int)(mm / 12);
            unsigned m = (unsigned)(mm % 12) + 1;
            if (y > 2200)
                break;
            if (!ru->months || ru->months & (1u << m))
                nd = month_days(ru, y, m, d0, days);
        } else {
            int y = y0 + (int)step;
            if (y > 2200)
                break;
            for (unsigned m = 1; m <= 12; m++)
                if (ru->months ? ru->months & (1u << m) : m == m0)
                    nd += month_days(ru, y, m, d0, days + nd);
        }
        s->budget -= nd + 1;
        for (unsigned i = 0; i < nd; i++) {
            if (days[i] < z0)
                continue;
            if (ru->count && ++n > ru->count)
                return;
            int64_t w = (int64_t)days[i] * 86400 + sod;
            dt_t t = {w, st.frame == F_DATE ? F_LOCAL : st.frame};
            int64_t u = to_utc(s, t);
            if ((ru->has_until && u > ru->until) || u > (int64_t)s->now + HORIZON_S)
                return;
            occurrence(s, w, dur);
        }
    }
}

static void end_event(ical_t *s)
{
    if (!s->ev.has_start || s->ev.cancelled)
        return;
    bool date = s->ev.start.frame == F_DATE;
    int64_t dur;
    if (s->ev.has_end && date) {
        dur = (fdiv(s->ev.end.wall, 86400) - fdiv(s->ev.start.wall, 86400)) * 86400;
    } else if (s->ev.has_end) {
        dur = to_utc(s, s->ev.end) - to_utc(s, s->ev.start);
    } else if (s->ev.has_dur) {
        dur = date ? s->ev.dur / 86400 * 86400 : s->ev.dur;
    } else {
        dur = date ? 86400 : 0;
    }
    if (date && dur < 86400)
        dur = 86400;
    if (dur < 0)
        dur = 0;
    if (dur > 366 * 86400)
        dur = 366 * 86400;
    if (s->ev.has_rid && s->novr < OVR_MAX) {
        s->ovr[s->novr].uid = s->ev.uid;
        s->ovr[s->novr++].rid = s->ev.rid;
    }
    rule_t ru;
    if (s->ev.rule[0] && !s->ev.has_rid) {
        parse_rule(s, s->ev.rule, &ru);
        if (!ru.bad) {
            expand(s, &ru, dur);
            return;
        }
    }
    occurrence(s, s->ev.start.wall, dur);
}

static bool name_is(const char *p, size_t n, const char *name)
{
    size_t k = strlen(name);
    if (n != k)
        return false;
    for (size_t i = 0; i < k; i++)
        if ((p[i] >= 'a' && p[i] <= 'z' ? p[i] - 32 : p[i]) != name[i])
            return false;
    return true;
}

/* One unfolded line: NAME;PARAM=...;PARAM="...":value */
static void line(ical_t *s, char *p, size_t n)
{
    if (n >= 3 && (uint8_t)p[0] == 0xef && (uint8_t)p[1] == 0xbb && (uint8_t)p[2] == 0xbf)
        p += 3, n -= 3; /* the BOM */
    size_t nl = 0;
    while (nl < n && p[nl] != ';' && p[nl] != ':')
        nl++;
    /* the value starts after the first ':' that is not in quotes */
    size_t i = nl;
    bool q = false;
    for (; i < n && (q || p[i] != ':'); i++)
        if (p[i] == '"')
            q = !q;
    if (i >= n)
        return;
    const char *par = p + nl, *v = p + i + 1;
    size_t pn = i - nl, vn = n - i - 1;
    bool date = false, zoned = false;
    for (size_t j = 0; j < pn; j++) {
        if (par[j] != ';')
            continue;
        if (pn - j > 10 && !memcmp(par + j + 1, "VALUE=DATE", 10) && (pn - j == 11 || par[j + 11] == ';'))
            date = true;
        if (pn - j > 5 && !memcmp(par + j + 1, "TZID=", 5))
            zoned = true;
    }
    (void)zoned; /* a TZID is read as the AP's own time zone */

    if (name_is(p, nl, "BEGIN")) {
        if (s->depth < 100)
            s->depth++;
        if (name_is(v, vn, "VCALENDAR"))
            s->calendar = true;
        if (name_is(v, vn, "VEVENT") && !s->ev.on) {
            memset(&s->ev, 0, sizeof(s->ev));
            s->ev.on = true;
            s->ev_depth = s->depth;
        }
        return;
    }
    if (name_is(p, nl, "END")) {
        if (s->ev.on && s->depth == s->ev_depth && name_is(v, vn, "VEVENT")) {
            end_event(s);
            s->ev.on = false;
        }
        if (s->depth > 0)
            s->depth--;
        return;
    }
    if (!s->ev.on || s->depth != s->ev_depth) {
        if (s->depth == 1 && name_is(p, nl, "X-WR-CALNAME")) {
            text_of(v, vn, s->title, sizeof(s->title));
            fit(s->title);
        }
        return;
    }
    if (name_is(p, nl, "SUMMARY")) {
        text_of(v, vn, s->ev.what, sizeof(s->ev.what));
    } else if (name_is(p, nl, "DTSTART")) {
        s->ev.has_start = parse_dt(v, vn, date, &s->ev.start);
    } else if (name_is(p, nl, "DTEND")) {
        s->ev.has_end = parse_dt(v, vn, date, &s->ev.end);
    } else if (name_is(p, nl, "DURATION")) {
        s->ev.has_dur = parse_dur(v, vn, &s->ev.dur);
    } else if (name_is(p, nl, "UID")) {
        s->ev.uid = fnv(v, vn);
    } else if (name_is(p, nl, "RRULE")) {
        if (vn < sizeof(s->ev.rule)) {
            memcpy(s->ev.rule, v, vn);
            s->ev.rule[vn] = '\0';
        } else {
            snprintf(s->ev.rule, sizeof(s->ev.rule), "X=too-long");
        }
    } else if (name_is(p, nl, "RECURRENCE-ID")) {
        dt_t t;
        if (parse_dt(v, vn, date, &t)) {
            if (t.frame == F_DATE)
                t.frame = F_LOCAL;
            s->ev.rid = to_utc(s, t);
            s->ev.has_rid = true;
        }
    } else if (name_is(p, nl, "EXDATE")) {
        for (const char *a = v, *e = v + vn; a < e && s->ev.nexd < EXD_MAX;) {
            const char *c = memchr(a, ',', (size_t)(e - a));
            dt_t t;
            if (parse_dt(a, (size_t)((c ? c : e) - a), date, &t)) {
                if (t.frame == F_DATE)
                    t.frame = F_LOCAL;
                s->ev.exd[s->ev.nexd++] = to_utc(s, t);
            }
            a = c ? c + 1 : e;
        }
    } else if (name_is(p, nl, "STATUS")) {
        s->ev.cancelled = name_is(v, vn, "CANCELLED");
    }
}

/* ---- the stream ---- */

static void *ical_new(const esl_source_cfg_t *c, uint32_t now, esl_utc_offset_fn off)
{
    ical_t *s = calloc(1, sizeof(*s));
    if (s) {
        s->cfg = c;
        s->now = now;
        s->off = off;
        s->budget = BUDGET;
    }
    return s;
}

static void ical_feed(void *st, const char *p, size_t n)
{
    ical_t *s = st;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (s->eol) {
            s->eol = false;
            if (c == ' ' || c == '\t')
                continue; /* a folded line goes on */
            line(s, s->line, s->len);
            s->len = 0;
        }
        if (c == '\n')
            s->eol = true;
        else if (c != '\r' && s->len < sizeof(s->line))
            s->line[s->len++] = c;
    }
}

static int by_start(const void *a, const void *b)
{
    const occ_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : strcmp(x->what, y->what);
}

static bool ical_end(void *st, char **body, size_t *len, char *err, size_t errlen)
{
    ical_t *s = st;
    bool ok = false;
    if (body) {
        if (s->len)
            line(s, s->line, s->len);
        if (!s->calendar) {
            snprintf(err, errlen, "not an iCalendar file (no BEGIN:VCALENDAR)");
        } else {
            /* occurrences a moved one replaces */
            unsigned k = 0;
            for (unsigned i = 0; i < s->nkeep; i++) {
                bool moved = false;
                for (unsigned j = 0; j < s->novr && !moved && !s->keep[i].single; j++)
                    moved = s->ovr[j].uid == s->keep[i].uid && s->ovr[j].rid == s->keep[i].start;
                if (!moved)
                    s->keep[k++] = s->keep[i];
            }
            s->nkeep = k;
            qsort(s->keep, s->nkeep, sizeof(s->keep[0]), by_start);
            size_t cap = 4 + sizeof(s->title) + KEEP * (2 * 21 + 4 + sizeof(s->keep[0].what));
            char *b = malloc(cap);
            if (!b) {
                snprintf(err, errlen, "no memory for the calendar");
            } else {
                int n = snprintf(b, cap, "X %s\n", s->title);
                for (unsigned i = 0; i < s->nkeep && n > 0 && (size_t)n < cap; i++)
                    n += snprintf(b + n, cap - (size_t)n, "%lld %lld %d %s\n", (long long)s->keep[i].start,
                                  (long long)s->keep[i].end, s->keep[i].allday, s->keep[i].what);
                *body = b;
                *len = n > 0 && (size_t)n < cap ? (size_t)n : 0;
                ok = true;
            }
        }
    }
    free(s);
    return ok;
}

/* ---- the values ---- */

static bool ical_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                        esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    if (len < 2 || body[0] != 'X' || body[1] != ' ') {
        snprintf(err, errlen, "not read as a calendar");
        return false;
    }
    bool en = !strcmp(c->lang, "en");
    static const char *const wd_nb[7] = {"man.", "tir.", "ons.", "tor.", "fre.", "lør.", "søn."};
    static const char *const wd_en[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char *const mo_nb[12] = {"jan", "feb", "mar", "apr", "mai", "jun",
                                          "jul", "aug", "sep", "okt", "nov", "des"};
    static const char *const mo_en[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int32_t onow = off ? off(now) : 0;
    int64_t today = fdiv((int64_t)now + onow, 86400);
    int64_t tomorrow_utc = (today + 1) * 86400 - onow;
    const char *p = body, *end = body + len;
    const char *nl = memchr(p, '\n', len);
    char buf[ESL_VALUE_MAX + 1], key[8];
    v->n = 0;
    snprintf(buf, sizeof(buf), "%.*s", nl ? (int)(nl - p - 2) : 0, p + 2);
    esl_values_set(v, "title", buf);
    p = nl ? nl + 1 : end;
    unsigned shown = 0, ntoday = 0;
    while (p < end) {
        nl = memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end;
        long long st, en_;
        int allday, used = 0;
        char line[ESL_VALUE_MAX + 64];
        snprintf(line, sizeof(line), "%.*s", (int)(e - p), p);
        p = nl ? nl + 1 : end;
        if (sscanf(line, "%lld %lld %d %n", &st, &en_, &allday, &used) < 3 || !used || en_ <= (long long)now)
            continue;
        if (st < tomorrow_utc)
            ntoday++;
        if (shown == SHOWN)
            continue;
        shown++;
        int64_t ls = st + (off ? off((uint32_t)st) : 0);
        int64_t z = fdiv(ls, 86400);
        if (st <= (long long)now || z == today) {
            snprintf(buf, sizeof(buf), "%s", en ? "today" : "i dag");
        } else if (z == today + 1) {
            snprintf(buf, sizeof(buf), "%s", en ? "tomorrow" : "i morgen");
        } else {
            int y;
            unsigned m, d;
            civil((int32_t)z, &y, &m, &d);
            int w = wday((int32_t)z);
            if (en)
                snprintf(buf, sizeof(buf), "%s %u %s", wd_en[w], d, mo_en[m - 1]);
            else
                snprintf(buf, sizeof(buf), "%s %u. %s", wd_nb[w], d, mo_nb[m - 1]);
        }
        snprintf(key, sizeof(key), "day%u", shown);
        esl_values_set(v, key, buf);
        if (allday) {
            snprintf(buf, sizeof(buf), "%s", en ? "all day" : "hele dagen");
        } else {
            /* under way: when it ends */
            int64_t at = st <= (long long)now ? en_ : st;
            int64_t l = at + (off ? off((uint32_t)at) : 0);
            int64_t sod = l - fdiv(l, 86400) * 86400;
            snprintf(buf, sizeof(buf), "%s%02d:%02d", st <= (long long)now ? (en ? "until " : "til ") : "",
                     (int)(sod / 3600), (int)(sod / 60 % 60));
        }
        snprintf(key, sizeof(key), "time%u", shown);
        esl_values_set(v, key, buf);
        snprintf(key, sizeof(key), "what%u", shown);
        esl_values_set(v, key, line + used);
    }
    /* the rest empty, so a layout's lines clear when there are fewer events */
    for (unsigned i = shown + 1; i <= SHOWN; i++) {
        static const char *const names[3] = {"day", "time", "what"};
        for (unsigned j = 0; j < 3; j++) {
            snprintf(key, sizeof(key), "%s%u", names[j], i);
            esl_values_set(v, key, "");
        }
    }
    snprintf(buf, sizeof(buf), "%u", ntoday);
    esl_values_set(v, "today", buf);
    *data_at = now;
    return true;
}

const esl_source_def_t esl_source_ical = {
    .name = "ical",
    .type = ESL_SOURCE_ICAL,
    .about = "the next events of an iCalendar (.ics) calendar",
    .keys = ical_keys,
    .fields = ical_fields,
    .cfg_keys = ical_cfg_keys,
    .every_min = 15,
    .own_pace = true, /* what is "next" follows the clock, whatever the server says */
    .align = true,    /* at each quarter, so "until 10:00" turns over when the meeting ends */
    .parse = ical_parse,
    .write = ical_write,
    .url = ical_url,
    .values = ical_values,
    .stream_new = ical_new,
    .stream_feed = ical_feed,
    .stream_end = ical_end,
};
