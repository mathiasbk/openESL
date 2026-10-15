#include "tinytest.h"
#include "esl_content.h"
#include "esl_crc32.h"
#include "esl_persist.h"
#include "esl_proto.h"
#include "esl_text.h"

#define S(x) (const uint8_t *)(x), sizeof(x) - 1

void test_crc32(void)
{
    CHECK_INT(esl_crc32("123456789", 9), 0xCBF43926u);
    CHECK_INT(esl_crc32("", 0), 0);
}

static size_t to_l1(const char *s, uint8_t *out, size_t max, bool *ovf)
{
    return esl_utf8_to_latin1(s, strlen(s), out, max, ovf);
}

void test_text(void)
{
    uint8_t out[64];
    bool ovf;
    size_t n;

    n = to_l1("\xC3\x85pent 9\xE2\x80\x93" "21", out, sizeof(out), &ovf);
    CHECK_INT(n, 10);
    CHECK_MEM(out, "\xC5pent 9-21", 10);
    CHECK(!ovf);

    n = to_l1("\xC3\xA6\xC3\xB8\xC3\xA5\xC3\x86\xC3\x98\xC3\x85", out, sizeof(out), &ovf);
    CHECK_INT(n, 6);
    CHECK_MEM(out, "\xE6\xF8\xE5\xC6\xD8\xC5", 6);

    /* curly quotes, no-break space, euro sign (not in Latin-1), emoji */
    n = to_l1("\xE2\x80\x9C" "a\xE2\x80\x9D\xC2\xA0\xE2\x82\xAC\xF0\x9F\x98\x80", out, sizeof(out), &ovf);
    CHECK_INT(n, 6);
    CHECK_MEM(out, "\"a\" ??", 6);

    /* invalid byte, truncated sequence, overlong '/', control char */
    n = to_l1("\xFF", out, sizeof(out), &ovf);
    CHECK_INT(n, 1);
    CHECK_INT(out[0], '?');
    n = to_l1("a\xC3", out, sizeof(out), &ovf);
    CHECK_INT(n, 2);
    CHECK_MEM(out, "a?", 2);
    n = to_l1("\xC0\xAF", out, sizeof(out), &ovf);
    CHECK_INT(n, 1);
    CHECK_INT(out[0], '?');
    n = to_l1("\xED\xA0\x80", out, sizeof(out), &ovf); /* UTF-8 encoded surrogate */
    CHECK_INT(n, 1);
    n = to_l1("a\tb", out, sizeof(out), &ovf);
    CHECK_MEM(out, "a?b", 3);
    /* en dash in a price/opening-hours text, as in the protocol example */
    n = to_l1("\xC3\x85pent 9\xE2\x80\x93" "21", out, sizeof(out), &ovf);
    CHECK_MEM(out, "\xC5pent 9-21", 10);
    CHECK(!ovf);

    /* ellipsis expands to three dots; overflow is detected before writing it */
    n = to_l1("Vent\xE2\x80\xA6", out, sizeof(out), &ovf);
    CHECK_INT(n, 7);
    CHECK_MEM(out, "Vent...", 7);
    n = to_l1("Vent\xE2\x80\xA6", out, 6, &ovf);
    CHECK_INT(n, 4);
    CHECK(ovf);
    CHECK_MEM(out, "Vent", 4);

    /* curly quotes, dashes and bullets */
    n = to_l1("\xE2\x80\x98" "a\xE2\x80\x99 \xE2\x80\x9C" "b\xE2\x80\x9D \xE2\x80\x94 \xE2\x80\xA2", out, sizeof(out), &ovf);
    CHECK_INT(n, 11);
    CHECK_MEM(out, "'a' \"b\" - \xB7", n);

    n = to_l1("\xC2\x85", out, sizeof(out), &ovf); /* C1 control */
    CHECK_INT(out[0], '?');

    n = to_l1("abcdef", out, 3, &ovf);
    CHECK_INT(n, 3);
    CHECK(ovf);
    n = to_l1("abc", out, 3, &ovf);
    CHECK_INT(n, 3);
    CHECK(!ovf);
}

