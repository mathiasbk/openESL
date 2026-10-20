#include "tinytest.h"

int tt_checks, tt_failures;

void test_crc32(void);
void test_text(void);
void test_content(void);
void test_proto(void);
void test_frame(void);
void test_persist(void);
void test_json(void);
void test_store(void);
void test_api(void);
void test_layout(void);
void test_radio(void);
void test_wifi(void);
void test_screen(void);
void test_render(void);
void test_sched(void);
void test_tagfw(void);

static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    {"crc32", test_crc32},
    {"text", test_text},
    {"content", test_content},
    {"proto", test_proto},
    {"frame", test_frame},
    {"persist", test_persist},
    {"json", test_json},
    {"store", test_store},
    {"api", test_api},
    {"layout", test_layout},
    {"radio", test_radio},
    {"wifi", test_wifi},
    {"screen", test_screen},
    {"render", test_render},
    {"sched", test_sched},
    {"tagfw", test_tagfw},
};

int main(void)
{
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = tt_failures;
        printf("%-10s", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        printf("%s\n", tt_failures == before ? "ok" : "");
    }
    printf("\n%d checks, %d failed\n", tt_checks, tt_failures);
    return tt_failures ? 1 : 0;
}
