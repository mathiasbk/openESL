/*
 * sec.h - the tag's side of radio security (docs/protokoll.md part 1, "Security").
 *
 * A tag without a key talks as before (ESL_PROTO_VER). A tag with a key adds the trailer
 * (counter + MIC) to everything it sends, and takes only answers whose MIC is right for
 * the counter of the message they answer. The key comes in PAIR from the AP.
 * The key and the restart count are kept in the settings record (app.c), so the counter
 * never repeats for a key.
 */
#ifndef SEC_H
#define SEC_H

#include "compat.h"
#include "proto.h"

typedef struct {
    uint8_t has_key;
    uint8_t key[ESL_SEC_KEY_LEN];
    uint16_t boots;           /* high 16 bits of the counter: restarts with this key */
    uint16_t n;               /* low 16 bits: messages since */
    uint32_t sent;            /* counter of the last message sent, which the answer is checked against */
    uint8_t save_due;         /* save the record (radio off): after sec_boot, sec_pair, or with n
                                 near its end (then sec_roll first) */
} sec_state_t;

extern sec_state_t __xdata sec;

/* Messages per restart before the counter moves to the next boots (a save is asked for well
   before the low half runs out; a round sends at most a few hundred messages) */
#define SEC_N_SAVE   0xF000u

/* After a restart, with the record loaded: a new boots value, so no counter is used twice
   (save_due is set; the caller saves the record). */
void sec_boot(void);

/* Before a save with the radio off: when n is near its end, the next range (boots + 1). It
   is saved before the next message uses it. */
void sec_roll(void);

/* The message at p (len bytes, with room for ESL_SEC_TRAILER_TAG more) as it is sent:
   with a key, ESL_PROTO_VER_SEC and the trailer. Returns the new length. A message sent
   again (no answer) is wrapped again: the AP takes a counter equal to the last one too. */
uint8_t sec_wrap(uint8_t *p, uint8_t len);

/* An answer at p (len bytes) to the last wrapped message. With a key it must be
   ESL_PROTO_VER_SEC with a right MIC; without, ESL_PROTO_VER. Returns the length of the
   message without the trailer, with the version byte set to ESL_PROTO_VER so it parses as
   any other, or -1 to ignore it. *secure says whether it was authenticated. */
int8_t sec_unwrap(uint8_t *p, uint8_t len, uint8_t *secure);

/* PAIR with key k (16 bytes), from an answer sec_unwrap took: a tag without a key takes it
   from a plain PAIR, a tag with one only from an authenticated PAIR, where all zeros means
   forget the key. Returns true if the key changed (save the record, radio off). */
bool sec_pair(const uint8_t *k, uint8_t secure);

#endif /* SEC_H */
