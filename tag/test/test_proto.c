#include <string.h>
#include "test.h"
#include "helpers.h"
#include "proto_tag.h"
#include "model.h"

static const uint8_t mac[8] = { 0x38, 0x3B, 0xCB, 0xE5, 0x97, 0x01, 0x00, 0x00 };

static void test_build_poll(void)
{
    uint8_t p[ESL_POLL_LEN_CFG];
    static const uint8_t expect[ESL_POLL_LEN_CFG] = {
        0x01, 0x01, 0x02, 0x01, 0x86, 0x0B, 0x01, 0x52, 0xB7, 0xAD, 0x03, MODEL_CODE,
        0x34, 0x12
    };
    CHECK_EQ(proto_build_poll(p, 0x0102, 2950, 0xADB75201ul, 0x03, 0x1234), ESL_POLL_LEN_CFG);
    CHECK_EQ(ESL_POLL_OFF_CFG_VER + 2, ESL_POLL_LEN_CFG);
    CHECK(memcmp(p, expect, sizeof(expect)) == 0);
}

static void test_img_messages(void)
{
    proto_reply_t r;
    uint8_t d[3] = { 1, 2, 3 }, pl[ESL_MAX_PAYLOAD], out[ESL_MAX_PAYLOAD], len;
    static const uint8_t expect[ESL_IREQ_LEN] = {
        0x0A, 0x01, MODEL_CODE, 92, 0xEF, 0xCD, 0xAB, 0x89, 0xF8, 0x11
    };

    len = make_img_block(pl, 300, 0x89ABCDEFul, 4736, 92, MODEL_CODE, ESL_IMG_FMT_RAW, d, 3);
    CHECK_EQ(len, ESL_IMG_HDR_LEN + 3);
    memset(&r, 0, sizeof(r));
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_OK);
    CHECK_EQ(r.type, ESL_MSG_IMG);
    CHECK_EQ(r.next_poll_s, 300);
    CHECK(r.img_id == 0x89ABCDEFul);
    CHECK_EQ(r.size, 4736);
    CHECK_EQ(r.pos, 92);
    CHECK_EQ(r.fw_model, MODEL_CODE);
    CHECK_EQ(r.img_fmt, ESL_IMG_FMT_RAW);
    CHECK_EQ(r.data_len, 3);
    CHECK_EQ(pl[r.data_off + 2], 3);
    CHECK(proto_parse_reply(pl, ESL_IMG_HDR_LEN - 1, &r) != PROTO_OK);
    /* a block past the end of the image, and a full one */
    len = make_img_block(pl, 0, 1, 4736, 4735, MODEL_CODE, ESL_IMG_FMT_RAW, d, 2);
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_E_FIELD);
    len = make_img_block(pl, 0, 1, 4736, 0, MODEL_CODE, ESL_IMG_FMT_RAW, out, ESL_IMG_CHUNK_MAX);
    CHECK_EQ(len, ESL_MAX_PAYLOAD - ESL_MIC_RESERVE);
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_OK);
    CHECK_EQ(r.data_len, ESL_IMG_CHUNK_MAX);

    CHECK_EQ(proto_build_img_req(out, 0x89ABCDEFul, 4600, ESL_IMG_CHUNK_MAX), ESL_IREQ_LEN);
    CHECK(memcmp(out, expect, sizeof(expect)) == 0);
}

static void test_crc32(void)
{
    /* CRC-32/ISO-HDLC of "123456789" is 0xCBF43926, also in two pieces */
    CHECK(proto_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926ul);
    CHECK(proto_crc32(proto_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) == 0xCBF43926ul);
    CHECK(proto_crc32(0, (const uint8_t *)"", 0) == 0);
}

static void test_parse_idle(void)
{
    uint8_t p[8];
    proto_reply_t r;
    uint8_t n = make_idle(p, 300);
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
    CHECK_EQ(r.type, ESL_MSG_IDLE);
    CHECK_EQ(r.next_poll_s, 300);
}

static void test_parse_errors(void)
{
    static const uint8_t d[4] = { 1, 2, 3, 4 };
    uint8_t p[ESL_MAX_PAYLOAD + 4];
    proto_reply_t r;
    uint8_t n;

    n = make_img_block(p, 0, 1, 4736, 0, MODEL_CODE, ESL_IMG_FMT_RAW, d, 4);
    CHECK_EQ(proto_parse_reply(p, 3, &r), PROTO_E_SHORT);

    p[1] = 2;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_VERSION);
    p[1] = ESL_PROTO_VER;

    /* messages for the tag of the old firmware, which drew the screen itself, and messages
       for the AP are not replies the tag takes */
    p[0] = ESL_MSG_POLL;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_TYPE);
    p[0] = ESL_MSG_CONTENT;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_TYPE);
    p[0] = ESL_MSG_LAYOUT;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_TYPE);
    p[0] = ESL_MSG_IMG;

    CHECK_EQ(proto_parse_reply(p, ESL_MAX_PAYLOAD + 1, &r), PROTO_E_LONG);
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
}

