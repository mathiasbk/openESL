#include "tinytest.h"
#include "esl_api.h"
#include "esl_crc32.h"
#include "esl_radio.h"
#include <stdlib.h>

/* Tag firmware over the air (docs/tag-ota.md), the AP side: FW / FW_REQ, the
 * image store and its upload (PUT /api/tag-firmware), and the offer to fake
 * tags through esl_radio_handle(). */

#define CAP 8
#define PAN 0x4553
#define AUTH "Bearer t"
#define MAC 0x0000012345678901ull

/* The store: two slots in RAM, like the flash partition on the C6 */
static uint8_t mem[ESL_MODEL_COUNT - 1][ESL_TAGFW_SLOT];
static int erases;
static bool fail_write;

static bool st_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    memset(mem[model - 1], 0xFF, ESL_TAGFW_SLOT);
    erases++;
    return true;
}

static bool st_write(void *ctx, uint8_t model, uint32_t off, const void *d, size_t n)
{
    (void)ctx;
    if (fail_write || off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(mem[model - 1] + off, d, n);
    return true;
}

static bool st_read(void *ctx, uint8_t model, uint32_t off, void *d, size_t n)
{
    (void)ctx;
    if (off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(d, mem[model - 1] + off, n);
    return true;
}

/* Who an image is for, kept as NVS would across a restart */
static esl_tagfw_to_t kept[ESL_MODEL_COUNT - 1];
static bool has_kept[ESL_MODEL_COUNT - 1];

static void st_save_to(void *ctx, uint8_t model, const void *to, size_t len)
{
    (void)ctx;
    has_kept[model - 1] = to && len == sizeof(esl_tagfw_to_t);
    if (has_kept[model - 1])
        memcpy(&kept[model - 1], to, len);
}

static bool st_load_to(void *ctx, uint8_t model, void *to, size_t len)
{
    (void)ctx;
    if (!has_kept[model - 1] || len != sizeof(esl_tagfw_to_t))
        return false;
    memcpy(to, &kept[model - 1], len);
    return true;
}

static const esl_tagfw_ops_t ops = {NULL, NULL, NULL, st_erase, st_write, st_read, st_save_to, st_load_to};
static esl_tagfw_t fw;
static esl_store_t st;
static esl_layouts_t layouts;
static esl_api_t api = {&st, "t", "0.1.0", 25, &layouts, NULL, NULL, NULL, NULL, NULL, &fw, false, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

/* An OTA file as build.py writes it: header, then size bytes of image */
static uint8_t file[ESL_FWF_HDR_LEN + ESL_FW_MAX];
static size_t make_file(uint8_t model, uint16_t ver, uint16_t size, uint8_t seed)
{
    uint8_t *img = file + ESL_FWF_HDR_LEN;
    for (size_t i = 0; i < size; i++)
        img[i] = (uint8_t)(i * 13 + seed);
    uint32_t crc = esl_crc32(img, size), sz = size, magic = ESL_FWF_MAGIC;
    memset(file, 0, ESL_FWF_HDR_LEN);
    memcpy(file + ESL_FWF_OFF_MAGIC, &magic, 4); /* little-endian, as on the PC */
    file[ESL_FWF_OFF_FORMAT] = ESL_FWF_FORMAT;
    file[ESL_FWF_OFF_MODEL] = model;
    file[ESL_FWF_OFF_VER] = (uint8_t)ver;
    file[ESL_FWF_OFF_VER + 1] = (uint8_t)(ver >> 8);
    memcpy(file + ESL_FWF_OFF_SIZE, &sz, 4);
    memcpy(file + ESL_FWF_OFF_CRC, &crc, 4);
    return ESL_FWF_HDR_LEN + size;
}

static int put_part(size_t off, size_t total, size_t len)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=%u&total=%u", (unsigned)off, (unsigned)total);
    return esl_api_handle(&api, "PUT", uri, AUTH, (const char *)file + off, len, 0, &out);
}

/* The whole file in parts of 16 KB, as esl-client does */
static int upload(size_t total)
{
    int status = 0;
    for (size_t off = 0; off < total; off += 16384) {
        size_t n = total - off < 16384 ? total - off : 16384;
        status = put_part(off, total, n);
        if (status != 200)
            return status;
    }
    return status;
}

static void reset(void);

/* One request as the AP's HTTP and serial glue hands it on (api_handle_tagfw_put in
   ap/main/http_api.c): in pieces of piece bytes, each a part of its own */
static int put_pieces(const char *uri, const uint8_t *body, size_t len, size_t piece)
{
    char puri[ESL_API_PIECE_URI_MAX];
    int status = 200;
    for (size_t got = 0; status == 200 && got < len; got += piece) {
        size_t n = len - got < piece ? len - got : piece;
        if (!esl_api_tagfw_piece_uri(uri, len, got, puri, sizeof(puri)))
            return -1;
        status = esl_api_handle(&api, "PUT", puri, AUTH, (const char *)body + got, n, 0, &out);
    }
    return status;
}

/* 16 KB parts, a whole file in one body, and a part for one tag, in 2 KB pieces */
static void test_pieces(void)
{
    char buf[ESL_API_PIECE_URI_MAX];
    CHECK(esl_api_tagfw_piece_uri("/api/tag-firmware?offset=16384&total=51216", 16384, 2048, buf, sizeof(buf)));
    CHECK_STR(buf, "/api/tag-firmware?offset=18432&total=51216");
    CHECK(esl_api_tagfw_piece_uri("/api/tag-firmware", 2064, 2048, buf, sizeof(buf)));
    CHECK_STR(buf, "/api/tag-firmware?offset=2048&total=2064");
    CHECK(esl_api_tagfw_piece_uri("/api/tag-firmware?to=0000012345678901&offset=0&total=2064", 2064, 0, buf,
                                  sizeof(buf)));
    CHECK_STR(buf, "/api/tag-firmware?offset=0&total=2064&to=0000012345678901");
    CHECK(!esl_api_tagfw_piece_uri("/api/tag-firmware?offset=0", 2064, 0, buf, sizeof(buf)));    /* goes whole: 400 */
    CHECK(!esl_api_tagfw_piece_uri("/api/tag-firmware/2.9", 10, 0, buf, sizeof(buf)));
    CHECK(!esl_api_tagfw_piece_uri("/api/tags", 10, 0, buf, sizeof(buf)));
    CHECK(!esl_api_tagfw_piece_uri("/api/tag-firmware?offset=0&total=1", 1, 0, buf, 8));          /* no room */

    reset();
    size_t total = make_file(ESL_MODEL_29, 204, 50 * 1024, 2);
    char uri[96];
    for (size_t off = 0; off < total; off += 16384) {
        size_t n = total - off < 16384 ? total - off : 16384;
        snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=%u&total=%u", (unsigned)off, (unsigned)total);
        CHECK_INT(put_pieces(uri, file + off, n, 2048), 200);
        if (off + n < total) {
            snprintf(uri, sizeof(uri), "\"received\":%u,\"total\":%u}", (unsigned)(off + n), (unsigned)total);
            CHECK(HAS(out.buf, uri));
        }
    }
    CHECK(HAS(out.buf, "\"model\":\"2.9\",\"version\":\"0.2.4\",\"size\":51200"));
    esl_tagfw_img_t img;
    CHECK(esl_tagfw_get(&fw, ESL_MODEL_29, &img));
    CHECK(img.crc == esl_crc32(file + ESL_FWF_HDR_LEN, 51200));

    /* A part sent again after its connection dropped half way: the pieces that got
       through are skipped, and the upload goes on */
    total = make_file(ESL_MODEL_29, 206, 20 * 1024, 4);
    snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=0&total=%u", (unsigned)total);
    CHECK_INT(put_pieces(uri, file, 8192, 2048), 200);
    snprintf(uri, sizeof(uri), "/api/tag-firmware?offset=8192&total=%u", (unsigned)total);
    CHECK_INT(put_pieces(uri, file + 8192, 4096, 2048), 200);           /* 2 of its 4 pieces */
    CHECK_INT(put_pieces(uri, file + 8192, 8192, 2048), 200);           /* sent again, whole */
    CHECK(HAS(out.buf, "\"received\":16384,"));
    CHECK_INT(put_part(16384 + 2048, total, 1024), 409);                /* ahead: still refused */
    CHECK_INT(put_part(16384, total, total - 16384), 200);
    CHECK(HAS(out.buf, "\"version\":\"0.2.6\""));

    /* Whole, in one body, for one tag */
    total = make_file(ESL_MODEL_29, 205, 2048, 3);
    CHECK_INT(put_pieces("/api/tag-firmware?to=0000012345678901", file, total, 2048), 200);
    CHECK(HAS(out.buf, "\"version\":\"0.2.5\""));
    CHECK(esl_tagfw_for(&fw, ESL_MODEL_29, MAC));
    CHECK(!esl_tagfw_for(&fw, ESL_MODEL_29, MAC + 1));
    /* A bad header stops it at the first piece */
    file[0] ^= 0xFF;
    CHECK_INT(put_pieces("/api/tag-firmware", file, total, 2048), 422);
}

static void reset(void)
{
    memset(mem, 0xFF, sizeof(mem));
    memset(has_kept, 0, sizeof(has_kept));
    erases = 0;
    fail_write = false;
    esl_tagfw_init(&fw, &ops);
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);
    esl_radio_set_tagfw(&fw);
}

static void test_messages(void)
{
    uint8_t p[ESL_MAX_PAYLOAD], d[3] = {7, 8, 9};
    esl_fw_req_t r = {ESL_MODEL_154, 91, 10203, 182, 0xA1B2C3D4u}, q;
    CHECK_INT(esl_fw_req_build(p, &r), ESL_FREQ_LEN);
    CHECK(esl_fw_req_parse(p, ESL_FREQ_LEN, &q));
    CHECK_INT(q.model, ESL_MODEL_154);
    CHECK_INT(q.max, 91);
    CHECK_INT(q.ver, 10203);
    CHECK_INT(q.pos, 182);
    CHECK(q.crc == 0xA1B2C3D4u);
    CHECK(!esl_fw_req_parse(p, ESL_FREQ_LEN - 1, &q));

    esl_reply_t rep;
    size_t n = esl_fw_msg_build(p, 300, 10203, 2048, 91, 0xA1B2C3D4u, ESL_MODEL_29, d, 3);
    CHECK_INT(n, ESL_FW_HDR_LEN + 3);
    CHECK(esl_reply_parse(p, n, &rep));
    CHECK_INT(rep.type, ESL_MSG_FW);
    CHECK_INT(rep.next_poll_s, 300);
    CHECK_INT(rep.fw_ver, 10203);
    CHECK_INT(rep.size, 2048);
    CHECK_INT(rep.pos, 91);
    CHECK(rep.fw_crc == 0xA1B2C3D4u);
    CHECK_INT(rep.fw_model, ESL_MODEL_29);
    CHECK_INT(rep.len, 3);
    CHECK_INT(rep.data[2], 9);
    /* a block past the end of the image */
    n = esl_fw_msg_build(p, 0, 1, 1024, 1023, 0, ESL_MODEL_29, d, 3);
    CHECK(!esl_reply_parse(p, n, &rep));

    /* IMG / IMG_REQ: a screen image, the same shape as FW */
    esl_img_req_t ir = {ESL_MODEL_29, ESL_IMG_CHUNK_MAX, 0x89ABCDEFu, 4600}, iq;
    CHECK_INT(esl_img_req_build(p, &ir), ESL_IREQ_LEN);
    CHECK_MEM(p, "\x0A\x01\x01\x5C\xEF\xCD\xAB\x89\xF8\x11", ESL_IREQ_LEN);
    CHECK(esl_img_req_parse(p, ESL_IREQ_LEN, &iq));
    CHECK_INT(iq.model, ESL_MODEL_29);
    CHECK_INT(iq.max, 92);
    CHECK(iq.id == 0x89ABCDEFu);
    CHECK_INT(iq.pos, 4600);
    CHECK(!esl_img_req_parse(p, ESL_IREQ_LEN - 1, &iq));
    p[0] = ESL_MSG_FW_REQ;
    CHECK(!esl_img_req_parse(p, ESL_IREQ_LEN, &iq));

    n = esl_img_msg_build(p, 300, 0x89ABCDEFu, 4736, 92, ESL_MODEL_29, ESL_IMG_FMT_RAW, d, 3);
    CHECK_INT(n, ESL_IMG_HDR_LEN + 3);
    CHECK_MEM(p, "\x09\x01\x2C\x01\xEF\xCD\xAB\x89\x80\x12\x5C\x00\x01\x00\x07\x08\x09", n);
    CHECK(esl_reply_parse(p, n, &rep));
    CHECK_INT(rep.type, ESL_MSG_IMG);
    CHECK_INT(rep.next_poll_s, 300);
    CHECK(rep.img_id == 0x89ABCDEFu);
    CHECK_INT(rep.size, 4736);
    CHECK_INT(rep.pos, 92);
    CHECK_INT(rep.fw_model, ESL_MODEL_29);
    CHECK_INT(rep.img_fmt, ESL_IMG_FMT_RAW);
    CHECK_INT(rep.len, 3);
    CHECK_INT(rep.data[2], 9);
    /* a full block, one past the end, id 0, an empty image, a short header */
    uint8_t big[ESL_IMG_CHUNK_MAX + 1];
    memset(big, 0xA5, sizeof(big));
    n = esl_img_msg_build(p, 0, 1, 4736, 0, ESL_MODEL_29, ESL_IMG_FMT_RAW, big, sizeof(big));
    CHECK_INT(n, ESL_MAX_PAYLOAD - ESL_MIC_RESERVE);
    CHECK(esl_reply_parse(p, n, &rep));
    CHECK_INT(rep.len, ESL_IMG_CHUNK_MAX);
    n = esl_img_msg_build(p, 0, 1, 4736, 4735, ESL_MODEL_29, ESL_IMG_FMT_RAW, d, 2);
    CHECK(!esl_reply_parse(p, n, &rep));
    n = esl_img_msg_build(p, 0, 0, 4736, 0, ESL_MODEL_29, ESL_IMG_FMT_RAW, d, 3);
    CHECK(!esl_reply_parse(p, n, &rep));
    n = esl_img_msg_build(p, 0, 1, 0, 0, ESL_MODEL_29, ESL_IMG_FMT_RAW, d, 0);
    CHECK(!esl_reply_parse(p, n, &rep));
    n = esl_img_msg_build(p, 0, 1, 4736, 0, ESL_MODEL_29, ESL_IMG_FMT_RAW, d, 3);
    CHECK(!esl_reply_parse(p, ESL_IMG_HDR_LEN - 1, &rep));
}

static void test_upload(void)
{
    reset();
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "{\"model\":\"2.9\",\"version\":null}"));

    /* 50 KB in four parts */
    size_t total = make_file(ESL_MODEL_29, 203, 50 * 1024, 1);
    CHECK_INT(put_part(0, total, 16384), 200);
    CHECK(HAS(out.buf, "\"received\":16384"));
    CHECK_INT(put_part(0 + 16384 * 2, total, 16384), 409);   /* a part skipped */
    CHECK_INT(put_part(16384, total, 16384), 200);
    CHECK_INT(put_part(32768, total, 16384), 200);
    CHECK(!esl_tagfw_get(&fw, ESL_MODEL_29, NULL));        /* not whole yet */
    CHECK_INT(put_part(49152, total, total - 49152), 200);
    CHECK(HAS(out.buf, "\"model\":\"2.9\",\"version\":\"0.2.3\",\"size\":51200"));
    esl_tagfw_img_t img;
    CHECK(esl_tagfw_get(&fw, ESL_MODEL_29, &img));
    CHECK_INT(img.ver, 203);
    CHECK_INT(img.size, 51200);
    CHECK(img.crc == esl_crc32(file + ESL_FWF_HDR_LEN, 51200));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"to\":\"all\",\"tags\":{\"updated\":0,\"pending\":0,\"failed\":0,\"no_ota\":0,\"not_chosen\":0},"
                     "\"progress\":{\"received\":0,\"total\":0}"));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware/1.54", AUTH, NULL, 0, 0, &out), 404);

    /* After a restart the AP finds it in the store again */
    esl_tagfw_init(&fw, &ops);
    CHECK(esl_tagfw_get(&fw, ESL_MODEL_29, &img));
    CHECK_INT(img.ver, 203);
    /* ... but not if the store was damaged */
    mem[0][ESL_FWF_HDR_LEN + 100] ^= 1;
    esl_tagfw_init(&fw, &ops);
    CHECK(!esl_tagfw_get(&fw, ESL_MODEL_29, NULL));

    /* Bad files */
    total = make_file(ESL_MODEL_154, 204, 2048, 2);
    file[0] ^= 1;
    CHECK_INT(put_part(0, total, total), 422);
    CHECK(HAS(out.buf, "not a tag firmware file"));
    make_file(ESL_MODEL_154, 204, 2000, 2);                  /* not whole pages */
    CHECK_INT(put_part(0, 2016, 2016), 422);
    make_file(9, 204, 2048, 2);
    CHECK_INT(put_part(0, total, total), 422);
    make_file(ESL_MODEL_154, 204, 2048, 2);
    CHECK_INT(put_part(0, total + 1, total), 400);           /* total does not match */
    CHECK_INT(put_part(0, total, 8), 400);                   /* no whole header */
    file[ESL_FWF_HDR_LEN + 5] ^= 1;                          /* CRC */
    CHECK_INT(put_part(0, total, total), 422);
    CHECK(!esl_tagfw_get(&fw, ESL_MODEL_154, NULL));
    CHECK_INT(put_part(16, total, 16), 409);                 /* nothing in progress */
    make_file(ESL_MODEL_154, 204, 2048, 2);
    fail_write = true;
    CHECK_INT(put_part(0, total, total), 500);
    fail_write = false;
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tag-firmware?offset=0", AUTH, (const char *)file, total, 0, &out), 400);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tag-firmware", AUTH, NULL, 0, 0, &out), 400);
    /* One request without a query: the whole file */
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tag-firmware", AUTH, (const char *)file, total, 0, &out), 200);
    CHECK(esl_tagfw_get(&fw, ESL_MODEL_154, NULL));

    /* Delete */
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tag-firmware/1.54", AUTH, NULL, 0, 0, &out), 200);
    CHECK(!esl_tagfw_get(&fw, ESL_MODEL_154, NULL));
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tag-firmware/1.54", AUTH, NULL, 0, 0, &out), 404);
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tag-firmware/3.7", AUTH, NULL, 0, 0, &out), 404);
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware", AUTH, NULL, 0, 0, &out), 405);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware", "Bearer x", NULL, 0, 0, &out), 401);

    /* Without a store the endpoint is not there */
    api.tagfw = NULL;
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware", AUTH, NULL, 0, 0, &out), 404);
    api.tagfw = &fw;
}

