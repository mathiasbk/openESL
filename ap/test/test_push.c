#include "tinytest.h"
#include "esl_image.h"
#include "esl_push.h"
#include "esl_render.h"
#include <stdlib.h>

/* Tags the AP sends to itself (esl_push): a store, images and a fake transport that
 * records what it was given and answers as told. */

#define TAGS 8
#define A 0xB1E0000000000A1ULL
#define B 0xB1E00000000FB2ULL

static esl_store_t st;
static esl_image_t images;
static esl_push_t push;

static struct {
    esl_push_res_t res;      /* what the next sends answer */
    unsigned calls;
    uint64_t mac;
    uint8_t model;
    size_t len;
    uint32_t crc;            /* a sum of the bytes, to tell images apart */
    uint16_t battery_mv;     /* what the tag reports */
    int8_t rssi;
} fake;

static esl_push_res_t fake_send(void *ctx, uint64_t mac, uint8_t model, const uint8_t *img, size_t len,
                                esl_push_info_t *info)
{
    (void)ctx;
    fake.calls++;
    fake.mac = mac;
    fake.model = model;
    fake.len = len;
    fake.crc = 0;
    for (size_t i = 0; i < len; i++)
        fake.crc = fake.crc * 31 + img[i];
    if (fake.res != ESL_PUSH_AWAY) {
        info->battery_mv = fake.battery_mv;
        info->rssi = fake.rssi;
    }
    return fake.res;
}

static void boot(void)
{
    static uint8_t *mem, *buf;
    static const esl_push_out_t out = {NULL, fake_send};
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    if (!mem)
        mem = malloc(esl_image_mem_size());
    if (!buf)
        buf = malloc(cap);
    esl_image_init(&images, mem, NULL);
    esl_store_free(&st);
    esl_store_init(&st, TAGS, NULL, 0);
    esl_push_init(&push, &out, buf, cap);
    memset(&fake, 0, sizeof(fake));
    fake.res = ESL_PUSH_SENT;
    fake.battery_mv = 2950;
    fake.rssi = -70;
}

static void set_price(uint64_t mac, const char *name, const char *p)
{
    esl_content_t c;
    esl_set_result_t r;
    esl_content_init(&c, ESL_LAYOUT_PRICE, 0);
    esl_content_add_field(&c, (const uint8_t *)name, strlen(name));
    esl_content_add_field(&c, (const uint8_t *)p, strlen(p));
    CHECK_INT(esl_store_set(&st, mac, &c, &r), ESL_STORE_OK);
}

static unsigned round_at(uint64_t now_ms)
{
    return esl_push_round(&push, &st, NULL, &images, now_ms, 8);
}

static void test_push_table(void)
{
    boot();
    CHECK(!esl_push_add(&push, 0, ESL_MODEL_29, 0));
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    CHECK(esl_push_add(&push, A, ESL_MODEL_154, 0)); /* again: the new model */
    CHECK_INT(esl_push_get(&push, A)->model, ESL_MODEL_154);
    CHECK_INT(esl_push_count(&push), 1);
    for (unsigned i = 1; i < ESL_PUSH_TAGS; i++)
        CHECK(esl_push_add(&push, A + i, ESL_MODEL_29, 0));
    CHECK(!esl_push_add(&push, B, ESL_MODEL_29, 0)); /* full */
    CHECK(esl_push_remove(&push, A + 3));
    CHECK(!esl_push_remove(&push, A + 3));
    CHECK(esl_push_get(&push, A + 3) == NULL);
    CHECK(esl_push_add(&push, B, ESL_MODEL_29, 0)); /* in the freed slot */
    CHECK_INT(esl_push_count(&push), ESL_PUSH_TAGS);
}

