// SPDX-License-Identifier: GPL-2.0-only
//
//  Minimal zero-dependency host test framework for the front-panel firmware.
//
//  Usage:
//      TEST(my_case) {
//          CHECK_EQ_INT(2 + 2, 4);
//          CHECK_TRUE(ptr != NULL);
//      }
//      // in a suite function:
//      RUN(my_case);
//
//  A test fails if any CHECK in it fails; remaining CHECKs still run so one
//  invocation reports every problem it finds. The process exit code is the
//  number of failed tests (0 == all passed), so `make test` fails CI cleanly.

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Shared counters (defined in test_main.c).
extern int g_tests_run;
extern int g_tests_failed;
extern int g_checks_failed_in_test;

#define TEST(name) static void name(void)

#define RUN(test)                                                              \
    do {                                                                       \
        g_checks_failed_in_test = 0;                                           \
        g_tests_run++;                                                         \
        test();                                                                \
        if (g_checks_failed_in_test) {                                         \
            g_tests_failed++;                                                  \
            printf("  [FAIL] %s (%d check%s failed)\n", #test,                 \
                   g_checks_failed_in_test,                                    \
                   g_checks_failed_in_test == 1 ? "" : "s");                   \
        } else {                                                               \
            printf("  [ ok ] %s\n", #test);                                    \
        }                                                                      \
    } while (0)

#define CHECK_TRUE(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("      x %s:%d: expected true: %s\n",                       \
                   __FILE__, __LINE__, #cond);                                 \
            g_checks_failed_in_test++;                                         \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
    do {                                                                       \
        long _a = (long)(actual);                                             \
        long _e = (long)(expected);                                           \
        if (_a != _e) {                                                        \
            printf("      x %s:%d: %s == %ld, expected %s == %ld\n",           \
                   __FILE__, __LINE__, #actual, _a, #expected, _e);            \
            g_checks_failed_in_test++;                                         \
        }                                                                      \
    } while (0)

#define CHECK_EQ_HEX(actual, expected)                                         \
    do {                                                                       \
        unsigned long _a = (unsigned long)(actual);                          \
        unsigned long _e = (unsigned long)(expected);                        \
        if (_a != _e) {                                                        \
            printf("      x %s:%d: %s == 0x%lx, expected %s == 0x%lx\n",       \
                   __FILE__, __LINE__, #actual, _a, #expected, _e);            \
            g_checks_failed_in_test++;                                         \
        }                                                                      \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                         \
    do {                                                                       \
        const char *_a = (actual);                                            \
        const char *_e = (expected);                                          \
        if (strcmp(_a, _e) != 0) {                                             \
            printf("      x %s:%d: %s == \"%s\", expected \"%s\"\n",           \
                   __FILE__, __LINE__, #actual, _a, _e);                       \
            g_checks_failed_in_test++;                                         \
        }                                                                      \
    } while (0)

#define CHECK_EQ_MEM(actual, expected, len)                                    \
    do {                                                                       \
        if (memcmp((actual), (expected), (len)) != 0) {                        \
            printf("      x %s:%d: %s != %s over %zu bytes\n",                 \
                   __FILE__, __LINE__, #actual, #expected, (size_t)(len));     \
            g_checks_failed_in_test++;                                         \
        }                                                                      \
    } while (0)

#define CHECK_ESP_OK(call) CHECK_EQ_INT((call), ESP_OK)

#endif // TEST_FRAMEWORK_H
