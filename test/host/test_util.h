// test_util.h — tiny assert-based host test harness (no external framework).
//
// Each test file defines int main(void), runs CHECK_* macros, and returns the
// failure count (0 = pass). CTest treats nonzero exit as failure.
#pragma once

#include <stdio.h>
#include <math.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: CHECK(%s)\n",                       \
                    __FILE__, __LINE__, #cond);                                \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        g_checks++;                                                            \
        double _da = (a), _db = (b), _dt = (tol);                              \
        if (fabs(_da - _db) > _dt) {                                           \
            g_failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: |%g - %g| > %g\n",                  \
                    __FILE__, __LINE__, _da, _db, _dt);                        \
        }                                                                      \
    } while (0)

#define TEST_BEGIN(name) fprintf(stderr, "[ RUN  ] %s\n", (name))

#define TEST_SUMMARY()                                                         \
    do {                                                                       \
        fprintf(stderr, "[ %s ] %d checks, %d failures\n",                     \
                g_failures ? "FAIL" : "PASS", g_checks, g_failures);           \
        return g_failures ? 1 : 0;                                             \
    } while (0)
