#include "esl_barcode.h"
#include <stdbool.h>

static bool digits(const uint8_t *t, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (t[i] < '0' || t[i] > '9')
            return false;
    return true;
}

int esl_barcode_modules(uint8_t sym, const uint8_t *t, size_t len)
{
    if (sym == ESL_BC_EAN13) {
        if ((len != 12 && len != 13) || !digits(t, len))
            return -1;
        if (len == 13) {
            unsigned sum = 0;
            for (size_t i = 0; i < 12; i++)
                sum += (unsigned)(t[i] - '0') * ((i & 1) ? 3 : 1);
            if ((unsigned)(t[12] - '0') != (10 - sum % 10) % 10)
                return -1;
        }
        return 95 + ESL_BC_QUIET_EAN_L + ESL_BC_QUIET_EAN_R;
    }
    if (sym != ESL_BC_CODE128 || len == 0)
        return -1;
    for (size_t i = 0; i < len; i++)
        if (t[i] < 32 || t[i] > 127)
            return -1;
    size_t symbols = len >= 2 && digits(t, len) ? len / 2 + (len & 1) * 2 : len;
    /* start, data, check digit: 11 modules each; stop: 13 */
    return (int)(11 * (symbols + 2) + 13 + 2 * ESL_BC_QUIET_128);
}

const char *esl_barcode_name(uint8_t sym)
{
    return sym == ESL_BC_EAN13 ? "ean13" : sym == ESL_BC_CODE128 ? "code128" : "?";
}
