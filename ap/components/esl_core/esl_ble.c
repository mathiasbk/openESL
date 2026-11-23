#include "esl_ble.h"

esl_push_res_t esl_ble_send(void *ctx, uint64_t mac, uint8_t model, const uint8_t *img, size_t len,
                            esl_push_info_t *info)
{
    esl_ble_t *b = ctx;
    if (!b || !b->fb || b->fb_cap < ESL_GICISKY_FB_SIZE || !img ||
        (len != ESL_GICISKY_PLANES_SIZE && len != ESL_GICISKY_PLANES3_SIZE) ||
        !b->connect || !b->disconnect || !b->write || !b->wait)
        return ESL_PUSH_AWAY;
    if (b->model && model != b->model)
        return ESL_PUSH_REFUSED;
    if (!b->connect(b->ctx, mac))
        return ESL_PUSH_AWAY;

    esl_push_res_t res = ESL_PUSH_AWAY;
    if (esl_gicisky_pack_planes(img, len, b->fb)) {
        const esl_gicisky_io_t io = {b->ctx, b->write, b->wait};
        esl_gicisky_res_t r = esl_gicisky_send(&io, b->fb, ESL_GICISKY_FB_SIZE);
        if (r == ESL_GICISKY_SENT)
            res = ESL_PUSH_SENT;
        else if (r == ESL_GICISKY_REFUSED)
            res = ESL_PUSH_REFUSED;
    }
    if (b->info && info)
        b->info(b->ctx, info);
    b->disconnect(b->ctx);
    return res;
}
