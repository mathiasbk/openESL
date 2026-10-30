#include "sec.h"
#include "cmac.h"

sec_state_t __xdata sec;

static uint8_t __xdata mic[ESL_SEC_MIC_LEN];

void sec_boot(void)
{
    if (!sec.has_key)
        return;
    sec.boots++;
    sec.n = 0;
    sec.save_due = 1;
}

uint8_t sec_wrap(uint8_t *p, uint8_t len)
{
    if (!sec.has_key)
        return len;
    sec.sent = ((uint32_t)sec.boots << 16) | sec.n;
    if (sec.n != 0xFFFFu)
        sec.n++;                     /* stays at the top only while saves keep failing */
    if (sec.n >= SEC_N_SAVE)
        sec.save_due = 1;            /* the next round starts a new range (sec_roll) */
    p[ESL_OFF_VER] = ESL_PROTO_VER_SEC;
    ESL_PUT_U32(p + len, sec.sent);
    sec_mic(sec.key, ESL_SEC_DIR_TAG, sec.sent, p, len, p + len + ESL_SEC_CTR_LEN);
    return len + ESL_SEC_TRAILER_TAG;
}

void sec_roll(void)
{
    if (sec.n >= SEC_N_SAVE) {
        sec.boots++;
        sec.n = 0;
    }
}

int8_t sec_unwrap(uint8_t *p, uint8_t len, uint8_t *secure)
{
    uint8_t i;

    *secure = 0;
    if (len < ESL_IDLE_LEN)
        return -1;
    if (!sec.has_key)
        return p[ESL_OFF_VER] == ESL_PROTO_VER ? (int8_t)len : -1;
    if (p[ESL_OFF_VER] != ESL_PROTO_VER_SEC || len < ESL_IDLE_LEN + ESL_SEC_TRAILER_AP)
        return -1;
    len -= ESL_SEC_TRAILER_AP;
    /* the MIC is over the message as sent, version byte included */
    sec_mic(sec.key, ESL_SEC_DIR_AP, sec.sent, p, len, mic);
    for (i = 0; i < ESL_SEC_MIC_LEN; i++)
        if (mic[i] != p[len + i])
            return -1;
    p[ESL_OFF_VER] = ESL_PROTO_VER;
    *secure = 1;
    return (int8_t)len;
}

bool sec_pair(const uint8_t *k, uint8_t secure)
{
    uint8_t i, zero = 1;

    if (sec.has_key && !secure)
        return false;
    for (i = 0; i < ESL_SEC_KEY_LEN; i++)
        if (k[i])
            zero = 0;
    if (zero && !sec.has_key)
        return false;
    sec.has_key = !zero;
    for (i = 0; i < ESL_SEC_KEY_LEN; i++)
        sec.key[i] = k[i];
    /* a new key starts its counter again: the AP forgets the old one when it sends PAIR */
    sec.boots = 1;
    sec.n = 0;
    sec.save_due = 0;
    return true;
}
