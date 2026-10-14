#include "tinytest.h"
#include "esl_layout.h"

typedef struct {
    int depth, locks, saves, erases;
    uint8_t last_model;
    size_t last_len;
} fake_t;

static void f_lock(void *ctx)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->depth++;
    f->locks++;
}

static void f_unlock(void *ctx)
{
    ((fake_t *)ctx)->depth--;
}

static void f_save(void *ctx, uint8_t model, const char *data, size_t len)
{
    fake_t *f = ctx;
    (void)data;
    CHECK_INT(f->depth, 0); /* persistence happens outside the lock */
    f->saves++;
    f->last_model = model;
    f->last_len = len;
}

static void f_erase(void *ctx, uint8_t model)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->erases++;
    f->last_model = model;
}

void test_layout(void)
{
    static esl_layouts_t ls;
    fake_t f = {0};
    esl_layout_ops_t ops = {&f, f_lock, f_unlock, f_save, f_erase};
    char buf[64];
    esl_sb_t sb;
    bool changed;
    uint32_t v;
    size_t len;

    esl_layouts_init(&ls, &ops);
    CHECK(!esl_layouts_info(&ls, ESL_MODEL_29, &v, &len));
    CHECK_INT(v, 0);

    /* unknown model, empty and oversized files are refused */
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_UNKNOWN, "{}", 2, &changed), 0);
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_COUNT, "{}", 2, &changed), 0);
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, "", 0, &changed), 0);
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, "{}", ESL_LAYOUT_MAX_SIZE + 1, &changed), 0);
    CHECK_INT(f.saves, 0);

    v = esl_layouts_set(&ls, ESL_MODEL_29, "{\"a\":1}", 7, &changed);
    CHECK(v != 0);
    CHECK(changed);
    CHECK_INT(f.saves, 1);
    CHECK_INT(f.last_model, ESL_MODEL_29);
    CHECK_INT(f.last_len, 7);
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, "{\"a\":1}", 7, &changed), v);
    CHECK(!changed);
    CHECK_INT(f.saves, 1);
    CHECK(esl_layouts_set(&ls, ESL_MODEL_29, "{\"a\":2}", 7, &changed) != v);
    CHECK(changed);

    uint32_t v2;
    CHECK(esl_layouts_info(&ls, ESL_MODEL_29, &v2, &len));
    CHECK_INT(len, 7);
    CHECK(!esl_layouts_info(&ls, ESL_MODEL_154, &v2, &len));
    esl_sb_init(&sb, buf, sizeof(buf));
    CHECK(esl_layouts_write(&ls, ESL_MODEL_29, &sb));
    CHECK_STR(buf, "{\"a\":2}");
    CHECK(!esl_layouts_write(&ls, ESL_MODEL_154, &sb));
    CHECK(!esl_layouts_write(&ls, 9, &sb));

    /* delete erases once */
    CHECK(esl_layouts_delete(&ls, ESL_MODEL_29));
    CHECK_INT(f.erases, 1);
    CHECK(!esl_layouts_delete(&ls, ESL_MODEL_29));
    CHECK(!esl_layouts_delete(&ls, 0));
    CHECK_INT(f.erases, 1);

    /* restore at boot does not write back */
    int saves = f.saves;
    CHECK(esl_layouts_restore(&ls, ESL_MODEL_154, "{}", 2));
    CHECK(!esl_layouts_restore(&ls, 0, "{}", 2));
    CHECK_INT(f.saves, saves);
    CHECK(esl_layouts_info(&ls, ESL_MODEL_154, &v, &len));
    CHECK(ls.ops.persist_save == f_save); /* hooks are back */
    CHECK_INT(f.depth, 0);
}
