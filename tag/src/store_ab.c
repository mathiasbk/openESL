/* store_ab.c - see store_ab.h */
#include "store_ab.h"

#define STORE_MAGIC_OLD 0xA5
#define STORE_MAGIC     0xA6

/* The length of the good copy at f (0 = none), its data and sequence number; 0x100 for
   the old format, which only page A can hold */
static uint8_t check(const uint8_t *f, bool page_a, const uint8_t **data, uint16_t *seq)
{
    uint8_t i, len, off, sum = 0;

    if (f[0] == STORE_MAGIC) {
        off = 3;
        *seq = f[1];
    } else if (f[0] == STORE_MAGIC_OLD && page_a) {
        off = 2;
        *seq = 0x100;
    } else {
        return 0;
    }
    len = f[off - 1];
    if (len == 0 || len > HAL_STORE_MAX)
        return 0;
    for (i = 0; i < len; i++)
        sum += f[off + i];
    *data = f + off;
    return sum == f[off + len] ? len : 0;
}

static uint8_t newest(const uint8_t *a, const uint8_t *b, const uint8_t **data, uint8_t *len, uint16_t *seq)
{
    const uint8_t *da, *db;
    uint16_t sa, sb;
    uint8_t la = check(a, true, &da, &sa), lb = check(b, false, &db, &sb);

    /* sequence numbers wrap: b is newer when it is 1..127 ahead of a; an old-format a is
       newer than any b (store_ab.h) */
    if (lb && (!la || (sa != 0x100 && (int8_t)((uint8_t)sb - (uint8_t)sa) > 0))) {
        *data = db;
        *len = lb;
        *seq = sb;
        return 2;
    }
    *data = da;
    *len = la;
    *seq = sa;
    return la ? 1 : 0;
}

uint8_t store_ab_newest(const uint8_t *a, const uint8_t *b, const uint8_t **data, uint8_t *len)
{
    uint16_t seq;
    return newest(a, b, data, len, &seq);
}

/* The copy in buf (seq and data already there) into one page, checked */
static bool put(const uint8_t *a, const uint8_t *b, uint8_t *buf, bool to_a, uint8_t len)
{
    const uint8_t *d;
    uint16_t seq;

    return store_ab_page(to_a, buf, (uint8_t)(len + 4)) &&
           check(to_a ? a : b, to_a, &d, &seq) == len;
}

bool store_ab_write(const uint8_t *a, const uint8_t *b, uint8_t *buf, const uint8_t *data, uint8_t len)
{
    const uint8_t *d;
    uint8_t i, sum = 0, n;
    uint16_t seq;
    uint8_t cur = newest(a, b, &d, &n, &seq);
    bool to_a = cur != 1;

    if (len == 0 || len > HAL_STORE_MAX)
        return false;
    buf[0] = STORE_MAGIC;
    buf[1] = cur ? (uint8_t)(seq + 1) : 0;
    buf[2] = len;
    for (i = 0; i < len; i++) {
        buf[3 + i] = data[i];
        sum += data[i];
    }
    buf[3 + len] = sum;
    if (!put(a, b, buf, to_a, len))
        return false;
    if (!to_a && a[0] == STORE_MAGIC_OLD) {
        /* the first write after older firmware: A gets the copy too, so its old one is
           gone (a cut here leaves B's copy, the newer one) */
        buf[1]++;
        put(a, b, buf, true, len);
    }
    /* what counts now is this copy */
    if (!newest(a, b, &d, &n, &seq) || n != len)
        return false;
    for (i = 0; i < len; i++)
        if (d[i] != data[i])
            return false;
    return true;
}

uint8_t store_ab_read(const uint8_t *a, const uint8_t *b, uint8_t *data, uint8_t maxlen)
{
    const uint8_t *d;
    uint8_t i, len;

    if (!store_ab_newest(a, b, &d, &len) || len > maxlen)
        return 0;                       /* also maxlen 0: just checks the record */
    for (i = 0; i < len; i++)
        data[i] = d[i];
    return len;
}
