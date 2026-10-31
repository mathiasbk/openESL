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

/* ---- font files ------------------------------------------------------------------------------ */

bool esl_layouts_font_restore(esl_layouts_t *ls, unsigned n, const uint8_t *blob, size_t len)
{
    esl_ufont_t *f = esl_font_from_blob(blob, len);
    bool ok = false;
    if (!ls || !f || n >= ESL_FONT_USER_MAX) {
        esl_font_free(f);
        return false;
    }
    const char *name = esl_font_name(f);
    lock(ls);
    if (!esl_font_slot(n) && esl_font_slot_of(name, strlen(name)) < 0 && esl_font_bytes() + len <= ESL_FONT_MAX_BYTES) {
        esl_font_swap(n, f);
        ok = true;
    }
    unlock(ls);
    if (!ok)
        esl_font_free(f);
    return ok;
}

int esl_layouts_font_put(esl_layouts_t *ls, esl_ufont_t *f, unsigned *models, bool *replaced, char *err,
                         size_t errlen)
{
    *models = 0;
    *replaced = false;
    esl_layout_bin_t *bin = ls && f ? malloc(sizeof(*bin)) : NULL;
    if (!bin) {
        esl_font_free(f);
        snprintf(err, errlen, "out of memory");
        return 500;
    }
    size_t blen, oldb = 0;
    const uint8_t *blob = esl_font_blob(f, &blen);
    const char *name = esl_font_name(f);
    int status = 0;

    lock(ls);
    int s = esl_font_slot_of(name, strlen(name));
    bool rep = s >= 0;
    if (rep)
        esl_font_blob(esl_font_slot((unsigned)s), &oldb);
    else
        s = esl_font_free_slot();
    size_t others = esl_font_bytes() - oldb;
    unlock(ls);
    if (s < 0) {
        status = 507;
        snprintf(err, errlen, "the AP holds at most %d font files: delete one first", ESL_FONT_USER_MAX);
    } else if (others + blen > ESL_FONT_MAX_BYTES) {
        status = 507;
        snprintf(err, errlen, "the font files may take %u bytes together on this AP: this one is %u, the others %u",
                 (unsigned)ESL_FONT_MAX_BYTES, (unsigned)blen, (unsigned)others);
    } else if (ls->ops.font_save && !ls->ops.font_save(ls->ops.ctx, (unsigned)s, blob, blen)) {
        status = 507;
        snprintf(err, errlen, "no room to save the font on the AP (NVS)");
    }
    if (status) {
        esl_font_free(f);
        free(bin);
        return status;
    }

    /* In use from here: the layout files that draw with the font it replaces are compiled
       again with it, and stay as they were if one of them fails */
    esl_layout_t *nl[ESL_MODEL_COUNT - 1] = {0};
    const unsigned bit = 1u << s;
    char cerr[ESL_LAYOUT_ERR_MAX];
    lock(ls);
    esl_ufont_t *old = esl_font_swap((unsigned)s, f);
    for (unsigned m = 0; m < ESL_MODEL_COUNT - 1 && !status; m++) {
        esl_layout_t *l = ls->models[m];
        if (!l || !(l->bin.user_fonts & bit))
            continue;
        if (!esl_layout_compile(l->data, l->len, (uint8_t)(m + 1), bin, cerr, sizeof(cerr))) {
            status = 422;
            snprintf(err, errlen, "the layout file for %s does not work with this font: %s",
                     esl_model_name((uint8_t)(m + 1)), cerr);
        } else if (!(nl[m] = alloc(ls, ESL_LAYOUT_BYTES(bin->size)))) {
            status = 500;
            snprintf(err, errlen, "out of memory");
        } else {
            nl[m]->data = l->data; /* the JSON stays */
            nl[m]->len = l->len;
            memcpy(&nl[m]->bin, bin, ESL_LAYOUT_BIN_BYTES(bin->size));
        }
    }
    if (status) {
        esl_font_swap((unsigned)s, old);
    } else {
        for (unsigned m = 0; m < ESL_MODEL_COUNT - 1; m++)
            if (nl[m]) {
                free(ls->models[m]); /* not its JSON, which the new one has */
                ls->models[m] = nl[m];
                nl[m] = NULL;
                *models |= 1u << m;
            }
    }
    unlock(ls);
    free(bin);
    for (unsigned m = 0; m < ESL_MODEL_COUNT - 1; m++)
        free(nl[m]);
    if (status) {
        esl_font_free(f);
        if (ls->ops.font_save) { /* back to what was saved before */
            size_t ol = 0;
            const uint8_t *ob = old ? esl_font_blob(old, &ol) : NULL;
            ls->ops.font_save(ls->ops.ctx, (unsigned)s, ob, ol);
        }
        return status;
    }
    esl_font_free(old);
    *replaced = rep;
    return 0;
}

