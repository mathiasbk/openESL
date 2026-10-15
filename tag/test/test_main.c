/* test_main.c - runs all the host tests. Exits with code 1 on failure. */
#include "test.h"

int test_failures;
int test_checks;

void test_proto(void);
void test_font(void);
void test_draw(void);
void test_layout(void);
void test_app(void);
void test_barcode(void);

int main(void)
{
    test_proto();
    test_font();
    test_draw();
    test_layout();
    test_app();
    test_barcode();
    printf("\n%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
