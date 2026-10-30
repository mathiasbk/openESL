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

/* esl_format_number(in, opts) is want, or not a number (want NULL: 0, drawn as sent) */
static void num(const char *in, uint8_t opts, const char *want)
{
    uint8_t out[ESL_LE_NUM_MAX_LEN];
    size_t n = esl_format_number((const uint8_t *)in, strlen(in), opts, out, sizeof(out));
    if (!want) {
        CHECK_INT(n, 0);
        return;
    }
    if (n != strlen(want) || memcmp(out, want, n) != 0)
        printf("  \"%s\" -> \"%.*s\", want \"%s\"\n", in, (int)n, (const char *)out, want);
    CHECK_INT(n, strlen(want));
    CHECK_MEM(out, want, strlen(want));
}

void test_number(void)
{
    const uint8_t T = ESL_LE_NUM_THOUSANDS;

    /* the ways a price comes in */
    num("1299.9", ESL_LE_NUM_2, "1299,90");
    num("1299,9", ESL_LE_NUM_2, "1299,90");
    num(" 1 299,90 ", ESL_LE_NUM_2, "1299,90");
    num("1.299,90", ESL_LE_NUM_2, "1299,90");
    num("1.299.000", ESL_LE_NUM_2, "1299000,00");
    num("199,-", ESL_LE_NUM_2, "199,00");
    num("1.299,-", ESL_LE_NUM_2, "1299,00");
    num(",5", ESL_LE_NUM_2, "0,50");
    num("199", ESL_LE_NUM_2, "199,00");
    num("-12.5", ESL_LE_NUM_2, "-12,50");
    num("+5", ESL_LE_NUM_2, "5,00");
    num("\xA0" "1\xA0" "299,90\xA0", ESL_LE_NUM_2, "1299,90");
    num(".5", ESL_LE_NUM_2, "0,50");
    num("1.299", ESL_LE_NUM_2, "1,30"); /* one dot and no comma: the decimal sign */

    /* decimals */
    num("199.9", ESL_LE_NUM_0, "200");
    num("199.49", ESL_LE_NUM_0, "199");
    num("999.5", ESL_LE_NUM_0 | T, "1 000");
    num("19.995", ESL_LE_NUM_2, "20,00");
    num("19.994", ESL_LE_NUM_2, "19,99");
    num("-0.4", ESL_LE_NUM_0, "0");
    num("199", ESL_LE_NUM_AUTO, "199");
    num("199.00", ESL_LE_NUM_AUTO, "199");
    num("199.9", ESL_LE_NUM_AUTO, "199,90");
    num("199", ESL_LE_NUM_DASH, "199,-");
    num("199.001", ESL_LE_NUM_DASH, "199,-");
    num("199.5", ESL_LE_NUM_DASH, "199,50");
    num("199.5", ESL_LE_NUM_KEEP, "199,5");
    num("199", ESL_LE_NUM_KEEP, "199");
    num("199,-", ESL_LE_NUM_KEEP, "199,-");
    num("199", ESL_LE_NUM_0_DASH, "199,-");
    num("199.90", ESL_LE_NUM_0_DASH, "200,-");
    num("199.49", ESL_LE_NUM_0_DASH, "199,-");
    num("199,-", ESL_LE_NUM_0_DASH, "199,-");
    num("1299.5", ESL_LE_NUM_0_DASH | T, "1 300,-");
    num("-0.4", ESL_LE_NUM_0_DASH, "0,-");

    /* thousands */
    num("1299.9", ESL_LE_NUM_KEEP | T, "1 299,9");
    num("999", ESL_LE_NUM_2 | T, "999,00");
    num("1234567.891", ESL_LE_NUM_2 | T, "1 234 567,89");
    num("-1234", ESL_LE_NUM_DASH | T, "-1 234,-");
    num("123456789012.999999", ESL_LE_NUM_2 | T, "123 456 789 013,00");

    /* not a number, or no format: drawn as sent */
    num("Tilbud", ESL_LE_NUM_2, NULL);
    num("kr 199", ESL_LE_NUM_2, NULL);
    num("1,2,3", ESL_LE_NUM_2, NULL);
    num("1,5.5", ESL_LE_NUM_2, NULL);
    num("12.5 0", ESL_LE_NUM_2, NULL);
    num("1.000.5", ESL_LE_NUM_2, NULL);
    num("12 34", ESL_LE_NUM_2, NULL);
    num("1  299", ESL_LE_NUM_2, NULL);
    num(".299,90", ESL_LE_NUM_2, NULL);
    num("1 2990,5", ESL_LE_NUM_2, NULL);
    num("1,5,-", ESL_LE_NUM_2, NULL);
    num("", ESL_LE_NUM_2, NULL);
    num(" - ", ESL_LE_NUM_2, NULL);
    num(",", ESL_LE_NUM_2, NULL);
    num("1234567890123", ESL_LE_NUM_2, NULL);
    num("1.1234567", ESL_LE_NUM_2, NULL);
    num("199", 0, NULL);
    num("199", T, NULL);
    num("199", 0x70, NULL);
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
    /* each field also takes a length byte: 2 fields leave 94 bytes for text */
    CHECK_INT(esl_content_add_field(&a, big, ESL_MAX_TEXT_FOR(2) - 49), ESL_CONTENT_ERR_TOO_LONG);
    CHECK_INT(esl_content_add_field(&a, big, ESL_MAX_TEXT_FOR(2) - 50), ESL_CONTENT_OK);
    CHECK_INT(esl_content_text_len(&a), ESL_MAX_TEXT_FOR(2));
    CHECK_INT(esl_content_add_field(&a, S("")), ESL_CONTENT_ERR_TOO_LONG);

    /* a named layout takes six fields, with 90 bytes of text between them */
    CHECK_INT(esl_content_init_named(&a, "vare", 4, 0), ESL_CONTENT_OK);
    for (int i = 0; i < ESL_MAX_FIELDS; i++)
        CHECK_INT(esl_content_add_field(&a, big, 15), ESL_CONTENT_OK);
    CHECK_INT(esl_content_add_field(&a, S("")), ESL_CONTENT_ERR_TOO_MANY_FIELDS);
    CHECK_INT(esl_content_text_len(&a), 90);
    CHECK_INT(ESL_MAX_TEXT_BYTES, 90);

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

    /* four fields hold 92 bytes of text */
    esl_content_init(&a, ESL_LAYOUT_TEXT, 3);
    for (int i = 0; i < 4; i++)
        esl_content_add_field(&a, big, 92 / 4);
    CHECK_INT(esl_content_text_len(&a), 92);
    CHECK_INT(ESL_MAX_TEXT_FOR(4), 92);

    /* a layout from the layout file, by name (krav A14) */
    esl_content_t n1, n2, b1;
    CHECK_INT(esl_content_init_named(&n1, "shelf", 5, ESL_CFLAG_FRAME), ESL_CONTENT_OK);
    CHECK_INT(n1.layout, ESL_LAYOUT_NAMED);
    CHECK_STR(n1.name, "shelf");
    CHECK_INT(esl_content_max_fields(ESL_LAYOUT_NAMED), 6);
    CHECK_INT(esl_content_init(&b1, ESL_LAYOUT_NAMED, 0), ESL_CONTENT_ERR_LAYOUT); /* only by name */
    CHECK_INT(esl_content_init(&b1, ESL_LAYOUT_FILE + 15, 0), ESL_CONTENT_OK);     /* by index */
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
    CHECK(!esl_layout_name_valid("image", 5)); /* a picture (ESL_LAYOUT_IMAGE) */
    {
        /* a picture: model and CRC in its one field, so each picture has its own id,
           and it survives the NVS format */
        esl_content_t p1, p2, back;
        uint8_t model, blob[ESL_PERSIST_BLOB_MAX];
        uint32_t crc;
        uint64_t mac;
        esl_content_init_image(&p1, ESL_MODEL_29, 0x12345678u);
        esl_content_init_image(&p2, ESL_MODEL_29, 0x12345679u);
        CHECK(esl_content_image(&p1, &model, &crc));
        CHECK_INT(model, ESL_MODEL_29);
        CHECK_INT(crc, 0x12345678u);
        CHECK(esl_content_id(&p1) != esl_content_id(&p2));
        CHECK_INT(esl_content_layout_hash(&p1), 0);
        CHECK(esl_persist_decode(blob, esl_persist_encode(0xA1, &p1, blob), &mac, &back));
        CHECK(esl_content_equal(&p1, &back));
        CHECK(!esl_content_image(&n1, &model, &crc));
    }
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
    esl_tag_cfg_t cfg = {0, 0, 0, 0, 0, 0}, cfg2;
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
    cfg.long_poll_s = 3600;
    cfg.long_after_min = 1440;
    CHECK(esl_tag_cfg_version(&cfg) != v);
    cfg2 = cfg;
    cfg2.long_after_min = ESL_CFG_LONG_NEVER;
    CHECK(esl_tag_cfg_version(&cfg2) != esl_tag_cfg_version(&cfg));
    /* the version of settings without the long backoff is the one older APs gave them (CRC
       of the first three fields), so an update sends tags no CONFIG they already have */
    cfg2 = (esl_tag_cfg_t){120, 0, 0, 0, 0, 0};
    uint8_t old_b[6] = {120, 0, 0, 0, 0, 0};
    uint32_t old_crc = esl_crc32(old_b, 6);
    CHECK_INT(esl_tag_cfg_version(&cfg2), (uint16_t)(old_crc ^ (old_crc >> 16)));
    cfg2 = (esl_tag_cfg_t){0, 0, 0, 0, 60, 0};
    CHECK(!esl_tag_cfg_is_default(&cfg2));
    CHECK(esl_tag_cfg_version(&cfg2) != 0);
    /* the channel is in the version only when it is not the default */
    v = esl_tag_cfg_version(&cfg);
    cfg.channel = 20;
    CHECK(esl_tag_cfg_version(&cfg) != v);
    cfg2 = (esl_tag_cfg_t){0, 0, 0, 0, 0, 20};
    CHECK(!esl_tag_cfg_is_default(&cfg2));
    CHECK_INT(esl_config_msg_build(buf, 120, &cfg), ESL_CONFIG_LEN);
    v = esl_tag_cfg_version(&cfg);
    uint8_t expect_cfg[ESL_CONFIG_LEN] = {0x08, 0x01, 0x78, 0x00, (uint8_t)v, (uint8_t)(v >> 8),
                                          0x78, 0x00, 0x84, 0x03, 0x34, 0x12, 0x10, 0x0E, 0xA0, 0x05, 20, 0};
    CHECK_MEM(buf, expect_cfg, ESL_CONFIG_LEN);
    esl_reply_t r;
    CHECK(esl_reply_parse(buf, ESL_CONFIG_LEN, &r));
    CHECK_INT(r.type, ESL_MSG_CONFIG);
    CHECK_INT(r.next_poll_s, 120);
    CHECK_INT(r.cfg_ver, v);
    CHECK_INT(r.cfg.poll_s, 120);
    CHECK_INT(r.cfg.max_poll_s, 900);
    CHECK_INT(r.cfg.lost_ap_s, 0x1234);
    CHECK_INT(r.cfg.long_poll_s, 3600);
    CHECK_INT(r.cfg.long_after_min, 1440);
    CHECK_INT(r.cfg.channel, 20);
    CHECK(esl_reply_parse(buf, ESL_CONFIG_OFF_CHANNEL, &r)); /* an AP from before the channel */
    CHECK_INT(r.cfg.long_poll_s, 3600);
    CHECK_INT(r.cfg.channel, 0);
    CHECK(esl_reply_parse(buf, ESL_CONFIG_OFF_LONG_POLL, &r)); /* an AP from before the long backoff */
    CHECK_INT(r.cfg.lost_ap_s, 0x1234);
    CHECK_INT(r.cfg.long_poll_s, 0);
    CHECK_INT(r.cfg.channel, 0);
    CHECK(!esl_reply_parse(buf, ESL_CONFIG_OFF_LONG_POLL - 1, &r));
    buf[ESL_CONFIG_LEN] = 0xEE; /* a newer AP may append settings */
    CHECK(esl_reply_parse(buf, ESL_CONFIG_LEN + 1, &r));

    CHECK_INT(esl_idle_build(buf, 60), 4);
    CHECK_MEM(buf, "\x02\x01\x3C\x00", 4);

    /* The link block (#261) after cfg_ver; a 14-byte POLL has none */
    esl_poll_t p3 = {1, 3000, 5, 0, ESL_MODEL_154, true, 0xBEEF, true, 0xC8, 0x6E, 3, ESL_POLL_LF_RETRY};
    CHECK_INT(esl_poll_build(buf, &p3), ESL_POLL_LEN_LINK);
    CHECK_MEM(&buf[12], "\xEF\xBE\xC8\x6E\x03\x01", 6);
    CHECK(esl_poll_parse(buf, ESL_POLL_LEN_LINK, &p));
    CHECK(p.has_cfg_ver);
    CHECK_INT(p.cfg_ver, 0xBEEF);
    CHECK_INT(p.model, ESL_MODEL_154);
    CHECK(p.has_link);
    CHECK_INT(p.rx_rssi, 0xC8);
    CHECK_INT(p.rx_lqi, 0x6E);
    CHECK_INT(p.lost, 3);
    CHECK_INT(p.link_flags, ESL_POLL_LF_RETRY);
    CHECK(esl_poll_parse(buf, ESL_POLL_LEN_LINK - 1, &p));
    CHECK(!p.has_link);
    CHECK_INT(p.lost, 0);
    /* the flag bits do not overlap */
    CHECK_INT(ESL_POLL_F_BOOTED & ESL_POLL_F_DRAWFAIL, 0);
    CHECK_INT((ESL_POLL_F_BOOTED | ESL_POLL_F_DRAWFAIL | ESL_POLL_F_OTA | ESL_POLL_F_FWFAIL) &
                  (ESL_POLL_F_IMAGE | ESL_POLL_F_IMG_RLE),
              0);
    /* the retired types (CONTENT, LAYOUT) are not replies */
    CHECK_INT(esl_idle_build(buf, 60), 4);
    buf[0] = 0x03;
    CHECK(!esl_reply_parse(buf, 4, &r));
    buf[0] = 0x04;
    CHECK(!esl_reply_parse(buf, 4, &r));

    uint16_t w, h;
    CHECK(esl_model_size(ESL_MODEL_29, &w, &h));
    CHECK_INT(w, 296);
    CHECK_INT(h, 128);
    CHECK(esl_model_size(ESL_MODEL_154, &w, &h));
    CHECK_INT(w, 200);
    CHECK(!esl_model_size(ESL_MODEL_UNKNOWN, &w, &h));

    /* a block that is not the image's last says ESL_BLOCKS_POLL_S, when that is sooner;
       the last one, IDLE and a sooner interval stay as they are */
    {
        uint8_t d[ESL_IMG_CHUNK_MAX > ESL_FW_CHUNK_MAX ? ESL_IMG_CHUNK_MAX : ESL_FW_CHUNK_MAX] = {0};
        esl_reply_t r;
        size_t n = esl_fw_msg_build(buf, 600, 101, 1024, 0, 1, ESL_MODEL_29, d, ESL_FW_CHUNK_MAX);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == ESL_BLOCKS_POLL_S);
        n = esl_fw_msg_build(buf, 0, 101, 1024, 0, 1, ESL_MODEL_29, d, ESL_FW_CHUNK_MAX);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == ESL_BLOCKS_POLL_S);
        n = esl_fw_msg_build(buf, 2, 101, 1024, 0, 1, ESL_MODEL_29, d, ESL_FW_CHUNK_MAX);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == 2);
        n = esl_fw_msg_build(buf, 600, 101, 1024, 1024 - 10, 1, ESL_MODEL_29, d, 10);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == 600);
        n = esl_img_msg_build(buf, 600, 7, 500, 92, ESL_MODEL_29, ESL_IMG_FMT_RLE, d, ESL_IMG_CHUNK_MAX);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == ESL_BLOCKS_POLL_S);
        n = esl_img_msg_build(buf, 600, 7, 500, 500 - 40, ESL_MODEL_29, ESL_IMG_FMT_RLE, d, 40);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == 600);
        n = esl_idle_build(buf, 600);
        esl_blocks_poll_soon(buf, n);
        CHECK(esl_reply_parse(buf, n, &r) && r.next_poll_s == 600);
    }
}