void test_content(void)
{
    esl_content_t a, b;
    uint8_t big[ESL_MAX_TEXT + 1];
    memset(big, 'x', sizeof(big));

    CHECK_INT(esl_content_init(&a, 3, 0), ESL_CONTENT_ERR_LAYOUT);
    CHECK_INT(esl_content_init(&a, ESL_LAYOUT_PRICE, ESL_CFLAG_PRICE_HIGHLIGHT), ESL_CONTENT_OK);
    CHECK_INT(esl_content_add_field(&a, S("Lettmelk 1 L")), ESL_CONTENT_OK);
    CHECK_INT(esl_content_add_field(&a, S("24,90")), ESL_CONTENT_OK);
    CHECK_INT(esl_content_add_field(&a, S("kr/l 24,90")), ESL_CONTENT_OK);
    CHECK_INT(esl_content_add_field(&a, S("x")), ESL_CONTENT_ERR_TOO_MANY_FIELDS);
    CHECK_INT(esl_content_text_len(&a), 27);

    uint8_t len;
    const uint8_t *f = esl_content_field(&a, 1, &len);
    CHECK_INT(len, 5);
    CHECK_MEM(f, "24,90", 5);

    uint8_t body[ESL_CONTENT_BODY_MAX];
    size_t n = esl_content_body(&a, body);
    CHECK_INT(n, 2 + 3 + 27);
    CHECK_MEM(body, "\x01\x01\x0CLettmelk 1 L\x05" "24,90\x0Akr/l 24,90", n);
    CHECK_INT(esl_content_id(&a), esl_crc32(body, n));

    /* deterministic, and flags are part of the id */
    b = a;
    CHECK(esl_content_equal(&a, &b));
    CHECK_INT(esl_content_id(&a), esl_content_id(&b));
    b.flags = 0;
    CHECK(!esl_content_equal(&a, &b));
    CHECK(esl_content_id(&a) != esl_content_id(&b));

    /* text limit is shared across fields */
    esl_content_init(&a, ESL_LAYOUT_TEXT, 0);
    CHECK_INT(esl_content_add_field(&a, big, ESL_MAX_TEXT + 1), ESL_CONTENT_ERR_TOO_LONG);
    CHECK_INT(esl_content_add_field(&a, big, 50), ESL_CONTENT_OK);
    CHECK_INT(esl_content_add_field(&a, big, ESL_MAX_TEXT - 49), ESL_CONTENT_ERR_TOO_LONG);
    CHECK_INT(esl_content_add_field(&a, big, ESL_MAX_TEXT - 50), ESL_CONTENT_OK);
    CHECK_INT(esl_content_text_len(&a), ESL_MAX_TEXT);

    /* trailing empty fields do not change the id */
    esl_content_init(&a, ESL_LAYOUT_TEXT, 0);
    esl_content_add_field(&a, S("Velkommen"));
    esl_content_add_field(&a, S(""));
    esl_content_add_field(&a, S("hei"));
    esl_content_add_field(&a, S(""));
    esl_content_trim(&a);
    CHECK_INT(a.nfields, 3);
    esl_content_init(&b, ESL_LAYOUT_TEXT, 0);
    esl_content_add_field(&b, S("Velkommen"));
    esl_content_add_field(&b, S(""));
    esl_content_add_field(&b, S("hei"));
    CHECK_INT(esl_content_id(&a), esl_content_id(&b));

    /* the biggest content still fits the radio payload with room for a MIC */
    uint8_t msg[ESL_MAX_PAYLOAD];
    esl_content_init(&a, ESL_LAYOUT_TEXT, 3);
    for (int i = 0; i < 4; i++)
        esl_content_add_field(&a, big, ESL_MAX_TEXT / 4);
    CHECK_INT(esl_content_text_len(&a), ESL_MAX_TEXT);
    CHECK_INT(ESL_MAX_TEXT, 92);
    n = esl_content_msg_build(msg, 0, 1, &a);
    CHECK_INT(n, 8 + 2 + 4 + 92);
    CHECK(n + ESL_MIC_RESERVE == ESL_MAX_PAYLOAD);

    /* a layout from the layout file, by name (krav A14) */
    esl_content_t n1, n2, b1;
    CHECK_INT(esl_content_init_named(&n1, "shelf", 5, ESL_CFLAG_FRAME), ESL_CONTENT_OK);
    CHECK_INT(n1.layout, ESL_LAYOUT_NAMED);
    CHECK_STR(n1.name, "shelf");
    CHECK_INT(esl_content_max_fields(ESL_LAYOUT_NAMED), 4);
    CHECK_INT(esl_content_init(&b1, ESL_LAYOUT_NAMED, 0), ESL_CONTENT_ERR_LAYOUT); /* only by name */
    CHECK_INT(esl_content_init(&b1, ESL_LAYOUT_FILE + 15, 0), ESL_CONTENT_OK);     /* as sent */
    CHECK_INT(esl_content_init(&b1, ESL_LAYOUT_FILE + 16, 0), ESL_CONTENT_ERR_LAYOUT);
    CHECK_INT(esl_content_init_named(&n2, "price", 5, 0), ESL_CONTENT_ERR_LAYOUT);
    CHECK_INT(esl_content_init_named(&n2, "", 0, 0), ESL_CONTENT_ERR_LAYOUT);
    CHECK(esl_layout_name_valid("price-2.9", 9));
    CHECK(esl_layout_name_valid("a_b.c-1", 7));
    CHECK(!esl_layout_name_valid("-a", 2));
    CHECK(!esl_layout_name_valid("Shelf", 5));
    CHECK(!esl_layout_name_valid("a b", 3));
    CHECK(!esl_layout_name_valid("text", 4));
    CHECK(esl_layout_name_valid("texts", 5));
    CHECK(esl_layout_name_valid("0123456789012345678901234567890", 31));
    CHECK(!esl_layout_name_valid("01234567890123456789012345678901", 32));
    esl_content_add_field(&n1, S("Kaffe"));
    esl_content_init_named(&n2, "sign", 4, ESL_CFLAG_FRAME);
    esl_content_add_field(&n2, S("Kaffe"));
    CHECK(!esl_content_equal(&n1, &n2));
    CHECK(esl_content_id(&n1) != esl_content_id(&n2));
    uint8_t nb[ESL_CONTENT_BODY_MAX];
    size_t nn = esl_content_body(&n1, nb);
    CHECK_INT(nn, 1 + 1 + 5 + 1 + 1 + 5);
    CHECK_MEM(nb, "\xFF\x05shelf\x02\x05Kaffe", nn);
    /* resolved for a tag: layout 0x10 + index, the same id as before */
    esl_content_resolve(&n1, 3, &b1);
    CHECK_INT(b1.layout, ESL_LAYOUT_FILE + 3);
    CHECK_STR(b1.name, "");
    uint8_t cm[ESL_MAX_PAYLOAD];
    nn = esl_content_msg_build(cm, 0, 77, &b1);
    CHECK_MEM(cm, "\x03\x01\x00\x00\x4D\x00\x00\x00\x13\x02\x05Kaffe", nn);
}

