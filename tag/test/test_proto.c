#include <string.h>
#include "test.h"
#include "helpers.h"
#include "proto_tag.h"
#include "model.h"

static const uint8_t mac[8] = { 0x38, 0x3B, 0xCB, 0xE5, 0x97, 0x01, 0x00, 0x00 };

static void test_build_poll(void)
{
    uint8_t p[ESL_POLL_LEN];
    static const uint8_t expect[ESL_POLL_LEN] = {
        0x01, 0x01, 0x02, 0x01, 0x86, 0x0B, 0x01, 0x52, 0xB7, 0xAD, 0x03, MODEL_CODE
    };
    CHECK_EQ(proto_build_poll(p, 0x0102, 2950, 0xADB75201ul, 0x03), ESL_POLL_LEN);
    CHECK(memcmp(p, expect, sizeof(expect)) == 0);
}

static void test_parse_idle(void)
{
    uint8_t p[8];
    proto_reply_t r;
    uint8_t n = make_idle(p, 300);
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
    CHECK_EQ(r.type, ESL_MSG_IDLE);
    CHECK_EQ(r.next_poll_s, 300);
    CHECK_EQ(r.nfields, 0);
}

static void test_parse_content(void)
{
    static const char *const f[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
    uint8_t p[ESL_MAX_PAYLOAD];
    proto_reply_t r;
    uint8_t n = make_content(p, 0, 0xADB75201ul, ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT, 3, f);

    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
    CHECK_EQ(r.type, ESL_MSG_CONTENT);
    CHECK_EQ(r.content_id, 0xADB75201ul);
    CHECK_EQ(r.layout, ESL_LAYOUT_PRICE);
    CHECK_EQ(r.flags, ESL_CF_PRICE_HIGHLIGHT);
    CHECK_EQ(r.nfields, 3);
    CHECK_EQ(r.field_len[0], 12);
    CHECK(memcmp(p + r.field_off[0], "Lettmelk 1 L", 12) == 0);
    CHECK(memcmp(p + r.field_off[1], "24,90", 5) == 0);
    CHECK(memcmp(p + r.field_off[2], "kr/l 24,90", 10) == 0);
    CHECK_EQ(r.field_len[3], 0);
}

static void test_parse_content_empty_fields_and_mic(void)
{
    static const char *const f[] = { "Velkommen", "", "", "" };
    uint8_t p[ESL_MAX_PAYLOAD];
    proto_reply_t r;
    uint8_t n = make_content(p, 0, 7, ESL_LAYOUT_TEXT, 0, 4, f);

    /* 4 bytes of future MIC after the fields must be ignored */
    memset(p + n, 0xEE, 4);
    CHECK_EQ(proto_parse_reply(p, (uint8_t)(n + 4), &r), PROTO_OK);
    CHECK_EQ(r.nfields, 4);
    CHECK_EQ(r.field_len[0], 9);
    CHECK_EQ(r.field_len[1], 0);
    CHECK_EQ(r.field_len[3], 0);
}

static void test_parse_errors(void)
{
    static const char *const f[] = { "Navn", "10,-" };
    uint8_t p[ESL_MAX_PAYLOAD + 4];
    proto_reply_t r;
    uint8_t n;

    n = make_content(p, 0, 1, ESL_LAYOUT_PRICE, 0, 2, f);
    CHECK_EQ(proto_parse_reply(p, 3, &r), PROTO_E_SHORT);
    CHECK_EQ(proto_parse_reply(p, 8, &r), PROTO_E_SHORT);       /* CONTENT-hode kuttet */
    CHECK_EQ(proto_parse_reply(p, (uint8_t)(n - 1), &r), PROTO_E_FIELD);

    p[1] = 2;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_VERSION);
    p[1] = ESL_PROTO_VER;

    p[0] = ESL_MSG_POLL;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_TYPE);
    p[0] = ESL_MSG_CONTENT;

    p[8] = 9;
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_E_LAYOUT);
    p[8] = ESL_LAYOUT_PRICE;

    CHECK_EQ(proto_parse_reply(p, ESL_MAX_PAYLOAD + 1, &r), PROTO_E_LONG);
    CHECK_EQ(proto_parse_reply(p, n, &r), PROTO_OK);
}

static void test_build_frame(void)
{
    uint8_t payload[ESL_POLL_LEN];
    uint8_t f[128];
    static const uint8_t hdr[ESL_MAC_HDR_LEN] = {
        0x41, 0xC8, 0x07, 0x53, 0x45, 0x00, 0x00,
        0x38, 0x3B, 0xCB, 0xE5, 0x97, 0x01, 0x00, 0x00
    };
    proto_build_poll(payload, 1, 3000, 0, 0);
    CHECK_EQ(proto_build_frame(f, 7, ESL_DEFAULT_PAN_ID, mac, payload, ESL_POLL_LEN),
             ESL_MAC_HDR_LEN + ESL_POLL_LEN);
    CHECK(memcmp(f, hdr, sizeof(hdr)) == 0);
    CHECK(memcmp(f + ESL_MAC_HDR_LEN, payload, ESL_POLL_LEN) == 0);
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
    RUN(test_parse_idle);
    RUN(test_parse_content);
    RUN(test_parse_content_empty_fields_and_mic);
    RUN(test_parse_errors);
    RUN(test_build_frame);
    RUN(test_check_frame);
    RUN(test_fcs);
}
