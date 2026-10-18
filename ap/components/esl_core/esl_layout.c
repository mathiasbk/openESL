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

/* The model's file, or NULL when it has none (or the model is unknown, or ls is
 * NULL). Call it under the lock: a delete frees the file. */
static esl_layout_t *slot(esl_layouts_t *ls, uint8_t model)
{
    return ls && esl_model_name(model) ? ls->models[model - 1] : NULL;
}

static void drop(esl_layout_t *l)
{
    if (l) {
        free(l->data);
        free(l);
    }
}

void esl_layouts_free(esl_layouts_t *ls)
{
    for (unsigned i = 0; i < ESL_MODEL_COUNT - 1; i++) {
        drop(ls->models[i]);
        ls->models[i] = NULL;
    }
}

void esl_layouts_init(esl_layouts_t *ls, const esl_layout_ops_t *ops)
{
    esl_layouts_free(ls);
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
    if (ls && esl_model_name(model) && esl_layout_compile(data, len, model, bin, err, sizeof(err))) {
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
    *changed = false;
    if (!ls || !esl_model_name(model) || len == 0 || len > ESL_LAYOUT_MAX_SIZE || !bin->version ||
        bin->bin[1] != model)
        return 0;

    lock(ls);
    esl_layout_t *l = ls->models[model - 1];
    uint32_t v = 0;
    *changed = !l || l->len != len || memcmp(l->data, data, len) != 0;
    if (*changed) {
        /* The new JSON first, so the old file stays if there is no memory */
        char *copy = malloc(len);
        esl_layout_t *nl = l ? l : calloc(1, sizeof(*nl));
        if (copy && nl) {
            memcpy(copy, data, len);
            free(nl->data);
            nl->data = copy;
            nl->len = (uint16_t)len;
            nl->bin = *bin;
            ls->models[model - 1] = l = nl;
        } else {
            free(copy);
            if (nl != l)
                free(nl);
            *changed = false;
        }
    }
    if (l)
        v = l->bin.version;
    unlock(ls);

    if (*changed && ls->ops.persist_save)
        ls->ops.persist_save(ls->ops.ctx, model, data, len);
    return v;
}

bool esl_layouts_delete(esl_layouts_t *ls, uint8_t model)
{
    if (!ls || !esl_model_name(model))
        return false;
    lock(ls);
    esl_layout_t *l = ls->models[model - 1];
    bool had = l != NULL;
    ls->models[model - 1] = NULL;
    unlock(ls);
    drop(l);
    if (had && ls->ops.persist_erase)
        ls->ops.persist_erase(ls->ops.ctx, model);
    return had;
}

bool esl_layouts_info(esl_layouts_t *ls, uint8_t model, uint32_t *version, size_t *len, size_t *bin_size)
{
    uint32_t v = 0;
    size_t n = 0, b = 0;
    if (ls) {
        lock(ls);
        esl_layout_t *l = slot(ls, model);
        if (l) {
            v = l->bin.version;
            n = l->len;
            b = l->bin.size;
        }
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
    if (!ls)
        return false;
    lock(ls);
    esl_layout_t *l = slot(ls, model);
    for (size_t i = 0; l && i < l->len; i++)
        esl_sb_putc(out, l->data[i]);
    unlock(ls);
    return l != NULL;
}

void esl_layouts_write_names(esl_layouts_t *ls, uint8_t model, esl_sb_t *out)
{
    esl_sb_putc(out, '[');
    if (ls) {
        lock(ls);
        esl_layout_t *l = slot(ls, model);
        for (unsigned i = 0; l && i < l->bin.count; i++) {
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
    int i = -1;
    if (!ls)
        return -1;
    lock(ls);
    esl_layout_t *l = slot(ls, model);
    if (l) {
        i = esl_layout_bin_find(&l->bin, name, len);
        if (i >= 0 && nfields)
            *nfields = l->bin.nfields[i];
    }
    unlock(ls);
    return i;
}

void esl_layouts_env(esl_layouts_t *ls, uint8_t model, esl_layout_env_t *env)
{
    memset(env, 0, sizeof(*env));
    if (!ls)
        return;
    lock(ls);
    esl_layout_t *l = slot(ls, model);
    if (l) {
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
    size_t n;
    if (!ls)
        return esl_layout_msg_build(out, next_poll_s, 0, 0, 0, NULL, 0);
    lock(ls);
    esl_layout_t *l = slot(ls, model);
    if (!l) {
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
    bool ok = true;
    if (!ls || c->layout != ESL_LAYOUT_NAMED)
        return true;
    size_t nl = 0;
    while (nl < ESL_LAYOUT_NAME_MAX && c->name[nl])
        nl++;
    lock(ls);
    esl_layout_t *l = slot(ls, model);
    int i = l ? esl_layout_bin_find(&l->bin, c->name, nl) : -1;
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
