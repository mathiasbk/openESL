#include "tinytest.h"
#include "esl_store.h"
#include "esl_persist.h"
#include <stdlib.h>

#define S(x) (const uint8_t *)(x), sizeof(x) - 1

typedef struct {
    int locks, unlocks, saves, erases;
    int depth;
    unsigned last_slot;
    uint64_t last_mac;
} fake_t;

static void f_lock(void *ctx)
{
    fake_t *f = ctx;
    f->locks++;
    CHECK_INT(f->depth, 0); /* never nested */
    f->depth++;
}

static void f_unlock(void *ctx)
{
    fake_t *f = ctx;
    f->unlocks++;
    f->depth--;
}

static bool f_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    fake_t *f = ctx;
    (void)c;
    CHECK_INT(f->depth, 0); /* persistence happens outside the lock */
    f->saves++;
    f->last_slot = slot;
    f->last_mac = mac;
    return true;
}

static void f_erase(void *ctx, unsigned slot)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->erases++;
    f->last_slot = slot;
}

static void f_cfg(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    if (cfg)
        f->saves++;
    else
        f->erases++;
    f->last_slot = slot;
    f->last_mac = mac;
}

static void f_next_poll(void *ctx, const uint16_t *next_poll_s)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->saves += next_poll_s != NULL;
    f->erases += next_poll_s == NULL;
}

static esl_content_t price(const char *name, const char *p)
{
    esl_content_t c;
    esl_content_init(&c, ESL_LAYOUT_PRICE, 0);
    esl_content_add_field(&c, (const uint8_t *)name, strlen(name));
    esl_content_add_field(&c, (const uint8_t *)p, strlen(p));
    return c;
}

static size_t poll(esl_store_t *st, uint64_t mac, uint32_t shown, uint64_t now, uint8_t *reply)
{
    esl_poll_t p = {3, 2950, shown, ESL_POLL_F_IMAGE, ESL_MODEL_29, false, 0};
    return esl_store_on_poll(st, mac, &p, -61, now, NULL, reply);
}

/* What the AP last heard from each tag survives a restart (esl_persist.h "seen") */
typedef struct {
    fake_t f;
    int writes, erases;
    unsigned slot;
    uint64_t mac;
    esl_seen_t rec;
    uint32_t clock;
} seenfake_t;

static void s_seen(void *ctx, unsigned slot, uint64_t mac, const esl_seen_t *rec)
{
    seenfake_t *g = ctx;
    CHECK_INT(g->f.depth, 0); /* outside the lock */
    if (rec) {
        g->writes++;
        g->rec = *rec;
    } else {
        g->erases++;
    }
    g->slot = slot;
    g->mac = mac;
}

static uint32_t s_clock(void *ctx)
{
    return ((seenfake_t *)ctx)->clock;
}