/* The models whose files draw with font file slot, as "2.9, 1.54" into buf; how many */
static unsigned users(esl_layouts_t *ls, unsigned slot, char *buf, size_t cap)
{
    unsigned n = 0;
    size_t pos = 0;
    if (cap)
        buf[0] = '\0';
    for (unsigned m = 0; m < ESL_MODEL_COUNT - 1; m++) {
        const esl_layout_t *l = ls->models[m];
        if (l && (l->bin.user_fonts & (1u << slot))) {
            int w = snprintf(buf + pos, cap > pos ? cap - pos : 0, "%s%s", n ? ", " : "",
                             esl_model_name((uint8_t)(m + 1)));
            if (w > 0 && pos + (size_t)w < cap)
                pos += (size_t)w;
            n++;
        }
    }
    return n;
}

int esl_layouts_font_delete(esl_layouts_t *ls, const char *name, size_t len, char *err, size_t errlen)
{
    char used[64];
    esl_ufont_t *old = NULL;
    int status = 0;
    if (!ls)
        return 404;
    lock(ls);
    int s = esl_font_slot_of(name, len);
    if (s < 0)
        status = 404;
    else if (users(ls, (unsigned)s, used, sizeof(used)))
        status = 409;
    else
        old = esl_font_swap((unsigned)s, NULL);
    unlock(ls);
    if (status == 404)
        snprintf(err, errlen, "no font file by that name");
    else if (status == 409)
        snprintf(err, errlen, "the layout file for %s draws with this font: change it first", used);
    if (old) {
        if (ls->ops.font_save)
            ls->ops.font_save(ls->ops.ctx, (unsigned)s, NULL, 0);
        esl_font_free(old);
    }
    return status;
}

bool esl_layouts_font_write(esl_layouts_t *ls, const char *name, size_t len, esl_sb_t *out)
{
    if (!ls)
        return false;
    lock(ls);
    int s = esl_font_slot_of(name, len);
    if (s >= 0)
        esl_font_write_json(esl_font_slot((unsigned)s), out);
    unlock(ls);
    return s >= 0;
}

void esl_layouts_font_list(esl_layouts_t *ls, esl_sb_t *out)
{
    unsigned n = 0;
    esl_sb_putc(out, '[');
    if (ls)
        lock(ls);
    for (unsigned s = 0; s < ESL_FONT_USER_MAX; s++) {
        const esl_ufont_t *f = esl_font_slot(s);
        if (!f)
            continue;
        esl_sb_puts(out, n++ ? "," : "");
        esl_font_write_info(f, out, true);
        esl_sb_puts(out, ",\"used_by\":[");
        unsigned u = 0;
        for (unsigned m = 0; ls && m < ESL_MODEL_COUNT - 1; m++) {
            const esl_layout_t *l = ls->models[m];
            if (l && (l->bin.user_fonts & (1u << s)))
                esl_sb_printf(out, "%s\"%s\"", u++ ? "," : "", esl_model_name((uint8_t)(m + 1)));
        }
        esl_sb_puts(out, "]}");
    }
    if (ls)
        unlock(ls);
    esl_sb_putc(out, ']');
}
