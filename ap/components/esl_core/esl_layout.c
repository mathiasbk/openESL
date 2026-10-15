#include "esl_layout.h"
#include "esl_barcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lock(esl_layouts_t *ls)
{
    if (ls->ops.lock)
        ls->ops.lock(ls->ops.ctx);
}

static void unlock(esl_layouts_t *ls)
{
    if (ls->ops.unlock)
        ls->ops.unlock(ls->ops.ctx);
}

static esl_layout_t *slot(esl_layouts_t *ls, uint8_t model)
{
    return ls && esl_model_name(model) ? &ls->models[model - 1] : NULL;
}

void esl_layouts_init(esl_layouts_t *ls, const esl_layout_ops_t *ops)
{
    memset(ls, 0, sizeof(*ls));
    if (ops)
        ls->ops = *ops;
}

bool esl_layouts_restore(esl_layouts_t *ls, uint8_t model, const char *data, size_t len)
{
    char err[ESL_LAYOUT_ERR_MAX];
    esl_layout_bin_t *bin = malloc(sizeof(*bin));
    bool changed, ok = false;
    if (!bin)
        return false;
    if (slot(ls, model) && esl_layout_compile(data, len, model, bin, err, sizeof(err))) {
        esl_layout_ops_t ops = ls->ops;
        ls->ops.persist_save = NULL;
        ok = esl_layouts_set(ls, model, data, len, bin, &changed) != 0;
        ls->ops = ops;
    }
    free(bin);
    return ok;
}

uint32_t esl_layouts_set(esl_layouts_t *ls, uint8_t model, const char *data, size_t len,
                         const esl_layout_bin_t *bin, bool *changed)
{
    esl_layout_t *l = slot(ls, model);
    *changed = false;
    if (!l || len == 0 || len > ESL_LAYOUT_MAX_SIZE || !bin->version || bin->bin[1] != model)
        return 0;

    lock(ls);
    *changed = l->len != len || memcmp(l->data, data, len) != 0;
    if (*changed) {
        memcpy(l->data, data, len);
        l->len = (uint16_t)len;
        l->bin = *bin;
    }
    uint32_t v = l->bin.version;
    unlock(ls);

    if (*changed && ls->ops.persist_save)
        ls->ops.persist_save(ls->ops.ctx, model, data, len);
    return v;
}

bool esl_layouts_delete(esl_layouts_t *ls, uint8_t model)
{
    esl_layout_t *l = slot(ls, model);
    if (!l)
        return false;
    lock(ls);
    bool had = l->len != 0;
    l->len = 0;
    memset(&l->bin, 0, sizeof(l->bin));
    unlock(ls);
    if (had && ls->ops.persist_erase)
        ls->ops.persist_erase(ls->ops.ctx, model);
    return had;
}

bool esl_layouts_info(esl_layouts_t *ls, uint8_t model, uint32_t *version, size_t *len, size_t *bin_size)
{
    esl_layout_t *l = slot(ls, model);
    uint32_t v = 0;
    size_t n = 0, b = 0;
    if (l) {
        lock(ls);
        v = l->len ? l->bin.version : 0;
        n = l->len;
        b = l->len ? l->bin.size : 0;
        unlock(ls);
    }
    if (version)
        *version = v;
    if (len)
        *len = n;
    if (bin_size)
        *bin_size = b;
    return v != 0;
}

bool esl_layouts_write(esl_layouts_t *ls, uint8_t model, esl_sb_t *out)
{
    esl_layout_t *l = slot(ls, model);
    if (!l)
        return false;
    lock(ls);
    bool has = l->len != 0;
    for (size_t i = 0; has && i < l->len; i++)
        esl_sb_putc(out, l->data[i]);
    unlock(ls);
    return has;
}