static void test_push_sends_content(void)
{
    esl_tag_t t;
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));

    /* paired, no content: the store knows it as a tag of its model, nothing is sent */
    CHECK_INT(round_at(1000), 0);
    CHECK(esl_store_get(&st, A, &t) && t.model == ESL_MODEL_29 && t.image_capable);
    CHECK_INT(fake.calls, 0);

    /* content: drawn and sent, raw, for its model; the store sees it shows it */
    set_price(A, "Milk", "1.90");
    CHECK_INT(round_at(2000), 1);
    CHECK_INT(fake.calls, 1);
    CHECK(fake.mac == A && fake.model == ESL_MODEL_29);
    CHECK_INT(fake.len, esl_render_size(ESL_MODEL_29));
    CHECK(esl_store_get(&st, A, &t));
    CHECK(t.image_id != 0 && t.shown_content_id == t.image_id);
    CHECK_INT(esl_push_get(&push, A)->shown, t.image_id);
    CHECK_INT(t.battery_mv, 2950);
    CHECK_INT(t.rssi, -70);
    CHECK_INT((long long)t.last_seen_ms, 2000);
    CHECK(!esl_tag_pending(&t));

    /* nothing new: no drawing, no sending */
    unsigned drawn = images.drawn;
    CHECK_INT(round_at(3000), 0);
    CHECK_INT(images.drawn, drawn);
    CHECK_INT(fake.calls, 1);

    /* new content: sent again, another image */
    uint32_t first = fake.crc;
    set_price(A, "Milk", "2.10");
    CHECK(esl_store_get(&st, A, &t) && esl_tag_pending(&t));
    CHECK_INT(round_at(4000), 1);
    CHECK_INT(fake.calls, 2);
    CHECK(fake.crc != first);
    CHECK(esl_store_get(&st, A, &t) && !esl_tag_pending(&t));

    /* the same content set again gives the same image: nothing is sent */
    set_price(A, "Milk", "2.10");
    round_at(5000);
    CHECK_INT(fake.calls, 2);
    CHECK_INT(push.stats.sent, 2);
}

static void test_push_away_backs_off(void)
{
    esl_tag_t t;
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    set_price(A, "Bread", "3.50");
    fake.res = ESL_PUSH_AWAY;

    CHECK_INT(round_at(1000), 1);
    CHECK_INT(esl_push_get(&push, A)->fails, 1);
    CHECK_INT((long long)esl_push_get(&push, A)->next_ms, 1000 + ESL_PUSH_RETRY_MS);
    CHECK(esl_store_get(&st, A, &t) && esl_tag_pending(&t));
    /* not before the back-off ... */
    CHECK_INT(round_at(1000 + ESL_PUSH_RETRY_MS - 1), 0);
    /* ... then again, waiting twice as long after each miss, up to the longest */
    CHECK_INT(round_at(1000 + ESL_PUSH_RETRY_MS), 1);
    CHECK_INT((long long)esl_push_get(&push, A)->next_ms, 1000 + 3 * ESL_PUSH_RETRY_MS);
    uint64_t now = 1000 + 3 * ESL_PUSH_RETRY_MS;
    for (int i = 0; i < 12; i++) {
        now = esl_push_get(&push, A)->next_ms;
        round_at(now);
    }
    CHECK_INT((long long)(esl_push_get(&push, A)->next_ms - now), (long long)ESL_PUSH_RETRY_MAX_MS);
    CHECK_INT(push.stats.away, 14);
    CHECK_INT(push.stats.sent, 0);

    /* the scan hears it: it is tried at the next round, and gets its image */
    fake.res = ESL_PUSH_SENT;
    CHECK(esl_push_seen(&push, &st, NULL, A, -55, 2800, now + 10));
    CHECK(esl_store_get(&st, A, &t) && t.rssi == -55 && t.battery_mv == 2800);
    CHECK_INT((long long)t.last_seen_ms, now + 10);
    CHECK_INT(round_at(now + 20), 1);
    CHECK_INT(esl_push_get(&push, A)->fails, 0);
    CHECK(esl_store_get(&st, A, &t) && !esl_tag_pending(&t));
    CHECK(!esl_push_seen(&push, &st, NULL, B, -55, 0, now)); /* not one of ours */
}

static void test_push_refused(void)
{
    esl_tag_t t;
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    set_price(A, "Eggs", "4.00");
    fake.res = ESL_PUSH_REFUSED;
    CHECK_INT(round_at(1000), 1);
    CHECK_INT(push.stats.refused, 1);
    CHECK(esl_store_get(&st, A, &t));
    CHECK_INT(esl_push_get(&push, A)->refused, t.image_id);
    /* the same image is not sent again, and not even drawn again */
    unsigned drawn = images.drawn;
    CHECK_INT(round_at(2000), 0);
    CHECK_INT(images.drawn, drawn);
    /* new content is */
    fake.res = ESL_PUSH_SENT;
    set_price(A, "Eggs", "4.20");
    CHECK_INT(round_at(3000), 1);
    CHECK_INT(push.stats.sent, 1);
    CHECK_INT(esl_push_get(&push, A)->refused, 0);
}

