#pragma once
/* Minimal test macros for the host tests. No dependencies. */
#include <stdio.h>
#include <string.h>

extern int tt_checks, tt_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        tt_checks++;                                                             \
        if (!(cond)) {                                                           \
            tt_failures++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define CHECK_INT(a, b)                                                          \
    do {                                                                         \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        tt_checks++;                                                             \
        if (_a != _b) {                                                          \
            tt_failures++;                                                       \
            printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,        \
                   __LINE__, #a, _a, _b);                                        \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        const char *_a = (a), *_b = (b);                                         \
        tt_checks++;                                                             \
        if (strcmp(_a, _b) != 0) {                                               \
            tt_failures++;                                                       \
            printf("  FAIL %s:%d: %s\n    got:      %s\n    expected: %s\n",     \
                   __FILE__, __LINE__, #a, _a, _b);                              \
        }                                                                        \
    } while (0)

#define CHECK_MEM(a, b, n)                                                       \
    do {                                                                         \
        tt_checks++;                                                             \
        if (memcmp((a), (b), (n)) != 0) {                                        \
            tt_failures++;                                                       \
            printf("  FAIL %s:%d: memcmp(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
        }                                                                        \
    } while (0)

/* True if s contains sub. */
#define HAS(s, sub) (strstr((s), (sub)) != NULL)
