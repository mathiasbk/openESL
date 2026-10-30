/* test_sec.c - AES, CMAC and the tag's side of radio security (sec.c), also through app.c. */
#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "app.h"
#include "config.h"
#include "cmac.h"
#include "sec.h"

static void hex(uint8_t *out, const char *s)
{
    while (s[0] && s[1]) {
        unsigned v;
        sscanf(s, "%2x", &v);
        *out++ = (uint8_t)v;
        s += 2;
    }
}

static void test_aes_fips197(void)
{
    uint8_t k[16], b[16], want[16];
    /* FIPS-197 appendix C.1 */
    hex(k, "000102030405060708090a0b0c0d0e0f");
    hex(b, "00112233445566778899aabbccddeeff");
    hex(want, "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes128_encrypt(k, b);
    CHECK(memcmp(b, want, 16) == 0);
    /* FIPS-197 appendix B */
    hex(k, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(b, "3243f6a8885a308d313198a2e0370734");
    hex(want, "3925841d02dc09fbdc118597196a0b32");
    aes128_encrypt(k, b);
    CHECK(memcmp(b, want, 16) == 0);
}

static void test_cmac_sp800_38b(void)
{
    /* SP 800-38B appendix D.1, AES-128: messages of 0, 16, 40 and 64 bytes */
    static const struct { uint8_t n; const char *mac; } v[] = {
        {0, "bb1d6929e95937287fa37d129b756746"},
        {16, "070a16b46b4d4144f79bdd9dd04a287c"},
        {40, "dfa66747de9ae63030ca32611497c827"},
        {64, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    uint8_t k[16], m[64], want[16], mac[16];
    cmac_t c;
    unsigned i, j;

    hex(k, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(m, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
           "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        hex(want, v[i].mac);
        cmac_start(&c, k);
        cmac_add(&c, m, v[i].n);
        cmac_finish(&c, mac);
        CHECK(memcmp(mac, want, 16) == 0);
        /* the same in pieces of 1, 3 and 7 bytes */
        for (j = 1; j <= 7; j += 2 + (j == 1)) {
            unsigned p = 0;
            cmac_start(&c, k);
            while (p < v[i].n) {
                unsigned n = v[i].n - p < j ? v[i].n - p : j;
                cmac_add(&c, m + p, (uint16_t)n);
                p += n;
            }
            cmac_finish(&c, mac);
            CHECK(memcmp(mac, want, 16) == 0);
        }
    }
}

/* ---- Through app.c ---- */

static const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static uint8_t pl[ESL_MAX_PAYLOAD], fr[128];

/* The counter the tag's next message gets */
static uint32_t next_ctr(void)
{
    return ((uint32_t)sec.boots << 16) | sec.n;
}

/* An answer (the message in pl, n bytes) as the AP sends it to a tag with key k, to the
   message with counter ctr: the frame in fr, returns its length */
static uint8_t make_sec(const uint8_t *k, uint32_t ctr, uint8_t n)
{
    pl[ESL_OFF_VER] = ESL_PROTO_VER_SEC;
    sec_mic(k, ESL_SEC_DIR_AP, ctr, pl, n, pl + n);
    return make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n + ESL_SEC_TRAILER_AP);
}

static void push_sec(const uint8_t *k, uint32_t ctr, uint8_t n)
{
    fake_rx_push(fr, make_sec(k, ctr, n));
}

static uint8_t make_pair(uint8_t *out, uint16_t next, const uint8_t *k)
{
    out[ESL_OFF_TYPE] = ESL_MSG_PAIR;
    out[ESL_OFF_VER] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_OFF_NEXT_POLL, next);
    memcpy(out + ESL_PAIR_OFF_KEY, k, ESL_SEC_KEY_LEN);
    return ESL_PAIR_LEN;
}

/* A new tag, paired with key by a plain PAIR */
static void paired(void)
{
    uint8_t n;
    fake_store_clear();
    fake_radio_reset();
    fake_epd_ok = true;
    app_init();
    CHECK_EQ(sec.has_key, 0);
    CHECK_EQ(fake_store_writes, 0);          /* nothing to save without a key */
    n = make_pair(pl, 0, key);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(sec.has_key, 1);
    CHECK_EQ(sec.boots, 1);
    CHECK_EQ(sec.n, 0);
    CHECK(memcmp(sec.key, key, 16) == 0);
    CHECK_EQ(fake_store_writes, 1);
    CHECK_EQ(fake_store_radio_on_write, false);
    fake_radio_reset();
}

/* The trailer of sent frame i is right for key k; returns its counter */
static uint32_t sent_ok(int i, const uint8_t *k)
{
    const uint8_t *p = fake_tx[i] + ESL_MAC_HDR_LEN;
    uint8_t len = (uint8_t)(fake_tx_len[i] - ESL_MAC_HDR_LEN - ESL_SEC_TRAILER_TAG), mic[4];
    uint32_t ctr = ESL_GET_U32(p + len);
    CHECK_EQ(p[ESL_OFF_VER], ESL_PROTO_VER_SEC);
    sec_mic(k, ESL_SEC_DIR_TAG, ctr, p, len, mic);
    CHECK(memcmp(mic, p + len + ESL_SEC_CTR_LEN, 4) == 0);
    return ctr;
}

static void test_pair_and_talk(void)
{
    uint32_t c;
    paired();
    c = next_ctr();
    CHECK_EQ(c, 0x10000);
    push_sec(key, c, make_idle(pl, 77));
    CHECK_EQ(app_poll_once(), 77);
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN_LINK + ESL_SEC_TRAILER_TAG);
    CHECK_EQ(sent_ok(0, key), c);
    CHECK_EQ(next_ctr(), c + 1);

    /* CONFIG with a MIC: taken */
    fake_radio_reset();
    push_sec(key, next_ctr(), make_config(pl, 0, 0x1234, 90, 900, 0));
    CHECK_EQ(app_poll_once(), 90);
    CHECK_EQ(app.cfg.ver, 0x1234);
}

static void test_reject_unauthenticated(void)
{
    uint8_t n;
    uint32_t c;
    paired();
    /* a plain IDLE, a plain CONFIG, a wrong MIC, another key and an old counter are all
       ignored: no answer, so the round is missed */
    n = make_idle(pl, 77);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    n = make_config(pl, 0, 0x1234, 10, 10, 0);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    c = next_ctr();
    n = make_sec(key, c, make_idle(pl, 77));
    fr[ESL_MAC_HDR_LEN + ESL_IDLE_LEN] ^= 1;    /* one MIC bit off */
    fake_rx_push(fr, n);
    {
        uint8_t other[16];
        memcpy(other, key, 16);
        other[0] ^= 0x80;
        push_sec(other, c, make_idle(pl, 77));
    }
    push_sec(key, c - 1, make_idle(pl, 77));    /* recorded earlier: replay */
    push_sec(key, c + 1, make_idle(pl, 77));    /* for a later message */
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(app.misses, 1);
    CHECK_EQ(app.cfg.ver, 0);
    CHECK_EQ(fake_tx_count, 2);                 /* the POLL and its one retry, same counter */
    CHECK_EQ(sent_ok(0, key), c);
    CHECK_EQ(sent_ok(1, key), c);

    /* a plain PAIR does not change the key of a paired tag */
    fake_radio_reset();
    {
        uint8_t k2[16] = {0};
        k2[5] = 9;
        n = make_pair(pl, 0, k2);
        fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    }
    app_poll_once();
    CHECK(memcmp(sec.key, key, 16) == 0);
}

static void test_restart_new_range(void)
{
    uint32_t before;
    paired();
    push_sec(key, next_ctr(), make_idle(pl, 0));
    app_poll_once();
    before = next_ctr();
    /* a restart (the record stays): the counter moves to the next range, saved after the
       first round (not at the start, next to the MAC read) */
    fake_radio_reset();
    app_init();
    CHECK_EQ(sec.has_key, 1);
    CHECK_EQ(sec.boots, 2);
    CHECK(next_ctr() > before);
    CHECK_EQ(ESL_GET_U16(fake_store + 30), 1);
    push_sec(key, next_ctr(), make_idle(pl, 33));
    CHECK_EQ(app_poll_once(), 33);
    CHECK_EQ(ESL_GET_U16(fake_store + 30), 2);
    CHECK_EQ(fake_store_radio_on_write, false);
    /* also when the first round gets no answer */
    fake_radio_reset();
    app_init();
    app_poll_once();
    CHECK_EQ(ESL_GET_U16(fake_store + 30), 3);
    CHECK_EQ(fake_store_radio_on_write, false);
}

static void test_counter_roll(void)
{
    int writes;
    paired();
    sec.n = SEC_N_SAVE - 1;
    writes = fake_store_writes;
    push_sec(key, next_ctr(), make_idle(pl, 0));
    app_poll_once();
    /* the range is near its end: the next one is saved before it is used */
    CHECK_EQ(sec.boots, 2);
    CHECK_EQ(sec.n, 0);
    CHECK_EQ(fake_store_writes, writes + 1);
    CHECK_EQ(ESL_GET_U16(fake_store + 30), 2);
}

static void test_rekey_and_forget(void)
{
    uint8_t k2[16], zero[16] = {0};
    paired();
    memset(k2, 0x5A, 16);
    /* an authenticated PAIR: a new key, and the counter starts again */
    push_sec(key, next_ctr(), make_pair(pl, 0, k2));
    app_poll_once();
    CHECK(memcmp(sec.key, k2, 16) == 0);
    CHECK_EQ(sec.boots, 1);
    fake_radio_reset();
    push_sec(k2, next_ctr(), make_idle(pl, 44));
    CHECK_EQ(app_poll_once(), 44);
    /* all zeros: the key is forgotten, the tag talks plainly again */
    fake_radio_reset();
    push_sec(k2, next_ctr(), make_pair(pl, 0, zero));
    app_poll_once();
    CHECK_EQ(sec.has_key, 0);
    fake_radio_reset();
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, make_idle(pl, 55)));
    CHECK_EQ(app_poll_once(), 55);
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN_LINK);
    CHECK_EQ(fake_tx[0][ESL_MAC_HDR_LEN + ESL_OFF_VER], ESL_PROTO_VER);
    /* a plain PAIR of zeros to a tag without a key does nothing */
    fake_radio_reset();
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, make_pair(pl, 0, zero)));
    app_poll_once();
    CHECK_EQ(sec.has_key, 0);
}

void test_sec(void)
{
    RUN(test_aes_fips197);
    RUN(test_cmac_sp800_38b);
    RUN(test_pair_and_talk);
    RUN(test_reject_unauthenticated);
    RUN(test_restart_new_range);
    RUN(test_counter_roll);
    RUN(test_rekey_and_forget);
    fake_store_clear();
}
