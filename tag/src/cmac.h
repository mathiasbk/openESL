/*
 * cmac.h - AES-128 (FIPS-197, encryption only) and AES-CMAC (NIST SP 800-38B), for the
 * radio's message authentication (docs/protokoll.md, "Security").
 * Our own code from the two standards. Used by the tag and, through the same file, by
 * the AP (ap/components/esl_core), so both sides compute the same MIC.
 */
#ifndef CMAC_H
#define CMAC_H

#include "compat.h"

#define AES_BLOCK 16

/* Encrypts block (16 bytes) in place with the 16-byte key. */
void aes128_encrypt(const uint8_t *key, uint8_t *block);

/* CMAC over data given in pieces: cmac_start, cmac_add as often as needed, cmac_finish. */
typedef struct {
    uint8_t key[AES_BLOCK];
    uint8_t x[AES_BLOCK];      /* the chain so far */
    uint8_t buf[AES_BLOCK];    /* the last, not yet chained block */
    uint8_t n;                 /* bytes in buf */
} cmac_t;

void cmac_start(cmac_t *c, const uint8_t *key);
void cmac_add(cmac_t *c, const uint8_t *p, uint16_t len);
/* Writes the 16-byte CMAC to mac. */
void cmac_finish(cmac_t *c, uint8_t *mac);

/*
 * The radio's MIC: the first ESL_SEC_MIC_LEN bytes of
 *   CMAC(key, dir || counter (u32, little-endian) || payload)
 * dir is ESL_SEC_DIR_TAG or ESL_SEC_DIR_AP; payload is the message without its trailer.
 */
void sec_mic(const uint8_t *key, uint8_t dir, uint32_t counter, const uint8_t *payload, uint8_t len,
             uint8_t *mic);

#endif /* CMAC_H */
