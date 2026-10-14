/* test.h - minimal test framework for the host tests. */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond) do { \
    test_checks++; \
    if (!(cond)) { \
        test_failures++; \
        printf("  FEIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    test_checks++; \
    if (_a != _b) { \
        test_failures++; \
        printf("  FEIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
    } \
} while (0)

#define RUN(fn) do { printf("%s\n", #fn); fn(); } while (0)

#endif