void test_proto(void)
{
    static const uint8_t poll_bytes[] = {0x01, 0x01, 0x34, 0x12, 0xB8, 0x0B, 0x78, 0x56, 0x34, 0x12, 0x01, 0x00};
    esl_poll_t p;
    uint8_t buf[ESL_MAX_PAYLOAD];

    CHECK(esl_poll_parse(poll_bytes, sizeof(poll_bytes), &p));
    CHECK_INT(p.fw_ver, 0x1234);
    CHECK_INT(p.battery_mv, 3000);
    CHECK_INT(p.content_id, 0x12345678);
    CHECK_INT(p.flags, ESL_POLL_FLAG_BOOT);
    CHECK_INT(p.model, ESL_MODEL_UNKNOWN); /* old firmware: reserved byte is 0 */
    CHECK_INT(esl_poll_build(buf, &p), 12);
    CHECK_MEM(buf, poll_bytes, 12);

    /* the model byte (formerly reserved) */
    memcpy(buf, poll_bytes, 12);
    buf[11] = ESL_MODEL_154;
    CHECK(esl_poll_parse(buf, 12, &p));
    CHECK_INT(p.model, ESL_MODEL_154);
    CHECK_INT(esl_poll_build(buf, &p), 12);
    CHECK_INT(buf[ESL_POLL_OFF_MODEL], ESL_MODEL_154);

    CHECK_STR(esl_model_name(ESL_MODEL_29), "2.9");
    CHECK_STR(esl_model_name(ESL_MODEL_154), "1.54");
    CHECK(esl_model_name(ESL_MODEL_UNKNOWN) == NULL);
    CHECK(esl_model_name(7) == NULL);
    CHECK_INT(esl_model_parse("1.54", 4), ESL_MODEL_154);
    CHECK_INT(esl_model_parse("2.9", 3), ESL_MODEL_29);
    CHECK_INT(esl_model_parse("2.90", 4), ESL_MODEL_UNKNOWN);
    CHECK_INT(esl_model_parse("2.", 2), ESL_MODEL_UNKNOWN);

    CHECK(!esl_poll_parse(poll_bytes, 11, &p));
    memcpy(buf, poll_bytes, 12);
    buf[0] = 0x02;
    CHECK(!esl_poll_parse(buf, 12, &p));
    buf[0] = 0x01;
    buf[1] = 0x02;
    CHECK(!esl_poll_parse(buf, 12, &p));
    uint8_t longer[16] = {0};
    memcpy(longer, poll_bytes, 12);
    CHECK(esl_poll_parse(longer, 13, &p));
    CHECK(!p.has_cfg_ver); /* 13 bytes: no complete cfg_ver */
    longer[12] = 0x34;
    longer[13] = 0x12;
    CHECK(esl_poll_parse(longer, sizeof(longer), &p));
    CHECK(p.has_cfg_ver);
    CHECK_INT(p.cfg_ver, 0x1234);
    CHECK_INT(esl_poll_build(buf, &p), ESL_POLL_LEN_CFG);
    CHECK_MEM(buf, longer, ESL_POLL_LEN_CFG);

    /* CONFIG: version 0 for all defaults, nonzero and value-derived otherwise */
    esl_tag_cfg_t cfg = {0, 0, 0}, cfg2;
    CHECK_INT(esl_tag_cfg_version(&cfg), 0);
    cfg.poll_s = 120;
    uint16_t v = esl_tag_cfg_version(&cfg);
    CHECK(v != 0);
    cfg2 = cfg;
    CHECK_INT(esl_tag_cfg_version(&cfg2), v);
    cfg2.lost_ap_s = ESL_CFG_LOST_AP_NEVER;
    CHECK(esl_tag_cfg_version(&cfg2) != v);
    cfg.max_poll_s = 900;
    cfg.lost_ap_s = 0x1234;
    CHECK_INT(esl_config_msg_build(buf, 120, &cfg), ESL_CONFIG_LEN);
    v = esl_tag_cfg_version(&cfg);
    uint8_t expect_cfg[ESL_CONFIG_LEN] = {0x08, 0x01, 0x78, 0x00, (uint8_t)v, (uint8_t)(v >> 8),
                                          0x78, 0x00, 0x84, 0x03, 0x34, 0x12};
    CHECK_MEM(buf, expect_cfg, ESL_CONFIG_LEN);
    esl_reply_t r;
    CHECK(esl_reply_parse(buf, ESL_CONFIG_LEN, &r));
    CHECK_INT(r.type, ESL_MSG_CONFIG);
    CHECK_INT(r.next_poll_s, 120);
    CHECK_INT(r.cfg_ver, v);
    CHECK_INT(r.cfg.poll_s, 120);
    CHECK_INT(r.cfg.max_poll_s, 900);
    CHECK_INT(r.cfg.lost_ap_s, 0x1234);
    CHECK(!esl_reply_parse(buf, ESL_CONFIG_LEN - 1, &r));
    buf[ESL_CONFIG_LEN] = 0xEE; /* a newer AP may append settings */
    CHECK(esl_reply_parse(buf, ESL_CONFIG_LEN + 1, &r));

    CHECK_INT(esl_idle_build(buf, 60), 4);
    CHECK_MEM(buf, "\x02\x01\x3C\x00", 4);

    esl_content_t c;
    esl_content_init(&c, ESL_LAYOUT_PRICE, ESL_CFLAG_PRICE_HIGHLIGHT);
    esl_content_add_field(&c, S("Melk"));
    esl_content_add_field(&c, S("9,90"));
    size_t n = esl_content_msg_build(buf, 300, 0xAABBCCDD, &c);
    CHECK_INT(n, 8 + 2 + 5 + 5);
    CHECK_MEM(buf, "\x03\x01\x2C\x01\xDD\xCC\xBB\xAA\x01\x01\x04Melk\x04" "9,90", n);

    /* POLL with the layout version (18 bytes; cfg_ver 0 at 12-13 when has_cfg_ver is false) */
    esl_poll_t p2 = {1, 3000, 5, ESL_POLL_FLAG_LAYOUT_FAILED, ESL_MODEL_29, .has_layout_ver = true,
                     .layout_ver = 0xA1B2C3D4u};
    CHECK_INT(esl_poll_build(buf, &p2), ESL_POLL_LEN_LAYOUT);
    CHECK_MEM(&buf[12], "\x00\x00\xD4\xC3\xB2\xA1", 6);
    CHECK(esl_poll_parse(buf, ESL_POLL_LEN_LAYOUT, &p));
    CHECK(p.has_layout_ver);
    CHECK_INT(p.layout_ver, 0xA1B2C3D4u);
    CHECK_INT(p.flags, ESL_POLL_F_LAYOUTFAIL);
    CHECK(esl_poll_parse(buf, 17, &p)); /* 13..17 bytes: no layout version */
    CHECK(!p.has_layout_ver);
    CHECK_INT(p.layout_ver, 0);

    /* Both: cfg_ver at 12-13 and the layout version at 14-17, as the tag sends it */
    esl_poll_t p3 = {1, 3000, 5, 0, ESL_MODEL_154, true, 0xBEEF, true, 0x11223344u};
    CHECK_INT(esl_poll_build(buf, &p3), ESL_POLL_LEN_LAYOUT);
    CHECK_MEM(&buf[12], "\xEF\xBE\x44\x33\x22\x11", 6);
    CHECK(esl_poll_parse(buf, ESL_POLL_LEN_LAYOUT, &p));
    CHECK(p.has_cfg_ver && p.has_layout_ver);
    CHECK_INT(p.cfg_ver, 0xBEEF);
    CHECK_INT(p.layout_ver, 0x11223344u);
    CHECK(esl_poll_parse(buf, ESL_POLL_LEN_CFG, &p)); /* firmware with CONFIG only */
    CHECK(p.has_cfg_ver && !p.has_layout_ver);
    CHECK_INT(p.cfg_ver, 0xBEEF);
    /* the flag bits do not overlap */
    CHECK_INT(ESL_POLL_F_BOOTED & ESL_POLL_F_DRAWFAIL, 0);
    CHECK_INT((ESL_POLL_F_BOOTED | ESL_POLL_F_DRAWFAIL) & ESL_POLL_F_LAYOUTFAIL, 0);
    /* message types are distinct */
    CHECK(ESL_MSG_CONFIG != ESL_MSG_LAYOUT && ESL_MSG_CONFIG != ESL_MSG_LAYOUT_REQ);

    /* LAYOUT_REQ */
    esl_layout_req_t rq = {ESL_MODEL_154, 94, 0x01020304, 300}, rq2;
    CHECK_INT(esl_layout_req_build(buf, &rq), ESL_LREQ_LEN);
    CHECK_MEM(buf, "\x05\x01\x02\x5E\x04\x03\x02\x01\x2C\x01", ESL_LREQ_LEN);
    CHECK(esl_layout_req_parse(buf, ESL_LREQ_LEN, &rq2));
    CHECK_INT(rq2.model, ESL_MODEL_154);
    CHECK_INT(rq2.max, 94);
    CHECK_INT(rq2.layout_ver, 0x01020304);
    CHECK_INT(rq2.pos, 300);
    CHECK(!esl_layout_req_parse(buf, ESL_LREQ_LEN - 1, &rq2));
    CHECK(!esl_poll_parse(buf, ESL_LREQ_LEN, &p));
    buf[1] = 2;
    CHECK(!esl_layout_req_parse(buf, ESL_LREQ_LEN, &rq2));

    /* LAYOUT, and what the tag's parser accepts */
    static uint8_t data[200];
    for (int i = 0; i < 200; i++)
        data[i] = (uint8_t)i;
    n = esl_layout_msg_build(buf, 60, 0xCAFE, 200, 100, data, 200); /* clamped to one chunk */
    CHECK_INT(n, ESL_LAYOUT_HDR_LEN + ESL_LAYOUT_CHUNK_MAX);
    CHECK_INT(n, ESL_MAX_PAYLOAD - ESL_MIC_RESERVE);
    CHECK_MEM(buf, "\x04\x01\x3C\x00\xFE\xCA\x00\x00\xC8\x00\x64\x00", 12);
    n = esl_layout_msg_build(buf, 60, 0xCAFE, 200, 100, data, 94);
    CHECK(esl_reply_parse(buf, n, &r));
    CHECK_INT(r.type, ESL_MSG_LAYOUT);
    CHECK_INT(r.layout_ver, 0xCAFE);
    CHECK_INT(r.layout_size, 200);
    CHECK_INT(r.layout_pos, 100);
    CHECK_INT(r.layout_len, 94);
    n = esl_layout_msg_build(buf, 60, 0xCAFE, 150, 100, data, 94); /* past the end of the file */
    CHECK(!esl_reply_parse(buf, n, &r));
    n = esl_layout_msg_build(buf, 60, 0, 10, 0, data, 0); /* version 0 has no file */
    CHECK(!esl_reply_parse(buf, n, &r));
    CHECK(!esl_reply_parse(buf, ESL_LAYOUT_HDR_LEN - 1, &r));

    uint16_t w, h;
    CHECK(esl_model_size(ESL_MODEL_29, &w, &h));
    CHECK_INT(w, 296);
    CHECK_INT(h, 128);
    CHECK(esl_model_size(ESL_MODEL_154, &w, &h));
    CHECK_INT(w, 200);
    CHECK(!esl_model_size(ESL_MODEL_UNKNOWN, &w, &h));
}

