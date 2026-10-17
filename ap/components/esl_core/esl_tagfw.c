#include "esl_tagfw.h"
#include "esl_crc32.h"
#include <string.h>

static void lock(esl_tagfw_t *fw)
{
    if (fw->ops.lock)
        fw->ops.lock(fw->ops.ctx);
}

static void unlock(esl_tagfw_t *fw)
{
    if (fw->ops.unlock)
        fw->ops.unlock(fw->ops.ctx);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool model_ok(uint8_t model)
{
    return model > ESL_MODEL_UNKNOWN && model < ESL_MODEL_COUNT;
}

/* The header of an OTA file: false unless it is one we can offer. */
static bool parse_header(const uint8_t *h, uint8_t *model, esl_tagfw_img_t *img, const char **err)
{
    uint32_t size = get32(h + ESL_FWF_OFF_SIZE);
    *model = h[ESL_FWF_OFF_MODEL];
    img->ver = (uint16_t)(h[ESL_FWF_OFF_VER] | h[ESL_FWF_OFF_VER + 1] << 8);
    img->size = (uint16_t)size;
    img->crc = get32(h + ESL_FWF_OFF_CRC);
    if (get32(h + ESL_FWF_OFF_MAGIC) != ESL_FWF_MAGIC)
        *err = "not a tag firmware file (openesl-ota.bin from the release)";
    else if (h[ESL_FWF_OFF_FORMAT] != ESL_FWF_FORMAT)
        *err = "unknown tag firmware file format";
    else if (!model_ok(*model))
        *err = "the file is for an unknown tag model";
    else if (size == 0 || size > ESL_FW_MAX || (size & 0x3FF) || img->ver == 0)
        *err = "the image in the file has a bad size or version";
    else
        return true;
    return false;
}

/* CRC-32 of the image in the store */
static bool check_slot(esl_tagfw_t *fw, uint8_t model, const esl_tagfw_img_t *img)
{
    uint8_t buf[256];
    uint32_t crc = 0;
    for (uint32_t pos = 0; pos < img->size; pos += sizeof(buf)) {
        size_t n = img->size - pos < sizeof(buf) ? img->size - pos : sizeof(buf);
        if (!fw->ops.read(fw->ops.ctx, model, ESL_FWF_HDR_LEN + pos, buf, n))
            return false;
        crc = esl_crc32_update(crc, buf, n);
    }
    return crc == img->crc;
}

/* Keeps who an image is for. Outside the lock: NVS writes are slow. */
static void save_to(esl_tagfw_t *fw, uint8_t model, const esl_tagfw_to_t *to)
{
    if (fw->ops.save_to)
        fw->ops.save_to(fw->ops.ctx, model, to, to ? sizeof(*to) : 0);
}

void esl_tagfw_init(esl_tagfw_t *fw, const esl_tagfw_ops_t *ops)
{
    memset(fw, 0, sizeof(*fw));
    fw->ops = *ops;
    lock(fw);
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        uint8_t h[ESL_FWF_HDR_LEN], model;
        esl_tagfw_img_t img;
        const char *err;
        if (fw->ops.read(fw->ops.ctx, m, 0, h, sizeof(h)) && parse_header(h, &model, &img, &err) &&
            model == m && check_slot(fw, m, &img)) {
            fw->img[m - 1] = img;
            /* An image without a kept list (from before the list existed) is for all */
            esl_tagfw_to_t to;
            if (!fw->ops.load_to || !fw->ops.load_to(fw->ops.ctx, m, &to, sizeof(to)) || to.ver != img.ver ||
                to.n > ESL_TAGFW_CHOSEN || (!to.all && !to.n))
                to = (esl_tagfw_to_t){.ver = img.ver, .all = 1};
            fw->to[m - 1] = to;
        }
    }
    unlock(fw);
}

static void ver_str(esl_sb_t *out, uint16_t v)
{
    esl_sb_printf(out, "\"%u.%u.%u\"", v / 10000u, v / 100u % 100u, v % 100u);
}

