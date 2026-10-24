#include "esl_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_api.h"

const char *esl_wifi_mode_name(esl_wifi_mode_t m)
{
    switch (m) {
    case ESL_WIFI_STA:       return "sta";
    case ESL_WIFI_SETUP:     return "setup";
    case ESL_WIFI_STA_SETUP: return "sta+setup";
    default:                 return "off";
    }
}

const char *esl_wifi_check(const char *ssid, size_t ssid_len, const char *pass, size_t pass_len)
{
    if (ssid_len > ESL_WIFI_SSID_MAX)
        return "ssid is longer than 32 bytes";
    if (memchr(ssid, 0, ssid_len))
        return "ssid must not contain a NUL byte";
    if (pass_len && (pass_len < ESL_WIFI_PASS_MIN || pass_len > ESL_WIFI_PASS_MAX))
        return "password must be empty (open network) or 8 to 63 characters";
    for (size_t i = 0; i < pass_len; i++)
        if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] > 0x7E)
            return "password must be printable ASCII";
    return NULL;
}

const char *esl_wifi_check_token(const char *token, size_t len)
{
    if (len < ESL_WIFI_TOKEN_MIN || len > ESL_WIFI_TOKEN_MAX)
        return "token must be 8 to 128 characters";
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)token[i] <= 0x20 || (unsigned char)token[i] > 0x7E)
            return "token must be printable ASCII without spaces";
    return NULL;
}

const char *esl_wifi_check_hostname(const char *name, size_t len)
{
    if (len < 1 || len > ESL_WIFI_HOSTNAME_MAX)
        return "hostname must be 1 to 32 characters";
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok)
            return "hostname may only hold letters, digits and '-'";
    }
    if (name[0] == '-' || name[len - 1] == '-')
        return "hostname must not start or end with '-'";
    return NULL;
}

/* ---- form parsing ---------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int esl_form_get(const char *body, size_t len, const char *key, char *out, size_t out_max)
{
    size_t kl = strlen(key), i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && body[end] != '&')
            end++;
        if (end - i > kl && memcmp(&body[i], key, kl) == 0 && body[i + kl] == '=') {
            size_t n = 0;
            for (size_t p = i + kl + 1; p < end; p++) {
                char c = body[p];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    int hi = p + 2 < end ? hexval(body[p + 1]) : -1;
                    int lo = hi >= 0 ? hexval(body[p + 2]) : -1;
                    if (lo < 0)
                        return -3;
                    c = (char)(hi * 16 + lo);
                    p += 2;
                }
                if (c == 0)
                    return -3;
                if (n + 1 >= out_max)
                    return -2;
                out[n++] = c;
            }
            out[n] = 0;
            return (int)n;
        }
        i = end + 1;
    }
    return -1;
}

/* ---- output ----------------------------------------------------------------- */

static void html_esc(esl_sb_t *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '<':  esl_sb_puts(out, "&lt;"); break;
        case '>':  esl_sb_puts(out, "&gt;"); break;
        case '&':  esl_sb_puts(out, "&amp;"); break;
        case '"':  esl_sb_puts(out, "&quot;"); break;
        case '\'': esl_sb_puts(out, "&#39;"); break;
        default:   esl_sb_putc(out, *s);
        }
    }
}

void esl_wifi_write_json(esl_sb_t *out, const esl_wifi_status_t *st)
{
    esl_sb_puts(out, "{\"mode\":");
    esl_sb_json_str(out, esl_wifi_mode_name(st->mode));
    esl_sb_puts(out, ",\"ssid\":");
    if (st->ssid[0])
        esl_sb_json_str(out, st->ssid);
    else
        esl_sb_puts(out, "null");
    esl_sb_printf(out, ",\"connected\":%s", st->connected ? "true" : "false");
    if (st->connected) {
        esl_sb_puts(out, ",\"ip\":");
        esl_sb_json_str(out, st->ip);
        esl_sb_printf(out, ",\"rssi\":%d", (int)st->rssi);
    }
    esl_sb_puts(out, ",\"setup_ssid\":");
    if (st->setup_ssid[0])
        esl_sb_json_str(out, st->setup_ssid);
    else
        esl_sb_puts(out, "null");
    esl_sb_printf(out, ",\"token_set\":%s", st->token_set ? "true" : "false");
    if (st->hostname[0]) {
        esl_sb_puts(out, ",\"hostname\":");
        esl_sb_json_str(out, st->hostname);
    }
    esl_sb_puts(out, ",\"networks\":[");
    for (unsigned i = 0; i < st->scan_count && i < ESL_WIFI_SCAN_MAX; i++) {
        if (i)
            esl_sb_putc(out, ',');
        esl_sb_puts(out, "{\"ssid\":");
        esl_sb_json_str(out, st->scan[i]);
        esl_sb_printf(out, ",\"rssi\":%d}", (int)st->scan_rssi[i]);
    }
    esl_sb_puts(out, "]}");
}

