#include "esl_remote.h"
#include <inttypes.h>
#include <string.h>

/* See include/esl_remote.h and docs/solum-remote.md */

/* FCF 0x8841: data, no ACK, PAN id compression, short addresses */
static const uint8_t HEAD[] = {0x41, 0x88};
/* after seq: PAN 0xFFFF, to 0xFFFF, from 0xBEEF, then the payload's first byte */
static const uint8_t ADDR[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0xBE, 0x19};
#define FRAME_LEN 12 /* without the FCS */

bool esl_remote_parse(const uint8_t *psdu, size_t len, uint8_t *button, uint8_t *seq)
{
    /* the frame without (12) or with (14) its FCS */
    if ((len != FRAME_LEN && len != FRAME_LEN + 2) || memcmp(psdu, HEAD, sizeof(HEAD)) != 0 ||
        memcmp(&psdu[3], ADDR, sizeof(ADDR)) != 0 || psdu[11] != 0x00)
        return false;
    if (psdu[10] < ESL_REMOTE_W || psdu[10] > ESL_REMOTE_RIGHT)
        return false;
    *button = psdu[10];
    *seq = psdu[2];
    return true;
}

static void lock(esl_remote_t *r, bool take)
{
    if (r->lock)
        r->lock(r, take);
}

bool esl_remote_frame(esl_remote_t *r, const uint8_t *psdu, size_t len, int8_t rssi, uint64_t now_ms,
                      esl_remote_press_t *press)
{
    uint8_t button, seq;
    if (!esl_remote_parse(psdu, len, &button, &seq))
        return false;
    lock(r, true);
    r->frames++;
    bool same = r->have_seq && seq == r->last_seq && now_ms - r->last_ms < ESL_REMOTE_SAME_MS;
    r->have_seq = true;
    r->last_seq = seq;
    r->last_ms = now_ms;
    if (same) {
        lock(r, false);
        return false;
    }
    esl_remote_press_t p = {++r->next_id, button, rssi, now_ms};
    if (r->n == ESL_REMOTE_LOG) {
        memmove(&r->log[0], &r->log[1], (ESL_REMOTE_LOG - 1) * sizeof(r->log[0]));
        r->n--;
    }
    r->log[r->n++] = p;
    lock(r, false);
    if (press)
        *press = p;
    return true;
}

const char *esl_remote_button_name(uint8_t button)
{
    switch (button) {
    case ESL_REMOTE_W:     return "w";
    case ESL_REMOTE_LEFT:  return "left";
    case ESL_REMOTE_RIGHT: return "right";
    default:               return NULL;
    }
}

void esl_remote_press_json(const esl_remote_t *r, const esl_remote_press_t *p, esl_sb_t *out)
{
    const char *name = esl_remote_button_name(p->button);
    esl_sb_printf(out, "{\"event\":\"remote\",\"button\":\"%s\",\"id\":%" PRIu32 ",\"boot\":%" PRIu32 ",\"rssi\":%d}",
                  name ? name : "?", p->id, r->boot, p->rssi);
}