static void test_offer(void)
{
    esl_faketag_t t, old, bad;
    static esl_image_t images;
    uint8_t *imem = malloc(esl_image_mem_size());
    esl_image_init(&images, imem, NULL);
    esl_radio_set_image(&images);
    reset();
    esl_faketag_init(&t, MAC, 100, 3000, ESL_MODEL_29);
    t.ota = true;
    esl_faketag_init(&old, MAC + 1, 100, 3000, ESL_MODEL_29);   /* no SPI flash, or old firmware */
    old.images = false;
    esl_faketag_init(&bad, MAC + 2, 100, 3000, ESL_MODEL_29);
    bad.ota = true;
    bad.fw_bad = true;

    /* Nothing to offer: IDLE */
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);

    /* 6 KB for the 2.9" and content waiting for the tag: its image first */
    size_t total = make_file(ESL_MODEL_29, 101, 6 * 1024, 3);
    CHECK_INT(upload(total), 200);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tags/0000012345678901", AUTH,
                             "{\"layout\":\"text\",\"fields\":[\"Hei\"]}", 34, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 0), ESL_MSG_IMG);
    CHECK_INT(t.img_shown, 1);
    CHECK_INT(t.fw, 100);
    /* Then the image: 68 blocks in one wake-up, and the tag runs the new version */
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(t.fw_blocks, (6 * 1024 + ESL_FW_CHUNK_MAX - 1) / ESL_FW_CHUNK_MAX);
    CHECK_INT(t.fw, 101);
    CHECK_INT(t.fw_installs, 1);
    CHECK_INT(t.rejected, 0);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);   /* up to date */

    /* Old firmware never gets it; a tag whose update fails is not offered it again */
    CHECK_INT(esl_faketag_round(&old, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&bad, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(bad.fw, 100);
    CHECK_INT(bad.fw_failed, 101);
    CHECK_INT(esl_faketag_round(&bad, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&bad, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(bad.fw_blocks, 6 * 1024 / ESL_FW_CHUNK_MAX + 1);

    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"tags\":{\"updated\":1,\"pending\":0,\"failed\":1,\"no_ota\":1,\"not_chosen\":0}"));

    /* A newer image is offered to the failed tag again */
    total = make_file(ESL_MODEL_29, 102, 2048, 4);
    CHECK_INT(upload(total), 200);
    bad.fw_bad = false;
    {
        /* The offer is lost, both to the POLL and to the same POLL sent again, and the tag
           polls again in its next round, still with the fail flag for 101: that is not a
           rejection of 102, which it never started on, and 102 is offered again */
        uint8_t prx[ESL_PSDU_MAX], ptx[ESL_PSDU_MAX];
        size_t pn = esl_faketag_poll(&bad, PAN, prx);
        CHECK(esl_radio_handle(&st, &layouts, PAN, prx, pn, -50, 0, ptx, NULL) > 0);
        CHECK_INT(ptx[ESL_MHR_LEN + ESL_OFF_TYPE], ESL_MSG_FW);
        CHECK(esl_radio_handle(&st, &layouts, PAN, prx, pn, -50, 0, ptx, NULL) > 0);
        pn = esl_faketag_poll(&bad, PAN, prx);
        size_t pm = esl_radio_handle(&st, &layouts, PAN, prx, pn, -50, 0, ptx, NULL);
        CHECK_INT(esl_faketag_receive(&bad, ptx, pm), ESL_MSG_FW);
        CHECK_INT(bad.fw_failed, 0);
    }
    CHECK_INT(esl_faketag_round(&bad, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(bad.fw, 102);
    CHECK_INT(bad.fw_failed, 0);

    /* The image changes in the middle of a download: the tag starts over */
    esl_faketag_init(&t, MAC + 3, 100, 3000, ESL_MODEL_29);
    t.ota = true;
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_poll(&t, PAN, rx);
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -50, 0, tx, NULL);
    CHECK_INT(esl_faketag_receive(&t, tx, m), ESL_MSG_FW);
    CHECK_INT(t.fw_dl_got, ESL_FW_CHUNK_MAX);
    total = make_file(ESL_MODEL_29, 103, 1024, 5);
    CHECK_INT(upload(total), 200);
    n = esl_faketag_fw_request(&t, PAN, rx);
    m = esl_radio_handle(&st, &layouts, PAN, rx, n, -50, 0, tx, NULL);
    CHECK_INT(esl_faketag_receive(&t, tx, m), ESL_MSG_FW);
    CHECK_INT(t.fw_dl_ver, 103);
    CHECK_INT(t.fw_dl_got, ESL_FW_CHUNK_MAX);
    /* ... and is told IDLE when the image is gone */
    esl_tagfw_delete(&fw, ESL_MODEL_29);
    n = esl_faketag_fw_request(&t, PAN, rx);
    m = esl_radio_handle(&st, &layouts, PAN, rx, n, -50, 0, tx, NULL);
    CHECK_INT(esl_faketag_receive(&t, tx, m), ESL_MSG_IDLE);

    /* No image store at all (the default) */
    esl_radio_set_tagfw(NULL);
    esl_faketag_init(&t, MAC + 4, 100, 3000, ESL_MODEL_29);
    t.ota = true;
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    esl_radio_set_image(NULL);
    free(imem);
}

/* An image larger than a tag fetches in one wake-up (ESL_FAKETAG_FW_REQS blocks): the
   block it stops at tells it to come back in ESL_BLOCKS_POLL_S, not after its interval,
   and the last block gives it its interval back */
static void test_offer_long(void)
{
    esl_faketag_t t;
    reset();
    esl_faketag_init(&t, MAC, 100, 3000, ESL_MODEL_29);
    t.ota = true;
    t.images = false;
    CHECK_INT(upload(make_file(ESL_MODEL_29, 101, 14 * 1024, 6)), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(t.fw, 100);
    CHECK_INT(t.fw_dl_got, (ESL_FAKETAG_FW_REQS + 1) * ESL_FW_CHUNK_MAX);
    CHECK_INT(t.interval_s, ESL_BLOCKS_POLL_S);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 5000), ESL_MSG_FW);
    CHECK_INT(t.fw, 101);
    CHECK(t.interval_s != ESL_BLOCKS_POLL_S);

    /* the next poll offers nothing more: the IDLE has its interval too */
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -50, 10000), ESL_MSG_IDLE);
    CHECK(t.interval_s != ESL_BLOCKS_POLL_S);
}

