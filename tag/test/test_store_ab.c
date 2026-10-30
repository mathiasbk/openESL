/* test_store_ab.c - the settings record in two flash pages (store_ab.c): cut writes, the
   old single copy, and a downgrade to older firmware and back. */
#include <string.h>
#include "test.h"
#include "store_ab.h"

static uint8_t page[2][STORE_AB_SIZE];   /* [0] = A (0x3E), [1] = B (0x3D) */
static int writes;
static int cut_at = -1;                  /* this write stops after half the bytes */

bool store_ab_page(bool to_a, const uint8_t *buf, uint8_t len)
{
    uint8_t *p = page[to_a ? 0 : 1];
    if (writes++ == cut_at) {
        memset(p, 0xFF, STORE_AB_SIZE);  /* erased, then cut */
        memcpy(p, buf, len / 2);
        return false;
    }
    memset(p, 0xFF, STORE_AB_SIZE);
    memcpy(p, buf, len);
    return true;
}

static uint8_t buf[STORE_AB_SIZE];

static bool wr(const uint8_t *d, uint8_t n)
{
    return store_ab_write(page[0], page[1], buf, d, n);
}

static uint8_t rd(uint8_t *d)
{
    return store_ab_read(page[0], page[1], d, HAL_STORE_MAX);
}

static void fresh(void)
{
    memset(page, 0xFF, sizeof(page));
    writes = 0;
    cut_at = -1;
}

/* What firmware before radio security wrote: one copy in A as 0xA5, length, data, sum */
static void old_copy(const uint8_t *d, uint8_t n)
{
    uint8_t i, sum = 0;
    memset(page[0], 0xFF, STORE_AB_SIZE);
    page[0][0] = 0xA5;
    page[0][1] = n;
    for (i = 0; i < n; i++) {
        page[0][2 + i] = d[i];
        sum += d[i];
    }
    page[0][2 + n] = sum;
}

static void test_turns_and_cuts(void)
{
    uint8_t r[HAL_STORE_MAX], d[HAL_STORE_MAX];
    int i;

    fresh();
    CHECK_EQ(rd(r), 0);
    for (i = 0; i < 300; i++) {          /* past the sequence number's wrap */
        memset(d, i, sizeof(d));
        CHECK(wr(d, 32));
        CHECK_EQ(rd(r), 32);
        CHECK(memcmp(r, d, 32) == 0);
    }
    /* a write cut short: the copy before still counts, and the next write goes on */
    memset(d, 0x55, sizeof(d));
    cut_at = writes;
    CHECK(!wr(d, 32));
    CHECK_EQ(rd(r), 32);
    CHECK_EQ(r[0], (uint8_t)299);
    CHECK(wr(d, 32));
    CHECK_EQ(rd(r), 32);
    CHECK_EQ(r[0], 0x55);
    /* a damaged page is turned away */
    page[0][5] ^= 1;
    page[1][5] ^= 1;
    CHECK_EQ(rd(r), 0);
}

static void test_old_copy_and_downgrade(void)
{
    uint8_t r[HAL_STORE_MAX], d[HAL_STORE_MAX], old[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

    /* the old copy is read, and the first write replaces it in both pages */
    fresh();
    old_copy(old, sizeof(old));
    CHECK_EQ(rd(r), 9);
    CHECK(memcmp(r, old, 9) == 0);
    memset(d, 0x11, sizeof(d));
    CHECK(wr(d, 32));
    CHECK_EQ(page[0][0], 0xA6);
    CHECK_EQ(page[1][0], 0xA6);
    CHECK_EQ(rd(r), 32);
    CHECK_EQ(r[0], 0x11);
    /* a cut while A is rewritten: B's copy counts */
    fresh();
    old_copy(old, sizeof(old));
    cut_at = 1;
    CHECK(wr(d, 32));
    CHECK_EQ(rd(r), 32);
    CHECK_EQ(r[0], 0x11);
    /* two writes as usual */
    fresh();
    old_copy(old, sizeof(old));
    CHECK(wr(d, 32));
    memset(d, 0x22, sizeof(d));
    CHECK(wr(d, 32));

    /* a downgrade: the older firmware writes its copy into A; B keeps one of ours, with
       the key in it. Back on this firmware the old copy counts, not the stale one in B */
    old_copy(old, sizeof(old));
    CHECK_EQ(rd(r), 9);
    CHECK(memcmp(r, old, 9) == 0);
    memset(d, 0x33, sizeof(d));
    CHECK(wr(d, 32));
    CHECK_EQ(page[0][0], 0xA6);
    CHECK_EQ(rd(r), 32);
    CHECK_EQ(r[0], 0x33);
    memset(d, 0x44, sizeof(d));
    CHECK(wr(d, 32));
    CHECK_EQ(rd(r), 32);
    CHECK_EQ(r[0], 0x44);
}

void test_store_ab(void)
{
    RUN(test_turns_and_cuts);
    RUN(test_old_copy_and_downgrade);
}
