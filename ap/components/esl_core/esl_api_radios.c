/* /api/radios: the radio coprocessors (docs/ap-netradio.md, "Several radios"). */
#include "esl_api_int.h"

/* The radios the AP has: links[] (radio_net.c) or the one link (radio_uart.c); NULL, 0: none */
const esl_radio_link_t *api_radio_links(const esl_api_t *api, unsigned *n)
{
    if (api->links && api->n_links) {
        *n = api->n_links > 32 ? 32 : api->n_links;
        return api->links;
    }
    *n = api->link ? 1 : 0;
    return api->link;
}

/* Seconds since the radio lost contact (since boot when it never had any) */
uint64_t api_radio_lost_s(const esl_radio_link_t *l, uint64_t now_ms)
{
    /* written by the radio's task without the API's lock: may be just ahead of now_ms */
    uint64_t down = l->down_ms;
    return (down < now_ms ? now_ms - down : 0) / 1000;
}

/* "via": how the AP reaches the radio */
const char *api_radio_via(const esl_radio_link_t *l)
{
    return l->local ? "local" : l->uart ? "uart" : "wifi";
}

bool api_radio_is_lost(const esl_radio_link_t *l, unsigned i, uint64_t now_ms)
{
    if (l->up || (i && !l->id))
        return false;
    return api_radio_lost_s(l, now_ms) >= ESL_RADIO_LOST_S;
}

uint32_t esl_api_radios_lost(const esl_api_t *api, uint64_t now_ms)
{
    unsigned n;
    const esl_radio_link_t *l = api_radio_links(api, &n);
    uint32_t lost = 0;
    for (unsigned i = 0; i < n; i++)
        if (api_radio_is_lost(&l[i], i, now_ms))
            lost |= 1u << i;
    return lost;
}

/* The fields of a radio's link after "connected": where it is and when it was last heard, or
   since when it is gone */
void api_write_link_state(esl_sb_t *out, const esl_radio_link_t *l, unsigned i, uint64_t now_ms)
{
    esl_sb_printf(out, ",\"via\":\"%s\",\"lost\":%s", api_radio_via(l),
                  api_radio_is_lost(l, i, now_ms) ? "true" : "false");
    if (!l->up) {
        esl_sb_printf(out, ",\"lost_s\":%" PRIu64, api_radio_lost_s(l, now_ms));
        return;
    }
    /* the radio's task writes seen_ms without the API's lock: a read just after
       a write can see it ahead of now_ms */
    uint64_t ago = l->seen_ms < now_ms ? now_ms - l->seen_ms : 0;
    if (l->ip)
        esl_sb_printf(out, ",\"ip\":\"%u.%u.%u.%u\"", (unsigned)(l->ip >> 24), (unsigned)((l->ip >> 16) & 0xff),
                      (unsigned)((l->ip >> 8) & 0xff), (unsigned)(l->ip & 0xff));
    esl_sb_printf(out, ",\"seen_s\":%" PRIu64 ",\"fw\":%u", ago / 1000, (unsigned)l->ver);
}

/* "[{radio, id, channel, tags, connected, ...}]": every network radio the AP knows, the one
   on its channel (0) and those with their own; channel is the AP's for radio 0 */
void api_write_radios(const esl_api_t *api, esl_sb_t *out, uint8_t ap_channel, uint64_t now_ms)
{
    esl_sb_putc(out, '[');
    bool first = true;
    for (unsigned i = 0; i < api->n_links; i++) {
        const esl_radio_link_t *l = &api->links[i];
        if (!l->id)
            continue;
        char id[17];
        esl_mac_format(l->id, id);
        esl_sb_printf(out, "%s{\"radio\":%u,\"id\":\"%s\",\"channel\":%u,\"tags\":%u,\"connected\":%s",
                      first ? "" : ",", i, id, (unsigned)(l->channel ? l->channel : ap_channel), l->tags,
                      l->up ? "true" : "false");
        first = false;
        api_write_link_state(out, l, i, now_ms);
        esl_sb_putc(out, '}');
    }
    esl_sb_putc(out, ']');
}

/* GET /api/radios: the network radios; DELETE /api/radios/{id}: forget one that is not connected */
static int handle_radios(const esl_api_t *api, const char *method, const char *uri, size_t plen, uint64_t now_ms,
                         esl_sb_t *out)
{
    const size_t base = sizeof(PATH_RADIOS) - 1;
    size_t il = plen > base ? plen - base - 1 : 0;
    if (il && uri[base + il] == '/')
        il--;
    if (!il) {
        if (strcmp(method, "GET") != 0)
            return api_error(out, 405, "method not allowed");
        esl_channel_state_t ch;
        esl_store_channel_state(api->store, now_ms, &ch);
        esl_sb_puts(out, "{\"radios\":");
        api_write_radios(api, out, ch.channel, now_ms);
        esl_sb_putc(out, '}');
        return 200;
    }
    uint64_t id;
    if (il != 16 || !esl_mac_parse(&uri[base + 1], 16, &id) || !id)
        return api_error(out, 400, "the radio id must be 16 hex digits");
    if (strcmp(method, "DELETE") != 0 || !api->radio_ops)
        return api_error(out, 405, "method not allowed");
    int r = api->radio_ops->forget(api->radio_ops->ctx, id);
    if (r == 404)
        return api_error(out, 404, "unknown radio");
    if (r == 409)
        return api_error(out, 409, "the radio is connected: turn it off first, then forget it");
    char m[17];
    esl_mac_format(id, m);
    esl_sb_printf(out, "{\"forgotten\":\"%s\"}", m);
    return 200;
}

/* More than one network radio is known: each tag says which one serves it */
bool api_several_radios(const esl_api_t *api)
{
    unsigned n = 0;
    for (unsigned i = 0; api->links && i < api->n_links; i++)
        n += api->links[i].id != 0;
    return n > 1;
}

int api_route_radios(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return handle_radios(api, rq->method, rq->uri, rq->plen, rq->now_ms, out);
}
