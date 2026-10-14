#include "tinytest.h"
#include "esl_store.h"

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

static void f_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    fake_t *f = ctx;
    (void)c;
    CHECK_INT(f->depth, 0); /* persistence happens outside the lock */
    f->saves++;
    f->last_slot = slot;
    f->last_mac = mac;
}

static void f_erase(void *ctx, unsigned slot)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->erases++;
    f->last_slot = slot;
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
    esl_poll_t p = {3, 2950, shown, 0};
    return esl_store_on_poll(st, mac, &p, -61, now, reply);
}

void test_store(void)
{
    enum { CAP = 4 };
    esl_tag_t tags[CAP];
    esl_store_t st;
    fake_t f = {0};
    esl_store_ops_t ops = {&f, f_lock, f_unlock, f_save, f_erase};
    esl_set_result_t r;
    esl_tag_t t;
    uint8_t reply[ESL_MAX_PAYLOAD];
    const uint64_t A = 0xA1, B = 0xB2;

    esl_store_init(&st, tags, CAP, &ops, 0);
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

    /* tag polls with old content -> CONTENT with the ready-made reply */
    size_t n = poll(&st, A, 0, 1000, reply);
    CHECK_INT(reply[0], ESL_MSG_CONTENT);
    uint8_t expect[ESL_MAX_PAYLOAD];
    CHECK_INT(n, esl_content_msg_build(expect, 0, r.content_id, &milk));
    CHECK_MEM(reply, expect, n);
    CHECK(esl_store_get(&st, A, &t));
    CHECK(t.seen);
    CHECK_INT(t.battery_mv, 2950);
    CHECK_INT(t.rssi, -61);
    CHECK_INT(t.fw, 3);
    CHECK_INT(t.polls, 1);
    CHECK(esl_tag_pending(&t));

    /* tag reports the new id -> IDLE, no longer pending */
    n = poll(&st, A, r.content_id, 2000, reply);
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
    n = poll(&st, A, esl_content_id(&milk), 3000, reply);
    CHECK_INT(reply[0], ESL_MSG_CONTENT);

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
    esl_store_init(&st, tags, CAP, &ops, 0);
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
    esl_store_init(&st, tags, CAP, NULL, 0);
    CHECK_INT(esl_store_set(&st, A, &milk, &r), ESL_STORE_OK);
    CHECK(esl_store_delete(&st, A));
}