void esl_tagfw_write_fields(esl_sb_t *out, uint8_t model, const esl_tagfw_img_t *img)
{
    esl_sb_puts(out, "\"model\":");
    esl_sb_json_str(out, esl_model_name(model));
    esl_sb_puts(out, ",\"version\":");
    ver_str(out, img->ver);
    esl_sb_printf(out, ",\"size\":%u,\"crc\":\"%08X\"", (unsigned)img->size, (unsigned)img->crc);
}

static int error(esl_sb_t *out, int status, const char *msg)
{
    esl_sb_puts(out, "{\"error\":");
    esl_sb_json_str(out, msg);
    esl_sb_putc(out, '}');
    return status;
}

/* The store is erased, written and checked outside the lock: the radio task takes
   it for every POLL, and a slot being uploaded is not offered (its version is 0),
   so the radio never reads it meanwhile. One upload at a time. */
int esl_tagfw_part(esl_tagfw_t *fw, size_t offset, size_t total, const uint8_t *data, size_t len, uint64_t only,
                   esl_sb_t *out)
{
    const char *err = NULL;
    int status = 409;
    uint8_t model = 0;
    esl_tagfw_img_t img = {0};

    if (offset == 0) {
        status = 400;
        if (len < ESL_FWF_HDR_LEN)
            err = "the first part must hold the 16-byte file header";
        else if (!parse_header(data, &model, &img, &err))
            status = 422;
        else if (total != ESL_FWF_HDR_LEN + (size_t)img.size)
            err = "total does not match the size in the file header";
        lock(fw);
        fw->up_model = 0;
        if (!err)
            fw->img[model - 1].ver = 0; /* no longer offered */
        unlock(fw);
        if (err)
            return error(out, status, err);
        if (!fw->ops.erase(fw->ops.ctx, model))
            return error(out, 500, "could not erase the firmware store");
        lock(fw);
        fw->up_model = model;
        fw->up_got = 0;
        fw->up_total = (uint32_t)total;
        fw->up_only = only;
        unlock(fw);
    }

    lock(fw);
    uint32_t at = fw->up_got;
    model = fw->up_model;
    if (!model) {
        err = "no upload in progress: send offset 0 first";
    } else if (offset != at || total != fw->up_total) {
        err = "not the next part of the upload in progress";
    } else if (at + len > fw->up_total) {
        status = 413;
        err = "more bytes than the total";
        fw->up_model = 0;
    }
    unlock(fw);
    if (err)
        return error(out, status, err);

    if (len && !fw->ops.write(fw->ops.ctx, model, at, data, len)) {
        lock(fw);
        fw->up_model = 0;
        unlock(fw);
        return error(out, 500, "writing the firmware store failed");
    }
    lock(fw);
    fw->up_got = at + (uint32_t)len;
    uint32_t got = fw->up_got, all = fw->up_total;
    if (got >= all)
        fw->up_model = 0;
    unlock(fw);
    if (got < all) {
        esl_sb_printf(out, "{\"received\":%u,\"total\":%u}", (unsigned)got, (unsigned)all);
        return 200;
    }

    /* Complete: check what the store holds before it is offered */
    uint8_t h[ESL_FWF_HDR_LEN];
    uint8_t m;
    if (!fw->ops.read(fw->ops.ctx, model, 0, h, sizeof(h)) || !parse_header(h, &m, &img, &err) || m != model ||
        !check_slot(fw, model, &img))
        return error(out, 422, "the image does not match the CRC in its header");
    esl_tagfw_to_t to = {.ver = img.ver, .all = 1};
    lock(fw);
    if (fw->up_only) {
        to.all = 0;
        to.n = 1;
        to.mac[0] = fw->up_only;
    }
    fw->img[model - 1] = img;
    fw->to[model - 1] = to;
    unlock(fw);
    save_to(fw, model, &to);
    esl_sb_putc(out, '{');
    esl_tagfw_write_fields(out, model, &img);
    esl_sb_printf(out, ",\"received\":%u,\"total\":%u}", (unsigned)got, (unsigned)all);
    return 200;
}