void test_frame(void)
{
    const uint64_t mac = 0x00000123456789A8ull;
    uint8_t payload[ESL_POLL_LEN], frame[ESL_PSDU_MAX];
    esl_poll_t p = {1, 2950, 0, 0, ESL_MODEL_UNKNOWN, false, 0, false, 0};
    esl_rx_frame_t rx;

    esl_poll_build(payload, &p);
    size_t n = esl_frame_build_from_tag(frame, 7, 0x4553, 0x0000, mac, payload, sizeof(payload));
    CHECK_INT(n, 15 + 12 + 2);
    CHECK_MEM(frame, "\x41\xC8\x07\x53\x45\x00\x00\xA8\x89\x67\x45\x23\x01\x00\x00", 15);

    CHECK(esl_frame_parse_from_tag(frame, n, &rx));
    CHECK_INT(rx.seq, 7);
    CHECK_INT(rx.dst_pan, 0x4553);
    CHECK_INT(rx.dst_short, 0);
    CHECK(rx.src_mac == mac);
    CHECK_INT(rx.payload_len, 12);
    CHECK_MEM(rx.payload, payload, 12);

    /* ACK request / frame version bits are tolerated, other bits are not */
    frame[0] |= 0x20;
    CHECK(esl_frame_parse_from_tag(frame, n, &rx));
    frame[0] |= 0x08; /* security enabled */
    CHECK(!esl_frame_parse_from_tag(frame, n, &rx));
    CHECK(!esl_frame_parse_from_tag(frame, 16, &rx));

    uint8_t idle[4];
    esl_idle_build(idle, 0);
    n = esl_frame_build_to_tag(frame, 7, 0x4553, 0x0000, mac, idle, sizeof(idle));
    CHECK_INT(n, 15 + 4 + 2);
    CHECK_MEM(frame, "\x41\x8C\x07\x53\x45\xA8\x89\x67\x45\x23\x01\x00\x00\x00\x00\x02\x01\x00\x00", 19);
    CHECK(!esl_frame_parse_from_tag(frame, n, &rx)); /* AP->tag frames are not polls */

    uint8_t huge[ESL_PSDU_MAX] = {0};
    CHECK_INT(esl_frame_build_to_tag(frame, 0, 0, 0, mac, huge, ESL_PSDU_MAX - 16), 0);
    CHECK_INT(esl_frame_build_to_tag(frame, 0, 0, 0, mac, huge, ESL_MAX_PAYLOAD), ESL_PSDU_MAX);

    uint64_t m;
    char s[17];
    CHECK(esl_mac_parse("00000123456789a8", 16, &m));
    CHECK(m == mac);
    esl_mac_format(m, s);
    CHECK_STR(s, "00000123456789A8");
    CHECK(!esl_mac_parse("00000123456789A", 15, &m));
    CHECK(!esl_mac_parse("00000123456789AG", 16, &m));
}