void esl_layouts_write_names(esl_layouts_t *ls, uint8_t model, esl_sb_t *out)
{
    esl_layout_t *l = slot(ls, model);
    esl_sb_putc(out, '[');
    if (l) {
        lock(ls);
        for (unsigned i = 0; l->len && i < l->bin.count; i++) {
            if (i)
                esl_sb_putc(out, ',');
            esl_sb_json_str(out, l->bin.names[i]); /* names are plain ASCII */
        }
        unlock(ls);
    }
    esl_sb_putc(out, ']');
}

int esl_layouts_find(esl_layouts_t *ls, uint8_t model, const char *name, size_t len, uint8_t *nfields)
{
    esl_layout_t *l = slot(ls, model);
    int i = -1;
    if (!l)
        return -1;
    lock(ls);
    if (l->len) {
        i = esl_layout_bin_find(&l->bin, name, len);
        if (i >= 0 && nfields)
            *nfields = l->bin.nfields[i];
    }
    unlock(ls);
    return i;
}

void esl_layouts_env(esl_layouts_t *ls, uint8_t model, esl_layout_env_t *env)
{
    esl_layout_t *l = slot(ls, model);
    memset(env, 0, sizeof(*env));
    if (!l)
        return;
    lock(ls);
    if (l->len) {
        env->version = l->bin.version;
        env->count = l->bin.count;
        memcpy(env->nfields, l->bin.nfields, sizeof(env->nfields));
        memcpy(env->names, l->bin.names, sizeof(env->names));
    }
    unlock(ls);
}

size_t esl_layouts_block(esl_layouts_t *ls, uint8_t model, uint32_t ver, uint16_t pos, uint8_t max,
                         uint16_t next_poll_s, uint8_t *out)
{
    esl_layout_t *l = slot(ls, model);
    size_t n;
    if (!l)
        return esl_layout_msg_build(out, next_poll_s, 0, 0, 0, NULL, 0);
    lock(ls);
    if (!l->len) {
        n = esl_layout_msg_build(out, next_poll_s, 0, 0, 0, NULL, 0);
    } else {
        const esl_layout_bin_t *b = &l->bin;
        if (ver != b->version || pos >= b->size)
            pos = 0; /* first block, or the file changed: start over */
        size_t take = b->size - pos;
        if (max && take > max)
            take = max;
        n = esl_layout_msg_build(out, next_poll_s, b->version, b->size, pos, &b->bin[pos], take);
    }
    unlock(ls);
    return n;
}

bool esl_layouts_check_barcode(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, char *msg, size_t msglen)
{
    esl_layout_t *l = slot(ls, model);
    bool ok = true;
    if (!l || c->layout != ESL_LAYOUT_NAMED)
        return true;
    size_t nl = 0;
    while (nl < ESL_LAYOUT_NAME_MAX && c->name[nl])
        nl++;
    lock(ls);
    int i = l->len ? esl_layout_bin_find(&l->bin, c->name, nl) : -1;
    if (i >= 0 && l->bin.bc_field[i] >= 0) {
        uint8_t len;
        const uint8_t *t = esl_content_field(c, (unsigned)l->bin.bc_field[i], &len);
        uint8_t sym = l->bin.bc_sym[i];
        int m = len ? esl_barcode_modules(sym, t, len) : 0;
        if (m < 0) {
            ok = false;
            snprintf(msg, msglen, "field %d of layout %s is a %s barcode: %s", l->bin.bc_field[i], c->name,
                     esl_barcode_name(sym),
                     sym == ESL_BC_EAN13 ? "it takes 12 digits, or 13 with a correct check digit"
                                         : "it takes ASCII characters only");
        } else if (m > l->bin.bc_max[i]) {
            ok = false;
            snprintf(msg, msglen, "field %d of layout %s is too long for its barcode on model %s (%d of %u modules)",
                     l->bin.bc_field[i], c->name, esl_model_name(model), m, (unsigned)l->bin.bc_max[i]);
        }
    }
    unlock(ls);
    return ok;
}