static void test_store_seen(void)
{
    enum { CAP = 4 };
    esl_store_t st = {0};
    seenfake_t g = {0};
    esl_store_ops_t ops = {&g, f_lock, f_unlock, NULL, NULL, NULL, NULL, s_seen, s_clock, NULL, NULL};
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_tag_t t;
    esl_store_free(&st);
    esl_store_init(&st, CAP, &ops, 0);

    /* a new tag is saved at once, with the time when the clock is known */
    g.clock = 1792000000;
    poll(&st, 0xA1, 0, 1000, reply);
    CHECK_INT(g.writes, 1);
    CHECK(g.mac == 0xA1);
    CHECK_INT(g.slot, 0);
    CHECK_INT(g.rec.model, ESL_MODEL_29);
    CHECK_INT(g.rec.fw, 3);
    CHECK_INT(g.rec.battery_mv, 2950);
    CHECK_INT(g.rec.rssi, -61);
    CHECK(g.rec.unix_s == 1792000000u);

    /* the same tag again: nothing written until esl_store_save_seen */
    g.clock = 1792000060;
    poll(&st, 0xA1, 0, 61000, reply);
    CHECK_INT(g.writes, 1);
    CHECK_INT(esl_store_save_seen(&st), 1);
    CHECK_INT(g.writes, 2);
    CHECK(g.rec.unix_s == 1792000060u);
    CHECK_INT(esl_store_save_seen(&st), 0); /* not heard since */

    /* other firmware: saved at once */
    esl_poll_t p = {4, 2900, 0, 0, ESL_MODEL_29, false, 0};
    esl_store_on_poll(&st, 0xA1, &p, -70, 62000, NULL, reply);
    CHECK_INT(g.writes, 3);
    CHECK_INT(g.rec.fw, 4);
    CHECK_INT(esl_store_save_seen(&st), 0);

    /* the clock unknown: the last known time is kept */
    g.clock = 0;
    esl_store_on_poll(&st, 0xA1, &p, -70, 63000, NULL, reply);
    CHECK_INT(esl_store_save_seen(&st), 1);
    CHECK(g.rec.unix_s == 1792000060u);

    /* deleting a tag erases its record */
    CHECK(esl_store_delete(&st, 0xA1));
    CHECK_INT(g.erases, 1);
    CHECK_INT(g.slot, 0);

    /* after a restart: back in its slot, not seen since boot, with what it said */
    esl_store_t st2 = {0};
    esl_store_free(&st2);
    esl_store_init(&st2, CAP, &ops, 0);
    esl_seen_t rec = {ESL_MODEL_154, 0x0401, 2600, -88, 1792000000};
    CHECK(esl_store_restore_seen(&st2, 2, 0xB2, &rec));
    CHECK(!esl_store_restore_seen(&st2, 2, 0xC3, &rec)); /* slot holds another tag */
    CHECK(!esl_store_restore_seen(&st2, CAP, 0xC3, &rec));
    CHECK(esl_store_get_slot(&st2, 2, &t));
    CHECK(t.mac == 0xB2);
    CHECK(!t.seen);
    CHECK_INT(t.model, ESL_MODEL_154);
    CHECK_INT(t.fw, 0x0401);
    CHECK(t.seen_unix == 1792000000u);
    CHECK(!t.has_content);
    /* its first POLL with the same model and firmware writes nothing at once */
    int w = g.writes;
    esl_poll_t p2 = {0x0401, 2600, 0, 0, ESL_MODEL_154, false, 0};
    g.clock = 1792003600;
    esl_store_on_poll(&st2, 0xB2, &p2, -80, 5000, NULL, reply);
    CHECK_INT(g.writes, w);
    CHECK(esl_store_get(&st2, 0xB2, &t) && t.seen);
    CHECK_INT(esl_store_save_seen(&st2), 1);
    CHECK_INT(g.slot, 2);
    CHECK_INT(g.f.locks, g.f.unlocks);
    esl_store_free(&st);
    esl_store_free(&st2);
}

/* ---- a table that grows, and content kept only in "NVS" ---- */
enum { GROW_MAX = 100 };
typedef struct {
    uint8_t blob[GROW_MAX][ESL_PERSIST_BLOB_MAX];
    size_t len[GROW_MAX];
    bool fail_save;
    int chunks_left; /* allocations allowed; -1 = any */
} nvsfake_t;

static bool n_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    nvsfake_t *n = ctx;
    if (n->fail_save)
        return false;
    n->len[slot] = esl_persist_encode(mac, c, n->blob[slot]);
    return true;
}

static bool n_load(void *ctx, unsigned slot, uint64_t mac, esl_content_t *c)
{
    nvsfake_t *n = ctx;
    uint64_t m;
    return n->len[slot] && esl_persist_decode(n->blob[slot], n->len[slot], &m, c) && m == mac;
}

static void *n_alloc(void *ctx, size_t size)
{
    nvsfake_t *n = ctx;
    if (n->chunks_left == 0)
        return NULL;
    if (n->chunks_left > 0)
        n->chunks_left--;
    return calloc(1, size);
}

