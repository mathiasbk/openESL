#include "barcode.h"

/* Code 128 symbol values 0-105, 11 modules each, the first module in bit 10 */
static const uint16_t __code c128[106] = {
    0x6CC, 0x66C, 0x666, 0x498, 0x48C, 0x44C, 0x4C8, 0x4C4,
    0x464, 0x648, 0x644, 0x624, 0x59C, 0x4DC, 0x4CE, 0x5CC,
    0x4EC, 0x4E6, 0x672, 0x65C, 0x64E, 0x6E4, 0x674, 0x76E,
    0x74C, 0x72C, 0x726, 0x764, 0x734, 0x732, 0x6D8, 0x6C6,
    0x636, 0x518, 0x458, 0x446, 0x588, 0x468, 0x462, 0x688,
    0x628, 0x622, 0x5B8, 0x58E, 0x46E, 0x5D8, 0x5C6, 0x476,
    0x776, 0x68E, 0x62E, 0x6E8, 0x6E2, 0x6EE, 0x758, 0x746,
    0x716, 0x768, 0x762, 0x71A, 0x77A, 0x642, 0x78A, 0x530,
    0x50C, 0x4B0, 0x486, 0x42C, 0x426, 0x590, 0x584, 0x4D0,
    0x4C2, 0x434, 0x432, 0x612, 0x650, 0x7BA, 0x614, 0x47A,
    0x53C, 0x4BC, 0x49E, 0x5E4, 0x4F4, 0x4F2, 0x7A4, 0x794,
    0x792, 0x6DE, 0x6F6, 0x7B6, 0x578, 0x51E, 0x45E, 0x5E8,
    0x5E2, 0x7A8, 0x7A2, 0x5DE, 0x5EE, 0x75E, 0x7AE, 0x684,
    0x690, 0x69C,};
#define C128_STOP      0x18EBu  /* 13 modules */
#define C128_CODE_B    100
#define C128_START_B   104
#define C128_START_C   105

/* EAN-13: the L code of each digit (R is its complement, G is R backwards), and the
   L/G pattern of the left half chosen by the first digit (bit 5 = first left digit, 1 = G) */
static const uint8_t __code ean_l[10] = {0x0D, 0x19, 0x13, 0x3D, 0x23, 0x31, 0x2F, 0x3B, 0x37, 0x0B};
static const uint8_t __code ean_par[10] = {0x00, 0x0B, 0x0D, 0x0E, 0x13, 0x19, 0x1C, 0x15, 0x16, 0x1A};

/* Writer state in XRAM, not on the small 8051 stack */
static uint8_t *__xdata bc_bits;
static uint16_t __xdata bc_pos;
static uint16_t __xdata bc_max;

/* Appends the n low bits of v, highest first. false if it would pass bc_max. */
static bool put(uint16_t v, uint8_t n)
{
    uint8_t m;
    if (bc_pos + n > bc_max)
        return false;
    while (n--) {
        m = (uint8_t)(0x80 >> (bc_pos & 7));
        if ((v >> n) & 1)
            bc_bits[bc_pos >> 3] |= m;
        else
            bc_bits[bc_pos >> 3] &= (uint8_t)~m;
        bc_pos++;
    }
    return true;
}

static bool is_digits(const uint8_t *t, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        if (t[i] < '0' || t[i] > '9')
            return false;
    return true;
}

static uint16_t code128(const uint8_t *t, uint8_t len)
{
    uint16_t sum;
    uint8_t i, v, pos = 1;
    bool c = len >= 2 && is_digits(t, len);

    if (!len)
        return 0;
    for (i = 0; i < len; i++)
        if (t[i] < 32 || t[i] > 127)
            return 0;
    v = c ? C128_START_C : C128_START_B;
    sum = v;
    if (!put(c128[v], 11))
        return 0;
    i = 0;
    if (c) {
        for (; i + 1 < len; i += 2) {
            v = (uint8_t)((t[i] - '0') * 10 + (t[i + 1] - '0'));
            sum += (uint16_t)v * pos++;
            if (!put(c128[v], 11))
                return 0;
        }
        if (i < len) {              /* an odd last digit: switch to code set B */
            sum += (uint16_t)C128_CODE_B * pos++;
            if (!put(c128[C128_CODE_B], 11))
                return 0;
        }
    }
    for (; i < len; i++) {
        v = (uint8_t)(t[i] - 32);
        sum += (uint16_t)v * pos++;
        sum %= 103;                 /* keeps sum in 16 bits for long texts */
        if (!put(c128[v], 11))
            return 0;
    }
    if (!put(c128[sum % 103], 11) || !put(C128_STOP, 13))
        return 0;
    return bc_pos;
}

static uint8_t rev7(uint8_t v)
{
    uint8_t r = 0, i;
    for (i = 0; i < 7; i++, v >>= 1)
        r = (uint8_t)((r << 1) | (v & 1));
    return r;
}

static uint16_t ean13(const uint8_t *t, uint8_t len)
{
    uint8_t i, d, sum = 0, par;

    if ((len != 12 && len != 13) || !is_digits(t, len))
        return 0;
    for (i = 0; i < 12; i++)
        sum += (uint8_t)((t[i] - '0') * ((i & 1) ? 3 : 1));
    sum = (uint8_t)((10 - sum % 10) % 10);
    if (len == 13 && t[12] - '0' != sum)
        return 0;
    par = ean_par[t[0] - '0'];
    if (!put(0x5, 3))
        return 0;
    for (i = 1; i <= 12; i++) {
        d = i < 12 ? (uint8_t)(t[i] - '0') : sum;
        if (i == 7 && !put(0x0A, 5))        /* centre guard 01010 */
            return 0;
        if (i >= 7)
            d = (uint8_t)(~ean_l[d] & 0x7F);                 /* R */
        else if (par & (0x20 >> (i - 1)))
            d = rev7((uint8_t)(~ean_l[d] & 0x7F));           /* G */
        else
            d = ean_l[d];                                    /* L */
        if (!put(d, 7))
            return 0;
    }
    if (!put(0x5, 3))
        return 0;
    return bc_pos;
}

uint16_t barcode_encode(uint8_t sym, const uint8_t *t, uint8_t len, uint8_t *bits, uint16_t max)
{
    bc_bits = bits;
    bc_pos = 0;
    bc_max = max;
    if (sym == ESL_BC_CODE128)
        return code128(t, len);
    if (sym == ESL_BC_EAN13)
        return ean13(t, len);
    return 0;
}

uint8_t barcode_quiet_left(uint8_t sym)
{
    return sym == ESL_BC_EAN13 ? ESL_BC_QUIET_EAN_L : ESL_BC_QUIET_128;
}

uint8_t barcode_quiet_right(uint8_t sym)
{
    return sym == ESL_BC_EAN13 ? ESL_BC_QUIET_EAN_R : ESL_BC_QUIET_128;
}
