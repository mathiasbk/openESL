#include "esl_source.h"
#include <stdio.h>
#include <string.h>

/* Data source type "rss": the newest headlines of a news feed (RSS 2.0 or Atom), as values in
 * the fields of a layout: a news ticker on a shelf label, the latest posts of a blog, a
 * status page's incidents. Only titles are read: no pictures, no links. */

#define HEADS 6 /* headlines given: {h1} to {h6} */

static const char *const rss_keys[] = {"title", "h1", "h2", "h3", "h4", "h5", "h6", "count", NULL};
static const char *const rss_fields[] = {"{title}", "{h1}", "{h2}", "{h3}", NULL};
static const char *const rss_cfg_keys[] = {"url", NULL};

static bool rss_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "url");
    int n = t >= 0 && tok[t].type == ESL_JSON_STRING
                ? esl_json_str_decode(js, &tok[t], c->rss.url, sizeof(c->rss.url) - 1)
                : -1;
    if (n >= 0)
        c->rss.url[n] = '\0';
    if (n < 0 || !esl_source_url_ok(c->rss.url)) {
        snprintf(err, errlen, "url: an http:// or https:// URL of an RSS or Atom feed, up to %d bytes, without spaces or a login",
                 ESL_SOURCE_URL_MAX - 1);
        return false;
    }
    return true;
}

static void rss_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_puts(out, ",\"url\":");
    esl_sb_json_str(out, c->rss.url);
}

static size_t rss_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s", c->rss.url);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ---- reading the XML: just enough for titles ----
 * Every scan is bounded and moves forward, so a hostile body (no closing tags, nothing but
 * "<item>") is read a few times at most, never once per item. */

static const char *find_str(const char *p, const char *end, const char *s)
{
    size_t n = strlen(s);
    for (; p + n <= end; p++)
        if (*p == *s && memcmp(p, s, n) == 0)
            return p;
    return NULL;
}

/* The first "<name" in [p, end) followed by '>', a space or '/', outside <!-- comments -->;
   or NULL */
static const char *find_tag(const char *p, const char *end, const char *name)
{
    size_t n = strlen(name);
    for (; p + n + 1 < end; p++) {
        if (p[0] != '<')
            continue;
        if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *e = find_str(p + 4, end, "-->");
            if (!e)
                return NULL;
            p = e + 2;
            continue;
        }
        if (memcmp(p + 1, name, n) != 0)
            continue;
        char c = p[n + 1];
        if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n')
            return p;
    }
    return NULL;
}

/* code point cp as UTF-8 into out (room for 4); its length */
static size_t utf8(unsigned long cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

typedef struct {
    char *out;
    size_t cap, n; /* cap with the NUL */
    bool latin1;   /* the feed is ISO-8859-1 or windows-1252: bytes from 0x80 are characters */
} txt_t;

/* Appends one byte; a run of white space is one space, none at the start */
static void put(txt_t *t, char c)
{
    bool sp = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (sp && (!t->n || t->out[t->n - 1] == ' '))
        return;
    if (t->n + 1 < t->cap)
        t->out[t->n++] = sp ? ' ' : c;
}

/* Appends a code point, leaving out control characters, surrogates and the BOM */
static void put_cp(txt_t *t, unsigned long cp)
{
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xa0)
        cp = ' ';
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || (cp >= 0xd800 && cp < 0xe000) || cp == 0xfeff || cp > 0x10ffff)
        return;
    char u[4];
    size_t k = utf8(cp, u);
    for (size_t i = 0; i < k; i++)
        put(t, u[i]);
}

