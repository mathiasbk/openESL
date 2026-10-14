#include "esl_layout.h"
#include "esl_crc32.h"
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
    return esl_model_name(model) ? &ls->models[model - 1] : NULL;
}

static uint32_t version_of(const char *data, size_t len)
{
    uint32_t v = esl_crc32(data, len);
    return v ? v : 1;
}

void esl_layouts_init(esl_layouts_t *ls, const esl_layout_ops_t *ops)
{
    memset(ls, 0, sizeof(*ls));
    if (ops)
        ls->ops = *ops;
}

bool esl_layouts_restore(esl_layouts_t *ls, uint8_t model, const char *data, size_t len)
{
    bool changed;
    esl_layout_ops_t ops = ls->ops;
    ls->ops.persist_save = NULL;
    bool ok = esl_layouts_set(ls, model, data, len, &changed) != 0;
    ls->ops = ops;
    return ok;
}

uint32_t esl_layouts_set(esl_layouts_t *ls, uint8_t model, const char *data, size_t len, bool *changed)
{
    esl_layout_t *l = slot(ls, model);
    *changed = false;
    if (!l || len == 0 || len > ESL_LAYOUT_MAX_SIZE)
        return 0;

    uint32_t v = version_of(data, len);
    lock(ls);
    *changed = l->version != v || l->len != len || memcmp(l->data, data, len) != 0;
    if (*changed) {
        memcpy(l->data, data, len);
        l->len = (uint16_t)len;
        l->version = v;
    }
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
    bool had = l->version != 0;
    l->version = 0;
    l->len = 0;
    unlock(ls);
    if (had && ls->ops.persist_erase)
        ls->ops.persist_erase(ls->ops.ctx, model);
    return had;
}

bool esl_layouts_info(esl_layouts_t *ls, uint8_t model, uint32_t *version, size_t *len)
{
    esl_layout_t *l = slot(ls, model);
    if (!l)
        return false;
    lock(ls);
    *version = l->version;
    *len = l->len;
    unlock(ls);
    return *version != 0;
}

bool esl_layouts_write(esl_layouts_t *ls, uint8_t model, esl_sb_t *out)
{
    esl_layout_t *l = slot(ls, model);
    if (!l)
        return false;
    lock(ls);
    bool has = l->version != 0;
    for (size_t i = 0; has && i < l->len; i++)
        esl_sb_putc(out, l->data[i]);
    unlock(ls);
    return has;
}