static void test_store_grow(void)
{
    esl_store_t st = {0};
    esl_set_result_t r;
    esl_content_t c;
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_content_t milk = price("Melk", "24,90"), bread = price("Brod", "39,90");

    /* no memory until the first tag; then ESL_STORE_CHUNK slots at a time, up to max */
    esl_store_init(&st, GROW_MAX, NULL, 0);
    CHECK_INT(esl_store_slots(&st), 0);
    CHECK_INT(esl_store_available(&st), GROW_MAX);
    for (unsigned i = 0; i < GROW_MAX; i++)
        CHECK_INT(esl_store_set(&st, 0x1000 + i, &milk, &r), ESL_STORE_OK);
    CHECK_INT(esl_store_slots(&st), GROW_MAX);
    CHECK_INT(esl_store_count(&st), GROW_MAX);
    CHECK_INT(esl_store_set(&st, 0x2000, &milk, &r), ESL_STORE_FULL);
    CHECK_INT(esl_store_available(&st), 0);
    esl_tag_t t;
    CHECK(esl_store_get_slot(&st, GROW_MAX - 1, &t) && t.mac == 0x1000 + GROW_MAX - 1);
    CHECK(esl_store_get_content(&st, 0x1000 + 70, &c) && esl_content_equal(&c, &milk)); /* RAM copy without load_content */
    CHECK(esl_store_delete(&st, 0x1000 + 40));
    CHECK_INT(esl_store_set(&st, 0x2000, &milk, &r), ESL_STORE_OK); /* the freed slot */
    CHECK(esl_store_get_slot(&st, 40, &t) && t.mac == 0x2000);
    esl_store_free(&st);
    CHECK_INT(esl_store_slots(&st), 0);

    /* without memory the table stops growing: a new tag is then not added */
    nvsfake_t *n = calloc(1, sizeof(*n));
    esl_store_ops_t ops = {n, NULL, NULL, n_save, NULL, NULL, NULL, NULL, NULL, n_load, n_alloc};
    n->chunks_left = 1;
    esl_store_init(&st, GROW_MAX, &ops, 0);
    for (unsigned i = 0; i < ESL_STORE_CHUNK; i++)
        poll(&st, 0x3000 + i, 0, 1000, reply);
    CHECK_INT(esl_store_count(&st), ESL_STORE_CHUNK);
    poll(&st, 0x4000, 0, 1000, reply);
    CHECK(!esl_store_get(&st, 0x4000, &t));
    CHECK_INT(esl_store_slots(&st), ESL_STORE_CHUNK);
    /* a set takes the slot of the longest-unseen tag without content instead */
    CHECK_INT(esl_store_set(&st, 0x4000, &milk, &r), ESL_STORE_OK);
    CHECK(!esl_store_get(&st, 0x3000, &t));

    /* with load_content no content is kept in RAM: it is read back from NVS */
    CHECK(st.content[0] == NULL);
    CHECK(esl_store_get(&st, 0x4000, &t));
    CHECK(t.has_content && t.content_id == esl_content_id(&milk));
    CHECK(esl_store_get_content(&st, 0x4000, &c) && esl_content_equal(&c, &milk));
    CHECK(!esl_store_get_content(&st, 0x3001, &c)); /* no content */
    n->len[0] = 0;                                  /* lost from NVS */
    CHECK(!esl_store_get_content(&st, 0x4000, &c));

    /* NVS full: the tag keeps what it had */
    CHECK_INT(esl_store_set(&st, 0x4000, &milk, &r), ESL_STORE_OK); /* unchanged: nothing written */
    CHECK_INT(esl_store_set(&st, 0x3001, &milk, &r), ESL_STORE_OK);
    n->fail_save = true;
    CHECK_INT(esl_store_set(&st, 0x3001, &bread, &r), ESL_STORE_NOT_SAVED);
    CHECK(esl_store_get(&st, 0x3001, &t) && t.content_id == esl_content_id(&milk));
    CHECK(esl_store_get_content(&st, 0x3001, &c) && esl_content_equal(&c, &milk));
    CHECK_INT(esl_store_set(&st, 0x3002, &bread, &r), ESL_STORE_NOT_SAVED);
    CHECK(esl_store_get(&st, 0x3002, &t) && !t.has_content);
    esl_store_free(&st);
    free(n);
}