static void page_head(esl_sb_t *out)
{
    esl_sb_puts(out,
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<title>OpenESL AP setup</title><style>"
                "body{font-family:sans-serif;margin:16px;max-width:28em}"
                "label{display:block;margin-top:12px}input{width:100%;padding:6px;box-sizing:border-box}"
                "button{margin-top:16px;padding:8px 16px}.m{background:#eef;padding:8px}"
                ".e{background:#fee;padding:8px}small{color:#555}"
                "select{width:100%;padding:6px;margin-top:6px}"
                "</style></head><body><h1>OpenESL AP setup</h1>");
}

void esl_wifi_page(esl_sb_t *out, const esl_wifi_status_t *st, bool token_set, const char *fw, const char *msg)
{
    page_head(out);
    if (msg)
        esl_sb_printf(out, "<p class=\"m\">%s</p>", msg);
    esl_sb_puts(out, "<p>Firmware ");
    html_esc(out, fw ? fw : "");
    esl_sb_puts(out, ". WiFi: ");
    if (st->connected) {
        esl_sb_puts(out, "connected to <b>");
        html_esc(out, st->ssid);
        esl_sb_printf(out, "</b>, IP %s, %d dBm.", st->ip, (int)st->rssi);
    } else if (st->ssid[0]) {
        esl_sb_puts(out, "trying to join <b>");
        html_esc(out, st->ssid);
        esl_sb_puts(out, "</b>, no answer yet.");
    } else {
        esl_sb_puts(out, "no network saved.");
    }
    esl_sb_puts(out, "</p><form method=\"post\" action=\"/setup\">"
                     "<label>Network (SSID)<input name=\"ssid\" maxlength=\"32\" required value=\"");
    html_esc(out, st->ssid);
    esl_sb_puts(out, "\"></label>");
    /* The networks found, strongest first, in a menu that fills in the name (a
       datalist only suggests while typing, and phones and the captive portal's
       own window often never show it). The name is in the page once, so a full
       list of long names still fits ESL_WIFI_PAGE_SIZE. */
    if (st->scan_count) {
        esl_sb_printf(out, "<select class=\"nets\" aria-label=\"Networks found\"><option value=\"\">"
                           "Pick one of the %u networks found (strongest first)</option>", st->scan_count);
        for (unsigned i = 0; i < st->scan_count && i < ESL_WIFI_SCAN_MAX; i++) {
            esl_sb_puts(out, "<option>");
            html_esc(out, st->scan[i]);
            esl_sb_puts(out, "</option>");
        }
        /* The option's text, not its value: without a value attribute that is the text
           with spaces trimmed and collapsed, and a network name may have them */
        esl_sb_puts(out, "</select><script>var n=document.querySelector(\".nets\");n.onchange=function(){"
                         "if(n.selectedIndex>0){n.form.ssid.value=n.options[n.selectedIndex].textContent;"
                         "n.form.pass.focus()}}</script>");
    }
    esl_sb_puts(out, ""
                     "<label>Password <small>(empty for an open network)</small>"
                     "<input name=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\"></label>");
    if (token_set)
        esl_sb_puts(out, "<label>Current API token <small>(required to change the settings)</small>"
                         "<input name=\"token\" type=\"password\" required autocomplete=\"off\"></label>"
                         "<label>New API token <small>(empty keeps the current one)</small>");
    else
        esl_sb_puts(out, "<label>API token <small>(no token is set, so the HTTP API answers 401 until you choose "
                         "one; 8 characters or more)</small>");
    esl_sb_puts(out, "<input name=\"new_token\" type=\"password\" maxlength=\"128\" autocomplete=\"off\"></label>"
                     "<button type=\"submit\">Save and connect</button></form>"
                     "<p><a href=\"/setup?scan=1\">Search for networks again</a> "
                     "<small>(reload the page after a few seconds)</small></p>");
    if (st->setup_ssid[0]) {
        esl_sb_puts(out, "<p><small>This page is served on the open setup network <b>");
        html_esc(out, st->setup_ssid);
        esl_sb_printf(out, "</b> at http://%s/. It turns off once the AP has joined your network.</small></p>",
                      st->setup_ip[0] ? st->setup_ip : "192.168.4.1");
    }
    esl_sb_puts(out, "</body></html>");
}

