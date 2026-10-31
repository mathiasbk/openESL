/* One layout file for several models with the same screen (#181, same_as in
 * shared/models.json), with the registry in models_gen.h here: 2.9b uses the file of 2.9,
 * and "bad" names 1.54, whose screen differs, so it keeps its own. */
#include "../tinytest.h"
#include "esl_api.h"
#include "esl_render.h"
#include <stdlib.h>

int tt_checks, tt_failures;

#define CAP 8
#define AUTH "Bearer s3cret"
#define M29B 3
#define MBAD 4

static esl_store_t st;
static esl_layouts_t layouts;
static esl_api_t api = {&st, "s3cret", "0.1.0", &layouts, NULL, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL, 0, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

static int req(const char *method, const char *uri, const char *body)
{
    return esl_api_handle(&api, method, uri, AUTH, body, body ? strlen(body) : 0, 0, &out);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = f ? malloc(ESL_LAYOUT_MAX_SIZE + 1) : NULL;
    *len = buf ? fread(buf, 1, ESL_LAYOUT_MAX_SIZE, f) : 0;
    if (buf)
        buf[*len] = '\0';
    if (f)
        fclose(f);
    return buf;
}

static void poll_model(uint64_t mac, uint8_t model)
{
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_layout_env_t env;
    esl_poll_t p = {2, 2950, 0, ESL_POLL_F_IMAGE, model, true, 0, false, 0, 0, 0, 0};
    esl_layouts_env(&layouts, model, &env);
    esl_store_on_poll(&st, mac, &p, -61, 1000, &env, reply);
}

static void test_registry(void)
{
    CHECK_INT(esl_model_layout_model(ESL_MODEL_29), ESL_MODEL_29);
    CHECK_INT(esl_model_layout_model(ESL_MODEL_154), ESL_MODEL_154);
    CHECK_INT(esl_model_layout_model(M29B), ESL_MODEL_29);
    CHECK_INT(esl_model_layout_model(MBAD), MBAD); /* another screen: refused, its own file */
    CHECK_INT(esl_model_layout_model(ESL_MODEL_UNKNOWN), ESL_MODEL_UNKNOWN);
    CHECK_INT(esl_model_layout_model(ESL_MODEL_COUNT), ESL_MODEL_UNKNOWN);
}

static void test_store_and_draw(const char *f29, size_t len29)
{
    esl_layout_bin_t *bin = malloc(sizeof(*bin));
    char err[ESL_LAYOUT_ERR_MAX];
    bool changed;
    esl_layouts_init(&layouts, NULL);

    /* the 2.9 file compiles for 2.9b too (same screen), but 2.9b keeps none of its own */
    CHECK(esl_layout_compile(f29, len29, ESL_MODEL_29, bin, err, sizeof(err)));
    CHECK_INT(esl_layouts_set(&layouts, M29B, f29, len29, bin, &changed), 0);
    CHECK(!esl_layouts_restore(&layouts, M29B, f29, len29));
    CHECK(!esl_layouts_info(&layouts, M29B, NULL, NULL, NULL));

    uint32_t v = esl_layouts_set(&layouts, ESL_MODEL_29, f29, len29, bin, &changed);
    CHECK(v != 0 && changed);
    uint32_t v29b = 0;
    CHECK(esl_layouts_info(&layouts, M29B, &v29b, NULL, NULL));
    CHECK_INT(v29b, v);
    CHECK(esl_layouts_find(&layouts, M29B, "shelf", 5, NULL) >= 0);
    CHECK(esl_layouts_find(&layouts, MBAD, "shelf", 5, NULL) < 0);
    /* field names (#182) follow the same file */
    CHECK_INT(esl_layouts_field_index(&layouts, M29B, "shelf", 5, "unit", 4), 2);
    CHECK_INT(esl_layouts_field_index(&layouts, ESL_MODEL_29, "shelf", 5, "info", 4), 3);
    CHECK_INT(esl_layouts_field_index(&layouts, ESL_MODEL_29, "shelf", 5, "inf", 3), -1);
    CHECK_INT(esl_layouts_field_index(&layouts, ESL_MODEL_29, "sign", 4, "body", 4), 1);
    CHECK_INT(esl_layouts_field_index(&layouts, ESL_MODEL_29, "nope", 4, "body", 4), -2);
    CHECK_INT(esl_layouts_field_index(&layouts, MBAD, "shelf", 5, "unit", 4), -2);
    esl_layout_env_t env;
    esl_layouts_env(&layouts, M29B, &env);
    CHECK_INT(env.version, v);
    CHECK_INT(esl_layouts_image_ver(&layouts, M29B, 0), esl_layouts_image_ver(&layouts, ESL_MODEL_29, 0));

    /* a 2.9b tag is drawn exactly as a 2.9 tag */
    esl_content_t c;
    CHECK(esl_content_init_named(&c, "shelf", 5, 0) == ESL_CONTENT_OK);
    CHECK(esl_content_add_field(&c, (const uint8_t *)"Kaffe", 5) == ESL_CONTENT_OK);
    static uint8_t a[ESL_LBIN_MAX * 2], b[ESL_LBIN_MAX * 2];
    size_t na = esl_layouts_render(&layouts, ESL_MODEL_29, &c, a, sizeof(a));
    size_t nb = esl_layouts_render(&layouts, M29B, &c, b, sizeof(b));
    CHECK(na > 0);
    CHECK_INT(nb, na);
    CHECK(memcmp(a, b, na) == 0);
    /* "bad" has no file: drawn with the built-in layouts only, which have no "shelf" */
    CHECK_INT(esl_layouts_render(&layouts, MBAD, &c, b, sizeof(b)), 0);

    /* only the file's own model deletes it */
    CHECK(!esl_layouts_delete(&layouts, M29B));
    CHECK(esl_layouts_info(&layouts, M29B, NULL, NULL, NULL));
    CHECK(esl_layouts_delete(&layouts, ESL_MODEL_29));
    CHECK(!esl_layouts_info(&layouts, M29B, NULL, NULL, NULL));
    free(bin);
}

static void test_api(const char *f29)
{
    esl_layouts_init(&layouts, NULL);
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_sb_init(&out, resp, sizeof(resp));

    CHECK_INT(req("PUT", "/api/layouts/2.9b", f29), 409);
    CHECK_STR(resp, "{\"error\":\"model 2.9b uses the layout file of model 2.9 (same_as): upload it with PUT "
                    "/api/layouts/2.9\"}");
    CHECK_INT(req("PUT", "/api/layouts/2.9", f29), 200);
    CHECK(HAS(resp, "{\"model\":\"2.9\",\"same_as\":null,\"used_by\":[\"2.9\",\"2.9b\"],\"version\":"));
    CHECK_INT(req("GET", "/api/layouts/2.9b", NULL), 200);
    CHECK_STR(resp, f29);
    CHECK_INT(req("GET", "/api/layouts", NULL), 200);
    CHECK(HAS(resp, "{\"model\":\"2.9b\",\"same_as\":\"2.9\",\"used_by\":[\"2.9\",\"2.9b\"],\"version\":"));
    CHECK(HAS(resp, "{\"model\":\"bad\",\"same_as\":null,\"used_by\":[\"bad\"],\"version\":null"));
    CHECK(HAS(resp, "{\"model\":\"1.54\",\"same_as\":null,\"used_by\":[\"1.54\"],\"version\":null"));

    /* a 2.9b tag takes the layouts of the 2.9 file, and a new 2.9 file is checked against its content */
    poll_model(0x29B, M29B);
    CHECK_INT(req("PUT", "/api/tags/000000000000029B", "{\"layout\":\"shelf\",\"fields\":[\"Kaffe\",\"49,90\"]}"), 200);
    const char *bc = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"shelf\","
                     "\"fields\":[\"name\",\"ean\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"ean\","
                     "\"symbology\":\"ean13\",\"x\":0,\"y\":0,\"w\":150,\"h\":40}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/2.9", bc), 200);
    CHECK(HAS(resp, "\"content_errors\":[{\"mac\":\"000000000000029B\""));
    CHECK(HAS(resp, "\"content_errors_total\":1}"));
    CHECK_INT(esl_store_check_layout(&st, &layouts, M29B, NULL, NULL), 0); /* none use 2.9b's own file */

    CHECK_INT(req("DELETE", "/api/layouts/2.9b", NULL), 409);
    CHECK(HAS(resp, "delete it with DELETE /api/layouts/2.9"));
    CHECK_INT(req("DELETE", "/api/layouts/2.9", NULL), 200);
    CHECK_INT(req("GET", "/api/layouts/2.9b", NULL), 404);
}

int main(void)
{
    size_t len29;
    char *f29 = read_file("layouts/2.9.json", &len29);
    CHECK(f29 != NULL);
    if (f29) {
        test_registry();
        test_store_and_draw(f29, len29);
        test_api(f29);
    }
    esl_layouts_free(&layouts);
    esl_store_free(&st);
    free(f29);
    printf("same_as   %d checks, %d failed\n", tt_checks, tt_failures);
    return tt_failures != 0;
}