void test_frame(void)
{
    const uint64_t mac = 0x00000123456789A8ull;
    uint8_t payload[ESL_POLL_LEN], frame[ESL_PSDU_MAX];
    esl_poll_t p = {1, 2950, 0, 0, ESL_MODEL_UNKNOWN, false, 0, false, 0, 0, 0, 0};
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
    {
        /* what the AP last heard from a tag */
        uint8_t sb[ESL_PERSIST_SEEN_LEN];
        esl_seen_t a = {ESL_MODEL_154, 0x0401, 2950, -61, 0xF0E0D0C0u}, b;
        uint64_t smac;
        CHECK_INT(esl_persist_seen_encode(0x1122334455667788ull, &a, sb), ESL_PERSIST_SEEN_LEN);
        CHECK(esl_persist_seen_decode(sb, sizeof(sb), &smac, &b));
        CHECK(smac == 0x1122334455667788ull);
        CHECK_INT(b.model, ESL_MODEL_154);
        CHECK_INT(b.fw, 0x0401);
        CHECK_INT(b.battery_mv, 2950);
        CHECK_INT(b.rssi, -61);
        CHECK(b.unix_s == 0xF0E0D0C0u);
        CHECK(!esl_persist_seen_decode(sb, sizeof(sb) - 1, &smac, &b));
        sb[0] = 9;
        CHECK(!esl_persist_seen_decode(sb, sizeof(sb), &smac, &b));
    }

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
    esl_tag_cfg_t cfg = {120, 900, ESL_CFG_LOST_AP_NEVER, 0, 0, 0}, cfg2;
    n = esl_persist_cfg_encode(0x1122334455667788ull, &cfg, blob);
    CHECK_INT(n, ESL_PERSIST_CFG_LEN);
    CHECK_MEM(blob, "\x02\x88\x77\x66\x55\x44\x33\x22\x11\x78\x00\x84\x03\xFF\xFF\x00\x00\x00\x00", n);
    CHECK(esl_persist_cfg_decode(blob, n, &mac, &cfg2));
    CHECK(mac == 0x1122334455667788ull);
    CHECK(memcmp(&cfg, &cfg2, sizeof(cfg)) == 0);
    cfg.long_poll_s = 7200;
    cfg.long_after_min = ESL_CFG_LONG_NEVER;
    n = esl_persist_cfg_encode(0x1122334455667788ull, &cfg, blob);
    CHECK_MEM(&blob[15], "\x20\x1C\xFF\xFF", 4);
    CHECK(esl_persist_cfg_decode(blob, n, &mac, &cfg2));
    CHECK(memcmp(&cfg, &cfg2, sizeof(cfg)) == 0);
    CHECK(!esl_persist_cfg_decode(blob, n - 1, &mac, &cfg2));
    blob[0] = 3;
    CHECK(!esl_persist_cfg_decode(blob, n, &mac, &cfg2));
    /* version 1 (before the long backoff, 15 bytes) is still read, with it at the defaults */
    memcpy(blob, "\x01\x88\x77\x66\x55\x44\x33\x22\x11\x78\x00\x84\x03\xFF\xFF", 15);
    CHECK(esl_persist_cfg_decode(blob, 15, &mac, &cfg2));
    CHECK_INT(cfg2.lost_ap_s, ESL_CFG_LOST_AP_NEVER);
    CHECK_INT(cfg2.long_poll_s, 0);
    CHECK_INT(cfg2.long_after_min, 0);
    CHECK(!esl_persist_cfg_decode(blob, 19, &mac, &cfg2));
}