void test_store(void)
{
    enum { CAP = 4 };
    esl_store_t st = {0};
    fake_t f = {0};
    esl_store_ops_t ops = {&f, f_lock, f_unlock, f_save, f_erase, NULL, NULL, NULL, NULL, NULL, NULL};
    esl_set_result_t r;
    esl_tag_t t;
    uint8_t reply[ESL_MAX_PAYLOAD];
    const uint64_t A = 0xA1, B = 0xB2;

    esl_store_free(&st);
    esl_store_init(&st, CAP, &ops, 0);
    CHECK_INT(esl_store_count(&st), 0);
    CHECK_INT(esl_store_available(&st), CAP);

    /* new content: changed, pending, persisted */
    esl_content_t milk = price("Melk", "24,90");
    CHECK_INT(esl_store_set(&st, A, &milk, &r), ESL_STORE_OK);
    CHECK(r.changed);
    CHECK(r.pending);
    CHECK_INT(r.content_id, esl_content_id(&milk));
    CHECK_INT(f.saves, 1);
    CHECK(f.last_mac == A);

    /* same content again: no change, no NVS write */
    CHECK_INT(esl_store_set(&st, A, &milk, &r), ESL_STORE_OK);
    CHECK(!r.changed);
    CHECK(r.pending);
    CHECK_INT(f.saves, 1);

    /* tag polls showing something else -> IDLE (the caller then offers the image) */
    size_t n = poll(&st, A, 0, 1000, reply);
    CHECK_INT(n, ESL_IDLE_LEN);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    CHECK(esl_store_get(&st, A, &t));
    CHECK_INT(t.content_err, ESL_CERR_NONE);
    CHECK(t.seen);
    CHECK_INT(t.battery_mv, 2950);
    CHECK_INT(t.rssi, -61);
    CHECK_INT(t.fw, 3);
    CHECK_INT(t.polls, 1);
    CHECK(esl_tag_pending(&t));

    /* the image is drawn and the tag reports its id -> IDLE, no longer pending */
    esl_store_image_known(&st, A, r.content_id, 0, 0x1234ABCDu);
    esl_store_get(&st, A, &t);
    CHECK(esl_tag_pending(&t));
    n = poll(&st, A, 0x1234ABCDu, 2000, reply);
    CHECK_INT(n, ESL_IDLE_LEN);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    esl_store_get(&st, A, &t);
    CHECK(!esl_tag_pending(&t));
    CHECK_INT(t.last_seen_ms, 2000);
    CHECK_INT(esl_store_set(&st, A, &milk, &r), ESL_STORE_OK);
    CHECK(!r.changed);
    CHECK(!r.pending);

    /* changed price -> pending again */
    esl_content_t milk2 = price("Melk", "22,90");
    esl_store_set(&st, A, &milk2, &r);
    CHECK(r.changed);
    CHECK(r.pending);
    CHECK_INT(f.saves, 2);
    n = poll(&st, A, 0x1234ABCDu, 3000, reply);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    esl_store_get(&st, A, &t);
    CHECK(esl_tag_pending(&t)); /* new content: drawn again */
    CHECK_INT(t.image_id, 0);

    /* a tag without images cannot show content */
    esl_poll_t old = {3, 2950, 0, 0, ESL_MODEL_29, true, 0};
    CHECK_INT(esl_store_on_poll(&st, A, &old, -61, 3500, NULL, reply), ESL_IDLE_LEN);
    esl_store_get(&st, A, &t);
    CHECK_INT(t.content_err, ESL_CERR_NO_IMAGE);
    CHECK(esl_content_err_text(t.content_err) != NULL);

    /* unknown tag polls: added with content_id 0, gets IDLE, not persisted */
    n = poll(&st, B, 0, 4000, reply);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    CHECK(esl_store_get(&st, B, &t));
    CHECK(!t.has_content);
    CHECK_INT(t.content_id, 0);
    CHECK(!esl_tag_pending(&t));
    CHECK_INT(f.saves, 2);
    CHECK_INT(esl_store_count(&st), 2);

    /* next_poll_s goes into both IDLE and the prebuilt CONTENT */
    esl_store_set_next_poll(&st, 120);
    n = poll(&st, B, 0, 4100, reply);
    CHECK_MEM(reply, "\x02\x01\x78\x00", 4);
    n = poll(&st, A, 0, 4100, reply);
    CHECK_INT(reply[2], 120);

    /* fill the table with discovered tags */
    poll(&st, 0xC3, 0, 5000, reply);
    poll(&st, 0xD4, 0, 6000, reply);
    CHECK_INT(esl_store_count(&st), 4);
    CHECK_INT(esl_store_available(&st), 3); /* B, C3, D4 have no content */

    /* a fifth tag polling while full is answered but not added */
    n = poll(&st, 0xE5, 0, 7000, reply);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    CHECK(!esl_store_get(&st, 0xE5, &t));

    /* content for a new MAC evicts the longest-unseen tag without content (B) */
    esl_store_set(&st, 0xE5, &milk, &r);
    CHECK(esl_store_get(&st, 0xE5, &t));
    CHECK(!esl_store_get(&st, B, &t));
    esl_store_set(&st, 0xC3, &milk, &r);
    esl_store_set(&st, 0xD4, &milk, &r);
    CHECK_INT(esl_store_available(&st), 0);
    CHECK_INT(esl_store_set(&st, 0xF6, &milk, &r), ESL_STORE_FULL);

    /* delete frees the slot and erases NVS */
    int erases = f.erases;
    CHECK(esl_store_delete(&st, 0xC3));
    CHECK_INT(f.erases, erases + 1);
    CHECK(!esl_store_delete(&st, 0xC3));
    CHECK_INT(esl_store_count(&st), 3);
    CHECK_INT(esl_store_set(&st, 0xF6, &milk, &r), ESL_STORE_OK);

    /* deleting a tag without content does not touch NVS */
    poll(&st, 0x77, 0, 8000, reply); /* table is full again: not added */
    esl_store_delete(&st, 0xF6);
    poll(&st, 0x77, 0, 8000, reply);
    erases = f.erases;
    CHECK(esl_store_delete(&st, 0x77));
    CHECK_INT(f.erases, erases);

    CHECK_INT(f.locks, f.unlocks);

    /* restore at boot fills the given slot without writing NVS */
    esl_store_free(&st);
    esl_store_init(&st, CAP, &ops, 0);
    int saves = f.saves;
    CHECK(esl_store_restore(&st, 2, A, &milk));
    CHECK(!esl_store_restore(&st, 2, B, &milk)); /* slot taken */
    CHECK(!esl_store_restore(&st, 3, A, &milk)); /* MAC already present */
    CHECK(!esl_store_restore(&st, CAP, B, &milk));
    CHECK_INT(f.saves, saves);
    CHECK(esl_store_get_slot(&st, 2, &t));
    CHECK(t.mac == A);
    CHECK(!t.seen);
    CHECK(esl_tag_pending(&t));
    CHECK(!esl_store_get_slot(&st, 0, &t));
    n = poll(&st, A, esl_content_id(&milk), 100, reply);
    CHECK_INT(reply[0], ESL_MSG_IDLE); /* tag already shows it: no redraw after AP reboot */

    /* works without ops */
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    CHECK_INT(esl_store_set(&st, A, &milk, &r), ESL_STORE_OK);
    CHECK(esl_store_delete(&st, A));

    /* ---- per-tag settings and the default interval ---- */
    fake_t g = {0};
    esl_store_ops_t cops = {&g, f_lock, f_unlock, f_save, f_erase, f_cfg, f_next_poll, NULL, NULL, NULL, NULL};
    esl_cfg_result_t cr;
    esl_store_free(&st);
    esl_store_init(&st, CAP, &cops, 0);
    esl_tag_cfg_t c120 = {120, 0, 0}, none = {0, 0, 0};
    CHECK_INT(esl_store_set_cfg(&st, A, &c120, &cr), ESL_STORE_OK);
    CHECK(cr.changed);
    CHECK(!cr.pending); /* not seen yet */
    CHECK_INT(g.saves, 1);
    CHECK_INT(esl_store_set_cfg(&st, A, &c120, &cr), ESL_STORE_OK);
    CHECK(!cr.changed);
    CHECK_INT(g.saves, 1);
    CHECK_INT(esl_store_available(&st), CAP - 1);

    /* old firmware (no cfg_ver): IDLE with its interval; new firmware: CONFIG until it confirms */
    n = poll(&st, A, 0, 100, reply);
    CHECK_MEM(reply, "\x02\x01\x78\x00", 4);
    esl_poll_t np = {3, 2950, 0, ESL_POLL_F_IMAGE, ESL_MODEL_29, true, 0};
    n = esl_store_on_poll(&st, A, &np, -61, 200, NULL, reply);
    CHECK_INT(n, ESL_CONFIG_LEN);
    CHECK_INT(reply[0], ESL_MSG_CONFIG);
    esl_store_get(&st, A, &t);
    CHECK(esl_tag_cfg_pending(&t, 0));
    np.cfg_ver = esl_tag_cfg_version(&c120);
    n = esl_store_on_poll(&st, A, &np, -61, 300, NULL, reply);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    esl_store_get(&st, A, &t);
    CHECK(!esl_tag_cfg_pending(&t, 0));

    /* own poll_s goes into the reply; the default does not override it */
    esl_store_set(&st, A, &milk, &r);
    esl_store_set_next_poll(&st, 300);
    CHECK_INT(g.saves, 3); /* content + default */
    n = esl_store_on_poll(&st, A, &np, -61, 400, NULL, reply);
    CHECK_INT(reply[0], ESL_MSG_IDLE);
    CHECK_INT(ESL_GET_U16(&reply[ESL_OFF_NEXT_POLL]), 120);
    esl_store_set_cfg(&st, A, &none, &cr); /* back to defaults: stored entry erased */
    CHECK_INT(g.erases, 1);
    n = esl_store_on_poll(&st, A, &np, -61, 500, NULL, reply);
    CHECK_INT(ESL_GET_U16(&reply[ESL_OFF_NEXT_POLL]), 300);
    np.content_id = esl_content_id(&milk);
    n = esl_store_on_poll(&st, A, &np, -61, 600, NULL, reply);
    CHECK_INT(reply[0], ESL_MSG_CONFIG); /* {300,0,0} differs from what the tag has */
    CHECK_INT(ESL_GET_U16(&reply[ESL_CONFIG_OFF_POLL]), 300);
    esl_store_reset_next_poll(&st);
    CHECK_INT(esl_store_get_next_poll(&st), 0);
    CHECK_INT(g.erases, 2);

    /* settings without content keep the tag from eviction; delete erases them */
    esl_store_set_cfg(&st, B, &c120, &cr);
    erases = g.erases;
    CHECK(esl_store_delete(&st, B));
    CHECK_INT(g.erases, erases + 1);

    /* restore: settings and content land in the same slot, in either order */
    esl_store_free(&st);
    esl_store_init(&st, CAP, &cops, 60);
    saves = g.saves;
    CHECK(esl_store_restore_cfg(&st, 1, A, &c120));
    CHECK(esl_store_restore(&st, 1, A, &milk));
    CHECK(esl_store_restore(&st, 2, B, &milk));
    CHECK(esl_store_restore_cfg(&st, 2, B, &c120));
    CHECK(!esl_store_restore_cfg(&st, 2, A, &c120)); /* slot held by another MAC */
    CHECK(!esl_store_restore_cfg(&st, 3, A, &c120)); /* MAC already present */
    esl_store_restore_next_poll(&st, 90);
    CHECK_INT(g.saves, saves);
    CHECK_INT(esl_store_count(&st), 2);
    esl_store_get_slot(&st, 1, &t);
    CHECK_INT(t.cfg.poll_s, 120);
    uint8_t rb[ESL_MAX_PAYLOAD];
    CHECK(poll(&st, A, 0, 0, rb) > 0); /* its own interval goes out in the reply */
    CHECK_INT(rb[ESL_OFF_TYPE], ESL_MSG_IDLE);
    CHECK_INT(ESL_GET_U16(&rb[ESL_OFF_NEXT_POLL]), 120);
    CHECK_INT(st.next_poll_builtin, 60);
    CHECK_INT(g.locks, g.unlocks);
    esl_store_free(&st);

    test_store_seen();
    test_store_grow();
}