static void test_push_restart_and_limits(void)
{
    esl_tag_t t;
    boot();
    set_price(A, "Tea", "5.00");
    set_price(B, "Coffee", "6.00");
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    CHECK(esl_push_add(&push, B, ESL_MODEL_154, 0));
    /* max_sends: one per round here */
    CHECK_INT(esl_push_round(&push, &st, NULL, &images, 1000, 1), 1);
    CHECK_INT(esl_push_round(&push, &st, NULL, &images, 1000, 1), 1);
    CHECK_INT(esl_push_round(&push, &st, NULL, &images, 1000, 1), 0);
    CHECK_INT(fake.calls, 2);
    CHECK(fake.mac == B && fake.model == ESL_MODEL_154 && fake.len == esl_render_size(ESL_MODEL_154));

    /* after a restart the AP knows what each was last given (from NVS): nothing is sent */
    uint32_t shown_a = esl_push_get(&push, A)->shown;
    CHECK(esl_store_get(&st, A, &t) && t.image_id == shown_a);
    esl_push_init(&push, &push.out, push.buf, push.cap);
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, shown_a));
    unsigned calls = fake.calls;
    round_at(2000);
    CHECK_INT(fake.calls, calls);

    /* a tag deleted from the store but still paired comes back without content */
    CHECK(esl_store_delete(&st, A));
    round_at(3000);
    CHECK(esl_store_get(&st, A, &t) && !t.has_content);
    CHECK_INT(fake.calls, calls);
}

static void test_push_heard_but_unreachable(void)
{
    /* Heard every second but never reached (busy, weak): one early try per run of misses,
       then the back-off holds */
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    set_price(A, "Jam", "3.30");
    fake.res = ESL_PUSH_AWAY;
    uint64_t now = 1000;
    for (int s = 0; s < 20; s++, now += 1000) {
        esl_push_seen(&push, &st, NULL, A, -60, 0, now);
        round_at(now + 500);
    }
    CHECK_INT(fake.calls, 2); /* the first try, and the one the scan woke */
    /* a success ends the run: the next run of misses may be woken once again */
    fake.res = ESL_PUSH_SENT;
    now = esl_push_get(&push, A)->next_ms;
    round_at(now);
    CHECK_INT(fake.calls, 3);
    CHECK(!esl_push_get(&push, A)->woken);
}

static void test_push_rssi_and_scan_rate(void)
{
    esl_tag_t t;
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    esl_push_seen(&push, &st, NULL, A, -66, 0, 1000);
    CHECK(esl_store_get(&st, A, &t) && t.rssi == -66);
    unsigned polls = t.polls;
    /* heard again within ESL_PUSH_SEEN_MS: the store is not told again */
    esl_push_seen(&push, &st, NULL, A, -64, 0, 2000);
    CHECK(esl_store_get(&st, A, &t) && t.polls == polls);
    esl_push_seen(&push, &st, NULL, A, -63, 0, 1000 + ESL_PUSH_SEEN_MS);
    CHECK(esl_store_get(&st, A, &t) && t.polls == polls + 1 && t.rssi == -63);
    /* a send whose transport does not know the signal keeps the last one heard, not 0 dBm */
    fake.rssi = 0;
    set_price(A, "Salt", "0.90");
    round_at(2 * ESL_PUSH_SEEN_MS);
    CHECK(esl_store_get(&st, A, &t) && t.rssi == -63);
}

static void test_push_round_robin(void)
{
    /* One send a round: a tag whose content changes every round does not starve the next */
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    CHECK(esl_push_add(&push, B, ESL_MODEL_29, 0));
    set_price(B, "Oil", "7.00");
    unsigned b_calls = 0;
    char price[16];
    for (int r = 0; r < 4; r++) {
        snprintf(price, sizeof(price), "1.%02d", r);
        set_price(A, "Clock", price);
        esl_push_round(&push, &st, NULL, &images, 1000 + r, 1);
        b_calls += fake.mac == B;
    }
    CHECK_INT(b_calls, 1);
}

static void test_push_saved(void)
{
    boot();
    CHECK(esl_push_add(&push, A, ESL_MODEL_29, 0));
    CHECK(esl_push_unsaved(&push) == NULL);
    set_price(A, "Rice", "2.40");
    round_at(1000);
    const esl_push_tag_t *u = esl_push_unsaved(&push);
    CHECK(u && u->mac == A && u->shown != 0);
    esl_push_saved(&push, A);
    CHECK(esl_push_unsaved(&push) == NULL);
}

void test_push(void)
{
    test_push_heard_but_unreachable();
    test_push_rssi_and_scan_rate();
    test_push_round_robin();
    test_push_saved();
    test_push_table();
    test_push_sends_content();
    test_push_away_backs_off();
    test_push_refused();
    test_push_restart_and_limits();
}