/* The entity at p ("&amp;", "&#248;", "&#x2013;", "&rsquo;"): its code point and its end, or 0 */
static unsigned long entity(const char *p, const char *end, const char **after)
{
    static const struct {
        const char *name;
        unsigned short cp;
    } ents[] = {{"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
                {"nbsp", 0xa0},    {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201d}, {"ldquo", 0x201c},
                {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}};
    size_t room = (size_t)(end - p) < 10 ? (size_t)(end - p) : 10;
    const char *semi = memchr(p, ';', room);
    if (!semi)
        return 0;
    *after = semi + 1;
    if (p[1] == '#') {
        unsigned long cp = 0;
        bool hex = p[2] == 'x' || p[2] == 'X';
        const char *d = p + (hex ? 3 : 2);
        if (d >= semi)
            return 0;
        for (; d < semi; d++) {
            int v = *d >= '0' && *d <= '9'                                 ? *d - '0'
                    : hex && (*d | 0x20) >= 'a' && (*d | 0x20) <= 'f' ? (*d | 0x20) - 'a' + 10
                                                                       : -1;
            if (v < 0)
                return 0;
            cp = cp * (hex ? 16 : 10) + (unsigned long)v;
        }
        return cp ? cp : 0x7f; /* &#0; is dropped */
    }
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
        if ((size_t)(semi - p - 1) == strlen(ents[i].name) && !memcmp(p + 1, ents[i].name, strlen(ents[i].name)))
            return ents[i].cp;
    return 0;
}

/* A byte from 0x80 of a Latin-1 feed: windows-1252 has curly quotes, dashes and more in
   0x80-0x9f, where ISO-8859-1 has only control characters (dropped) */
static unsigned long latin1_cp(unsigned char b)
{
    static const unsigned short w1252[32] = {
        0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
        0x2039, 0x0152, 0,      0x017d, 0,      0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178};
    return b >= 0x80 && b < 0xa0 ? w1252[b - 0x80] : b;
}

/* Whether the '<' at p starts a tag, a comment or an end tag: "a < b" in a CDATA title is
   text, and so is a '<' with no '>' after it ("x <y" would else lose the rest). *nogt is
   where the text was found to have no '>' left (NULL until then): a '<' after it is text
   without a new search, so a title of only "<a<a<a..." is still read once. */
static bool tag_start(const char *p, const char *end, const char **nogt)
{
    if (end - p < 2)
        return false;
    char c = p[1];
    if (!(c == '/' || c == '!' || c == '?' || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')))
        return false;
    if (*nogt && p >= *nogt)
        return false;
    if (memchr(p, '>', (size_t)(end - p)))
        return true;
    *nogt = p;
    return false;
}

/* Text from [p, end): entities decoded, tags left out, CDATA read the same way (it mostly
   holds HTML), white space made single */
static void emit(txt_t *t, const char *p, const char *end, bool cdata)
{
    const char *nogt = NULL;
    while (p < end) {
        if (!cdata && end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
            const char *e = find_str(p + 9, end, "]]>");
            emit(t, p + 9, e ? e : end, true); /* once deep: CDATA does not nest */
            p = e ? e + 3 : end;
        } else if (*p == '<' && tag_start(p, end, &nogt)) {
            while (p < end && *p != '>')
                p++;
            if (p < end)
                p++;
            put(t, ' ');
        } else if (*p == '&') {
            const char *after;
            unsigned long cp = entity(p, end, &after);
            if (cp) {
                put_cp(t, cp);
                p = after;
            } else {
                put(t, *p++);
            }
        } else if ((unsigned char)*p >= 0x80 && t->latin1) {
            unsigned long cp = latin1_cp((unsigned char)*p++);
            if (cp)
                put_cp(t, cp);
        } else {
            put(t, *p++);
        }
    }
}

/* Ends the text: no space at the end, and any byte that is not part of valid UTF-8 as '?' */
static void finish(txt_t *t)
{
    while (t->n && t->out[t->n - 1] == ' ')
        t->n--;
    t->out[t->n] = '\0';
    for (size_t i = 0; i < t->n;) {
        unsigned char c = (unsigned char)t->out[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 && c >= 0xc2 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 && c <= 0xf4 ? 4 : 0;
        bool ok = k && i + k <= t->n;
        for (size_t j = 1; ok && j < k; j++)
            ok = ((unsigned char)t->out[i + j] & 0xc0) == 0x80;
        if (ok) {
            i += k;
        } else {
            t->out[i++] = '?';
        }
    }
}

/* A headline cut to fit a value: at a character's start, with "..." */
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

/* The text of the first <title> in [p, end), into out (cap bytes with the NUL). Atom's
   type="html" is HTML inside the XML: its entities and tags are read a second time. */
static bool title_in(const char *p, const char *end, char *out, size_t cap, bool latin1)
{
    const char *t = find_tag(p, end, "title");
    if (!t)
        return false;
    const char *gt = memchr(t, '>', (size_t)(end - t));
    if (!gt || gt[-1] == '/')
        return false;
    const char *e = find_str(gt + 1, end, "</title>");
    if (!e)
        return false;
    txt_t x = {out, cap, 0, latin1};
    emit(&x, gt + 1, e, false);
    finish(&x);
    if (find_str(t, gt, "type=\"html\"") || find_str(t, gt, "type='html'")) {
        char again[ESL_VALUE_MAX * 4 + 1];
        txt_t y = {again, sizeof(again), 0, false};
        emit(&y, out, out + x.n, true);
        finish(&y);
        snprintf(out, cap, "%s", again);
    }
    return true;
}

static bool rss_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                       esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    (void)c, (void)now, (void)off, (void)data_at;
    const char *end = body + len;
    /* <?xml ... encoding="ISO-8859-1"?> (or windows-1252): bytes from 0x80 are Latin-1 */
    const char *decl_end = len >= 5 && !memcmp(body, "<?xml", 5) ? find_str(body, len > 200 ? body + 200 : end, "?>") : NULL;
    bool latin1 = false;
    if (decl_end) {
        char d[201];
        size_t dn = (size_t)(decl_end - body);
        for (size_t i = 0; i < dn; i++)
            d[i] = (char)(body[i] | ((body[i] >= 'A' && body[i] <= 'Z') ? 0x20 : 0));
        latin1 = find_str(d, d + dn, "iso-8859-1") || find_str(d, d + dn, "windows-1252") || find_str(d, d + dn, "latin1");
    }
    const char *it = "item", *close = "</item>";
    const char *first = find_tag(body, end, "item");
    if (!first) {
        const char *entry = find_tag(body, end, "entry");
        if (entry || find_tag(body, end, "feed")) {
            it = "entry";
            close = "</entry>";
            first = entry;
        } else if (!find_tag(body, end, "rss") && !find_tag(body, end, "channel") && !find_tag(body, end, "rdf:RDF")) {
            snprintf(err, errlen, "the answer is not an RSS or Atom feed");
            return false;
        }
    }
    char buf[ESL_VALUE_MAX * 4 + 1];
    v->n = 0;
    /* the feed's own title: the first <title> before the first item */
    if (title_in(body, first ? first : end, buf, sizeof(buf), latin1)) {
        fit(buf);
        esl_values_set(v, "title", buf);
    } else {
        esl_values_set(v, "title", "");
    }
    unsigned k = 0;
    /* each item is read only up to the next one's start */
    for (const char *p = first, *next; p && k < HEADS; p = next) {
        next = find_tag(p + 1, end, it);
        const char *lim = next ? next : end, *e = find_str(p, lim, close);
        if (!title_in(p, e ? e : lim, buf, sizeof(buf), latin1) || !buf[0])
            continue;
        fit(buf);
        char key[4] = {'h', (char)('1' + k), '\0'};
        esl_values_set(v, key, buf);
        k++;
    }
    for (unsigned i = k; i < HEADS; i++) {
        char key[4] = {'h', (char)('1' + i), '\0'};
        esl_values_set(v, key, "");
    }
    snprintf(buf, sizeof(buf), "%u", k);
    esl_values_set(v, "count", buf);
    return true;
}

const esl_source_def_t esl_source_rss = {
    .name = "rss",
    .type = ESL_SOURCE_RSS,
    .about = "the newest headlines of an RSS or Atom feed",
    .keys = rss_keys,
    .fields = rss_fields,
    .cfg_keys = rss_cfg_keys,
    .every_min = 15,
    .prefix = true, /* only the first items matter: a long feed is read up to the size limit */
    .parse = rss_parse,
    .write = rss_write,
    .url = rss_url,
    .values = rss_values,
};