/* One POLL, and the FW block the AP answers it with */
static uint8_t poll_once(esl_faketag_t *t)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -50, 0, tx, NULL);
    return (uint8_t)esl_faketag_receive(t, tx, m);
}

/* One FW_REQ for the next block, and the AP's answer */
static uint8_t fw_req_once(esl_faketag_t *t)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_fw_request(t, PAN, rx);
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -50, 0, tx, NULL);
    return (uint8_t)esl_faketag_receive(t, tx, m);
}

/* An image for one tag, then one more, then all; how far each one is */
static void test_choose(void)
{
    esl_faketag_t a, b, c;
    reset();
    esl_faketag_init(&a, MAC, 100, 3000, ESL_MODEL_29);
    esl_faketag_init(&b, MAC + 1, 100, 3000, ESL_MODEL_29);
    esl_faketag_init(&c, MAC + 2, 100, 3000, ESL_MODEL_29);
    a.ota = b.ota = c.ota = true;
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);

    /* For tag a only */
    size_t total = make_file(ESL_MODEL_29, 101, 2048, 6);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tag-firmware?offset=0&total=2064&to=00000123456789", AUTH,
                             (const char *)file, total, 0, &out), 400);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tag-firmware?offset=0&total=2064&to=0000012345678901", AUTH,
                             (const char *)file, total, 0, &out), 200);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"to\":[\"0000012345678901\"],\"tags\":{\"updated\":0,\"pending\":1,\"failed\":0,"
                       "\"no_ota\":0,\"not_chosen\":2},\"progress\":{\"received\":0,\"total\":2048}"));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000012345678901", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"fw_update\":{\"version\":\"0.1.1\",\"received\":0,\"size\":2048}"));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000012345678902", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"fw_update\":null"));

    /* b is not offered it, and a FW_REQ of its own gets IDLE */
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(b.fw_blocks, 0);
    /* a: the first block, and the API says how far it is */
    CHECK_INT(poll_once(&a), ESL_MSG_FW);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000012345678901", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"received\":91,\"size\":2048}"));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tag-firmware/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"progress\":{\"received\":91,\"total\":2048}"));
    /* Two more blocks; then the next POLL is answered with the block at 0 again, which
       the tag skips: how far it is stays where it was */
    CHECK_INT(fw_req_once(&a), ESL_MSG_FW);
    CHECK_INT(fw_req_once(&a), ESL_MSG_FW);
    CHECK_INT(a.fw_dl_got, 273);
    CHECK_INT(poll_once(&a), ESL_MSG_FW);
    CHECK_INT(a.fw_dl_got, 273);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000012345678901", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"received\":273,\"size\":2048}"));
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(a.fw, 101);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000012345678901", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"fw_update\":null"));

    /* b too: kept across a restart */
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware/2.9", AUTH, NULL, 0, 0, &out), 400);
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware/2.9?to=xyz", AUTH, NULL, 0, 0, &out), 400);
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware/1.54?to=all", AUTH, NULL, 0, 0, &out), 404);
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware/2.9?to=0000012345678902", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"to\":[\"0000012345678901\",\"0000012345678902\"]"));
    CHECK(HAS(out.buf, "\"updated\":1,\"pending\":1,\"failed\":0,\"no_ota\":0,\"not_chosen\":1}"));
    CHECK(HAS(out.buf, "\"progress\":{\"received\":2048,\"total\":4096}"));
    esl_tagfw_init(&fw, &ops);
    CHECK(!esl_tagfw_for(&fw, ESL_MODEL_29, MAC + 2));
    CHECK(esl_tagfw_for(&fw, ESL_MODEL_29, MAC + 1));
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -50, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(b.fw, 101);

    /* Then all of them */
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware/2.9?to=all", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(out.buf, "\"to\":\"all\""));
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -50, 0), ESL_MSG_FW);
    CHECK_INT(c.fw, 101);

    /* A new image for all (no to=) is for all, even with a list kept before */
    total = make_file(ESL_MODEL_29, 102, 1024, 7);
    CHECK_INT(upload(total), 200);
    esl_tagfw_init(&fw, &ops);
    CHECK(esl_tagfw_for(&fw, ESL_MODEL_29, MAC + 2));
    /* A list kept for an older image counts for nothing */
    kept[0].ver = 101;
    kept[0].all = 0;
    esl_tagfw_init(&fw, &ops);
    CHECK(esl_tagfw_for(&fw, ESL_MODEL_29, MAC + 2));

    /* At most ESL_TAGFW_CHOSEN one by one */
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tag-firmware?to=0000012345678901", AUTH, (const char *)file, total, 0,
                             &out), 200);
    for (unsigned i = 1; i < ESL_TAGFW_CHOSEN; i++)
        CHECK_INT(esl_tagfw_choose(&fw, ESL_MODEL_29, MAC + 100 + i), 0);
    CHECK_INT(esl_tagfw_choose(&fw, ESL_MODEL_29, MAC), 0);       /* already chosen */
    CHECK_INT(esl_api_handle(&api, "POST", "/api/tag-firmware/2.9?to=0000012345678999", AUTH, NULL, 0, 0, &out), 409);
    CHECK_INT(esl_tagfw_choose(&fw, ESL_MODEL_29, 0), 0);
    /* Deleting it forgets the list */
    esl_tagfw_delete(&fw, ESL_MODEL_29);
    CHECK(!has_kept[0]);
}

void test_tagfw(void)
{
    esl_sb_init(&out, resp, sizeof(resp));
    test_messages();
    test_upload();
    test_pieces();
    test_offer();
    test_offer_long();
    test_choose();
    esl_radio_set_tagfw(NULL);
}
