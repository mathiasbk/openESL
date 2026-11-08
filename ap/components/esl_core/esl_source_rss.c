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

/* ---- reading the XML: just enough for titles ---- */

/* The first "<name" in [p, end) followed by '>', a space or '/', or NULL */
static const char *find_tag(const char *p, const char *end, const char *name)
{
    size_t n = strlen(name);
    for (; p + n + 1 < end; p++) {
        if (p[0] != '<' || memcmp(p + 1, name, n) != 0)
            continue;
        char c = p[n + 1];
        if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n')
            return p;
    }
    return NULL;
}

static const char *find_str(const char *p, const char *end, const char *s)
{
    size_t n = strlen(s);
    for (; p + n <= end; p++)
        if (*p == *s && memcmp(p, s, n) == 0)
            return p;
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

/* Appends one byte to out (cap bytes with the NUL); a run of white space is one space, none
   at the start */
static void put(char *out, size_t cap, size_t *n, char c)
{
    bool sp = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (sp && (!*n || out[*n - 1] == ' '))
        return;
    if (*n + 1 < cap)
        out[(*n)++] = sp ? ' ' : c;
}

/* The text of an element from its content [p, end): CDATA as it is, entities decoded, tags
   left out (Atom's type="html"), white space made single. Into out, cap bytes with the NUL. */
static void text_of(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = 0;
    while (p < end) {
        if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
            const char *e = find_str(p + 9, end, "]]>");
            const char *q = e ? e : end;
            /* CDATA may hold HTML: its tags are left out too */
            bool in_tag = false;
            for (p += 9; p < q; p++) {
                if (*p == '<')
                    in_tag = true;
                else if (*p == '>' && in_tag)
                    in_tag = false;
                else if (!in_tag)
                    put(out, cap, &n, *p);
            }
            p = e ? e + 3 : end;
        } else if (*p == '<') {
            while (p < end && *p != '>')
                p++;
            if (p < end)
                p++;
            put(out, cap, &n, ' ');
        } else if (*p == '&') {
            const char *semi = memchr(p, ';', (size_t)(end - p) < 10 ? (size_t)(end - p) : 10);
            static const struct {
                const char *name;
                char c;
            } ents[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '}};
            bool done = false;
            if (semi && p[1] == '#') {
                unsigned long cp = 0;
                bool hex = p[2] == 'x' || p[2] == 'X';
                for (const char *d = p + (hex ? 3 : 2); d < semi; d++) {
                    int v = *d >= '0' && *d <= '9' ? *d - '0'
                            : hex && (*d | 0x20) >= 'a' && (*d | 0x20) <= 'f' ? (*d | 0x20) - 'a' + 10
                                                                            : -1;
                    if (v < 0 || cp > 0x10ffff) {
                        cp = 0;
                        break;
                    }
                    cp = cp * (hex ? 16 : 10) + (unsigned long)v;
                }
                if (cp && cp <= 0x10ffff) {
                    char u[4];
                    size_t k = utf8(cp == 0xa0 ? ' ' : cp, u);
                    for (size_t i = 0; i < k; i++)
                        put(out, cap, &n, u[i]);
                    done = true;
                }
            } else if (semi) {
                for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]) && !done; i++)
                    if ((size_t)(semi - p - 1) == strlen(ents[i].name) && !memcmp(p + 1, ents[i].name, strlen(ents[i].name))) {
                        put(out, cap, &n, ents[i].c);
                        done = true;
                    }
            }
            if (done)
                p = semi + 1;
            else
                put(out, cap, &n, *p++);
        } else {
            put(out, cap, &n, *p++);
        }
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
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

/* The text of the first <title> in [p, end), into out */
static bool title_in(const char *p, const char *end, char *out, size_t cap)
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
    text_of(gt + 1, e, out, cap);
    return true;
}

static bool rss_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                       esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    (void)c, (void)now, (void)off, (void)data_at;
    const char *end = body + len;
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
    if (title_in(body, first ? first : end, buf, sizeof(buf))) {
        fit(buf);
        esl_values_set(v, "title", buf);
    } else {
        esl_values_set(v, "title", "");
    }
    unsigned k = 0;
    for (const char *p = first; p && k < HEADS; p = find_tag(p + 1, end, it)) {
        const char *e = find_str(p, end, close);
        if (!title_in(p, e ? e : end, buf, sizeof(buf)) || !buf[0])
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
    .parse = rss_parse,
    .write = rss_write,
    .url = rss_url,
    .values = rss_values,
};
