/* /api/wifi: the WiFi the AP joins. */
#include "esl_api_int.h"

/* GET/PUT /api/wifi: the network the AP joins (krav A1). The password is never
 * shown. ssid "" or null forgets the network, and the AP opens its setup
 * network again. "token" sets a new API token (the page sets both right after
 * flashing, over the USB port); "hostname" is the AP's name on the network
 * (DHCP and mDNS). With a token or a hostname, ssid may be left out to keep
 * the network. Everything is checked before anything is saved. */
static int ap_wifi(const esl_api_t *api, const char *method, const char *body, size_t len, esl_sb_t *out)
{
    esl_wifi_status_t ws;
    if (strcmp(method, "PUT") == 0) {
        esl_jtok_t *tok = NULL;
        int status = api_parse_json(body, len, &tok, out);
        if (status == 0) {
            char ssid[ESL_WIFI_SSID_MAX + 1] = "", pass[ESL_WIFI_PASS_MAX + 1] = "";
            char token[ESL_WIFI_TOKEN_MAX + 1] = "", host[ESL_WIFI_HOSTNAME_MAX + 1] = "";
            int s = esl_json_obj_get(body, tok, 0, "ssid");
            int h = esl_json_obj_get(body, tok, 0, "hostname");
            int p = esl_json_obj_get(body, tok, 0, "password");
            int t = esl_json_obj_get(body, tok, 0, "token");
            int sl = 0, pl = 0, tl = 0, hl = 0;
            const char *err = NULL;
            if (t >= 0 && tok[t].type == ESL_JSON_STRING)
                tl = esl_json_str_decode(body, &tok[t], token, sizeof(token) - 1);
            else if (t >= 0 && tok[t].type != ESL_JSON_NULL)
                err = "token must be a string";
            if (!err && h >= 0 && tok[h].type == ESL_JSON_STRING)
                hl = esl_json_str_decode(body, &tok[h], host, sizeof(host) - 1);
            else if (!err && h >= 0 && tok[h].type != ESL_JSON_NULL)
                err = "hostname must be a string";
            if (!err && s < 0 && t < 0 && h < 0)
                err = "ssid is required (null or \"\" forgets the saved network)";
            else if (!err && s >= 0 && tok[s].type == ESL_JSON_STRING)
                sl = esl_json_str_decode(body, &tok[s], ssid, sizeof(ssid) - 1);
            else if (!err && s >= 0 && tok[s].type != ESL_JSON_NULL)
                err = "ssid must be a string or null";
            if (!err && p >= 0 && tok[p].type == ESL_JSON_STRING)
                pl = esl_json_str_decode(body, &tok[p], pass, sizeof(pass) - 1);
            else if (!err && p >= 0 && tok[p].type != ESL_JSON_NULL)
                err = "password must be a string";
            if (!err && sl == -2)
                err = "ssid is longer than 32 bytes";
            else if (!err && pl == -2)
                err = "password must be empty (open network) or 8 to 63 characters";
            else if (!err && tl == -2)
                err = "token must be 8 to 128 characters";
            else if (!err && hl == -2)
                err = "hostname must be 1 to 32 characters";
            else if (!err && (sl < 0 || pl < 0 || tl < 0 || hl < 0))
                err = "bad escape in ssid, password, token or hostname";
            if (!err) {
                ssid[sl] = 0;
                pass[pl] = 0;
                token[tl] = 0;
                host[hl] = 0;
                err = esl_wifi_check(ssid, (size_t)sl, pass, (size_t)pl);
            }
            if (!err && h >= 0 && tok[h].type == ESL_JSON_STRING)
                err = esl_wifi_check_hostname(host, (size_t)hl);
            if (!err && hl && !api->wifi->set_hostname)
                err = "this AP cannot change its hostname";
            if (!err && tl)
                err = esl_wifi_check_token(token, (size_t)tl);
            if (!err && tl && !api->wifi->set_token)
                err = "this AP cannot change its token here";
            if (err)
                status = api_error(out, 422, err);
            else if (tl && api->wifi->set_token(api->wifi->ctx, token) != 0)
                status = api_error(out, 500, "could not save the token");
            else if (hl && api->wifi->set_hostname(api->wifi->ctx, host) != 0)
                status = api_error(out, 500, "could not save the hostname");
            else if (s >= 0 && api->wifi->set(api->wifi->ctx, ssid, pass) != 0)
                status = api_error(out, 500, "could not save the network");
        }
        free(tok);
        if (status)
            return status;
    } else if (strcmp(method, "GET") != 0) {
        return api_error(out, 405, "method not allowed");
    }
    api->wifi->status(api->wifi->ctx, &ws);
    esl_wifi_write_json(out, &ws);
    return 200;
}

int api_route_wifi(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return ap_wifi(api, rq->method, rq->body, rq->body_len, out);
}