bool esl_tagfw_get(esl_tagfw_t *fw, uint8_t model, esl_tagfw_img_t *img)
{
    esl_tagfw_img_t v = {0};
    if (fw && model_ok(model)) {
        lock(fw);
        v = fw->img[model - 1];
        unlock(fw);
    }
    if (img)
        *img = v;
    return v.ver != 0;
}

bool esl_tagfw_delete(esl_tagfw_t *fw, uint8_t model)
{
    if (!model_ok(model))
        return false;
    lock(fw);
    bool had = fw->img[model - 1].ver != 0;
    fw->img[model - 1].ver = 0;
    if (fw->up_model == model)
        fw->up_model = 0;
    unlock(fw);
    if (had) {
        fw->ops.erase(fw->ops.ctx, model); /* outside the lock, as in esl_tagfw_part */
        save_to(fw, model, NULL);
    }
    return had;
}

bool esl_tagfw_get_to(esl_tagfw_t *fw, uint8_t model, esl_tagfw_to_t *to)
{
    esl_tagfw_to_t v = {0};
    bool has = false;
    if (fw && model_ok(model)) {
        lock(fw);
        has = fw->img[model - 1].ver != 0;
        if (has)
            v = fw->to[model - 1];
        unlock(fw);
    }
    if (to)
        *to = v;
    return has;
}

static bool chosen(const esl_tagfw_to_t *to, uint64_t mac)
{
    if (to->all)
        return true;
    for (unsigned i = 0; i < to->n; i++)
        if (to->mac[i] == mac)
            return true;
    return false;
}

bool esl_tagfw_for(esl_tagfw_t *fw, uint8_t model, uint64_t mac)
{
    esl_tagfw_to_t to;
    return esl_tagfw_get_to(fw, model, &to) && chosen(&to, mac);
}

int esl_tagfw_choose(esl_tagfw_t *fw, uint8_t model, uint64_t mac)
{
    if (!fw || !model_ok(model))
        return 404;
    int status = 0;
    esl_tagfw_to_t to;
    lock(fw);
    esl_tagfw_to_t *t = &fw->to[model - 1];
    if (fw->img[model - 1].ver == 0)
        status = 404;
    else if (!mac)
        t->all = 1;
    else if (chosen(t, mac))
        ; /* already */
    else if (t->n >= ESL_TAGFW_CHOSEN)
        status = 409;
    else
        t->mac[t->n++] = mac;
    to = *t;
    unlock(fw);
    if (status == 0)
        save_to(fw, model, &to);
    return status;
}

size_t esl_tagfw_block(esl_tagfw_t *fw, uint8_t model, uint16_t ver, uint32_t crc, uint16_t pos, uint8_t max,
                       uint16_t next_poll_s, uint8_t *out)
{
    uint8_t data[ESL_FW_CHUNK_MAX];
    size_t n = 0;

    if (!fw || !model_ok(model))
        return 0;
    lock(fw);
    esl_tagfw_img_t img = fw->img[model - 1];
    if (img.ver == 0) {
        unlock(fw);
        return 0;
    }
    if (ver != img.ver || crc != img.crc || pos >= img.size)
        pos = 0;
    n = img.size - pos;
    if (n > max)
        n = max;
    if (n > ESL_FW_CHUNK_MAX)
        n = ESL_FW_CHUNK_MAX;
    if (n == 0 || !fw->ops.read(fw->ops.ctx, model, ESL_FWF_HDR_LEN + pos, data, n)) {
        unlock(fw);
        return 0;
    }
    unlock(fw);
    return esl_fw_msg_build(out, next_poll_s, img.ver, img.size, pos, img.crc, model, data, n);
}