static void test_parse_config(void)
{
    uint8_t p[ESL_MAX_PAYLOAD + 1];
    proto_reply_t r;
    uint8_t n = make_config(p, 120, 0xBEEF, 120, 900, ESL_CFG_LOST_AP_NEVER);

    CHECK_EQ(n, ESL_CONFIG_LEN);
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
    CHECK_EQ(r.type, ESL_MSG_CONFIG);
    CHECK_EQ(r.next_poll_s, 120);
    CHECK_EQ(r.cfg_ver, 0xBEEF);
    CHECK_EQ(r.cfg_poll_s, 120);
    CHECK_EQ(r.cfg_max_poll_s, 900);
    CHECK_EQ(r.cfg_lost_ap_s, ESL_CFG_LOST_AP_NEVER);

    /* settings a newer AP appends are skipped; a cut message is not accepted */
    memset(p + n, 0xEE, 6);
    CHECK_EQ(proto_parse_reply(p, (uint8_t)(n + 6), &r), PROTO_OK);
    CHECK_EQ(r.cfg_lost_ap_s, ESL_CFG_LOST_AP_NEVER);
    CHECK_EQ(proto_parse_reply(p, (uint8_t)(n - 1), &r), PROTO_E_SHORT);
    CHECK_EQ(proto_parse_reply(p, ESL_MAX_PAYLOAD + 1, &r), PROTO_E_LONG);

    /* an IDLE afterwards leaves no settings behind in r */
    n = make_idle(p, 0);
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
    CHECK_EQ(r.cfg_ver, 0);
    CHECK_EQ(r.cfg_poll_s, 0);
}

static void test_build_frame(void)
{
    uint8_t payload[ESL_POLL_LEN_CFG];
    uint8_t f[128];
    static const uint8_t hdr[ESL_MAC_HDR_LEN] = {
        0x41, 0xC8, 0x07, 0x53, 0x45, 0x00, 0x00,
        0x38, 0x3B, 0xCB, 0xE5, 0x97, 0x01, 0x00, 0x00
    };
    proto_build_poll(payload, 1, 3000, 0, 0, 0);
    CHECK_EQ(proto_build_frame(f, 7, ESL_DEFAULT_PAN_ID, mac, payload, ESL_POLL_LEN_CFG),
             ESL_MAC_HDR_LEN + ESL_POLL_LEN_CFG);
    CHECK(memcmp(f, hdr, sizeof(hdr)) == 0);
    CHECK(memcmp(f + ESL_MAC_HDR_LEN, payload, ESL_POLL_LEN_CFG) == 0);
    CHECK_EQ(proto_build_frame(f, 0, ESL_DEFAULT_PAN_ID, mac, payload, ESL_MAX_PAYLOAD + 1), 0);
}

static void test_check_frame(void)
{
    uint8_t payload[8], f[128];
    uint8_t other[8];
    uint8_t off = 0, n;

    n = make_ap_frame(f, 1, ESL_DEFAULT_PAN_ID, mac, payload, make_idle(payload, 0));
    CHECK_EQ(f[0], 0x41);
    CHECK_EQ(f[1], 0x8C);
    CHECK_EQ(proto_check_frame(f, n, ESL_DEFAULT_PAN_ID, mac, &off), ESL_IDLE_LEN);
    CHECK_EQ(off, ESL_MAC_HDR_LEN);

    CHECK_EQ(proto_check_frame(f, n, 0x1234, mac, &off), PROTO_E_PAN);
    CHECK_EQ(proto_check_frame(f, 10, ESL_DEFAULT_PAN_ID, mac, &off), PROTO_E_SHORT);

    memcpy(other, mac, 8);
    other[0] ^= 1;
    CHECK_EQ(proto_check_frame(f, n, ESL_DEFAULT_PAN_ID, other, &off), PROTO_E_ADDR);

    f[ESL_AT_OFF_SRC] = 0x01;     /* not from the AP */
    CHECK_EQ(proto_check_frame(f, n, ESL_DEFAULT_PAN_ID, mac, &off), PROTO_E_ADDR);
    f[ESL_AT_OFF_SRC] = 0x00;

    f[0] = 0x61;                   /* ACK request satt */
    CHECK_EQ(proto_check_frame(f, n, ESL_DEFAULT_PAN_ID, mac, &off), PROTO_E_FRAME);
}

static void test_fcs(void)
{
    /* CRC-16/KERMIT (= 802.15.4 FCS) of "123456789" is 0x2189 */
    CHECK_EQ(proto_fcs((const uint8_t *)"123456789", 9), 0x2189);
}

void test_proto(void)
{
    RUN(test_build_poll);
    RUN(test_img_messages);
    RUN(test_crc32);
    RUN(test_parse_idle);
    RUN(test_parse_errors);
    RUN(test_parse_config);
    RUN(test_build_frame);
    RUN(test_check_frame);
    RUN(test_fcs);
}