void test_persist(void)
{
    esl_content_t c, d;
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    uint64_t mac;

    esl_content_init(&c, ESL_LAYOUT_PRICE, ESL_CFLAG_PRICE_HIGHLIGHT);
    esl_content_add_field(&c, S("Br\xF8" "d"));
    esl_content_add_field(&c, S(""));
    esl_content_add_field(&c, S("39,90"));
    size_t n = esl_persist_encode(0x1122334455667788ull, &c, blob);
    CHECK_INT(n, 1 + 8 + 3 + 3 + 9);
    CHECK(esl_persist_decode(blob, n, &mac, &d));
    CHECK(mac == 0x1122334455667788ull);
    CHECK(esl_content_equal(&c, &d));
    CHECK_INT(esl_content_id(&c), esl_content_id(&d));

    CHECK(!esl_persist_decode(blob, n - 1, &mac, &d));
    blob[n] = 0;
    CHECK(!esl_persist_decode(blob, n + 1, &mac, &d));
    blob[0] = 2;
    CHECK(!esl_persist_decode(blob, n, &mac, &d));
    blob[0] = ESL_PERSIST_VER;
    blob[9] = 9; /* layout */
    CHECK(!esl_persist_decode(blob, n, &mac, &d));
    blob[9] = ESL_LAYOUT_PRICE;
    blob[11] = 4; /* too many fields for the price layout */
    CHECK(!esl_persist_decode(blob, n, &mac, &d));

    /* named layout: version 2 blob; built-in content is still written as version 1 */
    blob[11] = 3;
    CHECK(esl_persist_decode(blob, n, &mac, &d));
    CHECK_INT(blob[0], ESL_PERSIST_VER);
    esl_content_init_named(&c, "a-long-layout-name-with-31-ch15", 31, ESL_CFLAG_FRAME);
    esl_content_add_field(&c, S("Kaffe"));
    esl_content_add_field(&c, S("49,90"));
    n = esl_persist_encode(7, &c, blob);
    CHECK_INT(n, 1 + 8 + 1 + 1 + 31 + 2 + 2 + 10);
    CHECK_INT(blob[0], ESL_PERSIST_VER_NAMED);
    CHECK(esl_persist_decode(blob, n, &mac, &d));
    CHECK(mac == 7);
    CHECK(esl_content_equal(&c, &d));
    CHECK_STR(d.name, "a-long-layout-name-with-31-ch15");
    CHECK(!esl_persist_decode(blob, n - 1, &mac, &d));
    blob[9] = ESL_LAYOUT_PRICE;
    CHECK(!esl_persist_decode(blob, n, &mac, &d));
    blob[9] = ESL_LAYOUT_NAMED;
    blob[10] = 40; /* name longer than the blob */
    CHECK(!esl_persist_decode(blob, n, &mac, &d));
    blob[10] = 31;
    blob[11] = 'A'; /* not a valid name */
    CHECK(!esl_persist_decode(blob, n, &mac, &d));
    esl_tag_cfg_t cfg = {120, 900, ESL_CFG_LOST_AP_NEVER}, cfg2;
    n = esl_persist_cfg_encode(0x1122334455667788ull, &cfg, blob);
    CHECK_INT(n, ESL_PERSIST_CFG_LEN);
    CHECK_MEM(blob, "\x01\x88\x77\x66\x55\x44\x33\x22\x11\x78\x00\x84\x03\xFF\xFF", n);
    CHECK(esl_persist_cfg_decode(blob, n, &mac, &cfg2));
    CHECK(mac == 0x1122334455667788ull);
    CHECK(memcmp(&cfg, &cfg2, sizeof(cfg)) == 0);
    CHECK(!esl_persist_cfg_decode(blob, n - 1, &mac, &cfg2));
    blob[0] = 2;
    CHECK(!esl_persist_cfg_decode(blob, n, &mac, &cfg2));
}