static bool token_eq(const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    unsigned diff = (unsigned)(al ^ bl);
    for (size_t i = 0; i < bl; i++)
        diff |= (unsigned char)b[i] ^ (unsigned char)a[al ? i % al : 0];
    return al && diff == 0;
}

static int fail(esl_sb_t *out, int status, const char *msg)
{
    page_head(out);
    esl_sb_puts(out, "<p class=\"e\">");
    html_esc(out, msg);
    esl_sb_puts(out, "</p><p><a href=\"/setup\">Back</a></p></body></html>");
    return status;
}

int esl_wifi_setup_post(const esl_wifi_ops_t *ops, const char *token, const char *fw, const char *body, size_t len,
                        struct esl_api_lock *lock, uint64_t now_ms, esl_sb_t *out)
{
    (void)fw;
    char ssid[ESL_WIFI_SSID_MAX + 1], pass[ESL_WIFI_PASS_MAX + 1];
    char given[ESL_WIFI_TOKEN_MAX + 1], new_token[ESL_WIFI_TOKEN_MAX + 1];
    bool token_set = token && token[0];

    if (token_set) {
        unsigned wait_s = esl_api_lock_wait(lock, now_ms);
        if (wait_s) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Too many wrong API tokens. Try again in %u s.", wait_s);
            return fail(out, 429, msg);
        }
        int gl = esl_form_get(body, len, "token", given, sizeof(given));
        bool ok = gl >= 0 && token_eq(token, given);
        esl_api_lock_note(lock, ok, now_ms);
        if (!ok)
            return fail(out, 401, "Wrong API token.");
    }

    int sl = esl_form_get(body, len, "ssid", ssid, sizeof(ssid));
    if (sl <= 0)
        return fail(out, 422, sl == -2 ? "The network name is longer than 32 bytes." : "The network name is missing.");
    int pl = esl_form_get(body, len, "pass", pass, sizeof(pass));
    if (pl == -1) {
        pl = 0;
        pass[0] = 0;
    }
    if (pl < 0)
        return fail(out, 422, "The password is longer than 63 characters.");
    const char *err = esl_wifi_check(ssid, (size_t)sl, pass, (size_t)pl);
    if (err)
        return fail(out, 422, err);

    int nl = esl_form_get(body, len, "new_token", new_token, sizeof(new_token));
    if (nl == -2 || (nl > 0 && nl < ESL_WIFI_TOKEN_MIN))
        return fail(out, 422, "The API token must be 8 to 128 characters.");
    if (nl > 0) {
        if (esl_wifi_check_token(new_token, (size_t)nl))
            return fail(out, 422, "The API token must be printable ASCII without spaces.");
        if (!ops->set_token)
            return fail(out, 422, "This AP cannot change its token here.");
        if (ops->set_token(ops->ctx, new_token) != 0)
            return fail(out, 500, "Could not save the token.");
    }

    if (ops->set(ops->ctx, ssid, pass) != 0)
        return fail(out, 500, "Could not save the network.");

    page_head(out);
    esl_sb_puts(out, "<p class=\"m\">Saved. The AP now joins <b>");
    html_esc(out, ssid);
    esl_sb_puts(out, "</b>");
    if (nl > 0)
        esl_sb_puts(out, " and uses the new API token");
    /* the name it has now: openesl-xxxx on a new AP, or what was set before */
    esl_wifi_status_t *st = malloc(sizeof(*st));
    if (st) {
        memset(st, 0, sizeof(*st));
        ops->status(ops->ctx, st);
    }
    esl_sb_puts(out, ".</p><p>Find it afterwards as http://");
    html_esc(out, st && st->hostname[0] ? st->hostname : "openesl");
    free(st);
    esl_sb_puts(out, ".local/ or by the address your router gives it. "
                     "If it cannot join, the setup network comes back after a while.</p>"
                     "<p><a href=\"/setup\">Status</a></p></body></html>");
    return 200;
}
