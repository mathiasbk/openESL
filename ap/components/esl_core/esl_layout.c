#include "esl_clock.h"
#include "esl_layout.h"
#include "esl_render.h"
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

/* The file a tag of the model is drawn with (its own, or the one of the model its same_as
 * names: esl_model_layout_model), or NULL when there is none (or the model is unknown, or ls
 * is NULL). Call it under the lock: a delete frees the file. */
static esl_layout_t *slot(esl_layouts_t *ls, uint8_t model)
{
    uint8_t file = esl_model_layout_model(model);
    return ls && file ? ls->models[file - 1] : NULL;
}

/* A model with same_as has no file of its own: it is stored, restored and deleted only
 * under the model it names */
static bool own_file(uint8_t model)
{
    return esl_model_name(model) && esl_model_layout_model(model) == model;
}

static void *alloc(esl_layouts_t *ls, size_t n)
{
    return ls->ops.alloc ? ls->ops.alloc(n) : malloc(n);
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
    if (ls && own_file(model) && esl_layout_compile(data, len, model, bin, err, sizeof(err))) {
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
    if (!ls || !own_file(model) || len == 0 || len > ESL_LAYOUT_MAX_SIZE || !bin->version ||
        bin->size < ESL_LBIN_HDR_LEN || bin->size > ESL_LBIN_CAP || bin->bin[1] != model)
        return 0;

    lock(ls);
    esl_layout_t *l = ls->models[model - 1], *old = NULL;
    uint32_t v = 0;
    *changed = !l || l->len != len || memcmp(l->data, data, len) != 0;
    if (*changed) {
        /* A new one as big as this file (the compiled size changes with it), and the old one
           freed after: it stays if there is no memory */
        char *copy = alloc(ls, len);
        esl_layout_t *nl = alloc(ls, ESL_LAYOUT_BYTES(bin->size));
        if (copy && nl) {
            memcpy(copy, data, len);
            nl->data = copy;
            nl->len = (uint16_t)len;
            memcpy(&nl->bin, bin, ESL_LAYOUT_BIN_BYTES(bin->size));
            old = l;
            ls->models[model - 1] = l = nl;
        } else {
            /* 0 to the caller: the old file (if any) is unchanged, but the new one is not stored */
            free(copy);
            free(nl);
            *changed = false;
            l = NULL;
        }
    }
    if (l)
        v = l->bin.version;
    unlock(ls);
    drop(old);

    if (*changed && ls->ops.persist_save)
        ls->ops.persist_save(ls->ops.ctx, model, data, len);
    return v;
}

bool esl_layouts_delete(esl_layouts_t *ls, uint8_t model)
{
    if (!ls || !own_file(model))
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

/* ESL_CLOCK_* of the layout whose name hashes to layout_hash in l, NONE when it has none. Under the lock. */
static uint8_t clock_of(const esl_layout_t *l, uint32_t layout_hash)
{
    for (unsigned i = 0; l && layout_hash && i < l->bin.count; i++)
        if (esl_layout_name_hash(l->bin.names[i]) == layout_hash)
            return l->bin.clock[i];
    return ESL_CLOCK_NONE;
}

uint8_t esl_layouts_clock(esl_layouts_t *ls, uint8_t model, uint32_t layout_hash)
{
    uint8_t res = ESL_CLOCK_NONE;
    if (ls) {
        lock(ls);
        res = clock_of(slot(ls, model), layout_hash);
        unlock(ls);
    }
    return res;
}

uint32_t esl_layouts_image_ver(esl_layouts_t *ls, uint8_t model, uint32_t layout_hash)
{
    uint32_t v = 0;
    uint8_t res = ESL_CLOCK_NONE;
    if (ls) {
        lock(ls);
        esl_layout_t *l = slot(ls, model);
        if (l) {
            v = l->bin.version;
            res = clock_of(l, layout_hash);
        }
        unlock(ls);
    }
    return v ^ esl_clock_stamp(res);
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

size_t esl_layouts_render(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, uint8_t *out, size_t cap)
{
    if (!ls)
        return esl_render(model, NULL, c, out, cap);
    lock(ls);
    const esl_layout_t *l = slot(ls, model);
    size_t n = esl_render(model, l ? &l->bin : NULL, c, out, cap);
    unlock(ls);
    return n;
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

bool esl_layouts_check_content(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, char *msg, size_t msglen)
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
    if (i >= 0)
        ok = esl_layout_check_fields(&l->bin, i, model, c, msg, msglen);
    if (ok && i >= 0 && esl_render_number_cut(model, &l->bin, c)) {
        snprintf(msg, msglen, "a number in layout %s does not fit in its box on model %s, not even in its smallest font",
                 l->bin.names[i], esl_model_name(model));
        ok = false;
    }
    unlock(ls);
    return ok;
}
